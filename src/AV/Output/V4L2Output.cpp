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

#include "V4L2Output.h"

#if SSR_USE_V4L2

#include "Logger.h"

V4L2Output::V4L2Output(const QString& device, unsigned int width, unsigned int height, unsigned int frame_rate)
	: m_device(device), m_width(width), m_height(height), m_frame_rate(frame_rate), m_fd(-1) {

	{
		SharedLock lock(&m_shared_data);
		lock->should_stop = false;
		lock->error_occurred = false;
	}

	try {
		if(!InitDevice()) {
			Logger::LogError("[V4L2Output::V4L2Output] " + Logger::tr("Failed to initialize V4L2 output device."));
			throw V4L2Exception();
		}
		m_thread = std::thread(&V4L2Output::WriterThread, this);
	} catch(...) {
		FreeDevice();
		throw;
	}

}

V4L2Output::~V4L2Output() {
	ConnectVideoSource(NULL);
	{
		SharedLock lock(&m_shared_data);
		lock->should_stop = true;
	}
	if(m_thread.joinable()) {
		m_thread.join();
	}
	FreeDevice();
}

int64_t V4L2Output::GetNextVideoTimestamp() {
	return SINK_TIMESTAMP_ASAP;
}

void V4L2Output::ReadVideoFrame(unsigned int width, unsigned int height, const uint8_t* const* data, const int* stride, AVPixelFormat format, int colorspace, int64_t timestamp) {
	Q_UNUSED(timestamp);

	// Convert frame to RGB24 at target resolution
	size_t frame_size = m_width * m_height * 3;
	std::shared_ptr<TempBuffer<uint8_t>> buffer = std::make_shared<TempBuffer<uint8_t>>();
	buffer->Alloc(frame_size);

	uint8_t* out_data = buffer->GetData();
	int out_stride = m_width * 3;

	m_fast_scaler.Scale(width, height, format, colorspace, data, stride,
						m_width, m_height, AV_PIX_FMT_RGB24, SWS_CS_DEFAULT,
						&out_data, &out_stride);

	SharedLock lock(&m_shared_data);
	if(lock->error_occurred)
		return;

	// Limit queue size
	if(lock->queue.size() >= 3) {
		lock->queue.pop_front();
	}

	FrameData fd;
	fd.buffer = buffer;
	fd.length = frame_size;
	lock->queue.push_back(fd);
}

void V4L2Output::WriterThread() {
	try {

		Logger::LogInfo("[V4L2Output::WriterThread] " + Logger::tr("Virtual camera output thread started."));

		while(true) {
			FrameData fd;
			{
				SharedLock lock(&m_shared_data);
				if(lock->should_stop)
					break;
				if(lock->queue.empty()) {
					lock.lock().unlock();
					usleep(5000);
					continue;
				}
				fd = lock->queue.front();
				lock->queue.pop_front();
			}

			// Write to V4L2 device
			struct v4l2_buffer buf;
			memset(&buf, 0, sizeof(buf));
			buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
			buf.memory = V4L2_MEMORY_MMAP;

			if(::ioctl(m_fd, VIDIOC_DQBUF, &buf) < 0) {
				if(errno == EAGAIN) {
					// No buffer available yet, skip frame
					continue;
				}
				Logger::LogError("[V4L2Output::WriterThread] " + Logger::tr("Failed to dequeue buffer."));
				continue;
			}

			size_t copy_len = std::min(fd.length, m_buffer_lengths[buf.index]);
			memcpy(m_buffer_data[buf.index], fd.buffer->GetData(), copy_len);

			if(::ioctl(m_fd, VIDIOC_QBUF, &buf) < 0) {
				Logger::LogError("[V4L2Output::WriterThread] " + Logger::tr("Failed to queue buffer."));
			}
		}

		Logger::LogInfo("[V4L2Output::WriterThread] " + Logger::tr("Virtual camera output thread stopped."));

	} catch(const std::exception& e) {
		SharedLock lock(&m_shared_data);
		lock->error_occurred = true;
		Logger::LogError("[V4L2Output::WriterThread] " + Logger::tr("Exception '%1' in virtual camera output thread.").arg(e.what()));
	} catch(...) {
		SharedLock lock(&m_shared_data);
		lock->error_occurred = true;
		Logger::LogError("[V4L2Output::WriterThread] " + Logger::tr("Unknown exception in virtual camera output thread."));
	}
}

