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

// FILE: OpenALAudioManager.h /////////////////////////////////////////////////////////////////////
// EastWind: our own audio system on OpenAL Soft. The game logic (limits, priorities, loops, fades,
// the file cache) is ported from MilesAudioManager. Completion is found by polling in update(),
// so there is no callback thread and no locking.

#pragma once

#include "Common/AsciiString.h"
#include "Common/GameAudio.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <list>
#include <map>
#include <set>
#include <vector>

class AudioEventRTS;
class DynamicAudioEventRTS;
class OpenALAudioStream;
struct AudioEventInfo;

class OpenALAudioManager : public AudioManager
{
public:
	/// Returns the AL buffer format for the sample layout, or AL_NONE. 32 bits means float samples.
	static ALenum getALFormat(int channels, int bits);

#if defined(RTS_DEBUG)
	virtual void audioDebugDisplay(DebugDisplayInterface *dd, void *, FILE *fp = nullptr) override;
	virtual AudioHandle addAudioEvent(const AudioEventRTS *eventToAdd) override;
#endif

	virtual void init() override;
	virtual void postProcessLoad() override;
	virtual void reset() override;
	virtual void update() override;

	OpenALAudioManager();
	virtual ~OpenALAudioManager() override;

	virtual AsciiString nextMusicTrack() override;
	virtual AsciiString prevMusicTrack() override;
	virtual Bool isMusicPlaying() const override;
	virtual Bool hasMusicTrackCompleted(const AsciiString &trackName, Int numberOfTimes) const override;

	virtual void openDevice() override;
	virtual void closeDevice() override;
	virtual void *getDevice() override { return m_device; }

	virtual void stopAudio(AudioAffect which) override;
	virtual void pauseAudio(AudioAffect which) override;
	virtual void resumeAudio(AudioAffect which) override;
	virtual void pauseAmbient(Bool shouldPause) override;

	virtual void killAudioEventImmediately(AudioHandle audioEvent) override;

	///< NOTE: DO NOT USE THIS FOR GAMELOGIC PURPOSES
	virtual Bool isCurrentlyPlaying(AudioHandle handle) override;

	/// Miles called this from its timer thread. Here update() finds finished sources and calls
	/// onAudioCompleted directly, so this entry point is unused.
	virtual void notifyOfAudioCompletion(UnsignedInt handle, UnsignedInt flags) override {}

	virtual UnsignedInt getProviderCount() const override;
	virtual AsciiString getProviderName(UnsignedInt providerNum) const override;
	virtual UnsignedInt getProviderIndex(AsciiString providerName) const override;
	virtual void selectProvider(UnsignedInt providerNdx) override;
	virtual void unselectProvider() override;
	virtual UnsignedInt getSelectedProvider() const override;
	virtual void setSpeakerType(UnsignedInt speakerType) override;
	virtual UnsignedInt getSpeakerType() override;

	/// Returns the video audio stream (an OpenALAudioStream*), created on first use. Never null.
	virtual void *getHandleForBink() override;
	virtual void releaseHandleForBink() override;

	virtual void friend_forcePlayAudioEventRTS(const AudioEventRTS *eventToPlay) override;

	virtual UnsignedInt getNum2DSamples() const override;
	virtual UnsignedInt getNum3DSamples() const override;
	virtual UnsignedInt getNumStreams() const override;
	virtual UnsignedInt getNumAvailable2DSamples() const override;
	virtual UnsignedInt getNumAvailable3DSamples() const override;

	virtual Bool doesViolateLimit(AudioEventRTS *event) const override;
	virtual Bool isPlayingLowerPriority(AudioEventRTS *event) const override;
	virtual Bool isPlayingAlready(AudioEventRTS *event) const override;
	virtual Bool isObjectPlayingVoice(UnsignedInt objID) const override;

	virtual void adjustVolumeOfPlayingAudio(AsciiString eventName, Real newVolume) override;
	virtual void removePlayingAudio(AsciiString eventName) override;
	virtual void removeAllDisabledAudio() override;

	virtual void processRequestList() override;

	virtual void setSpeakerSurround(Bool surround) override;

	virtual void setPreferredProvider(AsciiString provider) override { m_pref3DProvider = provider; }
	virtual void setPreferredSpeaker(AsciiString speakerType) override { m_prefSpeaker = speakerType; }

	/// Reads only the file header, so it works without a device. Scripts use it, so it affects CRC.
	virtual Real getFileLengthMS(AsciiString strToLoad) const override;

	virtual void closeAnySamplesUsingFile(const void *fileToClose) override;

protected:
	enum PlayingAudioType
	{
		PAT_Sample,
		PAT_3DSample,
		PAT_Stream,
		PAT_INVALID
	};

