/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// FILE: OpenALAudioManager.cpp ///////////////////////////////////////////////////////////////////
// EastWind: the game logic is ported from MilesAudioManager.cpp. Miles reported finished sounds from
// its timer thread; here update() polls each source, so everything runs on the main thread.

#include "Lib/BaseType.h"
#include "OpenALAudioDevice/OpenALAudioManager.h"
#include "OpenALAudioDevice/OpenALAudioStream.h"

#include "Common/AudioAffect.h"
#include "Common/AudioHandleSpecialValues.h"
#include "Common/AudioRequest.h"
#include "Common/AudioSettings.h"
#include "Common/AsciiString.h"
#include "Common/AudioEventInfo.h"
#include "Common/AudioEventRTS.h"
#include "Common/FileSystem.h"
#include "Common/GameCommon.h"
#include "Common/GameSounds.h"
#include "Common/GlobalData.h"
#include "Common/file.h"

#include "GameClient/DebugDisplay.h"
#include "GameClient/Drawable.h"
#include "GameClient/GameClient.h"
#include "GameClient/VideoPlayer.h"
#include "GameClient/View.h"

#include "GameLogic/GameLogic.h"
#include "GameLogic/TerrainLogic.h"

#define AL_ALEXT_PROTOTYPES
#include <AL/alext.h>
#include <AL/efx.h>

#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>
#define DR_MP3_IMPLEMENTATION
#include <dr_mp3.h>

#include <deque>
#include <stdarg.h>

namespace
{
const char *const PROVIDER_NAME = "OpenAL Soft";
const Int STREAM_BUFFER_COUNT = 8; ///< ~2 s queued at 0.25 s per buffer, enough to ride out loading hitches

// Returns the whole file in a new[] buffer, or nullptr.
char *readWholeFile(const AsciiString &name, UnsignedInt &size)
{
	File *file = TheFileSystem->openFile(name.str());
	if (!file) {
		return nullptr;
	}
	size = file->size();
	return file->readEntireAndClose();
}

Bool isRiff(const char *data, UnsignedInt size)
{
	return size >= 4 && memcmp(data, "RIFF", 4) == 0;
}

// Decodes a whole WAV (PCM or IMA ADPCM) or MP3 file to 16-bit samples. Free the result with freePcm.
void *decodeWhole(const char *data, UnsignedInt size, UnsignedInt &channels, UnsignedInt &rate, drwav_uint64 &frames)
{
	if (isRiff(data, size)) {
		unsigned int ch = 0, sampleRate = 0;
		void *pcm = drwav_open_memory_and_read_pcm_frames_s16(data, size, &ch, &sampleRate, &frames, nullptr);
		channels = ch;
		rate = sampleRate;
		return pcm;
	}

	drmp3_config config = {};
	drmp3_uint64 mp3Frames = 0;
	void *pcm = drmp3_open_memory_and_read_pcm_frames_s16(data, size, &config, &mp3Frames, nullptr);
	channels = config.channels;
	rate = config.sampleRate;
	frames = mp3Frames;
	return pcm;
}

void freePcm(const char *data, UnsignedInt size, void *pcm)
{
	if (isRiff(data, size)) {
		drwav_free(pcm, nullptr);
	} else {
		drmp3_free(pcm, nullptr);
	}
}

Bool isSourceStopped(ALuint source)
{
	ALint state = AL_STOPPED;
	alGetSourcei(source, AL_SOURCE_STATE, &state);
	return state == AL_STOPPED;
}
}

//-------------------------------------------------------------------------------------------------
// Music or speech decoded a piece at a time from a file held in memory.
// shortcut: the whole file is read up front (music tracks are a few MB); stream from File if mods ship huge tracks.
struct OpenALAudioManager::StreamDecoder
{
	char *m_data;
	UnsignedInt m_size;
	Bool m_isWav;
	drwav m_wav;
	drmp3 m_mp3;
	UnsignedInt m_channels;
	UnsignedInt m_sampleRate;
	ALenum m_format;
	Bool m_loop; ///< Music rewinds at the end of the file
	Bool m_ended;
	UnsignedInt m_framesThisPass;
	std::deque<Bool> m_endsPass; ///< One entry per queued AL buffer: true if it is the last one of a pass
	std::vector<drwav_int16> m_pcm;

	StreamDecoder() : m_data(nullptr), m_size(0), m_isWav(false), m_channels(0), m_sampleRate(0), m_format(AL_NONE),
		m_loop(false), m_ended(false), m_framesThisPass(0) {}

	~StreamDecoder()
	{
		if (m_isWav) {
			drwav_uninit(&m_wav);
		} else if (m_data) {
			drmp3_uninit(&m_mp3);
		}
		delete [] m_data;
	}

	Bool open(const AsciiString &name)
	{
		m_data = readWholeFile(name, m_size);
		if (!m_data) {
			return false;
		}

		if (isRiff(m_data, m_size)) {
			if (!drwav_init_memory(&m_wav, m_data, m_size, nullptr)) {
				return false;
			}
			m_isWav = true;
			m_channels = m_wav.channels;
			m_sampleRate = m_wav.sampleRate;
		} else {
			if (!drmp3_init_memory(&m_mp3, m_data, m_size, nullptr)) {
				delete [] m_data;
				m_data = nullptr; // the destructor must not uninit an mp3 that never opened
				return false;
			}
			m_channels = m_mp3.channels;
			m_sampleRate = m_mp3.sampleRate;
		}

		m_format = OpenALAudioManager::getALFormat(m_channels, 16);
		if (m_format == AL_NONE || m_sampleRate == 0) {
			return false;
		}

		m_pcm.resize((m_sampleRate / 4) * m_channels);
		return true;
	}

	UnsignedInt read(UnsignedInt frames)
	{
		if (m_isWav) {
			return (UnsignedInt)drwav_read_pcm_frames_s16(&m_wav, frames, &m_pcm[0]);
		}
		return (UnsignedInt)drmp3_read_pcm_frames_s16(&m_mp3, frames, &m_pcm[0]);
	}

	void rewind()
	{
		if (m_isWav) {
			drwav_seek_to_pcm_frame(&m_wav, 0);
		} else {
			drmp3_seek_to_pcm_frame(&m_mp3, 0);
		}
	}
};

//-------------------------------------------------------------------------------------------------
OpenALAudioManager::PlayingAudio::PlayingAudio() :
	m_source(0),
	m_file(nullptr),
	m_decoder(nullptr),
	m_type(PAT_INVALID),
	m_status(PS_Playing),
	m_loopsDone(0),
	m_framesFaded(0),
	m_fade(false),
	m_rerequestOnNextUpdate(false),
	m_requestStop(false)
{
}

//-------------------------------------------------------------------------------------------------
ALenum OpenALAudioManager::getALFormat(int channels, int bits)
{
	if (channels == 1) {
		switch (bits) {
			case 8: return AL_FORMAT_MONO8;
			case 16: return AL_FORMAT_MONO16;
			case 32: return AL_FORMAT_MONO_FLOAT32;
		}
	} else if (channels == 2) {
		switch (bits) {
			case 8: return AL_FORMAT_STEREO8;
			case 16: return AL_FORMAT_STEREO16;
			case 32: return AL_FORMAT_STEREO_FLOAT32;
		}
	}
	return AL_NONE;
}

//-------------------------------------------------------------------------------------------------
OpenALAudioManager::OpenALAudioManager() :
	m_selectedProvider(PROVIDER_ERROR),
	m_selectedSpeakerType(0),
	m_pref3DProvider(AsciiString::TheEmptyString),
	m_prefSpeaker(AsciiString::TheEmptyString),
	m_device(nullptr),
	m_context(nullptr),
	m_lowPassFilter(0),
	m_audioCache(NEW AudioFileCache),
	m_binkStream(nullptr),
	m_num2DSamples(0),
	m_num3DSamples(0),
	m_numStreams(0)
{
}

//-------------------------------------------------------------------------------------------------
OpenALAudioManager::~OpenALAudioManager()
{
	releaseHandleForBink();
	stopAllAudioImmediately();
	// The cache frees its AL buffers, so it goes before the context.
	delete m_audioCache;
	m_audioCache = nullptr;
	closeDevice();

	DEBUG_ASSERTCRASH(this == TheAudio, ("Umm..."));
	TheAudio = nullptr;
}

//-------------------------------------------------------------------------------------------------
#if defined(RTS_DEBUG)
AudioHandle OpenALAudioManager::addAudioEvent(const AudioEventRTS *eventToAdd)
{
	if (TheGlobalData->m_preloadReport) {
		if (!eventToAdd->getEventName().isEmpty()) {
			m_allEventsLoaded.insert(eventToAdd->getEventName());
		}
	}

	return AudioManager::addAudioEvent(eventToAdd);
}

