#ifndef LIBWEBRTC_WIN_INTERGALACTIC_GAME_CAPTURE_VIDEO_CAPTURER_H_
#define LIBWEBRTC_WIN_INTERGALACTIC_GAME_CAPTURE_VIDEO_CAPTURER_H_

#include <cstdint>
#include <memory>

#include "rtc_base/thread.h"
#include "src/internal/video_capturer.h"

namespace libwebrtc {

std::shared_ptr<webrtc::internal::VideoCapturer>
CreateIntergalacticGameCaptureVideoCapturer(webrtc::Thread* worker_thread,
                                            const char* helper_path,
                                            uint32_t target_process_id,
                                            size_t max_width,
                                            size_t max_height,
                                            size_t target_fps,
                                            const char* source_mode = nullptr);

}  // namespace libwebrtc

#endif  // LIBWEBRTC_WIN_INTERGALACTIC_GAME_CAPTURE_VIDEO_CAPTURER_H_
