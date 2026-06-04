/*
Copyright (c) 2012-2020 Maarten Baert <maarten-baert@hotmail.com>

This file is part of SimpleScreenRecorder.

SimpleScreenRecorder is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

SimpleScreenRecorder is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with SimpleScreenRecorder.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once
#include "Global.h"

#include "SourceSink.h"
#include "FastScaler.h"
#include "MutexDataPair.h"
#include "TempBuffer.h"

#if SSR_USE_V4L2

#include <linux/videodev2.h>

class V4L2Output : public VideoSink {

private:
	struct FrameData {
		std::shared_ptr<TempBuffer<uint8_t>> buffer;
		size_t length;
	};
	struct SharedData {
		std::deque<FrameData> queue;
		bool should_stop;
		bool error_occurred;
	};
	typedef MutexDataPair<SharedData>::Lock SharedLock;

	struct Buffer {
		void* data;
		size_t length;
	};

	QString m_device;
	unsigned int m_width;
	unsigned int m_height;
	unsigned int m_frame_rate;

	int m_fd;
	std::vector<Buffer> m_buffers;
	unsigned int m_frame_size;

	FastScaler m_fast_scaler;
	MutexDataPair<SharedData> m_shared_data;
	std::thread m_thread;

public:
	V4L2Output(const QString& device, unsigned int width, unsigned int height, unsigned int frame_rate);
	~V4L2Output();

	virtual int64_t GetNextVideoTimestamp() override;
	virtual void ReadVideoFrame(unsigned int width, unsigned int height, const uint8_t* const* data, const int* stride, AVPixelFormat format, int colorspace, int64_t timestamp) override;

private:
	void WriterThread();
	bool InitDevice();
	void FreeDevice();

};

#endif