//-------------------------------------------------------------------------------------------------
static void debugPrint(DebugDisplayInterface *dd, FILE *fp, const char *format, ...)
{
	char buffer[512];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	if (dd) {
		dd->printf("%s", buffer);
	}
	if (fp) {
		fputs(buffer, fp);
	}
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::audioDebugDisplay(DebugDisplayInterface *dd, void *, FILE *fp)
{
	const char *version = m_context ? alGetString(AL_VERSION) : nullptr;
	debugPrint(dd, fp, "%s version: %s    ", PROVIDER_NAME, version ? version : "none");
	debugPrint(dd, fp, "Memory Usage : %u/%u\n", m_audioCache->getCurrentlyUsedSize(), m_audioCache->getMaxSize());
	debugPrint(dd, fp, "Sound: %s    3DSound: %s    Speech: %s    Music: %s\n",
		isOn(AudioAffect_Sound) ? "Yes" : "No", isOn(AudioAffect_Sound3D) ? "Yes" : "No",
		isOn(AudioAffect_Speech) ? "Yes" : "No", isOn(AudioAffect_Music) ? "Yes" : "No");
	debugPrint(dd, fp, "Channels Available: %u Sounds    %u 3D Sounds\n", getNumAvailable2DSamples(), getNumAvailable3DSamples());
	debugPrint(dd, fp, "Volume: Sound: %d    3DSound: %d    Speech: %d    Music: %d\n",
		REAL_TO_INT(m_soundVolume * 100.0f), REAL_TO_INT(m_sound3DVolume * 100.0f),
		REAL_TO_INT(m_speechVolume * 100.0f), REAL_TO_INT(m_musicVolume * 100.0f));
	debugPrint(dd, fp, "Current Speaker Type: %s\n", translateUnsignedIntToSpeakerType(getSpeakerType()).str());

	const Coord3D *mikePos = getListenerPosition();
	debugPrint(dd, fp, "Microphone at: (%d,%d,%d) -- Zoom Volume: %d%%\n",
		(Int)mikePos->x, (Int)mikePos->y, (Int)mikePos->z, (Int)(getZoomVolume() * 100.0f));

	struct ListToShow { std::list<PlayingAudio *> *list; const char *title; UnsignedInt channels; };
	const ListToShow lists[] = {
		{ &m_playingSounds, "-----------------------------------------------------Sounds\n", getNum2DSamples() },
		{ &m_playing3DSounds, "--------------------------------------------------3D Sounds\n", getNum3DSamples() },
		{ &m_playingStreams, "----------------------------------------------------Streams\n", getNumStreams() },
	};

	for (const ListToShow &show : lists) {
		debugPrint(dd, fp, "%s", show.title);
		UnsignedInt channel = 1;
		for (PlayingAudio *playing : *show.list) {
			AudioEventRTS *event = playing->m_audioEventRTS.Peek();
			AsciiString filename = getFileName(event->getFilename().str());
			Int volume = REAL_TO_INT(100.0f * getEffectiveVolume(event));
			debugPrint(dd, fp, "%2u: %-24s - (%s) Volume: %d\n", channel++, event->getEventName().str(), filename.str(), volume);
		}
		for (; channel <= show.channels; ++channel) {
			debugPrint(dd, fp, "%2u: Silence\n", channel);
		}
	}
	debugPrint(dd, fp, "===========================================================\n");
}
#endif

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::init()
{
	AudioManager::init();
	openDevice();
	m_audioCache->setMaxSize(getAudioSettings()->m_maxCacheSize);
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::postProcessLoad()
{
	AudioManager::postProcessLoad();
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::reset()
{
#if defined(RTS_DEBUG)
	dumpAllAssetsUsed();
	m_allEventsLoaded.clear();
#endif

	AudioManager::reset();
	stopAllAudioImmediately();
	removeAllAudioRequests();
	// This must come after stopAllAudioImmediately() and removeAllAudioRequests(), to ensure that
	// sounds pointing to the temporary AudioEventInfo handles are deleted before their info is deleted
	removeLevelSpecificAudioEventInfos();
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::update()
{
	AudioManager::update();
	setDeviceListenerPosition();
	processRequestList();

	pollCompletion(m_playingSounds);
	pollCompletion(m_playing3DSounds);
	for (PlayingAudio *playing : m_playingStreams) {
		pumpStream(playing);
	}
	for (PlayingAudio *playing : m_fadingAudio) {
		pumpStream(playing); // fading music must keep being fed
	}

	for (std::list<ForcePlayed>::iterator it = m_audioForcePlayed.begin(); it != m_audioForcePlayed.end(); ) {
		if (isSourceStopped(it->m_source)) {
			alDeleteSources(1, &it->m_source);
			alDeleteBuffers(1, &it->m_buffer);
			it = m_audioForcePlayed.erase(it);
		} else {
			++it;
		}
	}

	processPlayingList();
	processFadingList();
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::pollCompletion(std::list<PlayingAudio *> &list)
{
	// Paused sources report AL_PAUSED, so they are not mistaken for finished ones.
	for (PlayingAudio *playing : list) {
		if (playing->isPlaying() && isSourceStopped(playing->m_source)) {
			onAudioCompleted(playing);
		}
	}
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::onAudioCompleted(PlayingAudio *playing)
{
	const AudioEventInfo *info = playing->m_audioEventRTS->getAudioEventInfo();

	if (getDisallowSpeech() && info->m_soundType == AT_Streaming) {
		setDisallowSpeech(FALSE);
	}

	// Streams loop inside pumpStream, so only samples take the loop path.
	if (playing->m_type != PAT_Stream && (info->m_control & AC_LOOP)) {
		if (playing->m_audioEventRTS->getNextPlayPortion() == PP_Attack) {
			playing->m_audioEventRTS->setNextPlayPortion(PP_Sound);
		}
		if (playing->m_audioEventRTS->getNextPlayPortion() == PP_Sound) {
			playing->m_audioEventRTS->decreaseLoopCount();
			if (startNextLoop(playing)) {
				return;
			}
		}
	}

	playing->m_audioEventRTS->advanceNextPlayPortion();

	if (playing->m_type != PAT_Stream && playing->m_audioEventRTS->getNextPlayPortion() != PP_Done) {
		closeFile(playing->m_file);
		// Cleared before the reload, so that freeing cache space cannot stop this sound for using the old file.
		playing->m_file = nullptr;
		if (playing->m_type == PAT_3DSample) {
			playing->m_file = playSample3D(playing->m_audioEventRTS.Peek(), playing);
		} else {
			playing->m_file = playSample(playing->m_audioEventRTS.Peek(), playing);
		}
		if (playing->m_file) {
			return;
		}
	}

	// it will be cleaned up on the next frame update
	if (playing->m_status == PS_Playing) {
		playing->m_status = PS_Stopping;
	}
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::pumpStream(PlayingAudio *playing)
{
	StreamDecoder *decoder = playing->m_decoder;
	if (!decoder || !playing->m_source) {
		return;
	}
	const ALuint source = playing->m_source;

	ALint processed = 0;
	alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
	while (processed-- > 0) {
		ALuint buffer = 0;
		alSourceUnqueueBuffers(source, 1, &buffer);
		alDeleteBuffers(1, &buffer);
		if (decoder->m_endsPass.front()) {
			++playing->m_loopsDone;
		}
		decoder->m_endsPass.pop_front();
	}

	const UnsignedInt chunkFrames = decoder->m_sampleRate / 4;
	while (!decoder->m_ended && decoder->m_endsPass.size() < STREAM_BUFFER_COUNT) {
		const UnsignedInt frames = decoder->read(chunkFrames);
		if (frames > 0) {
			ALuint buffer = 0;
			alGenBuffers(1, &buffer);
			alBufferData(buffer, decoder->m_format, &decoder->m_pcm[0], (ALsizei)(frames * decoder->m_channels * sizeof(drwav_int16)), decoder->m_sampleRate);
			alSourceQueueBuffers(source, 1, &buffer);
			decoder->m_endsPass.push_back(false);
			decoder->m_framesThisPass += frames;
		}

		if (frames < chunkFrames) {
			// End of the file. An empty file would rewind forever, so it ends instead.
			if (!decoder->m_loop || decoder->m_framesThisPass == 0) {
				decoder->m_ended = true;
				break;
			}
			if (decoder->m_endsPass.empty()) {
				++playing->m_loopsDone; // the pass already finished playing
			} else {
				decoder->m_endsPass.back() = true;
			}
			decoder->rewind();
			decoder->m_framesThisPass = 0;
		}
	}

	ALint state = AL_STOPPED;
	alGetSourcei(source, AL_SOURCE_STATE, &state);
	if (decoder->m_endsPass.empty()) {
		if (decoder->m_ended && playing->isPlaying()) {
			onAudioCompleted(playing);
		}
	} else if (state == AL_STOPPED || state == AL_INITIAL) {
		alSourcePlay(source); // first start, or an underrun
	}
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::stopAudio(AudioAffect which)
{
	if (BitIsSet(which, AudioAffect_Sound)) {
		for (PlayingAudio *playing : m_playingSounds) {
			stopPlayingAudio(playing);
		}
	}

	if (BitIsSet(which, AudioAffect_Sound3D)) {
		for (PlayingAudio *playing : m_playing3DSounds) {
			stopPlayingAudio(playing);
		}
	}

	if (BitIsSet(which, AudioAffect_Speech | AudioAffect_Music)) {
		for (PlayingAudio *playing : m_playingStreams) {
			const AudioAffect affect = playing->m_audioEventRTS->getAudioEventInfo()->m_soundType == AT_Music ? AudioAffect_Music : AudioAffect_Speech;
			if (BitIsSet(which, affect)) {
				stopPlayingAudio(playing);
			}
		}
	}
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::pauseAudio(AudioAffect which)
{
	if (BitIsSet(which, AudioAffect_Sound)) {
		for (PlayingAudio *playing : m_playingSounds) {
			if (playing->isPlaying()) {
				alSourcePause(playing->m_source);
			}
		}
	}

	if (BitIsSet(which, AudioAffect_Sound3D)) {
		for (PlayingAudio *playing : m_playing3DSounds) {
			if (playing->isPlaying()) {
				alSourcePause(playing->m_source);
			}
		}
	}

	if (BitIsSet(which, AudioAffect_Speech | AudioAffect_Music)) {
		for (PlayingAudio *playing : m_playingStreams) {
			if (!playing->isPlaying()) {
				continue;
			}
			const AudioAffect affect = playing->m_audioEventRTS->getAudioEventInfo()->m_soundType == AT_Music ? AudioAffect_Music : AudioAffect_Speech;
			if (BitIsSet(which, affect)) {
				alSourcePause(playing->m_source);
			}
		}
	}

	//Get rid of PLAY audio requests when pausing audio.
	for (std::list<AudioRequest *>::iterator ait = m_audioRequests.begin(); ait != m_audioRequests.end(); ) {
		AudioRequest *req = (*ait);
		if (req->m_request == AR_Play) {
			deleteInstance(req);
			ait = m_audioRequests.erase(ait);
		} else {
			++ait;
		}
	}
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::resumeAudio(AudioAffect which)
{
	// alSourcePlay continues a paused source where it was.
	if (BitIsSet(which, AudioAffect_Sound)) {
		for (PlayingAudio *playing : m_playingSounds) {
			if (playing->isPlaying()) {
				alSourcePlay(playing->m_source);
			}
		}
	}

	if (BitIsSet(which, AudioAffect_Sound3D)) {
		for (PlayingAudio *playing : m_playing3DSounds) {
			if (playing->isPlaying()) {
				alSourcePlay(playing->m_source);
			}
		}
	}

	if (BitIsSet(which, AudioAffect_Speech | AudioAffect_Music)) {
		for (PlayingAudio *playing : m_playingStreams) {
			if (!playing->isPlaying()) {
				continue;
			}
			const AudioAffect affect = playing->m_audioEventRTS->getAudioEventInfo()->m_soundType == AT_Music ? AudioAffect_Music : AudioAffect_Speech;
			if (BitIsSet(which, affect)) {
				alSourcePlay(playing->m_source);
			}
		}
	}
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::pauseAmbient(Bool shouldPause)
{
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::playAudioEvent(AudioRequest *req)
{
	DEBUG_ASSERTCRASH(req->m_pendingEvent != nullptr, ("audio request was expected to contain a valid audio event"));

	if (!m_context) {
		return;
	}

	AudioEventRTS *event = req->m_pendingEvent.Peek();
	const AudioEventInfo *info = event->getAudioEventInfo();
	if (!info) {
		return;
	}

	AudioHandle handleToKill = event->getHandleToKill();

	PlayingAudio *newPlaying = NEW PlayingAudio;
	newPlaying->m_requestStop = req->m_requestStop;
	newPlaying->m_audioEventRTS = req->m_pendingEvent;

	switch (info->m_soundType)
	{
		case AT_Music:
		case AT_Streaming:
		{
			if ((info->m_soundType == AT_Streaming) && event->getUninterruptible()) {
				stopAllSpeech();
			}

			Bool foundSoundToReplace = false;
			if (handleToKill) {
				for (PlayingAudio *playing : m_playingStreams) {
					if (playing->m_audioEventRTS && playing->m_audioEventRTS->getPlayingHandle() == handleToKill) {
						//Release this streaming channel immediately because we are going to play another sound in it's place.
						stopPlayingAudio(playing);
						foundSoundToReplace = true;
						break;
					}
				}
			}

			newPlaying->m_type = PAT_Stream;
			if (!handleToKill || foundSoundToReplace) {
				newPlaying->m_decoder = NEW StreamDecoder;
				if (!newPlaying->m_decoder->open(event->getFilename())) {
					DEBUG_LOG(("Could not open stream '%s'", event->getFilename().str()));
					delete newPlaying->m_decoder;
					newPlaying->m_decoder = nullptr;
				}
			}

			if (newPlaying->m_decoder) {
				if ((info->m_soundType == AT_Streaming) && event->getUninterruptible()) {
					setDisallowSpeech(TRUE);
				}
				playStream(event, newPlaying);
				if (newPlaying->m_source) {
					m_playingStreams.push_back(newPlaying);
					newPlaying = nullptr;
				}
			}
			break;
		}

		case AT_SoundEffect:
		{
			const Bool positional = event->isPositionalAudio();
			std::list<PlayingAudio *> &list = positional ? m_playing3DSounds : m_playingSounds;

			Bool foundSoundToReplace = false;
			if (handleToKill) {
				for (PlayingAudio *playing : list) {
					if (playing->m_audioEventRTS && playing->m_audioEventRTS->getPlayingHandle() == handleToKill) {
						//Release this sound channel immediately because we are going to play another sound in it's place.
						stopPlayingAudio(playing);
						foundSoundToReplace = true;
						break;
					}
				}
			}

			ALuint source = 0;
			if (!handleToKill || foundSoundToReplace) {
				source = positional ? getAvailable3DSample() : getAvailable2DSample();
				if (!source) {
					//If we don't have an available sample, kill the lowest priority assuming we have one that is lower
					//than the sound we are trying to add.
					if (killLowestPrioritySoundImmediately(event)) {
						source = positional ? getAvailable3DSample() : getAvailable2DSample();
					}
				}
			}

			newPlaying->m_source = source;
			newPlaying->m_type = positional ? PAT_3DSample : PAT_Sample;

			if (source) {
				newPlaying->m_file = positional ? playSample3D(event, newPlaying) : playSample(event, newPlaying);
			}

			if (newPlaying->m_file) {
				list.push_back(newPlaying);
				newPlaying = nullptr;
			}
			break;
		}
	}

	// If we were able to successfully play audio, then we set it to null above. Otherwise it must be freed.
	if (newPlaying) {
		stopPlayingAudio(newPlaying);
		releasePlayingAudio(newPlaying);
	}
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::stopAudioEvent(AudioHandle handle)
{
	if (handle == AHSV_StopTheMusic || handle == AHSV_StopTheMusicFade) {
		// for music, find and stop the currently playing music stream(s).
		for (PlayingAudio *playing : m_playingStreams) {
			if (playing->m_audioEventRTS->getAudioEventInfo()->m_soundType == AT_Music) {
				if (handle == AHSV_StopTheMusicFade) {
					playing->m_fade = true;
				} else {
					stopPlayingAudio(playing);
				}
			}
		}
		return;
	}

	// Look for it in the request list.
	for (std::list<AudioRequest *>::iterator it = m_audioRequests.begin(); it != m_audioRequests.end(); ++it) {
		AudioRequest *req = (*it);
		if (req->m_pendingEvent && req->m_pendingEvent->getPlayingHandle() == handle) {
			if (req->m_pendingEvent->getAudioEventInfo()->m_soundType == AT_SoundEffect) {
				req->m_requestStop = true;
			} else {
				deleteInstance(req);
				m_audioRequests.erase(it);
			}
			return;
		}
	}

	for (PlayingAudio *playing : m_playingStreams) {
		if (playing->m_audioEventRTS->getPlayingHandle() == handle) {
			stopPlayingAudio(playing);
			return;
		}
	}

	for (PlayingAudio *playing : m_playingSounds) {
		if (playing->m_audioEventRTS->getPlayingHandle() == handle) {
			playing->m_requestStop = true;
			return;
		}
	}

	for (PlayingAudio *playing : m_playing3DSounds) {
		if (playing->m_audioEventRTS->getPlayingHandle() == handle) {
			playing->m_requestStop = true;
			return;
		}
	}
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::killAudioEventImmediately(AudioHandle audioEvent)
{
	//First look for it in the request list.
	for (std::list<AudioRequest *>::iterator ait = m_audioRequests.begin(); ait != m_audioRequests.end(); ++ait) {
		AudioRequest *req = (*ait);
		if (req->m_pendingEvent && req->m_pendingEvent->getPlayingHandle() == audioEvent) {
			deleteInstance(req);
			m_audioRequests.erase(ait);
			return;
		}
	}

	std::list<PlayingAudio *> *lists[] = { &m_playing3DSounds, &m_playingSounds, &m_playingStreams };
	for (std::list<PlayingAudio *> *list : lists) {
		for (PlayingAudio *playing : *list) {
			if (playing->m_audioEventRTS->getPlayingHandle() == audioEvent) {
				stopPlayingAudio(playing);
				return;
			}
		}
	}
}

//-------------------------------------------------------------------------------------------------
void *OpenALAudioManager::loadFileForRead(AudioEventRTS *eventToLoadFrom)
{
	return m_audioCache->openFile(eventToLoadFrom);
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::closeFile(void *fileRead)
{
	m_audioCache->closeFile(fileRead);
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::releaseSource(PlayingAudio *playing)
{
	const ALuint source = playing->m_source;
	switch (playing->m_type)
	{
		case PAT_Sample:
		case PAT_3DSample:
		{
			if (source) {
				alSourceStop(source);
				alSourcei(source, AL_BUFFER, 0);
				if (m_lowPassFilter) {
					alSourcei(source, AL_DIRECT_FILTER, AL_FILTER_NULL);
				}
				(playing->m_type == PAT_Sample ? m_availableSamples : m_available3DSamples).push_back(source);
			}
			break;
		}
		case PAT_Stream:
		{
			if (source) {
				alSourceStop(source);
				// After a stop every queued buffer counts as processed.
				ALint queued = 0;
				alGetSourcei(source, AL_BUFFERS_QUEUED, &queued);
				while (queued-- > 0) {
					ALuint buffer = 0;
					alSourceUnqueueBuffers(source, 1, &buffer);
					alDeleteBuffers(1, &buffer);
				}
				alDeleteSources(1, &source);
			}
			delete playing->m_decoder;
			playing->m_decoder = nullptr;
			break;
		}
		default:
			break;
	}
	playing->m_source = 0;
	playing->m_type = PAT_INVALID;
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::releasePlayingAudio(PlayingAudio *playing)
{
	DEBUG_ASSERTCRASH(playing->m_status == PS_Stopped, ("PlayingAudio must be stopped by now"));
	DEBUG_ASSERTCRASH(playing->m_type == PAT_INVALID, ("PlayingAudio must be invalidated by now"));

	if (playing->m_file) {
		closeFile(playing->m_file);
		playing->m_file = nullptr;
	}

	delete playing;
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::stopPlayingAudio(PlayingAudio *playing)
{
	if (playing->m_status == PS_Stopped) {
		return;
	}
	playing->m_status = PS_Stopped;
	releaseSource(playing);
	playing->m_rerequestOnNextUpdate = false;
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::rerequestPlayingAudio(PlayingAudio *playing)
{
	AudioRequest *req = allocateAudioRequest();
	req->m_pendingEvent = playing->m_audioEventRTS;
	req->m_requiresCheckForSample = true;
	req->m_requestStop = playing->m_requestStop;
	appendAudioRequest(req);
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::rerequestPlayingAudioWhenSignalled(PlayingAudio *playing)
{
	if (playing->m_rerequestOnNextUpdate) {
		rerequestPlayingAudio(playing);
		playing->m_rerequestOnNextUpdate = false;
	}
}

//-------------------------------------------------------------------------------------------------
OpenALAudioManager::PlayingAudio *OpenALAudioManager::findActiveMusic(const AsciiString *trackName)
{
	for (PlayingAudio *playing : m_playingStreams) {
		if (!playing->isPlaying() || playing->m_fade) {
			continue;
		}
		if (playing->m_audioEventRTS->getAudioEventInfo()->m_soundType != AT_Music) {
			continue;
		}
		if (trackName && *trackName != playing->m_audioEventRTS->getEventName()) {
			continue;
		}
		return playing;
	}
	return nullptr;
}

//-------------------------------------------------------------------------------------------------
const OpenALAudioManager::PlayingAudio *OpenALAudioManager::findActiveMusic(const AsciiString *trackName) const
{
	return const_cast<OpenALAudioManager *>(this)->findActiveMusic(trackName);
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::releasePlayingAudioInListIfStopped(std::list<PlayingAudio *> &list)
{
	for (std::list<PlayingAudio *>::iterator it = list.begin(); it != list.end(); ) {
		PlayingAudio *playing = (*it);
		if (playing->m_status == PS_Stopped) {
			releasePlayingAudio(playing);
			it = list.erase(it);
		} else {
			++it;
		}
	}
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::stopAllAudioImmediately()
{
	std::list<PlayingAudio *> *lists[] = { &m_playingSounds, &m_playing3DSounds, &m_playingStreams, &m_fadingAudio };
	for (std::list<PlayingAudio *> *list : lists) {
		for (PlayingAudio *playing : *list) {
			stopPlayingAudio(playing);
		}
		releasePlayingAudioInListIfStopped(*list);
		DEBUG_ASSERTCRASH(list->empty(), ("List is expected to be empty now"));
	}

	for (ForcePlayed &forced : m_audioForcePlayed) {
		alSourceStop(forced.m_source);
		alDeleteSources(1, &forced.m_source);
		alDeleteBuffers(1, &forced.m_buffer);
	}
	m_audioForcePlayed.clear();
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::freeAllSources()
{
	// No sound may hold a source while the pools are deleted.
	stopAllAudioImmediately();

	if (!m_availableSamples.empty()) {
		alDeleteSources((ALsizei)m_availableSamples.size(), &m_availableSamples[0]);
	}
	m_availableSamples.clear();
	m_num2DSamples = 0;

	if (!m_available3DSamples.empty()) {
		alDeleteSources((ALsizei)m_available3DSamples.size(), &m_available3DSamples[0]);
	}
	m_available3DSamples.clear();
	m_num3DSamples = 0;
	m_numStreams = 0;
}

//-------------------------------------------------------------------------------------------------
ALuint OpenALAudioManager::getAvailable2DSample()
{
	if (m_availableSamples.empty()) {
		return 0;
	}
	ALuint source = m_availableSamples.back();
	m_availableSamples.pop_back();
	return source;
}

//-------------------------------------------------------------------------------------------------
ALuint OpenALAudioManager::getAvailable3DSample()
{
	if (m_available3DSamples.empty()) {
		return 0;
	}
	ALuint source = m_available3DSamples.back();
	m_available3DSamples.pop_back();
	return source;
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::stopAllSpeech()
{
	for (PlayingAudio *playing : m_playingStreams) {
		if (playing->m_audioEventRTS->getAudioEventInfo()->m_soundType == AT_Streaming) {
			stopPlayingAudio(playing);
		}
	}
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::initFilters(ALuint source, AudioEventRTS *event)
{
	alSourcef(source, AL_GAIN, getEffectiveVolume(event));

	// Sources are reused, so the pitch is always set.
	Real pitchShift = event->getPitchShift();
	if (pitchShift == 0.0f) {
		DEBUG_CRASH(("Invalid Pitch shift in sound: '%s'", event->getEventName().str()));
		pitchShift = 1.0f;
	}
	alSourcef(source, AL_PITCH, pitchShift);

	// Miles applied a "Mono Delay Filter" when the event has a delay, but with its delay and mix set to 0
	// it changed nothing, so there is no filter here. The delay itself is handled by the request list.
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::initFilters3D(ALuint source, AudioEventRTS *event, const Coord3D *pos)
{
	initFilters(source, event);

	// Low pass filter for sounds off screen. AL copies the filter into the source, so one filter object serves all.
	if (m_lowPassFilter) {
		const Real lowPass = event->getAudioEventInfo()->m_lowPassFreq;
		if (lowPass > 0.0f && !isOnScreen(pos)) {
			alFilterf(m_lowPassFilter, AL_LOWPASS_GAINHF, std::min(lowPass, 1.0f));
			alSourcei(source, AL_DIRECT_FILTER, (ALint)m_lowPassFilter);
		} else {
			alSourcei(source, AL_DIRECT_FILTER, AL_FILTER_NULL);
		}
	}
}

//-------------------------------------------------------------------------------------------------
AsciiString OpenALAudioManager::nextMusicTrack()
{
	AsciiString trackName;

	if (PlayingAudio *playing = findActiveMusic()) {
		trackName = playing->m_audioEventRTS->getEventName();
	}

	// Stop currently playing music
	TheAudio->removeAudioEvent(AHSV_StopTheMusic);

	trackName = nextTrackName(trackName);
	AudioEventRTS newTrack(trackName);
	TheAudio->addAudioEvent(&newTrack);

	return trackName;
}

//-------------------------------------------------------------------------------------------------
AsciiString OpenALAudioManager::prevMusicTrack()
{
	AsciiString trackName;

	if (PlayingAudio *playing = findActiveMusic()) {
		trackName = playing->m_audioEventRTS->getEventName();
	}

	// Stop currently playing music
	TheAudio->removeAudioEvent(AHSV_StopTheMusic);

	trackName = prevTrackName(trackName);
	AudioEventRTS newTrack(trackName);
	TheAudio->addAudioEvent(&newTrack);

	return trackName;
}

//-------------------------------------------------------------------------------------------------
Bool OpenALAudioManager::isMusicPlaying() const
{
	return findActiveMusic() != nullptr;
}

//-------------------------------------------------------------------------------------------------
Bool OpenALAudioManager::hasMusicTrackCompleted(const AsciiString &trackName, Int numberOfTimes) const
{
	const PlayingAudio *playing = findActiveMusic(&trackName);
	return playing && playing->m_loopsDone >= numberOfTimes;
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::openDevice()
{
	if (!TheGlobalData->m_audioOn) {
		return;
	}

	m_device = alcOpenDevice(nullptr);
	if (m_device) {
		m_context = alcCreateContext(m_device, nullptr);
	}

	if (m_context && alcMakeContextCurrent(m_context)) {
		// Miles passed the min and max distance in swapped order, so it had almost no distance rolloff of
		// its own. The game's getEffectiveVolume fade is the only one, and AL only pans by position.
		alDistanceModel(AL_NONE);

		if (alcIsExtensionPresent(m_device, "ALC_EXT_EFX")) {
			alGenFilters(1, &m_lowPassFilter);
			alFilteri(m_lowPassFilter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
			alFilterf(m_lowPassFilter, AL_LOWPASS_GAIN, 1.0f);
		}
	} else {
		DEBUG_LOG(("Could not open an OpenAL device. Audio will be turned off."));
		if (m_context) {
			alcDestroyContext(m_context);
			m_context = nullptr;
		}
		if (m_device) {
			alcCloseDevice(m_device);
			m_device = nullptr;
		}
		setOn(false, AudioAffect_All);
	}

	m_selectedSpeakerType = translateSpeakerTypeToUnsignedInt(m_prefSpeaker);
	selectProvider(getProviderIndex(m_pref3DProvider));

	// Now that we're all done, update the cached variables so that everything is in sync.
	refreshCachedVariables();
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::closeDevice()
{
	if (!m_context) {
		return;
	}

	freeAllSources();
	unselectProvider();
	releaseHandleForBink(); // its source must go before the context

	if (m_lowPassFilter) {
		alDeleteFilters(1, &m_lowPassFilter);
		m_lowPassFilter = 0;
	}

	alcMakeContextCurrent(nullptr);
	alcDestroyContext(m_context);
	m_context = nullptr;
	alcCloseDevice(m_device);
	m_device = nullptr;
}

//-------------------------------------------------------------------------------------------------
Bool OpenALAudioManager::isCurrentlyPlaying(AudioHandle handle)
{
	const std::list<PlayingAudio *> *lists[] = { &m_playingSounds, &m_playing3DSounds, &m_playingStreams };
	for (const std::list<PlayingAudio *> *list : lists) {
		for (PlayingAudio *playing : *list) {
			if (playing->isPlayingOrRequested() && playing->m_audioEventRTS->getPlayingHandle() == handle) {
				return true;
			}
		}
	}

	// if something is requested, it is also considered playing
	for (AudioRequest *req : m_audioRequests) {
		if (req->m_pendingEvent && req->m_pendingEvent->getPlayingHandle() == handle) {
			return true;
		}
	}

	return false;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt OpenALAudioManager::getProviderCount() const
{
	return m_context ? 1 : 0;
}

//-------------------------------------------------------------------------------------------------
AsciiString OpenALAudioManager::getProviderName(UnsignedInt providerNum) const
{
	if (isOn(AudioAffect_Sound3D) && providerNum < getProviderCount()) {
		return PROVIDER_NAME;
	}

	return AsciiString::TheEmptyString;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt OpenALAudioManager::getProviderIndex(AsciiString providerName) const
{
	return providerName == PROVIDER_NAME ? 0 : PROVIDER_ERROR;
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::selectProvider(UnsignedInt providerNdx)
{
	// There is one provider, the default device, so any index selects it (Miles also overrode the index).
	if (!isOn(AudioAffect_Sound3D) || !m_context || isValidProvider()) {
		return;
	}

	m_selectedProvider = 0;
	initSamplePools();
	setSpeakerType(m_selectedSpeakerType);

	if (TheVideoPlayer) {
		TheVideoPlayer->notifyVideoPlayerOfNewProvider(TRUE);
	}
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::unselectProvider()
{
	if (!(isOn(AudioAffect_Sound3D) && isValidProvider())) {
		return;
	}

	if (TheVideoPlayer) {
		TheVideoPlayer->notifyVideoPlayerOfNewProvider(FALSE);
	}

	m_selectedProvider = PROVIDER_ERROR;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt OpenALAudioManager::getSelectedProvider() const
{
	return m_selectedProvider;
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::setSpeakerType(UnsignedInt speakerType)
{
	// OpenAL Soft follows the Windows speaker setup by itself; the type is only kept for the options screen.
	if (!isValidProvider()) {
		return;
	}
	m_selectedSpeakerType = speakerType;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt OpenALAudioManager::getSpeakerType()
{
	if (!isValidProvider()) {
		return 0;
	}
	return m_selectedSpeakerType;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt OpenALAudioManager::getNum2DSamples() const
{
	return m_num2DSamples;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt OpenALAudioManager::getNum3DSamples() const
{
	return m_num3DSamples;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt OpenALAudioManager::getNumStreams() const
{
	return m_numStreams;
}

//-------------------------------------------------------------------------------------------------
UnsignedInt OpenALAudioManager::getNumAvailable2DSamples() const
{
	return (UnsignedInt)m_availableSamples.size();
}

//-------------------------------------------------------------------------------------------------
UnsignedInt OpenALAudioManager::getNumAvailable3DSamples() const
{
	return (UnsignedInt)m_available3DSamples.size();
}

//-------------------------------------------------------------------------------------------------
Bool OpenALAudioManager::doesViolateLimit(AudioEventRTS *event) const
{
	Int limit = event->getAudioEventInfo()->m_limit;
	if (limit == 0) {
		return false;
	}

	Int totalCount = 0;
	Int totalRequestCount = 0;

	const std::list<PlayingAudio *> &list = event->isPositionalAudio() ? m_playing3DSounds : m_playingSounds;
	for (const PlayingAudio *playing : list) {
		if (!playing->isPlayingOrRequested()) {
			continue;
		}
		if (playing->m_audioEventRTS->getEventName() == event->getEventName()) {
			if (totalCount == 0) {
				// This is the oldest audio of this type playing.
				event->setHandleToKill(playing->m_audioEventRTS->getPlayingHandle());
			}
			++totalCount;
		}
	}

	// Also check the request list in case we've requested to play this sound.
	for (AudioRequest *req : m_audioRequests) {
		if (req->m_pendingEvent && req->m_pendingEvent->getEventName() == event->getEventName()) {
			totalRequestCount++;
			totalCount++;
		}
	}

	//If our event is an interrupting type, then normally we would always add it. The exception is when we have requested
	//multiple sounds in the same frame and those requests violate the limit. Because we don't have any "old" sounds to
	//remove in the case of an interrupt, we need to catch it early and prevent the sound from being added if we already
	//reached the limit
	if (event->getAudioEventInfo()->m_control & AC_INTERRUPT) {
		if (totalRequestCount < limit) {
			Int totalPlayingCount = totalCount - totalRequestCount;
			if (totalRequestCount + totalPlayingCount < limit) {
				//We aren't exceeding the actual limit, then clear the kill handle.
				event->setHandleToKill(0);
				return false;
			}

			//We are exceeding the limit - the kill handle will kill the
			//oldest playing sound to enforce the actual limit.
			return false;
		}
	}

	if (totalCount < limit) {
		event->setHandleToKill(0);
		return false;
	}

	return true;
}

//-------------------------------------------------------------------------------------------------
Bool OpenALAudioManager::isPlayingAlready(AudioEventRTS *event) const
{
	const std::list<PlayingAudio *> &list = event->isPositionalAudio() ? m_playing3DSounds : m_playingSounds;
	for (const PlayingAudio *playing : list) {
		if (playing->isPlaying() && playing->m_audioEventRTS->getEventName() == event->getEventName()) {
			return true;
		}
	}
	return false;
}

//-------------------------------------------------------------------------------------------------
Bool OpenALAudioManager::isObjectPlayingVoice(UnsignedInt objID) const
{
	if (objID == 0) {
		return false;
	}

	const std::list<PlayingAudio *> *lists[] = { &m_playingSounds, &m_playing3DSounds };
	for (const std::list<PlayingAudio *> *list : lists) {
		for (const PlayingAudio *playing : *list) {
			if (!playing->isPlaying()) {
				continue;
			}
			if (playing->m_audioEventRTS->getObjectID() == objID && playing->m_audioEventRTS->getAudioEventInfo()->m_type & ST_VOICE) {
				return true;
			}
		}
	}

	return false;
}

//-------------------------------------------------------------------------------------------------
AudioEventRTS *OpenALAudioManager::findLowestPrioritySound(AudioEventRTS *event)
{
	const AudioPriority priority = event->getAudioEventInfo()->m_priority;
	if (priority == AP_LOWEST) {
		//If the event we pass in is the lowest priority, don't bother checking because
		//there is nothing lower priority than lowest.
		return nullptr;
	}

	AudioEventRTS *lowestPriorityEvent = nullptr;
	AudioPriority lowestPriority = priority;

	const std::list<PlayingAudio *> &list = event->isPositionalAudio() ? m_playing3DSounds : m_playingSounds;
	for (PlayingAudio *playing : list) {
		if (!playing->isPlaying()) {
			continue;
		}
		AudioEventRTS *itEvent = playing->m_audioEventRTS.Peek();
		const AudioPriority itPriority = itEvent->getAudioEventInfo()->m_priority;
		if (itPriority < lowestPriority) {
			lowestPriorityEvent = itEvent;
			lowestPriority = itPriority;
			if (lowestPriority == AP_LOWEST) {
				return lowestPriorityEvent;
			}
		}
	}
	return lowestPriorityEvent;
}

//-------------------------------------------------------------------------------------------------
Bool OpenALAudioManager::isPlayingLowerPriority(AudioEventRTS *event) const
{
	const AudioPriority priority = event->getAudioEventInfo()->m_priority;
	if (priority == AP_LOWEST) {
		return false;
	}

	const std::list<PlayingAudio *> &list = event->isPositionalAudio() ? m_playing3DSounds : m_playingSounds;
	for (const PlayingAudio *playing : list) {
		if (playing->isPlaying() && playing->m_audioEventRTS->getAudioEventInfo()->m_priority < priority) {
			return true;
		}
	}

	return false;
}

//-------------------------------------------------------------------------------------------------
Bool OpenALAudioManager::killLowestPrioritySoundImmediately(AudioEventRTS *event)
{
	AudioEventRTS *lowestPriorityEvent = findLowestPrioritySound(event);
	if (lowestPriorityEvent) {
		std::list<PlayingAudio *> &list = event->isPositionalAudio() ? m_playing3DSounds : m_playingSounds;
		for (PlayingAudio *playing : list) {
			if (playing->isPlaying() && playing->m_audioEventRTS.Peek() == lowestPriorityEvent) {
				// Stop this sound channel immediately because we are going to play another sound in it's place.
				stopPlayingAudio(playing);
				return TRUE;
			}
		}
	}
	return FALSE;
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::adjustVolumeOfPlayingAudio(AsciiString eventName, Real newVolume)
{
	std::list<PlayingAudio *> *lists[] = { &m_playingSounds, &m_playing3DSounds, &m_playingStreams };
	for (std::list<PlayingAudio *> *list : lists) {
		for (PlayingAudio *playing : *list) {
			if (!playing->isPlaying()) {
				continue;
			}
			AudioEventRTS *itEvent = playing->m_audioEventRTS.Peek();
			if (itEvent->getEventName() == eventName) {
				itEvent->setVolume(newVolume);
				alSourcef(playing->m_source, AL_GAIN, getEffectiveVolume(itEvent));
			}
		}
	}
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::removePlayingAudio(AsciiString eventName)
{
	std::list<PlayingAudio *> *lists[] = { &m_playingSounds, &m_playing3DSounds, &m_playingStreams };
	for (std::list<PlayingAudio *> *list : lists) {
		for (PlayingAudio *playing : *list) {
			if (playing->m_audioEventRTS->getEventName() == eventName) {
				stopPlayingAudio(playing);
			}
		}
	}
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::removeAllDisabledAudio()
{
	std::list<PlayingAudio *> *lists[] = { &m_playingSounds, &m_playing3DSounds, &m_playingStreams };
	for (std::list<PlayingAudio *> *list : lists) {
		for (PlayingAudio *playing : *list) {
			if (playing->m_audioEventRTS->getVolume() == 0.0f) {
				stopPlayingAudio(playing);
			}
		}
	}
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::processRequestList()
{
	for (std::list<AudioRequest *>::iterator it = m_audioRequests.begin(); it != m_audioRequests.end(); ) {
		AudioRequest *req = (*it);
		if (!shouldProcessRequestThisFrame(req)) {
			adjustRequest(req);
			++it;
			continue;
		}

		if (!req->m_requiresCheckForSample || checkForSample(req)) {
			processRequest(req);
		}
		deleteInstance(req);
		it = m_audioRequests.erase(it);
	}
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::processPlayingList()
{
	for (PlayingAudio *playing : m_playingSounds) {
		if (playing->m_status == PS_Stopping) {
			rerequestPlayingAudioWhenSignalled(playing);
			stopPlayingAudio(playing);
			continue;
		}
		if (playing->m_status == PS_Stopped) {
			continue;
		}
		if (m_volumeHasChanged) {
			alSourcef(playing->m_source, AL_GAIN, getEffectiveVolume(playing->m_audioEventRTS.Peek()));
		}
	}

	for (PlayingAudio *playing : m_playing3DSounds) {
		if (playing->m_status == PS_Stopping) {
			rerequestPlayingAudioWhenSignalled(playing);
			stopPlayingAudio(playing);
			continue;
		}
		if (playing->m_status == PS_Stopped) {
			continue;
		}

		const Coord3D *pos = getCurrentPositionFromEvent(playing->m_audioEventRTS.Peek());
		if (!pos) {
			stopPlayingAudio(playing);
			continue;
		}

		if (playing->m_audioEventRTS->isDead()) {
			stopAudioEvent(playing->m_audioEventRTS->getPlayingHandle());
			continue;
		}

		// Gain follows the camera every frame: with no AL distance model this is the only distance fade.
		const Real volume = getEffectiveVolume(playing->m_audioEventRTS.Peek());
		Real volForConsideration = volume;
		volForConsideration /= (m_sound3DVolume > 0.0f ? m_soundVolume : 1.0f);
		Bool playAnyways = BitIsSet(playing->m_audioEventRTS->getAudioEventInfo()->m_type, ST_GLOBAL) ||
			playing->m_audioEventRTS->getAudioEventInfo()->m_priority == AP_CRITICAL;
		if (volForConsideration < m_audioSettings->m_minVolume && !playAnyways) {
			stopPlayingAudio(playing);
		} else {
			alSourcef(playing->m_source, AL_GAIN, volume);
			alSource3f(playing->m_source, AL_POSITION, pos->x, pos->y, pos->z);
		}
	}

	for (PlayingAudio *playing : m_playingStreams) {
		if (playing->m_status == PS_Stopping) {
			rerequestPlayingAudioWhenSignalled(playing);
			stopPlayingAudio(playing);
			continue;
		}
		if (playing->m_status == PS_Stopped) {
			continue;
		}
		if (m_volumeHasChanged) {
			alSourcef(playing->m_source, AL_GAIN, getEffectiveVolume(playing->m_audioEventRTS.Peek()));
		}
	}

	releasePlayingAudioInListIfStopped(m_playingSounds);
	releasePlayingAudioInListIfStopped(m_playing3DSounds);
	releasePlayingAudioInListIfStopped(m_playingStreams);

	// Transfer streams to fade list when signaled.
	for (std::list<PlayingAudio *>::iterator it = m_playingStreams.begin(); it != m_playingStreams.end(); ) {
		if ((*it)->m_fade) {
			m_fadingAudio.push_back(*it);
			it = m_playingStreams.erase(it);
		} else {
			++it;
		}
	}

	if (m_volumeHasChanged) {
		m_volumeHasChanged = false;

		// Video audio plays on its own stream, outside the lists above.
		if (TheVideoPlayer) {
			TheVideoPlayer->setVolume(getVolume(AudioAffect_Speech));
		}
		if (m_binkStream) {
			m_binkStream->setVolume(getVolume(AudioAffect_Speech));
		}
	}
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::processFadingList()
{
	for (PlayingAudio *playing : m_fadingAudio) {
		if (playing->m_framesFaded >= getAudioSettings()->m_fadeAudioFrames) {
			stopPlayingAudio(playing);
			continue;
		}

		if (playing->m_status == PS_Stopped) {
			continue;
		}

		++playing->m_framesFaded;
		Real volume = getEffectiveVolume(playing->m_audioEventRTS.Peek());
		volume *= (1.0f - 1.0f * playing->m_framesFaded / getAudioSettings()->m_fadeAudioFrames);
		alSourcef(playing->m_source, AL_GAIN, volume);
	}

	releasePlayingAudioInListIfStopped(m_fadingAudio);
}

//-------------------------------------------------------------------------------------------------
Bool OpenALAudioManager::shouldProcessRequestThisFrame(AudioRequest *req) const
{
	if (req->m_pendingEvent == nullptr) {
		return true;
	}

	return req->m_pendingEvent->getDelay() < MSEC_PER_LOGICFRAME_REAL;
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::adjustRequest(AudioRequest *req)
{
	if (req->m_pendingEvent == nullptr) {
		return;
	}

	req->m_pendingEvent->decrementDelay(MSEC_PER_LOGICFRAME_REAL);
	req->m_requiresCheckForSample = true;
}

//-------------------------------------------------------------------------------------------------
Bool OpenALAudioManager::checkForSample(AudioRequest *req)
{
	if (req->m_pendingEvent == nullptr) {
		return true;
	}

	if (req->m_pendingEvent->getAudioEventInfo() == nullptr) {
		// Fill in event info
		getInfoForAudioEvent(req->m_pendingEvent.Peek());
	}

	// Kept from Miles for parity: this compares m_type (ST_ flags) with an AudioType value.
	if (req->m_pendingEvent->getAudioEventInfo()->m_type != AT_SoundEffect) {
		return true;
	}

	return m_sound->canPlayNow(req->m_pendingEvent.Peek());
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::setSpeakerSurround(Bool surround)
{
	Bool retEarly = (surround == m_surroundSpeakers);
	AudioManager::setSpeakerSurround(surround);

	if (retEarly) {
		return;
	}

	UnsignedInt speakerType;
	if (m_surroundSpeakers) {
		speakerType = getAudioSettings()->m_defaultSpeakerType3D;
	} else {
		speakerType = getAudioSettings()->m_defaultSpeakerType2D;
	}

	setSpeakerType(speakerType);
}

//-------------------------------------------------------------------------------------------------
Real OpenALAudioManager::getFileLengthMS(AsciiString strToLoad) const
{
	if (strToLoad.isEmpty()) {
		return 0.0f;
	}

	UnsignedInt size = 0;
	char *data = readWholeFile(strToLoad, size);
	if (!data) {
		return 0.0f;
	}

	drwav_uint64 frames = 0;
	UnsignedInt rate = 0;
	if (isRiff(data, size)) {
		drwav wav;
		if (drwav_init_memory(&wav, data, size, nullptr)) {
			frames = wav.totalPCMFrameCount;
			rate = wav.sampleRate;
			drwav_uninit(&wav);
		}
	} else {
		drmp3 mp3;
		if (drmp3_init_memory(&mp3, data, size, nullptr)) {
			rate = mp3.sampleRate;
			frames = drmp3_get_pcm_frame_count(&mp3);
			drmp3_uninit(&mp3);
		}
	}
	delete [] data;

	if (rate == 0) {
		return 0.0f;
	}
	return INT_TO_REAL((Int)(frames * 1000 / rate));
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::closeAnySamplesUsingFile(const void *fileToClose)
{
	std::list<PlayingAudio *> *lists[] = { &m_playingSounds, &m_playing3DSounds };
	for (std::list<PlayingAudio *> *list : lists) {
		for (PlayingAudio *playing : *list) {
			if (playing->m_file == fileToClose) {
				stopPlayingAudio(playing);
				playing->m_file = nullptr; // the cache is releasing it
			}
		}
	}
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::setDeviceListenerPosition()
{
	if (!m_context) {
		return;
	}

	// Miles used up = (0,0,-1) in its left-handed space; AL is right-handed, so up is +Z here.
	const ALfloat orientation[6] = { m_listenerOrientation.x, m_listenerOrientation.y, m_listenerOrientation.z, 0.0f, 0.0f, 1.0f };
	alListenerfv(AL_ORIENTATION, orientation);
	alListener3f(AL_POSITION, m_listenerPosition.x, m_listenerPosition.y, m_listenerPosition.z);
}

//-------------------------------------------------------------------------------------------------
const Coord3D *OpenALAudioManager::getCurrentPositionFromEvent(AudioEventRTS *event)
{
	if (!event->isPositionalAudio()) {
		return nullptr;
	}

	return event->getCurrentPosition();
}

//-------------------------------------------------------------------------------------------------
Bool OpenALAudioManager::isOnScreen(const Coord3D *pos) const
{
	static ICoord2D dummy;
	// WorldToScreen will return True if the point is onscreen and false if it is offscreen.
	return TheTacticalView->worldToScreen(pos, &dummy);
}

//-------------------------------------------------------------------------------------------------
Real OpenALAudioManager::getEffectiveVolume(AudioEventRTS *event) const
{
	Real volume = event->getVolume() * event->getVolumeShift();

	switch (event->getAudioEventInfo()->m_soundType)
	{
	case AT_Music:
	{
		volume *= m_musicVolume;
		break;
	}
	case AT_Streaming:
	{
		volume *= m_speechVolume;
		break;
	}
	case AT_SoundEffect:
	{
		if (event->isPositionalAudio())
		{
			volume *= m_sound3DVolume;
			const Coord3D *pos = event->getCurrentPosition();
			if (pos)
			{
				Coord3D distance = m_listenerPosition;
				distance.sub(*pos);
				Real objMinDistance;
				Real objMaxDistance;

				const AudioSettings *audioSettings = getAudioSettings();

				if (event->getAudioEventInfo()->m_type & ST_GLOBAL)
				{
					objMinDistance = audioSettings->m_globalMinRange;
					objMaxDistance = audioSettings->m_globalMaxRange;
				}
				else
				{
					objMinDistance = event->getAudioEventInfo()->m_minDistance;
					objMaxDistance = event->getAudioEventInfo()->m_maxDistance;
				}

				const Real objDistance = distance.length();

				if (objDistance >= objMaxDistance)
				{
					volume = 0.0f;
				}
				else if (audioSettings->m_use3DSoundRangeVolumeFade && objDistance > objMinDistance)
				{
					Real attenuation = (objDistance - objMinDistance) / (objMaxDistance - objMinDistance);
					attenuation = pow(attenuation, audioSettings->m_3DSoundRangeVolumeFadeExponent);
					volume *= 1.0f - attenuation;
				}
			}
		}
		else
		{
			volume *= m_soundVolume;
		}
		break;
	}
	}

	return volume;
}

//-------------------------------------------------------------------------------------------------
Bool OpenALAudioManager::startNextLoop(PlayingAudio *playing)
{
	closeFile(playing->m_file);
	playing->m_file = nullptr;

	if (!playing->isPlaying()) {
		return false;
	}

	if (playing->m_requestStop) {
		return false;
	}

	if (playing->m_audioEventRTS->hasMoreLoops()) {
		// generate a new filename, and test to see whether we can play with it now
		playing->m_audioEventRTS->generateFilename();

		if (playing->m_audioEventRTS->getDelay() > MSEC_PER_LOGICFRAME_REAL) {
			playing->m_rerequestOnNextUpdate = true;
			playing->m_status = PS_Stopping;
			return true;
		}

		if (playing->m_type == PAT_3DSample) {
			playing->m_file = playSample3D(playing->m_audioEventRTS.Peek(), playing);
		} else {
			playing->m_file = playSample(playing->m_audioEventRTS.Peek(), playing);
		}

		return playing->m_file != nullptr;
	}
	return false;
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::playStream(AudioEventRTS *event, PlayingAudio *playing)
{
	alGenSources(1, &playing->m_source);
	if (!playing->m_source) {
		return;
	}

	alSourcei(playing->m_source, AL_SOURCE_RELATIVE, AL_TRUE);
	alSource3f(playing->m_source, AL_POSITION, 0.0f, 0.0f, 0.0f);
	alSourcef(playing->m_source, AL_GAIN, getEffectiveVolume(event));

	playing->m_decoder->m_loop = (event->getAudioEventInfo()->m_soundType == AT_Music);
	pumpStream(playing);
}

//-------------------------------------------------------------------------------------------------
void *OpenALAudioManager::playSample(AudioEventRTS *event, PlayingAudio *playing)
{
	const ALuint source = playing->m_source;
	// Detach the old buffer first, or the cache could not delete it when it frees space below.
	alSourceStop(source);
	alSourcei(source, AL_BUFFER, 0);

	initFilters(source, event);

	OpenAudioFile *file = static_cast<OpenAudioFile *>(loadFileForRead(event));
	if (file) {
		alSourcei(source, AL_BUFFER, (ALint)file->m_buffer);
		alSourcePlay(source);
	}

	return file;
}

//-------------------------------------------------------------------------------------------------
void *OpenALAudioManager::playSample3D(AudioEventRTS *event, PlayingAudio *playing)
{
	const Coord3D *pos = getCurrentPositionFromEvent(event);
	if (!pos) {
		return nullptr;
	}

	const ALuint source = playing->m_source;
	alSourceStop(source);
	alSourcei(source, AL_BUFFER, 0);

	OpenAudioFile *file = static_cast<OpenAudioFile *>(loadFileForRead(event));
	if (file) {
		alSourcei(source, AL_BUFFER, (ALint)file->m_buffer);
		alSource3f(source, AL_POSITION, pos->x, pos->y, pos->z);
		initFilters3D(source, event, pos);
		alSourcePlay(source);
	}

	return file;
}

//-------------------------------------------------------------------------------------------------
Bool OpenALAudioManager::isValidProvider() const
{
	return m_selectedProvider < getProviderCount();
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::initSamplePools()
{
	if (!(isOn(AudioAffect_Sound3D) && isValidProvider())) {
		return;
	}

	const AudioSettings *settings = getAudioSettings();
	m_availableSamples.reserve(settings->m_sampleCount2D);
	m_available3DSamples.reserve(settings->m_sampleCount3D);

	for (Int i = 0; i < settings->m_sampleCount2D; ++i) {
		ALuint source = 0;
		alGenSources(1, &source);
		DEBUG_ASSERTCRASH(source, ("Couldn't get %d 2D samples", i + 1));
		if (source) {
			alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
			alSource3f(source, AL_POSITION, 0.0f, 0.0f, 0.0f);
			m_availableSamples.push_back(source);
			++m_num2DSamples;
		}
	}

	for (Int i = 0; i < settings->m_sampleCount3D; ++i) {
		ALuint source = 0;
		alGenSources(1, &source);
		DEBUG_ASSERTCRASH(source, ("Couldn't get %d 3D samples", i + 1));
		if (source) {
			alSourcei(source, AL_SOURCE_RELATIVE, AL_FALSE);
			m_available3DSamples.push_back(source);
			++m_num3DSamples;
		}
	}

	// Streams make their own source when they start.
	m_numStreams = settings->m_streamCount;
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::processRequest(AudioRequest *req)
{
	switch (req->m_request)
	{
		case AR_Play:
			playAudioEvent(req);
			break;
		case AR_Pause:
			break; // Miles did nothing here either
		case AR_Stop:
			stopAudioEvent(req->m_handleToInteractOn);
			break;
	}
}

//-------------------------------------------------------------------------------------------------
void *OpenALAudioManager::getHandleForBink()
{
	// The FFmpeg player uses the handle without a null check, so this always returns a stream.
	if (!m_binkStream) {
		m_binkStream = NEW OpenALAudioStream;
		m_binkStream->setVolume(getVolume(AudioAffect_Speech));
	}
	return m_binkStream;
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::releaseHandleForBink()
{
	delete m_binkStream;
	m_binkStream = nullptr;
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::friend_forcePlayAudioEventRTS(const AudioEventRTS *eventToPlay)
{
	if (!eventToPlay->getAudioEventInfo()) {
		getInfoForAudioEvent(eventToPlay);
		if (!eventToPlay->getAudioEventInfo()) {
			DEBUG_CRASH(("No info for forced audio event '%s'", eventToPlay->getEventName().str()));
			return;
		}
	}

	switch (eventToPlay->getAudioEventInfo()->m_soundType)
	{
		case AT_Music:
			if (!isOn(AudioAffect_Music))
				return;
			break;
		case AT_SoundEffect:
			if (!isOn(AudioAffect_Sound) || !isOn(AudioAffect_Sound3D))
				return;
			break;
		case AT_Streaming:
			if (!isOn(AudioAffect_Speech))
				return;
			break;
	}

	if (!m_context) {
		return;
	}

	AudioEventRTS event = *eventToPlay;

	event.generateFilename();
	event.generatePlayInfo();

	for (std::list<std::pair<AsciiString, Real>/**/>::iterator it = m_adjustedVolumes.begin(); it != m_adjustedVolumes.end(); ++it) {
		if (it->first == event.getEventName()) {
			event.setVolume(it->second);
			break;
		}
	}

	UnsignedInt size = 0;
	char *data = readWholeFile(event.getFilename(), size);
	if (!data) {
		return;
	}

	UnsignedInt channels = 0, rate = 0;
	drwav_uint64 frames = 0;
	void *pcm = decodeWhole(data, size, channels, rate, frames);
	const ALenum format = getALFormat(channels, 16);

	ForcePlayed forced = { 0, 0 };
	if (pcm && format != AL_NONE) {
		alGenBuffers(1, &forced.m_buffer);
		alBufferData(forced.m_buffer, format, pcm, (ALsizei)(frames * channels * sizeof(drwav_int16)), rate);
		alGenSources(1, &forced.m_source);
	}
	if (pcm) {
		freePcm(data, size, pcm);
	}
	delete [] data;

	if (!forced.m_source) {
		alDeleteBuffers(1, &forced.m_buffer);
		return;
	}

	// Even though the event type is not Speech, this is used only for mission briefings, so use the
	// speech slider to adjust the volume.
	alSourcei(forced.m_source, AL_SOURCE_RELATIVE, AL_TRUE);
	alSource3f(forced.m_source, AL_POSITION, 0.0f, 0.0f, 0.0f);
	alSourcei(forced.m_source, AL_BUFFER, (ALint)forced.m_buffer);
	alSourcef(forced.m_source, AL_GAIN, event.getVolume() * getVolume(AudioAffect_Speech));
	alSourcePlay(forced.m_source);
	m_audioForcePlayed.push_back(forced);
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
OpenALAudioManager::AudioFileCache::~AudioFileCache()
{
	for (std::map<AsciiString, OpenAudioFile *>::iterator it = m_openFiles.begin(); it != m_openFiles.end(); ++it) {
		if (it->second->m_openCount > 0) {
			DEBUG_CRASH(("Sample '%s' is still playing, and we're trying to quit.", it->second->m_eventInfo->m_audioName.str()));
		}
		releaseOpenAudioFile(it->second);
		delete it->second;
	}
}

//-------------------------------------------------------------------------------------------------
void *OpenALAudioManager::AudioFileCache::openFile(AudioEventRTS *eventToOpenFrom)
{
	AsciiString strToFind;
	switch (eventToOpenFrom->getNextPlayPortion())
	{
		case PP_Attack:
			strToFind = eventToOpenFrom->getAttackFilename();
			break;
		case PP_Sound:
			strToFind = eventToOpenFrom->getFilename();
			break;
		case PP_Decay:
			strToFind = eventToOpenFrom->getDecayFilename();
			break;
		case PP_Done:
			return nullptr;
	}

	std::map<AsciiString, OpenAudioFile *>::iterator it = m_openFiles.find(strToFind);
	if (it != m_openFiles.end()) {
		++it->second->m_openCount;
		return it->second;
	}

	// Couldn't find the file, so actually open it.
	UnsignedInt size = 0;
	char *data = readWholeFile(strToFind, size);
	if (!data) {
		DEBUG_ASSERTLOG(strToFind.isEmpty(), ("Missing Audio File: '%s'", strToFind.str()));
		return nullptr;
	}

	UnsignedInt channels = 0, rate = 0;
	drwav_uint64 frames = 0;
	void *pcm = decodeWhole(data, size, channels, rate, frames);
	if (!pcm) {
		DEBUG_CRASH(("Could not decode audio file '%s'", strToFind.str()));
		delete [] data;
		return nullptr;
	}

	const ALenum format = OpenALAudioManager::getALFormat(channels, 16);
	if (format == AL_NONE || (eventToOpenFrom->isPositionalAudio() && channels > 1)) {
		DEBUG_CRASH(("Requested Positional Play of audio '%s', but it is in stereo, or it has %u channels.", strToFind.str(), channels));
		freePcm(data, size, pcm);
		delete [] data;
		return nullptr;
	}

	OpenAudioFile *openedAudioFile = NEW OpenAudioFile;
	openedAudioFile->m_buffer = 0;
	openedAudioFile->m_openCount = 1;
	openedAudioFile->m_fileSize = (UnsignedInt)(frames * channels * sizeof(drwav_int16));
	openedAudioFile->m_eventInfo = eventToOpenFrom->getAudioEventInfo();

	m_currentlyUsedSize += openedAudioFile->m_fileSize;
	if (m_currentlyUsedSize > m_maxSize) {
		// We need to free some samples, or we're not going to be able to play this sound.
		if (!freeEnoughSpaceForSample(*openedAudioFile)) {
			m_currentlyUsedSize -= openedAudioFile->m_fileSize;
			delete openedAudioFile;
			freePcm(data, size, pcm);
			delete [] data;
			return nullptr;
		}
	}

	alGenBuffers(1, &openedAudioFile->m_buffer);
	alBufferData(openedAudioFile->m_buffer, format, pcm, (ALsizei)openedAudioFile->m_fileSize, rate);
	freePcm(data, size, pcm);
	delete [] data;

	m_openFiles[strToFind] = openedAudioFile;
	return openedAudioFile;
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::AudioFileCache::closeFile(void *fileToClose)
{
	if (!fileToClose) {
		return;
	}

	for (std::map<AsciiString, OpenAudioFile *>::iterator it = m_openFiles.begin(); it != m_openFiles.end(); ++it) {
		if (it->second == fileToClose) {
			--it->second->m_openCount;
			return;
		}
	}
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::AudioFileCache::releaseOpenAudioFile(OpenAudioFile *fileToRelease)
{
	if (fileToRelease->m_openCount > 0) {
		// This thing needs to be terminated IMMEDIATELY.
		TheAudio->closeAnySamplesUsingFile(fileToRelease);
	}

	if (fileToRelease->m_buffer) {
		alDeleteBuffers(1, &fileToRelease->m_buffer);
		fileToRelease->m_buffer = 0;
	}
	fileToRelease->m_eventInfo = nullptr;
}

//-------------------------------------------------------------------------------------------------
Bool OpenALAudioManager::AudioFileCache::freeEnoughSpaceForSample(const OpenAudioFile &sampleThatNeedsSpace)
{
	Int spaceRequired = m_currentlyUsedSize - m_maxSize;
	Int runningTotal = 0;

	std::list<AsciiString> filesToClose;
	// First, search for any samples that have ref counts of 0. They are low-hanging fruit, and
	// should be considered immediately.
	std::map<AsciiString, OpenAudioFile *>::iterator it;
	for (it = m_openFiles.begin(); it != m_openFiles.end(); ++it) {
		if (it->second->m_openCount == 0) {
			filesToClose.push_back(it->first);
			runningTotal += it->second->m_fileSize;
			if (runningTotal >= spaceRequired) {
				break;
			}
		}
	}

	// If we don't have enough space yet, then search through the events who have a count of 1 or more
	// and who are lower priority than this sound.
	if (runningTotal < spaceRequired) {
		for (it = m_openFiles.begin(); it != m_openFiles.end(); ++it) {
			if (it->second->m_openCount > 0) {
				if (it->second->m_eventInfo->m_priority < sampleThatNeedsSpace.m_eventInfo->m_priority) {
					filesToClose.push_back(it->first);
					runningTotal += it->second->m_fileSize;
					if (runningTotal >= spaceRequired) {
						break;
					}
				}
			}
		}
	}

	// We weren't able to find enough sounds to truncate. Therefore, this sound is not going to play.
	if (runningTotal < spaceRequired) {
		return FALSE;
	}

	for (std::list<AsciiString>::iterator ait = filesToClose.begin(); ait != filesToClose.end(); ++ait) {
		std::map<AsciiString, OpenAudioFile *>::iterator itToErase = m_openFiles.find(*ait);
		if (itToErase != m_openFiles.end()) {
			OpenAudioFile *file = itToErase->second;
			releaseOpenAudioFile(file);
			m_currentlyUsedSize -= file->m_fileSize;
			delete file;
			m_openFiles.erase(itToErase);
		}
	}

	return TRUE;
}

#if defined(RTS_DEBUG)
//-------------------------------------------------------------------------------------------------
void OpenALAudioManager::dumpAllAssetsUsed()
{
	if (!TheGlobalData->m_preloadReport) {
		return;
	}

	// Dump all the audio assets we've used.
	FILE *logfile = fopen("PreloadedAssets.txt", "a+"); //append to log
	if (!logfile)
		return;

	std::list<AsciiString> missingEvents;
	std::list<AsciiString> usedFiles;

	fprintf(logfile, "\nAudio Asset Report - BEGIN\n");
	for (std::set<AsciiString>::iterator it = m_allEventsLoaded.begin(); it != m_allEventsLoaded.end(); ++it) {
		AsciiString astr = *it;
		AudioEventInfo *aei = findAudioEventInfo(astr);
		if (!aei) {
			missingEvents.push_back(astr);
			continue;
		}

		usedFiles.insert(usedFiles.end(), aei->m_attackSounds.begin(), aei->m_attackSounds.end());
		usedFiles.insert(usedFiles.end(), aei->m_sounds.begin(), aei->m_sounds.end());
		usedFiles.insert(usedFiles.end(), aei->m_decaySounds.begin(), aei->m_decaySounds.end());

		if (!aei->m_filename.isEmpty()) {
			usedFiles.push_back(aei->m_filename);
		}
	}

	fprintf(logfile, "\nEvents Requested that are missing information - BEGIN\n");
	for (std::list<AsciiString>::iterator lit = missingEvents.begin(); lit != missingEvents.end(); ++lit) {
		fprintf(logfile, "%s\n", (*lit).str());
	}
	fprintf(logfile, "\nEvents Requested that are missing information - END\n");

	fprintf(logfile, "\nFiles Used - BEGIN\n");
	for (std::list<AsciiString>::iterator lit = usedFiles.begin(); lit != usedFiles.end(); ++lit) {
		fprintf(logfile, "%s\n", (*lit).str());
	}
	fprintf(logfile, "\nFiles Used - END\n");

	fprintf(logfile, "\nAudio Asset Report - END\n");
	fclose(logfile);
}
#endif
