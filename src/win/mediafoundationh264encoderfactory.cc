// Copyright (C) 2026 Inter Galactic contributors
//
// SPDX-License-Identifier: Apache-2.0

#include "src/win/mediafoundationh264encoderfactory.h"

#include <codecapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <windows.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
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
#include "src/win/intergalactic_d3d11_nv12_buffer.h"
#include "src/win/intergalactic_native_config.h"
#include "src/win/intergalactic_native_diagnostics.h"
#include "third_party/libyuv/include/libyuv/convert_from.h"

namespace owt {
namespace base {
namespace {

using Microsoft::WRL::ComPtr;
using intergalactic::win::EnvironmentValueStatus;
using intergalactic::win::ReadEnvironmentFlag;
using intergalactic::win::ReadEnvironmentUint32;
using intergalactic::win::ReadEnvironmentValue;

constexpr int kLowH264QpThreshold = 24;
constexpr int kHighH264QpThreshold = 37;
constexpr DWORD kInputStreamId = 0;
constexpr DWORD kOutputStreamId = 0;
constexpr DWORD kNativeNv12FenceWaitTimeoutMs = 50;

using SteadyClock = std::chrono::steady_clock;

uint16_t SourceFrameTrackingId(uint64_t source_frame_index) {
  if (source_frame_index == 0) {
    return webrtc::VideoFrame::kNotSetId;
  }
  return static_cast<uint16_t>(((source_frame_index - 1) % 65535) + 1);
}

double ElapsedMs(SteadyClock::time_point start,
                 SteadyClock::time_point end = SteadyClock::now()) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

int64_t CurrentQpc() {
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  return now.QuadPart;
}

double QpcDeltaMs(uint64_t start_qpc, int64_t end_qpc) {
  if (start_qpc == 0 || end_qpc <= static_cast<int64_t>(start_qpc)) {
    return 0.0;
  }
  LARGE_INTEGER frequency{};
  QueryPerformanceFrequency(&frequency);
  const double ticks_per_second =
      frequency.QuadPart == 0 ? 1.0 : static_cast<double>(frequency.QuadPart);
  return (static_cast<double>(end_qpc) - static_cast<double>(start_qpc)) *
         1000.0 / ticks_per_second;
}

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

struct D3dAdapterDiagnostics {
  bool available = false;
  std::string luid = "unknown";
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
};

std::string LuidLabel(const LUID& luid) {
  return std::to_string(luid.HighPart) + ":" +
         std::to_string(luid.LowPart);
}

D3dAdapterDiagnostics QueryD3dAdapterDiagnostics(ID3D11Device* device) {
  D3dAdapterDiagnostics diagnostics;
  if (device == nullptr) {
    return diagnostics;
  }
  ComPtr<IDXGIDevice> dxgi_device;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) ||
      dxgi_device == nullptr) {
    return diagnostics;
  }
  ComPtr<IDXGIAdapter> adapter;
  if (FAILED(dxgi_device->GetAdapter(&adapter)) || adapter == nullptr) {
    return diagnostics;
  }
  DXGI_ADAPTER_DESC desc{};
  if (FAILED(adapter->GetDesc(&desc))) {
    return diagnostics;
  }
  diagnostics.available = true;
  diagnostics.luid = LuidLabel(desc.AdapterLuid);
  diagnostics.vendor_id = desc.VendorId;
  diagnostics.device_id = desc.DeviceId;
  return diagnostics;
}

void AppendNativeWebrtcDiagnosticLine(const std::string& line) {
  intergalactic::win::AppendNativeWebrtcDiagnosticLine(line);
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

HRESULT UnlockAsyncTransformIfNeeded(IMFTransform* transform) {
  if (!transform) {
    return E_POINTER;
  }

  ComPtr<IMFAttributes> attributes;
  HRESULT hr = transform->GetAttributes(&attributes);
  if (FAILED(hr) || !attributes) {
    RTC_LOG(LS_WARNING)
        << "Inter Galactic: Media Foundation H.264 GetAttributes failed "
        << HrToString(hr);
    return hr;
  }

  UINT32 is_async = FALSE;
  hr = attributes->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);
  if (hr == MF_E_ATTRIBUTENOTFOUND || is_async == FALSE) {
    RTC_LOG(LS_INFO)
        << "Inter Galactic: Media Foundation H.264 MFT is synchronous";
    return S_OK;
  }
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING)
        << "Inter Galactic: Media Foundation H.264 async attribute read "
           "failed "
        << HrToString(hr);
    return hr;
  }

  hr = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING)
        << "Inter Galactic: Media Foundation H.264 async unlock failed "
        << HrToString(hr);
    return hr;
  }

  RTC_LOG(LS_INFO)
      << "Inter Galactic: Media Foundation H.264 async MFT unlocked";
  return S_OK;
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

enum class EncodedCallbackDropPolicy {
  kDropNewest,
  kDropOldest,
};

struct RateControlModeConfig {
  ULONG codec_api_value = eAVEncCommonRateControlMode_CBR;
  const char* label = "cbr";
  bool set_max_bitrate = true;
  bool set_min_bitrate = true;
};

EncodedCallbackDropPolicy ReadEncodedCallbackDropPolicy() {
  char buffer[32] = {};
  const EnvironmentValueStatus status = ReadEnvironmentValue(
      "INTERGALACTIC_MF_ASYNC_CALLBACK_DROP_POLICY", buffer,
      sizeof(buffer), nullptr, true);
  if (status == EnvironmentValueStatus::kMissing) {
    return EncodedCallbackDropPolicy::kDropNewest;
  }
  if (status == EnvironmentValueStatus::kOverlong) {
    return EncodedCallbackDropPolicy::kDropNewest;
  }
  if (_stricmp(buffer, "drop_oldest") == 0 ||
      _stricmp(buffer, "oldest") == 0) {
    return EncodedCallbackDropPolicy::kDropOldest;
  }
  if (_stricmp(buffer, "drop_newest") == 0 ||
      _stricmp(buffer, "newest") == 0) {
    return EncodedCallbackDropPolicy::kDropNewest;
  }
  RTC_LOG(LS_WARNING)
      << "Inter Galactic: ignoring invalid "
         "INTERGALACTIC_MF_ASYNC_CALLBACK_DROP_POLICY value '"
      << buffer << "'";
  return EncodedCallbackDropPolicy::kDropNewest;
}

const char* EncodedCallbackDropPolicyName(
    EncodedCallbackDropPolicy drop_policy) {
  switch (drop_policy) {
    case EncodedCallbackDropPolicy::kDropOldest:
      return "drop_oldest";
    case EncodedCallbackDropPolicy::kDropNewest:
    default:
      return "drop_newest";
  }
}

