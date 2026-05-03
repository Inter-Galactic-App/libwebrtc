// Copyright (C) 2026 Inter Galactic contributors
//
// SPDX-License-Identifier: Apache-2.0

#include "src/win/mediafoundationh264encoderfactory.h"

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <windows.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_buffer.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/video_encoder_software_fallback_wrapper.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv/convert_from.h"

namespace owt {
namespace base {
namespace {

using Microsoft::WRL::ComPtr;

constexpr int kLowH264QpThreshold = 24;
constexpr int kHighH264QpThreshold = 37;
constexpr DWORD kInputStreamId = 0;
constexpr DWORD kOutputStreamId = 0;

std::string HrToString(HRESULT hr) {
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "0x%08lx", static_cast<unsigned long>(hr));
  return std::string(buffer);
}

std::string Narrow(LPCWSTR value) {
  if (!value) {
    return {};
  }
  const int size =
      WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) {
    return {};
  }
  std::string result(static_cast<size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr,
                      nullptr);
  return result;
}

HRESULT EnsureMediaFoundationStarted() {
  static std::once_flag start_once;
  static HRESULT start_result = E_FAIL;
  std::call_once(
      start_once, [] {
        const HRESULT co_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(co_result) && co_result != RPC_E_CHANGED_MODE) {
          RTC_LOG(LS_WARNING)
              << "Inter Galactic: CoInitializeEx for Media Foundation returned "
              << HrToString(co_result);
        }
        start_result = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
        if (FAILED(start_result)) {
          RTC_LOG(LS_ERROR) << "Inter Galactic: MFStartup failed "
                            << HrToString(start_result);
        }
      });
  return start_result;
}

bool IsH264(const webrtc::SdpVideoFormat& format) {
  return absl::EqualsIgnoreCase(format.name, "H264");
}

void ReleaseActivates(IMFActivate** activates, UINT32 count,
                      IMFActivate* keep) {
  if (!activates) {
    return;
  }
  for (UINT32 i = 0; i < count; ++i) {
    if (activates[i] && activates[i] != keep) {
      activates[i]->Release();
    }
  }
  CoTaskMemFree(activates);
}

HRESULT EnumerateHardwareH264Encoder(ComPtr<IMFActivate>* activate) {
  const HRESULT startup_result = EnsureMediaFoundationStarted();
  if (FAILED(startup_result)) {
    return startup_result;
  }

  MFT_REGISTER_TYPE_INFO input_type = {};
  input_type.guidMajorType = MFMediaType_Video;
  input_type.guidSubtype = MFVideoFormat_NV12;

  MFT_REGISTER_TYPE_INFO output_type = {};
  output_type.guidMajorType = MFMediaType_Video;
  output_type.guidSubtype = MFVideoFormat_H264;

  IMFActivate** activates = nullptr;
  UINT32 activate_count = 0;
  const HRESULT enum_result =
      MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                &input_type, &output_type, &activates, &activate_count);
  if (FAILED(enum_result)) {
    RTC_LOG(LS_WARNING)
        << "Inter Galactic: hardware Media Foundation H.264 MFT enumeration "
           "failed "
        << HrToString(enum_result);
    return enum_result;
  }
  if (activate_count == 0) {
    RTC_LOG(LS_WARNING)
        << "Inter Galactic: no hardware Media Foundation H.264 MFT found";
    ReleaseActivates(activates, activate_count, nullptr);
    return MF_E_TOPO_CODEC_NOT_FOUND;
  }

  for (UINT32 i = 0; i < activate_count; ++i) {
    LPWSTR friendly_name = nullptr;
    UINT32 name_length = 0;
    if (activates[i] &&
        SUCCEEDED(activates[i]->GetAllocatedString(
            MFT_FRIENDLY_NAME_Attribute, &friendly_name, &name_length))) {
      RTC_LOG(LS_INFO)
          << "Inter Galactic: hardware Media Foundation H.264 MFT candidate: "
          << Narrow(friendly_name);
      CoTaskMemFree(friendly_name);
    }
  }

  if (activate) {
    activate->Attach(activates[0]);
    ReleaseActivates(activates, activate_count, activates[0]);
  } else {
    ReleaseActivates(activates, activate_count, nullptr);
  }
  return S_OK;
}

