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

// FILE: OpenALAudioStream.h //////////////////////////////////////////////////////////////////////
// EastWind: one OpenAL source fed by a queue of buffers. The FFmpeg video player pushes decoded
// audio into it; the names and calls are the ones upstream's FFmpegVideoPlayer.cpp expects.

#pragma once

#include <AL/al.h>
#include <stdint.h>

class OpenALAudioStream
{
public:
	OpenALAudioStream();
	~OpenALAudioStream();

	void bufferData(uint8_t *data, size_t size, ALenum format, int sampleRate); ///< Queues one more buffer
	void update();	///< Frees the buffers that finished playing
	void play();		///< Starts the source if it has buffers and is not playing
	void reset();		///< Stops and drops all queued buffers
	void setVolume(float volume);

	int getQueuedCount() const;
	bool isPlaying() const;
	ALuint getSource() const { return m_source; }

private:
	ALuint m_source;
};
