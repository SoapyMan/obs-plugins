/*
Copyright (C) 2019-present, Facebook, Inc.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#pragma once

#include <stdint.h>
#include <memory>
#include <vector>
#include <list>
#include <mutex>
#include <cassert>

typedef struct {
	int MajorVersion;
	int MinorVersion;
	int PatchVersion;
} ovrmVersion;

namespace ovrmConstants {
const static uint32_t Magic = 0x2877AF94;
}

enum class PayloadType : uint32_t {
	VIDEO_DIMENSION = 10,
	VIDEO_DATA = 11,
	AUDIO_SAMPLERATE = 12,
	AUDIO_DATA = 13,
	CAPTURE_CONTROL_DATA = 14,
	DATA_VERSION = 15,
};

struct ovrmPayloadHeader {
	uint32_t Magic;
	uint32_t TotalDataLengthExcludingMagic;
	uint32_t PayloadType;
	uint32_t PayloadLength;
};

struct Frame {
	PayloadType m_type;
	double m_secondsSinceEpoch;
	std::vector<uint8_t> m_payload;
};

struct AudioDataHeader {
	uint64_t timestamp;
	int channels;
	int dataLength;
};

struct FrameDimension {
	int w;
	int h;
};

//typedef std::vector<uint8_t> Frame;

class FrameCollection {
public:
	FrameCollection();
	~FrameCollection();

	void Reset();

	void AddData(const uint8_t *data, uint32_t len);

	bool HasCompletedFrame();

	std::shared_ptr<Frame> PopFrame();

	bool HasError() const { return m_hasError; }

	bool HasFirstFrame() const
	{
		return m_firstFrameTimeSet && !m_hasError;
	}

	std::chrono::time_point<std::chrono::system_clock>
	GetFirstFrameTime() const
	{
		if (m_firstFrameTimeSet) {
			return m_firstFrameTime;
		} else {
			return std::chrono::system_clock::now();
		}
	}

private:
	bool m_firstFrameTimeSet = false;
	std::chrono::time_point<std::chrono::system_clock> m_firstFrameTime;

	std::vector<uint8_t> m_scratchPad;
	std::list<std::shared_ptr<Frame>> m_frames;

	std::mutex m_frameMutex;

	bool m_hasError;
};