bool HasAnnexBStartCode(const uint8_t* data, size_t size) {
  if (!data || size < 4) {
    return false;
  }
  for (size_t i = 0; i + 3 < size; ++i) {
    if (data[i] == 0 && data[i + 1] == 0 &&
        ((data[i + 2] == 1) ||
         (i + 4 < size && data[i + 2] == 0 && data[i + 3] == 1))) {
      return true;
    }
  }
  return false;
}

bool AppendLengthPrefixedAsAnnexB(const uint8_t* data, size_t size,
                                  std::vector<uint8_t>* output) {
  size_t offset = 0;
  std::vector<uint8_t> converted;
  while (offset + 4 <= size) {
    const uint32_t nalu_size = (static_cast<uint32_t>(data[offset]) << 24) |
                               (static_cast<uint32_t>(data[offset + 1]) << 16) |
                               (static_cast<uint32_t>(data[offset + 2]) << 8) |
                               static_cast<uint32_t>(data[offset + 3]);
    offset += 4;
    if (nalu_size == 0 || nalu_size > size - offset) {
      return false;
    }
    const uint8_t start_code[] = {0, 0, 0, 1};
    converted.insert(converted.end(), start_code,
                     start_code + sizeof(start_code));
    converted.insert(converted.end(), data + offset, data + offset + nalu_size);
    offset += nalu_size;
  }
  if (offset != size || converted.empty()) {
    return false;
  }
  output->insert(output->end(), converted.begin(), converted.end());
  return true;
}

void AppendAnnexB(const uint8_t* data, size_t size,
                  std::vector<uint8_t>* output, bool* warned_unknown_layout) {
  if (!data || size == 0) {
    return;
  }
  if (HasAnnexBStartCode(data, size)) {
    output->insert(output->end(), data, data + size);
    return;
  }
  if (AppendLengthPrefixedAsAnnexB(data, size, output)) {
    return;
  }
  if (warned_unknown_layout && !*warned_unknown_layout) {
    RTC_LOG(LS_WARNING)
        << "Inter Galactic: Media Foundation H.264 output did not look like "
           "Annex B or 4-byte length-prefixed NAL units; forwarding bytes "
           "unchanged";
    *warned_unknown_layout = true;
  }
  output->insert(output->end(), data, data + size);
}

bool CopySampleBytes(IMFSample* sample, std::vector<uint8_t>* bytes) {
  if (!sample || !bytes) {
    return false;
  }
  ComPtr<IMFMediaBuffer> contiguous_buffer;
  HRESULT hr = sample->ConvertToContiguousBuffer(&contiguous_buffer);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "Inter Galactic: ConvertToContiguousBuffer failed "
                        << HrToString(hr);
    return false;
  }

  BYTE* data = nullptr;
  DWORD max_length = 0;
  DWORD current_length = 0;
  hr = contiguous_buffer->Lock(&data, &max_length, &current_length);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "Inter Galactic: IMFMediaBuffer::Lock failed "
                        << HrToString(hr);
    return false;
  }
  bytes->assign(data, data + current_length);
  contiguous_buffer->Unlock();
  return true;
}

bool GetSequenceHeader(IMFMediaType* output_type,
                       std::vector<uint8_t>* sequence_header) {
  if (!output_type || !sequence_header) {
    return false;
  }
  UINT32 header_size = 0;
  if (FAILED(
          output_type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &header_size)) ||
      header_size == 0) {
    return false;
  }
  sequence_header->resize(header_size);
  UINT32 copied = 0;
  if (FAILED(output_type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER,
                                  sequence_header->data(), header_size,
                                  &copied)) ||
      copied == 0) {
    sequence_header->clear();
    return false;
  }
  sequence_header->resize(copied);
  return true;
}

