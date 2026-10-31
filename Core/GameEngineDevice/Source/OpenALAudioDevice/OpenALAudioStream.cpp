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

// FILE: OpenALAudioStream.cpp ////////////////////////////////////////////////////////////////////

#include "OpenALAudioDevice/OpenALAudioStream.h"

//-------------------------------------------------------------------------------------------------
OpenALAudioStream::OpenALAudioStream() : m_source(0)
{
	// Without a current context (headless) this leaves m_source at 0, and every call below is a no-op.
	alGenSources(1, &m_source);
	alSourcei(m_source, AL_SOURCE_RELATIVE, AL_TRUE);
	alSource3f(m_source, AL_POSITION, 0.0f, 0.0f, 0.0f);
}

//-------------------------------------------------------------------------------------------------
OpenALAudioStream::~OpenALAudioStream()
{
	reset();
	if (m_source)
		alDeleteSources(1, &m_source);
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioStream::bufferData(uint8_t *data, size_t size, ALenum format, int sampleRate)
{
	if (!m_source || format == AL_NONE)
		return;

	ALuint buffer = 0;
	alGenBuffers(1, &buffer);
	alBufferData(buffer, format, data, (ALsizei)size, sampleRate);
	alSourceQueueBuffers(m_source, 1, &buffer);

	// The load screen's movie loop never calls play(), so start (or restart after an underrun) here.
	play();
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioStream::update()
{
	if (!m_source)
		return;

	ALint processed = 0;
	alGetSourcei(m_source, AL_BUFFERS_PROCESSED, &processed);
	while (processed-- > 0) {
		ALuint buffer = 0;
		alSourceUnqueueBuffers(m_source, 1, &buffer);
		alDeleteBuffers(1, &buffer);
	}
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioStream::play()
{
	if (!m_source || isPlaying() || getQueuedCount() == 0)
		return;

	alSourcePlay(m_source);
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioStream::reset()
{
	if (!m_source)
		return;

	alSourceStop(m_source);
	update(); // after a stop every queued buffer counts as processed
	alSourcei(m_source, AL_BUFFER, 0);
}

//-------------------------------------------------------------------------------------------------
void OpenALAudioStream::setVolume(float volume)
{
	if (m_source)
		alSourcef(m_source, AL_GAIN, volume);
}

//-------------------------------------------------------------------------------------------------
int OpenALAudioStream::getQueuedCount() const
{
	ALint queued = 0;
	if (m_source)
		alGetSourcei(m_source, AL_BUFFERS_QUEUED, &queued);
	return queued;
}

//-------------------------------------------------------------------------------------------------
bool OpenALAudioStream::isPlaying() const
{
	ALint state = AL_STOPPED;
	if (m_source)
		alGetSourcei(m_source, AL_SOURCE_STATE, &state);
	return state == AL_PLAYING;
}
