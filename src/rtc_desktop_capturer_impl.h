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

#ifndef LIBWEBRTC_RTC_DESKTOP_CAPTURER_IMPL_HXX
#define LIBWEBRTC_RTC_DESKTOP_CAPTURER_IMPL_HXX

#include <cstdint>
#include <string>
#include <vector>

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "include/rtc_desktop_capturer.h"
#include "include/rtc_types.h"
#include "modules/desktop_capture/desktop_and_cursor_composer.h"
#include "modules/desktop_capture/desktop_capture_options.h"
#include "modules/desktop_capture/desktop_capturer.h"
#include "modules/desktop_capture/desktop_frame.h"
#include "rtc_base/thread.h"
#include "src/internal/vcm_capturer.h"
#include "src/internal/video_capturer.h"

namespace libwebrtc {

class RTCDesktopCapturerImpl : public RTCDesktopCapturer,
                               public webrtc::DesktopCapturer::Callback,
                               public webrtc::internal::VideoCapturer {
 public:
  RTCDesktopCapturerImpl(DesktopType type,
                         webrtc::DesktopCapturer::SourceId source_id,
                         webrtc::Thread* signaling_thread,
                         scoped_refptr<MediaSource> source, bool showCursor = true);
  ~RTCDesktopCapturerImpl();

  void RegisterDesktopCapturerObserver(
      DesktopCapturerObserver* observer) override {
    observer_ = observer;
  }

  void DeRegisterDesktopCapturerObserver() override { observer_ = nullptr; }
  CaptureState Start(uint32_t fps) override;

  CaptureState Start(uint32_t fps, uint32_t x, uint32_t y, uint32_t w,
                     uint32_t h) override;

  CaptureState StartWithMaxFrameSize(uint32_t fps, uint32_t max_w,
                                     uint32_t max_h) override;

  void SetWindowsCaptureBackendMode(const char* mode) override;

  void SetWindowsCaptureDirtyRegionMode(const char* mode) override;

  void SetWindowsWindowGdiCaptureMode(const char* mode) override;

  void SetLatestFramePacingEnabled(bool enabled) override;

  void Stop() override;

  bool IsRunning() override;

  scoped_refptr<MediaSource> source() override { return source_; }

 protected:
  virtual void OnCaptureResult(
      webrtc::DesktopCapturer::Result result,
      std::unique_ptr<webrtc::DesktopFrame> frame) override;

 private:
  void CaptureFrame();
  void PaceLatestFrame();
  void CreateDesktopCapturerOnThread();
  void ConfigureWindowsCaptureBackendMode(const std::string& mode);
  void ResetDesktopCaptureOptionsForCurrentModes();
  void ResetLatestFramePacerState();
  webrtc::DesktopCaptureOptions options_;
  std::unique_ptr<webrtc::DesktopCapturer> capturer_;
  std::unique_ptr<webrtc::Thread> thread_;
  webrtc::scoped_refptr<webrtc::I420Buffer> i420_buffer_;
  webrtc::scoped_refptr<webrtc::I420Buffer> i420_full_frame_buffer_;
  CaptureState capture_state_ = CS_STOPPED;
  DesktopType type_;
  webrtc::DesktopCapturer::SourceId source_id_;
  DesktopCapturerObserver* observer_ = nullptr;
  bool show_cursor_ = true;
  std::string windows_capture_backend_mode_ = "default";
  std::string windows_capture_dirty_region_mode_ = "auto";
  std::string windows_window_gdi_capture_mode_ = "default";
  bool force_full_frame_dirty_region_mode_ = false;
  bool latest_frame_pacing_enabled_ = false;
  uint32_t capture_delay_ = 1000;  // 1s
  webrtc::DesktopCapturer::Result result_ =
      webrtc::DesktopCapturer::Result::SUCCESS;
  webrtc::Thread* signaling_thread_ = nullptr;
  scoped_refptr<MediaSource> source_;
  uint32_t x_ = 0;
  uint32_t y_ = 0;
  uint32_t w_ = 0;
  uint32_t h_ = 0;
  uint32_t max_frame_width_ = 0;
  uint32_t max_frame_height_ = 0;
  int last_logged_source_width_ = 0;
  int last_logged_source_height_ = 0;
  int last_logged_window_rect_width_ = 0;
  int last_logged_window_rect_height_ = 0;
  uint32_t last_logged_max_frame_width_ = 0;
  uint32_t last_logged_max_frame_height_ = 0;
  int last_logged_output_width_ = 0;
  int last_logged_output_height_ = 0;
  int last_logged_content_width_ = 0;
  int last_logged_content_height_ = 0;
  bool last_logged_fixed_canvas_ = false;
  bool last_logged_crop_region_ = false;
  uint32_t last_logged_capturer_id_ = 0;
  int64_t capture_pipeline_log_start_ms_ = 0;
  uint32_t capture_pipeline_log_frames_ = 0;
  int64_t capture_schedule_log_start_ms_ = 0;
  uint32_t capture_schedule_log_calls_ = 0;
  int64_t capture_schedule_log_work_total_ms_ = 0;
  int64_t capture_schedule_log_max_work_ms_ = 0;
  int64_t capture_schedule_log_source_capture_total_ms_ = 0;
  int64_t capture_schedule_log_source_capture_max_ms_ = 0;
  uint32_t capture_schedule_log_source_capture_count_ = 0;
  int64_t capture_schedule_log_callback_total_us_ = 0;
  int64_t capture_schedule_log_callback_max_us_ = 0;
  int64_t capture_schedule_log_callback_entry_delay_total_us_ = 0;
  int64_t capture_schedule_log_callback_entry_delay_max_us_ = 0;
  int64_t capture_schedule_log_post_callback_wait_total_us_ = 0;
  int64_t capture_schedule_log_post_callback_wait_max_us_ = 0;
  int64_t capture_schedule_log_unaccounted_wait_total_us_ = 0;
  int64_t capture_schedule_log_unaccounted_wait_max_us_ = 0;
  int64_t capture_schedule_log_acquire_wait_total_us_ = 0;
  int64_t capture_schedule_log_acquire_wait_max_us_ = 0;
  uint32_t capture_schedule_log_callback_count_ = 0;
  int64_t capture_schedule_last_delay_ms_ = 0;
  uint32_t capture_schedule_log_temp_errors_ = 0;
  uint32_t capture_schedule_log_permanent_errors_ = 0;
  bool capture_call_active_ = false;
  int64_t capture_call_started_us_ = 0;
  int64_t capture_call_current_source_capture_total_us_ = 0;
  int64_t capture_call_current_callback_entry_delay_us_ = -1;
  int64_t capture_call_current_callback_finished_us_ = 0;
  int64_t capture_call_current_callback_total_us_ = 0;
  int64_t capture_call_current_callback_max_us_ = 0;
  uint32_t capture_call_current_callback_count_ = 0;
  int64_t capture_frame_timing_log_start_ms_ = 0;
  uint32_t capture_frame_timing_log_frames_ = 0;
  int64_t capture_frame_last_success_us_ = 0;
  std::vector<int64_t> capture_frame_interval_values_us_;
  uint32_t capture_frame_duplicated_frames_ = 0;
  uint32_t capture_frame_stale_reuse_count_ = 0;
  uint32_t capture_frame_wait_timeouts_ = 0;
  uint32_t capture_frame_permanent_errors_ = 0;
  int64_t capture_frame_convert_total_us_ = 0;
  int64_t capture_frame_scale_total_us_ = 0;
  int64_t capture_frame_on_frame_total_us_ = 0;
  int64_t capture_frame_callback_total_us_ = 0;
  int64_t capture_frame_callback_max_us_ = 0;
  uint32_t capture_frame_updated_region_empty_count_ = 0;
  uint32_t capture_frame_updated_region_nonempty_count_ = 0;
  uint32_t capture_frame_updated_region_rect_count_ = 0;
  uint32_t capture_frame_updated_region_max_rect_count_ = 0;
  double capture_frame_updated_region_area_ratio_total_ = 0.0;
  double capture_frame_updated_region_area_ratio_max_ = 0.0;
  uint32_t capture_frame_updated_region_full_frame_count_ = 0;
  uint32_t capture_frame_updated_region_tiny_frame_count_ = 0;
  int64_t capture_frame_updated_region_total_us_ = 0;
  int64_t capture_frame_updated_region_max_us_ = 0;
  webrtc::scoped_refptr<webrtc::I420Buffer> latest_paced_frame_buffer_;
  uint64_t latest_paced_frame_sequence_ = 0;
  uint64_t last_paced_submitted_sequence_ = 0;
  int64_t latest_paced_frame_capture_us_ = 0;
  int64_t latest_paced_submit_last_us_ = 0;
  int64_t latest_paced_log_start_ms_ = 0;
  uint32_t latest_paced_log_ticks_ = 0;
  uint32_t latest_paced_log_submitted_frames_ = 0;
  uint32_t latest_paced_log_unique_frames_ = 0;
  uint32_t latest_paced_log_duplicate_frames_ = 0;
  uint32_t latest_paced_log_skipped_ticks_ = 0;
  uint32_t latest_paced_log_overwritten_frames_ = 0;
  int64_t latest_paced_log_age_total_us_ = 0;
  int64_t latest_paced_log_age_max_us_ = 0;
  int64_t latest_paced_log_on_frame_total_us_ = 0;
  int64_t latest_paced_log_on_frame_max_us_ = 0;
  std::vector<int64_t> latest_paced_submit_interval_values_us_;
};

}  // namespace libwebrtc

#endif  // LIBWEBRTC_RTC_DESKTOP_CAPTURER_IMPL_HXX