HRESULT SetCodecApiUint32(IMFTransform* transform, const GUID& property,
                          ULONG value, const char* label) {
  ComPtr<ICodecAPI> codec_api;
  HRESULT hr = transform->QueryInterface(IID_PPV_ARGS(&codec_api));
  if (FAILED(hr)) {
    return hr;
  }
  VARIANT variant;
  VariantInit(&variant);
  variant.vt = VT_UI4;
  variant.ulVal = value;
  hr = codec_api->SetValue(&property, &variant);
  VariantClear(&variant);
  if (FAILED(hr)) {
    RTC_LOG(LS_INFO) << "Inter Galactic: Media Foundation H.264 did not accept "
                     << label << " (" << HrToString(hr) << ")";
  }
  return hr;
}

HRESULT SetCodecApiBool(IMFTransform* transform, const GUID& property,
                        bool value, const char* label) {
  ComPtr<ICodecAPI> codec_api;
  HRESULT hr = transform->QueryInterface(IID_PPV_ARGS(&codec_api));
  if (FAILED(hr)) {
    return hr;
  }
  VARIANT variant;
  VariantInit(&variant);
  variant.vt = VT_BOOL;
  variant.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
  hr = codec_api->SetValue(&property, &variant);
  VariantClear(&variant);
  if (FAILED(hr)) {
    RTC_LOG(LS_INFO) << "Inter Galactic: Media Foundation H.264 did not accept "
                     << label << " (" << HrToString(hr) << ")";
  }
  return hr;
}

struct FrameMetadata {
  uint32_t rtp_timestamp = 0;
  int64_t ntp_time_ms = 0;
  std::optional<webrtc::ColorSpace> color_space;
  bool key_frame_requested = false;
};

class MediaFoundationH264Encoder final : public webrtc::VideoEncoder {
 public:
  explicit MediaFoundationH264Encoder(webrtc::H264EncoderSettings h264_settings)
      : packetization_mode_(h264_settings.packetization_mode) {}

  ~MediaFoundationH264Encoder() override { Release(); }

