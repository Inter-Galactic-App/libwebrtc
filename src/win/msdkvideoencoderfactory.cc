// Copyright (C) <2018> Intel Corporation
//
// SPDX-License-Identifier: Apache-2.0

#include "src/win/msdkvideoencoderfactory.h"

#include <string>

#include "absl/strings/match.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/video_encoder_software_fallback_wrapper.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "modules/video_coding/codecs/vp9/include/vp9.h"
#include "src/win/codecutils.h"
#include "src/win/msdkvideoencoder.h"
// #include "common_video/h264/profile_level_id.h"
// #include "media/base/vp9_profile.h"

namespace owt {
namespace base {

MSDKVideoEncoderFactory::MSDKVideoEncoderFactory() {
  supported_codec_types_.clear();
  MediaCapabilities* media_capability = MediaCapabilities::Get();
  std::vector<owt::base::VideoCodec> codecs_to_check;
  codecs_to_check.push_back(owt::base::VideoCodec::kH264);
#if 0
  codecs_to_check.push_back(owt::base::VideoCodec::kVp9);
  codecs_to_check.push_back(owt::base::VideoCodec::kAv1);
  codecs_to_check.push_back(owt::base::VideoCodec::kVp8);
#endif

  std::vector<VideoEncoderCapability> capabilities =
      media_capability->SupportedCapabilitiesForVideoEncoder(codecs_to_check);
  // TODO(jianlin): use the check result from MSDK.
  supported_codec_types_.push_back(webrtc::kVideoCodecH264);
  supported_codec_types_.push_back(webrtc::kVideoCodecVP8);
  supported_codec_types_.push_back(webrtc::kVideoCodecVP9);
  supported_codec_types_.push_back(webrtc::kVideoCodecAV1);
}

std::unique_ptr<webrtc::VideoEncoder>
MSDKVideoEncoderFactory::Create(
    const webrtc::Environment& env,
    const webrtc::SdpVideoFormat& format) {
  auto builtin_factory = webrtc::CreateBuiltinVideoEncoderFactory();
  if (!absl::EqualsIgnoreCase(format.name, webrtc::kH264CodecName)) {
    return builtin_factory->Create(env, format);
  }

  webrtc::VideoCodec codec;
  codec.codecType = owt::base::CodecUtils::ConvertSdpFormatToCodecType(format);
  auto hardware_encoder = MSDKVideoEncoder::Create(codec);
  auto software_encoder = builtin_factory->Create(env, format);
  if (!software_encoder) {
    return hardware_encoder;
  }
  return webrtc::CreateVideoEncoderSoftwareFallbackWrapper(
      env, std::move(software_encoder), std::move(hardware_encoder),
      /*prefer_temporal_support=*/false);
}

std::vector<webrtc::SdpVideoFormat>
MSDKVideoEncoderFactory::GetSupportedFormats() const {
  std::vector<webrtc::SdpVideoFormat> supported_codecs;
  // TODO: We should combine the codec profiles that hardware H.264 encoder
  // supports with those provided by built-in H.264 encoder
  for (const webrtc::SdpVideoFormat& format :
       owt::base::CodecUtils::SupportedH264Codecs())
    supported_codecs.push_back(format);

  const auto builtin_formats =
      webrtc::CreateBuiltinVideoEncoderFactory()->GetSupportedFormats();
  for (const webrtc::SdpVideoFormat& format : builtin_formats) {
    if (!format.IsCodecInList(supported_codecs)) {
      supported_codecs.push_back(format);
    }
  }

  return supported_codecs;
}

webrtc::VideoEncoderFactory::CodecSupport
MSDKVideoEncoderFactory::QueryCodecSupport(
    const webrtc::SdpVideoFormat& format,
    std::optional<std::string> scalability_mode) const {
  if (absl::EqualsIgnoreCase(format.name, webrtc::kH264CodecName) &&
      format.IsCodecInList(GetSupportedFormats())) {
    return {.is_supported = true, .is_power_efficient = true};
  }
  return webrtc::CreateBuiltinVideoEncoderFactory()->QueryCodecSupport(
      format, scalability_mode);
}

}  // namespace base
}  // namespace owt