	enum PlayingStatus
	{
		PS_Playing,
		PS_Stopping, ///< Is about to be stopped
		PS_Stopped, ///< Is about to be released
	};

	struct StreamDecoder; ///< Music (mp3) or speech (wav) decoded a piece at a time; defined in the .cpp

	struct PlayingAudio
	{
		ALuint m_source;
		RefCountPtr<DynamicAudioEventRTS> m_audioEventRTS;
		void *m_file; ///< The cache entry that was opened to play this
		StreamDecoder *m_decoder; ///< Streams only
		PlayingAudioType m_type;
		PlayingStatus m_status;
		Int m_loopsDone; ///< Streams only: how often the music track played to its end
		Short m_framesFaded;
		Bool m_fade;
		Bool m_rerequestOnNextUpdate;
		Bool m_requestStop; ///< Let the audio finish but stop looping if it is looping

		PlayingAudio();
		Bool isPlaying() const { return m_status == PS_Playing; }
		Bool isPlayingOrRequested() const { return m_status == PS_Playing || m_rerequestOnNextUpdate; }
	};

	/// One decoded sample file in an AL buffer. Same rules as Miles' AudioFileCache: a size limit,
	/// ref counts, and unused files are freed first.
	struct OpenAudioFile
	{
		ALuint m_buffer;
		UnsignedInt m_openCount;
		UnsignedInt m_fileSize;
		const AudioEventInfo *m_eventInfo; ///< Not owned
	};

	class AudioFileCache
	{
	public:
		AudioFileCache() : m_currentlyUsedSize(0), m_maxSize(0) {}
		~AudioFileCache();
		void *openFile(AudioEventRTS *eventToOpenFrom);
		void closeFile(void *fileToClose);
		void setMaxSize(UnsignedInt size) { m_maxSize = size; }
		UnsignedInt getCurrentlyUsedSize() const { return m_currentlyUsedSize; }
		UnsignedInt getMaxSize() const { return m_maxSize; }

	protected:
		void releaseOpenAudioFile(OpenAudioFile *fileToRelease);
		Bool freeEnoughSpaceForSample(const OpenAudioFile &sampleThatNeedsSpace);

		// Entries live on the heap so their address can be the void* file handle.
		std::map<AsciiString, OpenAudioFile *> m_openFiles;
		UnsignedInt m_currentlyUsedSize;
		UnsignedInt m_maxSize;
	};

	/// A briefing line played with friend_forcePlayAudioEventRTS, outside the cache.
	struct ForcePlayed
	{
		ALuint m_source;
		ALuint m_buffer;
	};

	virtual void setDeviceListenerPosition() override;
	const Coord3D *getCurrentPositionFromEvent(AudioEventRTS *event);
	Bool isOnScreen(const Coord3D *pos) const;
	Real getEffectiveVolume(AudioEventRTS *event) const;

	Bool startNextLoop(PlayingAudio *playing);
	void onAudioCompleted(PlayingAudio *playing);
	void pollCompletion(std::list<PlayingAudio *> &list);
	void pumpStream(PlayingAudio *playing);

	void playStream(AudioEventRTS *event, PlayingAudio *playing);
	void *playSample(AudioEventRTS *event, PlayingAudio *playing);
	void *playSample3D(AudioEventRTS *event, PlayingAudio *playing);

	Bool isValidProvider() const;
	void initSamplePools();
	void processRequest(AudioRequest *req);
	void processPlayingList();
	void processFadingList();
	Bool shouldProcessRequestThisFrame(AudioRequest *req) const;
	void adjustRequest(AudioRequest *req);
	Bool checkForSample(AudioRequest *req);

	void playAudioEvent(AudioRequest *req);
	void stopAudioEvent(AudioHandle handle);

	void *loadFileForRead(AudioEventRTS *eventToLoadFrom);
	void closeFile(void *fileRead);

	void releaseSource(PlayingAudio *playing);
	void releasePlayingAudio(PlayingAudio *playing);
	void stopPlayingAudio(PlayingAudio *playing);
	void rerequestPlayingAudio(PlayingAudio *playing);
	void rerequestPlayingAudioWhenSignalled(PlayingAudio *playing);

	PlayingAudio *findActiveMusic(const AsciiString *trackName = nullptr);
	const PlayingAudio *findActiveMusic(const AsciiString *trackName = nullptr) const;

	void releasePlayingAudioInListIfStopped(std::list<PlayingAudio *> &list);
	void stopAllAudioImmediately();
	void freeAllSources();

