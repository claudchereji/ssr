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

// Browser-friendly resolutions for virtual cameras
static const unsigned int V4L2_OUTPUT_MAX_WIDTH = 1280;
static const unsigned int V4L2_OUTPUT_MAX_HEIGHT = 720;

V4L2Output::V4L2Output(const QString& device, unsigned int width, unsigned int height, unsigned int frame_rate)
	: m_device(device), m_width(width), m_height(height), m_frame_rate(frame_rate), m_fd(-1), m_frame_size(0) {

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

	// The output device is always exactly 1280x720 (akvcam forces this from config)
	const unsigned int out_width = V4L2_OUTPUT_MAX_WIDTH;
	const unsigned int out_height = V4L2_OUTPUT_MAX_HEIGHT;
	const size_t out_frame_size = out_width * out_height * 3;

	// Calculate aspect-ratio-preserving dimensions that fit inside 1280x720
	double scale_x = (double) out_width / (double) width;
	double scale_y = (double) out_height / (double) height;
	double scale = std::min(scale_x, scale_y);
	unsigned int scaled_w = (unsigned int) (width * scale);
	unsigned int scaled_h = (unsigned int) (height * scale);
	// Ensure even dimensions for safety
	scaled_w = scaled_w / 2 * 2;
	scaled_h = scaled_h / 2 * 2;

	// Create the final 1280x720 buffer
	std::shared_ptr<TempBuffer<uint8_t>> buffer = std::make_shared<TempBuffer<uint8_t>>();
	buffer->Alloc(out_frame_size);
	uint8_t* out_data = buffer->GetData();

	if(scaled_w == out_width && scaled_h == out_height) {
		// Exact fit, direct scale
		int out_stride = out_width * 3;
		m_fast_scaler.Scale(width, height, format, colorspace, data, stride,
							out_width, out_height, AV_PIX_FMT_RGB24, SWS_CS_DEFAULT,
							&out_data, &out_stride);
	} else {
		// Scale to temp buffer, then copy centered with black bars
		TempBuffer<uint8_t> scaled_buffer;
		scaled_buffer.Alloc(scaled_w * scaled_h * 3);
		uint8_t* scaled_data = scaled_buffer.GetData();
		int scaled_stride = scaled_w * 3;

		m_fast_scaler.Scale(width, height, format, colorspace, data, stride,
							scaled_w, scaled_h, AV_PIX_FMT_RGB24, SWS_CS_DEFAULT,
							&scaled_data, &scaled_stride);

		// Clear to black
		memset(out_data, 0, out_frame_size);

		// Copy centered
		unsigned int offset_x = (out_width - scaled_w) / 2;
		unsigned int offset_y = (out_height - scaled_h) / 2;
		for(unsigned int y = 0; y < scaled_h; ++y) {
			memcpy(out_data + (offset_y + y) * out_width * 3 + offset_x * 3,
				   scaled_data + y * scaled_stride,
				   scaled_w * 3);
		}
	}

	SharedLock lock(&m_shared_data);
	if(lock->error_occurred)
		return;

	// Limit queue size
	if(lock->queue.size() >= 3) {
		lock->queue.pop_front();
	}

	FrameData fd;
	fd.buffer = buffer;
	fd.length = out_frame_size;
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

			// Dequeue a buffer from the driver
			struct v4l2_buffer buf;
			memset(&buf, 0, sizeof(buf));
			buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
			buf.memory = V4L2_MEMORY_MMAP;
			if(::ioctl(m_fd, VIDIOC_DQBUF, &buf) < 0) {
				if(errno == EAGAIN) {
					continue;
				}
				Logger::LogError("[V4L2Output::WriterThread] " + Logger::tr("Failed to dequeue buffer: %1").arg(strerror(errno)));
				continue;
			}

			// Copy frame data into the mmap'd buffer
			size_t copy_len = std::min(fd.length, m_buffers[buf.index].length);
			memcpy(m_buffers[buf.index].data, fd.buffer->GetData(), copy_len);
			buf.bytesused = copy_len;

			// Queue the buffer back
			if(::ioctl(m_fd, VIDIOC_QBUF, &buf) < 0) {
				Logger::LogError("[V4L2Output::WriterThread] " + Logger::tr("Failed to queue buffer: %1").arg(strerror(errno)));
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
	// Force exact 1280x720 because akvcam's config only supports this resolution
	if(m_width > V4L2_OUTPUT_MAX_WIDTH || m_height > V4L2_OUTPUT_MAX_HEIGHT ||
	   m_width != V4L2_OUTPUT_MAX_WIDTH || m_height != V4L2_OUTPUT_MAX_HEIGHT) {
		Logger::LogInfo("[V4L2Output::InitDevice] " + Logger::tr("Forcing resolution to %1x%2 for akvcam/browser compatibility.")
						   .arg(V4L2_OUTPUT_MAX_WIDTH).arg(V4L2_OUTPUT_MAX_HEIGHT));
		m_width = V4L2_OUTPUT_MAX_WIDTH;
		m_height = V4L2_OUTPUT_MAX_HEIGHT;
	}

	m_fd = ::open(m_device.toUtf8().constData(), O_RDWR | O_NONBLOCK, 0);
	if(m_fd < 0) {
		Logger::LogError("[V4L2Output::InitDevice] " + Logger::tr("Failed to open V4L2 device %1: %2").arg(m_device).arg(strerror(errno)));
		return false;
	}

	struct v4l2_capability cap;
	memset(&cap, 0, sizeof(cap));
	if(::ioctl(m_fd, VIDIOC_QUERYCAP, &cap) < 0) {
		Logger::LogError("[V4L2Output::InitDevice] " + Logger::tr("Failed to query capabilities: %1").arg(strerror(errno)));
		::close(m_fd);
		m_fd = -1;
		return false;
	}

	// Set output format to RGB24
	struct v4l2_format fmt;
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width = m_width;
	fmt.fmt.pix.height = m_height;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	if(::ioctl(m_fd, VIDIOC_S_FMT, &fmt) < 0) {
		Logger::LogError("[V4L2Output::InitDevice] " + Logger::tr("Failed to set format: %1").arg(strerror(errno)));
		::close(m_fd);
		m_fd = -1;
		return false;
	}

	// Update dimensions if driver adjusted them
	if(fmt.fmt.pix.width != m_width || fmt.fmt.pix.height != m_height) {
		Logger::LogWarning("[V4L2Output::InitDevice] " + Logger::tr("Resolution adjusted from %1x%2 to %3x%4.")
						   .arg(m_width).arg(m_height).arg(fmt.fmt.pix.width).arg(fmt.fmt.pix.height));
		m_width = fmt.fmt.pix.width;
		m_height = fmt.fmt.pix.height;
	}

	if(fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB24) {
		Logger::LogError("[V4L2Output::InitDevice] " + Logger::tr("Device does not support RGB24 format."));
		::close(m_fd);
		m_fd = -1;
		return false;
	}

	m_frame_size = m_width * m_height * 3;

	// Request buffers for mmap streaming
	struct v4l2_requestbuffers req;
	memset(&req, 0, sizeof(req));
	req.count = 4;
	req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	req.memory = V4L2_MEMORY_MMAP;
	if(::ioctl(m_fd, VIDIOC_REQBUFS, &req) < 0) {
		Logger::LogError("[V4L2Output::InitDevice] " + Logger::tr("Failed to request buffers: %1").arg(strerror(errno)));
		::close(m_fd);
		m_fd = -1;
		return false;
	}

	m_buffers.resize(req.count);
	for(unsigned int i = 0; i < req.count; ++i) {
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if(::ioctl(m_fd, VIDIOC_QUERYBUF, &buf) < 0) {
			Logger::LogError("[V4L2Output::InitDevice] " + Logger::tr("Failed to query buffer: %1").arg(strerror(errno)));
			FreeDevice();
			return false;
		}
		m_buffers[i].length = buf.length;
		m_buffers[i].data = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, buf.m.offset);
		if(m_buffers[i].data == MAP_FAILED) {
			Logger::LogError("[V4L2Output::InitDevice] " + Logger::tr("Failed to mmap buffer."));
			FreeDevice();
			return false;
		}
	}

	// Queue all buffers
	for(unsigned int i = 0; i < req.count; ++i) {
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if(::ioctl(m_fd, VIDIOC_QBUF, &buf) < 0) {
			Logger::LogError("[V4L2Output::InitDevice] " + Logger::tr("Failed to queue initial buffer: %1").arg(strerror(errno)));
			FreeDevice();
			return false;
		}
	}

	// Start streaming
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if(::ioctl(m_fd, VIDIOC_STREAMON, &type) < 0) {
		Logger::LogError("[V4L2Output::InitDevice] " + Logger::tr("Failed to start streaming: %1").arg(strerror(errno)));
		FreeDevice();
		return false;
	}

	Logger::LogInfo("[V4L2Output::InitDevice] " + Logger::tr("Virtual camera output initialized: %1x%2 RGB24 @ %3 fps, frame size=%4 bytes")
					.arg(m_width).arg(m_height).arg(m_frame_rate).arg(m_frame_size));

	return true;
}

void V4L2Output::FreeDevice() {
	if(m_fd >= 0) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		::ioctl(m_fd, VIDIOC_STREAMOFF, &type);

		for(size_t i = 0; i < m_buffers.size(); ++i) {
			if(m_buffers[i].data != NULL && m_buffers[i].data != MAP_FAILED)
				munmap(m_buffers[i].data, m_buffers[i].length);
		}
		m_buffers.clear();

		::close(m_fd);
		m_fd = -1;
	}
}

#endif
