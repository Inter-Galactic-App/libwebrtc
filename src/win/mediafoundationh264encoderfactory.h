// Copyright (C) 2026 Inter Galactic contributors
//
// SPDX-License-Identifier: Apache-2.0

#ifndef OWT_BASE_WIN_MEDIAFOUNDATIONH264ENCODERFACTORY_H_
#define OWT_BASE_WIN_MEDIAFOUNDATIONH264ENCODERFACTORY_H_

#include <memory>

#include "api/video_codecs/video_encoder_factory.h"

namespace owt {
namespace base {

bool IsMediaFoundationH264HardwareEncoderAvailable();

std::unique_ptr<webrtc::VideoEncoderFactory>
CreateMediaFoundationH264EncoderFactory();

}  // namespace base
}  // namespace owt

#endif  // OWT_BASE_WIN_MEDIAFOUNDATIONH264ENCODERFACTORY_H_