  int32_t InitEncode(const webrtc::VideoCodec* codec_settings,
                     const webrtc::VideoEncoder::Settings& settings) override {
    if (!codec_settings ||
        codec_settings->codecType != webrtc::kVideoCodecH264) {
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    }
    if (codec_settings->width == 0 || codec_settings->height == 0 ||
        codec_settings->maxFramerate == 0) {
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    }
    if (codec_settings->numberOfSimulcastStreams > 1) {
      RTC_LOG(LS_WARNING)
          << "Inter Galactic: Media Foundation H.264 encoder is single-layer; "
             "falling back for simulcast";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    Release();
    codec_ = *codec_settings;
    width_ = codec_settings->width;
    height_ = codec_settings->height;
    fps_ = std::max<uint32_t>(1, codec_settings->maxFramerate);
    bitrate_bps_ = std::max<uint32_t>(1, codec_settings->startBitrate) * 1000;
    max_payload_size_ = settings.max_payload_size;
    sample_duration_hns_ = 10000000LL / fps_;

    const HRESULT hr = InitializeTransform();
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING)
          << "Inter Galactic: Media Foundation H.264 init failed "
          << HrToString(hr) << "; software fallback can continue";
      Release();
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    RTC_LOG(LS_INFO) << "Inter Galactic: Media Foundation H.264 encoder "
                     << "initialized " << width_ << "x" << height_ << "@"
                     << fps_ << " bitrate=" << bitrate_bps_;
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t RegisterEncodeCompleteCallback(
      webrtc::EncodedImageCallback* callback) override {
    encoded_image_callback_ = callback;
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t Release() override {
    if (transform_) {
      transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
      transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    transform_.Reset();
    output_type_.Reset();
    metadata_queue_.clear();
    nv12_buffer_.clear();
    warned_unknown_layout_ = false;
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t Encode(
      const webrtc::VideoFrame& input_frame,
      const std::vector<webrtc::VideoFrameType>* frame_types) override {
    if (!transform_) {
      return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    }
    if (!encoded_image_callback_) {
      return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    }

    webrtc::scoped_refptr<webrtc::I420BufferInterface> frame_buffer =
        input_frame.video_frame_buffer()->ToI420();
    if (!frame_buffer) {
      RTC_LOG(LS_WARNING)
          << "Inter Galactic: Media Foundation H.264 could not convert input "
             "frame to I420";
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }
    if (static_cast<uint32_t>(frame_buffer->width()) != width_ ||
        static_cast<uint32_t>(frame_buffer->height()) != height_) {
      RTC_LOG(LS_WARNING)
          << "Inter Galactic: Media Foundation H.264 frame size changed from "
          << width_ << "x" << height_ << " to " << frame_buffer->width() << "x"
          << frame_buffer->height() << "; software fallback can continue";
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }

    const bool key_frame_requested =
        frame_types && !frame_types->empty() &&
        (*frame_types)[0] == webrtc::VideoFrameType::kVideoFrameKey;
    if (key_frame_requested) {
      ForceKeyFrame();
    }

    if (!ConvertToNv12(*frame_buffer)) {
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }

    ComPtr<IMFSample> sample;
    HRESULT hr = CreateInputSample(input_frame, &sample);
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING)
          << "Inter Galactic: Media Foundation H.264 input sample failed "
          << HrToString(hr);
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }

    hr = transform_->ProcessInput(kInputStreamId, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
      DrainOutput(input_frame);
      hr = transform_->ProcessInput(kInputStreamId, sample.Get(), 0);
    }
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING)
          << "Inter Galactic: Media Foundation H.264 ProcessInput failed "
          << HrToString(hr);
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }

    FrameMetadata metadata;
    metadata.rtp_timestamp = input_frame.rtp_timestamp();
    metadata.ntp_time_ms = input_frame.ntp_time_ms();
    metadata.color_space = input_frame.color_space();
    metadata.key_frame_requested = key_frame_requested;
    metadata_queue_.push_back(std::move(metadata));

    return DrainOutput(input_frame);
  }

  void SetRates(const RateControlParameters& parameters) override {
    const uint32_t bitrate_bps =
        static_cast<uint32_t>(parameters.bitrate.get_sum_bps());
    if (bitrate_bps == 0) {
      return;
    }
    bitrate_bps_ = bitrate_bps;
    if (transform_) {
      SetCodecApiUint32(transform_.Get(), CODECAPI_AVEncCommonMeanBitRate,
                        bitrate_bps_, "mean bitrate");
    }
  }

  EncoderInfo GetEncoderInfo() const override {
    EncoderInfo info;
    info.supports_native_handle = false;
    info.implementation_name = "MediaFoundationH264";
    info.scaling_settings = webrtc::VideoEncoder::ScalingSettings(
        kLowH264QpThreshold, kHighH264QpThreshold);
    info.is_hardware_accelerated = true;
    info.supports_simulcast = false;
    info.requested_resolution_alignment = 2;
    info.apply_alignment_to_all_simulcast_layers = true;
    info.preferred_pixel_formats = {webrtc::VideoFrameBuffer::Type::kI420};
    return info;
  }