RateControlModeConfig ReadRateControlModeConfig() {
  char buffer[64] = {};
  const EnvironmentValueStatus status = ReadEnvironmentValue(
      "INTERGALACTIC_MF_H264_RATE_CONTROL_MODE", buffer, sizeof(buffer),
      nullptr, true);
  if (status == EnvironmentValueStatus::kMissing ||
      status == EnvironmentValueStatus::kOverlong) {
    return {};
  }
  if (_stricmp(buffer, "default") == 0) {
    return {};
  }
  if (_stricmp(buffer, "unconstrained_vbr") == 0 ||
      _stricmp(buffer, "unconstrained-vbr") == 0 ||
      _stricmp(buffer, "uvbr") == 0 ||
      _stricmp(buffer, "legacy") == 0) {
    return {eAVEncCommonRateControlMode_UnconstrainedVBR,
            "unconstrained_vbr", false, false};
  }
  if (_stricmp(buffer, "cbr") == 0) {
    return {eAVEncCommonRateControlMode_CBR, "cbr", true, true};
  }
  if (_stricmp(buffer, "peak_constrained_vbr") == 0 ||
      _stricmp(buffer, "peak-constrained-vbr") == 0 ||
      _stricmp(buffer, "peak_vbr") == 0 ||
      _stricmp(buffer, "peak-vbr") == 0) {
    return {eAVEncCommonRateControlMode_PeakConstrainedVBR,
            "peak_constrained_vbr", true, false};
  }
  if (_stricmp(buffer, "low_delay_vbr") == 0 ||
      _stricmp(buffer, "low-delay-vbr") == 0 ||
      _stricmp(buffer, "ldvbr") == 0) {
    return {eAVEncCommonRateControlMode_LowDelayVBR, "low_delay_vbr", true,
            false};
  }
  if (_stricmp(buffer, "global_vbr") == 0 ||
      _stricmp(buffer, "global-vbr") == 0) {
    return {eAVEncCommonRateControlMode_GlobalVBR, "global_vbr", true, false};
  }
  if (_stricmp(buffer, "global_low_delay_vbr") == 0 ||
      _stricmp(buffer, "global-low-delay-vbr") == 0 ||
      _stricmp(buffer, "gldvbr") == 0) {
    return {eAVEncCommonRateControlMode_GlobalLowDelayVBR,
            "global_low_delay_vbr", true, false};
  }
  RTC_LOG(LS_WARNING)
      << "Inter Galactic: ignoring invalid "
         "INTERGALACTIC_MF_H264_RATE_CONTROL_MODE value '"
      << buffer << "'";
  return {};
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
                          ULONG value, const char* label,
                          bool log_success = true) {
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
  } else if (log_success) {
    RTC_LOG(LS_INFO) << "Inter Galactic: Media Foundation H.264 accepted "
                     << label << "=" << value;
  }
  return hr;
}

HRESULT SetCodecApiBool(IMFTransform* transform, const GUID& property,
                        bool value, const char* label,
                        bool log_success = true) {
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
  } else if (log_success) {
    RTC_LOG(LS_INFO) << "Inter Galactic: Media Foundation H.264 accepted "
                     << label << "=" << (value ? "true" : "false");
  }
  return hr;
}

struct FrameMetadata {
  uint32_t rtp_timestamp = 0;
  int64_t ntp_time_ms = 0;
  std::optional<webrtc::ColorSpace> color_space;
  bool key_frame_requested = false;
  ComPtr<IMFSample> retained_input_sample;
  bool native_input_sample_retained = false;
  SteadyClock::time_point retained_input_sample_created_at =
      SteadyClock::now();
  std::string native_source_mode;
  uint32_t native_source_format = 0;
  uint64_t native_source_frame_index = 0;
  double native_source_age_ms = 0.0;
  double native_source_age_at_create_ms = 0.0;
  double native_buffer_age_ms = 0.0;
};

struct EncoderFrameTiming {
  double total_ms = 0.0;
  double to_i420_ms = 0.0;
  double pre_drain_ms = 0.0;
  double convert_nv12_ms = 0.0;
  double create_sample_ms = 0.0;
  double input_copy_ms = 0.0;
  double native_ready_fence_wait_ms = 0.0;
  double process_input_ms = 0.0;
  double retry_drain_ms = 0.0;
  double post_drain_ms = 0.0;
  double process_output_ms = 0.0;
  double output_copy_ms = 0.0;
  double encoded_callback_ms = 0.0;
  double encoded_callback_enqueue_ms = 0.0;
  int output_frames = 0;
  int encoded_callback_invocations = 0;
  int encoded_callback_drops = 0;
  size_t encoded_callback_queue_depth = 0;
  size_t output_bytes = 0;
  std::string native_source_mode;
  uint32_t native_source_format = 0;
  uint64_t native_source_frame_index = 0;
  double native_source_age_ms = 0.0;
  double native_source_age_at_create_ms = 0.0;
  double native_buffer_age_ms = 0.0;
  double native_sample_lifetime_ms = 0.0;
  double native_sample_lifetime_max_ms = 0.0;
  int native_sample_lifetime_samples = 0;
  std::string native_adapter_luid = "unknown";
  uint32_t native_adapter_vendor_id = 0;
  uint32_t native_adapter_device_id = 0;
  bool native_nv12_input = false;
  bool native_nv12_used = false;
  bool native_nv12_suspended = false;
  bool native_nv12_sample_failed = false;
  bool native_ready_fence = false;
  bool native_ready_fence_timeout = false;
  bool encoded_callback_async = false;
};

struct PendingEncodedCallback {
  webrtc::EncodedImage encoded_image;
  webrtc::CodecSpecificInfo codec_specific;
  SteadyClock::time_point queued_at = SteadyClock::now();
  uint64_t frame = 0;
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
    rate_control_mode_ = ReadRateControlModeConfig();
    max_payload_size_ = settings.max_payload_size;
    sample_duration_hns_ = 10000000LL / fps_;
    frames_seen_ = 0;
    encoded_outputs_seen_ = 0;
    consecutive_native_no_output_frames_ = 0;
    consecutive_native_not_accepting_frames_ = 0;
    native_nv12_startup_reinitialize_attempts_ = 0;
    native_nv12_suspended_ = false;
    async_encoded_callback_enabled_ = ReadEnvironmentFlag(
        "INTERGALACTIC_MF_ASYNC_ENCODED_CALLBACK", false);
    async_encoded_callback_queue_depth_limit_ = ReadEnvironmentUint32(
        "INTERGALACTIC_MF_ASYNC_CALLBACK_QUEUE_DEPTH", 2, 1, 8);
    async_encoded_callback_drop_policy_ = ReadEncodedCallbackDropPolicy();
    async_encoded_callback_outputs_seen_ = 0;
    async_encoded_callback_drops_seen_ = 0;

