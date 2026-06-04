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

	// Convert frame to YUY2 at target resolution
	size_t frame_size = m_width * m_height * 2;
	std::shared_ptr<TempBuffer<uint8_t>> buffer = std::make_shared<TempBuffer<uint8_t>>();
	buffer->Alloc(frame_size);

	uint8_t* out_data = buffer->GetData();
	int out_stride = m_width * 2;

	m_fast_scaler.Scale(width, height, format, colorspace, data, stride,
						m_width, m_height, AV_PIX_FMT_YUYV422, SWS_CS_DEFAULT,
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

		int64_t frame_interval = 1000000 / m_frame_rate;
		int64_t next_frame_time = hrt_time_micro();

		while(true) {
			FrameData fd;
			{
				SharedLock lock(&m_shared_data);
				if(lock->should_stop)
					break;
				if(lock->queue.empty()) {
					lock.lock().unlock();
					usleep(1000);
					continue;
				}
				fd = lock->queue.front();
				lock->queue.pop_front();
			}

			// Write frame to V4L2 device using write()
			ssize_t written = ::write(m_fd, fd.buffer->GetData(), fd.length);
			if(written < 0) {
				if(errno == EAGAIN || errno == EWOULDBLOCK)
					continue;
				Logger::LogError("[V4L2Output::WriterThread] " + Logger::tr("Failed to write frame: %1").arg(strerror(errno)));
				continue;
			}

			// Frame rate control
			next_frame_time += frame_interval;
			int64_t sleep_time = next_frame_time - hrt_time_micro();
			if(sleep_time > 0) {
				usleep(sleep_time);
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
	m_fd = ::open(m_device.toUtf8().constData(), O_WRONLY);
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

	// Cap resolution to browser-friendly maximum, preserving aspect ratio
	if(m_width > V4L2_OUTPUT_MAX_WIDTH || m_height > V4L2_OUTPUT_MAX_HEIGHT) {
		double scale_x = (double) V4L2_OUTPUT_MAX_WIDTH / (double) m_width;
		double scale_y = (double) V4L2_OUTPUT_MAX_HEIGHT / (double) m_height;
		double scale = std::min(scale_x, scale_y);
		unsigned int new_width = (unsigned int) (m_width * scale);
		unsigned int new_height = (unsigned int) (m_height * scale);
		// Ensure even dimensions for YUY2
		new_width = new_width / 2 * 2;
		new_height = new_height / 2 * 2;
		Logger::LogInfo("[V4L2Output::InitDevice] " + Logger::tr("Capping resolution from %1x%2 to %3x%4 for browser compatibility.")
						   .arg(m_width).arg(m_height).arg(new_width).arg(new_height));
		m_width = new_width;
		m_height = new_height;
	}

	// Set output format to YUY2
	struct v4l2_format fmt;
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width = m_width;
	fmt.fmt.pix.height = m_height;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	fmt.fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;
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

	if(fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
		Logger::LogError("[V4L2Output::InitDevice] " + Logger::tr("Device does not support YUY2 format."));
		::close(m_fd);
		m_fd = -1;
		return false;
	}

	m_frame_size = m_width * m_height * 2;
	Logger::LogInfo("[V4L2Output::InitDevice] " + Logger::tr("Virtual camera output initialized: %1x%2 YUY2 @ %3 fps, frame size=%4 bytes")
					.arg(m_width).arg(m_height).arg(m_frame_rate).arg(m_frame_size));

	return true;
}

void V4L2Output::FreeDevice() {
	if(m_fd >= 0) {
		::close(m_fd);
		m_fd = -1;
	}
}

#endif