 private:
  HRESULT InitializeTransform() {
    ComPtr<IMFActivate> activate;
    HRESULT hr = EnumerateHardwareH264Encoder(&activate);
    if (FAILED(hr)) {
      return hr;
    }
    hr = activate->ActivateObject(IID_PPV_ARGS(&transform_));
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING)
          << "Inter Galactic: Media Foundation H.264 activation failed "
          << HrToString(hr);
      return hr;
    }

    SetCodecApiBool(transform_.Get(), CODECAPI_AVLowLatencyMode, true,
                    "low latency mode");
    SetCodecApiUint32(transform_.Get(), CODECAPI_AVEncCommonRateControlMode,
                      eAVEncCommonRateControlMode_UnconstrainedVBR,
                      "rate control mode");
    SetCodecApiUint32(transform_.Get(), CODECAPI_AVEncCommonMeanBitRate,
                      bitrate_bps_, "mean bitrate");
    SetCodecApiUint32(transform_.Get(), CODECAPI_AVEncMPVDefaultBPictureCount,
                      0, "B-frame count");

    ComPtr<IMFMediaType> output_type;
    hr = MFCreateMediaType(&output_type);
    if (FAILED(hr)) {
      return hr;
    }
    output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    output_type->SetUINT32(MF_MT_AVG_BITRATE, bitrate_bps_);
    output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    output_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    MFSetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(output_type.Get(), MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(output_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = transform_->SetOutputType(kOutputStreamId, output_type.Get(), 0);
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING)
          << "Inter Galactic: Media Foundation H.264 SetOutputType failed "
          << HrToString(hr);
      return hr;
    }

    ComPtr<IMFMediaType> input_type;
    hr = MFCreateMediaType(&input_type);
    if (FAILED(hr)) {
      return hr;
    }
    input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(input_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = transform_->SetInputType(kInputStreamId, input_type.Get(), 0);
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING)
          << "Inter Galactic: Media Foundation H.264 SetInputType failed "
          << HrToString(hr);
      return hr;
    }

    output_type_ = output_type;
    ComPtr<IMFMediaType> current_output_type;
    if (SUCCEEDED(transform_->GetOutputCurrentType(kOutputStreamId,
                                                   &current_output_type))) {
      output_type_ = current_output_type;
    }

    hr = transform_->GetOutputStreamInfo(kOutputStreamId, &output_stream_info_);
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING)
          << "Inter Galactic: Media Foundation H.264 GetOutputStreamInfo "
             "failed "
          << HrToString(hr);
      return hr;
    }

    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return S_OK;
  }

  bool ConvertToNv12(const webrtc::I420BufferInterface& frame_buffer) {
    const size_t y_size = static_cast<size_t>(width_) * height_;
    const size_t uv_size = static_cast<size_t>(width_) * ((height_ + 1) / 2);
    nv12_buffer_.resize(y_size + uv_size);
    const int result = libyuv::I420ToNV12(
        frame_buffer.DataY(), frame_buffer.StrideY(), frame_buffer.DataU(),
        frame_buffer.StrideU(), frame_buffer.DataV(), frame_buffer.StrideV(),
        nv12_buffer_.data(), width_, nv12_buffer_.data() + y_size, width_,
        width_, height_);
    if (result != 0) {
      RTC_LOG(LS_WARNING)
          << "Inter Galactic: I420ToNV12 conversion failed for Media "
             "Foundation H.264";
      return false;
    }
    return true;
  }

  HRESULT CreateInputSample(const webrtc::VideoFrame& input_frame,
                            ComPtr<IMFSample>* sample_out) {
    ComPtr<IMFSample> sample;
    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
      return hr;
    }

    ComPtr<IMFMediaBuffer> buffer;
    hr = MFCreateMemoryBuffer(static_cast<DWORD>(nv12_buffer_.size()), &buffer);
    if (FAILED(hr)) {
      return hr;
    }
    BYTE* destination = nullptr;
    DWORD max_length = 0;
    DWORD current_length = 0;
    hr = buffer->Lock(&destination, &max_length, &current_length);
    if (FAILED(hr)) {
      return hr;
    }
    std::memcpy(destination, nv12_buffer_.data(), nv12_buffer_.size());
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(nv12_buffer_.size()));
    sample->AddBuffer(buffer.Get());

    const LONGLONG sample_time =
        input_frame.timestamp_us() > 0
            ? static_cast<LONGLONG>(input_frame.timestamp_us() * 10)
            : static_cast<LONGLONG>(metadata_queue_.size() *
                                    sample_duration_hns_);
    sample->SetSampleTime(sample_time);
    sample->SetSampleDuration(sample_duration_hns_);
    *sample_out = sample;
    return S_OK;
  }

  int32_t DrainOutput(const webrtc::VideoFrame& input_frame) {
    for (int i = 0; i < 8; ++i) {
      const HRESULT hr = DrainOneOutput(input_frame);
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        return WEBRTC_VIDEO_CODEC_OK;
      }
      if (FAILED(hr)) {
        RTC_LOG(LS_WARNING)
            << "Inter Galactic: Media Foundation H.264 ProcessOutput failed "
            << HrToString(hr);
        return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
      }
    }
    return WEBRTC_VIDEO_CODEC_OK;
  }

  HRESULT DrainOneOutput(const webrtc::VideoFrame& input_frame) {
    ComPtr<IMFSample> caller_sample;
    if (!(output_stream_info_.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
      HRESULT hr = MFCreateSample(&caller_sample);
      if (FAILED(hr)) {
        return hr;
      }
      ComPtr<IMFMediaBuffer> output_buffer;
      const DWORD output_size = std::max<DWORD>(
          output_stream_info_.cbSize,
          static_cast<DWORD>(std::max<size_t>(
              4096, static_cast<size_t>(width_) * height_ * 3 / 2)));
      hr = MFCreateMemoryBuffer(output_size, &output_buffer);
      if (FAILED(hr)) {
        return hr;
      }
      caller_sample->AddBuffer(output_buffer.Get());
    }

    MFT_OUTPUT_DATA_BUFFER output_data = {};
    output_data.dwStreamID = kOutputStreamId;
    output_data.pSample = caller_sample.Get();
    DWORD process_status = 0;
    HRESULT hr = transform_->ProcessOutput(0, 1, &output_data, &process_status);
    if (output_data.pEvents) {
      output_data.pEvents->Release();
      output_data.pEvents = nullptr;
    }
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      ComPtr<IMFMediaType> current_output_type;
      if (SUCCEEDED(transform_->GetOutputCurrentType(kOutputStreamId,
                                                     &current_output_type))) {
        output_type_ = current_output_type;
      }
      return MF_E_TRANSFORM_NEED_MORE_INPUT;
    }
    if (FAILED(hr)) {
      return hr;
    }

    IMFSample* produced_sample = caller_sample.Get();
    ComPtr<IMFSample> provided_sample;
    if (!produced_sample && output_data.pSample) {
      provided_sample.Attach(output_data.pSample);
      produced_sample = provided_sample.Get();
    }
    if (!produced_sample) {
      return MF_E_TRANSFORM_NEED_MORE_INPUT;
    }

    std::vector<uint8_t> sample_bytes;
    if (!CopySampleBytes(produced_sample, &sample_bytes) ||
        sample_bytes.empty()) {
      return S_OK;
    }

    FrameMetadata metadata;
    if (!metadata_queue_.empty()) {
      metadata = metadata_queue_.front();
      metadata_queue_.pop_front();
    } else {
      metadata.rtp_timestamp = input_frame.rtp_timestamp();
      metadata.ntp_time_ms = input_frame.ntp_time_ms();
      metadata.color_space = input_frame.color_space();
    }

    UINT32 clean_point = 0;
    produced_sample->GetUINT32(MFSampleExtension_CleanPoint, &clean_point);
    const bool is_key_frame = clean_point != 0 || metadata.key_frame_requested;

    std::vector<uint8_t> encoded_bytes;
    if (is_key_frame) {
      std::vector<uint8_t> sequence_header;
      if (GetSequenceHeader(output_type_.Get(), &sequence_header)) {
        AppendAnnexB(sequence_header.data(), sequence_header.size(),
                     &encoded_bytes, &warned_unknown_layout_);
      }
    }
    AppendAnnexB(sample_bytes.data(), sample_bytes.size(), &encoded_bytes,
                 &warned_unknown_layout_);
    if (encoded_bytes.empty()) {
      return S_OK;
    }

    webrtc::EncodedImage encoded_image;
    encoded_image.SetEncodedData(webrtc::EncodedImageBuffer::Create(
        encoded_bytes.data(), encoded_bytes.size()));
    encoded_image._encodedWidth = width_;
    encoded_image._encodedHeight = height_;
    encoded_image.SetRtpTimestamp(metadata.rtp_timestamp);
    encoded_image.ntp_time_ms_ = metadata.ntp_time_ms;
    encoded_image.capture_time_ms_ = metadata.ntp_time_ms;
    encoded_image.SetColorSpace(metadata.color_space);
    encoded_image._frameType = is_key_frame
                                   ? webrtc::VideoFrameType::kVideoFrameKey
                                   : webrtc::VideoFrameType::kVideoFrameDelta;
    encoded_image.SetSimulcastIndex(0);

    webrtc::CodecSpecificInfo codec_specific;
    codec_specific.codecType = webrtc::kVideoCodecH264;
    codec_specific.codecSpecific.H264.packetization_mode = packetization_mode_;
    codec_specific.codecSpecific.H264.temporal_idx = webrtc::kNoTemporalIdx;
    codec_specific.codecSpecific.H264.idr_frame = is_key_frame;
    codec_specific.codecSpecific.H264.base_layer_sync = false;
    encoded_image_callback_->OnEncodedImage(encoded_image, &codec_specific);
    return S_OK;
  }

  void ForceKeyFrame() {
    if (!transform_) {
      return;
    }
    SetCodecApiUint32(transform_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1,
                      "force key frame");
  }

  webrtc::H264PacketizationMode packetization_mode_;
  webrtc::VideoCodec codec_;
  webrtc::EncodedImageCallback* encoded_image_callback_ = nullptr;
  ComPtr<IMFTransform> transform_;
  ComPtr<IMFMediaType> output_type_;
  MFT_OUTPUT_STREAM_INFO output_stream_info_ = {};
  std::vector<uint8_t> nv12_buffer_;
  std::deque<FrameMetadata> metadata_queue_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t fps_ = 30;
  uint32_t bitrate_bps_ = 2500000;
  size_t max_payload_size_ = 0;
  LONGLONG sample_duration_hns_ = 333333;
  bool warned_unknown_layout_ = false;
};

