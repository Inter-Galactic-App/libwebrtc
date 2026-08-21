/*
 *  Copyright (c) 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "src/internal/video_capturer.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#include "api/scoped_refptr.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame_buffer.h"
#include "api/video/video_rotation.h"
#if defined(WEBRTC_WIN)
#include "src/win/intergalactic_d3d11_nv12_buffer.h"
#endif

namespace webrtc {
namespace internal {
namespace {

using IntergalacticClock = std::chrono::steady_clock;

double IntergalacticElapsedMs(IntergalacticClock::time_point start,
                              IntergalacticClock::time_point end =
                                  IntergalacticClock::now()) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

std::string IntergalacticNativeWebrtcDiagnosticPath() {
  const char* temp = std::getenv("TEMP");
  if (temp == nullptr || temp[0] == '\0') {
    temp = std::getenv("TMP");
  }
  if (temp == nullptr || temp[0] == '\0') {
    return {};
  }
  std::string path(temp);
  if (!path.empty() && path.back() != '\\' && path.back() != '/') {
    path.push_back('\\');
  }
  path.append("intergalactic-native-webrtc-diagnostics.log");
  return path;
}

void AppendIntergalacticNativeWebrtcDiagnosticLine(const std::string& line) {
  if (line.empty()) {
    return;
  }
  static std::mutex file_mutex;
  std::lock_guard<std::mutex> lock(file_mutex);
  const std::string path = IntergalacticNativeWebrtcDiagnosticPath();
  if (path.empty()) {
    return;
  }
  std::ofstream file(path, std::ios::out | std::ios::app | std::ios::binary);
  if (!file.is_open()) {
    return;
  }
  file << line << '\n';
}

class IntergalacticSourceOnFrameStats {
 public:
  void Add(bool native_frame,
           double total_ms,
           double adapt_ms,
           double scale_ms,
           double broadcast_ms,
           bool dropped_by_adapter,
           bool scaled) {
    if (!native_frame) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ++calls_;
    total_ms_ += total_ms;
    max_total_ms_ = std::max(max_total_ms_, total_ms);
    adapt_ms_ += adapt_ms;
    max_adapt_ms_ = std::max(max_adapt_ms_, adapt_ms);
    scale_ms_ += scale_ms;
    max_scale_ms_ = std::max(max_scale_ms_, scale_ms);
    broadcast_ms_ += broadcast_ms;
    max_broadcast_ms_ = std::max(max_broadcast_ms_, broadcast_ms);
    if (dropped_by_adapter) {
      ++adapter_drops_;
    }
    if (scaled) {
      ++scaled_frames_;
    }
    const auto now = IntergalacticClock::now();
    if (calls_ <= 3 || now - last_log_ >= std::chrono::seconds(1) ||
        total_ms > 16.0 || adapter_drops_ > last_logged_adapter_drops_) {
      LogLocked(now);
    }
  }

 private:
  double Average(double total, uint64_t samples) const {
    return samples == 0 ? 0.0 : total / static_cast<double>(samples);
  }

  void LogLocked(IntergalacticClock::time_point now) {
    last_log_ = now;
    last_logged_adapter_drops_ = adapter_drops_;
    std::ostringstream message;
    message << "Inter Galactic WebRTC sender handoff source_on_frame stats"
            << " calls=" << calls_
            << " adapter_drops=" << adapter_drops_
            << " scaled=" << scaled_frames_
            << " avg_ms=" << Average(total_ms_, calls_)
            << " max_ms=" << max_total_ms_
            << " adapt_ms=" << Average(adapt_ms_, calls_)
            << " adapt_max_ms=" << max_adapt_ms_
            << " scale_ms=" << Average(scale_ms_, calls_)
            << " scale_max_ms=" << max_scale_ms_
            << " broadcast_ms=" << Average(broadcast_ms_, calls_)
            << " broadcast_max_ms=" << max_broadcast_ms_;
    AppendIntergalacticNativeWebrtcDiagnosticLine(message.str());
  }

  std::mutex mutex_;
  uint64_t calls_ = 0;
  uint64_t adapter_drops_ = 0;
  uint64_t scaled_frames_ = 0;
  uint64_t last_logged_adapter_drops_ = 0;
  double total_ms_ = 0.0;
  double max_total_ms_ = 0.0;
  double adapt_ms_ = 0.0;
  double max_adapt_ms_ = 0.0;
  double scale_ms_ = 0.0;
  double max_scale_ms_ = 0.0;
  double broadcast_ms_ = 0.0;
  double max_broadcast_ms_ = 0.0;
  IntergalacticClock::time_point last_log_{};
};

IntergalacticSourceOnFrameStats& IntergalacticSourceStats() {
  static IntergalacticSourceOnFrameStats stats;
  return stats;
}

bool IsIntergalacticD3d11HelperNativeFrame(const VideoFrame& frame) {
#if defined(WEBRTC_WIN)
  if (frame.video_frame_buffer() == nullptr ||
      frame.video_frame_buffer()->type() != VideoFrameBuffer::Type::kNative) {
    return false;
  }
  auto* native_nv12 =
      owt::base::IntergalacticD3D11Nv12Buffer::FromVideoFrameBuffer(
          frame.video_frame_buffer().get());
  return native_nv12 != nullptr &&
         native_nv12->source_mode() == "helper-d3d11";
#else
  (void)frame;
  return false;
#endif
}

bool CanBypassNativeGameplayFramerateDrop(const VideoFrame& frame,
                                          const VideoSinkWants& wants) {
  if (!IsIntergalacticD3d11HelperNativeFrame(frame) || !wants.is_active ||
      wants.black_frames || wants.max_pixel_count <= 0 || frame.width() <= 0 ||
      frame.height() <= 0) {
    return false;
  }

  const int64_t pixels = static_cast<int64_t>(frame.width()) * frame.height();
  if (pixels > wants.max_pixel_count) {
    return false;
  }
  if (wants.target_pixel_count.has_value() &&
      pixels > *wants.target_pixel_count) {
    return false;
  }
  if (wants.requested_resolution.has_value() &&
      (frame.width() > wants.requested_resolution->width ||
       frame.height() > wants.requested_resolution->height)) {
    return false;
  }
  if (wants.resolution_alignment > 1 &&
      (frame.width() % wants.resolution_alignment != 0 ||
       frame.height() % wants.resolution_alignment != 0)) {
    return false;
  }
  return true;
}

}  // namespace

VideoCapturer::VideoCapturer() = default;
VideoCapturer::~VideoCapturer() = default;

void VideoCapturer::OnFrame(const VideoFrame& frame) {
  const bool native_frame = frame.video_frame_buffer() != nullptr &&
                            frame.video_frame_buffer()->type() ==
                                VideoFrameBuffer::Type::kNative;
  const auto total_start = IntergalacticClock::now();
  int cropped_width = 0;
  int cropped_height = 0;
  int out_width = 0;
  int out_height = 0;
  const VideoSinkWants sink_wants = broadcaster_.wants();

  const auto adapt_start = IntergalacticClock::now();
  if (!video_adapter_.AdaptFrameResolution(
          frame.width(), frame.height(), frame.timestamp_us() * 1000,
          &cropped_width, &cropped_height, &out_width, &out_height)) {
    if (CanBypassNativeGameplayFramerateDrop(frame, sink_wants)) {
      const double adapt_ms = IntergalacticElapsedMs(adapt_start);
      const auto broadcast_start = IntergalacticClock::now();
      broadcaster_.OnFrame(frame);
      IntergalacticSourceStats().Add(
          native_frame, IntergalacticElapsedMs(total_start), adapt_ms, 0.0,
          IntergalacticElapsedMs(broadcast_start),
          /*dropped_by_adapter=*/false, /*scaled=*/false);
      return;
    }
    // Drop frame in order to respect frame rate constraint.
    IntergalacticSourceStats().Add(
        native_frame, IntergalacticElapsedMs(total_start),
        IntergalacticElapsedMs(adapt_start), 0.0, 0.0,
        /*dropped_by_adapter=*/true, /*scaled=*/false);
    return;
  }
  const double adapt_ms = IntergalacticElapsedMs(adapt_start);

  if (out_height != frame.height() || out_width != frame.width()) {
    // Video adapter has requested a down-scale. Allocate a new buffer and
    // return scaled version.
    const auto scale_start = IntergalacticClock::now();
    webrtc::scoped_refptr<I420BufferInterface> i420_buffer =
        frame.video_frame_buffer()->ToI420();
    if (!i420_buffer) {
      IntergalacticSourceStats().Add(
          native_frame, IntergalacticElapsedMs(total_start), adapt_ms,
          IntergalacticElapsedMs(scale_start), 0.0,
          /*dropped_by_adapter=*/true, /*scaled=*/true);
      return;
    }
    webrtc::scoped_refptr<I420Buffer> scaled_buffer =
        I420Buffer::Create(out_width, out_height);
    scaled_buffer->ScaleFrom(*i420_buffer);
    const double scale_ms = IntergalacticElapsedMs(scale_start);
    const auto broadcast_start = IntergalacticClock::now();
    broadcaster_.OnFrame(VideoFrame::Builder()
                             .set_video_frame_buffer(scaled_buffer)
                             .set_rotation(kVideoRotation_0)
                             .set_timestamp_us(frame.timestamp_us())
                             .set_id(frame.id())
                             .build());
    IntergalacticSourceStats().Add(
        native_frame, IntergalacticElapsedMs(total_start), adapt_ms, scale_ms,
        IntergalacticElapsedMs(broadcast_start),
        /*dropped_by_adapter=*/false, /*scaled=*/true);
  } else {
    // No adaptations needed, just return the frame as is.
    const auto broadcast_start = IntergalacticClock::now();
    broadcaster_.OnFrame(frame);
    IntergalacticSourceStats().Add(
        native_frame, IntergalacticElapsedMs(total_start), adapt_ms, 0.0,
        IntergalacticElapsedMs(broadcast_start),
        /*dropped_by_adapter=*/false, /*scaled=*/false);
  }
}

webrtc::VideoSinkWants VideoCapturer::GetSinkWants() {
  return broadcaster_.wants();
}

void VideoCapturer::AddOrUpdateSink(webrtc::VideoSinkInterface<VideoFrame>* sink,
                                    const webrtc::VideoSinkWants& wants) {
  broadcaster_.AddOrUpdateSink(sink, wants);
  UpdateVideoAdapter();
}

void VideoCapturer::RemoveSink(webrtc::VideoSinkInterface<VideoFrame>* sink) {
  broadcaster_.RemoveSink(sink);
  UpdateVideoAdapter();
}

void VideoCapturer::UpdateVideoAdapter() {
  webrtc::VideoSinkWants wants = broadcaster_.wants();

  if (0 < wants.resolutions.size()) {
    auto size = wants.resolutions.at(0);
    std::pair<int, int> target_aspect_ratiot(size.width, size.height);
    video_adapter_.OnOutputFormatRequest(
        target_aspect_ratiot, wants.max_pixel_count, wants.max_framerate_fps);
  } else {
    video_adapter_.OnSinkWants(wants);
  }
}

}  // namespace internal
}  // namespace webrtc
