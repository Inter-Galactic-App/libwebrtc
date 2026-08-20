// Copyright (C) <2018> Intel Corporation
//
// SPDX-License-Identifier: Apache-2.0
#ifndef OWT_NATIVE_HANDLE_BUFFER_H_
#define OWT_NATIVE_HANDLE_BUFFER_H_
#include "api/scoped_refptr.h"
#include "api/video/video_frame_buffer.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"

namespace owt {
namespace base {
using namespace webrtc;

enum class NativeHandleBufferKind {
  kGeneric,
  kIntergalacticD3D11Nv12,
};

class NativeHandleBuffer : public VideoFrameBuffer {
 public:
  NativeHandleBuffer(void* native_handle, int width, int height)
      : native_handle_(native_handle), width_(width), height_(height) {}
  Type type() const override { return Type::kNative; }
  int width() const override { return width_; }
  int height() const override { return height_; }
  virtual NativeHandleBufferKind native_handle_kind() const {
    return NativeHandleBufferKind::kGeneric;
  }
  webrtc::scoped_refptr<I420BufferInterface> ToI420() override {
    RTC_LOG(LS_WARNING)
        << "NativeHandleBuffer: ToI420 requested for unsupported native "
           "handle";
    return nullptr;
  }

  void* native_handle() { return native_handle_; }

 private:
  void* native_handle_;
  const int width_;
  const int height_;
};
}  // namespace base
}  // namespace owt
#endif  // OWT_NATIVE_HANDLE_BUFFER_H_
