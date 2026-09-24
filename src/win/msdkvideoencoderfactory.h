// Copyright (C) <2018> Intel Corporation
//
// SPDX-License-Identifier: Apache-2.0

#ifndef OWT_BASE_WIN_MSDKVIDEOENCODER_FACTORY_H_
#define OWT_BASE_WIN_MSDKVIDEOENCODER_FACTORY_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "api/environment/environment.h"
#include "api/video/video_codec_type.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "src/win/mediacapabilities.h"

namespace owt {
namespace base {
class MSDKVideoEncoderFactory : public webrtc::VideoEncoderFactory {
 public:
  MSDKVideoEncoderFactory();
  ~MSDKVideoEncoderFactory() {}
  std::unique_ptr<webrtc::VideoEncoder> Create(
      const webrtc::Environment& env,
      const webrtc::SdpVideoFormat& format) override;

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;

  CodecSupport QueryCodecSupport(
      const webrtc::SdpVideoFormat& format,
      std::optional<std::string> scalability_mode) const override;

 private:
  // Built-in simulcast encoders retain a non-owning pointer into this factory.
  std::unique_ptr<webrtc::VideoEncoderFactory> builtin_factory_;
  std::vector<webrtc::VideoCodecType> supported_codec_types_;
};
}  // namespace base
}  // namespace owt
#endif  // OWT_BASE_WIN_MSDKVIDEOENCODER_FACTORY_H_
