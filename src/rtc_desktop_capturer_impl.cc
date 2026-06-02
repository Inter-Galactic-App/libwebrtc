/*
 * Copyright 2022 LiveKit
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "rtc_desktop_capturer_impl.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>

#include "api/sequence_checker.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv.h"
#ifdef WEBRTC_WIN
#include <windows.h>

#include "modules/desktop_capture/win/window_capture_utils.h"
#endif

namespace libwebrtc {

enum { kCaptureDelay = 33, kCaptureMessageId = 1000 };

namespace {

int MakeEvenDimension(int value) {
  if (value <= 2) {
    return 2;
  }
  return value % 2 == 0 ? value : value - 1;
}

int ContainFitDimension(int source, int max, double scale) {
  if (max <= 0 || scale >= 1.0) {
    return MakeEvenDimension(source);
  }
  int scaled = static_cast<int>(source * scale + 0.5);
  if (scaled > max) {
    scaled = max;
  }
  return MakeEvenDimension(scaled);
}

void FillI420Black(const webrtc::scoped_refptr<webrtc::I420Buffer>& buffer) {
  if (!buffer) {
    return;
  }

  for (int y = 0; y < buffer->height(); ++y) {
    std::memset(buffer->MutableDataY() + y * buffer->StrideY(), 16,
                buffer->width());
  }

  const int chroma_width = buffer->width() / 2;
  const int chroma_height = buffer->height() / 2;
  for (int y = 0; y < chroma_height; ++y) {
    std::memset(buffer->MutableDataU() + y * buffer->StrideU(), 128,
                chroma_width);
    std::memset(buffer->MutableDataV() + y * buffer->StrideV(), 128,
                chroma_width);
  }
}

int64_t NowMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

double MicrosToMillis(int64_t micros) {
  return static_cast<double>(micros) / 1000.0;
}

double PercentileMillis(std::vector<int64_t> values, double percentile) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const double clamped = std::max(0.0, std::min(1.0, percentile));
  const size_t index = std::min(
      values.size() - 1,
      static_cast<size_t>(clamped * static_cast<double>(values.size() - 1)));
  return MicrosToMillis(values[index]);
}

double MaxMillis(const std::vector<int64_t>& values) {
  if (values.empty()) {
    return 0.0;
  }
  return MicrosToMillis(*std::max_element(values.begin(), values.end()));
}

struct UpdatedRegionStats {
  uint32_t rect_count = 0;
  int64_t area = 0;
  double area_ratio = 0.0;
  bool full_frame = false;
  bool tiny = false;
};

UpdatedRegionStats AnalyzeUpdatedRegion(const webrtc::DesktopRegion& region,
                                         int frame_width,
                                         int frame_height) {
  UpdatedRegionStats stats;
  if (region.is_empty() || frame_width <= 0 || frame_height <= 0) {
    return stats;
  }

  const int64_t frame_area =
      static_cast<int64_t>(frame_width) * static_cast<int64_t>(frame_height);
  for (webrtc::DesktopRegion::Iterator it(region); !it.IsAtEnd();
       it.Advance()) {
    const webrtc::DesktopRect& rect = it.rect();
    if (rect.is_empty()) {
      continue;
    }
    ++stats.rect_count;
    const int rect_width = std::max(0, rect.width());
    const int rect_height = std::max(0, rect.height());
    stats.area +=
        static_cast<int64_t>(rect_width) * static_cast<int64_t>(rect_height);
  }

  if (frame_area <= 0) {
    return stats;
  }
  stats.area = std::max<int64_t>(0, std::min(stats.area, frame_area));
  stats.area_ratio =
      static_cast<double>(stats.area) / static_cast<double>(frame_area);
  stats.full_frame = stats.area_ratio >= 0.98;
  stats.tiny = stats.rect_count > 0 && stats.area_ratio <= 0.01;
  return stats;
}

void AppendNativeWebrtcDiagnosticLine(const std::string& line) {
#ifdef WEBRTC_WIN
  if (line.empty()) {
    return;
  }

  static std::mutex file_mutex;
  std::lock_guard<std::mutex> lock(file_mutex);

  wchar_t temp_path[MAX_PATH + 1] = {};
  const DWORD temp_length = GetTempPathW(MAX_PATH + 1, temp_path);
  if (temp_length == 0 || temp_length > MAX_PATH) {
    return;
  }

  std::wstring path(temp_path, temp_length);
  if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
    path.push_back(L'\\');
  }
  path.append(L"intergalactic-native-webrtc-diagnostics.log");

  HANDLE file = CreateFileW(
      path.c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }

  LARGE_INTEGER size = {};
  if (GetFileSizeEx(file, &size) && size.QuadPart > 512 * 1024) {
    LARGE_INTEGER zero = {};
    if (SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) {
      SetEndOfFile(file);
    }
  }

  LARGE_INTEGER end = {};
  SetFilePointerEx(file, end, nullptr, FILE_END);

  SYSTEMTIME now = {};
  GetSystemTime(&now);
  char prefix[64];
  snprintf(prefix, sizeof(prefix),
           "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ native-webrtc ",
           now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
           now.wSecond, now.wMilliseconds);

  std::string output(prefix);
  output.append(line);
  output.append("\r\n");

  DWORD written = 0;
  WriteFile(file, output.data(), static_cast<DWORD>(output.size()), &written,
            nullptr);
  FlushFileBuffers(file);
  CloseHandle(file);
#endif
}

std::string NormalizeWindowsCaptureBackendMode(const char* mode) {
  if (mode == nullptr) {
    return "default";
  }
  std::string normalized(mode);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  normalized.erase(
      std::remove_if(normalized.begin(), normalized.end(),
                     [](unsigned char c) { return std::isspace(c) != 0; }),
      normalized.end());
  if (normalized == "wgconly") {
    normalized = "wgc-only";
  } else if (normalized == "directxonly") {
    normalized = "directx-only";
  } else if (normalized == "windowcrop" || normalized == "crop-window") {
    normalized = "window-crop";
  }
  if (normalized == "wgc-only" || normalized == "directx-only" ||
      normalized == "window-crop") {
    return normalized;
  }
  return "default";
}

}  // namespace

RTCDesktopCapturerImpl::RTCDesktopCapturerImpl(
    DesktopType type, webrtc::DesktopCapturer::SourceId source_id,
    webrtc::Thread* signaling_thread, scoped_refptr<MediaSource> source,
    bool showCursor)
    : thread_(webrtc::Thread::Create()),
      type_(type),
      source_id_(source_id),
      show_cursor_(showCursor),
      signaling_thread_(signaling_thread),
      source_(source) {
  RTC_DCHECK(thread_);
  thread_->Start();
  options_ = webrtc::DesktopCaptureOptions::CreateDefault();
  options_.set_detect_updated_region(true);
  ConfigureWindowsCaptureBackendMode(windows_capture_backend_mode_);
#ifdef WEBRTC_LINUX
  if (type == kScreen) {
    options_.set_allow_pipewire(true);
  }
#endif
  thread_->BlockingCall([this] { CreateDesktopCapturerOnThread(); });
}

RTCDesktopCapturerImpl::~RTCDesktopCapturerImpl() {
  thread_->Stop();
  capturer_.reset();
}

void RTCDesktopCapturerImpl::CreateDesktopCapturerOnThread() {
  RTC_DCHECK_RUN_ON(thread_.get());
  if (type_ == kScreen) {
    if (show_cursor_) {
      capturer_ = std::make_unique<webrtc::DesktopAndCursorComposer>(
          webrtc::DesktopCapturer::CreateScreenCapturer(options_), options_);
    } else {
      capturer_ =
          webrtc::DesktopAndCursorComposer::CreateWithoutMouseCursorMonitor(
              webrtc::DesktopCapturer::CreateScreenCapturer(options_));
    }
  } else {
    capturer_ = std::make_unique<webrtc::DesktopAndCursorComposer>(
        webrtc::DesktopCapturer::CreateWindowCapturer(options_), options_);
  }
}

void RTCDesktopCapturerImpl::ConfigureWindowsCaptureBackendMode(
    const std::string& mode) {
#ifdef WEBRTC_WIN
  options_.set_allow_directx_capturer(true);
  options_.set_allow_cropping_window_capturer(true);
#if defined(RTC_ENABLE_WIN_WGC)
  options_.set_allow_wgc_screen_capturer(true);
  options_.set_allow_wgc_window_capturer(true);
  options_.set_allow_wgc_capturer_fallback(true);
#endif

  if (mode == "wgc-only") {
    options_.set_allow_directx_capturer(false);
    options_.set_allow_cropping_window_capturer(false);
#if defined(RTC_ENABLE_WIN_WGC)
    options_.set_allow_wgc_screen_capturer(true);
    options_.set_allow_wgc_window_capturer(true);
    options_.set_allow_wgc_capturer_fallback(false);
#endif
  } else if (mode == "directx-only") {
    options_.set_allow_directx_capturer(true);
    options_.set_allow_cropping_window_capturer(false);
#if defined(RTC_ENABLE_WIN_WGC)
    options_.set_allow_wgc_screen_capturer(false);
    options_.set_allow_wgc_window_capturer(false);
    options_.set_allow_wgc_capturer_fallback(false);
#endif
  } else if (mode == "window-crop") {
    options_.set_allow_directx_capturer(true);
    options_.set_allow_cropping_window_capturer(true);
#if defined(RTC_ENABLE_WIN_WGC)
    options_.set_allow_wgc_screen_capturer(false);
    options_.set_allow_wgc_window_capturer(false);
    options_.set_allow_wgc_capturer_fallback(false);
#endif
  }

  std::ostringstream options_message;
  options_message << "Inter Galactic desktop capture options type="
                  << (type_ == kScreen ? "screen" : "window")
                  << " mode=" << mode
                  << " detect_updated_region="
                  << options_.detect_updated_region()
                  << " directx=" << options_.allow_directx_capturer()
                  << " crop_window="
                  << options_.allow_cropping_window_capturer()
#if defined(RTC_ENABLE_WIN_WGC)
                  << " wgc_screen=" << options_.allow_wgc_screen_capturer()
                  << " wgc_window=" << options_.allow_wgc_window_capturer()
                  << " wgc_fallback=" << options_.allow_wgc_capturer_fallback()
#else
                  << " wgc=unavailable"
#endif
      ;
  const std::string options_line = options_message.str();
  RTC_LOG(LS_INFO) << options_line;
  AppendNativeWebrtcDiagnosticLine(options_line);
#else
  (void)mode;
#endif
}

void RTCDesktopCapturerImpl::SetWindowsCaptureBackendMode(const char* mode) {
#ifdef WEBRTC_WIN
  const std::string normalized = NormalizeWindowsCaptureBackendMode(mode);
  windows_capture_backend_mode_ = normalized;
  if (capture_state_ == CS_RUNNING) {
    std::ostringstream message;
    message << "Inter Galactic desktop capture backend change ignored while "
            << "running mode=" << normalized;
    const std::string line = message.str();
    RTC_LOG(LS_INFO) << line;
    AppendNativeWebrtcDiagnosticLine(line);
    return;
  }

  options_ = webrtc::DesktopCaptureOptions::CreateDefault();
  options_.set_detect_updated_region(true);
  ConfigureWindowsCaptureBackendMode(windows_capture_backend_mode_);
  thread_->BlockingCall([this] { CreateDesktopCapturerOnThread(); });
#else
  (void)mode;
#endif
}

void RTCDesktopCapturerImpl::SetLatestFramePacingEnabled(bool enabled) {
#ifdef WEBRTC_WIN
  latest_frame_pacing_enabled_ = enabled;
  std::ostringstream message;
  message << "Inter Galactic desktop capture latest-frame pacer "
          << (enabled ? "enabled" : "disabled");
  const std::string line = message.str();
  RTC_LOG(LS_INFO) << line;
  AppendNativeWebrtcDiagnosticLine(line);
#else
  (void)enabled;
#endif
}

void RTCDesktopCapturerImpl::ResetLatestFramePacerState() {
  latest_paced_frame_buffer_ = nullptr;
  latest_paced_frame_sequence_ = 0;
  last_paced_submitted_sequence_ = 0;
  latest_paced_frame_capture_us_ = 0;
  latest_paced_submit_last_us_ = 0;
  latest_paced_log_start_ms_ = 0;
  latest_paced_log_ticks_ = 0;
  latest_paced_log_submitted_frames_ = 0;
  latest_paced_log_unique_frames_ = 0;
  latest_paced_log_duplicate_frames_ = 0;
  latest_paced_log_skipped_ticks_ = 0;
  latest_paced_log_overwritten_frames_ = 0;
  latest_paced_log_age_total_us_ = 0;
  latest_paced_log_age_max_us_ = 0;
  latest_paced_log_on_frame_total_us_ = 0;
  latest_paced_log_on_frame_max_us_ = 0;
  latest_paced_submit_interval_values_us_.clear();
}

RTCDesktopCapturerImpl::CaptureState RTCDesktopCapturerImpl::Start(
    uint32_t fps, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  x_ = x;
  y_ = y;
  w_ = w;
  h_ = h;
  max_frame_width_ = 0;
  max_frame_height_ = 0;
  if (!w_ || !h_) {
    x_ = 0;
    y_ = 0;
    w_ = 0;
    h_ = 0;
  }
  return Start(fps);
}

RTCDesktopCapturerImpl::CaptureState
RTCDesktopCapturerImpl::StartWithMaxFrameSize(uint32_t fps, uint32_t max_w,
                                              uint32_t max_h) {
  x_ = 0;
  y_ = 0;
  w_ = 0;
  h_ = 0;
  max_frame_width_ = max_w;
  max_frame_height_ = max_h;
  if (!max_frame_width_ || !max_frame_height_) {
    max_frame_width_ = 0;
    max_frame_height_ = 0;
  }
  return Start(fps);
}

RTCDesktopCapturerImpl::CaptureState RTCDesktopCapturerImpl::Start(
    uint32_t fps) {
  if (capture_state_ == CS_RUNNING) {
    return capture_state_;
  }

  if (fps == 0) {
    capture_state_ = CS_FAILED;
    return capture_state_;
  }

  if (fps >= 60) {
    capture_delay_ = uint32_t(1000.0 / 60.0);
  } else {
    capture_delay_ = uint32_t(1000.0 / fps);
  }
  capture_pipeline_log_start_ms_ = 0;
  capture_pipeline_log_frames_ = 0;
  capture_schedule_log_start_ms_ = 0;
  capture_schedule_log_calls_ = 0;
  capture_schedule_log_work_total_ms_ = 0;
  capture_schedule_log_max_work_ms_ = 0;
  capture_schedule_log_source_capture_total_ms_ = 0;
  capture_schedule_log_source_capture_max_ms_ = 0;
  capture_schedule_log_source_capture_count_ = 0;
  capture_schedule_log_callback_total_us_ = 0;
  capture_schedule_log_callback_max_us_ = 0;
  capture_schedule_log_callback_entry_delay_total_us_ = 0;
  capture_schedule_log_callback_entry_delay_max_us_ = 0;
  capture_schedule_log_post_callback_wait_total_us_ = 0;
  capture_schedule_log_post_callback_wait_max_us_ = 0;
  capture_schedule_log_unaccounted_wait_total_us_ = 0;
  capture_schedule_log_unaccounted_wait_max_us_ = 0;
  capture_schedule_log_acquire_wait_total_us_ = 0;
  capture_schedule_log_acquire_wait_max_us_ = 0;
  capture_schedule_log_callback_count_ = 0;
  capture_schedule_last_delay_ms_ = capture_delay_;
  capture_schedule_log_temp_errors_ = 0;
  capture_schedule_log_permanent_errors_ = 0;
  capture_call_active_ = false;
  capture_call_started_us_ = 0;
  capture_call_current_source_capture_total_us_ = 0;
  capture_call_current_callback_entry_delay_us_ = -1;
  capture_call_current_callback_finished_us_ = 0;
  capture_call_current_callback_total_us_ = 0;
  capture_call_current_callback_max_us_ = 0;
  capture_call_current_callback_count_ = 0;
  capture_frame_timing_log_start_ms_ = 0;
  capture_frame_timing_log_frames_ = 0;
  capture_frame_last_success_us_ = 0;
  capture_frame_interval_values_us_.clear();
  capture_frame_duplicated_frames_ = 0;
  capture_frame_stale_reuse_count_ = 0;
  capture_frame_wait_timeouts_ = 0;
  capture_frame_permanent_errors_ = 0;
  capture_frame_convert_total_us_ = 0;
  capture_frame_scale_total_us_ = 0;
  capture_frame_on_frame_total_us_ = 0;
  capture_frame_callback_total_us_ = 0;
  capture_frame_callback_max_us_ = 0;
  capture_frame_updated_region_empty_count_ = 0;
  capture_frame_updated_region_nonempty_count_ = 0;
  capture_frame_updated_region_rect_count_ = 0;
  capture_frame_updated_region_max_rect_count_ = 0;
  capture_frame_updated_region_area_ratio_total_ = 0.0;
  capture_frame_updated_region_area_ratio_max_ = 0.0;
  capture_frame_updated_region_full_frame_count_ = 0;
  capture_frame_updated_region_tiny_frame_count_ = 0;
  ResetLatestFramePacerState();

  if (source_id_ != -1) {
    if (!capturer_->SelectSource(source_id_)) {
      capture_state_ = CS_FAILED;
      return capture_state_;
    }
    if (type_ == kWindow) {
      if (!capturer_->FocusOnSelectedSource()) {
        capture_state_ = CS_FAILED;
        return capture_state_;
      }
    }
  }

  thread_->BlockingCall([this] { capturer_->Start(this); });
  capture_state_ = CS_RUNNING;
  thread_->PostTask([this] { CaptureFrame(); });
  if (latest_frame_pacing_enabled_) {
    thread_->PostTask([this] { PaceLatestFrame(); });
  }
  if (observer_) {
    signaling_thread_->BlockingCall([&, this]() { observer_->OnStart(this); });
  }
  return capture_state_;
}

void RTCDesktopCapturerImpl::Stop() {
  if (observer_) {
    if (!signaling_thread_->IsCurrent()) {
      signaling_thread_->BlockingCall([&, this]() { observer_->OnStop(this); });
    } else {
      observer_->OnStop(this);
    }
  }
  capture_state_ = CS_STOPPED;
  ResetLatestFramePacerState();
}

bool RTCDesktopCapturerImpl::IsRunning() {
  return capture_state_ == CS_RUNNING;
}

#ifdef WEBRTC_WIN
int filterException(int code, PEXCEPTION_POINTERS ex) {
  return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void RTCDesktopCapturerImpl::OnCaptureResult(
    webrtc::DesktopCapturer::Result result,
    std::unique_ptr<webrtc::DesktopFrame> frame) {
  const int64_t callback_entry_started_us = NowMicros();
  if (capture_call_active_ &&
      capture_call_current_callback_entry_delay_us_ < 0 &&
      capture_call_started_us_ > 0) {
    capture_call_current_callback_entry_delay_us_ =
        std::max<int64_t>(0,
                          callback_entry_started_us - capture_call_started_us_);
  }

  if (result == webrtc::DesktopCapturer::Result::ERROR_TEMPORARY) {
    ++capture_schedule_log_temp_errors_;
    ++capture_frame_wait_timeouts_;
  } else if (result == webrtc::DesktopCapturer::Result::ERROR_PERMANENT) {
    ++capture_schedule_log_permanent_errors_;
    ++capture_frame_permanent_errors_;
  }

  if (result != result_) {
    if (result == webrtc::DesktopCapturer::Result::ERROR_PERMANENT) {
      if (observer_) {
        signaling_thread_->BlockingCall(
            [&, this]() { observer_->OnError(this); });
      }
      capture_state_ = CS_FAILED;
      return;
    }

    if (result == webrtc::DesktopCapturer::Result::ERROR_TEMPORARY) {
      result_ = result;
      if (observer_) {
        signaling_thread_->BlockingCall(
            [&, this]() { observer_->OnPaused(this); });
      }
      return;
    }

    if (result == webrtc::DesktopCapturer::Result::SUCCESS) {
      result_ = result;
      if (observer_) {
        signaling_thread_->BlockingCall(
            [&, this]() { observer_->OnStart(this); });
      }
    }
  }

  if (result == webrtc::DesktopCapturer::Result::ERROR_TEMPORARY) {
    return;
  }

  int frame_width = frame->size().width();
  int frame_height = frame->size().height();
#ifdef WEBRTC_WIN
  webrtc::DesktopRect rect_ = webrtc::DesktopRect::MakeWH(frame_width, frame_height);

  if (type_ != kScreen) {
    webrtc::GetWindowRect(reinterpret_cast<HWND>(source_id_), &rect_);
  }

  __try
#endif
  {
    const int64_t callback_started_us = NowMicros();
#ifdef WEBRTC_WIN
    int source_width = frame_width;
    int source_height = frame_height;
    int window_rect_width = rect_.width();
    int window_rect_height = rect_.height();
#else
    int source_width = frame_width;
    int source_height = frame_height;
    int window_rect_width = frame_width;
    int window_rect_height = frame_height;
#endif
    const int64_t source_capture_time_ms =
        std::max<int64_t>(0, frame->capture_time_ms());
    const bool updated_region_empty = frame->updated_region().is_empty();
    const UpdatedRegionStats updated_region_stats =
        AnalyzeUpdatedRegion(frame->updated_region(), source_width,
                             source_height);
    if (capture_call_active_) {
      capture_schedule_log_source_capture_total_ms_ +=
          source_capture_time_ms;
      capture_schedule_log_source_capture_max_ms_ =
          std::max(capture_schedule_log_source_capture_max_ms_,
                   source_capture_time_ms);
      ++capture_schedule_log_source_capture_count_;
      capture_call_current_source_capture_total_us_ +=
          source_capture_time_ms * 1000;
    }

    const bool crop_region = w_ > 0 && h_ > 0;
    int crop_x = crop_region ? x_ : 0;
    int crop_y = crop_region ? y_ : 0;
    int capture_width = crop_region ? w_ : source_width;
    int capture_height = crop_region ? h_ : source_height;
    if (capture_width <= 0 || capture_height <= 0) {
      return;
    }

    int scaled_width = MakeEvenDimension(capture_width);
    int scaled_height = MakeEvenDimension(capture_height);
    int output_width = scaled_width;
    int output_height = scaled_height;

    const bool has_max_frame_bounds =
        !crop_region && max_frame_width_ > 0 && max_frame_height_ > 0;
    const bool fixed_window_canvas =
        has_max_frame_bounds && type_ != kScreen;
    if (has_max_frame_bounds) {
      const double width_scale =
          static_cast<double>(max_frame_width_) / capture_width;
      const double height_scale =
          static_cast<double>(max_frame_height_) / capture_height;
      const double scale = std::min(1.0, std::min(width_scale, height_scale));
      scaled_width = ContainFitDimension(capture_width, max_frame_width_, scale);
      scaled_height =
          ContainFitDimension(capture_height, max_frame_height_, scale);
      output_width = fixed_window_canvas
                         ? MakeEvenDimension(static_cast<int>(max_frame_width_))
                         : scaled_width;
      output_height =
          fixed_window_canvas
              ? MakeEvenDimension(static_cast<int>(max_frame_height_))
              : scaled_height;
    }

    int output_x = 0;
    int output_y = 0;
    if (fixed_window_canvas) {
      output_x = std::max(0, (output_width - scaled_width) / 2);
      output_y = std::max(0, (output_height - scaled_height) / 2);
      output_x -= output_x % 2;
      output_y -= output_y % 2;
    }

    const bool uses_letterbox_canvas =
        fixed_window_canvas &&
        (output_width != scaled_width || output_height != scaled_height ||
         output_x != 0 || output_y != 0);

    if (source_width != last_logged_source_width_ ||
        source_height != last_logged_source_height_ ||
        window_rect_width != last_logged_window_rect_width_ ||
        window_rect_height != last_logged_window_rect_height_ ||
        max_frame_width_ != last_logged_max_frame_width_ ||
        max_frame_height_ != last_logged_max_frame_height_ ||
        output_width != last_logged_output_width_ ||
        output_height != last_logged_output_height_ ||
        scaled_width != last_logged_content_width_ ||
        scaled_height != last_logged_content_height_ ||
        fixed_window_canvas != last_logged_fixed_canvas_ ||
        crop_region != last_logged_crop_region_) {
      std::ostringstream message;
      message << "Inter Galactic desktop capture frame size source="
              << source_width << "x" << source_height
              << " window_rect=" << window_rect_width << "x"
              << window_rect_height
              << " max=" << max_frame_width_ << "x" << max_frame_height_
              << " content=" << scaled_width << "x" << scaled_height
              << " output=" << output_width << "x" << output_height
              << " canvas=" << (fixed_window_canvas ? "fixed" : "dynamic")
              << " crop_region=" << crop_region;
      const std::string line = message.str();
      RTC_LOG(LS_INFO) << line;
      AppendNativeWebrtcDiagnosticLine(line);
      last_logged_source_width_ = source_width;
      last_logged_source_height_ = source_height;
      last_logged_window_rect_width_ = window_rect_width;
      last_logged_window_rect_height_ = window_rect_height;
      last_logged_max_frame_width_ = max_frame_width_;
      last_logged_max_frame_height_ = max_frame_height_;
      last_logged_output_width_ = output_width;
      last_logged_output_height_ = output_height;
      last_logged_content_width_ = scaled_width;
      last_logged_content_height_ = scaled_height;
      last_logged_fixed_canvas_ = fixed_window_canvas;
      last_logged_crop_region_ = crop_region;
    }

    if (!i420_buffer_ || !i420_buffer_.get() ||
        i420_buffer_->width() != output_width ||
        i420_buffer_->height() != output_height) {
      i420_buffer_ = webrtc::I420Buffer::Create(output_width, output_height);
    }

    const bool should_scale =
        scaled_width != capture_width || scaled_height != capture_height;
    const bool needs_intermediate_buffer = should_scale || uses_letterbox_canvas;
    webrtc::scoped_refptr<webrtc::I420Buffer> convert_buffer = i420_buffer_;

    if (needs_intermediate_buffer) {
      if (!i420_full_frame_buffer_ || !i420_full_frame_buffer_.get() ||
          i420_full_frame_buffer_->width() != capture_width ||
          i420_full_frame_buffer_->height() != capture_height) {
        i420_full_frame_buffer_ =
            webrtc::I420Buffer::Create(capture_width, capture_height);
      }
      convert_buffer = i420_full_frame_buffer_;
    }

    const int64_t convert_started_us = NowMicros();
    libyuv::ConvertToI420(
        frame->data(), 0, convert_buffer->MutableDataY(),
        convert_buffer->StrideY(), convert_buffer->MutableDataU(),
        convert_buffer->StrideU(), convert_buffer->MutableDataV(),
        convert_buffer->StrideV(), crop_x, crop_y, source_width, source_height,
        capture_width, capture_height, libyuv::kRotate0, libyuv::FOURCC_ARGB);
    const int64_t convert_finished_us = NowMicros();

    int64_t scale_started_us = convert_finished_us;
    int64_t scale_finished_us = scale_started_us;
    if (needs_intermediate_buffer) {
      scale_started_us = NowMicros();
      if (uses_letterbox_canvas) {
        FillI420Black(i420_buffer_);
      }
      uint8_t* scaled_data_y = i420_buffer_->MutableDataY() +
                               output_y * i420_buffer_->StrideY() + output_x;
      uint8_t* scaled_data_u =
          i420_buffer_->MutableDataU() +
          (output_y / 2) * i420_buffer_->StrideU() + (output_x / 2);
      uint8_t* scaled_data_v =
          i420_buffer_->MutableDataV() +
          (output_y / 2) * i420_buffer_->StrideV() + (output_x / 2);
      libyuv::I420Scale(
          convert_buffer->DataY(), convert_buffer->StrideY(),
          convert_buffer->DataU(), convert_buffer->StrideU(),
          convert_buffer->DataV(), convert_buffer->StrideV(), capture_width,
          capture_height, scaled_data_y, i420_buffer_->StrideY(),
          scaled_data_u, i420_buffer_->StrideU(), scaled_data_v,
          i420_buffer_->StrideV(), scaled_width, scaled_height,
          libyuv::kFilterBox);
      scale_finished_us = NowMicros();
    }

    const int64_t now_ms = webrtc::TimeMillis();
    if (capture_pipeline_log_start_ms_ <= 0) {
      capture_pipeline_log_start_ms_ = now_ms;
      capture_pipeline_log_frames_ = 0;
    }
    ++capture_pipeline_log_frames_;
    const int64_t elapsed_ms = now_ms - capture_pipeline_log_start_ms_;
    if (elapsed_ms >= 2000) {
      const double emitted_fps =
          static_cast<double>(capture_pipeline_log_frames_) * 1000.0 /
          static_cast<double>(elapsed_ms);
      const double target_fps =
          capture_delay_ == 0 ? 0.0 : 1000.0 / capture_delay_;
      std::ostringstream message;
      message << "Inter Galactic desktop capture pipeline native_source="
              << source_width << "x" << source_height
              << " native_window_rect=" << window_rect_width << "x"
              << window_rect_height << " requested_max=" << max_frame_width_
              << "x" << max_frame_height_ << " content=" << scaled_width
              << "x" << scaled_height
              << " pre_encode=" << output_width << "x" << output_height
              << " target_fps=" << target_fps
              << " native_fps=" << emitted_fps
              << " scale=" << (should_scale ? "down" : "none")
              << " canvas="
              << (uses_letterbox_canvas
                      ? "letterbox"
                      : (fixed_window_canvas ? "fixed" : "dynamic"))
              << " crop_region=" << (crop_region ? "true" : "false");
      const std::string line = message.str();
      RTC_LOG(LS_INFO) << line;
      AppendNativeWebrtcDiagnosticLine(line);
      capture_pipeline_log_start_ms_ = now_ms;
      capture_pipeline_log_frames_ = 0;
    }

    if (latest_frame_pacing_enabled_) {
      if (latest_paced_frame_sequence_ > last_paced_submitted_sequence_) {
        ++latest_paced_log_overwritten_frames_;
      }
      latest_paced_frame_buffer_ = i420_buffer_;
      latest_paced_frame_capture_us_ = callback_started_us;
      ++latest_paced_frame_sequence_;
    }

    const int64_t on_frame_started_us = NowMicros();
    if (!latest_frame_pacing_enabled_) {
      OnFrame(webrtc::VideoFrame(i420_buffer_, 0, webrtc::TimeMillis(),
                                 webrtc::kVideoRotation_0));
    }
    const int64_t on_frame_finished_us = NowMicros();
    const int64_t callback_total_us =
        std::max<int64_t>(0, on_frame_finished_us - callback_started_us);
    const int64_t convert_total_us =
        std::max<int64_t>(0, convert_finished_us - convert_started_us);
    const int64_t scale_total_us =
        std::max<int64_t>(0, scale_finished_us - scale_started_us);
    const int64_t on_frame_total_us =
        std::max<int64_t>(0, on_frame_finished_us - on_frame_started_us);
    if (capture_call_active_) {
      capture_call_current_callback_total_us_ += callback_total_us;
      capture_call_current_callback_max_us_ =
          std::max(capture_call_current_callback_max_us_, callback_total_us);
      ++capture_call_current_callback_count_;
      capture_call_current_callback_finished_us_ = on_frame_finished_us;
    }
    if (capture_frame_last_success_us_ > 0) {
      capture_frame_interval_values_us_.push_back(
          std::max<int64_t>(0, callback_started_us -
                                   capture_frame_last_success_us_));
    }
    capture_frame_last_success_us_ = callback_started_us;

    const int64_t timing_now_ms = webrtc::TimeMillis();
    if (capture_frame_timing_log_start_ms_ <= 0) {
      capture_frame_timing_log_start_ms_ = timing_now_ms;
      capture_frame_timing_log_frames_ = 0;
      capture_frame_convert_total_us_ = 0;
      capture_frame_scale_total_us_ = 0;
      capture_frame_on_frame_total_us_ = 0;
      capture_frame_callback_total_us_ = 0;
      capture_frame_callback_max_us_ = 0;
      capture_frame_updated_region_empty_count_ = 0;
      capture_frame_updated_region_nonempty_count_ = 0;
      capture_frame_updated_region_rect_count_ = 0;
      capture_frame_updated_region_max_rect_count_ = 0;
      capture_frame_updated_region_area_ratio_total_ = 0.0;
      capture_frame_updated_region_area_ratio_max_ = 0.0;
      capture_frame_updated_region_full_frame_count_ = 0;
      capture_frame_updated_region_tiny_frame_count_ = 0;
    }
    ++capture_frame_timing_log_frames_;
    if (updated_region_empty) {
      ++capture_frame_updated_region_empty_count_;
    } else {
      ++capture_frame_updated_region_nonempty_count_;
    }
    capture_frame_updated_region_rect_count_ +=
        updated_region_stats.rect_count;
    capture_frame_updated_region_max_rect_count_ =
        std::max(capture_frame_updated_region_max_rect_count_,
                 updated_region_stats.rect_count);
    capture_frame_updated_region_area_ratio_total_ +=
        updated_region_stats.area_ratio;
    capture_frame_updated_region_area_ratio_max_ =
        std::max(capture_frame_updated_region_area_ratio_max_,
                 updated_region_stats.area_ratio);
    if (updated_region_stats.full_frame) {
      ++capture_frame_updated_region_full_frame_count_;
    }
    if (updated_region_stats.tiny) {
      ++capture_frame_updated_region_tiny_frame_count_;
    }
    capture_frame_convert_total_us_ += convert_total_us;
    capture_frame_scale_total_us_ += scale_total_us;
    capture_frame_on_frame_total_us_ += on_frame_total_us;
    capture_frame_callback_total_us_ += callback_total_us;
    capture_frame_callback_max_us_ =
        std::max(capture_frame_callback_max_us_, callback_total_us);
    const int64_t timing_elapsed_ms =
        timing_now_ms - capture_frame_timing_log_start_ms_;
    if (timing_elapsed_ms >= 2000 && capture_frame_timing_log_frames_ > 0) {
      const double frames =
          static_cast<double>(capture_frame_timing_log_frames_);
      const double acquired_fps =
          frames * 1000.0 / static_cast<double>(timing_elapsed_ms);
      const double submitted_fps =
          latest_frame_pacing_enabled_ ? 0.0 : acquired_fps;
      const double new_fps = acquired_fps;
      const double max_interval_ms =
          MaxMillis(capture_frame_interval_values_us_);
      const double p95_interval_ms =
          PercentileMillis(capture_frame_interval_values_us_, 0.95);
      {
        std::ostringstream message;
        message << "Inter Galactic desktop capture frame cadence new_fps="
                << new_fps << " submitted_fps=" << submitted_fps
                << " max_interval_ms=" << max_interval_ms
                << " p95_interval_ms=" << p95_interval_ms
                << " duplicated_frames=" << capture_frame_duplicated_frames_
                << " stale_reuse=" << capture_frame_stale_reuse_count_
                << " wait_timeouts=" << capture_frame_wait_timeouts_
                << " permanent_errors=" << capture_frame_permanent_errors_
                << " frames=" << capture_frame_timing_log_frames_;
        const std::string line = message.str();
        RTC_LOG(LS_INFO) << line;
        AppendNativeWebrtcDiagnosticLine(line);
      }
      std::ostringstream message;
      message << "Inter Galactic desktop capture frame timing avg_convert_ms="
              << MicrosToMillis(capture_frame_convert_total_us_) / frames
              << " avg_scale_ms="
              << MicrosToMillis(capture_frame_scale_total_us_) / frames
              << " avg_on_frame_ms="
              << MicrosToMillis(capture_frame_on_frame_total_us_) / frames
              << " avg_callback_ms="
              << MicrosToMillis(capture_frame_callback_total_us_) / frames
              << " max_callback_ms="
              << MicrosToMillis(capture_frame_callback_max_us_)
              << " updated_region_empty="
              << capture_frame_updated_region_empty_count_
              << " updated_region_nonempty="
              << capture_frame_updated_region_nonempty_count_
              << " updated_region_rects="
              << capture_frame_updated_region_rect_count_
              << " updated_region_max_rects="
              << capture_frame_updated_region_max_rect_count_
              << " avg_updated_region_area_ratio="
              << capture_frame_updated_region_area_ratio_total_ / frames
              << " max_updated_region_area_ratio="
              << capture_frame_updated_region_area_ratio_max_
              << " updated_region_full_frames="
              << capture_frame_updated_region_full_frame_count_
              << " updated_region_tiny_frames="
              << capture_frame_updated_region_tiny_frame_count_
              << " frames=" << capture_frame_timing_log_frames_;
      const std::string line = message.str();
      RTC_LOG(LS_INFO) << line;
      AppendNativeWebrtcDiagnosticLine(line);
      capture_frame_timing_log_start_ms_ = timing_now_ms;
      capture_frame_timing_log_frames_ = 0;
      capture_frame_convert_total_us_ = 0;
      capture_frame_scale_total_us_ = 0;
      capture_frame_on_frame_total_us_ = 0;
      capture_frame_callback_total_us_ = 0;
      capture_frame_callback_max_us_ = 0;
      capture_frame_updated_region_empty_count_ = 0;
      capture_frame_updated_region_nonempty_count_ = 0;
      capture_frame_updated_region_rect_count_ = 0;
      capture_frame_updated_region_max_rect_count_ = 0;
      capture_frame_updated_region_area_ratio_total_ = 0.0;
      capture_frame_updated_region_area_ratio_max_ = 0.0;
      capture_frame_updated_region_full_frame_count_ = 0;
      capture_frame_updated_region_tiny_frame_count_ = 0;
      capture_frame_interval_values_us_.clear();
      capture_frame_duplicated_frames_ = 0;
      capture_frame_stale_reuse_count_ = 0;
      capture_frame_wait_timeouts_ = 0;
      capture_frame_permanent_errors_ = 0;
    }
  }
#ifdef WEBRTC_WIN
  __except (filterException(GetExceptionCode(), GetExceptionInformation())) {
  }
#endif
}

void RTCDesktopCapturerImpl::PaceLatestFrame() {
  RTC_DCHECK_RUN_ON(thread_.get());
  if (capture_state_ != CS_RUNNING || !latest_frame_pacing_enabled_) {
    return;
  }

  const int64_t started_us = NowMicros();
  const int64_t started_ms = webrtc::TimeMillis();
  ++latest_paced_log_ticks_;

  bool submitted = false;
  bool unique_frame = false;
  int64_t frame_age_us = 0;
  const auto frame_buffer = latest_paced_frame_buffer_;
  const uint64_t frame_sequence = latest_paced_frame_sequence_;
  const int64_t frame_capture_us = latest_paced_frame_capture_us_;

  if (frame_buffer && frame_capture_us > 0) {
    unique_frame = frame_sequence != last_paced_submitted_sequence_;
    if (unique_frame) {
      last_paced_submitted_sequence_ = frame_sequence;
      ++latest_paced_log_unique_frames_;
    } else {
      ++latest_paced_log_duplicate_frames_;
    }

    frame_age_us = std::max<int64_t>(0, started_us - frame_capture_us);
    latest_paced_log_age_total_us_ += frame_age_us;
    latest_paced_log_age_max_us_ =
        std::max(latest_paced_log_age_max_us_, frame_age_us);

    if (latest_paced_submit_last_us_ > 0) {
      latest_paced_submit_interval_values_us_.push_back(
          std::max<int64_t>(0, started_us - latest_paced_submit_last_us_));
    }
    latest_paced_submit_last_us_ = started_us;

    const int64_t on_frame_started_us = NowMicros();
    OnFrame(webrtc::VideoFrame(frame_buffer, 0, webrtc::TimeMillis(),
                               webrtc::kVideoRotation_0));
    const int64_t on_frame_finished_us = NowMicros();
    const int64_t on_frame_total_us =
        std::max<int64_t>(0, on_frame_finished_us - on_frame_started_us);
    latest_paced_log_on_frame_total_us_ += on_frame_total_us;
    latest_paced_log_on_frame_max_us_ =
        std::max(latest_paced_log_on_frame_max_us_, on_frame_total_us);
    ++latest_paced_log_submitted_frames_;
    submitted = true;
  } else {
    ++latest_paced_log_skipped_ticks_;
  }

  const int64_t finished_us = NowMicros();
  const int64_t finished_ms = webrtc::TimeMillis();
  const int64_t work_ms =
      std::max<int64_t>(0, (finished_us - started_us + 999) / 1000);
  const int64_t target_delay_ms = static_cast<int64_t>(capture_delay_);
  const int64_t next_delay_ms =
      work_ms >= target_delay_ms ? 0 : target_delay_ms - work_ms;

  if (latest_paced_log_start_ms_ <= 0) {
    latest_paced_log_start_ms_ = started_ms;
  }
  const int64_t elapsed_ms = finished_ms - latest_paced_log_start_ms_;
  if (elapsed_ms >= 2000 && latest_paced_log_ticks_ > 0) {
    const double submitted_fps =
        static_cast<double>(latest_paced_log_submitted_frames_) * 1000.0 /
        static_cast<double>(elapsed_ms);
    const double unique_fps =
        static_cast<double>(latest_paced_log_unique_frames_) * 1000.0 /
        static_cast<double>(elapsed_ms);
    const double target_fps =
        capture_delay_ == 0 ? 0.0 : 1000.0 / capture_delay_;
    const double avg_age_ms =
        latest_paced_log_submitted_frames_ == 0
            ? 0.0
            : MicrosToMillis(latest_paced_log_age_total_us_) /
                  static_cast<double>(latest_paced_log_submitted_frames_);
    const double avg_on_frame_ms =
        latest_paced_log_submitted_frames_ == 0
            ? 0.0
            : MicrosToMillis(latest_paced_log_on_frame_total_us_) /
                  static_cast<double>(latest_paced_log_submitted_frames_);
    std::ostringstream message;
    message << "Inter Galactic desktop capture latest-frame pacer cadence"
            << " enabled=true"
            << " target_fps=" << target_fps
            << " submitted_fps=" << submitted_fps
            << " unique_fps=" << unique_fps
            << " p95_interval_ms="
            << PercentileMillis(latest_paced_submit_interval_values_us_, 0.95)
            << " max_interval_ms="
            << MaxMillis(latest_paced_submit_interval_values_us_)
            << " avg_frame_age_ms=" << avg_age_ms
            << " max_frame_age_ms="
            << MicrosToMillis(latest_paced_log_age_max_us_)
            << " avg_on_frame_ms=" << avg_on_frame_ms
            << " max_on_frame_ms="
            << MicrosToMillis(latest_paced_log_on_frame_max_us_)
            << " duplicate_submits="
            << latest_paced_log_duplicate_frames_
            << " overwritten_frames="
            << latest_paced_log_overwritten_frames_
            << " skipped_ticks=" << latest_paced_log_skipped_ticks_
            << " ticks=" << latest_paced_log_ticks_;
    const std::string line = message.str();
    RTC_LOG(LS_INFO) << line;
    AppendNativeWebrtcDiagnosticLine(line);
    latest_paced_log_start_ms_ = finished_ms;
    latest_paced_log_ticks_ = 0;
    latest_paced_log_submitted_frames_ = 0;
    latest_paced_log_unique_frames_ = 0;
    latest_paced_log_duplicate_frames_ = 0;
    latest_paced_log_skipped_ticks_ = 0;
    latest_paced_log_overwritten_frames_ = 0;
    latest_paced_log_age_total_us_ = 0;
    latest_paced_log_age_max_us_ = 0;
    latest_paced_log_on_frame_total_us_ = 0;
    latest_paced_log_on_frame_max_us_ = 0;
    latest_paced_submit_interval_values_us_.clear();
  }

  (void)submitted;
  (void)unique_frame;
  (void)frame_age_us;
  if (capture_state_ == CS_RUNNING && latest_frame_pacing_enabled_) {
    thread_->PostDelayedHighPrecisionTask(
        [this]() { PaceLatestFrame(); },
        webrtc::TimeDelta::Millis(next_delay_ms));
  }
}

void RTCDesktopCapturerImpl::CaptureFrame() {
  RTC_DCHECK_RUN_ON(thread_.get());
  if (capture_state_ != CS_RUNNING) {
    return;
  }

  const int64_t started_us = NowMicros();
  const int64_t started_ms = webrtc::TimeMillis();
  capture_call_active_ = true;
  capture_call_started_us_ = started_us;
  capture_call_current_source_capture_total_us_ = 0;
  capture_call_current_callback_entry_delay_us_ = -1;
  capture_call_current_callback_finished_us_ = 0;
  capture_call_current_callback_total_us_ = 0;
  capture_call_current_callback_max_us_ = 0;
  capture_call_current_callback_count_ = 0;
  capturer_->CaptureFrame();
  capture_call_active_ = false;
  const int64_t finished_us = NowMicros();
  const int64_t finished_ms = webrtc::TimeMillis();
  const int64_t work_ms = std::max<int64_t>(0, finished_ms - started_ms);
  const int64_t callback_us =
      std::max<int64_t>(0, capture_call_current_callback_total_us_);
  const int64_t acquire_wait_us =
      std::max<int64_t>(0, finished_us - started_us - callback_us);
  const int64_t source_capture_us =
      std::max<int64_t>(0, capture_call_current_source_capture_total_us_);
  const int64_t callback_entry_delay_us =
      capture_call_current_callback_entry_delay_us_ < 0
          ? 0
          : capture_call_current_callback_entry_delay_us_;
  const int64_t post_callback_wait_us =
      capture_call_current_callback_finished_us_ <= 0
          ? 0
          : std::max<int64_t>(
                0, finished_us - capture_call_current_callback_finished_us_);
  const int64_t unaccounted_wait_us =
      std::max<int64_t>(
          0, acquire_wait_us - source_capture_us - post_callback_wait_us);
  const int64_t target_delay_ms = static_cast<int64_t>(capture_delay_);
  const int64_t next_delay_ms =
      work_ms >= target_delay_ms ? 0 : target_delay_ms - work_ms;
  capture_schedule_last_delay_ms_ = next_delay_ms;

  if (capture_schedule_log_start_ms_ <= 0) {
    capture_schedule_log_start_ms_ = finished_ms;
    capture_schedule_log_calls_ = 0;
    capture_schedule_log_work_total_ms_ = 0;
    capture_schedule_log_max_work_ms_ = 0;
    capture_schedule_log_source_capture_total_ms_ = 0;
    capture_schedule_log_source_capture_max_ms_ = 0;
    capture_schedule_log_source_capture_count_ = 0;
    capture_schedule_log_callback_total_us_ = 0;
    capture_schedule_log_callback_max_us_ = 0;
    capture_schedule_log_callback_entry_delay_total_us_ = 0;
    capture_schedule_log_callback_entry_delay_max_us_ = 0;
    capture_schedule_log_post_callback_wait_total_us_ = 0;
    capture_schedule_log_post_callback_wait_max_us_ = 0;
    capture_schedule_log_unaccounted_wait_total_us_ = 0;
    capture_schedule_log_unaccounted_wait_max_us_ = 0;
    capture_schedule_log_acquire_wait_total_us_ = 0;
    capture_schedule_log_acquire_wait_max_us_ = 0;
    capture_schedule_log_callback_count_ = 0;
  }
  ++capture_schedule_log_calls_;
  capture_schedule_log_work_total_ms_ += work_ms;
  capture_schedule_log_max_work_ms_ =
      std::max(capture_schedule_log_max_work_ms_, work_ms);
  capture_schedule_log_callback_total_us_ += callback_us;
  capture_schedule_log_callback_max_us_ =
      std::max(capture_schedule_log_callback_max_us_,
               capture_call_current_callback_max_us_);
  capture_schedule_log_callback_entry_delay_total_us_ +=
      callback_entry_delay_us;
  capture_schedule_log_callback_entry_delay_max_us_ =
      std::max(capture_schedule_log_callback_entry_delay_max_us_,
               callback_entry_delay_us);
  capture_schedule_log_post_callback_wait_total_us_ += post_callback_wait_us;
  capture_schedule_log_post_callback_wait_max_us_ =
      std::max(capture_schedule_log_post_callback_wait_max_us_,
               post_callback_wait_us);
  capture_schedule_log_unaccounted_wait_total_us_ += unaccounted_wait_us;
  capture_schedule_log_unaccounted_wait_max_us_ =
      std::max(capture_schedule_log_unaccounted_wait_max_us_,
               unaccounted_wait_us);
  capture_schedule_log_acquire_wait_total_us_ += acquire_wait_us;
  capture_schedule_log_acquire_wait_max_us_ =
      std::max(capture_schedule_log_acquire_wait_max_us_, acquire_wait_us);
  capture_schedule_log_callback_count_ +=
      capture_call_current_callback_count_;
  const int64_t elapsed_ms = finished_ms - capture_schedule_log_start_ms_;
  if (elapsed_ms >= 2000 && capture_schedule_log_calls_ > 0) {
    const double avg_work_ms =
        static_cast<double>(capture_schedule_log_work_total_ms_) /
        static_cast<double>(capture_schedule_log_calls_);
    const double avg_callback_ms =
        MicrosToMillis(capture_schedule_log_callback_total_us_) /
        static_cast<double>(capture_schedule_log_calls_);
    const double avg_acquire_wait_ms =
        MicrosToMillis(capture_schedule_log_acquire_wait_total_us_) /
        static_cast<double>(capture_schedule_log_calls_);
    const double avg_callback_entry_delay_ms =
        MicrosToMillis(capture_schedule_log_callback_entry_delay_total_us_) /
        static_cast<double>(capture_schedule_log_calls_);
    const double avg_post_callback_wait_ms =
        MicrosToMillis(capture_schedule_log_post_callback_wait_total_us_) /
        static_cast<double>(capture_schedule_log_calls_);
    const double avg_unaccounted_wait_ms =
        MicrosToMillis(capture_schedule_log_unaccounted_wait_total_us_) /
        static_cast<double>(capture_schedule_log_calls_);
    const double avg_source_capture_ms =
        capture_schedule_log_source_capture_count_ == 0
            ? 0.0
            : static_cast<double>(
                  capture_schedule_log_source_capture_total_ms_) /
                  static_cast<double>(
                      capture_schedule_log_source_capture_count_);
    const double submitted_fps =
        static_cast<double>(capture_schedule_log_calls_) * 1000.0 /
        static_cast<double>(elapsed_ms);
    std::ostringstream message;
    message << "Inter Galactic desktop capture cadence target_delay_ms="
            << target_delay_ms << " avg_capture_call_ms=" << avg_work_ms
            << " max_capture_call_ms=" << capture_schedule_log_max_work_ms_
            << " scheduled_delay_ms=" << capture_schedule_last_delay_ms_
            << " calls=" << capture_schedule_log_calls_
            << " submitted_fps=" << submitted_fps
            << " temp_errors=" << capture_schedule_log_temp_errors_
            << " permanent_errors=" << capture_schedule_log_permanent_errors_
            << " avg_source_capture_ms=" << avg_source_capture_ms
            << " max_source_capture_ms="
            << capture_schedule_log_source_capture_max_ms_
            << " source_capture_count="
            << capture_schedule_log_source_capture_count_
            << " avg_callback_entry_delay_ms="
            << avg_callback_entry_delay_ms
            << " max_callback_entry_delay_ms="
            << MicrosToMillis(
                   capture_schedule_log_callback_entry_delay_max_us_)
            << " avg_result_callback_ms=" << avg_callback_ms
            << " max_result_callback_ms="
            << MicrosToMillis(capture_schedule_log_callback_max_us_)
            << " avg_acquire_wait_ms=" << avg_acquire_wait_ms
            << " max_acquire_wait_ms="
            << MicrosToMillis(capture_schedule_log_acquire_wait_max_us_)
            << " avg_post_callback_wait_ms=" << avg_post_callback_wait_ms
            << " max_post_callback_wait_ms="
            << MicrosToMillis(
                   capture_schedule_log_post_callback_wait_max_us_)
            << " avg_unaccounted_wait_ms=" << avg_unaccounted_wait_ms
            << " max_unaccounted_wait_ms="
            << MicrosToMillis(
                   capture_schedule_log_unaccounted_wait_max_us_)
            << " callback_count=" << capture_schedule_log_callback_count_;
    const std::string line = message.str();
    RTC_LOG(LS_INFO) << line;
    AppendNativeWebrtcDiagnosticLine(line);
    capture_schedule_log_start_ms_ = finished_ms;
    capture_schedule_log_calls_ = 0;
    capture_schedule_log_work_total_ms_ = 0;
    capture_schedule_log_max_work_ms_ = 0;
    capture_schedule_log_source_capture_total_ms_ = 0;
    capture_schedule_log_source_capture_max_ms_ = 0;
    capture_schedule_log_source_capture_count_ = 0;
    capture_schedule_log_callback_total_us_ = 0;
    capture_schedule_log_callback_max_us_ = 0;
    capture_schedule_log_callback_entry_delay_total_us_ = 0;
    capture_schedule_log_callback_entry_delay_max_us_ = 0;
    capture_schedule_log_post_callback_wait_total_us_ = 0;
    capture_schedule_log_post_callback_wait_max_us_ = 0;
    capture_schedule_log_unaccounted_wait_total_us_ = 0;
    capture_schedule_log_unaccounted_wait_max_us_ = 0;
    capture_schedule_log_acquire_wait_total_us_ = 0;
    capture_schedule_log_acquire_wait_max_us_ = 0;
    capture_schedule_log_callback_count_ = 0;
    capture_schedule_log_temp_errors_ = 0;
    capture_schedule_log_permanent_errors_ = 0;
  }

  if (capture_state_ == CS_RUNNING) {
    thread_->PostDelayedHighPrecisionTask(
        [this]() { CaptureFrame(); },
        webrtc::TimeDelta::Millis(next_delay_ms));
  }
}

}  // namespace libwebrtc