	ALuint getAvailable2DSample();
	ALuint getAvailable3DSample();
	Bool killLowestPrioritySoundImmediately(AudioEventRTS *event);
	AudioEventRTS *findLowestPrioritySound(AudioEventRTS *event);

	void stopAllSpeech();

	void initFilters(ALuint source, AudioEventRTS *event);
	void initFilters3D(ALuint source, AudioEventRTS *event, const Coord3D *pos);

protected:
	UnsignedInt m_selectedProvider;
	UnsignedInt m_selectedSpeakerType;

	AsciiString m_pref3DProvider;
	AsciiString m_prefSpeaker;

	ALCdevice *m_device;
	ALCcontext *m_context;
	ALuint m_lowPassFilter; ///< 0 when the device has no EFX

	std::list<ForcePlayed> m_audioForcePlayed;

	// Free sources for 2-D and 3-D sounds. Streams make their own source when they start.
	std::vector<ALuint> m_availableSamples;
	std::vector<ALuint> m_available3DSamples;

	std::list<PlayingAudio *> m_playingSounds;
	std::list<PlayingAudio *> m_playing3DSounds;
	std::list<PlayingAudio *> m_playingStreams;
	std::list<PlayingAudio *> m_fadingAudio; ///< Music that is fading out, released when done

	AudioFileCache *m_audioCache;
	OpenALAudioStream *m_binkStream;
	UnsignedInt m_num2DSamples;
	UnsignedInt m_num3DSamples;
	UnsignedInt m_numStreams;

#if defined(RTS_DEBUG)
	std::set<AsciiString> m_allEventsLoaded;
	void dumpAllAssetsUsed();
#endif
};

// AudioManager for headless mode. It opens no device, so nothing plays, but getFileLengthMS still
// reads the real file lengths that scripts (and so the CRC) depend on.
class OpenALAudioManagerDummy : public OpenALAudioManager
{
#if defined(RTS_DEBUG)
	virtual void audioDebugDisplay(DebugDisplayInterface *dd, void *userData, FILE *fp) override {}
#endif
	virtual void openDevice() override {}
	virtual void stopAudio(AudioAffect which) override {}
	virtual void pauseAudio(AudioAffect which) override {}
	virtual void resumeAudio(AudioAffect which) override {}
	virtual void pauseAmbient(Bool shouldPause) override {}
	virtual void killAudioEventImmediately(AudioHandle audioEvent) override {}
	virtual AsciiString nextMusicTrack() override { return AsciiString::TheEmptyString; }
	virtual AsciiString prevMusicTrack() override { return AsciiString::TheEmptyString; }
	virtual Bool isMusicPlaying() const override { return false; }
	virtual Bool hasMusicTrackCompleted(const AsciiString &trackName, Int numberOfTimes) const override { return false; }
	virtual UnsignedInt getProviderCount() const override { return 0; }
	virtual AsciiString getProviderName(UnsignedInt providerNum) const override { return AsciiString::TheEmptyString; }
	virtual UnsignedInt getProviderIndex(AsciiString providerName) const override { return 0; }
	virtual void selectProvider(UnsignedInt providerNdx) override {}
	virtual void unselectProvider() override {}
	virtual UnsignedInt getSelectedProvider() const override { return 0; }
	virtual void setSpeakerType(UnsignedInt speakerType) override {}
	virtual UnsignedInt getSpeakerType() override { return 0; }
	virtual UnsignedInt getNum2DSamples() const override { return 0; }
	virtual UnsignedInt getNum3DSamples() const override { return 0; }
	virtual UnsignedInt getNumStreams() const override { return 0; }
	virtual UnsignedInt getNumAvailable2DSamples() const override { return 0; }
	virtual UnsignedInt getNumAvailable3DSamples() const override { return 0; }
	virtual Bool doesViolateLimit(AudioEventRTS *event) const override { return false; }
	virtual Bool isPlayingLowerPriority(AudioEventRTS *event) const override { return false; }
	virtual Bool isPlayingAlready(AudioEventRTS *event) const override { return false; }
	virtual Bool isObjectPlayingVoice(UnsignedInt objID) const override { return false; }
	virtual void adjustVolumeOfPlayingAudio(AsciiString eventName, Real newVolume) override {}
	virtual void removePlayingAudio(AsciiString eventName) override {}
	virtual void removeAllDisabledAudio() override {}
	virtual void friend_forcePlayAudioEventRTS(const AudioEventRTS *eventToPlay) override {}
	virtual void setPreferredProvider(AsciiString providerNdx) override {}
	virtual void setPreferredSpeaker(AsciiString speakerType) override {}
	virtual void closeAnySamplesUsingFile(const void *fileToClose) override {}
	virtual void setDeviceListenerPosition() override {}
};