    const HRESULT hr = InitializeTransform();
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING)
          << "Inter Galactic: Media Foundation H.264 init failed "
          << HrToString(hr) << "; software fallback can continue";
      Release();
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    if (async_encoded_callback_enabled_) {
      StartAsyncEncodedCallbackWorker();
    }

    RTC_LOG(LS_INFO) << "Inter Galactic: Media Foundation H.264 encoder "
                     << "initialized " << width_ << "x" << height_ << "@"
                     << fps_ << " bitrate=" << bitrate_bps_
                     << " rate_control_mode=" << rate_control_mode_.label
                     << " async_encoded_callback="
                     << (async_encoded_callback_enabled_ ? "yes" : "no")
                     << " async_callback_queue_depth="
                     << async_encoded_callback_queue_depth_limit_
                     << " async_callback_drop_policy="
                     << EncodedCallbackDropPolicyName(
                            async_encoded_callback_drop_policy_);
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t RegisterEncodeCompleteCallback(
      webrtc::EncodedImageCallback* callback) override {
    encoded_image_callback_ = callback;
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t Release() override {
    StopAsyncEncodedCallbackWorker();
    if (transform_) {
      transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
      transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    event_generator_.Reset();
    transform_.Reset();
    dxgi_device_manager_.Reset();
    native_d3d_device_.Reset();
    native_adapter_diagnostics_ = D3dAdapterDiagnostics{};
    dxgi_device_manager_token_ = 0;
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

    const bool key_frame_requested =
        frame_types && !frame_types->empty() &&
        (*frame_types)[0] == webrtc::VideoFrameType::kVideoFrameKey;

    EncoderFrameTiming timing;
    const auto total_start = SteadyClock::now();
    ++frames_seen_;

    bool prepared_for_input = false;
    auto prepare_encoder_for_input = [&]() -> int32_t {
      if (prepared_for_input) {
        return WEBRTC_VIDEO_CODEC_OK;
      }
      if (event_generator_) {
        const auto drain_start = SteadyClock::now();
        const int32_t drain_result = DrainOutput(input_frame, &timing);
        timing.pre_drain_ms += ElapsedMs(drain_start);
        if (drain_result != WEBRTC_VIDEO_CODEC_OK) {
          timing.total_ms = ElapsedMs(total_start);
          MaybeLogEncoderTiming(timing, "pre_drain_failed");
          return drain_result;
        }
      }
      if (key_frame_requested) {
        ForceKeyFrame();
      }
      prepared_for_input = true;
      return WEBRTC_VIDEO_CODEC_OK;
    };

    ComPtr<IMFSample> sample;
    double input_copy_ms = 0.0;
    HRESULT hr = S_OK;
    auto* native_nv12 = owt::base::IntergalacticD3D11Nv12Buffer::
        FromVideoFrameBuffer(input_frame.video_frame_buffer().get());
    if (native_nv12 != nullptr) {
      timing.native_nv12_input = true;
      const int64_t native_seen_qpc = CurrentQpc();
      timing.native_source_mode = native_nv12->source_mode().empty()
                                      ? "unknown"
                                      : native_nv12->source_mode();
      timing.native_source_format = native_nv12->source_format();
      timing.native_source_frame_index = native_nv12->source_frame_index();
      timing.native_source_age_at_create_ms =
          native_nv12->source_age_at_create_ms();
      timing.native_source_age_ms =
          QpcDeltaMs(native_nv12->source_qpc(), native_seen_qpc);
      timing.native_buffer_age_ms =
          QpcDeltaMs(static_cast<uint64_t>(
                         std::max<int64_t>(0, native_nv12->created_qpc())),
                     native_seen_qpc);
      if (native_nv12_suspended_) {
        timing.native_nv12_suspended = true;
      } else {
        if (static_cast<uint32_t>(native_nv12->width()) != width_ ||
            static_cast<uint32_t>(native_nv12->height()) != height_) {
          hr = ReinitializeForFrameSize(
              static_cast<uint32_t>(native_nv12->width()),
              static_cast<uint32_t>(native_nv12->height()));
          if (FAILED(hr)) {
            RTC_LOG(LS_WARNING)
                << "Inter Galactic: Media Foundation H.264 native frame-size "
                   "reinitialization failed "
                << HrToString(hr) << "; software fallback can continue";
            timing.total_ms = ElapsedMs(total_start);
            MaybeLogEncoderTiming(timing, "native_resize_failed");
            return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
          }
        }
        const int32_t prepare_result = prepare_encoder_for_input();
        if (prepare_result != WEBRTC_VIDEO_CODEC_OK) {
          return prepare_result;
        }
        const auto fence_wait_start = SteadyClock::now();
        const HRESULT fence_hr =
            native_nv12->WaitForReadyFence(kNativeNv12FenceWaitTimeoutMs);
        timing.native_ready_fence_wait_ms = ElapsedMs(fence_wait_start);
        timing.native_ready_fence = fence_hr != S_FALSE;
        timing.native_ready_fence_timeout =
            fence_hr == HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        if (FAILED(fence_hr)) {
          hr = fence_hr;
          timing.native_nv12_sample_failed = true;
          RTC_LOG(LS_WARNING)
              << "Inter Galactic: Media Foundation H.264 native NV12 ready "
                 "fence wait failed "
              << HrToString(hr) << "; falling back to scaled CPU input";
        } else {
          const auto sample_start = SteadyClock::now();
          hr = CreateNativeInputSample(input_frame, native_nv12, &sample,
                                       &input_copy_ms);
          timing.create_sample_ms = ElapsedMs(sample_start);
          timing.input_copy_ms = input_copy_ms;
          timing.native_adapter_luid = native_adapter_diagnostics_.luid;
          timing.native_adapter_vendor_id =
              native_adapter_diagnostics_.vendor_id;
          timing.native_adapter_device_id =
              native_adapter_diagnostics_.device_id;
        }
        if (FAILED(hr)) {
          timing.native_nv12_sample_failed = true;
          sample.Reset();
          RTC_LOG(LS_WARNING)
              << "Inter Galactic: Media Foundation H.264 native NV12 sample "
                 "failed "
              << HrToString(hr) << "; falling back to scaled CPU input";
        } else {
          timing.native_nv12_used = true;
        }
      }
    }

    if (sample == nullptr) {
      const auto to_i420_start = SteadyClock::now();
      webrtc::scoped_refptr<webrtc::I420BufferInterface> frame_buffer =
          input_frame.video_frame_buffer()->ToI420();
      timing.to_i420_ms = ElapsedMs(to_i420_start);
      if (!frame_buffer) {
        RTC_LOG(LS_WARNING)
            << "Inter Galactic: Media Foundation H.264 could not convert "
               "input frame to I420";
        timing.total_ms = ElapsedMs(total_start);
        MaybeLogEncoderTiming(timing, "to_i420_failed");
        return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
      }
      if (static_cast<uint32_t>(frame_buffer->width()) != width_ ||
          static_cast<uint32_t>(frame_buffer->height()) != height_) {
        hr = ReinitializeForFrameSize(
            static_cast<uint32_t>(frame_buffer->width()),
            static_cast<uint32_t>(frame_buffer->height()));
        if (FAILED(hr)) {
          RTC_LOG(LS_WARNING)
              << "Inter Galactic: Media Foundation H.264 frame-size "
                 "reinitialization failed "
              << HrToString(hr) << "; software fallback can continue";
          timing.total_ms = ElapsedMs(total_start);
          MaybeLogEncoderTiming(timing, "resize_failed");
          return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
        }
        prepared_for_input = false;
      }
      const int32_t prepare_result = prepare_encoder_for_input();
      if (prepare_result != WEBRTC_VIDEO_CODEC_OK) {
        return prepare_result;
      }

      const auto convert_start = SteadyClock::now();
      if (!ConvertToNv12(*frame_buffer)) {
        timing.convert_nv12_ms = ElapsedMs(convert_start);
        timing.total_ms = ElapsedMs(total_start);
        MaybeLogEncoderTiming(timing, "nv12_failed");
        return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
      }
      timing.convert_nv12_ms = ElapsedMs(convert_start);

      const auto sample_start = SteadyClock::now();
      hr = CreateInputSample(input_frame, &sample, &input_copy_ms);
      timing.create_sample_ms = ElapsedMs(sample_start);
      timing.input_copy_ms = input_copy_ms;
      if (FAILED(hr)) {
        RTC_LOG(LS_WARNING)
            << "Inter Galactic: Media Foundation H.264 input sample failed "
            << HrToString(hr);
        timing.total_ms = ElapsedMs(total_start);
        MaybeLogEncoderTiming(timing, "sample_failed");
        return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
      }
    }

    auto process_input_start = SteadyClock::now();
    hr = transform_->ProcessInput(kInputStreamId, sample.Get(), 0);
    timing.process_input_ms += ElapsedMs(process_input_start);
    if (hr == MF_E_NOTACCEPTING) {
      const auto retry_drain_start = SteadyClock::now();
      DrainOutput(input_frame, &timing);
      timing.retry_drain_ms += ElapsedMs(retry_drain_start);
      process_input_start = SteadyClock::now();
      hr = transform_->ProcessInput(kInputStreamId, sample.Get(), 0);
      timing.process_input_ms += ElapsedMs(process_input_start);
    }
    if (hr == MF_E_NOTACCEPTING && event_generator_) {
      uint32_t not_accepting_streak = 0;
      const uint32_t not_accepting_threshold =
          NativeNotAcceptingRecoveryThreshold();
      if (timing.native_nv12_used) {
        not_accepting_streak = ++consecutive_native_not_accepting_frames_;
      } else {
        consecutive_native_not_accepting_frames_ = 0;
      }

      HRESULT recovery_hr = S_OK;
      bool recovered_native_not_accepting = false;
      if (timing.native_nv12_used && !native_nv12_suspended_ &&
          not_accepting_streak >= not_accepting_threshold) {
        recovery_hr =
            RecoverFromNativeNotAcceptingStall(not_accepting_streak,
                                               not_accepting_threshold);
        timing.native_nv12_suspended = native_nv12_suspended_;
        recovered_native_not_accepting = SUCCEEDED(recovery_hr);
      }

      const bool should_log_not_accepting_warning =
          !timing.native_nv12_used || not_accepting_streak == 1 ||
          not_accepting_streak == not_accepting_threshold ||
          (not_accepting_streak > 0 &&
           not_accepting_streak % std::max<uint32_t>(1, fps_) == 0);
      if (should_log_not_accepting_warning) {
        RTC_LOG(LS_WARNING)
            << "Inter Galactic: Media Foundation H.264 async encoder is not "
               "accepting input yet; dropping one frame without software "
               "fallback native_nv12="
            << (timing.native_nv12_used ? "yes" : "no")
            << " streak=" << not_accepting_streak
            << " threshold=" << not_accepting_threshold
            << " queue=" << metadata_queue_.size()
            << " retained_samples=" << RetainedNativeSampleCount()
            << " encoded_outputs=" << encoded_outputs_seen_
            << " native_suspended="
            << (native_nv12_suspended_ ? "yes" : "no");
      }
      timing.total_ms = ElapsedMs(total_start);
      MaybeLogEncoderTiming(
          timing,
          recovered_native_not_accepting ? "not_accepting_recovered"
                                         : "not_accepting");
      if (FAILED(recovery_hr)) {
        return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
      }
      return WEBRTC_VIDEO_CODEC_OK;
    }
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING)
          << "Inter Galactic: Media Foundation H.264 ProcessInput failed "
          << HrToString(hr);
      timing.total_ms = ElapsedMs(total_start);
      MaybeLogEncoderTiming(timing, "process_input_failed");
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }
    consecutive_native_not_accepting_frames_ = 0;
    if (timing.native_nv12_used) {
      native_nv12_startup_reinitialize_attempts_ = 0;
    }

    FrameMetadata metadata;
    metadata.rtp_timestamp = input_frame.rtp_timestamp();
    metadata.ntp_time_ms = input_frame.ntp_time_ms();
    metadata.color_space = input_frame.color_space();
    metadata.key_frame_requested = key_frame_requested;
    if (timing.native_nv12_used) {
      metadata.retained_input_sample = sample;
      metadata.native_input_sample_retained = true;
      metadata.retained_input_sample_created_at = SteadyClock::now();
      metadata.native_source_mode = timing.native_source_mode;
      metadata.native_source_format = timing.native_source_format;
      metadata.native_source_frame_index = timing.native_source_frame_index;
      metadata.native_source_age_ms = timing.native_source_age_ms;
      metadata.native_source_age_at_create_ms =
          timing.native_source_age_at_create_ms;
      metadata.native_buffer_age_ms = timing.native_buffer_age_ms;
    }
    metadata_queue_.push_back(std::move(metadata));

    const auto post_drain_start = SteadyClock::now();
    const int32_t result = DrainOutput(input_frame, &timing);
    timing.post_drain_ms += ElapsedMs(post_drain_start);
    timing.total_ms = ElapsedMs(total_start);
    MaybeLogEncoderTiming(timing,
                          result == WEBRTC_VIDEO_CODEC_OK
                              ? (timing.native_nv12_used ? "ok_native_nv12"
                                                         : "ok")
                              : "drain_failed");
    if (result == WEBRTC_VIDEO_CODEC_OK) {
      MaybeRecoverFromNativeHandoffStall(timing);
    }
    return result;
  }

  void SetRates(const RateControlParameters& parameters) override {
    const uint32_t bitrate_bps =
        static_cast<uint32_t>(parameters.bitrate.get_sum_bps());
    if (bitrate_bps == 0) {
      return;
    }
    bitrate_bps_ = bitrate_bps;
    if (transform_) {
      ApplyBitrateCodecApiProperties(false);
    }
  }

  EncoderInfo GetEncoderInfo() const override {
    EncoderInfo info;
    info.supports_native_handle = true;
    info.implementation_name = "MediaFoundationH264";
    info.scaling_settings = webrtc::VideoEncoder::ScalingSettings(
        kLowH264QpThreshold, kHighH264QpThreshold);
    info.is_hardware_accelerated = true;
    info.supports_simulcast = false;
    info.requested_resolution_alignment = 2;
    info.apply_alignment_to_all_simulcast_layers = true;
    info.preferred_pixel_formats = {webrtc::VideoFrameBuffer::Type::kNative,
                                    webrtc::VideoFrameBuffer::Type::kI420};
    return info;
  }

 private:
  void ApplyBitrateCodecApiProperties(bool log_success) {
    if (!transform_) {
      return;
    }
    SetCodecApiUint32(transform_.Get(), CODECAPI_AVEncCommonMeanBitRate,
                      bitrate_bps_, "mean bitrate", log_success);
    if (rate_control_mode_.set_max_bitrate) {
      SetCodecApiUint32(transform_.Get(), CODECAPI_AVEncCommonMaxBitRate,
                        bitrate_bps_, "max bitrate", log_success);
    }
    if (rate_control_mode_.set_min_bitrate) {
      SetCodecApiUint32(transform_.Get(), CODECAPI_AVEncCommonMinBitRate,
                        bitrate_bps_, "min bitrate", log_success);
    }
  }

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

    hr = UnlockAsyncTransformIfNeeded(transform_.Get());
    if (FAILED(hr)) {
      return hr;
    }
    event_generator_.Reset();
    ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(transform_->GetAttributes(&attributes)) && attributes) {
      UINT32 is_async = FALSE;
      if (SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &is_async)) &&
          is_async != FALSE) {
        hr = transform_.As(&event_generator_);
        if (FAILED(hr) || !event_generator_) {
          RTC_LOG(LS_WARNING)
              << "Inter Galactic: Media Foundation H.264 async MFT does not "
                 "expose IMFMediaEventGenerator "
              << HrToString(hr);
          return FAILED(hr) ? hr : E_NOINTERFACE;
        }
        RTC_LOG(LS_INFO)
            << "Inter Galactic: Media Foundation H.264 using async event "
               "drain";
      }
    }

    SetCodecApiBool(transform_.Get(), CODECAPI_AVLowLatencyMode, true,
                    "low latency mode");
    SetCodecApiUint32(transform_.Get(), CODECAPI_AVEncCommonQualityVsSpeed,
                      100, "quality vs speed");
    SetCodecApiUint32(transform_.Get(), CODECAPI_AVEncCommonRateControlMode,
                      rate_control_mode_.codec_api_value,
                      "rate control mode");
    ApplyBitrateCodecApiProperties(true);
    SetCodecApiUint32(transform_.Get(), CODECAPI_AVEncMPVDefaultBPictureCount,
                      0, "B-frame count");
    SetCodecApiBool(transform_.Get(), CODECAPI_AVEncH264CABACEnable, false,
                    "CABAC");

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

  HRESULT ReinitializeForFrameSize(uint32_t width, uint32_t height) {
    RTC_LOG(LS_INFO)
        << "Inter Galactic: Media Foundation H.264 reinitializing for frame "
           "size change "
        << width_ << "x" << height_ << " -> " << width << "x" << height;

    Release();
    width_ = width;
    height_ = height;
    codec_.width = width;
    codec_.height = height;
    return InitializeTransform();
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
                            ComPtr<IMFSample>* sample_out,
                            double* input_copy_ms) {
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
    const auto copy_start = SteadyClock::now();
    hr = buffer->Lock(&destination, &max_length, &current_length);
    if (FAILED(hr)) {
      return hr;
    }
    std::memcpy(destination, nv12_buffer_.data(), nv12_buffer_.size());
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(nv12_buffer_.size()));
    if (input_copy_ms) {
      *input_copy_ms = ElapsedMs(copy_start);
    }
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

  HRESULT EnsureNativeD3DManager(ID3D11Device* device) {
    if (device == nullptr || transform_ == nullptr) {
      return E_POINTER;
    }
    if (dxgi_device_manager_ != nullptr &&
        native_d3d_device_.Get() == device) {
      return S_OK;
    }

    ComPtr<IMFDXGIDeviceManager> manager;
    UINT reset_token = 0;
    HRESULT hr = MFCreateDXGIDeviceManager(&reset_token, &manager);
    if (FAILED(hr)) {
      return hr;
    }
    hr = manager->ResetDevice(device, reset_token);
    if (FAILED(hr)) {
      return hr;
    }
    hr = transform_->ProcessMessage(
        MFT_MESSAGE_SET_D3D_MANAGER,
        reinterpret_cast<ULONG_PTR>(manager.Get()));
    if (FAILED(hr)) {
      return hr;
    }

    dxgi_device_manager_ = manager;
    dxgi_device_manager_token_ = reset_token;
    native_d3d_device_ = device;
    native_adapter_diagnostics_ = QueryD3dAdapterDiagnostics(device);
    RTC_LOG(LS_INFO)
        << "Inter Galactic: Media Foundation H.264 DXGI device manager set "
        << "adapter_luid=" << native_adapter_diagnostics_.luid
        << " vendor_id=" << native_adapter_diagnostics_.vendor_id
        << " device_id=" << native_adapter_diagnostics_.device_id;
    return S_OK;
  }

  HRESULT CreateNativeInputSample(
      const webrtc::VideoFrame& input_frame,
      owt::base::IntergalacticD3D11Nv12Buffer* native_buffer,
      ComPtr<IMFSample>* sample_out,
      double* input_copy_ms) {
    if (native_buffer == nullptr || native_buffer->texture() == nullptr ||
        native_buffer->device() == nullptr) {
      return E_POINTER;
    }

    const auto start = SteadyClock::now();
    HRESULT hr = EnsureNativeD3DManager(native_buffer->device());
    if (FAILED(hr)) {
      return hr;
    }

    ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
      return hr;
    }
    ComPtr<IMFMediaBuffer> buffer;
    hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D),
                                   native_buffer->texture(), 0, FALSE,
                                   &buffer);
    if (SUCCEEDED(hr)) {
      hr = buffer->SetCurrentLength(width_ * height_ * 3 / 2);
    }
    if (FAILED(hr)) {
      return hr;
    }
    hr = sample->AddBuffer(buffer.Get());
    if (FAILED(hr)) {
      return hr;
    }

    const LONGLONG sample_time =
        input_frame.timestamp_us() > 0
            ? static_cast<LONGLONG>(input_frame.timestamp_us() * 10)
            : static_cast<LONGLONG>(metadata_queue_.size() *
                                    sample_duration_hns_);
    sample->SetSampleTime(sample_time);
    sample->SetSampleDuration(sample_duration_hns_);
    if (input_copy_ms) {
      *input_copy_ms = ElapsedMs(start);
    }
    *sample_out = sample;
    return S_OK;
  }

  int32_t DrainOutput(const webrtc::VideoFrame& input_frame,
                      EncoderFrameTiming* timing = nullptr) {
    if (event_generator_) {
      return DrainAsyncOutput(input_frame, timing);
    }
    for (int i = 0; i < 8; ++i) {
      const HRESULT hr = DrainOneOutput(input_frame, timing);
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

  int32_t DrainAsyncOutput(const webrtc::VideoFrame& input_frame,
                           EncoderFrameTiming* timing) {
    for (int i = 0; i < 16; ++i) {
      ComPtr<IMFMediaEvent> event;
      HRESULT hr = event_generator_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
      if (hr == MF_E_NO_EVENTS_AVAILABLE) {
        return WEBRTC_VIDEO_CODEC_OK;
      }
      if (FAILED(hr)) {
        RTC_LOG(LS_WARNING)
            << "Inter Galactic: Media Foundation H.264 GetEvent failed "
            << HrToString(hr);
        return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
      }

      MediaEventType event_type = MEUnknown;
      HRESULT event_status = S_OK;
      event->GetType(&event_type);
      event->GetStatus(&event_status);
      if (FAILED(event_status)) {
        RTC_LOG(LS_WARNING)
            << "Inter Galactic: Media Foundation H.264 async event failed "
            << HrToString(event_status);
        return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
      }

      if (event_type != METransformHaveOutput) {
        continue;
      }

      hr = DrainOneOutput(input_frame, timing);
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        continue;
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

  HRESULT DrainOneOutput(const webrtc::VideoFrame& input_frame,
                         EncoderFrameTiming* timing) {
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
    const auto process_output_start = SteadyClock::now();
    HRESULT hr = transform_->ProcessOutput(0, 1, &output_data, &process_status);
    if (timing) {
      timing->process_output_ms += ElapsedMs(process_output_start);
    }
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
    const auto output_copy_start = SteadyClock::now();
    if (!CopySampleBytes(produced_sample, &sample_bytes) ||
        sample_bytes.empty()) {
      if (timing) {
        timing->output_copy_ms += ElapsedMs(output_copy_start);
      }
      return S_OK;
    }
    if (timing) {
      timing->output_copy_ms += ElapsedMs(output_copy_start);
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
    if (timing && metadata.native_input_sample_retained) {
      const double lifetime_ms =
          ElapsedMs(metadata.retained_input_sample_created_at);
      timing->native_sample_lifetime_ms += lifetime_ms;
      timing->native_sample_lifetime_max_ms =
          std::max(timing->native_sample_lifetime_max_ms, lifetime_ms);
      timing->native_sample_lifetime_samples += 1;
      if (timing->native_source_mode.empty() ||
          timing->native_source_mode == "unknown") {
        timing->native_source_mode = metadata.native_source_mode;
        timing->native_source_format = metadata.native_source_format;
        timing->native_source_frame_index =
            metadata.native_source_frame_index;
        timing->native_source_age_ms = metadata.native_source_age_ms;
        timing->native_source_age_at_create_ms =
            metadata.native_source_age_at_create_ms;
        timing->native_buffer_age_ms = metadata.native_buffer_age_ms;
      }
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
    if (timing) {
      timing->output_frames += 1;
      timing->output_bytes += encoded_bytes.size();
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
    const uint16_t tracking_id =
        SourceFrameTrackingId(metadata.native_source_frame_index);
    if (tracking_id != webrtc::VideoFrame::kNotSetId) {
      encoded_image.SetVideoFrameTrackingId(std::make_optional(tracking_id));
    }
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
    DispatchEncodedImage(std::move(encoded_image), codec_specific, timing);
    ++encoded_outputs_seen_;
    return S_OK;
  }

  void DispatchEncodedImage(webrtc::EncodedImage encoded_image,
                            const webrtc::CodecSpecificInfo& codec_specific,
                            EncoderFrameTiming* timing) {
    if (!async_encoded_callback_enabled_) {
      const auto callback_start = SteadyClock::now();
      const webrtc::EncodedImageCallback::Result result =
          encoded_image_callback_->OnEncodedImage(encoded_image,
                                                 &codec_specific);
      const double callback_ms = ElapsedMs(callback_start);
      if (timing) {
        timing->encoded_callback_ms += callback_ms;
        timing->encoded_callback_invocations += 1;
      }
      (void)result;
      return;
    }

    PendingEncodedCallback pending;
    pending.encoded_image = std::move(encoded_image);
    pending.codec_specific = codec_specific;
    pending.queued_at = SteadyClock::now();
    pending.frame = frames_seen_;

    const auto enqueue_start = SteadyClock::now();
    int dropped = 0;
    size_t queue_depth = 0;
    {
      std::lock_guard<std::mutex> lock(async_encoded_callback_mutex_);
      if (async_encoded_callback_stop_) {
        dropped = 1;
      } else if (async_encoded_callback_queue_.size() >=
                 async_encoded_callback_queue_depth_limit_) {
        dropped = 1;
        async_encoded_callback_drops_seen_.fetch_add(
            1, std::memory_order_relaxed);
        if (async_encoded_callback_drop_policy_ ==
            EncodedCallbackDropPolicy::kDropOldest) {
          async_encoded_callback_queue_.pop_front();
          async_encoded_callback_queue_.push_back(std::move(pending));
        }
      } else {
        async_encoded_callback_queue_.push_back(std::move(pending));
      }
      queue_depth = async_encoded_callback_queue_.size();
    }
    async_encoded_callback_cv_.notify_one();

    if (timing) {
      timing->encoded_callback_async = true;
      timing->encoded_callback_enqueue_ms += ElapsedMs(enqueue_start);
      timing->encoded_callback_drops += dropped;
      timing->encoded_callback_queue_depth =
          std::max(timing->encoded_callback_queue_depth, queue_depth);
    }
  }

  void StartAsyncEncodedCallbackWorker() {
    StopAsyncEncodedCallbackWorker();
    {
      std::lock_guard<std::mutex> lock(async_encoded_callback_mutex_);
      async_encoded_callback_stop_ = false;
      async_encoded_callback_queue_.clear();
    }
    async_encoded_callback_thread_ = std::thread([this] {
      AsyncEncodedCallbackLoop();
    });
  }

  void StopAsyncEncodedCallbackWorker() {
    if (!async_encoded_callback_thread_.joinable()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(async_encoded_callback_mutex_);
      async_encoded_callback_stop_ = true;
      async_encoded_callback_queue_.clear();
    }
    async_encoded_callback_cv_.notify_all();
    async_encoded_callback_thread_.join();
    {
      std::lock_guard<std::mutex> lock(async_encoded_callback_mutex_);
      async_encoded_callback_stop_ = false;
    }
  }

  void AsyncEncodedCallbackLoop() {
    while (true) {
      PendingEncodedCallback pending;
      size_t queue_depth_after_pop = 0;
      {
        std::unique_lock<std::mutex> lock(async_encoded_callback_mutex_);
        async_encoded_callback_cv_.wait(lock, [this] {
          return async_encoded_callback_stop_ ||
                 !async_encoded_callback_queue_.empty();
        });
        if (async_encoded_callback_stop_) {
          return;
        }
        pending = std::move(async_encoded_callback_queue_.front());
        async_encoded_callback_queue_.pop_front();
        queue_depth_after_pop = async_encoded_callback_queue_.size();
      }

      if (!encoded_image_callback_) {
        continue;
      }
      const double queue_wait_ms = ElapsedMs(pending.queued_at);
      const auto callback_start = SteadyClock::now();
      const webrtc::EncodedImageCallback::Result result =
          encoded_image_callback_->OnEncodedImage(pending.encoded_image,
                                                 &pending.codec_specific);
      const double callback_ms = ElapsedMs(callback_start);
      async_encoded_callback_outputs_seen_.fetch_add(
          1, std::memory_order_relaxed);
      MaybeLogEncodedCallbackTiming(pending.frame, true, queue_wait_ms,
                                    callback_ms, queue_depth_after_pop,
                                    result,
                                    async_encoded_callback_drops_seen_.load(
                                        std::memory_order_relaxed));
    }
  }

  void MaybeLogEncodedCallbackTiming(
      uint64_t frame,
      bool async_callback,
      double queue_wait_ms,
      double callback_ms,
      size_t queue_depth,
      const webrtc::EncodedImageCallback::Result& result,
      uint64_t callback_drops) const {
    const double frame_budget_ms = fps_ == 0 ? 33.3 : 1000.0 / fps_;
    const bool slow = callback_ms > frame_budget_ms * 0.5;
    const bool very_slow = callback_ms > frame_budget_ms;
    const bool should_record = frame <= 3 || slow || very_slow ||
                               (async_callback && queue_wait_ms > 1.0) ||
                               callback_drops > 0;
    if (!should_record) {
      return;
    }

    std::ostringstream message;
    message << "Inter Galactic: Media Foundation H.264 encoded callback timing "
            << "frame=" << frame
            << " async=" << (async_callback ? "yes" : "no")
            << " callback_ms=" << callback_ms
            << " queue_wait_ms=" << queue_wait_ms
            << " queue_depth=" << queue_depth
            << " callback_outputs="
            << async_encoded_callback_outputs_seen_.load(
                   std::memory_order_relaxed)
            << " callback_drops=" << callback_drops
            << " result="
            << (result.error == webrtc::EncodedImageCallback::Result::OK
                    ? "ok"
                    : "send_failed")
            << " drop_next=" << (result.drop_next_frame ? "yes" : "no")
            << " slow=" << (slow ? "yes" : "no")
            << " very_slow=" << (very_slow ? "yes" : "no");
    const std::string line = message.str();
    if (very_slow || callback_drops > 0) {
      RTC_LOG(LS_INFO) << line;
    }
    AppendNativeWebrtcDiagnosticLine(line);
  }

  size_t RetainedNativeSampleCount() const {
    size_t retained = 0;
    for (const FrameMetadata& metadata : metadata_queue_) {
      if (metadata.retained_input_sample) {
        ++retained;
      }
    }
    return retained;
  }

  void MaybeRecoverFromNativeHandoffStall(
      const EncoderFrameTiming& timing) {
    if (!timing.native_nv12_used) {
      if (timing.output_frames > 0) {
        consecutive_native_no_output_frames_ = 0;
      }
      return;
    }
    if (timing.output_frames > 0) {
      consecutive_native_no_output_frames_ = 0;
      return;
    }

    ++consecutive_native_no_output_frames_;
    const size_t queue_limit = std::max<size_t>(4, fps_ / 2);
    if (native_nv12_suspended_ || metadata_queue_.size() < queue_limit ||
        consecutive_native_no_output_frames_ < queue_limit) {
      return;
    }

    const size_t queued_frames = metadata_queue_.size();
    const size_t retained_samples = RetainedNativeSampleCount();
    native_nv12_suspended_ = true;
    consecutive_native_no_output_frames_ = 0;
    consecutive_native_not_accepting_frames_ = 0;
    RTC_LOG(LS_WARNING)
        << "Inter Galactic: Media Foundation H.264 native NV12 handoff "
           "accepted frames without output; reinitializing encoder and "
           "suspending native input queue="
        << queued_frames << " retained_samples=" << retained_samples;
    std::ostringstream message;
    message
        << "Inter Galactic: Media Foundation H.264 native NV12 handoff stall "
        << "action=reinitialize_cpu_i420 queue=" << queued_frames
        << " retained_samples=" << retained_samples
        << " encoded_outputs=" << encoded_outputs_seen_
        << " threshold=" << queue_limit;
    AppendNativeWebrtcDiagnosticLine(message.str());

    Release();
    const HRESULT hr = InitializeTransform();
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING)
          << "Inter Galactic: Media Foundation H.264 reinitialize after "
             "native NV12 handoff stall failed "
          << HrToString(hr);
      AppendNativeWebrtcDiagnosticLine(
          "Inter Galactic: Media Foundation H.264 native NV12 handoff stall "
          "reinitialize failed");
    }
  }

  uint32_t NativeNotAcceptingRecoveryThreshold() const {
    const uint32_t active_threshold = std::max<uint32_t>(4, fps_ / 4);
    if (encoded_outputs_seen_ > 0) {
      return active_threshold;
    }
    // Startup can report MF_E_NOTACCEPTING before the async encoder emits its
    // first output. Keep the grace window bounded, then fall back if needed.
    return std::max<uint32_t>(active_threshold, std::max<uint32_t>(30, fps_));
  }

  HRESULT RecoverFromNativeNotAcceptingStall(
      uint32_t not_accepting_streak,
      uint32_t threshold) {
    const size_t queued_frames = metadata_queue_.size();
    const size_t retained_samples = RetainedNativeSampleCount();
    const bool startup_without_backlog =
        encoded_outputs_seen_ == 0 && queued_frames <= 1 && retained_samples == 0;
    if (startup_without_backlog &&
        native_nv12_startup_reinitialize_attempts_ < 2) {
      ++native_nv12_startup_reinitialize_attempts_;
      consecutive_native_no_output_frames_ = 0;
      consecutive_native_not_accepting_frames_ = 0;
      RTC_LOG(LS_WARNING)
          << "Inter Galactic: Media Foundation H.264 native NV12 startup hit "
             "repeated MF_E_NOTACCEPTING; reinitializing encoder without "
             "suspending native input streak="
          << not_accepting_streak << " threshold=" << threshold
          << " startup_reinitializes="
          << native_nv12_startup_reinitialize_attempts_
          << " queue=" << queued_frames
          << " retained_samples=" << retained_samples;
      std::ostringstream message;
      message
          << "Inter Galactic: Media Foundation H.264 native NV12 startup "
          << "not_accepting recovery "
          << "action=reinitialize_retry_native queue=" << queued_frames
          << " retained_samples=" << retained_samples
          << " encoded_outputs=" << encoded_outputs_seen_
          << " startup_reinitializes="
          << native_nv12_startup_reinitialize_attempts_
          << " not_accepting_streak=" << not_accepting_streak
          << " threshold=" << threshold;
      AppendNativeWebrtcDiagnosticLine(message.str());

      Release();
      const HRESULT hr = InitializeTransform();
      if (FAILED(hr)) {
        RTC_LOG(LS_WARNING)
            << "Inter Galactic: Media Foundation H.264 reinitialize after "
               "native NV12 startup not_accepting failed "
            << HrToString(hr);
        AppendNativeWebrtcDiagnosticLine(
            "Inter Galactic: Media Foundation H.264 native NV12 startup "
            "not_accepting reinitialize failed");
      }
      return hr;
    }

    native_nv12_suspended_ = true;
    consecutive_native_no_output_frames_ = 0;
    consecutive_native_not_accepting_frames_ = 0;
    RTC_LOG(LS_WARNING)
        << "Inter Galactic: Media Foundation H.264 native NV12 handoff hit "
           "repeated MF_E_NOTACCEPTING; reinitializing encoder and "
           "suspending native input streak="
        << not_accepting_streak << " threshold=" << threshold
        << " queue=" << queued_frames
        << " retained_samples=" << retained_samples;
    std::ostringstream message;
    message
        << "Inter Galactic: Media Foundation H.264 native NV12 handoff stall "
        << "action=reinitialize_cpu_i420_not_accepting queue="
        << queued_frames << " retained_samples=" << retained_samples
        << " encoded_outputs=" << encoded_outputs_seen_
        << " not_accepting_streak=" << not_accepting_streak
        << " threshold=" << threshold;
    AppendNativeWebrtcDiagnosticLine(message.str());

    Release();
    const HRESULT hr = InitializeTransform();
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING)
          << "Inter Galactic: Media Foundation H.264 reinitialize after "
             "native NV12 not_accepting stall failed "
          << HrToString(hr);
      AppendNativeWebrtcDiagnosticLine(
          "Inter Galactic: Media Foundation H.264 native NV12 not_accepting "
          "stall reinitialize failed");
    }
    return hr;
  }

  void MaybeLogEncoderTiming(const EncoderFrameTiming& timing,
                             const char* stage) const {
    const double frame_budget_ms = fps_ == 0 ? 33.3 : 1000.0 / fps_;
    const double native_sample_lifetime_avg_ms =
        timing.native_sample_lifetime_samples == 0
            ? 0.0
            : timing.native_sample_lifetime_ms /
                  static_cast<double>(timing.native_sample_lifetime_samples);
    const bool slow = timing.total_ms > frame_budget_ms * 1.25;
    const bool very_slow = timing.total_ms > frame_budget_ms * 2.0;
    const uint64_t cadence = std::max<uint32_t>(1, fps_);
    const bool first_frames = frames_seen_ <= 3;
    const bool regular_cadence = frames_seen_ % cadence == 0;
    const bool slow_cadence = slow && frames_seen_ % 15 == 0;
    const bool stage_is_not_accepting =
        stage != nullptr && std::strncmp(stage, "not_accepting", 13) == 0;
    const bool should_record = first_frames || regular_cadence ||
                               slow_cadence || very_slow ||
                               timing.native_nv12_sample_failed ||
                               timing.native_nv12_suspended ||
                               timing.native_ready_fence_timeout ||
                               timing.native_ready_fence_wait_ms >
                                   frame_budget_ms * 0.5;
    if (!should_record) {
      return;
    }

    std::ostringstream message;
    message << "Inter Galactic: Media Foundation H.264 encoder timing "
            << "frame=" << frames_seen_ << " stage=" << stage << " size="
            << width_ << "x" << height_ << " target_fps=" << fps_
            << " target_bitrate_bps=" << bitrate_bps_
            << " rate_control_mode=" << rate_control_mode_.label
            << " input_path="
            << (timing.native_nv12_used ? "native_nv12" : "cpu_i420")
            << " native_input="
            << (timing.native_nv12_input ? "yes" : "no")
            << " native_sample_failed="
            << (timing.native_nv12_sample_failed ? "yes" : "no")
            << " native_suspended="
            << (timing.native_nv12_suspended ? "yes" : "no")
            << " native_ready_fence="
            << (timing.native_ready_fence ? "yes" : "no")
            << " native_ready_fence_timeout="
            << (timing.native_ready_fence_timeout ? "yes" : "no")
            << " native_source_mode="
            << (timing.native_source_mode.empty() ? "unknown"
                                                  : timing.native_source_mode)
            << " native_source_format=" << timing.native_source_format
            << " native_source_frame=" << timing.native_source_frame_index
            << " frame_id=" << timing.native_source_frame_index
            << " video_frame_tracking_id="
            << SourceFrameTrackingId(timing.native_source_frame_index)
            << " video_frame_tracking_id_set="
            << (SourceFrameTrackingId(timing.native_source_frame_index) !=
                        webrtc::VideoFrame::kNotSetId
                    ? "yes"
                    : "no")
            << " native_source_age_ms=" << timing.native_source_age_ms
            << " frame_age_ms=" << timing.native_source_age_ms
            << " native_source_age_at_create_ms="
            << timing.native_source_age_at_create_ms
            << " native_buffer_age_ms=" << timing.native_buffer_age_ms
            << " native_sample_lifetime_ms="
            << native_sample_lifetime_avg_ms
            << " native_sample_lifetime_max_ms="
            << timing.native_sample_lifetime_max_ms
            << " native_sample_lifetime_samples="
            << timing.native_sample_lifetime_samples
            << " native_adapter_luid=" << timing.native_adapter_luid
            << " native_adapter_vendor_id="
            << timing.native_adapter_vendor_id
            << " native_adapter_device_id="
            << timing.native_adapter_device_id
            << " budget_ms=" << frame_budget_ms << " total_ms="
            << timing.total_ms << " to_i420_ms=" << timing.to_i420_ms
            << " nv12_ms=" << timing.convert_nv12_ms
            << " create_sample_ms=" << timing.create_sample_ms
            << " input_copy_ms=" << timing.input_copy_ms
            << " native_ready_fence_wait_ms="
            << timing.native_ready_fence_wait_ms
            << " process_input_ms=" << timing.process_input_ms
            << " pre_drain_ms=" << timing.pre_drain_ms
            << " retry_drain_ms=" << timing.retry_drain_ms
            << " post_drain_ms=" << timing.post_drain_ms
            << " process_output_ms=" << timing.process_output_ms
            << " output_copy_ms=" << timing.output_copy_ms
            << " encoded_callback_ms=" << timing.encoded_callback_ms
            << " encode_callback_ms=" << timing.encoded_callback_ms
            << " encoded_callback_enqueue_ms="
            << timing.encoded_callback_enqueue_ms
            << " encoded_callback_invocations="
            << timing.encoded_callback_invocations
            << " encoded_callback_async="
            << (timing.encoded_callback_async ? "yes" : "no")
            << " encoded_callback_queue_depth="
            << timing.encoded_callback_queue_depth
            << " encoded_callback_drops=" << timing.encoded_callback_drops
            << " outputs=" << timing.output_frames
            << " output_bytes=" << timing.output_bytes
            << " queue=" << metadata_queue_.size()
            << " retained_samples=" << RetainedNativeSampleCount()
            << " encoded_outputs=" << encoded_outputs_seen_
            << " async=" << (event_generator_ ? "yes" : "no")
            << " slow=" << (slow ? "yes" : "no")
            << " very_slow=" << (very_slow ? "yes" : "no");
    const std::string line = message.str();
    const bool abnormal_stage =
        stage != nullptr && std::strcmp(stage, "ok_native_nv12") != 0 &&
        std::strcmp(stage, "ok_i420") != 0 && std::strcmp(stage, "ok") != 0;
    const bool should_forward_to_app_log =
        first_frames || slow_cadence || very_slow ||
        (abnormal_stage && !stage_is_not_accepting) ||
        timing.native_nv12_sample_failed || timing.native_nv12_suspended ||
        timing.native_ready_fence_timeout;
    if (should_forward_to_app_log) {
      RTC_LOG(LS_INFO) << line;
    }
    AppendNativeWebrtcDiagnosticLine(line);
  }

  void ForceKeyFrame() {
    if (!transform_) {
      return;
    }
    SetCodecApiUint32(transform_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1,
                      "force key frame", false);
  }

  webrtc::H264PacketizationMode packetization_mode_;
  webrtc::VideoCodec codec_;
  webrtc::EncodedImageCallback* encoded_image_callback_ = nullptr;
  ComPtr<IMFTransform> transform_;
  ComPtr<IMFMediaEventGenerator> event_generator_;
  ComPtr<IMFDXGIDeviceManager> dxgi_device_manager_;
  ComPtr<ID3D11Device> native_d3d_device_;
  D3dAdapterDiagnostics native_adapter_diagnostics_;
  ComPtr<IMFMediaType> output_type_;
  MFT_OUTPUT_STREAM_INFO output_stream_info_ = {};
  std::vector<uint8_t> nv12_buffer_;
  std::deque<FrameMetadata> metadata_queue_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t fps_ = 30;
  uint32_t bitrate_bps_ = 2500000;
  RateControlModeConfig rate_control_mode_;
  uint64_t frames_seen_ = 0;
  uint64_t encoded_outputs_seen_ = 0;
  std::atomic<uint64_t> async_encoded_callback_outputs_seen_{0};
  std::atomic<uint64_t> async_encoded_callback_drops_seen_{0};
  uint32_t consecutive_native_no_output_frames_ = 0;
  uint32_t consecutive_native_not_accepting_frames_ = 0;
  uint32_t native_nv12_startup_reinitialize_attempts_ = 0;
  size_t max_payload_size_ = 0;
  LONGLONG sample_duration_hns_ = 333333;
  UINT dxgi_device_manager_token_ = 0;
  bool warned_unknown_layout_ = false;
  bool native_nv12_suspended_ = false;
  bool async_encoded_callback_enabled_ = false;
  uint32_t async_encoded_callback_queue_depth_limit_ = 2;
  EncodedCallbackDropPolicy async_encoded_callback_drop_policy_ =
      EncodedCallbackDropPolicy::kDropNewest;
  mutable std::mutex async_encoded_callback_mutex_;
  std::condition_variable async_encoded_callback_cv_;
  std::deque<PendingEncodedCallback> async_encoded_callback_queue_;
  std::thread async_encoded_callback_thread_;
  bool async_encoded_callback_stop_ = false;
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