bool V4L2Output::InitDevice() {
	m_fd = ::open(m_device.toUtf8().constData(), O_RDWR | O_NONBLOCK, 0);
	if(m_fd < 0) {
		Logger::LogError("[V4L2Output::InitDevice] " + Logger::tr("Failed to open V4L2 device %1.").arg(m_device));
		return false;
	}

	struct v4l2_capability cap;
	memset(&cap, 0, sizeof(cap));
	if(::ioctl(m_fd, VIDIOC_QUERYCAP, &cap) < 0) {
		Logger::LogError("[V4L2Output::InitDevice] " + Logger::tr("Failed to query capabilities."));
		::close(m_fd);
		m_fd = -1;
		return false;
	}

	struct v4l2_format fmt;
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width = m_width;
	fmt.fmt.pix.height = m_height;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	if(::ioctl(m_fd, VIDIOC_S_FMT, &fmt) < 0) {
		Logger::LogError("[V4L2Output::InitDevice] " + Logger::tr("Failed to set format."));
		::close(m_fd);
		m_fd = -1;
		return false;
	}

	// Update width/height in case driver changed them
	if(fmt.fmt.pix.width != m_width || fmt.fmt.pix.height != m_height) {
		Logger::LogWarning("[V4L2Output::InitDevice] " + Logger::tr("Resolution adjusted to %1x%2.")
						   .arg(fmt.fmt.pix.width).arg(fmt.fmt.pix.height));
		m_width = fmt.fmt.pix.width;
		m_height = fmt.fmt.pix.height;
	}

	struct v4l2_requestbuffers req;
	memset(&req, 0, sizeof(req));
	req.count = 4;
	req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	req.memory = V4L2_MEMORY_MMAP;
	if(::ioctl(m_fd, VIDIOC_REQBUFS, &req) < 0) {
		Logger::LogError("[V4L2Output::InitDevice] " + Logger::tr("Failed to request buffers."));
		::close(m_fd);
		m_fd = -1;
		return false;
	}

	m_buffer_data.resize(req.count);
	m_buffer_lengths.resize(req.count);

	for(unsigned int i = 0; i < req.count; ++i) {
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if(::ioctl(m_fd, VIDIOC_QUERYBUF, &buf) < 0) {
			Logger::LogError("[V4L2Output::InitDevice] " + Logger::tr("Failed to query buffer."));
			FreeDevice();
			return false;
		}
		m_buffer_data[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, buf.m.offset);
		m_buffer_lengths[i] = buf.length;
		if(m_buffer_data[i] == MAP_FAILED) {
			Logger::LogError("[V4L2Output::InitDevice] " + Logger::tr("Failed to mmap buffer."));
			FreeDevice();
			return false;
		}
	}

	for(unsigned int i = 0; i < req.count; ++i) {
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if(::ioctl(m_fd, VIDIOC_QBUF, &buf) < 0) {
			Logger::LogError("[V4L2Output::InitDevice] " + Logger::tr("Failed to queue initial buffer."));
			FreeDevice();
			return false;
		}
	}

	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if(::ioctl(m_fd, VIDIOC_STREAMON, &type) < 0) {
		Logger::LogError("[V4L2Output::InitDevice] " + Logger::tr("Failed to start streaming."));
		FreeDevice();
		return false;
	}

	return true;
}

void V4L2Output::FreeDevice() {
	if(m_fd >= 0) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		::ioctl(m_fd, VIDIOC_STREAMOFF, &type);

		for(size_t i = 0; i < m_buffer_data.size(); ++i) {
			if(m_buffer_data[i] != NULL && m_buffer_data[i] != MAP_FAILED)
				munmap(m_buffer_data[i], m_buffer_lengths[i]);
		}
		m_buffer_data.clear();
		m_buffer_lengths.clear();

		::close(m_fd);
		m_fd = -1;
	}
}

#endif