class MediaFoundationH264EncoderFactory final
    : public webrtc::VideoEncoderFactory {
 public:
  std::unique_ptr<webrtc::VideoEncoder> Create(
      const webrtc::Environment& env,
      const webrtc::SdpVideoFormat& format) override {
    auto builtin_factory = webrtc::CreateBuiltinVideoEncoderFactory();
    if (!IsH264(format)) {
      return builtin_factory->Create(env, format);
    }

    auto hardware_encoder = std::make_unique<MediaFoundationH264Encoder>(
        webrtc::H264EncoderSettings::Parse(format));
    auto software_encoder = builtin_factory->Create(env, format);
    if (!software_encoder) {
      return hardware_encoder;
    }
    return webrtc::CreateVideoEncoderSoftwareFallbackWrapper(
        env, std::move(software_encoder), std::move(hardware_encoder),
        /*prefer_temporal_support=*/false);
  }

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
    return webrtc::CreateBuiltinVideoEncoderFactory()->GetSupportedFormats();
  }

  CodecSupport QueryCodecSupport(
      const webrtc::SdpVideoFormat& format,
      std::optional<std::string> scalability_mode) const override {
    if (IsH264(format)) {
      return {.is_supported = true, .is_power_efficient = true};
    }
    return webrtc::CreateBuiltinVideoEncoderFactory()->QueryCodecSupport(
        format, scalability_mode);
  }
};

}  // namespace

bool IsMediaFoundationH264HardwareEncoderAvailable() {
  return SUCCEEDED(EnumerateHardwareH264Encoder(nullptr));
}

std::unique_ptr<webrtc::VideoEncoderFactory>
CreateMediaFoundationH264EncoderFactory() {
  RTC_LOG(LS_INFO)
      << "Inter Galactic: using Media Foundation hardware H.264 encoder "
         "factory with WebRTC software fallback";
  return std::make_unique<MediaFoundationH264EncoderFactory>();
}

}  // namespace base
}  // namespace owt
