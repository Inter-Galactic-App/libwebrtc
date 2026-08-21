#include "src/win/intergalactic_game_capture_video_capturer.h"

#include <d3d11.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_rotation.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"
#include "src/win/intergalactic_d3d11_nv12_buffer.h"
#include "src/win/intergalactic_game_capture_config.h"
#include "src/win/intergalactic_native_config.h"
#include "src/win/intergalactic_native_diagnostics.h"
#include "src/win/intergalactic_proof_frame_writer.h"
#include "third_party/libyuv/include/libyuv.h"

namespace libwebrtc {
namespace {

using intergalactic::win::AnalyzeBgra;
using intergalactic::win::BgraVisibilityStats;
using intergalactic::win::DeliveryRepeatPolicy;
using intergalactic::win::DeliveryRepeatPolicyName;
using intergalactic::win::GameCaptureSourceMode;
using intergalactic::win::GameCaptureSourceModeFromString;
using intergalactic::win::GameCaptureSourceModeName;
using intergalactic::win::NativeNv12ReadyPolicy;
using intergalactic::win::NativeNv12ReadyPolicyName;
using intergalactic::win::ReadBooleanEnvironment;
using intergalactic::win::ReadDeliveryQueueDepth;
using intergalactic::win::ReadDeliveryRepeatPolicy;
using intergalactic::win::ReadEnvironmentFlag;
using intergalactic::win::ReadEnvironmentUint32;
using intergalactic::win::ReadNativeNv12MaxPendingSlots;
using intergalactic::win::ReadNativeNv12OnFrameBackpressureFrameLimit;
using intergalactic::win::ReadNativeNv12OnFrameBackpressureSuspendEnabled;
using intergalactic::win::ReadNativeNv12OnFrameBackpressureThresholdMs;
using intergalactic::win::ReadNativeNv12PendingPollMs;
using intergalactic::win::ReadNativeNv12ReadyDrainDepth;
using intergalactic::win::ReadNativeNv12ReadyPolicy;
using intergalactic::win::ReadNativeNv12SingleInFlightEnabled;
using intergalactic::win::WebrtcProofDirectory;
using intergalactic::win::WriteBgraBmp;
using Microsoft::WRL::ComPtr;

struct D3dAdapterDiagnostics {
  bool available = false;
  std::string luid = "unknown";
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
};

std::string LuidLabel(const LUID& luid) {
  return std::to_string(luid.HighPart) + ":" + std::to_string(luid.LowPart);
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

constexpr uint32_t kProtocolMagic = 0x43474749u;
// Must stay in lockstep with game_capture_protocol.h. v4 adds the seqlock
// `state_sequence` field (repurposed spare, same struct size) so consumers can
// reject torn reads of the producer's multi-field publishes.
constexpr uint32_t kSharedTextureStateVersion = 4;
constexpr int kRingDepth = 3;
// Bounded seqlock read attempts before treating the shared-state snapshot as
// unavailable for this tick. A stuck-odd sequence means the producer is
// mid-write (or died mid-write); giving up keeps the consumer loop live rather
// than spinning, and the next frame event will retry.
constexpr int kMaxSharedStateSeqReadAttempts = 16;
// Consumer keyed-mutex acquire timeout. The consumer holds a slot only across a
// single CopyResource into a private texture, so this only needs to outlast a
// producer copy; on timeout the tick is skipped and the next frame retries.
constexpr DWORD kConsumerKeyedMutexTimeoutMs = 8;
// Single-key model shared with the producer (see game_capture_protocol.h).
constexpr uint64_t kKeyedMutexAcquireKey = 0;
constexpr int kGpuReadbackRingDepth = 24;
constexpr int kGpuNv12RingDepth = 16;
constexpr size_t kGpuNv12MaxPendingSlots = kGpuNv12RingDepth;
constexpr uint64_t kMaxGpuReadbackLatencyFrames = 12;
constexpr int kMaxStableGameHookReadbackWidth = 1280;
constexpr int kMaxStableGameHookReadbackHeight = 720;
constexpr bool kEnableNativeNv12EncoderHandoff = true;
constexpr uint32_t kDefaultNativeNv12WarmupI420Frames = 2;
constexpr uint64_t kNativeNv12R10FailureDisableThreshold = 3;
constexpr int kDeliveryLateGraceMs = 10;
constexpr int kNativeNv12UsefulFrameAgeMultiplier = 2;
constexpr char kNativeNv12WarmupI420FramesEnv[] =
    "INTERGALACTIC_GAME_CAPTURE_NATIVE_NV12_WARMUP_I420_FRAMES";
constexpr uint32_t kDefaultNativeNv12LateReadyDropThresholdMs = 34;

uint16_t SourceFrameTrackingId(uint64_t source_frame_index) {
  if (source_frame_index == 0) {
    return webrtc::VideoFrame::kNotSetId;
  }
  return static_cast<uint16_t>(((source_frame_index - 1) % 65535) + 1);
}

constexpr char kNativeNv12DropLateReadyEnv[] =
    "INTERGALACTIC_GAME_CAPTURE_NV12_DROP_LATE_READY";
constexpr char kNativeNv12LateReadyDropThresholdMsEnv[] =
    "INTERGALACTIC_GAME_CAPTURE_NV12_LATE_READY_DROP_MS";
constexpr char kNativeNv12AdmissionMaxSourceAgeMsEnv[] =
    "INTERGALACTIC_GAME_CAPTURE_NV12_ADMISSION_MAX_SOURCE_AGE_MS";
constexpr char kSourceFrameVisualMarkerEnv[] =
    "INTERGALACTIC_GAME_CAPTURE_SOURCE_FRAME_VISUAL_MARKER";
constexpr char kNativeAdmissionStrictDeadlineEnv[] =
    "INTERGALACTIC_GAME_CAPTURE_STRICT_ADMISSION_CLOCK";
constexpr char kNativeNv12GpuQueueBackoffEnv[] =
    "INTERGALACTIC_GAME_CAPTURE_NV12_GPU_QUEUE_BACKOFF";
constexpr char kNativeNv12AdmissionMailboxEnv[] =
    "INTERGALACTIC_GAME_CAPTURE_NV12_ADMISSION_MAILBOX";
constexpr char kNativeNv12GpuQueueBackoffThresholdFramesEnv[] =
    "INTERGALACTIC_GAME_CAPTURE_NV12_GPU_QUEUE_BACKOFF_THRESHOLD_FRAMES";
constexpr char kNativeNv12GpuQueueBackoffDurationFramesEnv[] =
    "INTERGALACTIC_GAME_CAPTURE_NV12_GPU_QUEUE_BACKOFF_DURATION_FRAMES";
constexpr uint32_t kDefaultNativeNv12GpuQueueBackoffThresholdFrames = 2;
constexpr uint32_t kDefaultNativeNv12GpuQueueBackoffDurationFrames = 1;
constexpr char kConsumerGpuThreadPriorityEnv[] =
    "INTERGALACTIC_GAME_CAPTURE_GPU_THREAD_PRIORITY";
constexpr size_t kMinimumHelperHookTargetFps = 60;
constexpr char kHelperHookTargetFpsEnv[] =
    "INTERGALACTIC_GAME_CAPTURE_HELPER_HOOK_TARGET_FPS";
constexpr int kMaxHelperStartWaitMs = 6000;
constexpr int kHelperStopWaitMs = 15000;
constexpr int kLiveDurationMs = 60 * 60 * 1000;

using D3DCompileProc = HRESULT(WINAPI*)(LPCVOID src_data, SIZE_T src_data_size,
                                        LPCSTR source_name,
                                        const D3D_SHADER_MACRO* defines,
                                        ID3DInclude* include, LPCSTR entrypoint,
                                        LPCSTR target, UINT flags1, UINT flags2,
                                        ID3DBlob** code,
                                        ID3DBlob** error_messages);

using AvSetMmThreadCharacteristicsWProc = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
using AvRevertMmThreadCharacteristicsProc = BOOL(WINAPI*)(HANDLE);

struct GpuScaleMarkerConstants {
  float output_width = 0.0f;
  float output_height = 0.0f;
  uint32_t source_frame_tracking_id = 0;
  uint32_t enabled = 0;
  uint32_t source_frame_qpc_low = 0;
  uint32_t source_frame_qpc_high = 0;
  uint32_t marker_version = 0;
  uint32_t reserved = 0;
};

static_assert(sizeof(GpuScaleMarkerConstants) % 16 == 0,
              "D3D11 constant buffers must be 16-byte aligned.");

size_t ResolveHelperHookTargetFps(size_t target_fps) {
  const auto default_target = static_cast<uint32_t>(
      std::clamp<size_t>(std::max(target_fps, kMinimumHelperHookTargetFps), 1,
                         kMinimumHelperHookTargetFps));
  return ReadEnvironmentUint32(
      kHelperHookTargetFpsEnv, default_target, 1,
      static_cast<uint32_t>(kMinimumHelperHookTargetFps));
}

bool ReadConsumerGpuThreadPriority(int* priority) {
  if (priority == nullptr) {
    return false;
  }
  char buffer[32] = {};
  size_t length = 0;
  if (intergalactic::win::ReadEnvironmentValue(
          kConsumerGpuThreadPriorityEnv, buffer, sizeof(buffer), &length) !=
      intergalactic::win::EnvironmentValueStatus::kPresent) {
    return false;
  }
  char* end = nullptr;
  const long parsed = std::strtol(buffer, &end, 10);
  if (end == buffer || end == nullptr || *end != '\0') {
    return false;
  }
  *priority = std::clamp<int>(static_cast<int>(parsed), -7, 7);
  return true;
}

struct MmcssThreadRegistration {
  HMODULE library = nullptr;
  AvRevertMmThreadCharacteristicsProc revert = nullptr;
  HANDLE task_handle = nullptr;

  MmcssThreadRegistration() = default;
  MmcssThreadRegistration(const MmcssThreadRegistration&) = delete;
  MmcssThreadRegistration& operator=(const MmcssThreadRegistration&) = delete;

  MmcssThreadRegistration(MmcssThreadRegistration&& other) noexcept {
    library = other.library;
    revert = other.revert;
    task_handle = other.task_handle;
    other.library = nullptr;
    other.revert = nullptr;
    other.task_handle = nullptr;
  }

  MmcssThreadRegistration& operator=(MmcssThreadRegistration&& other) noexcept {
    if (this != &other) {
      Reset();
      library = other.library;
      revert = other.revert;
      task_handle = other.task_handle;
      other.library = nullptr;
      other.revert = nullptr;
      other.task_handle = nullptr;
    }
    return *this;
  }

  ~MmcssThreadRegistration() { Reset(); }

  void Reset() {
    if (task_handle != nullptr && revert != nullptr) {
      revert(task_handle);
    }
    task_handle = nullptr;
    revert = nullptr;
    if (library != nullptr) {
      FreeLibrary(library);
      library = nullptr;
    }
  }
};

enum class CaptureBackend : uint32_t {
  kUnknown = 0,
  kD3D11PresentHook = 1,
  kVulkan = 2,
  kD3D12 = 3,
  kOpenGL = 4,
};

enum class SourceFormat : uint32_t {
  kUnknown = 0,
  kBgra8 = 1,
  kRgba8 = 2,
  kR10G10B10A2 = 3,
  kNv12 = 4,
  kP010 = 5,
  kOther = 255,
};

enum class ColorSpace : uint32_t {
  kUnknown = 0,
  kSdr = 1,
  kHdr10 = 2,
};

enum class SyncKind : uint32_t {
  kUnknown = 0,
  kNone = 1,
  kEvent = 2,
  kKeyedMutex = 3,
  kFence = 4,
  kSemaphore = 5,
};

enum class FrameReadyState : uint32_t {
  kUnknown = 0,
  kEmpty = 1,
  kReady = 2,
  kStale = 3,
  kFailed = 4,
};

enum class FailureReason : uint32_t {
  kNone = 0,
  kUnsupportedProtectedTarget = 1,
  kUnsupportedFormat = 2,
  kSharedTextureUnavailable = 3,
  kSyncTimeout = 4,
  kDeviceLost = 5,
  kConversionFailed = 6,
  kEncoderNotAccepting = 7,
  kAttachFailed = 8,
};

constexpr int kDummyNv12RingDepth = 4;

const char* CaptureBackendName(CaptureBackend backend) {
  switch (backend) {
    case CaptureBackend::kD3D11PresentHook:
      return "d3d11";
    case CaptureBackend::kVulkan:
      return "vulkan";
    case CaptureBackend::kD3D12:
      return "d3d12";
    case CaptureBackend::kOpenGL:
      return "opengl";
    case CaptureBackend::kUnknown:
    default:
      return "unknown";
  }
}

const char* SourceFormatName(SourceFormat format) {
  switch (format) {
    case SourceFormat::kBgra8:
      return "bgra8";
    case SourceFormat::kRgba8:
      return "rgba8";
    case SourceFormat::kR10G10B10A2:
      return "r10g10b10a2";
    case SourceFormat::kNv12:
      return "nv12";
    case SourceFormat::kP010:
      return "p010";
    case SourceFormat::kOther:
      return "other";
    case SourceFormat::kUnknown:
    default:
      return "unknown";
  }
}

const char* ColorSpaceName(ColorSpace color_space) {
  switch (color_space) {
    case ColorSpace::kSdr:
      return "sdr";
    case ColorSpace::kHdr10:
      return "hdr10";
    case ColorSpace::kUnknown:
    default:
      return "unknown";
  }
}

const char* SyncKindName(SyncKind sync_kind) {
  switch (sync_kind) {
    case SyncKind::kNone:
      return "none";
    case SyncKind::kEvent:
      return "event";
    case SyncKind::kKeyedMutex:
      return "keyed_mutex";
    case SyncKind::kFence:
      return "fence";
    case SyncKind::kSemaphore:
      return "semaphore";
    case SyncKind::kUnknown:
    default:
      return "unknown";
  }
}

const char* FrameReadyStateName(FrameReadyState state) {
  switch (state) {
    case FrameReadyState::kEmpty:
      return "empty";
    case FrameReadyState::kReady:
      return "ready";
    case FrameReadyState::kStale:
      return "stale";
    case FrameReadyState::kFailed:
      return "failed";
    case FrameReadyState::kUnknown:
    default:
      return "unknown";
  }
}

const char* FailureReasonName(FailureReason reason) {
  switch (reason) {
    case FailureReason::kNone:
      return "none";
    case FailureReason::kUnsupportedProtectedTarget:
      return "unsupported_protected_target";
    case FailureReason::kUnsupportedFormat:
      return "unsupported_format";
    case FailureReason::kSharedTextureUnavailable:
      return "shared_texture_unavailable";
    case FailureReason::kSyncTimeout:
      return "sync_timeout";
    case FailureReason::kDeviceLost:
      return "device_lost";
    case FailureReason::kConversionFailed:
      return "conversion_failed";
    case FailureReason::kEncoderNotAccepting:
      return "encoder_not_accepting";
    case FailureReason::kAttachFailed:
      return "attach_failed";
    default:
      return "unknown";
  }
}

SourceFormat SourceFormatFromDxgiFormat(uint32_t dxgi_format) {
  switch (dxgi_format) {
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      return SourceFormat::kR10G10B10A2;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      return SourceFormat::kRgba8;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
      return SourceFormat::kBgra8;
    case DXGI_FORMAT_NV12:
      return SourceFormat::kNv12;
    case DXGI_FORMAT_P010:
      return SourceFormat::kP010;
    case DXGI_FORMAT_UNKNOWN:
      return SourceFormat::kUnknown;
    default:
      return SourceFormat::kOther;
  }
}

struct SharedTextureSlotState {
  uint64_t shared_handle = 0;
  uint64_t frame_index = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t dxgi_format = 0;
  uint32_t sample_count = 1;
};

struct SharedTextureState {
  uint32_t magic = kProtocolMagic;
  uint32_t version = kSharedTextureStateVersion;
  uint32_t ring_depth = kRingDepth;
  uint32_t latest_slot_index = 0;
  uint64_t generation = 0;
  uint64_t latest_frame_index = 0;
  uint64_t latest_qpc = 0;
  uint64_t present_count = 0;
  uint64_t copied_frames = 0;
  uint64_t dropped_frames = 0;
  uint64_t overwritten_frames = 0;
  uint32_t backbuffer_width = 0;
  uint32_t backbuffer_height = 0;
  uint32_t backbuffer_format = 0;
  uint32_t sample_count = 1;
  uint32_t shared_texture_supported = 0;
  // Seqlock counter (was `reserved`); see game_capture_protocol.h. Even = stable
  // published snapshot, odd = producer write in progress.
  uint32_t state_sequence = 0;
  SharedTextureSlotState slots[kRingDepth];
  uint32_t source_api = static_cast<uint32_t>(CaptureBackend::kUnknown);
  uint32_t source_format = static_cast<uint32_t>(SourceFormat::kUnknown);
  uint32_t color_space = static_cast<uint32_t>(ColorSpace::kUnknown);
  uint32_t hdr_flags = 0;
  uint32_t sync_kind = static_cast<uint32_t>(SyncKind::kUnknown);
  uint32_t ready_state = static_cast<uint32_t>(FrameReadyState::kUnknown);
  uint32_t failure_reason = static_cast<uint32_t>(FailureReason::kNone);
  uint32_t contract_reserved = 0;
  uint64_t producer_present_gap_qpc_total = 0;
  uint64_t producer_present_gap_qpc_max = 0;
  uint64_t producer_present_gap_samples = 0;
  uint64_t producer_capture_gap_qpc_total = 0;
  uint64_t producer_capture_gap_qpc_max = 0;
  uint64_t producer_capture_gap_samples = 0;
  uint64_t producer_present_to_publish_qpc_total = 0;
  uint64_t producer_present_to_publish_qpc_max = 0;
  uint64_t producer_present_to_publish_samples = 0;
  uint64_t producer_copy_qpc_total = 0;
  uint64_t producer_copy_qpc_max = 0;
  uint64_t producer_copy_samples = 0;
  uint64_t producer_resolve_qpc_total = 0;
  uint64_t producer_resolve_qpc_max = 0;
  uint64_t producer_resolve_samples = 0;
  uint64_t producer_throttled_frames = 0;
  uint64_t producer_latest_publish_qpc = 0;
};

std::wstring Utf8ToWide(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
  if (size <= 1) {
    return {};
  }
  std::wstring result(size - 1, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value, -1, result.data(), size);
  return result;
}

std::string WideToUtf8(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                       static_cast<int>(value.size()), nullptr,
                                       0, nullptr, nullptr);
  if (size <= 0) {
    return {};
  }
  std::string result(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      result.data(), size, nullptr, nullptr);
  return result;
}

std::string JsonEscape(const std::string& value) {
  std::ostringstream out;
  for (char c : value) {
    switch (c) {
      case '\\':
        out << "\\\\";
        break;
      case '"':
        out << "\\\"";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        out << c;
        break;
    }
  }
  return out.str();
}

std::wstring QuoteArg(const std::wstring& value) {
  std::wstring out = L"\"";
  for (wchar_t c : value) {
    if (c == L'"') {
      out.append(L"\\\"");
    } else {
      out.push_back(c);
    }
  }
  out.push_back(L'"');
  return out;
}

std::wstring ParentDirectory(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return {};
  }
  return path.substr(0, slash);
}

std::wstring JoinPath(const std::wstring& directory,
                      const std::wstring& child) {
  if (directory.empty()) {
    return child;
  }
  const wchar_t last = directory.back();
  if (last == L'\\' || last == L'/') {
    return directory + child;
  }
  return directory + L"\\" + child;
}

std::wstring ModuleDirectory() {
  wchar_t module_path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, module_path, MAX_PATH);
  std::wstring path(module_path);
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return {};
  }
  return path.substr(0, slash);
}

bool FileExists(const std::wstring& path) {
  if (path.empty()) {
    return false;
  }
  const DWORD attrs = GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES &&
         (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool WriteUtf8File(const wchar_t* path, const std::string& text) {
  if (path == nullptr || path[0] == L'\0') {
    return false;
  }
  HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD written = 0;
  const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()),
                            &written, nullptr);
  CloseHandle(file);
  return ok && written == text.size();
}

std::wstring ResolveHelperPath(const std::wstring& requested) {
  if (FileExists(requested)) {
    return requested;
  }
  const std::wstring module_dir = ModuleDirectory();
  if (!module_dir.empty()) {
    const std::wstring beside_app =
        module_dir + L"\\intergalactic_game_capture_helper.exe";
    if (FileExists(beside_app)) {
      return beside_app;
    }
  }
  const std::wstring repo_debug =
      L"Z:\\Matrix_Dev\\intergalactic-app\\inter-galactic\\plugins\\"
      L"intergalactic_game_capture\\windows\\build\\Debug\\"
      L"intergalactic_game_capture_helper.exe";
  if (FileExists(repo_debug)) {
    return repo_debug;
  }
  return {};
}

std::string HexSessionId() {
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  uint64_t state =
      static_cast<uint64_t>(std::chrono::high_resolution_clock::now()
                                .time_since_epoch()
                                .count()) ^
      (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^
      static_cast<uint64_t>(GetCurrentThreadId()) ^
      static_cast<uint64_t>(counter.QuadPart);
  std::ostringstream out;
  out << "wrtc";
  for (int i = 0; i < 8; ++i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    char buffer[4];
    snprintf(buffer, sizeof(buffer), "%02x",
             static_cast<unsigned int>(state & 0xffu));
    out << buffer;
  }
  return out.str();
}

std::wstring WideFromAscii(const std::string& value) {
  return std::wstring(value.begin(), value.end());
}

std::string HResultHex(HRESULT hr) {
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned int>(hr));
  return std::string(buffer);
}

int MakeEven(int value) {
  if (value <= 2) {
    return 2;
  }
  return value % 2 == 0 ? value : value - 1;
}

int ContainFitDimension(int source, int max, double scale) {
  if (max <= 0 || scale >= 1.0) {
    return MakeEven(source);
  }
  int scaled = static_cast<int>(source * scale + 0.5);
  if (scaled > max) {
    scaled = max;
  }
  return MakeEven(scaled);
}

void AppendNativeDiagnosticLine(const std::string& line) {
  intergalactic::win::NativeDiagnosticOptions options;
  options.max_file_bytes = 1024 * 1024;
  options.flush_file = false;
  options.log_to_rtc = true;
  intergalactic::win::AppendNativeWebrtcDiagnosticLine(line, options);
}

bool IsBgra(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_B8G8R8A8_UNORM ||
         format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
         format == DXGI_FORMAT_B8G8R8X8_UNORM ||
         format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

bool IsRgba(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_R8G8B8A8_UNORM ||
         format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

bool IsR10G10B10A2(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_R10G10B10A2_UNORM;
}

class IntergalacticGameCaptureVideoCapturer
    : public webrtc::internal::VideoCapturer {
 public:
  IntergalacticGameCaptureVideoCapturer(
      std::wstring helper_path, uint32_t target_process_id, size_t max_width,
      size_t max_height, size_t target_fps,
      GameCaptureSourceMode source_mode = GameCaptureSourceMode::kHelperD3d11,
      std::wstring helper_output_root = {})
      : helper_path_(std::move(helper_path)),
        helper_output_root_(std::move(helper_output_root)),
        target_process_id_(target_process_id),
        source_mode_(source_mode),
        max_width_(std::max<size_t>(2, max_width)),
        max_height_(std::max<size_t>(2, max_height)),
        target_fps_(std::clamp<size_t>(target_fps, 1, 60)),
        native_admission_strict_deadline_enabled_(
            ReadBooleanEnvironment(kNativeAdmissionStrictDeadlineEnv)),
        delivery_queue_depth_(ReadDeliveryQueueDepth()),
        delivery_repeat_policy_(ReadDeliveryRepeatPolicy()),
        native_nv12_ready_policy_(ReadNativeNv12ReadyPolicy()),
        native_nv12_pending_poll_ms_(ReadNativeNv12PendingPollMs()),
        native_nv12_max_pending_slots_(ReadNativeNv12MaxPendingSlots()),
        native_nv12_ready_drain_depth_(ReadNativeNv12ReadyDrainDepth()),
        native_nv12_force_disabled_(ReadBooleanEnvironment(
            "INTERGALACTIC_GAME_CAPTURE_DISABLE_NATIVE_NV12")),
        native_nv12_warmup_i420_frames_(
            ReadEnvironmentUint32(kNativeNv12WarmupI420FramesEnv,
                                  kDefaultNativeNv12WarmupI420Frames, 0, 120)),
        native_nv12_onframe_backpressure_suspend_enabled_(
            ReadNativeNv12OnFrameBackpressureSuspendEnabled()),
        native_nv12_onframe_backpressure_threshold_ms_(
            ReadNativeNv12OnFrameBackpressureThresholdMs()),
        native_nv12_onframe_backpressure_frame_limit_(
            ReadNativeNv12OnFrameBackpressureFrameLimit()),
        native_nv12_single_in_flight_enabled_(
            ReadNativeNv12SingleInFlightEnabled()),
        native_nv12_admission_mailbox_enabled_(
            ReadEnvironmentFlag(kNativeNv12AdmissionMailboxEnv, false)),
        native_nv12_gpu_queue_backoff_enabled_(
            ReadEnvironmentFlag(kNativeNv12GpuQueueBackoffEnv, true)),
        native_nv12_gpu_queue_backoff_threshold_frames_(ReadEnvironmentUint32(
            kNativeNv12GpuQueueBackoffThresholdFramesEnv,
            kDefaultNativeNv12GpuQueueBackoffThresholdFrames, 1, 10)),
        native_nv12_gpu_queue_backoff_duration_frames_(ReadEnvironmentUint32(
            kNativeNv12GpuQueueBackoffDurationFramesEnv,
            kDefaultNativeNv12GpuQueueBackoffDurationFrames, 1, 10)),
        source_frame_visual_marker_enabled_(
            ReadBooleanEnvironment(kSourceFrameVisualMarkerEnv)),
        native_nv12_drop_late_ready_enabled_(
            ReadBooleanEnvironment(kNativeNv12DropLateReadyEnv)),
        native_nv12_late_ready_drop_threshold_ms_(ReadEnvironmentUint32(
            kNativeNv12LateReadyDropThresholdMsEnv,
            kDefaultNativeNv12LateReadyDropThresholdMs, 1, 1000)) {
    native_nv12_admission_max_source_age_ms_ = ReadEnvironmentUint32(
        kNativeNv12AdmissionMaxSourceAgeMsEnv, 0, 0, 1000);
    native_nv12_render_convert_enabled_ = ReadBooleanEnvironment(
        "INTERGALACTIC_GAME_CAPTURE_NV12_RENDER_CONVERT");
    native_keyed_mutex_enabled_ = ReadBooleanEnvironment(
        "INTERGALACTIC_GAME_CAPTURE_ENABLE_KEYED_MUTEX");
  }

  ~IntergalacticGameCaptureVideoCapturer() override { StopCapture(); }

  bool StartCapture() override {
    if (started_.load()) {
      return true;
    }
    if (source_mode_ == GameCaptureSourceMode::kHelperD3d11 &&
        (target_process_id_ == 0 || helper_path_.empty())) {
      Log("start_failed reason=missing_pid_or_helper");
      return false;
    }
    timestamp_base_source_qpc_ = 0;
    timestamp_base_us_ = 0;
    last_frame_timestamp_us_ = 0;
    last_delivery_wall_timestamp_us_ = 0;
    last_timestamp_source_qpc_ = 0;
    timestamp_delta_us_total_ = 0;
    timestamp_delta_us_max_ = 0;
    timestamp_delta_samples_ = 0;
    timestamp_delta_window_us_total_ = 0;
    timestamp_delta_window_us_max_ = 0;
    timestamp_delta_window_samples_ = 0;
    timestamp_adjustments_ = 0;
    delivery_wall_delta_us_total_ = 0;
    delivery_wall_delta_us_max_ = 0;
    delivery_wall_delta_samples_ = 0;
    delivery_wall_delta_window_us_total_ = 0;
    delivery_wall_delta_window_us_max_ = 0;
    delivery_wall_delta_window_samples_ = 0;
    source_qpc_delta_us_total_ = 0;
    source_qpc_delta_us_max_ = 0;
    source_qpc_delta_samples_ = 0;
    source_qpc_regressions_ = 0;
    source_qpc_over_2x_frames_ = 0;
    source_qpc_over_3x_frames_ = 0;
    source_qpc_under_half_frames_ = 0;
    source_latest_observed_frames_ = 0;
    source_latest_frame_gaps_ = 0;
    source_latest_frame_regressions_ = 0;
    source_latest_qpc_delta_ = 0;
    source_latest_qpc_delta_max_ = 0;
    source_latest_qpc_delta_samples_ = 0;
    source_latest_qpc_regressions_ = 0;
    source_latest_qpc_over_2x_frames_ = 0;
    source_latest_qpc_over_3x_frames_ = 0;
    source_latest_qpc_under_half_frames_ = 0;
    source_latest_observation_delta_ = 0;
    source_latest_observation_delta_max_ = 0;
    source_latest_observation_delta_samples_ = 0;
    source_latest_observation_over_2x_frames_ = 0;
    source_latest_observation_over_3x_frames_ = 0;
    source_latest_event_age_ = 0;
    source_latest_event_age_max_ = 0;
    source_latest_event_age_samples_ = 0;
    source_latest_event_age_over_1x_frames_ = 0;
    source_latest_event_age_over_2x_frames_ = 0;
    source_latest_event_age_over_3x_frames_ = 0;
    source_publish_observation_age_ = 0;
    source_publish_observation_age_max_ = 0;
    source_publish_observation_age_samples_ = 0;
    source_publish_observation_age_over_1x_frames_ = 0;
    source_publish_observation_age_over_2x_frames_ = 0;
    source_publish_observation_age_over_3x_frames_ = 0;
    last_observed_source_frame_index_ = 0;
    last_observed_source_qpc_ = 0;
    last_source_observation_qpc_ = 0;
    native_admission_deadline_due_frames_ = 0;
    native_admission_deadline_lateness_qpc_ = 0;
    native_admission_deadline_lateness_max_qpc_ = 0;
    native_admission_deadline_lateness_samples_ = 0;
    native_admission_deadline_over_1x_frames_ = 0;
    native_admission_deadline_over_2x_frames_ = 0;
    native_admission_deadline_over_3x_frames_ = 0;
    native_admission_no_source_on_deadline_frames_ = 0;
    native_admission_repeated_on_deadline_frames_ = 0;
    native_admission_submit_on_deadline_frames_ = 0;
    native_admission_submit_on_early_source_frames_ = 0;
    native_nv12_pending_on_deadline_frames_ = 0;
    native_nv12_no_pending_on_deadline_frames_ = 0;
    native_nv12_ready_on_deadline_frames_ = 0;
    native_nv12_no_ready_on_deadline_frames_ = 0;
    native_admission_source_driven_fresh_due_frames_ = 0;
    native_admission_source_qpc_due_frames_ = 0;
    native_admission_early_source_due_suppressed_frames_ = 0;
    native_admission_last_suppressed_source_frame_index_ = 0;
    native_nv12_suspended_after_onframe_backpressure_.store(false);
    native_nv12_onframe_backpressure_suspend_logged_.store(false);
    native_nv12_onframe_backpressure_streak_ = 0;
    native_nv12_onframe_backpressure_frames_ = 0;
    native_nv12_onframe_backpressure_max_ms_ = 0.0;
    native_nv12_single_in_flight_deferred_frames_ = 0;
    native_nv12_single_in_flight_deferred_fresh_frames_ = 0;
    native_nv12_single_in_flight_pending_max_ = 0;
    native_nv12_single_in_flight_deferred_source_age_qpc_ = 0;
    native_nv12_single_in_flight_deferred_source_age_max_qpc_ = 0;
    native_nv12_single_in_flight_deferred_source_age_samples_ = 0;
    native_nv12_gpu_queue_backoff_until_qpc_ = 0;
    native_nv12_gpu_queue_backoff_triggered_frames_ = 0;
    native_nv12_gpu_queue_backoff_suppressed_frames_ = 0;
    native_nv12_gpu_queue_backoff_suppressed_fresh_frames_ = 0;
    native_nv12_gpu_queue_backoff_duration_qpc_ = 0;
    native_nv12_gpu_queue_backoff_duration_max_qpc_ = 0;
    native_nv12_gpu_queue_backoff_duration_samples_ = 0;
    native_nv12_gpu_queue_backoff_trigger_blt_to_ready_qpc_ = 0;
    native_nv12_gpu_queue_backoff_trigger_blt_to_ready_max_qpc_ = 0;
    native_nv12_gpu_queue_backoff_trigger_blt_to_ready_samples_ = 0;
    native_nv12_gpu_queue_backoff_suppressed_source_age_qpc_ = 0;
    native_nv12_gpu_queue_backoff_suppressed_source_age_max_qpc_ = 0;
    native_nv12_gpu_queue_backoff_suppressed_source_age_samples_ = 0;
    ClearPendingNativeNv12Admission();
    native_nv12_admission_mailbox_stored_frames_ = 0;
    native_nv12_admission_mailbox_replaced_frames_ = 0;
    native_nv12_admission_mailbox_submitted_frames_ = 0;
    native_nv12_admission_mailbox_stale_dropped_frames_ = 0;
    native_nv12_admission_mailbox_pending_age_qpc_ = 0;
    native_nv12_admission_mailbox_pending_age_max_qpc_ = 0;
    native_nv12_admission_mailbox_pending_age_samples_ = 0;
    native_nv12_admission_mailbox_submit_source_age_qpc_ = 0;
    native_nv12_admission_mailbox_submit_source_age_max_qpc_ = 0;
    native_nv12_admission_mailbox_submit_source_age_samples_ = 0;
    gpu_nv12_ready_observed_immediate_after_blt_frames_ = 0;
    gpu_nv12_ready_observed_post_fence_registration_frames_ = 0;
    gpu_nv12_ready_observed_fence_event_frames_ = 0;
    gpu_nv12_ready_observed_source_event_frames_ = 0;
    gpu_nv12_ready_observed_wait_other_frames_ = 0;
    gpu_nv12_ready_observed_loop_idle_frames_ = 0;
    gpu_nv12_ready_observed_duplicate_skip_frames_ = 0;
    gpu_nv12_ready_observed_pre_submit_frames_ = 0;
    gpu_nv12_ready_observed_write_slot_scan_frames_ = 0;
    gpu_nv12_ready_observed_unknown_frames_ = 0;
    gpu_nv12_blt_to_ready_over_1x_frames_ = 0;
    gpu_nv12_blt_to_ready_over_2x_frames_ = 0;
    gpu_nv12_blt_to_ready_over_3x_frames_ = 0;
    startup_failure_reason_.clear();
    helper_exit_code_ = STILL_ACTIVE;
    helper_exit_code_available_ = false;
    helper_exited_before_shared_state_ = false;
    open_shared_state_wait_ms_ = 0;
    stop_requested_.store(false);
    started_.store(true);
    worker_ = std::thread([this] { Run(); });
    return true;
  }

  bool CaptureStarted() override { return started_.load(); }

  void StopCapture() override {
    if (started_.load() || worker_.joinable() || helper_process_ != nullptr) {
      Log("stop_capture requested");
    }
    started_.store(false);
    stop_requested_.store(true);
    HANDLE stop_event = stop_event_;
    if (stop_event != nullptr) {
      Log("stop_capture signaling_stop_event");
      SetEvent(stop_event);
    }
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
      worker_.join();
      Log("stop_capture worker_joined");
    }
    StopDeliveryThread();
  }

  const std::string& StartupFailureReason() const {
    return startup_failure_reason_;
  }

  std::string SmokeStatsJson(const std::string& status) const {
    const uint64_t submitted = submitted_frames_;
    const double submitted_denominator = submitted == 0 ? 1.0 : submitted;
    const double queued_denominator =
        gpu_readback_queued_frames_ == 0 ? 1.0 : gpu_readback_queued_frames_;
    const double frequency = frequency_.QuadPart == 0
                                 ? 1.0
                                 : static_cast<double>(frequency_.QuadPart);
    const double gpu_scale_ms = static_cast<double>(gpu_scale_us_) * 1000.0 /
                                frequency / queued_denominator;
    const double copy_ms = static_cast<double>(readback_copy_us_) * 1000.0 /
                           frequency / queued_denominator;
    const double map_ms = static_cast<double>(readback_map_us_) * 1000.0 /
                          frequency / submitted_denominator;
    const double convert_ms = static_cast<double>(convert_us_) * 1000.0 /
                              frequency / submitted_denominator;
    const double readback_ready_denominator =
        gpu_readback_ready_frames_ == 0
            ? 1.0
            : static_cast<double>(gpu_readback_ready_frames_);
    const double readback_latency_ms =
        static_cast<double>(readback_latency_us_) * 1000.0 / frequency /
        readback_ready_denominator;
    const double readback_latency_frames_avg =
        gpu_readback_ready_frames_ == 0
            ? 0.0
            : static_cast<double>(readback_latency_frames_total_) /
                  static_cast<double>(gpu_readback_ready_frames_);
    const uint32_t backend_contract_version =
        shared_state_ ? shared_state_->version : 0;
    const auto source_api =
        shared_state_ ? static_cast<CaptureBackend>(shared_state_->source_api)
                      : CaptureBackend::kUnknown;
    const auto source_format =
        shared_state_ && shared_state_->source_format != 0
            ? static_cast<SourceFormat>(shared_state_->source_format)
            : SourceFormatFromDxgiFormat(
                  static_cast<uint32_t>(last_source_format_));
    const auto color_space =
        shared_state_ ? static_cast<ColorSpace>(shared_state_->color_space)
                      : ColorSpace::kUnknown;
    const auto sync_kind = shared_state_
                               ? static_cast<SyncKind>(shared_state_->sync_kind)
                               : SyncKind::kUnknown;
    const auto ready_state =
        shared_state_ ? static_cast<FrameReadyState>(shared_state_->ready_state)
                      : FrameReadyState::kUnknown;
    const auto failure_reason =
        shared_state_
            ? static_cast<FailureReason>(shared_state_->failure_reason)
            : FailureReason::kNone;

    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"intergalactic.gameCaptureWebrtcSourceSmoke.v1\",\n";
    out << "  \"status\": \"" << status << "\",\n";
    out << "  \"targetProcessId\": " << target_process_id_ << ",\n";
    out << "  \"sourceMode\": \"" << GameCaptureSourceModeName(source_mode_)
        << "\",\n";
    out << "  \"startupFailureReason\": \""
        << JsonEscape(startup_failure_reason_) << "\",\n";
    out << "  \"helperOutputRoot\": \""
        << JsonEscape(WideToUtf8(helper_output_root_)) << "\",\n";
    out << "  \"helperExitedBeforeSharedState\": "
        << (helper_exited_before_shared_state_ ? "true" : "false") << ",\n";
    out << "  \"helperExitCodeAvailable\": "
        << (helper_exit_code_available_ ? "true" : "false") << ",\n";
    out << "  \"helperExitCode\": " << helper_exit_code_ << ",\n";
    out << "  \"openSharedStateWaitMs\": " << open_shared_state_wait_ms_
        << ",\n";
    out << "  \"requestedMaxWidth\": " << max_width_ << ",\n";
    out << "  \"requestedMaxHeight\": " << max_height_ << ",\n";
    out << "  \"requestedTargetFps\": " << target_fps_ << ",\n";
    out << "  \"nativeAdmissionStrictDeadlineEnabled\": "
        << (native_admission_strict_deadline_enabled_ ? "true" : "false")
        << ",\n";
    out << "  \"nativeAdmissionSourceDrivenFreshDue\": "
        << native_admission_source_driven_fresh_due_frames_ << ",\n";
    out << "  \"nativeAdmissionSourceQpcDue\": "
        << native_admission_source_qpc_due_frames_ << ",\n";
    out << "  \"nativeAdmissionEarlySourceDueSuppressed\": "
        << native_admission_early_source_due_suppressed_frames_ << ",\n";
    out << "  \"nativeAdmissionDeadlineDue\": "
        << native_admission_deadline_due_frames_ << ",\n";
    out << "  \"nativeAdmissionDeadlineLatenessMs\": "
        << AverageMetricMs(native_admission_deadline_lateness_qpc_,
                           native_admission_deadline_lateness_samples_)
        << ",\n";
    out << "  \"nativeAdmissionDeadlineLatenessMaxMs\": "
        << TicksToMs(native_admission_deadline_lateness_max_qpc_) << ",\n";
    out << "  \"nativeAdmissionDeadlineLatenessSamples\": "
        << native_admission_deadline_lateness_samples_ << ",\n";
    out << "  \"nativeAdmissionDeadlineOver1x\": "
        << native_admission_deadline_over_1x_frames_ << ",\n";
    out << "  \"nativeAdmissionDeadlineOver2x\": "
        << native_admission_deadline_over_2x_frames_ << ",\n";
    out << "  \"nativeAdmissionDeadlineOver3x\": "
        << native_admission_deadline_over_3x_frames_ << ",\n";
    out << "  \"nativeAdmissionNoSourceOnDeadline\": "
        << native_admission_no_source_on_deadline_frames_ << ",\n";
    out << "  \"nativeAdmissionRepeatedOnDeadline\": "
        << native_admission_repeated_on_deadline_frames_ << ",\n";
    out << "  \"nativeAdmissionSubmitOnDeadline\": "
        << native_admission_submit_on_deadline_frames_ << ",\n";
    out << "  \"nativeAdmissionSubmitOnEarlySource\": "
        << native_admission_submit_on_early_source_frames_ << ",\n";
    out << "  \"nativeNv12PendingOnDeadline\": "
        << native_nv12_pending_on_deadline_frames_ << ",\n";
    out << "  \"nativeNv12NoPendingOnDeadline\": "
        << native_nv12_no_pending_on_deadline_frames_ << ",\n";
    out << "  \"nativeNv12ReadyOnDeadline\": "
        << native_nv12_ready_on_deadline_frames_ << ",\n";
    out << "  \"nativeNv12NoReadyOnDeadline\": "
        << native_nv12_no_ready_on_deadline_frames_ << ",\n";
    out << "  \"sourceWidth\": " << last_source_width_ << ",\n";
    out << "  \"sourceHeight\": " << last_source_height_ << ",\n";
    out << "  \"outputWidth\": " << last_output_width_ << ",\n";
    out << "  \"outputHeight\": " << last_output_height_ << ",\n";
    out << "  \"format\": " << last_source_format_ << ",\n";
    out << "  \"backendContractVersion\": " << backend_contract_version
        << ",\n";
    out << "  \"sourceApi\": \"" << CaptureBackendName(source_api) << "\",\n";
    out << "  \"sourceApiId\": " << static_cast<uint32_t>(source_api) << ",\n";
    out << "  \"sourceFormat\": \"" << SourceFormatName(source_format)
        << "\",\n";
    out << "  \"sourceFormatId\": " << static_cast<uint32_t>(source_format)
        << ",\n";
    out << "  \"colorSpace\": \"" << ColorSpaceName(color_space) << "\",\n";
    out << "  \"syncKind\": \"" << SyncKindName(sync_kind) << "\",\n";
    out << "  \"readyState\": \"" << FrameReadyStateName(ready_state)
        << "\",\n";
    out << "  \"failureReason\": \"" << FailureReasonName(failure_reason)
        << "\",\n";
    out << "  \"consumerAdapterLuid\": \""
        << JsonEscape(consumer_adapter_diagnostics_.luid) << "\",\n";
    out << "  \"consumerAdapterVendorId\": "
        << consumer_adapter_diagnostics_.vendor_id << ",\n";
    out << "  \"consumerAdapterDeviceId\": "
        << consumer_adapter_diagnostics_.device_id << ",\n";
    out << "  \"consumerGpuThreadPriorityRequested\": "
        << (consumer_gpu_thread_priority_requested_ ? "true" : "false")
        << ",\n";
    out << "  \"consumerGpuThreadPriorityRequestedValue\": "
        << consumer_gpu_thread_priority_requested_value_ << ",\n";
    out << "  \"consumerGpuThreadPriorityApplied\": "
        << (consumer_gpu_thread_priority_applied_ ? "true" : "false") << ",\n";
    out << "  \"consumerGpuThreadPriorityBefore\": "
        << consumer_gpu_thread_priority_before_ << ",\n";
    out << "  \"consumerGpuThreadPriorityAfter\": "
        << consumer_gpu_thread_priority_after_ << ",\n";
    out << "  \"consumerGpuThreadPriorityHr\": \""
        << HResultHex(consumer_gpu_thread_priority_hr_) << "\",\n";
    out << "  \"sourceAdapterLuid\": \"unknown\",\n";
    out << "  \"crossAdapterSuspected\": \"unknown\",\n";
    out << "  \"submitted\": " << submitted_frames_ << ",\n";
    out << "  \"repeated\": " << repeated_frames_ << ",\n";
    out << "  \"duplicateSkipped\": " << duplicate_source_frame_skips_ << ",\n";
    out << "  \"deliveryQueued\": " << delivery_queued_frames_ << ",\n";
    out << "  \"deliverySubmitted\": " << delivery_submitted_frames_ << ",\n";
    out << "  \"deliveryOverwritten\": " << delivery_overwritten_frames_
        << ",\n";
    out << "  \"deliveryPacerResyncs\": " << delivery_pacer_resyncs_ << ",\n";
    out << "  \"deliveryPacerLagMaxMs\": " << delivery_pacer_lag_ms_max_
        << ",\n";
    out << "  \"deliveryRepeatNoQueued\": " << delivery_repeat_no_queued_frames_
        << ",\n";
    out << "  \"deliverySkipNoQueued\": " << delivery_skip_no_queued_frames_
        << ",\n";
    out << "  \"deliveryFreshWakeAfterSkip\": "
        << delivery_fresh_wake_after_skip_frames_ << ",\n";
    out << "  \"deliveryFreshImmediate\": " << delivery_fresh_immediate_frames_
        << ",\n";
    out << "  \"deliveryRepeatPolicy\": \""
        << DeliveryRepeatPolicyName(delivery_repeat_policy_) << "\",\n";
    out << "  \"deliveryQueueDepth\": " << delivery_queue_depth_ << ",\n";
    out << "  \"nativeNv12ReadyPolicy\": \""
        << NativeNv12ReadyPolicyName(native_nv12_ready_policy_) << "\",\n";
    out << "  \"nativeNv12FenceAvailable\": "
        << (native_nv12_fence_available_ ? "true" : "false") << ",\n";
    out << "  \"nativeNv12PendingPollMs\": " << native_nv12_pending_poll_ms_
        << ",\n";
    out << "  \"nativeNv12MaxPendingSlots\": " << native_nv12_max_pending_slots_
        << ",\n";
    out << "  \"nativeNv12ReadyDrainDepth\": " << native_nv12_ready_drain_depth_
        << ",\n";
    out << "  \"nativeNv12FrameOwnership\": \""
        << NativeNv12FrameOwnershipName() << "\",\n";
    out << "  \"nativeNv12OnFrameBackpressureEnabled\": "
        << (native_nv12_onframe_backpressure_suspend_enabled_ ? "true"
                                                              : "false")
        << ",\n";
    out << "  \"nativeNv12OnFrameBackpressureThresholdMs\": "
        << native_nv12_onframe_backpressure_threshold_ms_ << ",\n";
    out << "  \"nativeNv12OnFrameBackpressureFrameLimit\": "
        << native_nv12_onframe_backpressure_frame_limit_ << ",\n";
    out << "  \"nativeNv12SingleInFlightEnabled\": "
        << (native_nv12_single_in_flight_enabled_ ? "true" : "false") << ",\n";
    out << "  \"nativeNv12AdmissionMailboxEnabled\": "
        << (native_nv12_admission_mailbox_enabled_ ? "true" : "false")
        << ",\n";
    out << "  \"nativeNv12GpuQueueBackoffEnabled\": "
        << (native_nv12_gpu_queue_backoff_enabled_ ? "true" : "false") << ",\n";
    out << "  \"nativeNv12GpuQueueBackoffThresholdFrames\": "
        << native_nv12_gpu_queue_backoff_threshold_frames_ << ",\n";
    out << "  \"nativeNv12GpuQueueBackoffDurationFrames\": "
        << native_nv12_gpu_queue_backoff_duration_frames_ << ",\n";
    out << "  \"nativeNv12LateReadyDropEnabled\": "
        << (native_nv12_drop_late_ready_enabled_ ? "true" : "false") << ",\n";
    out << "  \"nativeNv12LateReadyDropThresholdMs\": "
        << native_nv12_late_ready_drop_threshold_ms_ << ",\n";
    out << "  \"nativeNv12AdmissionMaxSourceAgeMs\": "
        << native_nv12_admission_max_source_age_ms_ << ",\n";
    out << "  \"nativeNv12WarmupI420Frames\": "
        << native_nv12_warmup_i420_frames_ << ",\n";
    out << "  \"deliveryRepeatSourceAgeMs\": "
        << AverageMetricMs(delivery_repeat_source_age_qpc_,
                           delivery_repeat_source_age_samples_)
        << ",\n";
    out << "  \"deliveryRepeatSourceAgeMaxMs\": "
        << TicksToMs(delivery_repeat_source_age_max_qpc_) << ",\n";
    out << "  \"deliveryRepeatSourceAgeSamples\": "
        << delivery_repeat_source_age_samples_ << ",\n";
    out << "  \"deliveryOnFrameMs\": "
        << AverageTicksMs(delivery_on_frame_qpc_, delivery_submitted_frames_)
        << ",\n";
    out << "  \"deliveryOnFrameMaxMs\": "
        << TicksToMs(delivery_on_frame_max_qpc_) << ",\n";
    out << "  \"deliverySubmitPrepMs\": "
        << AverageMetricMs(delivery_submit_prep_qpc_,
                           delivery_submit_prep_samples_)
        << ",\n";
    out << "  \"deliverySubmitPrepMaxMs\": "
        << TicksToMs(delivery_submit_prep_max_qpc_) << ",\n";
    out << "  \"deliverySubmitPrepSamples\": " << delivery_submit_prep_samples_
        << ",\n";
    out << "  \"deliveryOnFrameCallMs\": "
        << AverageMetricMs(delivery_on_frame_call_qpc_,
                           delivery_on_frame_call_samples_)
        << ",\n";
    out << "  \"deliveryOnFrameCallMaxMs\": "
        << TicksToMs(delivery_on_frame_call_max_qpc_) << ",\n";
    out << "  \"deliveryOnFrameCallSamples\": "
        << delivery_on_frame_call_samples_ << ",\n";
    out << "  \"deliveryPostOnFrameMs\": "
        << AverageMetricMs(delivery_post_on_frame_qpc_,
                           delivery_post_on_frame_samples_)
        << ",\n";
    out << "  \"deliveryPostOnFrameMaxMs\": "
        << TicksToMs(delivery_post_on_frame_max_qpc_) << ",\n";
    out << "  \"deliveryPostOnFrameSamples\": "
        << delivery_post_on_frame_samples_ << ",\n";
    out << "  \"nativeBufferReleaseMs\": "
        << AverageMetricMs(native_buffer_release_qpc_,
                           native_buffer_release_samples_)
        << ",\n";
    out << "  \"nativeBufferReleaseMaxMs\": "
        << TicksToMs(native_buffer_release_max_qpc_) << ",\n";
    out << "  \"nativeBufferReleaseSamples\": "
        << native_buffer_release_samples_ << ",\n";
    out << "  \"readyToQueueMs\": "
        << AverageMetricMs(ready_to_queue_qpc_, ready_to_queue_samples_)
        << ",\n";
    out << "  \"readyToQueueMaxMs\": " << TicksToMs(ready_to_queue_max_qpc_)
        << ",\n";
    out << "  \"readyToQueueSamples\": " << ready_to_queue_samples_ << ",\n";
    out << "  \"deliveryQueueWaitMs\": "
        << AverageMetricMs(delivery_queue_wait_qpc_,
                           delivery_queue_wait_samples_)
        << ",\n";
    out << "  \"deliveryQueueWaitMaxMs\": "
        << TicksToMs(delivery_queue_wait_max_qpc_) << ",\n";
    out << "  \"deliveryQueueWaitSamples\": " << delivery_queue_wait_samples_
        << ",\n";
    out << "  \"deliveryOverwriteAgeMs\": "
        << AverageMetricMs(delivery_overwrite_age_qpc_,
                           delivery_overwrite_age_samples_)
        << ",\n";
    out << "  \"deliveryOverwriteAgeMaxMs\": "
        << TicksToMs(delivery_overwrite_age_max_qpc_) << ",\n";
    out << "  \"deliveryOverwriteAgeSamples\": "
        << delivery_overwrite_age_samples_ << ",\n";
    out << "  \"deliveryOverwrittenFresh\": "
        << delivery_overwritten_fresh_frames_ << ",\n";
    out << "  \"readyToSubmitMs\": "
        << AverageMetricMs(ready_to_submit_qpc_, ready_to_submit_samples_)
        << ",\n";
    out << "  \"readyToSubmitMaxMs\": " << TicksToMs(ready_to_submit_max_qpc_)
        << ",\n";
    out << "  \"readyToSubmitSamples\": " << ready_to_submit_samples_ << ",\n";
    out << "  \"sourceToSubmitMs\": "
        << AverageMetricMs(source_to_submit_qpc_, source_to_submit_samples_)
        << ",\n";
    out << "  \"sourceToSubmitMaxMs\": " << TicksToMs(source_to_submit_max_qpc_)
        << ",\n";
    out << "  \"sourceToSubmitSamples\": " << source_to_submit_samples_
        << ",\n";
    out << "  \"sourceToReadbackReadyMs\": "
        << AverageMetricMs(source_to_readback_ready_qpc_,
                           source_to_readback_ready_samples_)
        << ",\n";
    out << "  \"sourceToReadbackReadyMaxMs\": "
        << TicksToMs(source_to_readback_ready_max_qpc_) << ",\n";
    out << "  \"sourceToReadbackReadySamples\": "
        << source_to_readback_ready_samples_ << ",\n";
    out << "  \"readbackQueueToMapMs\": "
        << AverageMetricMs(readback_queue_to_map_qpc_,
                           readback_queue_to_map_samples_)
        << ",\n";
    out << "  \"readbackQueueToMapMaxMs\": "
        << TicksToMs(readback_queue_to_map_max_qpc_) << ",\n";
    out << "  \"readbackQueueToMapSamples\": " << readback_queue_to_map_samples_
        << ",\n";
    out << "  \"mapToI420Ms\": "
        << AverageMetricMs(map_to_i420_qpc_, map_to_i420_samples_) << ",\n";
    out << "  \"mapToI420MaxMs\": " << TicksToMs(map_to_i420_max_qpc_) << ",\n";
    out << "  \"mapToI420Samples\": " << map_to_i420_samples_ << ",\n";
    out << "  \"sourceToI420ReadyMs\": "
        << AverageMetricMs(source_to_i420_ready_qpc_,
                           source_to_i420_ready_samples_)
        << ",\n";
    out << "  \"sourceToI420ReadyMaxMs\": "
        << TicksToMs(source_to_i420_ready_max_qpc_) << ",\n";
    out << "  \"sourceToI420ReadySamples\": " << source_to_i420_ready_samples_
        << ",\n";
    out << "  \"sourceToQueueMs\": "
        << AverageMetricMs(source_to_queue_qpc_, source_to_queue_samples_)
        << ",\n";
    out << "  \"sourceToQueueMaxMs\": " << TicksToMs(source_to_queue_max_qpc_)
        << ",\n";
    out << "  \"sourceToQueueSamples\": " << source_to_queue_samples_ << ",\n";
    out << "  \"sourceDuplicateSkipAgeMs\": "
        << AverageMetricMs(source_duplicate_skip_age_qpc_,
                           source_duplicate_skip_age_samples_)
        << ",\n";
    out << "  \"sourceDuplicateSkipAgeMaxMs\": "
        << TicksToMs(source_duplicate_skip_age_max_qpc_) << ",\n";
    out << "  \"sourceDuplicateSkipAgeSamples\": "
        << source_duplicate_skip_age_samples_ << ",\n";
    out << "  \"gpuScaled\": " << gpu_scaled_frames_ << ",\n";
    out << "  \"gpuScaleFailures\": " << gpu_scale_failures_ << ",\n";
    out << "  \"nativeNv12Submitted\": " << gpu_nv12_submitted_frames_ << ",\n";
    out << "  \"nativeNv12Failures\": " << gpu_nv12_failures_ << ",\n";
    out << "  \"nativeNv12Queued\": " << gpu_nv12_queued_frames_ << ",\n";
    out << "  \"nativeNv12Ready\": " << gpu_nv12_ready_frames_ << ",\n";
    out << "  \"nativeNv12NotReadyPolls\": " << gpu_nv12_not_ready_polls_
        << ",\n";
    out << "  \"nativeNv12FenceSignaled\": " << gpu_nv12_fence_signaled_frames_
        << ",\n";
    out << "  \"nativeNv12FenceReady\": " << gpu_nv12_fence_ready_frames_
        << ",\n";
    out << "  \"nativeNv12FenceSignalFailures\": "
        << gpu_nv12_fence_signal_failures_ << ",\n";
    out << "  \"nativeNv12OwnedCopies\": " << gpu_nv12_owned_copy_frames_
        << ",\n";
    out << "  \"nativeNv12OwnedCopyMs\": "
        << AverageMetricMs(gpu_nv12_owned_copy_qpc_,
                           gpu_nv12_owned_copy_samples_)
        << ",\n";
    out << "  \"nativeNv12OwnedCopyMaxMs\": "
        << TicksToMs(gpu_nv12_owned_copy_max_qpc_) << ",\n";
    out << "  \"nativeNv12OwnedCopySamples\": " << gpu_nv12_owned_copy_samples_
        << ",\n";
    out << "  \"nativeNv12Overwritten\": " << gpu_nv12_overwritten_frames_
        << ",\n";
    out << "  \"nativeNv12OverwriteAgeMs\": "
        << AverageMetricMs(gpu_nv12_overwrite_age_qpc_,
                           gpu_nv12_overwrite_age_samples_)
        << ",\n";
    out << "  \"nativeNv12OverwriteAgeMaxMs\": "
        << TicksToMs(gpu_nv12_overwrite_age_max_qpc_) << ",\n";
    out << "  \"nativeNv12OverwriteAgeSamples\": "
        << gpu_nv12_overwrite_age_samples_ << ",\n";
    out << "  \"nativeNv12OverwrittenFresh\": "
        << gpu_nv12_overwritten_fresh_frames_ << ",\n";
    out << "  \"nativeNv12ReadyDropped\": " << gpu_nv12_ready_dropped_frames_
        << ",\n";
    out << "  \"nativeNv12ReadyDropAgeMs\": "
        << AverageMetricMs(gpu_nv12_ready_drop_age_qpc_,
                           gpu_nv12_ready_drop_age_samples_)
        << ",\n";
    out << "  \"nativeNv12ReadyDropAgeMaxMs\": "
        << TicksToMs(gpu_nv12_ready_drop_age_max_qpc_) << ",\n";
    out << "  \"nativeNv12ReadyDropAgeSamples\": "
        << gpu_nv12_ready_drop_age_samples_ << ",\n";
    out << "  \"nativeNv12ReadyDroppedFresh\": "
        << gpu_nv12_ready_dropped_fresh_frames_ << ",\n";
    out << "  \"nativeNv12LateReadyDropped\": "
        << gpu_nv12_late_ready_dropped_frames_ << ",\n";
    out << "  \"nativeNv12LateReadyDropAgeMs\": "
        << AverageMetricMs(gpu_nv12_late_ready_drop_age_qpc_,
                           gpu_nv12_late_ready_drop_age_samples_)
        << ",\n";
    out << "  \"nativeNv12LateReadyDropAgeMaxMs\": "
        << TicksToMs(gpu_nv12_late_ready_drop_age_max_qpc_) << ",\n";
    out << "  \"nativeNv12LateReadyDropAgeSamples\": "
        << gpu_nv12_late_ready_drop_age_samples_ << ",\n";
    out << "  \"nativeNv12LateReadyDroppedFresh\": "
        << gpu_nv12_late_ready_dropped_fresh_frames_ << ",\n";
    out << "  \"nativeNv12LateReadyDropBltToReadyMs\": "
        << AverageMetricMs(gpu_nv12_late_ready_drop_blt_to_ready_qpc_,
                           gpu_nv12_late_ready_drop_blt_to_ready_samples_)
        << ",\n";
    out << "  \"nativeNv12LateReadyDropBltToReadyMaxMs\": "
        << TicksToMs(gpu_nv12_late_ready_drop_blt_to_ready_max_qpc_) << ",\n";
    out << "  \"nativeNv12LateReadyDropBltToReadySamples\": "
        << gpu_nv12_late_ready_drop_blt_to_ready_samples_ << ",\n";
    out << "  \"nativeNv12ConversionStartAgeMs\": "
        << AverageMetricMs(gpu_nv12_conversion_start_age_qpc_,
                           gpu_nv12_conversion_start_age_samples_)
        << ",\n";
    out << "  \"nativeNv12ConversionStartAgeMaxMs\": "
        << TicksToMs(gpu_nv12_conversion_start_age_max_qpc_) << ",\n";
    out << "  \"nativeNv12ConversionStartAgeSamples\": "
        << gpu_nv12_conversion_start_age_samples_ << ",\n";
    out << "  \"nativeNv12SingleInFlightDeferred\": "
        << native_nv12_single_in_flight_deferred_frames_ << ",\n";
    out << "  \"nativeNv12SingleInFlightDeferredFresh\": "
        << native_nv12_single_in_flight_deferred_fresh_frames_ << ",\n";
    out << "  \"nativeNv12SingleInFlightPendingMax\": "
        << native_nv12_single_in_flight_pending_max_ << ",\n";
    out << "  \"nativeNv12SingleInFlightDeferredSourceAgeMs\": "
        << AverageMetricMs(
               native_nv12_single_in_flight_deferred_source_age_qpc_,
               native_nv12_single_in_flight_deferred_source_age_samples_)
        << ",\n";
    out << "  \"nativeNv12SingleInFlightDeferredSourceAgeMaxMs\": "
        << TicksToMs(native_nv12_single_in_flight_deferred_source_age_max_qpc_)
        << ",\n";
    out << "  \"nativeNv12SingleInFlightDeferredSourceAgeSamples\": "
        << native_nv12_single_in_flight_deferred_source_age_samples_ << ",\n";
    out << "  \"nativeNv12GpuQueueBackoffTriggered\": "
        << native_nv12_gpu_queue_backoff_triggered_frames_ << ",\n";
    out << "  \"nativeNv12GpuQueueBackoffSuppressed\": "
        << native_nv12_gpu_queue_backoff_suppressed_frames_ << ",\n";
    out << "  \"nativeNv12GpuQueueBackoffSuppressedFresh\": "
        << native_nv12_gpu_queue_backoff_suppressed_fresh_frames_ << ",\n";
    out << "  \"nativeNv12GpuQueueBackoffMs\": "
        << AverageMetricMs(native_nv12_gpu_queue_backoff_duration_qpc_,
                           native_nv12_gpu_queue_backoff_duration_samples_)
        << ",\n";
    out << "  \"nativeNv12GpuQueueBackoffMaxMs\": "
        << TicksToMs(native_nv12_gpu_queue_backoff_duration_max_qpc_) << ",\n";
    out << "  \"nativeNv12GpuQueueBackoffSamples\": "
        << native_nv12_gpu_queue_backoff_duration_samples_ << ",\n";
    out << "  \"nativeNv12GpuQueueBackoffTriggerBltToReadyMs\": "
        << AverageMetricMs(
               native_nv12_gpu_queue_backoff_trigger_blt_to_ready_qpc_,
               native_nv12_gpu_queue_backoff_trigger_blt_to_ready_samples_)
        << ",\n";
    out << "  \"nativeNv12GpuQueueBackoffTriggerBltToReadyMaxMs\": "
        << TicksToMs(
               native_nv12_gpu_queue_backoff_trigger_blt_to_ready_max_qpc_)
        << ",\n";
    out << "  \"nativeNv12GpuQueueBackoffTriggerBltToReadySamples\": "
        << native_nv12_gpu_queue_backoff_trigger_blt_to_ready_samples_ << ",\n";
    out << "  \"nativeNv12GpuQueueBackoffSuppressedSourceAgeMs\": "
        << AverageMetricMs(
               native_nv12_gpu_queue_backoff_suppressed_source_age_qpc_,
               native_nv12_gpu_queue_backoff_suppressed_source_age_samples_)
        << ",\n";
    out << "  \"nativeNv12GpuQueueBackoffSuppressedSourceAgeMaxMs\": "
        << TicksToMs(
               native_nv12_gpu_queue_backoff_suppressed_source_age_max_qpc_)
        << ",\n";
    out << "  \"nativeNv12GpuQueueBackoffSuppressedSourceAgeSamples\": "
        << native_nv12_gpu_queue_backoff_suppressed_source_age_samples_
        << ",\n";
    out << "  \"nativeNv12AdmissionMailboxPendingActive\": "
        << (HasPendingNativeNv12Admission() ? "true" : "false") << ",\n";
    out << "  \"nativeNv12AdmissionMailboxStored\": "
        << native_nv12_admission_mailbox_stored_frames_ << ",\n";
    out << "  \"nativeNv12AdmissionMailboxReplaced\": "
        << native_nv12_admission_mailbox_replaced_frames_ << ",\n";
    out << "  \"nativeNv12AdmissionMailboxSubmitted\": "
        << native_nv12_admission_mailbox_submitted_frames_ << ",\n";
    out << "  \"nativeNv12AdmissionMailboxStaleDropped\": "
        << native_nv12_admission_mailbox_stale_dropped_frames_ << ",\n";
    out << "  \"nativeNv12AdmissionMailboxPendingAgeMs\": "
        << AverageMetricMs(native_nv12_admission_mailbox_pending_age_qpc_,
                           native_nv12_admission_mailbox_pending_age_samples_)
        << ",\n";
    out << "  \"nativeNv12AdmissionMailboxPendingAgeMaxMs\": "
        << TicksToMs(native_nv12_admission_mailbox_pending_age_max_qpc_)
        << ",\n";
    out << "  \"nativeNv12AdmissionMailboxPendingAgeSamples\": "
        << native_nv12_admission_mailbox_pending_age_samples_ << ",\n";
    out << "  \"nativeNv12AdmissionMailboxSubmitSourceAgeMs\": "
        << AverageMetricMs(
               native_nv12_admission_mailbox_submit_source_age_qpc_,
               native_nv12_admission_mailbox_submit_source_age_samples_)
        << ",\n";
    out << "  \"nativeNv12AdmissionMailboxSubmitSourceAgeMaxMs\": "
        << TicksToMs(
               native_nv12_admission_mailbox_submit_source_age_max_qpc_)
        << ",\n";
    out << "  \"nativeNv12AdmissionMailboxSubmitSourceAgeSamples\": "
        << native_nv12_admission_mailbox_submit_source_age_samples_ << ",\n";
    out << "  \"nativeNv12ConvertMs\": "
        << AverageMetricMs(gpu_nv12_convert_qpc_, gpu_nv12_convert_samples_)
        << ",\n";
    out << "  \"nativeNv12ConvertMaxMs\": "
        << TicksToMs(gpu_nv12_convert_max_qpc_) << ",\n";
    out << "  \"nativeNv12ConvertSamples\": " << gpu_nv12_convert_samples_
        << ",\n";
    out << "  \"nativeNv12BgraScaleDrawMs\": "
        << AverageMetricMs(gpu_nv12_bgra_scale_draw_qpc_,
                           gpu_nv12_bgra_scale_draw_samples_)
        << ",\n";
    out << "  \"nativeNv12BgraScaleDrawMaxMs\": "
        << TicksToMs(gpu_nv12_bgra_scale_draw_max_qpc_) << ",\n";
    out << "  \"nativeNv12BgraScaleDrawSamples\": "
        << gpu_nv12_bgra_scale_draw_samples_ << ",\n";
    out << "  \"nativeNv12VideoProcessorBltSubmitMs\": "
        << AverageMetricMs(gpu_nv12_blt_submit_qpc_,
                           gpu_nv12_blt_submit_samples_)
        << ",\n";
    out << "  \"nativeNv12VideoProcessorBltSubmitMaxMs\": "
        << TicksToMs(gpu_nv12_blt_submit_max_qpc_) << ",\n";
    out << "  \"nativeNv12VideoProcessorBltSubmitSamples\": "
        << gpu_nv12_blt_submit_samples_ << ",\n";
    out << "  \"nativeNv12VideoProcessorBltCpuSubmitMs\": "
        << AverageMetricMs(gpu_nv12_blt_submit_qpc_,
                           gpu_nv12_blt_submit_samples_)
        << ",\n";
    out << "  \"nativeNv12VideoProcessorBltCpuSubmitMaxMs\": "
        << TicksToMs(gpu_nv12_blt_submit_max_qpc_) << ",\n";
    out << "  \"nativeNv12VideoProcessorBltCpuSubmitSamples\": "
        << gpu_nv12_blt_submit_samples_ << ",\n";
    out << "  \"nativeNv12VideoProcessorBltToReadyMs\": "
        << AverageMetricMs(gpu_nv12_blt_to_ready_qpc_,
                           gpu_nv12_blt_to_ready_samples_)
        << ",\n";
    out << "  \"nativeNv12VideoProcessorBltToReadyMaxMs\": "
        << TicksToMs(gpu_nv12_blt_to_ready_max_qpc_) << ",\n";
    out << "  \"nativeNv12VideoProcessorBltToReadySamples\": "
        << gpu_nv12_blt_to_ready_samples_ << ",\n";
    out << "  \"nativeNv12VideoProcessorBltSubmitToFenceMs\": "
        << AverageMetricMs(gpu_nv12_blt_to_ready_qpc_,
                           gpu_nv12_blt_to_ready_samples_)
        << ",\n";
    out << "  \"nativeNv12VideoProcessorBltSubmitToFenceMaxMs\": "
        << TicksToMs(gpu_nv12_blt_to_ready_max_qpc_) << ",\n";
    out << "  \"nativeNv12VideoProcessorBltSubmitToFenceSamples\": "
        << gpu_nv12_blt_to_ready_samples_ << ",\n";
    out << "  \"nativeNv12VideoProcessorBltGpuExecutionMs\": "
        << AverageMetricMs(gpu_nv12_blt_gpu_execution_ms_,
                           gpu_nv12_blt_gpu_execution_samples_)
        << ",\n";
    out << "  \"nativeNv12VideoProcessorBltGpuExecutionMaxMs\": "
        << gpu_nv12_blt_gpu_execution_max_ms_ << ",\n";
    out << "  \"nativeNv12VideoProcessorBltGpuExecutionSamples\": "
        << gpu_nv12_blt_gpu_execution_samples_ << ",\n";
    out << "  \"nativeNv12VideoProcessorBltEstimatedGpuQueueDelayMs\": "
        << AverageMetricMs(gpu_nv12_blt_gpu_queue_delay_ms_,
                           gpu_nv12_blt_gpu_queue_delay_samples_)
        << ",\n";
    out << "  \"nativeNv12VideoProcessorBltEstimatedGpuQueueDelayMaxMs\": "
        << gpu_nv12_blt_gpu_queue_delay_max_ms_ << ",\n";
    out << "  \"nativeNv12VideoProcessorBltEstimatedGpuQueueDelaySamples\": "
        << gpu_nv12_blt_gpu_queue_delay_samples_ << ",\n";
    out << "  \"nativeNv12VideoProcessorBltGpuTimestampFailures\": "
        << gpu_nv12_blt_gpu_timestamp_failures_ << ",\n";
    out << "  \"nativeNv12VideoProcessorBltGpuTimestampNotReady\": "
        << gpu_nv12_blt_gpu_timestamp_not_ready_ << ",\n";
    out << "  \"nativeNv12VideoProcessorBltGpuTimestampDisjoint\": "
        << gpu_nv12_blt_gpu_timestamp_disjoint_ << ",\n";
    out << "  \"nativeNv12BufferCreateMs\": "
        << AverageMetricMs(gpu_nv12_buffer_create_qpc_,
                           gpu_nv12_buffer_create_samples_)
        << ",\n";
    out << "  \"nativeNv12BufferCreateMaxMs\": "
        << TicksToMs(gpu_nv12_buffer_create_max_qpc_) << ",\n";
    out << "  \"nativeNv12BufferCreateSamples\": "
        << gpu_nv12_buffer_create_samples_ << ",\n";
    out << "  \"nativeNv12FrameReadyToQueueMs\": "
        << AverageMetricMs(gpu_nv12_frame_ready_to_queue_qpc_,
                           gpu_nv12_frame_ready_to_queue_samples_)
        << ",\n";
    out << "  \"nativeNv12FrameReadyToQueueMaxMs\": "
        << TicksToMs(gpu_nv12_frame_ready_to_queue_max_qpc_) << ",\n";
    out << "  \"nativeNv12FrameReadyToQueueSamples\": "
        << gpu_nv12_frame_ready_to_queue_samples_ << ",\n";
    out << "  \"nativeNv12StaleBeforeQueue\": "
        << gpu_nv12_stale_before_queue_frames_ << ",\n";
    out << "  \"consumerDeviceRecoveries\": " << consumer_device_recoveries_
        << ",\n";
    out << "  \"nativeNv12SuspendedAfterDeviceLoss\": "
        << (native_nv12_suspended_after_device_loss_ ? "true" : "false")
        << ",\n";
    out << "  \"nativeNv12SuspendedAfterOnFrameBackpressure\": "
        << (native_nv12_suspended_after_onframe_backpressure_.load() ? "true"
                                                                     : "false")
        << ",\n";
    out << "  \"nativeNv12OnFrameBackpressureFrames\": "
        << native_nv12_onframe_backpressure_frames_ << ",\n";
    out << "  \"nativeNv12OnFrameBackpressureStreak\": "
        << native_nv12_onframe_backpressure_streak_ << ",\n";
    out << "  \"nativeNv12OnFrameBackpressureMaxMs\": "
        << native_nv12_onframe_backpressure_max_ms_ << ",\n";
    out << "  \"cpuFallback\": " << cpu_fallback_frames_ << ",\n";
    out << "  \"readbackQueued\": " << gpu_readback_queued_frames_ << ",\n";
    out << "  \"readbackReady\": " << gpu_readback_ready_frames_ << ",\n";
    out << "  \"readbackNotReady\": " << gpu_readback_not_ready_frames_
        << ",\n";
    out << "  \"readbackOverwritten\": " << gpu_readback_overwritten_frames_
        << ",\n";
    out << "  \"readbackStaleDropped\": " << gpu_readback_stale_dropped_frames_
        << ",\n";
    out << "  \"readbackLatencyDropped\": "
        << gpu_readback_latency_dropped_frames_ << ",\n";
    out << "  \"readbackMapAttempts\": " << gpu_readback_map_attempts_ << ",\n";
    out << "  \"gpuScaleMs\": " << gpu_scale_ms << ",\n";
    out << "  \"copyMs\": " << copy_ms << ",\n";
    out << "  \"mapMs\": " << map_ms << ",\n";
    out << "  \"readbackLatencyMs\": " << readback_latency_ms << ",\n";
    out << "  \"readbackLatencyFramesAvg\": " << readback_latency_frames_avg
        << ",\n";
    out << "  \"readbackLatencyFramesMax\": " << readback_latency_frames_max_
        << ",\n";
    out << "  \"sourceFrameIndex\": "
        << (shared_state_ ? shared_state_->latest_frame_index : 0) << ",\n";
    out << "  \"lastSubmittedSourceFrameIndex\": "
        << last_submitted_source_frame_index_ << ",\n";
    out << "  \"sourceFrameRegressions\": " << source_frame_regressions_
        << ",\n";
    out << "  \"sourceFrameDuplicates\": "
        << source_frame_duplicate_submissions_ << ",\n";
    out << "  \"sourceFrameGaps\": " << source_frame_gaps_ << ",\n";
    out << "  \"sharedSlotMismatches\": " << shared_slot_mismatch_frames_
        << ",\n";
    out << "  \"timestampMode\": \"" << TimestampModeLabel() << "\",\n";
    out << "  \"timestampSourceQpcFrames\": " << timestamp_source_qpc_frames_
        << ",\n";
    out << "  \"timestampPacedFallbackFrames\": "
        << timestamp_paced_fallback_frames_ << ",\n";
    out << "  \"timestampRepeatedFrames\": " << timestamp_repeated_frames_
        << ",\n";
    out << "  \"timestampDeltaMs\": "
        << (timestamp_delta_samples_ == 0
                ? 0.0
                : static_cast<double>(timestamp_delta_us_total_) / 1000.0 /
                      static_cast<double>(timestamp_delta_samples_))
        << ",\n";
    out << "  \"timestampDeltaMaxMs\": "
        << static_cast<double>(timestamp_delta_us_max_) / 1000.0 << ",\n";
    out << "  \"timestampSamples\": " << timestamp_delta_samples_ << ",\n";
    out << "  \"timestampAdjustments\": " << timestamp_adjustments_ << ",\n";
    out << "  \"deliveryWallDeltaMs\": "
        << (delivery_wall_delta_samples_ == 0
                ? 0.0
                : static_cast<double>(delivery_wall_delta_us_total_) / 1000.0 /
                      static_cast<double>(delivery_wall_delta_samples_))
        << ",\n";
    out << "  \"deliveryWallDeltaMaxMs\": "
        << static_cast<double>(delivery_wall_delta_us_max_) / 1000.0 << ",\n";
    out << "  \"deliveryWallDeltaMinMs\": "
        << static_cast<double>(delivery_wall_delta_us_min_) / 1000.0 << ",\n";
    out << "  \"deliveryWallSamples\": " << delivery_wall_delta_samples_
        << ",\n";
    out << "  \"deliveryWallOver2x\": " << delivery_wall_over_2x_frames_
        << ",\n";
    out << "  \"deliveryWallOver3x\": " << delivery_wall_over_3x_frames_
        << ",\n";
    out << "  \"deliveryWallUnderHalf\": " << delivery_wall_under_half_frames_
        << ",\n";
    out << "  \"sourceQpcDeltaMs\": "
        << (source_qpc_delta_samples_ == 0
                ? 0.0
                : static_cast<double>(source_qpc_delta_us_total_) / 1000.0 /
                      static_cast<double>(source_qpc_delta_samples_))
        << ",\n";
    out << "  \"sourceQpcDeltaMaxMs\": "
        << static_cast<double>(source_qpc_delta_us_max_) / 1000.0 << ",\n";
    out << "  \"sourceQpcSamples\": " << source_qpc_delta_samples_ << ",\n";
    out << "  \"sourceQpcRegressions\": " << source_qpc_regressions_ << ",\n";
    out << "  \"sourceQpcOver2x\": " << source_qpc_over_2x_frames_ << ",\n";
    out << "  \"sourceQpcOver3x\": " << source_qpc_over_3x_frames_ << ",\n";
    out << "  \"sourceQpcUnderHalf\": " << source_qpc_under_half_frames_
        << ",\n";
    out << "  \"sourceLatestObservedFrames\": "
        << source_latest_observed_frames_ << ",\n";
    out << "  \"sourceLatestFrameGaps\": " << source_latest_frame_gaps_
        << ",\n";
    out << "  \"sourceLatestFrameRegressions\": "
        << source_latest_frame_regressions_ << ",\n";
    out << "  \"sourceLatestQpcDeltaMs\": "
        << AverageTicksMs(source_latest_qpc_delta_,
                          source_latest_qpc_delta_samples_)
        << ",\n";
    out << "  \"sourceLatestQpcDeltaMaxMs\": "
        << TicksToMs(source_latest_qpc_delta_max_) << ",\n";
    out << "  \"sourceLatestQpcSamples\": " << source_latest_qpc_delta_samples_
        << ",\n";
    out << "  \"sourceLatestQpcRegressions\": "
        << source_latest_qpc_regressions_ << ",\n";
    out << "  \"sourceLatestQpcOver2x\": " << source_latest_qpc_over_2x_frames_
        << ",\n";
    out << "  \"sourceLatestQpcOver3x\": " << source_latest_qpc_over_3x_frames_
        << ",\n";
    out << "  \"sourceLatestQpcUnderHalf\": "
        << source_latest_qpc_under_half_frames_ << ",\n";
    out << "  \"sourceLatestObservationDeltaMs\": "
        << AverageTicksMs(source_latest_observation_delta_,
                          source_latest_observation_delta_samples_)
        << ",\n";
    out << "  \"sourceLatestObservationDeltaMaxMs\": "
        << TicksToMs(source_latest_observation_delta_max_) << ",\n";
    out << "  \"sourceLatestObservationSamples\": "
        << source_latest_observation_delta_samples_ << ",\n";
    out << "  \"sourceLatestObservationOver2x\": "
        << source_latest_observation_over_2x_frames_ << ",\n";
    out << "  \"sourceLatestObservationOver3x\": "
        << source_latest_observation_over_3x_frames_ << ",\n";
    out << "  \"sourceLatestEventAgeMs\": "
        << AverageTicksMs(source_latest_event_age_,
                          source_latest_event_age_samples_)
        << ",\n";
    out << "  \"sourceLatestEventAgeMaxMs\": "
        << TicksToMs(source_latest_event_age_max_) << ",\n";
    out << "  \"sourceLatestEventAgeSamples\": "
        << source_latest_event_age_samples_ << ",\n";
    out << "  \"sourceLatestEventAgeOver1x\": "
        << source_latest_event_age_over_1x_frames_ << ",\n";
    out << "  \"sourceLatestEventAgeOver2x\": "
        << source_latest_event_age_over_2x_frames_ << ",\n";
    out << "  \"sourceLatestEventAgeOver3x\": "
        << source_latest_event_age_over_3x_frames_ << ",\n";
    out << "  \"sourcePublishObservationAgeMs\": "
        << AverageTicksMs(source_publish_observation_age_,
                          source_publish_observation_age_samples_)
        << ",\n";
    out << "  \"sourcePublishObservationAgeMaxMs\": "
        << TicksToMs(source_publish_observation_age_max_) << ",\n";
    out << "  \"sourcePublishObservationAgeSamples\": "
        << source_publish_observation_age_samples_ << ",\n";
    out << "  \"sourcePublishObservationAgeOver1x\": "
        << source_publish_observation_age_over_1x_frames_ << ",\n";
    out << "  \"sourcePublishObservationAgeOver2x\": "
        << source_publish_observation_age_over_2x_frames_ << ",\n";
    out << "  \"sourcePublishObservationAgeOver3x\": "
        << source_publish_observation_age_over_3x_frames_ << ",\n";
    out << "  \"producerPresentGapMs\": "
        << (shared_state_ == nullptr
                ? 0.0
                : AverageSharedTicksMs(
                      shared_state_->producer_present_gap_qpc_total,
                      shared_state_->producer_present_gap_samples))
        << ",\n";
    out << "  \"producerPresentGapMaxMs\": "
        << (shared_state_ == nullptr
                ? 0.0
                : SharedTicksToMs(shared_state_->producer_present_gap_qpc_max))
        << ",\n";
    out << "  \"producerPresentGapSamples\": "
        << (shared_state_ ? shared_state_->producer_present_gap_samples : 0)
        << ",\n";
    out << "  \"producerCaptureGapMs\": "
        << (shared_state_ == nullptr
                ? 0.0
                : AverageSharedTicksMs(
                      shared_state_->producer_capture_gap_qpc_total,
                      shared_state_->producer_capture_gap_samples))
        << ",\n";
    out << "  \"producerCaptureGapMaxMs\": "
        << (shared_state_ == nullptr
                ? 0.0
                : SharedTicksToMs(shared_state_->producer_capture_gap_qpc_max))
        << ",\n";
    out << "  \"producerCaptureGapSamples\": "
        << (shared_state_ ? shared_state_->producer_capture_gap_samples : 0)
        << ",\n";
    out << "  \"producerPresentToPublishMs\": "
        << (shared_state_ == nullptr
                ? 0.0
                : AverageSharedTicksMs(
                      shared_state_->producer_present_to_publish_qpc_total,
                      shared_state_->producer_present_to_publish_samples))
        << ",\n";
    out << "  \"producerPresentToPublishMaxMs\": "
        << (shared_state_ == nullptr
                ? 0.0
                : SharedTicksToMs(
                      shared_state_->producer_present_to_publish_qpc_max))
        << ",\n";
    out << "  \"producerPresentToPublishSamples\": "
        << (shared_state_ ? shared_state_->producer_present_to_publish_samples
                          : 0)
        << ",\n";
    out << "  \"producerCopyMs\": "
        << (shared_state_ == nullptr
                ? 0.0
                : AverageSharedTicksMs(shared_state_->producer_copy_qpc_total,
                                       shared_state_->producer_copy_samples))
        << ",\n";
    out << "  \"producerCopyMaxMs\": "
        << (shared_state_ == nullptr
                ? 0.0
                : SharedTicksToMs(shared_state_->producer_copy_qpc_max))
        << ",\n";
    out << "  \"producerCopySamples\": "
        << (shared_state_ ? shared_state_->producer_copy_samples : 0) << ",\n";
    out << "  \"producerResolveMs\": "
        << (shared_state_ == nullptr
                ? 0.0
                : AverageSharedTicksMs(
                      shared_state_->producer_resolve_qpc_total,
                      shared_state_->producer_resolve_samples))
        << ",\n";
    out << "  \"producerResolveMaxMs\": "
        << (shared_state_ == nullptr
                ? 0.0
                : SharedTicksToMs(shared_state_->producer_resolve_qpc_max))
        << ",\n";
    out << "  \"producerResolveSamples\": "
        << (shared_state_ ? shared_state_->producer_resolve_samples : 0)
        << ",\n";
    out << "  \"producerThrottledFrames\": "
        << (shared_state_ ? shared_state_->producer_throttled_frames : 0)
        << ",\n";
    out << "  \"convertMs\": " << convert_ms << ",\n";
    out << "  \"mapFailures\": " << map_failures_ << ",\n";
    out << "  \"convertFailures\": " << convert_failures_ << ",\n";
    out << "  \"proofFrames\": " << proof_frames_written_ << ",\n";
    out << "  \"visibleProofFrames\": " << visible_proof_frames_ << ",\n";
    out << "  \"i420ProofFrames\": " << i420_proof_frames_written_ << ",\n";
    out << "  \"visibleI420ProofFrames\": " << visible_i420_proof_frames_
        << ",\n";
    out << "  \"initialBlackSkipped\": " << initial_black_skipped_frames_
        << ",\n";
    out << "  \"visibleSourceSeen\": "
        << (visible_source_seen_ ? "true" : "false") << "\n";
    out << "}\n";
    return out.str();
  }

 private:
  struct GpuReadbackSlot {
    ComPtr<ID3D11Texture2D> texture;
    bool pending = false;
    uint64_t sequence = 0;
    uint64_t source_frame_index = 0;
    uint64_t source_qpc = 0;
    bool repeated = false;
    int source_width = 0;
    int source_height = 0;
    int output_width = 0;
    int output_height = 0;
    DXGI_FORMAT source_format = DXGI_FORMAT_UNKNOWN;
    int64_t start_qpc = 0;
    int64_t after_gpu_qpc = 0;
    int64_t after_copy_qpc = 0;
  };

  struct GpuNv12Slot {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11VideoProcessorOutputView> output_view;
    // Phase 2 (experimental, default off): plane render targets used by the
    // render-convert path instead of VideoProcessorBlt. R8_UNORM views the Y
    // plane, R8G8_UNORM views the UV plane of the same NV12 texture.
    ComPtr<ID3D11RenderTargetView> y_plane_rtv;
    ComPtr<ID3D11RenderTargetView> uv_plane_rtv;
    ComPtr<ID3D11Query> ready_query;
    ComPtr<ID3D11Query> blt_timestamp_disjoint_query;
    ComPtr<ID3D11Query> blt_timestamp_start_query;
    ComPtr<ID3D11Query> blt_timestamp_end_query;
    ComPtr<ID3D11Fence> ready_fence;
    uint64_t ready_fence_value = 0;
    bool blt_timestamp_pending = false;
    bool direct_encoder_output = false;
    bool pending = false;
    uint64_t sequence = 0;
    uint64_t attempt = 0;
    uint64_t source_frame_index = 0;
    uint64_t source_qpc = 0;
    bool repeated = false;
    int source_width = 0;
    int source_height = 0;
    int output_width = 0;
    int output_height = 0;
    DXGI_FORMAT source_format = DXGI_FORMAT_UNKNOWN;
    int64_t start_qpc = 0;
    int64_t after_gpu_qpc = 0;
    int64_t after_blt_qpc = 0;
  };

  struct NativeNv12PendingAdmission {
    ComPtr<ID3D11Texture2D> texture;
    uint64_t source_frame_index = 0;
    uint64_t source_qpc = 0;
    bool repeated = false;
    int64_t deferred_qpc = 0;
  };

  struct PendingVideoFrame {
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer;
    int source_width = 0;
    int source_height = 0;
    int output_width = 0;
    int output_height = 0;
    DXGI_FORMAT source_format = DXGI_FORMAT_UNKNOWN;
    uint64_t source_frame_index = 0;
    uint64_t source_qpc = 0;
    int64_t ready_qpc = 0;
    int64_t queued_qpc = 0;
    bool repeated = false;
    bool gpu_scaled = false;
    bool native_nv12 = false;
  };

  enum class SourceRefreshResult {
    kNoUpdate,
    kUpdated,
    kSlotMismatch,
  };

  enum class GpuNv12ReadyObservation {
    kImmediateAfterBlt,
    kPostFenceRegistration,
    kFenceEvent,
    kSourceEvent,
    kWaitOther,
    kLoopIdle,
    kDuplicateSkip,
    kPreSubmit,
    kWriteSlotScan,
    kUnknown,
  };

  const char* GpuNv12ReadyObservationName(
      GpuNv12ReadyObservation observation) const {
    switch (observation) {
      case GpuNv12ReadyObservation::kImmediateAfterBlt:
        return "immediate_after_blt";
      case GpuNv12ReadyObservation::kPostFenceRegistration:
        return "post_fence_registration";
      case GpuNv12ReadyObservation::kFenceEvent:
        return "fence_event";
      case GpuNv12ReadyObservation::kSourceEvent:
        return "source_event";
      case GpuNv12ReadyObservation::kWaitOther:
        return "wait_other";
      case GpuNv12ReadyObservation::kLoopIdle:
        return "loop_idle";
      case GpuNv12ReadyObservation::kDuplicateSkip:
        return "duplicate_skip";
      case GpuNv12ReadyObservation::kPreSubmit:
        return "pre_submit";
      case GpuNv12ReadyObservation::kWriteSlotScan:
        return "write_slot_scan";
      case GpuNv12ReadyObservation::kUnknown:
        return "unknown";
    }
    return "unknown";
  }

  void Log(const std::string& message) {
    std::string line = "game_capture_webrtc_source " + message;
    std::thread([line = std::move(line)] {
      AppendNativeDiagnosticLine(line);
    }).detach();
  }

  void LogSync(const std::string& message) {
    AppendNativeDiagnosticLine("game_capture_webrtc_source " + message);
  }

  void LogGpuNv12Stage(uint64_t attempt, const std::string& stage) {
    if (attempt <= 3 || attempt % 60 == 0) {
      LogSync("gpu_nv12_stage attempt=" + std::to_string(attempt) +
              " stage=" + stage);
    }
  }

  void LogGpuScaleFailure(const std::string& reason, HRESULT hr) {
    ++gpu_scale_failures_;
    if (!gpu_scale_failure_logged_ || gpu_scale_failures_ % 30 == 0) {
      gpu_scale_failure_logged_ = true;
      Log("gpu_scale_failed reason=" + reason + " hr=" + HResultHex(hr) +
          " failures=" + std::to_string(gpu_scale_failures_));
    }
  }

  void LogGpuNv12Failure(const std::string& reason, HRESULT hr) {
    ++gpu_nv12_failures_;
    if (!gpu_nv12_failure_logged_ || gpu_nv12_failures_ % 30 == 0) {
      gpu_nv12_failure_logged_ = true;
      Log("gpu_nv12_failed reason=" + reason + " hr=" + HResultHex(hr) +
          " failures=" + std::to_string(gpu_nv12_failures_));
    }
  }

  bool IsDeviceLossHResult(HRESULT hr) const {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
           hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
  }

  void ResetConsumerD3dState() {
    ResetSharedTextures();
    staging_texture_.Reset();
    staging_width_ = 0;
    staging_height_ = 0;
    staging_format_ = DXGI_FORMAT_UNKNOWN;
    ResetGpuScaleResources();
    context_.Reset();
    device_.Reset();
    consumer_adapter_diagnostics_ = D3dAdapterDiagnostics{};
  }

  bool RecoverConsumerD3dDevice(const std::string& phase, HRESULT hr,
                                bool suspend_native_nv12) {
    if (!IsDeviceLossHResult(hr)) {
      return false;
    }

    const HRESULT removed_reason =
        device_ != nullptr ? device_->GetDeviceRemovedReason() : hr;
    ++consumer_device_recoveries_;
    Log("consumer_d3d_device_recovery_begin phase=" + phase + " hr=" +
        HResultHex(hr) + " removedReason=" + HResultHex(removed_reason) +
        " recoveries=" + std::to_string(consumer_device_recoveries_) +
        " suspendNativeNv12=" +
        std::string(suspend_native_nv12 ? "true" : "false"));

    if (suspend_native_nv12) {
      native_nv12_suspended_after_device_loss_ = true;
      native_nv12_device_loss_suspend_logged_ = false;
    }

    ResetConsumerD3dState();
    if (!EnsureD3dDevice()) {
      Log("consumer_d3d_device_recovery_failed phase=" + phase);
      return true;
    }
    OpenTexturesForGeneration();
    Log("consumer_d3d_device_recovery_done phase=" + phase +
        " openedGeneration=" + std::to_string(opened_generation_) +
        " nativeNv12Suspended=" +
        std::string(native_nv12_suspended_after_device_loss_ ? "true"
                                                             : "false"));
    return true;
  }

  double TicksToMs(int64_t ticks) const {
    const double frequency = frequency_.QuadPart == 0
                                 ? 1.0
                                 : static_cast<double>(frequency_.QuadPart);
    return static_cast<double>(ticks) * 1000.0 / frequency;
  }

  double AverageTicksMs(int64_t ticks, uint64_t count) const {
    return count == 0 ? 0.0 : TicksToMs(ticks) / static_cast<double>(count);
  }

  double SharedTicksToMs(uint64_t ticks) const {
    const double frequency = frequency_.QuadPart == 0
                                 ? 1.0
                                 : static_cast<double>(frequency_.QuadPart);
    return static_cast<double>(ticks) * 1000.0 / frequency;
  }

  double AverageSharedTicksMs(uint64_t ticks, uint64_t count) const {
    return count == 0 ? 0.0
                      : SharedTicksToMs(ticks) / static_cast<double>(count);
  }

  void RecordQpcMetric(int64_t ticks, int64_t* total, int64_t* max_value,
                       uint64_t* samples) {
    if (ticks < 0) {
      return;
    }
    *total += ticks;
    *max_value = std::max<int64_t>(*max_value, ticks);
    ++(*samples);
  }

  void RecordMsMetric(double ms, double* total, double* max_value,
                      uint64_t* samples) {
    if (ms < 0.0 || !std::isfinite(ms)) {
      return;
    }
    *total += ms;
    *max_value = std::max<double>(*max_value, ms);
    ++(*samples);
  }

  double AverageMetricMs(int64_t total, uint64_t samples) const {
    return samples == 0 ? 0.0 : TicksToMs(total) / static_cast<double>(samples);
  }

  double AverageMetricMs(double total, uint64_t samples) const {
    return samples == 0 ? 0.0 : total / static_cast<double>(samples);
  }

  void RecordGpuNv12ReadyObservation(GpuNv12ReadyObservation observation) {
    switch (observation) {
      case GpuNv12ReadyObservation::kImmediateAfterBlt:
        ++gpu_nv12_ready_observed_immediate_after_blt_frames_;
        return;
      case GpuNv12ReadyObservation::kPostFenceRegistration:
        ++gpu_nv12_ready_observed_post_fence_registration_frames_;
        return;
      case GpuNv12ReadyObservation::kFenceEvent:
        ++gpu_nv12_ready_observed_fence_event_frames_;
        return;
      case GpuNv12ReadyObservation::kSourceEvent:
        ++gpu_nv12_ready_observed_source_event_frames_;
        return;
      case GpuNv12ReadyObservation::kWaitOther:
        ++gpu_nv12_ready_observed_wait_other_frames_;
        return;
      case GpuNv12ReadyObservation::kLoopIdle:
        ++gpu_nv12_ready_observed_loop_idle_frames_;
        return;
      case GpuNv12ReadyObservation::kDuplicateSkip:
        ++gpu_nv12_ready_observed_duplicate_skip_frames_;
        return;
      case GpuNv12ReadyObservation::kPreSubmit:
        ++gpu_nv12_ready_observed_pre_submit_frames_;
        return;
      case GpuNv12ReadyObservation::kWriteSlotScan:
        ++gpu_nv12_ready_observed_write_slot_scan_frames_;
        return;
      case GpuNv12ReadyObservation::kUnknown:
        ++gpu_nv12_ready_observed_unknown_frames_;
        return;
    }
    ++gpu_nv12_ready_observed_unknown_frames_;
  }

  void RecordGpuNv12BltToReadyBudget(int64_t ticks) {
    if (ticks <= 0 || frequency_.QuadPart <= 0 || target_fps_ == 0) {
      return;
    }
    const int64_t frame_interval_qpc =
        std::max<int64_t>(1, frequency_.QuadPart / target_fps_);
    if (ticks > frame_interval_qpc) {
      ++gpu_nv12_blt_to_ready_over_1x_frames_;
    }
    if (ticks > frame_interval_qpc * 2) {
      ++gpu_nv12_blt_to_ready_over_2x_frames_;
    }
    if (ticks > frame_interval_qpc * 3) {
      ++gpu_nv12_blt_to_ready_over_3x_frames_;
    }
  }

  void RecordNativeAdmissionDeadlineDue(int64_t lateness_qpc,
                                        int64_t frame_interval_qpc) {
    ++native_admission_deadline_due_frames_;
    RecordQpcMetric(lateness_qpc, &native_admission_deadline_lateness_qpc_,
                    &native_admission_deadline_lateness_max_qpc_,
                    &native_admission_deadline_lateness_samples_);
    if (frame_interval_qpc <= 0) {
      return;
    }
    if (lateness_qpc > frame_interval_qpc) {
      ++native_admission_deadline_over_1x_frames_;
    }
    if (lateness_qpc > frame_interval_qpc * 2) {
      ++native_admission_deadline_over_2x_frames_;
    }
    if (lateness_qpc > frame_interval_qpc * 3) {
      ++native_admission_deadline_over_3x_frames_;
    }
  }

  uint64_t NativeNv12ReadyObservationTotal() const {
    return gpu_nv12_ready_frames_ + gpu_nv12_ready_dropped_frames_ +
           gpu_nv12_late_ready_dropped_frames_;
  }

  void RecordNativeAdmissionDeadlineNv12Outcome(
      size_t pending_before, uint64_t ready_observed_before) {
    if (pending_before == 0) {
      ++native_nv12_no_pending_on_deadline_frames_;
      return;
    }
    ++native_nv12_pending_on_deadline_frames_;
    if (NativeNv12ReadyObservationTotal() > ready_observed_before) {
      ++native_nv12_ready_on_deadline_frames_;
    } else {
      ++native_nv12_no_ready_on_deadline_frames_;
    }
  }

  bool NativeNv12ReadyExceedsLateDropBudget(int64_t blt_to_ready_qpc) const {
    if (!native_nv12_drop_late_ready_enabled_ || blt_to_ready_qpc <= 0 ||
        frequency_.QuadPart <= 0) {
      return false;
    }
    const int64_t threshold_qpc =
        (frequency_.QuadPart *
         static_cast<int64_t>(native_nv12_late_ready_drop_threshold_ms_)) /
        1000;
    return blt_to_ready_qpc > std::max<int64_t>(1, threshold_qpc);
  }

  int64_t NativeNv12FrameIntervalQpc() const {
    if (frequency_.QuadPart <= 0 || target_fps_ == 0) {
      return 0;
    }
    return std::max<int64_t>(1, frequency_.QuadPart / target_fps_);
  }

  int64_t NativeNv12GpuQueueBackoffThresholdQpc() const {
    const int64_t frame_interval_qpc = NativeNv12FrameIntervalQpc();
    if (frame_interval_qpc <= 0) {
      return 0;
    }
    return frame_interval_qpc *
           static_cast<int64_t>(std::max<uint32_t>(
               1, native_nv12_gpu_queue_backoff_threshold_frames_));
  }

  int64_t NativeNv12GpuQueueBackoffDurationQpc() const {
    const int64_t frame_interval_qpc = NativeNv12FrameIntervalQpc();
    if (frame_interval_qpc <= 0) {
      return 0;
    }
    return frame_interval_qpc *
           static_cast<int64_t>(std::max<uint32_t>(
               1, native_nv12_gpu_queue_backoff_duration_frames_));
  }

  void MaybeUpdateNativeNv12GpuQueueBackoff(
      int64_t blt_to_ready_qpc, int64_t now_qpc, uint64_t attempt,
      GpuNv12ReadyObservation observation) {
    if (!native_nv12_gpu_queue_backoff_enabled_ ||
        source_mode_ != GameCaptureSourceMode::kHelperD3d11 ||
        blt_to_ready_qpc <= 0) {
      return;
    }
    const int64_t threshold_qpc = NativeNv12GpuQueueBackoffThresholdQpc();
    const int64_t duration_qpc = NativeNv12GpuQueueBackoffDurationQpc();
    if (threshold_qpc <= 0 || duration_qpc <= 0 ||
        blt_to_ready_qpc <= threshold_qpc) {
      return;
    }

    native_nv12_gpu_queue_backoff_until_qpc_ = std::max<int64_t>(
        native_nv12_gpu_queue_backoff_until_qpc_, now_qpc + duration_qpc);
    ++native_nv12_gpu_queue_backoff_triggered_frames_;
    RecordQpcMetric(duration_qpc, &native_nv12_gpu_queue_backoff_duration_qpc_,
                    &native_nv12_gpu_queue_backoff_duration_max_qpc_,
                    &native_nv12_gpu_queue_backoff_duration_samples_);
    RecordQpcMetric(
        blt_to_ready_qpc,
        &native_nv12_gpu_queue_backoff_trigger_blt_to_ready_qpc_,
        &native_nv12_gpu_queue_backoff_trigger_blt_to_ready_max_qpc_,
        &native_nv12_gpu_queue_backoff_trigger_blt_to_ready_samples_);

    if (native_nv12_gpu_queue_backoff_triggered_frames_ <= 5 ||
        native_nv12_gpu_queue_backoff_triggered_frames_ % 30 == 0) {
      LogGpuNv12Stage(
          attempt,
          "gpu_queue_backoff_triggered observedBy=" +
              std::string(GpuNv12ReadyObservationName(observation)) +
              " bltToReadyMs=" + std::to_string(TicksToMs(blt_to_ready_qpc)) +
              " thresholdFrames=" +
              std::to_string(native_nv12_gpu_queue_backoff_threshold_frames_) +
              " durationFrames=" +
              std::to_string(native_nv12_gpu_queue_backoff_duration_frames_) +
              " triggers=" +
              std::to_string(native_nv12_gpu_queue_backoff_triggered_frames_));
    }
  }

  void ResetGpuNv12BltTimestampQueries(GpuNv12Slot& slot,
                                       bool release_queries) {
    slot.blt_timestamp_pending = false;
    if (!release_queries) {
      return;
    }
    slot.blt_timestamp_disjoint_query.Reset();
    slot.blt_timestamp_start_query.Reset();
    slot.blt_timestamp_end_query.Reset();
  }

  bool EnsureGpuNv12BltTimestampQueries(GpuNv12Slot& slot, uint64_t attempt) {
    if (device_ == nullptr || context_ == nullptr) {
      return false;
    }
    if (slot.blt_timestamp_pending) {
      ResetGpuNv12BltTimestampQueries(slot, true);
    }
    if (slot.blt_timestamp_disjoint_query == nullptr) {
      D3D11_QUERY_DESC query_desc{};
      query_desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
      HRESULT hr =
          device_->CreateQuery(&query_desc, &slot.blt_timestamp_disjoint_query);
      if (FAILED(hr)) {
        ++gpu_nv12_blt_gpu_timestamp_failures_;
        LogGpuNv12Stage(attempt, "blt_timestamp_disjoint_create_failed hr=" +
                                     HResultHex(hr));
        return false;
      }
    }
    if (slot.blt_timestamp_start_query == nullptr) {
      D3D11_QUERY_DESC query_desc{};
      query_desc.Query = D3D11_QUERY_TIMESTAMP;
      HRESULT hr =
          device_->CreateQuery(&query_desc, &slot.blt_timestamp_start_query);
      if (FAILED(hr)) {
        ++gpu_nv12_blt_gpu_timestamp_failures_;
        LogGpuNv12Stage(
            attempt, "blt_timestamp_start_create_failed hr=" + HResultHex(hr));
        return false;
      }
    }
    if (slot.blt_timestamp_end_query == nullptr) {
      D3D11_QUERY_DESC query_desc{};
      query_desc.Query = D3D11_QUERY_TIMESTAMP;
      HRESULT hr =
          device_->CreateQuery(&query_desc, &slot.blt_timestamp_end_query);
      if (FAILED(hr)) {
        ++gpu_nv12_blt_gpu_timestamp_failures_;
        LogGpuNv12Stage(attempt,
                        "blt_timestamp_end_create_failed hr=" + HResultHex(hr));
        return false;
      }
    }
    return true;
  }

  bool BeginGpuNv12BltTimestampQueries(GpuNv12Slot& slot, uint64_t attempt) {
    if (!EnsureGpuNv12BltTimestampQueries(slot, attempt)) {
      return false;
    }
    context_->Begin(slot.blt_timestamp_disjoint_query.Get());
    context_->End(slot.blt_timestamp_start_query.Get());
    slot.blt_timestamp_pending = true;
    return true;
  }

  void EndGpuNv12BltTimestampQueries(GpuNv12Slot& slot) {
    if (!slot.blt_timestamp_pending || context_ == nullptr) {
      return;
    }
    context_->End(slot.blt_timestamp_end_query.Get());
    context_->End(slot.blt_timestamp_disjoint_query.Get());
  }

  void TryRecordGpuNv12BltTimestampMetrics(GpuNv12Slot& slot,
                                           int64_t submit_to_fence_qpc) {
    if (!slot.blt_timestamp_pending || context_ == nullptr ||
        slot.blt_timestamp_disjoint_query == nullptr ||
        slot.blt_timestamp_start_query == nullptr ||
        slot.blt_timestamp_end_query == nullptr) {
      return;
    }

    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    UINT64 start_timestamp = 0;
    UINT64 end_timestamp = 0;
    const HRESULT disjoint_hr =
        context_->GetData(slot.blt_timestamp_disjoint_query.Get(), &disjoint,
                          sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    const HRESULT start_hr = context_->GetData(
        slot.blt_timestamp_start_query.Get(), &start_timestamp,
        sizeof(start_timestamp), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    const HRESULT end_hr = context_->GetData(
        slot.blt_timestamp_end_query.Get(), &end_timestamp,
        sizeof(end_timestamp), D3D11_ASYNC_GETDATA_DONOTFLUSH);

    if (disjoint_hr == S_FALSE || start_hr == S_FALSE || end_hr == S_FALSE) {
      ++gpu_nv12_blt_gpu_timestamp_not_ready_;
      ResetGpuNv12BltTimestampQueries(slot, true);
      return;
    }
    if (FAILED(disjoint_hr) || FAILED(start_hr) || FAILED(end_hr)) {
      ++gpu_nv12_blt_gpu_timestamp_failures_;
      ResetGpuNv12BltTimestampQueries(slot, true);
      return;
    }
    if (disjoint.Disjoint || disjoint.Frequency == 0 ||
        end_timestamp < start_timestamp) {
      ++gpu_nv12_blt_gpu_timestamp_disjoint_;
      ResetGpuNv12BltTimestampQueries(slot, false);
      return;
    }

    const double gpu_execution_ms =
        static_cast<double>(end_timestamp - start_timestamp) * 1000.0 /
        static_cast<double>(disjoint.Frequency);
    RecordMsMetric(gpu_execution_ms, &gpu_nv12_blt_gpu_execution_ms_,
                   &gpu_nv12_blt_gpu_execution_max_ms_,
                   &gpu_nv12_blt_gpu_execution_samples_);
    if (submit_to_fence_qpc > 0) {
      const double submit_to_fence_ms = TicksToMs(submit_to_fence_qpc);
      const double queue_delay_ms =
          std::max(0.0, submit_to_fence_ms - gpu_execution_ms);
      RecordMsMetric(queue_delay_ms, &gpu_nv12_blt_gpu_queue_delay_ms_,
                     &gpu_nv12_blt_gpu_queue_delay_max_ms_,
                     &gpu_nv12_blt_gpu_queue_delay_samples_);
    }
    ResetGpuNv12BltTimestampQueries(slot, false);
  }

  owt::base::IntergalacticD3D11Nv12Buffer::Metadata BuildNativeNv12Metadata(
      const char* source_mode, DXGI_FORMAT source_format,
      uint64_t source_frame_index, uint64_t source_qpc,
      int64_t created_qpc) const {
    owt::base::IntergalacticD3D11Nv12Buffer::Metadata metadata;
    metadata.source_mode = source_mode != nullptr
                               ? source_mode
                               : GameCaptureSourceModeName(source_mode_);
    metadata.source_format = static_cast<uint32_t>(source_format);
    metadata.source_frame_index = source_frame_index;
    metadata.source_qpc = source_qpc;
    metadata.created_qpc = created_qpc;
    if (source_qpc > 0 && created_qpc > static_cast<int64_t>(source_qpc)) {
      metadata.source_age_at_create_ms =
          TicksToMs(created_qpc - static_cast<int64_t>(source_qpc));
    }
    return metadata;
  }

  void SetCaptureThreadPriority(const char* name) {
    const BOOL boost_ok = SetThreadPriorityBoost(GetCurrentThread(), FALSE);
    const DWORD boost_error = boost_ok ? 0 : GetLastError();
    const BOOL priority_ok =
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    const DWORD priority_error = priority_ok ? 0 : GetLastError();
    Log(std::string(name) +
        "_priority boostOk=" + (boost_ok ? "true" : "false") +
        " boostError=" + std::to_string(boost_error) +
        " priorityOk=" + (priority_ok ? "true" : "false") +
        " priorityError=" + std::to_string(priority_error));
  }

  MmcssThreadRegistration RegisterMmcssThread(const char* name) {
    MmcssThreadRegistration registration;
    registration.library = LoadLibraryW(L"avrt.dll");
    if (registration.library == nullptr) {
      Log(std::string(name) +
          "_mmcss task=Capture registered=false "
          "reason=load_library error=" +
          std::to_string(GetLastError()));
      return registration;
    }

    auto set_characteristics =
        reinterpret_cast<AvSetMmThreadCharacteristicsWProc>(GetProcAddress(
            registration.library, "AvSetMmThreadCharacteristicsW"));
    registration.revert =
        reinterpret_cast<AvRevertMmThreadCharacteristicsProc>(GetProcAddress(
            registration.library, "AvRevertMmThreadCharacteristics"));
    if (set_characteristics == nullptr || registration.revert == nullptr) {
      Log(std::string(name) +
          "_mmcss task=Capture registered=false "
          "reason=missing_proc");
      return registration;
    }

    DWORD task_index = 0;
    registration.task_handle = set_characteristics(L"Capture", &task_index);
    const DWORD error =
        registration.task_handle == nullptr ? GetLastError() : 0;
    Log(std::string(name) + "_mmcss task=Capture registered=" +
        (registration.task_handle == nullptr ? "false" : "true") +
        " taskIndex=" + std::to_string(task_index) +
        " error=" + std::to_string(error));
    return registration;
  }

  int64_t TargetFrameIntervalUs() const {
    const size_t fps = std::clamp<size_t>(target_fps_, 1, 120);
    return static_cast<int64_t>(1000000 / fps);
  }

  bool SourceAgeExceedsNativeNv12Budget(uint64_t source_qpc,
                                        int64_t now_qpc) const {
    if (source_qpc == 0 || frequency_.QuadPart <= 0 ||
        now_qpc <= static_cast<int64_t>(source_qpc)) {
      return false;
    }
    const int64_t useful_age_us =
        TargetFrameIntervalUs() * kNativeNv12UsefulFrameAgeMultiplier;
    const int64_t useful_age_qpc =
        (frequency_.QuadPart * useful_age_us) / 1000000;
    return now_qpc - static_cast<int64_t>(source_qpc) > useful_age_qpc;
  }

  bool SourceAgeExceedsNativeNv12AdmissionBudget(uint64_t source_qpc,
                                                 int64_t now_qpc) const {
    if (native_nv12_admission_max_source_age_ms_ == 0) {
      return SourceAgeExceedsNativeNv12Budget(source_qpc, now_qpc);
    }
    if (source_qpc == 0 || frequency_.QuadPart <= 0 ||
        now_qpc <= static_cast<int64_t>(source_qpc)) {
      return false;
    }
    const int64_t threshold_qpc =
        (frequency_.QuadPart *
         static_cast<int64_t>(native_nv12_admission_max_source_age_ms_)) /
        1000;
    return now_qpc - static_cast<int64_t>(source_qpc) >
           std::max<int64_t>(1, threshold_qpc);
  }

  std::string SourceAgeMsLabel(uint64_t source_qpc, int64_t now_qpc) const {
    if (source_qpc == 0 || now_qpc <= static_cast<int64_t>(source_qpc)) {
      return "unknown";
    }
    return std::to_string(
        TicksToMs(now_qpc - static_cast<int64_t>(source_qpc)));
  }

  const char* TimestampModeLabel() const {
    return timestamp_source_qpc_frames_ > 0 ? "source-qpc" : "paced";
  }

  int64_t TimestampUsForFrame(uint64_t source_qpc, bool repeated) {
    const int64_t delivery_wall_us = webrtc::TimeMicros();
    RecordDeliveryWallDelta(delivery_wall_us);
    if (!repeated) {
      RecordSourceQpcDelta(source_qpc);
    }
    int64_t timestamp_us = 0;
    if (!repeated && source_qpc != 0 && frequency_.QuadPart > 0) {
      if (timestamp_base_source_qpc_ == 0) {
        timestamp_base_source_qpc_ = source_qpc;
        timestamp_base_us_ = delivery_wall_us;
        timestamp_us = delivery_wall_us;
      } else if (source_qpc > timestamp_base_source_qpc_) {
        const double elapsed_us =
            static_cast<double>(source_qpc - timestamp_base_source_qpc_) *
            1000000.0 / static_cast<double>(frequency_.QuadPart);
        timestamp_us =
            timestamp_base_us_ + static_cast<int64_t>(elapsed_us + 0.5);
      }
      if (timestamp_us > 0) {
        ++timestamp_source_qpc_frames_;
      }
    }
    if (timestamp_us == 0) {
      timestamp_us = last_frame_timestamp_us_ == 0
                         ? delivery_wall_us
                         : last_frame_timestamp_us_ + TargetFrameIntervalUs();
      if (repeated) {
        ++timestamp_repeated_frames_;
      } else {
        ++timestamp_paced_fallback_frames_;
      }
    }
    if (timestamp_us <= last_frame_timestamp_us_) {
      timestamp_us = last_frame_timestamp_us_ + 1000;
      ++timestamp_adjustments_;
    }
    if (last_frame_timestamp_us_ > 0) {
      const int64_t delta_us = timestamp_us - last_frame_timestamp_us_;
      RecordTimestampDelta(delta_us);
    }
    last_frame_timestamp_us_ = timestamp_us;
    return timestamp_us;
  }

  void RecordTimestampDelta(int64_t delta_us) {
    if (delta_us <= 0) {
      return;
    }
    timestamp_delta_us_total_ += delta_us;
    timestamp_delta_us_max_ =
        std::max<int64_t>(timestamp_delta_us_max_, delta_us);
    ++timestamp_delta_samples_;
    timestamp_delta_window_us_total_ += delta_us;
    timestamp_delta_window_us_max_ =
        std::max<int64_t>(timestamp_delta_window_us_max_, delta_us);
    ++timestamp_delta_window_samples_;
  }

  void RecordDeliveryWallDelta(int64_t delivery_wall_us) {
    if (last_delivery_wall_timestamp_us_ > 0 &&
        delivery_wall_us > last_delivery_wall_timestamp_us_) {
      const int64_t delta_us =
          delivery_wall_us - last_delivery_wall_timestamp_us_;
      const int64_t target_us = TargetFrameIntervalUs();
      delivery_wall_delta_us_total_ += delta_us;
      delivery_wall_delta_us_max_ =
          std::max<int64_t>(delivery_wall_delta_us_max_, delta_us);
      if (delivery_wall_delta_samples_ == 0 ||
          delta_us < delivery_wall_delta_us_min_) {
        delivery_wall_delta_us_min_ = delta_us;
      }
      if (delta_us > target_us * 2) {
        ++delivery_wall_over_2x_frames_;
        ++delivery_wall_window_over_2x_frames_;
      }
      if (delta_us > target_us * 3) {
        ++delivery_wall_over_3x_frames_;
        ++delivery_wall_window_over_3x_frames_;
      }
      if (delta_us < target_us / 2) {
        ++delivery_wall_under_half_frames_;
        ++delivery_wall_window_under_half_frames_;
      }
      ++delivery_wall_delta_samples_;
      delivery_wall_delta_window_us_total_ += delta_us;
      delivery_wall_delta_window_us_max_ =
          std::max<int64_t>(delivery_wall_delta_window_us_max_, delta_us);
      if (delivery_wall_delta_window_samples_ == 0 ||
          delta_us < delivery_wall_delta_window_us_min_) {
        delivery_wall_delta_window_us_min_ = delta_us;
      }
      ++delivery_wall_delta_window_samples_;
    }
    last_delivery_wall_timestamp_us_ = delivery_wall_us;
  }

  void RecordSourceQpcDelta(uint64_t source_qpc) {
    if (source_qpc == 0 || frequency_.QuadPart <= 0) {
      return;
    }
    if (last_timestamp_source_qpc_ != 0) {
      if (source_qpc <= last_timestamp_source_qpc_) {
        ++source_qpc_regressions_;
      } else {
        const uint64_t delta_qpc = source_qpc - last_timestamp_source_qpc_;
        const int64_t delta_us = static_cast<int64_t>(
            delta_qpc * 1000000.0 / static_cast<double>(frequency_.QuadPart));
        source_qpc_delta_us_total_ += delta_us;
        source_qpc_delta_us_max_ =
            std::max<int64_t>(source_qpc_delta_us_max_, delta_us);
        const int64_t target_us = TargetFrameIntervalUs();
        if (delta_us > target_us * 2) {
          ++source_qpc_over_2x_frames_;
        }
        if (delta_us > target_us * 3) {
          ++source_qpc_over_3x_frames_;
        }
        if (delta_us < target_us / 2) {
          ++source_qpc_under_half_frames_;
        }
        ++source_qpc_delta_samples_;
      }
    }
    last_timestamp_source_qpc_ = source_qpc;
  }

  void RecordSourceLatestObservation(uint64_t source_frame_index,
                                     uint64_t source_qpc,
                                     uint64_t producer_publish_qpc,
                                     int64_t observed_qpc,
                                     int64_t frame_interval_qpc) {
    if (source_frame_index == 0 ||
        source_frame_index == last_observed_source_frame_index_) {
      return;
    }

    ++source_latest_observed_frames_;
    if (last_observed_source_frame_index_ != 0) {
      if (source_frame_index > last_observed_source_frame_index_ + 1) {
        source_latest_frame_gaps_ +=
            source_frame_index - last_observed_source_frame_index_ - 1;
      } else if (source_frame_index < last_observed_source_frame_index_) {
        ++source_latest_frame_regressions_;
      }
    }

    if (source_qpc != 0 && last_observed_source_qpc_ != 0) {
      if (source_qpc <= last_observed_source_qpc_) {
        ++source_latest_qpc_regressions_;
      } else {
        const int64_t delta_qpc =
            static_cast<int64_t>(source_qpc - last_observed_source_qpc_);
        RecordQpcMetric(delta_qpc, &source_latest_qpc_delta_,
                        &source_latest_qpc_delta_max_,
                        &source_latest_qpc_delta_samples_);
        if (frame_interval_qpc > 0 && delta_qpc > frame_interval_qpc * 2) {
          ++source_latest_qpc_over_2x_frames_;
        }
        if (frame_interval_qpc > 0 && delta_qpc > frame_interval_qpc * 3) {
          ++source_latest_qpc_over_3x_frames_;
        }
        if (frame_interval_qpc > 0 && delta_qpc * 2 < frame_interval_qpc) {
          ++source_latest_qpc_under_half_frames_;
        }
      }
    }

    if (last_source_observation_qpc_ != 0 &&
        observed_qpc > last_source_observation_qpc_) {
      const int64_t delta_qpc = observed_qpc - last_source_observation_qpc_;
      RecordQpcMetric(delta_qpc, &source_latest_observation_delta_,
                      &source_latest_observation_delta_max_,
                      &source_latest_observation_delta_samples_);
      if (frame_interval_qpc > 0 && delta_qpc > frame_interval_qpc * 2) {
        ++source_latest_observation_over_2x_frames_;
      }
      if (frame_interval_qpc > 0 && delta_qpc > frame_interval_qpc * 3) {
        ++source_latest_observation_over_3x_frames_;
      }
    }

    if (source_qpc != 0 && observed_qpc > static_cast<int64_t>(source_qpc)) {
      const int64_t age_qpc = observed_qpc - static_cast<int64_t>(source_qpc);
      RecordQpcMetric(age_qpc, &source_latest_event_age_,
                      &source_latest_event_age_max_,
                      &source_latest_event_age_samples_);
      if (frame_interval_qpc > 0 && age_qpc > frame_interval_qpc) {
        ++source_latest_event_age_over_1x_frames_;
      }
      if (frame_interval_qpc > 0 && age_qpc > frame_interval_qpc * 2) {
        ++source_latest_event_age_over_2x_frames_;
      }
      if (frame_interval_qpc > 0 && age_qpc > frame_interval_qpc * 3) {
        ++source_latest_event_age_over_3x_frames_;
      }
    }

    if (producer_publish_qpc != 0 &&
        observed_qpc > static_cast<int64_t>(producer_publish_qpc)) {
      const int64_t age_qpc =
          observed_qpc - static_cast<int64_t>(producer_publish_qpc);
      RecordQpcMetric(age_qpc, &source_publish_observation_age_,
                      &source_publish_observation_age_max_,
                      &source_publish_observation_age_samples_);
      if (frame_interval_qpc > 0 && age_qpc > frame_interval_qpc) {
        ++source_publish_observation_age_over_1x_frames_;
      }
      if (frame_interval_qpc > 0 && age_qpc > frame_interval_qpc * 2) {
        ++source_publish_observation_age_over_2x_frames_;
      }
      if (frame_interval_qpc > 0 && age_qpc > frame_interval_qpc * 3) {
        ++source_publish_observation_age_over_3x_frames_;
      }
    }

    last_observed_source_frame_index_ = source_frame_index;
    last_observed_source_qpc_ = source_qpc;
    last_source_observation_qpc_ = observed_qpc;
  }

  // Seqlock read of the producer's state_sequence with full memory barriers on
  // both sides. Even means a stable published snapshot; odd means the producer
  // is mid-write.
  uint32_t ReadSharedStateSequence() const {
    MemoryBarrier();
    const uint32_t seq = *reinterpret_cast<volatile const uint32_t*>(
        &shared_state_->state_sequence);
    MemoryBarrier();
    return seq;
  }

  // Captures the producer's per-frame publish fields as one torn-free snapshot.
  // Returns false when the sequence never settled within the bounded attempt
  // budget (producer mid-write or died mid-write), so the caller skips this
  // tick instead of consuming a half-updated slot pointer/frame index pair.
  bool SnapshotSharedFrameState(uint32_t* out_slot,
                                uint64_t* out_frame_index,
                                uint64_t* out_frame_qpc,
                                uint64_t* out_producer_publish_qpc,
                                uint64_t* out_slot_frame_index) {
    for (int attempt = 0; attempt < kMaxSharedStateSeqReadAttempts; ++attempt) {
      const uint32_t seq_begin = ReadSharedStateSequence();
      if ((seq_begin & 1u) == 0u) {
        const uint32_t slot = shared_state_->latest_slot_index;
        const uint64_t frame_index = shared_state_->latest_frame_index;
        const uint64_t frame_qpc = shared_state_->latest_qpc;
        const uint64_t producer_publish_qpc =
            shared_state_->producer_latest_publish_qpc;
        // Guard the index before touching slots[] so a torn slot value cannot
        // read out of bounds; the seq recheck below discards it anyway.
        const uint64_t slot_frame_index =
            slot < kRingDepth ? shared_state_->slots[slot].frame_index : 0;
        const uint32_t seq_end = ReadSharedStateSequence();
        if (seq_begin == seq_end) {
          *out_slot = slot;
          *out_frame_index = frame_index;
          *out_frame_qpc = frame_qpc;
          *out_producer_publish_qpc = producer_publish_qpc;
          *out_slot_frame_index = slot_frame_index;
          return true;
        }
      }
      ++shared_state_seq_retries_;
      YieldProcessor();
    }
    ++shared_state_seq_giveups_;
    return false;
  }

  // Resolves the texture to hand downstream for a validated latest slot. On the
  // keyed-mutex path it copies the shared slot into a private texture while
  // holding the slot's mutex, then releases, so downstream scale/convert reads a
  // stable copy with no lock held. Returns nullptr to skip this tick (acquire
  // timeout/failure or copy-target allocation failure). On the event path it
  // returns the shared texture directly (seqlock already guarded the CPU state).
  ID3D11Texture2D* AcquireLatestConsumerTexture(uint32_t slot) {
    if (!consumer_sync_is_keyed_mutex_ || keyed_mutexes_[slot] == nullptr) {
      return textures_[slot].Get();
    }
    D3D11_TEXTURE2D_DESC desc{};
    textures_[slot]->GetDesc(&desc);
    if (!EnsureKeyedCopyTexture(desc)) {
      ++keyed_mutex_copy_failures_;
      return nullptr;
    }
    const HRESULT acq = keyed_mutexes_[slot]->AcquireSync(
        kKeyedMutexAcquireKey, kConsumerKeyedMutexTimeoutMs);
    if (acq == static_cast<HRESULT>(WAIT_TIMEOUT)) {
      ++keyed_mutex_acquire_timeouts_;
      return nullptr;
    }
    if (acq == static_cast<HRESULT>(WAIT_ABANDONED)) {
      // Producer died mid-hold: we now own the mutex; release it and skip the
      // possibly-torn frame. Teardown follows from the stopped producer.
      keyed_mutexes_[slot]->ReleaseSync(kKeyedMutexAcquireKey);
      ++keyed_mutex_acquire_failures_;
      return nullptr;
    }
    if (acq != S_OK) {
      ++keyed_mutex_acquire_failures_;
      return nullptr;
    }
    ++keyed_mutex_acquires_;
    context_->CopyResource(keyed_copy_texture_.Get(), textures_[slot].Get());
    if (FAILED(keyed_mutexes_[slot]->ReleaseSync(kKeyedMutexAcquireKey))) {
      ++keyed_mutex_release_failures_;
    }
    return keyed_copy_texture_.Get();
  }

  SourceRefreshResult RefreshLatestSharedTexture(
      ComPtr<ID3D11Texture2D>* latest_texture,
      uint64_t* latest_frame_index,
      uint64_t* latest_frame_qpc,
      int64_t frame_interval_qpc,
      const char* reason) {
    if (latest_texture == nullptr || latest_frame_index == nullptr ||
        latest_frame_qpc == nullptr || shared_state_ == nullptr ||
        shared_state_->magic != kProtocolMagic ||
        shared_state_->version != kSharedTextureStateVersion) {
      // A version mismatch is a stale hook/helper/DLL sidecar pairing, not a
      // transient. Surface it once so a no-frame run is diagnosable instead of
      // silent.
      if (shared_state_ != nullptr &&
          shared_state_->magic == kProtocolMagic &&
          shared_state_->version != kSharedTextureStateVersion &&
          !shared_state_version_mismatch_logged_) {
        shared_state_version_mismatch_logged_ = true;
        LogSync("shared_state_version_mismatch observed=" +
                std::to_string(shared_state_->version) + " expected=" +
                std::to_string(kSharedTextureStateVersion) +
                " action=no_frames_verify_hook_helper_dll_sidecar_pairing");
      }
      return SourceRefreshResult::kNoUpdate;
    }

    OpenTexturesForGeneration();
    uint32_t slot = 0;
    uint64_t frame_index = 0;
    uint64_t frame_qpc = 0;
    uint64_t producer_publish_qpc = 0;
    uint64_t slot_frame_index = 0;
    if (!SnapshotSharedFrameState(&slot, &frame_index, &frame_qpc,
                                  &producer_publish_qpc, &slot_frame_index)) {
      return SourceRefreshResult::kNoUpdate;
    }
    if (slot >= kRingDepth || frame_index == 0 || textures_[slot] == nullptr) {
      return SourceRefreshResult::kNoUpdate;
    }

    if (slot_frame_index != frame_index) {
      ++shared_slot_mismatch_frames_;
      if (!shared_slot_mismatch_logged_ ||
          shared_slot_mismatch_frames_ <= 5 ||
          shared_slot_mismatch_frames_ % 30 == 0) {
        shared_slot_mismatch_logged_ = true;
        Log("shared_slot_mismatch latestSlot=" + std::to_string(slot) +
            " latestFrameIndex=" + std::to_string(frame_index) +
            " slotFrameIndex=" + std::to_string(slot_frame_index) +
            " reason=" + std::string(reason != nullptr ? reason : "unknown") +
            " mismatches=" + std::to_string(shared_slot_mismatch_frames_));
      }
      return SourceRefreshResult::kSlotMismatch;
    }

    ID3D11Texture2D* consumer_texture = AcquireLatestConsumerTexture(slot);
    if (consumer_texture == nullptr) {
      // Keyed-mutex acquire timed out / failed for this tick; skip and retry on
      // the next frame rather than read the slot without the lock.
      return SourceRefreshResult::kNoUpdate;
    }
    *latest_texture = consumer_texture;
    *latest_frame_index = frame_index;
    *latest_frame_qpc = frame_qpc;
    LARGE_INTEGER observed_now{};
    QueryPerformanceCounter(&observed_now);
    RecordSourceLatestObservation(frame_index, frame_qpc,
                                  producer_publish_qpc, observed_now.QuadPart,
                                  frame_interval_qpc);
    return SourceRefreshResult::kUpdated;
  }

  bool ShouldDropRegressingSourceFrame(uint64_t source_frame_index,
                                       const char* phase) {
    if (source_frame_index == 0 || last_submitted_source_frame_index_ == 0 ||
        source_frame_index >= last_submitted_source_frame_index_) {
      return false;
    }

    ++source_frame_regressions_;
    if (!source_frame_regression_logged_ || source_frame_regressions_ <= 5 ||
        source_frame_regressions_ % 30 == 0) {
      source_frame_regression_logged_ = true;
      Log("source_frame_regression_drop phase=" + std::string(phase) +
          " sourceFrameIndex=" + std::to_string(source_frame_index) +
          " lastSubmittedSourceFrameIndex=" +
          std::to_string(last_submitted_source_frame_index_) +
          " regressions=" + std::to_string(source_frame_regressions_));
    }
    return true;
  }

  void RecordSubmittedSourceFrame(uint64_t source_frame_index) {
    if (source_frame_index == 0) {
      return;
    }
    previous_submitted_source_frame_index_ = last_submitted_source_frame_index_;
    if (last_submitted_source_frame_index_ != 0) {
      if (source_frame_index == last_submitted_source_frame_index_) {
        ++source_frame_duplicate_submissions_;
      } else if (source_frame_index > last_submitted_source_frame_index_ + 1) {
        source_frame_gaps_ +=
            source_frame_index - last_submitted_source_frame_index_ - 1;
      }
    }
    if (source_frame_index > last_submitted_source_frame_index_) {
      last_submitted_source_frame_index_ = source_frame_index;
    }
  }

  void RecordSourceAge(uint64_t source_qpc, int64_t now_qpc, int64_t* total,
                       int64_t* max_value, uint64_t* samples) {
    if (source_qpc == 0 || now_qpc <= static_cast<int64_t>(source_qpc)) {
      return;
    }
    RecordQpcMetric(now_qpc - static_cast<int64_t>(source_qpc), total,
                    max_value, samples);
  }

  bool IsFreshRelativeToSubmitted(uint64_t source_frame_index) const {
    return source_frame_index != 0 &&
           (last_submitted_source_frame_index_ == 0 ||
            source_frame_index > last_submitted_source_frame_index_);
  }

  void RecordDeliveryOverwrite(const PendingVideoFrame& frame,
                               int64_t now_qpc) {
    RecordSourceAge(frame.source_qpc, now_qpc, &delivery_overwrite_age_qpc_,
                    &delivery_overwrite_age_max_qpc_,
                    &delivery_overwrite_age_samples_);
    if (!frame.repeated &&
        IsFreshRelativeToSubmitted(frame.source_frame_index)) {
      ++delivery_overwritten_fresh_frames_;
    }
  }

  void RecordNativeNv12ReadyDrop(const GpuNv12Slot& slot, int64_t now_qpc) {
    RecordSourceAge(slot.source_qpc, now_qpc, &gpu_nv12_ready_drop_age_qpc_,
                    &gpu_nv12_ready_drop_age_max_qpc_,
                    &gpu_nv12_ready_drop_age_samples_);
    if (!slot.repeated && IsFreshRelativeToSubmitted(slot.source_frame_index)) {
      ++gpu_nv12_ready_dropped_fresh_frames_;
    }
  }

  void RecordNativeNv12LateReadyDrop(const GpuNv12Slot& slot, int64_t now_qpc,
                                     int64_t blt_to_ready_qpc) {
    RecordSourceAge(slot.source_qpc, now_qpc,
                    &gpu_nv12_late_ready_drop_age_qpc_,
                    &gpu_nv12_late_ready_drop_age_max_qpc_,
                    &gpu_nv12_late_ready_drop_age_samples_);
    RecordQpcMetric(blt_to_ready_qpc,
                    &gpu_nv12_late_ready_drop_blt_to_ready_qpc_,
                    &gpu_nv12_late_ready_drop_blt_to_ready_max_qpc_,
                    &gpu_nv12_late_ready_drop_blt_to_ready_samples_);
    if (!slot.repeated && IsFreshRelativeToSubmitted(slot.source_frame_index)) {
      ++gpu_nv12_late_ready_dropped_fresh_frames_;
    }
  }

  void RecordNativeNv12Overwrite(const GpuNv12Slot& slot, int64_t now_qpc) {
    RecordSourceAge(slot.source_qpc, now_qpc, &gpu_nv12_overwrite_age_qpc_,
                    &gpu_nv12_overwrite_age_max_qpc_,
                    &gpu_nv12_overwrite_age_samples_);
    if (!slot.repeated && IsFreshRelativeToSubmitted(slot.source_frame_index)) {
      ++gpu_nv12_overwritten_fresh_frames_;
    }
  }

  int64_t QueueFrameForDelivery(PendingVideoFrame frame) {
    if (frame.buffer == nullptr) {
      return 0;
    }
    LARGE_INTEGER queued_now{};
    QueryPerformanceCounter(&queued_now);
    if (frame.ready_qpc > 0) {
      RecordQpcMetric(queued_now.QuadPart - frame.ready_qpc,
                      &ready_to_queue_qpc_, &ready_to_queue_max_qpc_,
                      &ready_to_queue_samples_);
    }
    if (!frame.repeated && frame.source_qpc > 0 &&
        queued_now.QuadPart > static_cast<int64_t>(frame.source_qpc)) {
      RecordQpcMetric(
          queued_now.QuadPart - static_cast<int64_t>(frame.source_qpc),
          &source_to_queue_qpc_, &source_to_queue_max_qpc_,
          &source_to_queue_samples_);
    }
    frame.queued_qpc = queued_now.QuadPart;
    {
      std::lock_guard<std::mutex> lock(delivery_mutex_);
      if (!frame.repeated && frame.source_frame_index != 0) {
        while (!delivery_frames_.empty()) {
          const auto& queued = delivery_frames_.front();
          if (queued.source_frame_index == 0 ||
              queued.source_frame_index >= frame.source_frame_index) {
            break;
          }
          RecordDeliveryOverwrite(queued, queued_now.QuadPart);
          delivery_frames_.pop_front();
          ++delivery_overwritten_frames_;
        }
      }
      if (delivery_frames_.size() >= delivery_queue_depth_) {
        RecordDeliveryOverwrite(delivery_frames_.front(), queued_now.QuadPart);
        delivery_frames_.pop_front();
        ++delivery_overwritten_frames_;
      }
      delivery_frames_.push_back(std::move(frame));
      ++delivery_queued_frames_;
    }
    delivery_cv_.notify_one();
    return queued_now.QuadPart;
  }

  void StartDeliveryThread() {
    delivery_stop_.store(false);
    delivery_waiting_for_fresh_.store(false);
    Log("delivery_repeat_policy=" +
        std::string(DeliveryRepeatPolicyName(delivery_repeat_policy_)) +
        " sourceMode=" + GameCaptureSourceModeName(source_mode_) +
        " queueDepth=" + std::to_string(delivery_queue_depth_) +
        " nativeNv12ReadyPolicy=" +
        NativeNv12ReadyPolicyName(native_nv12_ready_policy_) +
        " nativeNv12FenceAvailable=" +
        std::string(native_nv12_fence_available_ ? "true" : "false") +
        " nativeNv12PendingPollMs=" +
        std::to_string(native_nv12_pending_poll_ms_) +
        " nativeNv12MaxPendingSlots=" +
        std::to_string(native_nv12_max_pending_slots_) +
        " nativeNv12ReadyDrainDepth=" +
        std::to_string(native_nv12_ready_drain_depth_) +
        " nativeNv12OnFrameBackpressureEnabled=" +
        std::string(native_nv12_onframe_backpressure_suspend_enabled_
                        ? "true"
                        : "false") +
        " nativeNv12OnFrameBackpressureThresholdMs=" +
        std::to_string(native_nv12_onframe_backpressure_threshold_ms_) +
        " nativeNv12OnFrameBackpressureFrameLimit=" +
        std::to_string(native_nv12_onframe_backpressure_frame_limit_) +
        " nativeNv12SingleInFlightEnabled=" +
        std::string(native_nv12_single_in_flight_enabled_ ? "true" : "false") +
        " nativeNv12AdmissionMailboxEnabled=" +
        std::string(native_nv12_admission_mailbox_enabled_ ? "true"
                                                           : "false") +
        " nativeNv12GpuQueueBackoffEnabled=" +
        std::string(native_nv12_gpu_queue_backoff_enabled_ ? "true" : "false") +
        " nativeNv12GpuQueueBackoffThresholdFrames=" +
        std::to_string(native_nv12_gpu_queue_backoff_threshold_frames_) +
        " nativeNv12GpuQueueBackoffDurationFrames=" +
        std::to_string(native_nv12_gpu_queue_backoff_duration_frames_) +
        " nativeNv12LateReadyDropEnabled=" +
        std::string(native_nv12_drop_late_ready_enabled_ ? "true" : "false") +
        " nativeNv12LateReadyDropThresholdMs=" +
        std::to_string(native_nv12_late_ready_drop_threshold_ms_) +
        " nativeNv12AdmissionMaxSourceAgeMs=" +
        std::to_string(native_nv12_admission_max_source_age_ms_) +
        " nativeNv12WarmupI420Frames=" +
        std::to_string(native_nv12_warmup_i420_frames_));
    delivery_worker_ = std::thread([this] { DeliveryLoop(); });
  }

  void StopDeliveryThread() {
    delivery_stop_.store(true);
    delivery_waiting_for_fresh_.store(false);
    delivery_cv_.notify_all();
    if (delivery_worker_.joinable() &&
        delivery_worker_.get_id() != std::this_thread::get_id()) {
      delivery_worker_.join();
      Log("delivery_worker_joined");
    }
    {
      std::lock_guard<std::mutex> lock(delivery_mutex_);
      delivery_frames_.clear();
      last_delivered_frame_ = PendingVideoFrame();
    }
  }

  void DeliveryLoop() {
    SetCaptureThreadPriority("delivery_thread");
    auto mmcss = RegisterMmcssThread("delivery_thread");
    const auto interval = std::chrono::microseconds(
        std::max<int64_t>(1000, TargetFrameIntervalUs()));
    auto next_tick = std::chrono::steady_clock::now();
    bool delivery_started = false;
    bool skipped_tick_waiting_for_fresh = false;
    while (true) {
      PendingVideoFrame frame;
      bool have_frame = false;
      bool submit_after_skipped_tick = false;
      {
        std::unique_lock<std::mutex> lock(delivery_mutex_);
        if (delivery_frames_.empty() && !delivery_started) {
          delivery_cv_.wait(lock, [this] {
            return delivery_stop_.load() || !delivery_frames_.empty();
          });
          next_tick = std::chrono::steady_clock::now();
          skipped_tick_waiting_for_fresh = false;
        } else {
          // Once a stream has started, repeat-last-frame mode keeps delivery on
          // the pacer clock. Skip-on-miss mode is source-driven: a fresh ready
          // frame can submit early because there is no stale repeat to hide
          // native readiness latency.
          delivery_cv_.wait_until(lock, next_tick, [this] {
            return delivery_stop_.load() || !delivery_frames_.empty();
          });
          if (!delivery_stop_.load() && !delivery_frames_.empty()) {
            const bool allow_immediate_fresh =
                skipped_tick_waiting_for_fresh ||
                delivery_repeat_policy_ == DeliveryRepeatPolicy::kSkipOnMiss;
            const auto now = std::chrono::steady_clock::now();
            if (now < next_tick) {
              if (allow_immediate_fresh) {
                ++delivery_fresh_immediate_frames_;
                if (delivery_fresh_immediate_frames_ <= 5 ||
                    delivery_fresh_immediate_frames_ % 60 == 0) {
                  Log("delivery_fresh_immediate frames=" +
                      std::to_string(delivery_fresh_immediate_frames_) +
                      " afterSkip=" +
                      std::string(skipped_tick_waiting_for_fresh ? "true"
                                                                 : "false") +
                      " repeatPolicy=" +
                      DeliveryRepeatPolicyName(delivery_repeat_policy_));
                }
              } else {
                delivery_cv_.wait_until(
                    lock, next_tick, [this] { return delivery_stop_.load(); });
              }
            }
          }
          if (!delivery_stop_.load() && delivery_frames_.empty() &&
              delivery_started) {
            const auto fresh_grace =
                delivery_repeat_policy_ == DeliveryRepeatPolicy::kSkipOnMiss
                    ? interval
                    : std::chrono::milliseconds(kDeliveryLateGraceMs);
            const auto grace_deadline = next_tick + fresh_grace;
            delivery_cv_.wait_until(lock, grace_deadline, [this] {
              return delivery_stop_.load() || !delivery_frames_.empty();
            });
          }
        }
        if (delivery_stop_.load()) {
          delivery_waiting_for_fresh_.store(false);
          break;
        }
        if (!delivery_frames_.empty()) {
          frame = std::move(delivery_frames_.front());
          delivery_frames_.pop_front();
          if (delivery_repeat_policy_ ==
              DeliveryRepeatPolicy::kRepeatLastFrame) {
            last_delivered_frame_ = frame;
          } else {
            last_delivered_frame_ = PendingVideoFrame();
          }
          have_frame = true;
          delivery_started = true;
          delivery_waiting_for_fresh_.store(false);
          submit_after_skipped_tick = skipped_tick_waiting_for_fresh;
          if (submit_after_skipped_tick) {
            ++delivery_fresh_wake_after_skip_frames_;
            if (delivery_fresh_wake_after_skip_frames_ <= 5 ||
                delivery_fresh_wake_after_skip_frames_ % 60 == 0) {
              Log("delivery_fresh_wake_after_skip wakes=" +
                  std::to_string(delivery_fresh_wake_after_skip_frames_));
            }
          }
          skipped_tick_waiting_for_fresh = false;
        } else if (delivery_started) {
          if (delivery_repeat_policy_ == DeliveryRepeatPolicy::kSkipOnMiss) {
            ++delivery_skip_no_queued_frames_;
            skipped_tick_waiting_for_fresh = true;
            delivery_waiting_for_fresh_.store(true);
            if (delivery_skip_no_queued_frames_ <= 5 ||
                delivery_skip_no_queued_frames_ % 60 == 0) {
              Log("delivery_skip_on_miss skips=" +
                  std::to_string(delivery_skip_no_queued_frames_) +
                  " repeatPolicy=" +
                  DeliveryRepeatPolicyName(delivery_repeat_policy_));
            }
          } else {
            frame = last_delivered_frame_;
            frame.repeated = true;
            frame.ready_qpc = 0;
            frame.queued_qpc = 0;
            ++delivery_repeat_no_queued_frames_;
            delivery_waiting_for_fresh_.store(false);
            have_frame = true;
          }
        }
      }
      if (have_frame) {
        SubmitDeliveredFrame(std::move(frame));
      }
      next_tick += interval;
      const auto now = std::chrono::steady_clock::now();
      if (next_tick < now - interval) {
        const auto lag_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                                  next_tick)
                .count();
        ++delivery_pacer_resyncs_;
        delivery_pacer_lag_ms_max_ =
            std::max<int64_t>(delivery_pacer_lag_ms_max_, lag_ms);
        if (delivery_pacer_resyncs_ <= 5 || delivery_pacer_resyncs_ % 30 == 0) {
          Log("delivery_pacer_resync lagMs=" + std::to_string(lag_ms) +
              " resyncs=" + std::to_string(delivery_pacer_resyncs_) +
              " maxLagMs=" + std::to_string(delivery_pacer_lag_ms_max_));
        }
        next_tick = now + interval;
      }
    }
  }

  void MaybeSuspendNativeNv12ForOnFrameBackpressure(
      const PendingVideoFrame& frame, int64_t on_frame_call_ticks) {
    if (!frame.native_nv12 || on_frame_call_ticks <= 0 ||
        !native_nv12_onframe_backpressure_suspend_enabled_) {
      return;
    }
    const double on_frame_call_ms = TicksToMs(on_frame_call_ticks);
    if (on_frame_call_ms <=
        static_cast<double>(native_nv12_onframe_backpressure_threshold_ms_)) {
      native_nv12_onframe_backpressure_streak_ = 0;
      return;
    }

    ++native_nv12_onframe_backpressure_frames_;
    ++native_nv12_onframe_backpressure_streak_;
    native_nv12_onframe_backpressure_max_ms_ =
        std::max(native_nv12_onframe_backpressure_max_ms_, on_frame_call_ms);
    if (native_nv12_onframe_backpressure_streak_ <
        native_nv12_onframe_backpressure_frame_limit_) {
      return;
    }

    bool expected = false;
    if (!native_nv12_suspended_after_onframe_backpressure_
             .compare_exchange_strong(expected, true)) {
      return;
    }
    native_nv12_onframe_backpressure_suspend_logged_.store(true);

    Log("native_nv12_encoder_handoff_suspended "
        "reason=live_onframe_backpressure "
        "onFrameCallMs=" +
        std::to_string(on_frame_call_ms) + " thresholdMs=" +
        std::to_string(native_nv12_onframe_backpressure_threshold_ms_) +
        " streak=" + std::to_string(native_nv12_onframe_backpressure_streak_) +
        " frameLimit=" +
        std::to_string(native_nv12_onframe_backpressure_frame_limit_) +
        " slowFrames=" +
        std::to_string(native_nv12_onframe_backpressure_frames_) +
        " maxMs=" + std::to_string(native_nv12_onframe_backpressure_max_ms_) +
        " submitted=" + std::to_string(submitted_frames_) +
        " nativeNv12Submitted=" + std::to_string(gpu_nv12_submitted_frames_) +
        " using_gpu_scale_i420_readback=true");
  }

  void SubmitDeliveredFrame(PendingVideoFrame frame) {
    if (frame.buffer == nullptr) {
      return;
    }

    LARGE_INTEGER submit_start{};
    LARGE_INTEGER on_frame_call_start{};
    LARGE_INTEGER on_frame_call_end{};
    LARGE_INTEGER release_start{};
    LARGE_INTEGER release_end{};
    LARGE_INTEGER submit_end{};
    QueryPerformanceCounter(&submit_start);
    if (frame.queued_qpc > 0) {
      RecordQpcMetric(submit_start.QuadPart - frame.queued_qpc,
                      &delivery_queue_wait_qpc_, &delivery_queue_wait_max_qpc_,
                      &delivery_queue_wait_samples_);
    }
    if (frame.repeated) {
      RecordSourceAge(frame.source_qpc, submit_start.QuadPart,
                      &delivery_repeat_source_age_qpc_,
                      &delivery_repeat_source_age_max_qpc_,
                      &delivery_repeat_source_age_samples_);
    }
    if (frame.ready_qpc > 0) {
      RecordQpcMetric(submit_start.QuadPart - frame.ready_qpc,
                      &ready_to_submit_qpc_, &ready_to_submit_max_qpc_,
                      &ready_to_submit_samples_);
    }
    if (!frame.repeated && frame.source_qpc > 0 &&
        submit_start.QuadPart > static_cast<int64_t>(frame.source_qpc)) {
      RecordQpcMetric(
          submit_start.QuadPart - static_cast<int64_t>(frame.source_qpc),
          &source_to_submit_qpc_, &source_to_submit_max_qpc_,
          &source_to_submit_samples_);
    }
    {
      auto video_frame =
          webrtc::VideoFrame::Builder()
              .set_video_frame_buffer(frame.buffer)
              .set_rotation(webrtc::kVideoRotation_0)
              .set_timestamp_us(
                  TimestampUsForFrame(frame.source_qpc, frame.repeated))
              .set_id(SourceFrameTrackingId(frame.source_frame_index))
              .build();
      QueryPerformanceCounter(&on_frame_call_start);
      OnFrame(video_frame);
      QueryPerformanceCounter(&on_frame_call_end);
    }

    RecordQpcMetric(on_frame_call_start.QuadPart - submit_start.QuadPart,
                    &delivery_submit_prep_qpc_, &delivery_submit_prep_max_qpc_,
                    &delivery_submit_prep_samples_);
    RecordQpcMetric(on_frame_call_end.QuadPart - on_frame_call_start.QuadPart,
                    &delivery_on_frame_call_qpc_,
                    &delivery_on_frame_call_max_qpc_,
                    &delivery_on_frame_call_samples_);
    ++delivery_submitted_frames_;
    ++submitted_frames_;
    RecordSubmittedSourceFrame(frame.source_frame_index);
    if (frame.gpu_scaled) {
      ++gpu_scaled_frames_;
    }
    if (frame.native_nv12) {
      ++gpu_nv12_submitted_frames_;
    }
    MaybeSuspendNativeNv12ForOnFrameBackpressure(
        frame, on_frame_call_end.QuadPart - on_frame_call_start.QuadPart);
    if (frame.repeated) {
      ++repeated_frames_;
    }
    last_source_width_ = frame.source_width;
    last_source_height_ = frame.source_height;
    last_source_format_ = frame.source_format;
    last_output_width_ = frame.output_width;
    last_output_height_ = frame.output_height;
    if (frame.native_nv12 && frame.buffer != nullptr) {
      QueryPerformanceCounter(&release_start);
      frame.buffer = nullptr;
      QueryPerformanceCounter(&release_end);
      RecordQpcMetric(release_end.QuadPart - release_start.QuadPart,
                      &native_buffer_release_qpc_,
                      &native_buffer_release_max_qpc_,
                      &native_buffer_release_samples_);
    }
    QueryPerformanceCounter(&submit_end);
    RecordQpcMetric(submit_end.QuadPart - on_frame_call_end.QuadPart,
                    &delivery_post_on_frame_qpc_,
                    &delivery_post_on_frame_max_qpc_,
                    &delivery_post_on_frame_samples_);
    const int64_t submit_ticks = submit_end.QuadPart - submit_start.QuadPart;
    delivery_on_frame_qpc_ += submit_ticks;
    delivery_on_frame_max_qpc_ =
        std::max<int64_t>(delivery_on_frame_max_qpc_, submit_ticks);
    MaybeLogStats(frame.source_width, frame.source_height, frame.source_format);
  }

  bool LaunchHelper(const std::string& session_id) {
    std::wstring command = QuoteArg(helper_path_);
    command += L" --pid " + std::to_wstring(target_process_id_);
    command += L" --session-id " + WideFromAscii(session_id);
    command += L" --duration-ms " + std::to_wstring(kLiveDurationMs);
    command += L" --max-saved-frames 0";
    command += L" --host-consume-frames false";
    command += L" --external-consumer true";
    if (native_keyed_mutex_enabled_) {
      // Opt-in only. Default leaves the producer on the legacy
      // shared-handle/event ring, so the published sync_kind stays kEvent and
      // consumers skip the keyed-mutex acquire and its copy-under-lock.
      command += L" --enable-keyed-mutex true";
    }
    if (!helper_output_root_.empty()) {
      command += L" --output-root " + QuoteArg(helper_output_root_);
    }
    const size_t helper_hook_target_fps =
        ResolveHelperHookTargetFps(target_fps_);
    command += L" --hook-target-fps " + std::to_wstring(helper_hook_target_fps);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    std::vector<wchar_t> command_buffer(command.begin(), command.end());
    command_buffer.push_back(L'\0');
    const BOOL ok =
        CreateProcessW(nullptr, command_buffer.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
    if (!ok) {
      Log("helper_launch_failed error=" + std::to_string(GetLastError()));
      return false;
    }
    helper_process_ = info.hProcess;
    CloseHandle(info.hThread);
    Log("helper_launched pid=" + std::to_string(target_process_id_) +
        " target=" + std::to_string(max_width_) + "x" +
        std::to_string(max_height_) + "@" + std::to_string(target_fps_) +
        " hookTargetFps=" + std::to_string(helper_hook_target_fps) +
        " outputRoot=\"" + WideToUtf8(helper_output_root_) + "\"");
    return true;
  }

  int EvenOutputDimension(size_t requested, int fallback) const {
    const int clamped = std::max<int>(
        2, static_cast<int>(requested == 0 ? fallback : requested));
    return clamped & ~1;
  }

  void DummyPatternRgb(int x, int y, uint64_t frame_index, int width,
                       int height, int* r, int* g, int* b) const {
    const int band = std::clamp<int>((x * 6) / std::max(1, width), 0, 5);
    static constexpr int kBars[6][3] = {
        {235, 235, 235}, {235, 220, 30}, {30, 210, 210},
        {30, 200, 70},   {220, 45, 180}, {45, 75, 235},
    };
    *r = kBars[band][0];
    *g = kBars[band][1];
    *b = kBars[band][2];

    const int stripe =
        static_cast<int>((x + (y * 2) + (frame_index * 23)) & 0x7f);
    const int motion_delta = stripe < 64 ? 28 : -28;
    *r = std::clamp(*r + motion_delta, 0, 255);
    *g = std::clamp(*g + motion_delta, 0, 255);
    *b = std::clamp(*b + motion_delta, 0, 255);

    const int box_size = std::max(48, std::min(width, height) / 8);
    const int box_x =
        static_cast<int>((frame_index * 11) % std::max(1, width + box_size)) -
        box_size;
    const int box_y =
        static_cast<int>((frame_index * 7) % std::max(1, height + box_size)) -
        box_size;
    if (x >= box_x && x < box_x + box_size && y >= box_y &&
        y < box_y + box_size) {
      *r = 255;
      *g = 80;
      *b = 45;
    }
  }

  void RgbToNv12(int r, int g, int b, uint8_t* y, uint8_t* u, uint8_t* v) {
    const int yy = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
    const int uu = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
    const int vv = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
    *y = static_cast<uint8_t>(std::clamp(yy, 16, 235));
    *u = static_cast<uint8_t>(std::clamp(uu, 16, 240));
    *v = static_cast<uint8_t>(std::clamp(vv, 16, 240));
  }

  void FillDummyNv12FrameData(uint64_t frame_index, int width, int height) {
    const size_t y_size = static_cast<size_t>(width) * height;
    const size_t total_size = y_size + y_size / 2;
    if (dummy_nv12_frame_data_.size() != total_size) {
      dummy_nv12_frame_data_.assign(total_size, 0);
    }
    uint8_t* y_plane = dummy_nv12_frame_data_.data();
    uint8_t* uv_plane = y_plane + y_size;
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        int r = 0;
        int g = 0;
        int b = 0;
        DummyPatternRgb(x, y, frame_index, width, height, &r, &g, &b);
        uint8_t yy = 0;
        uint8_t uu = 0;
        uint8_t vv = 0;
        RgbToNv12(r, g, b, &yy, &uu, &vv);
        y_plane[static_cast<size_t>(y) * width + x] = yy;
      }
    }
    for (int y = 0; y < height / 2; ++y) {
      for (int x = 0; x < width / 2; ++x) {
        int r = 0;
        int g = 0;
        int b = 0;
        DummyPatternRgb(x * 2, y * 2, frame_index, width, height, &r, &g, &b);
        uint8_t yy = 0;
        uint8_t uu = 0;
        uint8_t vv = 0;
        RgbToNv12(r, g, b, &yy, &uu, &vv);
        const size_t offset = static_cast<size_t>(y) * width + x * 2;
        uv_plane[offset] = uu;
        uv_plane[offset + 1] = vv;
      }
    }
  }

  bool EnsureDummyNv12Resources(int width, int height) {
    if (device_ == nullptr || context_ == nullptr || width <= 0 ||
        height <= 0) {
      return false;
    }
    if (dummy_nv12_width_ == width && dummy_nv12_height_ == height) {
      bool ready = true;
      for (const auto& texture : dummy_nv12_textures_) {
        ready = ready && texture != nullptr;
      }
      if (ready) {
        return true;
      }
    }
    for (auto& texture : dummy_nv12_textures_) {
      texture.Reset();
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    for (auto& texture : dummy_nv12_textures_) {
      HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture);
      if (FAILED(hr) || texture == nullptr) {
        LogGpuNv12Failure("dummy_nv12_texture_create_failed", hr);
        return false;
      }
    }
    dummy_nv12_width_ = width;
    dummy_nv12_height_ = height;
    dummy_nv12_index_ = 0;
    EnsureNativeNv12FenceResources();
    Log("dummy_nv12_resources_ready output=" + std::to_string(width) + "x" +
        std::to_string(height) +
        " ring=" + std::to_string(dummy_nv12_textures_.size()) +
        " readyPolicy=" + NativeNv12ReadyPolicyName(native_nv12_ready_policy_) +
        " fenceAvailable=" +
        std::string(native_nv12_fence_available_ ? "true" : "false"));
    return true;
  }

  bool SignalDummyNv12Fence(ComPtr<ID3D11Fence>* fence, uint64_t* fence_value) {
    fence->Reset();
    *fence_value = 0;
    if (!EnsureNativeNv12FenceResources()) {
      return false;
    }
    const uint64_t value = ++gpu_nv12_ready_fence_value_;
    const HRESULT hr =
        gpu_nv12_fence_context_->Signal(gpu_nv12_ready_fence_.Get(), value);
    if (FAILED(hr)) {
      ++gpu_nv12_fence_signal_failures_;
      LogGpuNv12Failure("dummy_native_ready_fence_signal_failed", hr);
      return false;
    }
    *fence = gpu_nv12_ready_fence_;
    *fence_value = value;
    ++gpu_nv12_fence_signaled_frames_;
    if (value <= 3 || value % 60 == 0) {
      LogSync("dummy_nv12_ready_fence_signaled value=" + std::to_string(value));
    }
    return true;
  }

  bool SubmitDummyNv12Frame(uint64_t frame_index, uint64_t source_qpc) {
    const int output_width = EvenOutputDimension(max_width_, 1280);
    const int output_height = EvenOutputDimension(max_height_, 720);
    if (!EnsureDummyNv12Resources(output_width, output_height)) {
      return false;
    }
    auto texture = dummy_nv12_textures_[dummy_nv12_index_];
    dummy_nv12_index_ = (dummy_nv12_index_ + 1) % dummy_nv12_textures_.size();
    if (texture == nullptr) {
      return false;
    }

    LARGE_INTEGER update_start{};
    LARGE_INTEGER after_update{};
    QueryPerformanceCounter(&update_start);
    FillDummyNv12FrameData(frame_index, output_width, output_height);
    context_->UpdateSubresource(
        texture.Get(), 0, nullptr, dummy_nv12_frame_data_.data(),
        static_cast<UINT>(output_width),
        static_cast<UINT>(dummy_nv12_frame_data_.size()));
    ComPtr<ID3D11Fence> ready_fence;
    uint64_t ready_fence_value = 0;
    const bool signaled_fence =
        SignalDummyNv12Fence(&ready_fence, &ready_fence_value);
    if (!signaled_fence) {
      context_->Flush();
    }
    QueryPerformanceCounter(&after_update);
    RecordQpcMetric(after_update.QuadPart - update_start.QuadPart,
                    &gpu_nv12_convert_qpc_, &gpu_nv12_convert_max_qpc_,
                    &gpu_nv12_convert_samples_);
    if (source_qpc > 0 &&
        after_update.QuadPart > static_cast<int64_t>(source_qpc)) {
      RecordQpcMetric(after_update.QuadPart - static_cast<int64_t>(source_qpc),
                      &gpu_nv12_conversion_start_age_qpc_,
                      &gpu_nv12_conversion_start_age_max_qpc_,
                      &gpu_nv12_conversion_start_age_samples_);
    }

    auto native_buffer = owt::base::IntergalacticD3D11Nv12Buffer::Create(
        device_, texture, output_width, output_height, ready_fence,
        ready_fence_value,
        BuildNativeNv12Metadata("dummy-nv12-live-sender", DXGI_FORMAT_NV12,
                                frame_index, source_qpc,
                                after_update.QuadPart));
    LARGE_INTEGER after_buffer{};
    QueryPerformanceCounter(&after_buffer);
    if (native_buffer == nullptr) {
      LogGpuNv12Failure("dummy_native_buffer_create_failed", E_FAIL);
      return false;
    }
    RecordQpcMetric(after_buffer.QuadPart - after_update.QuadPart,
                    &gpu_nv12_buffer_create_qpc_,
                    &gpu_nv12_buffer_create_max_qpc_,
                    &gpu_nv12_buffer_create_samples_);

    PendingVideoFrame frame;
    frame.buffer = native_buffer;
    frame.source_width = output_width;
    frame.source_height = output_height;
    frame.output_width = output_width;
    frame.output_height = output_height;
    frame.source_format = DXGI_FORMAT_NV12;
    frame.source_frame_index = frame_index;
    frame.source_qpc = source_qpc;
    frame.ready_qpc = after_buffer.QuadPart;
    frame.gpu_scaled = true;
    frame.native_nv12 = true;
    ++gpu_nv12_queued_frames_;
    ++gpu_nv12_ready_frames_;
    QueueFrameForDelivery(std::move(frame));
    if (frame_index <= 3 || frame_index % 60 == 0) {
      LogSync("dummy_nv12_frame_queued frame=" + std::to_string(frame_index) +
              " output=" + std::to_string(output_width) + "x" +
              std::to_string(output_height));
    }
    return true;
  }

  void RunDummyNv12LiveSenderSource() {
    StartDeliveryThread();
    const int64_t frame_interval_qpc =
        std::max<int64_t>(1, frequency_.QuadPart / target_fps_);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    int64_t next_due_qpc = now.QuadPart;
    uint64_t frame_index = 0;
    Log("dummy_nv12_live_sender_start output=" +
        std::to_string(EvenOutputDimension(max_width_, 1280)) + "x" +
        std::to_string(EvenOutputDimension(max_height_, 720)) +
        " targetFps=" + std::to_string(target_fps_));
    while (!stop_requested_.load()) {
      QueryPerformanceCounter(&now);
      if (now.QuadPart < next_due_qpc) {
        const int64_t wait_qpc = next_due_qpc - now.QuadPart;
        const DWORD wait_ms = static_cast<DWORD>(std::clamp<int64_t>(
            (wait_qpc * 1000) / std::max<LONGLONG>(1, frequency_.QuadPart), 1,
            16));
        Sleep(wait_ms);
        continue;
      }
      ++frame_index;
      SubmitDummyNv12Frame(frame_index, now.QuadPart);
      next_due_qpc += frame_interval_qpc;
      while (next_due_qpc <= now.QuadPart - frame_interval_qpc) {
        next_due_qpc += frame_interval_qpc;
      }
    }
    Log("dummy_nv12_live_sender_stop queued=" +
        std::to_string(gpu_nv12_queued_frames_) +
        " submitted=" + std::to_string(submitted_frames_));
  }

  bool OpenSharedState(const std::string& session_id) {
    const std::wstring session = WideFromAscii(session_id);
    const std::wstring base = L"Local\\InterGalacticGameCapture." + session;
    const std::wstring stop_name = base + L".stop";
    const std::wstring frame_name = base + L".frame";
    const std::wstring shared_name = base + L".sharedState";

    const DWORD start = GetTickCount();
    while (!stop_requested_.load() &&
           GetTickCount() - start < kMaxHelperStartWaitMs) {
      if (helper_process_ != nullptr &&
          WaitForSingleObject(helper_process_, 0) == WAIT_OBJECT_0) {
        helper_exited_before_shared_state_ = true;
        helper_exit_code_available_ =
            GetExitCodeProcess(helper_process_, &helper_exit_code_);
        open_shared_state_wait_ms_ = GetTickCount() - start;
        Log("shared_state_open_failed helper_exited session=" + session_id +
            " exitCode=" + std::to_string(helper_exit_code_) +
            " exitCodeAvailable=" +
            std::string(helper_exit_code_available_ ? "true" : "false"));
        startup_failure_reason_ = "helper_exited_before_shared_state";
        return false;
      }
      if (stop_event_ == nullptr) {
        stop_event_ = OpenEventW(EVENT_MODIFY_STATE, FALSE, stop_name.c_str());
      }
      if (frame_event_ == nullptr) {
        frame_event_ = OpenEventW(SYNCHRONIZE, FALSE, frame_name.c_str());
      }
      if (shared_mapping_ == nullptr) {
        shared_mapping_ =
            OpenFileMappingW(FILE_MAP_READ, FALSE, shared_name.c_str());
      }
      if (shared_state_ == nullptr && shared_mapping_ != nullptr) {
        shared_state_ = reinterpret_cast<SharedTextureState*>(MapViewOfFile(
            shared_mapping_, FILE_MAP_READ, 0, 0, sizeof(SharedTextureState)));
      }
      if (stop_event_ != nullptr && frame_event_ != nullptr &&
          shared_state_ != nullptr) {
        open_shared_state_wait_ms_ = GetTickCount() - start;
        Log("shared_state_opened session=" + session_id);
        return true;
      }
      Sleep(50);
    }
    open_shared_state_wait_ms_ = GetTickCount() - start;
    Log("shared_state_open_failed session=" + session_id);
    return false;
  }

  bool EnsureD3dDevice() {
    if (device_ != nullptr && context_ != nullptr) {
      return true;
    }
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL level{};
    const HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION, &device_, &level, &context_);
    if (FAILED(hr)) {
      Log("d3d11_create_failed hr=" + HResultHex(hr));
      return false;
    }
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(device_.As(&multithread)) && multithread != nullptr) {
      const BOOL previous = multithread->SetMultithreadProtected(TRUE);
      Log("d3d11_multithread_protected enabled=true previous=" +
          std::to_string(previous ? 1 : 0));
    } else {
      Log("d3d11_multithread_protected unavailable");
    }
    MaybeApplyConsumerGpuThreadPriority();
    consumer_adapter_diagnostics_ = QueryD3dAdapterDiagnostics(device_.Get());
    Log("consumer_adapter luid=" + consumer_adapter_diagnostics_.luid +
        " vendorId=" + std::to_string(consumer_adapter_diagnostics_.vendor_id) +
        " deviceId=" + std::to_string(consumer_adapter_diagnostics_.device_id) +
        " sourceAdapterLuid=unknown crossAdapterSuspected=unknown");
    return true;
  }

  void MaybeApplyConsumerGpuThreadPriority() {
    int requested_priority = 0;
    if (!ReadConsumerGpuThreadPriority(&requested_priority)) {
      return;
    }
    consumer_gpu_thread_priority_requested_ = true;
    consumer_gpu_thread_priority_requested_value_ = requested_priority;

    ComPtr<IDXGIDevice> dxgi_device;
    HRESULT hr = device_.As(&dxgi_device);
    if (FAILED(hr) || dxgi_device == nullptr) {
      consumer_gpu_thread_priority_hr_ = hr;
      Log("consumer_gpu_thread_priority requested=" +
          std::to_string(requested_priority) +
          " applied=false reason=query_dxgi_device hr=" + HResultHex(hr));
      return;
    }

    INT before_priority = 0;
    const HRESULT before_hr =
        dxgi_device->GetGPUThreadPriority(&before_priority);
    if (SUCCEEDED(before_hr)) {
      consumer_gpu_thread_priority_before_ = before_priority;
    }

    hr = dxgi_device->SetGPUThreadPriority(requested_priority);
    consumer_gpu_thread_priority_hr_ = hr;
    consumer_gpu_thread_priority_applied_ = SUCCEEDED(hr);

    INT after_priority = before_priority;
    const HRESULT after_hr = dxgi_device->GetGPUThreadPriority(&after_priority);
    if (SUCCEEDED(after_hr)) {
      consumer_gpu_thread_priority_after_ = after_priority;
    }
    Log("consumer_gpu_thread_priority requested=" +
        std::to_string(requested_priority) + " applied=" +
        std::string(consumer_gpu_thread_priority_applied_ ? "true" : "false") +
        " before=" + std::to_string(consumer_gpu_thread_priority_before_) +
        " after=" + std::to_string(consumer_gpu_thread_priority_after_) +
        " hr=" + HResultHex(hr));
  }

  void ResetSharedTextures() {
    for (auto& texture : textures_) {
      texture.Reset();
    }
    for (auto& keyed_mutex : keyed_mutexes_) {
      keyed_mutex.Reset();
    }
    consumer_sync_is_keyed_mutex_ = false;
    opened_generation_ = 0;
  }

  // Ensures the private texture the consumer copies each keyed-mutex frame into
  // matches the shared source desc. Downstream scale/convert reads this copy, so
  // it needs SHADER_RESOURCE bind; it is never shared.
  bool EnsureKeyedCopyTexture(const D3D11_TEXTURE2D_DESC& source_desc) {
    if (keyed_copy_texture_ != nullptr &&
        keyed_copy_width_ == source_desc.Width &&
        keyed_copy_height_ == source_desc.Height &&
        keyed_copy_format_ == source_desc.Format) {
      return true;
    }
    keyed_copy_texture_.Reset();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = source_desc.Width;
    desc.Height = source_desc.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = source_desc.Format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    const HRESULT hr =
        device_->CreateTexture2D(&desc, nullptr, &keyed_copy_texture_);
    if (FAILED(hr)) {
      keyed_copy_texture_.Reset();
      return false;
    }
    keyed_copy_width_ = source_desc.Width;
    keyed_copy_height_ = source_desc.Height;
    keyed_copy_format_ = source_desc.Format;
    return true;
  }

  // Ring-state fields the producer publishes together under PublishRingState's
  // seqlock window (on resize/reinit). Snapshotting them as a unit prevents a
  // torn generation/sync_kind/handle read from opening the wrong or a
  // half-updated slot handle.
  struct SharedRingSnapshot {
    uint64_t generation = 0;
    uint32_t sync_kind = 0;
    uint32_t ring_depth = 0;
    uint32_t backbuffer_width = 0;
    uint32_t backbuffer_height = 0;
    uint32_t backbuffer_format = 0;
    uint64_t slot_handles[kRingDepth] = {};
  };

  bool SnapshotSharedRingState(SharedRingSnapshot* out) {
    for (int attempt = 0; attempt < kMaxSharedStateSeqReadAttempts; ++attempt) {
      const uint32_t seq_begin = ReadSharedStateSequence();
      if ((seq_begin & 1u) == 0u) {
        SharedRingSnapshot snap;
        snap.generation = shared_state_->generation;
        snap.sync_kind = shared_state_->sync_kind;
        snap.ring_depth = shared_state_->ring_depth;
        snap.backbuffer_width = shared_state_->backbuffer_width;
        snap.backbuffer_height = shared_state_->backbuffer_height;
        snap.backbuffer_format = shared_state_->backbuffer_format;
        const uint32_t depth =
            std::min<uint32_t>(snap.ring_depth, kRingDepth);
        for (uint32_t i = 0; i < depth; ++i) {
          snap.slot_handles[i] = shared_state_->slots[i].shared_handle;
        }
        const uint32_t seq_end = ReadSharedStateSequence();
        if (seq_begin == seq_end) {
          *out = snap;
          return true;
        }
      }
      ++shared_state_seq_retries_;
      YieldProcessor();
    }
    ++shared_state_seq_giveups_;
    return false;
  }

  void OpenTexturesForGeneration() {
    if (shared_state_ == nullptr || device_ == nullptr) {
      return;
    }
    SharedRingSnapshot snap;
    if (!SnapshotSharedRingState(&snap)) {
      // Producer mid-publish; retry on the next tick rather than open textures
      // from a torn ring state.
      return;
    }
    if (snap.generation == opened_generation_) {
      return;
    }
    ResetSharedTextures();
    opened_generation_ = snap.generation;
    const bool producer_keyed_mutex =
        snap.sync_kind == static_cast<uint32_t>(SyncKind::kKeyedMutex);
    const uint32_t depth = std::min<uint32_t>(snap.ring_depth, kRingDepth);
    uint32_t opened = 0;
    uint32_t keyed = 0;
    for (uint32_t i = 0; i < depth; ++i) {
      const uint64_t handle_value = snap.slot_handles[i];
      if (handle_value == 0) {
        continue;
      }
      HANDLE handle =
          reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle_value));
      const HRESULT hr =
          device_->OpenSharedResource(handle, IID_PPV_ARGS(&textures_[i]));
      if (SUCCEEDED(hr)) {
        ++opened;
        if (producer_keyed_mutex &&
            SUCCEEDED(textures_[i].As(&keyed_mutexes_[i])) &&
            keyed_mutexes_[i] != nullptr) {
          ++keyed;
        }
      }
    }
    // Only drive the keyed-mutex read path when the producer advertised it and
    // every opened slot exposed a keyed mutex. Otherwise fall back to the
    // seqlock-guarded event read; the producer's short-timeout acquire never
    // deadlocks against a consumer that does not hold the mutex.
    consumer_sync_is_keyed_mutex_ = producer_keyed_mutex && keyed == opened &&
                                    opened > 0;
    if (producer_keyed_mutex && !consumer_sync_is_keyed_mutex_ &&
        !keyed_mutex_query_warning_logged_) {
      keyed_mutex_query_warning_logged_ = true;
      LogSync("keyed_mutex_query_incomplete opened=" + std::to_string(opened) +
              " keyed=" + std::to_string(keyed) +
              " falling_back_to_event_read=true");
    }
    Log("shared_textures_opened generation=" +
        std::to_string(opened_generation_) +
        " count=" + std::to_string(opened) +
        " keyedMutex=" + std::to_string(keyed) +
        " syncKeyedMutex=" +
        std::string(consumer_sync_is_keyed_mutex_ ? "true" : "false") +
        " source=" + std::to_string(snap.backbuffer_width) + "x" +
        std::to_string(snap.backbuffer_height) +
        " format=" + std::to_string(snap.backbuffer_format));
  }

  bool EnsureStagingTexture(const D3D11_TEXTURE2D_DESC& desc) {
    if (staging_texture_ != nullptr && staging_width_ == desc.Width &&
        staging_height_ == desc.Height && staging_format_ == desc.Format) {
      return true;
    }
    staging_texture_.Reset();
    D3D11_TEXTURE2D_DESC staging = desc;
    staging.MipLevels = 1;
    staging.ArraySize = 1;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    staging.SampleDesc.Count = 1;
    staging.SampleDesc.Quality = 0;
    const HRESULT hr =
        device_->CreateTexture2D(&staging, nullptr, &staging_texture_);
    if (FAILED(hr)) {
      Log("staging_create_failed hr=" + HResultHex(hr));
      RecoverConsumerD3dDevice("cpu_staging.create", hr, true);
      return false;
    }
    staging_width_ = desc.Width;
    staging_height_ = desc.Height;
    staging_format_ = desc.Format;
    return true;
  }

  void ResetGpuScaleResources() {
    gpu_vertex_shader_.Reset();
    gpu_pixel_shader_.Reset();
    nv12_y_pixel_shader_.Reset();
    nv12_uv_pixel_shader_.Reset();
    gpu_scale_marker_constants_.Reset();
    gpu_sampler_state_.Reset();
    gpu_scaled_rtv_.Reset();
    gpu_scaled_srv_.Reset();
    gpu_scaled_texture_.Reset();
    ResetGpuReadbackSlots();
    ResetGpuNv12Resources();
    nv12_video_context_.Reset();
    nv12_video_device_.Reset();
    gpu_source_shader_views_.clear();
    gpu_input_width_ = 0;
    gpu_input_height_ = 0;
    gpu_output_width_ = 0;
    gpu_output_height_ = 0;
    gpu_input_format_ = DXGI_FORMAT_UNKNOWN;
  }

  void ResetGpuScaleOutputResources() {
    gpu_scaled_rtv_.Reset();
    gpu_scaled_srv_.Reset();
    gpu_scaled_texture_.Reset();
    ResetGpuReadbackSlots();
    ResetGpuNv12Resources();
    gpu_source_shader_views_.clear();
    gpu_input_width_ = 0;
    gpu_input_height_ = 0;
    gpu_output_width_ = 0;
    gpu_output_height_ = 0;
    gpu_input_format_ = DXGI_FORMAT_UNKNOWN;
  }

  void ResetGpuNv12Resources() {
    nv12_video_input_view_.Reset();
    nv12_video_processor_.Reset();
    nv12_video_processor_enumerator_.Reset();
    gpu_nv12_ready_fence_.Reset();
    gpu_nv12_fence_context_.Reset();
    gpu_nv12_ready_fence_value_ = 0;
    native_nv12_fence_available_ = false;
    if (native_nv12_ready_event_ != nullptr) {
      ResetEvent(native_nv12_ready_event_);
    }
    for (auto& slot : gpu_nv12_slots_) {
      slot.texture.Reset();
      slot.output_view.Reset();
      slot.y_plane_rtv.Reset();
      slot.uv_plane_rtv.Reset();
      slot.ready_query.Reset();
      ResetGpuNv12BltTimestampQueries(slot, true);
      slot.ready_fence.Reset();
      slot.ready_fence_value = 0;
      slot.direct_encoder_output = false;
      slot.pending = false;
      slot.sequence = 0;
      slot.attempt = 0;
      slot.source_frame_index = 0;
      slot.source_qpc = 0;
      slot.repeated = false;
      slot.source_width = 0;
      slot.source_height = 0;
      slot.output_width = 0;
      slot.output_height = 0;
      slot.source_format = DXGI_FORMAT_UNKNOWN;
      slot.start_qpc = 0;
      slot.after_gpu_qpc = 0;
      slot.after_blt_qpc = 0;
    }
    gpu_nv12_write_index_ = 0;
    nv12_render_convert_width_ = 0;
    nv12_render_convert_height_ = 0;
  }

  void ResetGpuReadbackSlots() {
    for (auto& slot : gpu_readback_slots_) {
      slot.texture.Reset();
      slot.pending = false;
      slot.sequence = 0;
      slot.source_frame_index = 0;
      slot.source_qpc = 0;
      slot.repeated = false;
    }
    gpu_readback_write_index_ = 0;
    gpu_readback_sequence_ = 0;
  }

  bool EnsureNativeNv12FenceResources() {
    if (native_nv12_ready_policy_ != NativeNv12ReadyPolicy::kFence) {
      return false;
    }
    if (gpu_nv12_ready_fence_ != nullptr &&
        gpu_nv12_fence_context_ != nullptr) {
      return true;
    }
    if (device_ == nullptr || context_ == nullptr) {
      return false;
    }

    ComPtr<ID3D11Device5> device5;
    HRESULT hr = device_.As(&device5);
    if (FAILED(hr) || device5 == nullptr) {
      if (!native_nv12_fence_unavailable_logged_) {
        native_nv12_fence_unavailable_logged_ = true;
        Log("gpu_nv12_fence_unavailable reason=device5 hr=" + HResultHex(hr));
      }
      return false;
    }

    ComPtr<ID3D11DeviceContext4> context4;
    hr = context_.As(&context4);
    if (FAILED(hr) || context4 == nullptr) {
      if (!native_nv12_fence_unavailable_logged_) {
        native_nv12_fence_unavailable_logged_ = true;
        Log("gpu_nv12_fence_unavailable reason=context4 hr=" + HResultHex(hr));
      }
      return false;
    }

    ComPtr<ID3D11Fence> fence;
    hr = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr) || fence == nullptr) {
      if (!native_nv12_fence_unavailable_logged_) {
        native_nv12_fence_unavailable_logged_ = true;
        Log("gpu_nv12_fence_unavailable reason=create_fence hr=" +
            HResultHex(hr));
      }
      RecoverConsumerD3dDevice("gpu_nv12.ready_fence_create", hr, true);
      return false;
    }

    gpu_nv12_ready_fence_ = fence;
    gpu_nv12_fence_context_ = context4;
    gpu_nv12_ready_fence_value_ = 0;
    native_nv12_fence_available_ = true;
    native_nv12_fence_unavailable_logged_ = false;
    Log("gpu_nv12_fence_ready policy=fence");
    return true;
  }

  bool EnsureNativeNv12ReadyEvent() {
    if (native_nv12_ready_event_ != nullptr) {
      return true;
    }
    native_nv12_ready_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (native_nv12_ready_event_ == nullptr) {
      Log("gpu_nv12_ready_event_create_failed error=" +
          std::to_string(GetLastError()));
      return false;
    }
    Log("gpu_nv12_ready_event_created policy=fence");
    return true;
  }

  bool SignalNativeNv12FenceValue(ComPtr<ID3D11Fence>* ready_fence,
                                  uint64_t* ready_fence_value, uint64_t attempt,
                                  const char* failure_reason,
                                  const std::string& stage) {
    if (ready_fence == nullptr || ready_fence_value == nullptr) {
      return false;
    }
    ready_fence->Reset();
    *ready_fence_value = 0;
    if (!EnsureNativeNv12FenceResources()) {
      return false;
    }

    const uint64_t fence_value = ++gpu_nv12_ready_fence_value_;
    const HRESULT hr = gpu_nv12_fence_context_->Signal(
        gpu_nv12_ready_fence_.Get(), fence_value);
    if (FAILED(hr)) {
      ++gpu_nv12_fence_signal_failures_;
      LogGpuNv12Failure(failure_reason, hr);
      RecoverConsumerD3dDevice("gpu_nv12.ready_fence_signal", hr, true);
      return false;
    }

    *ready_fence = gpu_nv12_ready_fence_;
    *ready_fence_value = fence_value;
    context_->Flush();
    ++gpu_nv12_fence_signaled_frames_;
    LogGpuNv12Stage(attempt, stage + " value=" + std::to_string(fence_value));
    return true;
  }

  bool IsGpuNv12FenceReady(GpuNv12Slot& slot, bool count_not_ready) {
    if (slot.ready_fence == nullptr || slot.ready_fence_value == 0) {
      return false;
    }
    const UINT64 completed_value = slot.ready_fence->GetCompletedValue();
    if (completed_value == static_cast<UINT64>(-1)) {
      LogGpuNv12Failure("native_ready_fence_removed",
                        DXGI_ERROR_DEVICE_REMOVED);
      RecoverConsumerD3dDevice("gpu_nv12.native_ready_fence",
                               DXGI_ERROR_DEVICE_REMOVED, true);
      slot.pending = false;
      ResetGpuNv12BltTimestampQueries(slot, true);
      return false;
    }
    if (completed_value >= slot.ready_fence_value) {
      ++gpu_nv12_fence_ready_frames_;
      return true;
    }
    if (count_not_ready) {
      ++gpu_nv12_not_ready_polls_;
    }
    return false;
  }

  bool RegisterNativeNv12FenceWake(GpuNv12Slot& slot) {
    if (slot.ready_fence == nullptr || slot.ready_fence_value == 0 ||
        !EnsureNativeNv12ReadyEvent()) {
      return false;
    }
    const HRESULT hr = slot.ready_fence->SetEventOnCompletion(
        slot.ready_fence_value, native_nv12_ready_event_);
    if (FAILED(hr)) {
      LogGpuNv12Stage(slot.attempt, "native_frame_ready_fence_wake_failed hr=" +
                                        HResultHex(hr));
      RecoverConsumerD3dDevice("gpu_nv12.native_ready_fence_wake", hr, true);
      return false;
    }
    LogGpuNv12Stage(slot.attempt, "native_frame_ready_pending_fence_event");
    return true;
  }

  const char* NativeNv12FrameOwnershipName() const {
    return native_nv12_ready_policy_ == NativeNv12ReadyPolicy::kFence
               ? "owned_texture_direct_blt"
               : "owned_texture_copy";
  }

  bool PrepareDirectEncoderGpuNv12Slot(GpuNv12Slot& slot, int output_width,
                                       int output_height, uint64_t attempt) {
    if (device_ == nullptr || nv12_video_device_ == nullptr ||
        nv12_video_processor_enumerator_ == nullptr || output_width <= 0 ||
        output_height <= 0) {
      return false;
    }

    D3D11_TEXTURE2D_DESC nv12_desc{};
    nv12_desc.Width = output_width;
    nv12_desc.Height = output_height;
    nv12_desc.MipLevels = 1;
    nv12_desc.ArraySize = 1;
    nv12_desc.Format = DXGI_FORMAT_NV12;
    nv12_desc.SampleDesc.Count = 1;
    nv12_desc.Usage = D3D11_USAGE_DEFAULT;
    nv12_desc.BindFlags = D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = device_->CreateTexture2D(&nv12_desc, nullptr, &texture);
    if (FAILED(hr) || texture == nullptr) {
      LogGpuNv12Failure("direct_native_texture_create_failed", hr);
      RecoverConsumerD3dDevice("gpu_nv12.direct_texture_create", hr, true);
      return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};
    output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    output_desc.Texture2D.MipSlice = 0;
    ComPtr<ID3D11VideoProcessorOutputView> output_view;
    hr = nv12_video_device_->CreateVideoProcessorOutputView(
        texture.Get(), nv12_video_processor_enumerator_.Get(), &output_desc,
        &output_view);
    if (FAILED(hr) || output_view == nullptr) {
      LogGpuNv12Failure("direct_video_output_view_create_failed", hr);
      RecoverConsumerD3dDevice("gpu_nv12.direct_output_view_create", hr, true);
      return false;
    }

    slot.texture = std::move(texture);
    slot.output_view = std::move(output_view);
    slot.direct_encoder_output = true;
    LogGpuNv12Stage(attempt, "direct_native_output_ready");
    return true;
  }

  bool CompleteDirectEncoderGpuNv12Slot(GpuNv12Slot& slot,
                                        ComPtr<ID3D11Texture2D>* owned_texture,
                                        ComPtr<ID3D11Fence>* owned_ready_fence,
                                        uint64_t* owned_ready_fence_value,
                                        int64_t* owned_ready_qpc) {
    if (context_ == nullptr || slot.texture == nullptr ||
        owned_texture == nullptr || owned_ready_fence == nullptr ||
        owned_ready_fence_value == nullptr || owned_ready_qpc == nullptr) {
      return false;
    }

    owned_texture->Reset();
    owned_ready_fence->Reset();
    *owned_ready_fence_value = 0;
    *owned_ready_qpc = 0;

    LARGE_INTEGER signal_start{};
    QueryPerformanceCounter(&signal_start);
    if (slot.ready_fence != nullptr && slot.ready_fence_value > 0) {
      *owned_ready_fence = slot.ready_fence;
      *owned_ready_fence_value = slot.ready_fence_value;
      LogGpuNv12Stage(slot.attempt,
                      "direct_native_ready_fence_forwarded value=" +
                          std::to_string(slot.ready_fence_value));
    } else if (native_nv12_ready_policy_ == NativeNv12ReadyPolicy::kFence &&
               EnsureNativeNv12FenceResources()) {
      if (!SignalNativeNv12FenceValue(owned_ready_fence,
                                      owned_ready_fence_value, slot.attempt,
                                      "direct_native_ready_fence_signal_failed",
                                      "direct_native_ready_fence_signaled")) {
        return false;
      }
    } else {
      context_->Flush();
      LogGpuNv12Stage(slot.attempt, "direct_native_output_flushed");
    }

    LARGE_INTEGER signal_done{};
    QueryPerformanceCounter(&signal_done);
    RecordQpcMetric(signal_done.QuadPart - signal_start.QuadPart,
                    &gpu_nv12_owned_copy_qpc_, &gpu_nv12_owned_copy_max_qpc_,
                    &gpu_nv12_owned_copy_samples_);
    ++gpu_nv12_owned_copy_frames_;
    *owned_texture = slot.texture;
    *owned_ready_qpc = signal_done.QuadPart;
    return true;
  }

  bool CopyReadyGpuNv12SlotToOwnedTexture(
      GpuNv12Slot& slot, ComPtr<ID3D11Texture2D>* owned_texture,
      ComPtr<ID3D11Fence>* owned_ready_fence, uint64_t* owned_ready_fence_value,
      int64_t* owned_copy_qpc) {
    if (device_ == nullptr || context_ == nullptr || slot.texture == nullptr ||
        owned_texture == nullptr || owned_ready_fence == nullptr ||
        owned_ready_fence_value == nullptr || owned_copy_qpc == nullptr) {
      return false;
    }

    owned_texture->Reset();
    owned_ready_fence->Reset();
    *owned_ready_fence_value = 0;
    *owned_copy_qpc = 0;

    D3D11_TEXTURE2D_DESC desc{};
    slot.texture->GetDesc(&desc);
    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(hr) || texture == nullptr) {
      LogGpuNv12Failure("owned_native_texture_create_failed", hr);
      RecoverConsumerD3dDevice("gpu_nv12.owned_texture_create", hr, true);
      return false;
    }

    LARGE_INTEGER copy_start{};
    QueryPerformanceCounter(&copy_start);
    context_->CopyResource(texture.Get(), slot.texture.Get());

    if (native_nv12_ready_policy_ == NativeNv12ReadyPolicy::kFence &&
        EnsureNativeNv12FenceResources()) {
      if (!SignalNativeNv12FenceValue(owned_ready_fence,
                                      owned_ready_fence_value, slot.attempt,
                                      "owned_native_ready_fence_signal_failed",
                                      "owned_native_ready_fence_signaled")) {
        return false;
      }
    } else {
      context_->Flush();
      LogGpuNv12Stage(slot.attempt, "owned_native_texture_copy_flushed");
    }

    LARGE_INTEGER copy_done{};
    QueryPerformanceCounter(&copy_done);
    RecordQpcMetric(copy_done.QuadPart - copy_start.QuadPart,
                    &gpu_nv12_owned_copy_qpc_, &gpu_nv12_owned_copy_max_qpc_,
                    &gpu_nv12_owned_copy_samples_);
    ++gpu_nv12_owned_copy_frames_;
    *owned_texture = std::move(texture);
    *owned_copy_qpc = copy_done.QuadPart;
    return true;
  }

  bool GpuReadbackSlotsReady(UINT width, UINT height,
                             DXGI_FORMAT format) const {
    for (const auto& slot : gpu_readback_slots_) {
      if (slot.texture == nullptr) {
        return false;
      }
      D3D11_TEXTURE2D_DESC desc{};
      slot.texture->GetDesc(&desc);
      if (desc.Width != width || desc.Height != height ||
          desc.Format != format) {
        return false;
      }
    }
    return true;
  }

  bool GpuNv12SlotsReady(UINT width, UINT height) const {
    for (const auto& slot : gpu_nv12_slots_) {
      if (slot.texture == nullptr || slot.output_view == nullptr) {
        return false;
      }
      D3D11_TEXTURE2D_DESC desc{};
      slot.texture->GetDesc(&desc);
      if (desc.Width != width || desc.Height != height ||
          desc.Format != DXGI_FORMAT_NV12) {
        return false;
      }
    }
    return true;
  }

  bool EnsureGpuScaleShaders() {
    if (device_ == nullptr) {
      return false;
    }
    if (gpu_vertex_shader_ != nullptr && gpu_pixel_shader_ != nullptr &&
        gpu_scale_marker_constants_ != nullptr &&
        gpu_sampler_state_ != nullptr) {
      return true;
    }
    if (!EnsureD3DCompiler()) {
      return false;
    }

    static constexpr char kShaderSource[] = R"(
struct VSOut {
  float4 pos : SV_POSITION;
  float2 uv : TEXCOORD0;
};

VSOut VSMain(uint vertexId : SV_VertexID) {
  float2 positions[3] = {
    float2(-1.0, -1.0),
    float2(-1.0,  3.0),
    float2( 3.0, -1.0)
  };
  VSOut output;
  output.pos = float4(positions[vertexId], 0.0, 1.0);
  output.uv = float2(
    0.5 * (output.pos.x + 1.0),
    0.5 * (1.0 - output.pos.y)
  );
  return output;
}

Texture2D<float4> sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);

cbuffer MarkerConstants : register(b0) {
  float outputWidth;
  float outputHeight;
  uint sourceFrameTrackingId;
  uint sourceFrameMarkerEnabled;
  uint sourceFrameQpcLow;
  uint sourceFrameQpcHigh;
  uint sourceFrameMarkerVersion;
  uint sourceFrameMarkerReserved;
};

float4 PSMain(VSOut input) : SV_TARGET {
  float4 color = sourceTexture.Sample(sourceSampler, input.uv);
  if (sourceFrameMarkerEnabled == 0 || sourceFrameTrackingId == 0) {
    return color;
  }

  float2 pixel = input.pos.xy;
  float markerWidth = max(96.0, outputWidth * 0.16);
  float markerHeight = max(48.0, outputHeight * 0.14);
  if (pixel.x >= markerWidth || pixel.y >= markerHeight) {
    return color;
  }

  float marginX = max(4.0, markerWidth * 0.10);
  float marginY = max(4.0, markerHeight * 0.18);
  float border = max(2.0, markerHeight * 0.04);
  if (pixel.x < border || pixel.y < border ||
      pixel.x >= markerWidth - border || pixel.y >= markerHeight - border) {
    return float4(0.0, 0.9, 1.0, 1.0);
  }

  float2 local = pixel - float2(marginX, marginY);
  float cellWidth = (markerWidth - (2.0 * marginX)) / 8.0;
  float cellHeight = (markerHeight - (2.0 * marginY)) / 8.0;
  if (local.x < 0.0 || local.y < 0.0 ||
      local.x >= cellWidth * 8.0 || local.y >= cellHeight * 8.0) {
    return float4(0.0, 0.0, 0.0, 1.0);
  }

  uint column = (uint)floor(local.x / cellWidth);
  uint row = (uint)floor(local.y / cellHeight);
  uint bitIndex = row * 8u + column;
  uint bit = 0u;
  if (bitIndex < 16u) {
    bit = (sourceFrameTrackingId >> bitIndex) & 1u;
  } else {
    uint qpcBitIndex = bitIndex - 16u;
    bit = qpcBitIndex < 32u
        ? ((sourceFrameQpcLow >> qpcBitIndex) & 1u)
        : ((sourceFrameQpcHigh >> (qpcBitIndex - 32u)) & 1u);
  }
  float value = bit == 0u ? 0.04 : 1.0;
  return float4(value, value, value, 1.0);
}
)";

    ComPtr<ID3DBlob> vertex_blob;
    ComPtr<ID3DBlob> pixel_blob;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = d3d_compile_(kShaderSource, std::strlen(kShaderSource),
                              "game_capture_webrtc_scale", nullptr, nullptr,
                              "VSMain", "vs_4_0", 0, 0, &vertex_blob, &errors);
    if (FAILED(hr)) {
      LogGpuScaleFailure("vertex_shader_compile_failed", hr);
      return false;
    }
    errors.Reset();
    hr = d3d_compile_(kShaderSource, std::strlen(kShaderSource),
                      "game_capture_webrtc_scale", nullptr, nullptr, "PSMain",
                      "ps_4_0", 0, 0, &pixel_blob, &errors);
    if (FAILED(hr)) {
      LogGpuScaleFailure("pixel_shader_compile_failed", hr);
      return false;
    }

    hr = device_->CreateVertexShader(vertex_blob->GetBufferPointer(),
                                     vertex_blob->GetBufferSize(), nullptr,
                                     &gpu_vertex_shader_);
    if (FAILED(hr)) {
      LogGpuScaleFailure("vertex_shader_create_failed", hr);
      return false;
    }
    hr = device_->CreatePixelShader(pixel_blob->GetBufferPointer(),
                                    pixel_blob->GetBufferSize(), nullptr,
                                    &gpu_pixel_shader_);
    if (FAILED(hr)) {
      LogGpuScaleFailure("pixel_shader_create_failed", hr);
      return false;
    }

    D3D11_BUFFER_DESC constants_desc{};
    constants_desc.ByteWidth = sizeof(GpuScaleMarkerConstants);
    constants_desc.Usage = D3D11_USAGE_DYNAMIC;
    constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constants_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateBuffer(&constants_desc, nullptr,
                               &gpu_scale_marker_constants_);
    if (FAILED(hr)) {
      LogGpuScaleFailure("marker_constants_create_failed", hr);
      return false;
    }

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device_->CreateSamplerState(&sampler, &gpu_sampler_state_);
    if (FAILED(hr)) {
      LogGpuScaleFailure("sampler_create_failed", hr);
      return false;
    }
    return true;
  }

  bool SetGpuScaleMarkerConstants(int output_width, int output_height,
                                  uint64_t source_frame_index,
                                  uint64_t source_qpc) {
    if (context_ == nullptr || gpu_scale_marker_constants_ == nullptr) {
      return false;
    }

    GpuScaleMarkerConstants constants;
    constants.output_width = static_cast<float>(std::max(1, output_width));
    constants.output_height = static_cast<float>(std::max(1, output_height));
    constants.source_frame_tracking_id =
        SourceFrameTrackingId(source_frame_index);
    constants.enabled = source_frame_visual_marker_enabled_ ? 1u : 0u;
    constants.source_frame_qpc_low =
        static_cast<uint32_t>(source_qpc & 0xffffffffULL);
    constants.source_frame_qpc_high =
        static_cast<uint32_t>((source_qpc >> 32) & 0xffffULL);
    constants.marker_version = 2u;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = context_->Map(gpu_scale_marker_constants_.Get(), 0,
                               D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
      LogGpuScaleFailure("marker_constants_map_failed", hr);
      return false;
    }
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context_->Unmap(gpu_scale_marker_constants_.Get(), 0);
    ID3D11Buffer* constants_buffer[] = {gpu_scale_marker_constants_.Get()};
    context_->PSSetConstantBuffers(0, 1, constants_buffer);
    return true;
  }

  bool EnsureD3DCompiler() {
    if (d3d_compile_ != nullptr) {
      return true;
    }

    static constexpr const wchar_t* kCompilerNames[] = {
        L"d3dcompiler_47.dll",
        L"d3dcompiler_46.dll",
        L"d3dcompiler_43.dll",
    };
    for (const wchar_t* name : kCompilerNames) {
      HMODULE module = LoadLibraryW(name);
      if (module == nullptr) {
        continue;
      }
      FARPROC proc = GetProcAddress(module, "D3DCompile");
      if (proc == nullptr) {
        FreeLibrary(module);
        continue;
      }
      d3dcompiler_module_ = module;
      d3d_compile_ = reinterpret_cast<D3DCompileProc>(proc);
      return true;
    }

    LogGpuScaleFailure("d3dcompiler_unavailable",
                       HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }

  bool EnsureGpuScaledBgraResources(const D3D11_TEXTURE2D_DESC& source_desc,
                                    int output_width, int output_height) {
    if (device_ == nullptr || context_ == nullptr || output_width <= 0 ||
        output_height <= 0) {
      return false;
    }
    if (!EnsureGpuScaleShaders()) {
      return false;
    }

    const bool matching =
        gpu_scaled_rtv_ != nullptr && gpu_scaled_texture_ != nullptr &&
        GpuReadbackSlotsReady(static_cast<UINT>(output_width),
                              static_cast<UINT>(output_height),
                              DXGI_FORMAT_B8G8R8A8_UNORM) &&
        gpu_input_width_ == source_desc.Width &&
        gpu_input_height_ == source_desc.Height &&
        gpu_output_width_ == static_cast<uint32_t>(output_width) &&
        gpu_output_height_ == static_cast<uint32_t>(output_height) &&
        gpu_input_format_ == source_desc.Format;
    if (matching) {
      return true;
    }

    ResetGpuScaleOutputResources();

    D3D11_TEXTURE2D_DESC output_desc{};
    output_desc.Width = output_width;
    output_desc.Height = output_height;
    output_desc.MipLevels = 1;
    output_desc.ArraySize = 1;
    output_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    output_desc.SampleDesc.Count = 1;
    output_desc.Usage = D3D11_USAGE_DEFAULT;
    output_desc.BindFlags =
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr =
        device_->CreateTexture2D(&output_desc, nullptr, &gpu_scaled_texture_);
    if (FAILED(hr)) {
      LogGpuScaleFailure("output_texture_create_failed", hr);
      RecoverConsumerD3dDevice("gpu_scale.output_texture_create", hr, true);
      return false;
    }

    hr = device_->CreateRenderTargetView(gpu_scaled_texture_.Get(), nullptr,
                                         &gpu_scaled_rtv_);
    if (FAILED(hr)) {
      LogGpuScaleFailure("output_rtv_create_failed", hr);
      RecoverConsumerD3dDevice("gpu_scale.output_rtv_create", hr, true);
      return false;
    }

    D3D11_TEXTURE2D_DESC staging_desc = output_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& slot : gpu_readback_slots_) {
      hr = device_->CreateTexture2D(&staging_desc, nullptr, &slot.texture);
      if (FAILED(hr)) {
        LogGpuScaleFailure("staging_texture_create_failed", hr);
        RecoverConsumerD3dDevice("gpu_scale.staging_texture_create", hr, true);
        return false;
      }
      slot.pending = false;
      slot.sequence = 0;
    }
    gpu_readback_write_index_ = 0;
    gpu_readback_sequence_ = 0;

    gpu_input_width_ = source_desc.Width;
    gpu_input_height_ = source_desc.Height;
    gpu_output_width_ = output_width;
    gpu_output_height_ = output_height;
    gpu_input_format_ = source_desc.Format;
    Log("gpu_scale_resources_ready source=" +
        std::to_string(source_desc.Width) + "x" +
        std::to_string(source_desc.Height) + " output=" +
        std::to_string(output_width) + "x" + std::to_string(output_height) +
        " inputFormat=" + std::to_string(source_desc.Format) +
        " outputFormat=" + std::to_string(DXGI_FORMAT_B8G8R8A8_UNORM));
    return true;
  }

  // Phase 2 experimental (default off): compile the two pixel shaders that
  // convert the scaled BGRA texture into NV12 plane render targets, replacing
  // VideoProcessorBlt. BT.709 limited-range RGB->YCbCr; the build machine must
  // confirm the matrix/range match the receiver's decode assumption.
  bool EnsureGpuNv12RenderConvertShaders() {
    if (device_ == nullptr) {
      return false;
    }
    if (nv12_y_pixel_shader_ != nullptr && nv12_uv_pixel_shader_ != nullptr) {
      return true;
    }
    if (!EnsureD3DCompiler()) {
      return false;
    }
    static constexpr char kShaderSource[] = R"(
struct VSOut {
  float4 pos : SV_POSITION;
  float2 uv : TEXCOORD0;
};

Texture2D<float4> scaledTexture : register(t0);

// BT.709 limited-range luma from gamma-encoded RGB in [0,1].
float RgbToYLimited(float3 rgb) {
  float y = dot(rgb, float3(0.2126, 0.7152, 0.0722));
  return (16.0 + 219.0 * y) / 255.0;
}

float2 RgbToUVLimited(float3 rgb) {
  float y = dot(rgb, float3(0.2126, 0.7152, 0.0722));
  float cb = (rgb.b - y) / 1.8556;
  float cr = (rgb.r - y) / 1.5748;
  float u = (128.0 + 224.0 * cb) / 255.0;
  float v = (128.0 + 224.0 * cr) / 255.0;
  return float2(u, v);
}

// Y plane: one output texel per source pixel (1:1).
float PSMainY(VSOut input) : SV_Target {
  int2 p = int2(input.pos.xy);
  float3 rgb = scaledTexture.Load(int3(p, 0)).rgb;
  return RgbToYLimited(rgb);
}

// UV plane: half resolution, box-average the 2x2 source block (4:2:0).
float2 PSMainUV(VSOut input) : SV_Target {
  int2 block = int2(input.pos.xy) * 2;
  float3 c0 = scaledTexture.Load(int3(block + int2(0, 0), 0)).rgb;
  float3 c1 = scaledTexture.Load(int3(block + int2(1, 0), 0)).rgb;
  float3 c2 = scaledTexture.Load(int3(block + int2(0, 1), 0)).rgb;
  float3 c3 = scaledTexture.Load(int3(block + int2(1, 1), 0)).rgb;
  return RgbToUVLimited((c0 + c1 + c2 + c3) * 0.25);
}
)";

    ComPtr<ID3DBlob> y_blob;
    ComPtr<ID3DBlob> uv_blob;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = d3d_compile_(kShaderSource, std::strlen(kShaderSource),
                              "game_capture_nv12_render_convert", nullptr,
                              nullptr, "PSMainY", "ps_4_0", 0, 0, &y_blob,
                              &errors);
    if (FAILED(hr)) {
      LogGpuNv12Failure("render_convert_y_shader_compile_failed", hr);
      return false;
    }
    errors.Reset();
    hr = d3d_compile_(kShaderSource, std::strlen(kShaderSource),
                      "game_capture_nv12_render_convert", nullptr, nullptr,
                      "PSMainUV", "ps_4_0", 0, 0, &uv_blob, &errors);
    if (FAILED(hr)) {
      LogGpuNv12Failure("render_convert_uv_shader_compile_failed", hr);
      return false;
    }
    hr = device_->CreatePixelShader(y_blob->GetBufferPointer(),
                                    y_blob->GetBufferSize(), nullptr,
                                    &nv12_y_pixel_shader_);
    if (FAILED(hr)) {
      LogGpuNv12Failure("render_convert_y_shader_create_failed", hr);
      return false;
    }
    hr = device_->CreatePixelShader(uv_blob->GetBufferPointer(),
                                    uv_blob->GetBufferSize(), nullptr,
                                    &nv12_uv_pixel_shader_);
    if (FAILED(hr)) {
      LogGpuNv12Failure("render_convert_uv_shader_create_failed", hr);
      return false;
    }
    return true;
  }

  bool GpuNv12RenderConvertSlotsReady(UINT width, UINT height) {
    if (nv12_render_convert_width_ != width ||
        nv12_render_convert_height_ != height) {
      return false;
    }
    for (const auto& slot : gpu_nv12_slots_) {
      if (slot.texture == nullptr || slot.y_plane_rtv == nullptr ||
          slot.uv_plane_rtv == nullptr || slot.ready_query == nullptr) {
        return false;
      }
    }
    return true;
  }

  bool EnsureGpuNv12RenderConvertResources(int output_width,
                                           int output_height) {
    if (device_ == nullptr || context_ == nullptr ||
        gpu_scaled_texture_ == nullptr || output_width <= 0 ||
        output_height <= 0) {
      return false;
    }
    if (!EnsureGpuNv12RenderConvertShaders()) {
      return false;
    }
    if (gpu_scaled_srv_ == nullptr) {
      const HRESULT hr = device_->CreateShaderResourceView(
          gpu_scaled_texture_.Get(), nullptr, &gpu_scaled_srv_);
      if (FAILED(hr)) {
        LogGpuNv12Failure("render_convert_scaled_srv_create_failed", hr);
        RecoverConsumerD3dDevice("gpu_nv12.render_convert_srv", hr, true);
        return false;
      }
    }
    if (GpuNv12RenderConvertSlotsReady(static_cast<UINT>(output_width),
                                       static_cast<UINT>(output_height))) {
      return true;
    }

    ResetGpuNv12Resources();
    D3D11_TEXTURE2D_DESC nv12_desc{};
    nv12_desc.Width = output_width;
    nv12_desc.Height = output_height;
    nv12_desc.MipLevels = 1;
    nv12_desc.ArraySize = 1;
    nv12_desc.Format = DXGI_FORMAT_NV12;
    nv12_desc.SampleDesc.Count = 1;
    nv12_desc.Usage = D3D11_USAGE_DEFAULT;
    nv12_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    for (auto& slot : gpu_nv12_slots_) {
      HRESULT hr = device_->CreateTexture2D(&nv12_desc, nullptr, &slot.texture);
      if (FAILED(hr)) {
        LogGpuNv12Failure("render_convert_nv12_texture_create_failed", hr);
        RecoverConsumerD3dDevice("gpu_nv12.render_convert_texture", hr, true);
        return false;
      }
      D3D11_RENDER_TARGET_VIEW_DESC y_desc{};
      y_desc.Format = DXGI_FORMAT_R8_UNORM;
      y_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
      y_desc.Texture2D.MipSlice = 0;
      hr = device_->CreateRenderTargetView(slot.texture.Get(), &y_desc,
                                           &slot.y_plane_rtv);
      if (FAILED(hr)) {
        LogGpuNv12Failure("render_convert_y_rtv_create_failed", hr);
        RecoverConsumerD3dDevice("gpu_nv12.render_convert_y_rtv", hr, true);
        return false;
      }
      D3D11_RENDER_TARGET_VIEW_DESC uv_desc{};
      uv_desc.Format = DXGI_FORMAT_R8G8_UNORM;
      uv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
      uv_desc.Texture2D.MipSlice = 0;
      hr = device_->CreateRenderTargetView(slot.texture.Get(), &uv_desc,
                                           &slot.uv_plane_rtv);
      if (FAILED(hr)) {
        LogGpuNv12Failure("render_convert_uv_rtv_create_failed", hr);
        RecoverConsumerD3dDevice("gpu_nv12.render_convert_uv_rtv", hr, true);
        return false;
      }
      D3D11_QUERY_DESC query_desc{};
      query_desc.Query = D3D11_QUERY_EVENT;
      hr = device_->CreateQuery(&query_desc, &slot.ready_query);
      if (FAILED(hr)) {
        LogGpuNv12Failure("render_convert_ready_query_create_failed", hr);
        return false;
      }
    }
    gpu_nv12_write_index_ = 0;
    nv12_render_convert_width_ = static_cast<uint32_t>(output_width);
    nv12_render_convert_height_ = static_cast<uint32_t>(output_height);
    Log("gpu_nv12_render_convert_resources_ready output=" +
        std::to_string(output_width) + "x" + std::to_string(output_height));
    return true;
  }

  // Two full-screen passes write the Y and UV planes of the slot's NV12 texture
  // from the scaled BGRA texture, replacing VideoProcessorBlt. Slot readiness,
  // delivery, and timing reuse the existing event-query machinery.
  bool SubmitGpuNv12RenderConvert(const D3D11_TEXTURE2D_DESC& source_desc,
                                  int output_width, int output_height,
                                  uint64_t source_frame_index,
                                  uint64_t source_qpc, bool repeated,
                                  const LARGE_INTEGER& start,
                                  const LARGE_INTEGER& after_gpu,
                                  uint64_t attempt) {
    if (!EnsureGpuNv12RenderConvertResources(output_width, output_height)) {
      if (!native_nv12_render_convert_failed_logged_) {
        native_nv12_render_convert_failed_logged_ = true;
        Log("native_nv12_render_convert_resources_failed falling_back=true");
      }
      return false;
    }
    if (native_nv12_max_pending_slots_ < kGpuNv12MaxPendingSlots) {
      TrimPendingGpuNv12Slots(native_nv12_max_pending_slots_ - 1, attempt);
    }
    GpuNv12Slot* slot = NextGpuNv12WriteSlot();
    if (slot == nullptr || slot->y_plane_rtv == nullptr ||
        slot->uv_plane_rtv == nullptr || slot->ready_query == nullptr) {
      LogGpuNv12Failure("render_convert_slot_unavailable", E_FAIL);
      return false;
    }
    slot->direct_encoder_output = false;

    ID3D11ShaderResourceView* srvs[] = {gpu_scaled_srv_.Get()};
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(gpu_vertex_shader_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 1, srvs);

    D3D11_VIEWPORT y_viewport{};
    y_viewport.Width = static_cast<float>(output_width);
    y_viewport.Height = static_cast<float>(output_height);
    y_viewport.MaxDepth = 1.0f;
    ID3D11RenderTargetView* y_rtv[] = {slot->y_plane_rtv.Get()};
    context_->RSSetViewports(1, &y_viewport);
    context_->OMSetRenderTargets(1, y_rtv, nullptr);
    context_->PSSetShader(nv12_y_pixel_shader_.Get(), nullptr, 0);
    context_->Draw(3, 0);

    D3D11_VIEWPORT uv_viewport{};
    uv_viewport.Width = static_cast<float>(output_width / 2);
    uv_viewport.Height = static_cast<float>(output_height / 2);
    uv_viewport.MaxDepth = 1.0f;
    ID3D11RenderTargetView* uv_rtv[] = {slot->uv_plane_rtv.Get()};
    context_->RSSetViewports(1, &uv_viewport);
    context_->OMSetRenderTargets(1, uv_rtv, nullptr);
    context_->PSSetShader(nv12_uv_pixel_shader_.Get(), nullptr, 0);
    context_->Draw(3, 0);

    ID3D11ShaderResourceView* null_srv[] = {nullptr};
    context_->PSSetShaderResources(0, 1, null_srv);
    ID3D11RenderTargetView* null_rtv[] = {nullptr};
    context_->OMSetRenderTargets(1, null_rtv, nullptr);

    LARGE_INTEGER after_nv12{};
    QueryPerformanceCounter(&after_nv12);
    LogGpuNv12Stage(attempt, "render_convert_planes_drawn");

    slot->pending = true;
    slot->sequence = ++gpu_nv12_sequence_;
    slot->attempt = attempt;
    slot->source_frame_index = source_frame_index;
    slot->source_qpc = source_qpc;
    slot->repeated = repeated;
    slot->source_width = static_cast<int>(source_desc.Width);
    slot->source_height = static_cast<int>(source_desc.Height);
    slot->output_width = output_width;
    slot->output_height = output_height;
    slot->source_format = source_desc.Format;
    slot->start_qpc = start.QuadPart;
    slot->after_gpu_qpc = after_gpu.QuadPart;
    slot->after_blt_qpc = after_nv12.QuadPart;
    slot->ready_fence.Reset();
    slot->ready_fence_value = 0;
    ++gpu_nv12_queued_frames_;
    ++nv12_render_convert_frames_;
    gpu_scale_us_ += after_gpu.QuadPart - start.QuadPart;
    gpu_nv12_write_index_ = (gpu_nv12_write_index_ + 1) % kGpuNv12RingDepth;
    context_->End(slot->ready_query.Get());
    LogGpuNv12Stage(attempt, "native_frame_ready_pending_render_convert");
    DrainReadyGpuNv12Slots(GpuNv12ReadyObservation::kPostFenceRegistration);
    return true;
  }

  bool EnsureGpuNv12Resources(int output_width, int output_height) {
    if (device_ == nullptr || context_ == nullptr ||
        gpu_scaled_texture_ == nullptr || output_width <= 0 ||
        output_height <= 0) {
      return false;
    }

    if (nv12_video_device_ == nullptr) {
      HRESULT hr = device_->QueryInterface(IID_PPV_ARGS(&nv12_video_device_));
      if (FAILED(hr)) {
        LogGpuNv12Failure("video_device_unavailable", hr);
        RecoverConsumerD3dDevice("gpu_nv12.video_device", hr, true);
        return false;
      }
    }
    if (nv12_video_context_ == nullptr) {
      HRESULT hr = context_->QueryInterface(IID_PPV_ARGS(&nv12_video_context_));
      if (FAILED(hr)) {
        LogGpuNv12Failure("video_context_unavailable", hr);
        RecoverConsumerD3dDevice("gpu_nv12.video_context", hr, true);
        return false;
      }
    }

    const bool matching = nv12_video_processor_ != nullptr &&
                          nv12_video_processor_enumerator_ != nullptr &&
                          nv12_video_input_view_ != nullptr &&
                          GpuNv12SlotsReady(static_cast<UINT>(output_width),
                                            static_cast<UINT>(output_height));
    if (matching) {
      return true;
    }

    nv12_video_input_view_.Reset();
    nv12_video_processor_.Reset();
    nv12_video_processor_enumerator_.Reset();
    for (auto& slot : gpu_nv12_slots_) {
      slot.texture.Reset();
      slot.output_view.Reset();
      slot.ready_query.Reset();
      ResetGpuNv12BltTimestampQueries(slot, true);
      slot.ready_fence.Reset();
      slot.ready_fence_value = 0;
      slot.pending = false;
      slot.sequence = 0;
    }
    gpu_nv12_write_index_ = 0;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content_desc{};
    content_desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content_desc.InputWidth = output_width;
    content_desc.InputHeight = output_height;
    content_desc.OutputWidth = output_width;
    content_desc.OutputHeight = output_height;
    content_desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    HRESULT hr = nv12_video_device_->CreateVideoProcessorEnumerator(
        &content_desc, &nv12_video_processor_enumerator_);
    if (FAILED(hr)) {
      LogGpuNv12Failure("video_processor_enum_failed", hr);
      RecoverConsumerD3dDevice("gpu_nv12.video_processor_enum", hr, true);
      return false;
    }
    hr = nv12_video_device_->CreateVideoProcessor(
        nv12_video_processor_enumerator_.Get(), 0, &nv12_video_processor_);
    if (FAILED(hr)) {
      LogGpuNv12Failure("video_processor_create_failed", hr);
      RecoverConsumerD3dDevice("gpu_nv12.video_processor_create", hr, true);
      return false;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
    input_desc.FourCC = 0;
    input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    input_desc.Texture2D.MipSlice = 0;
    input_desc.Texture2D.ArraySlice = 0;
    hr = nv12_video_device_->CreateVideoProcessorInputView(
        gpu_scaled_texture_.Get(), nv12_video_processor_enumerator_.Get(),
        &input_desc, &nv12_video_input_view_);
    if (FAILED(hr)) {
      LogGpuNv12Failure("video_input_view_create_failed", hr);
      RecoverConsumerD3dDevice("gpu_nv12.video_input_view_create", hr, true);
      return false;
    }

    D3D11_TEXTURE2D_DESC nv12_desc{};
    nv12_desc.Width = output_width;
    nv12_desc.Height = output_height;
    nv12_desc.MipLevels = 1;
    nv12_desc.ArraySize = 1;
    nv12_desc.Format = DXGI_FORMAT_NV12;
    nv12_desc.SampleDesc.Count = 1;
    nv12_desc.Usage = D3D11_USAGE_DEFAULT;
    nv12_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    for (auto& slot : gpu_nv12_slots_) {
      hr = device_->CreateTexture2D(&nv12_desc, nullptr, &slot.texture);
      if (FAILED(hr)) {
        LogGpuNv12Failure("nv12_texture_create_failed", hr);
        RecoverConsumerD3dDevice("gpu_nv12.nv12_texture_create", hr, true);
        return false;
      }
      D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};
      output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
      output_desc.Texture2D.MipSlice = 0;
      hr = nv12_video_device_->CreateVideoProcessorOutputView(
          slot.texture.Get(), nv12_video_processor_enumerator_.Get(),
          &output_desc, &slot.output_view);
      if (FAILED(hr)) {
        LogGpuNv12Failure("video_output_view_create_failed", hr);
        RecoverConsumerD3dDevice("gpu_nv12.video_output_view_create", hr, true);
        return false;
      }
      D3D11_QUERY_DESC query_desc{};
      query_desc.Query = D3D11_QUERY_EVENT;
      hr = device_->CreateQuery(&query_desc, &slot.ready_query);
      if (FAILED(hr)) {
        LogGpuNv12Failure("ready_query_create_failed", hr);
        RecoverConsumerD3dDevice("gpu_nv12.ready_query_create", hr, true);
        return false;
      }
    }
    const bool fence_available = EnsureNativeNv12FenceResources();

    Log("gpu_nv12_resources_ready output=" + std::to_string(output_width) +
        "x" + std::to_string(output_height) +
        " scratchRing=" + std::to_string(kGpuNv12RingDepth) +
        " frameOwnership=" + NativeNv12FrameOwnershipName() +
        " readyPolicy=" + NativeNv12ReadyPolicyName(native_nv12_ready_policy_) +
        " fenceAvailable=" + std::string(fence_available ? "true" : "false"));
    return true;
  }

  bool IsGpuNv12SlotReady(GpuNv12Slot& slot) {
    if (!slot.pending) {
      return false;
    }
    if (slot.ready_fence != nullptr && slot.ready_fence_value > 0) {
      return IsGpuNv12FenceReady(slot, true);
    }
    if (slot.ready_query == nullptr) {
      return false;
    }
    const HRESULT hr = context_->GetData(slot.ready_query.Get(), nullptr, 0,
                                         D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (hr == S_FALSE) {
      ++gpu_nv12_not_ready_polls_;
      return false;
    }
    if (FAILED(hr)) {
      LogGpuNv12Failure("native_frame_ready_query_failed", hr);
      RecoverConsumerD3dDevice("gpu_nv12.native_frame_ready_query", hr, true);
      slot.pending = false;
      ResetGpuNv12BltTimestampQueries(slot, true);
      return false;
    }
    return true;
  }

  bool QueueReadyGpuNv12Slot(GpuNv12Slot& slot,
                             GpuNv12ReadyObservation observation) {
    if (!slot.pending || slot.texture == nullptr) {
      return false;
    }
    LARGE_INTEGER ready_now{};
    QueryPerformanceCounter(&ready_now);
    RecordGpuNv12ReadyObservation(observation);
    if (slot.start_qpc > 0 && slot.after_gpu_qpc > slot.start_qpc) {
      RecordQpcMetric(slot.after_gpu_qpc - slot.start_qpc,
                      &gpu_nv12_bgra_scale_draw_qpc_,
                      &gpu_nv12_bgra_scale_draw_max_qpc_,
                      &gpu_nv12_bgra_scale_draw_samples_);
    }
    if (slot.after_gpu_qpc > 0 && slot.after_blt_qpc > slot.after_gpu_qpc) {
      RecordQpcMetric(slot.after_blt_qpc - slot.after_gpu_qpc,
                      &gpu_nv12_blt_submit_qpc_, &gpu_nv12_blt_submit_max_qpc_,
                      &gpu_nv12_blt_submit_samples_);
    }
    int64_t blt_to_ready_qpc = 0;
    if (slot.after_blt_qpc > 0 && ready_now.QuadPart > slot.after_blt_qpc) {
      blt_to_ready_qpc = ready_now.QuadPart - slot.after_blt_qpc;
      RecordQpcMetric(blt_to_ready_qpc, &gpu_nv12_blt_to_ready_qpc_,
                      &gpu_nv12_blt_to_ready_max_qpc_,
                      &gpu_nv12_blt_to_ready_samples_);
      RecordGpuNv12BltToReadyBudget(blt_to_ready_qpc);
      MaybeUpdateNativeNv12GpuQueueBackoff(blt_to_ready_qpc, ready_now.QuadPart,
                                           slot.attempt, observation);
    }
    TryRecordGpuNv12BltTimestampMetrics(slot, blt_to_ready_qpc);
    if (NativeNv12ReadyExceedsLateDropBudget(blt_to_ready_qpc)) {
      RecordNativeNv12LateReadyDrop(slot, ready_now.QuadPart, blt_to_ready_qpc);
      slot.pending = false;
      ResetGpuNv12BltTimestampQueries(slot, false);
      slot.ready_fence.Reset();
      slot.ready_fence_value = 0;
      ++gpu_nv12_late_ready_dropped_frames_;
      LogGpuNv12Stage(
          slot.attempt,
          "late_ready_dropped observedBy=" +
              std::string(GpuNv12ReadyObservationName(observation)) +
              " bltToReadyMs=" + std::to_string(TicksToMs(blt_to_ready_qpc)) +
              " thresholdMs=" +
              std::to_string(native_nv12_late_ready_drop_threshold_ms_) +
              " dropped=" +
              std::to_string(gpu_nv12_late_ready_dropped_frames_));
      return true;
    }
    if (native_nv12_drop_late_ready_enabled_ &&
        SourceAgeExceedsNativeNv12Budget(slot.source_qpc, ready_now.QuadPart)) {
      RecordNativeNv12ReadyDrop(slot, ready_now.QuadPart);
      slot.pending = false;
      ResetGpuNv12BltTimestampQueries(slot, false);
      slot.ready_fence.Reset();
      slot.ready_fence_value = 0;
      ++gpu_nv12_ready_dropped_frames_;
      LogGpuNv12Stage(
          slot.attempt,
          "source_age_stale_ready_dropped observedBy=" +
              std::string(GpuNv12ReadyObservationName(observation)) +
              " sourceAgeMs=" +
              SourceAgeMsLabel(slot.source_qpc, ready_now.QuadPart) +
              " dropped=" + std::to_string(gpu_nv12_ready_dropped_frames_));
      return true;
    }
    ComPtr<ID3D11Texture2D> owned_texture;
    ComPtr<ID3D11Fence> owned_ready_fence;
    uint64_t owned_ready_fence_value = 0;
    int64_t owned_copy_qpc = 0;
    const bool handoff_ready =
        slot.direct_encoder_output
            ? CompleteDirectEncoderGpuNv12Slot(
                  slot, &owned_texture, &owned_ready_fence,
                  &owned_ready_fence_value, &owned_copy_qpc)
            : CopyReadyGpuNv12SlotToOwnedTexture(
                  slot, &owned_texture, &owned_ready_fence,
                  &owned_ready_fence_value, &owned_copy_qpc);
    if (!handoff_ready) {
      slot.pending = false;
      ResetGpuNv12BltTimestampQueries(slot, false);
      slot.ready_fence.Reset();
      slot.ready_fence_value = 0;
      return false;
    }

    auto native_buffer = owt::base::IntergalacticD3D11Nv12Buffer::Create(
        device_, owned_texture, slot.output_width, slot.output_height,
        owned_ready_fence, owned_ready_fence_value,
        BuildNativeNv12Metadata(GameCaptureSourceModeName(source_mode_),
                                slot.source_format, slot.source_frame_index,
                                slot.source_qpc, owned_copy_qpc));
    LARGE_INTEGER after_buffer_create{};
    QueryPerformanceCounter(&after_buffer_create);
    if (native_buffer == nullptr) {
      LogGpuNv12Failure("native_buffer_create_failed", E_FAIL);
      slot.pending = false;
      ResetGpuNv12BltTimestampQueries(slot, false);
      return false;
    }
    RecordQpcMetric(after_buffer_create.QuadPart - owned_copy_qpc,
                    &gpu_nv12_buffer_create_qpc_,
                    &gpu_nv12_buffer_create_max_qpc_,
                    &gpu_nv12_buffer_create_samples_);

    if (slot.after_gpu_qpc > 0 && ready_now.QuadPart > slot.after_gpu_qpc) {
      RecordQpcMetric(ready_now.QuadPart - slot.after_gpu_qpc,
                      &gpu_nv12_convert_qpc_, &gpu_nv12_convert_max_qpc_,
                      &gpu_nv12_convert_samples_);
    }

    PendingVideoFrame frame;
    frame.buffer = native_buffer;
    frame.source_width = slot.source_width;
    frame.source_height = slot.source_height;
    frame.output_width = slot.output_width;
    frame.output_height = slot.output_height;
    frame.source_format = slot.source_format;
    frame.source_frame_index = slot.source_frame_index;
    frame.source_qpc = slot.source_qpc;
    frame.ready_qpc = after_buffer_create.QuadPart;
    frame.repeated = slot.repeated;
    frame.gpu_scaled = true;
    frame.native_nv12 = true;
    slot.pending = false;
    ResetGpuNv12BltTimestampQueries(slot, false);
    slot.ready_fence.Reset();
    slot.ready_fence_value = 0;
    ++gpu_nv12_ready_frames_;
    const int64_t queued_qpc = QueueFrameForDelivery(std::move(frame));
    if (queued_qpc > owned_copy_qpc) {
      RecordQpcMetric(queued_qpc - owned_copy_qpc,
                      &gpu_nv12_frame_ready_to_queue_qpc_,
                      &gpu_nv12_frame_ready_to_queue_max_qpc_,
                      &gpu_nv12_frame_ready_to_queue_samples_);
    }
    LogGpuNv12Stage(slot.attempt,
                    "newest_ready_queued observedBy=" +
                        std::string(GpuNv12ReadyObservationName(observation)));
    return true;
  }

  void DrainReadyGpuNv12Slots(GpuNv12ReadyObservation observation) {
    if (gpu_nv12_sequence_ == 0 || context_ == nullptr) {
      return;
    }

    std::vector<GpuNv12Slot*> ready_slots;
    for (size_t i = 0; i < kGpuNv12RingDepth; ++i) {
      auto& slot = gpu_nv12_slots_[i];
      if (!slot.pending) {
        continue;
      }
      if (!IsGpuNv12SlotReady(slot)) {
        continue;
      }
      ready_slots.push_back(&slot);
    }

    if (ready_slots.empty()) {
      return;
    }

    std::sort(ready_slots.begin(), ready_slots.end(),
              [](const GpuNv12Slot* a, const GpuNv12Slot* b) {
                return a->sequence < b->sequence;
              });

    const size_t queue_depth =
        std::min(delivery_queue_depth_, native_nv12_ready_drain_depth_);
    const size_t queue_start =
        ready_slots.size() > queue_depth ? ready_slots.size() - queue_depth : 0;
    LARGE_INTEGER drop_now{};
    if (queue_start > 0) {
      QueryPerformanceCounter(&drop_now);
    }
    for (size_t i = 0; i < queue_start; ++i) {
      auto* dropped_slot = ready_slots[i];
      int64_t blt_to_ready_qpc = 0;
      if (dropped_slot->after_blt_qpc > 0 &&
          drop_now.QuadPart > dropped_slot->after_blt_qpc) {
        blt_to_ready_qpc = drop_now.QuadPart - dropped_slot->after_blt_qpc;
        RecordGpuNv12BltToReadyBudget(blt_to_ready_qpc);
      }
      TryRecordGpuNv12BltTimestampMetrics(*dropped_slot, blt_to_ready_qpc);
      RecordNativeNv12ReadyDrop(*dropped_slot, drop_now.QuadPart);
      dropped_slot->pending = false;
      ResetGpuNv12BltTimestampQueries(*dropped_slot, false);
      ++gpu_nv12_ready_dropped_frames_;
    }
    for (size_t i = queue_start; i < ready_slots.size(); ++i) {
      QueueReadyGpuNv12Slot(*ready_slots[i], observation);
    }
  }

  bool HasPendingGpuNv12Slots() const {
    for (const auto& slot : gpu_nv12_slots_) {
      if (slot.pending) {
        return true;
      }
    }
    return false;
  }

  size_t PendingGpuNv12SlotCount() const {
    size_t count = 0;
    for (const auto& slot : gpu_nv12_slots_) {
      if (slot.pending) {
        ++count;
      }
    }
    return count;
  }

  bool HasPendingNativeNv12Admission() const {
    return native_nv12_admission_mailbox_enabled_ &&
           native_nv12_pending_admission_.texture != nullptr;
  }

  void ClearPendingNativeNv12Admission() {
    native_nv12_pending_admission_.texture.Reset();
    native_nv12_pending_admission_.source_frame_index = 0;
    native_nv12_pending_admission_.source_qpc = 0;
    native_nv12_pending_admission_.repeated = false;
    native_nv12_pending_admission_.deferred_qpc = 0;
  }

  bool StorePendingNativeNv12Admission(ID3D11Texture2D* texture,
                                       uint64_t source_frame_index,
                                       uint64_t source_qpc, bool repeated,
                                       int64_t now_qpc,
                                       const char* reason) {
    if (!native_nv12_admission_mailbox_enabled_ ||
        source_mode_ != GameCaptureSourceMode::kHelperD3d11 ||
        texture == nullptr) {
      return false;
    }

    if (HasPendingNativeNv12Admission()) {
      ++native_nv12_admission_mailbox_replaced_frames_;
      if (native_nv12_pending_admission_.deferred_qpc > 0 &&
          now_qpc > native_nv12_pending_admission_.deferred_qpc) {
        RecordQpcMetric(
            now_qpc - native_nv12_pending_admission_.deferred_qpc,
            &native_nv12_admission_mailbox_pending_age_qpc_,
            &native_nv12_admission_mailbox_pending_age_max_qpc_,
            &native_nv12_admission_mailbox_pending_age_samples_);
      }
    }

    native_nv12_pending_admission_.texture.Reset();
    texture->AddRef();
    native_nv12_pending_admission_.texture.Attach(texture);
    native_nv12_pending_admission_.source_frame_index = source_frame_index;
    native_nv12_pending_admission_.source_qpc = source_qpc;
    native_nv12_pending_admission_.repeated = repeated;
    native_nv12_pending_admission_.deferred_qpc = now_qpc;
    ++native_nv12_admission_mailbox_stored_frames_;

    if (native_nv12_admission_mailbox_stored_frames_ <= 5 ||
        native_nv12_admission_mailbox_stored_frames_ % 60 == 0) {
      Log("native_nv12_admission_mailbox_store"
          " reason=" +
          std::string(reason == nullptr ? "unknown" : reason) +
          " sourceFrameIndex=" + std::to_string(source_frame_index) +
          " sourceAgeMs=" + SourceAgeMsLabel(source_qpc, now_qpc) +
          " stored=" +
          std::to_string(native_nv12_admission_mailbox_stored_frames_) +
          " replaced=" +
          std::to_string(native_nv12_admission_mailbox_replaced_frames_));
    }
    return true;
  }

  bool TrySubmitPendingNativeNv12Admission(
      const char* reason, uint64_t* submitted_source_frame_index,
      uint64_t* submitted_source_qpc) {
    if (submitted_source_frame_index != nullptr) {
      *submitted_source_frame_index = 0;
    }
    if (submitted_source_qpc != nullptr) {
      *submitted_source_qpc = 0;
    }
    if (!native_nv12_admission_mailbox_enabled_ ||
        source_mode_ != GameCaptureSourceMode::kHelperD3d11 ||
        !HasPendingNativeNv12Admission() || HasPendingGpuNv12Slots()) {
      return false;
    }

    LARGE_INTEGER start{};
    QueryPerformanceCounter(&start);
    NativeNv12PendingAdmission pending;
    pending.texture = native_nv12_pending_admission_.texture;
    pending.source_frame_index =
        native_nv12_pending_admission_.source_frame_index;
    pending.source_qpc = native_nv12_pending_admission_.source_qpc;
    pending.repeated = native_nv12_pending_admission_.repeated;
    pending.deferred_qpc = native_nv12_pending_admission_.deferred_qpc;
    ClearPendingNativeNv12Admission();

    if (pending.deferred_qpc > 0 && start.QuadPart > pending.deferred_qpc) {
      RecordQpcMetric(start.QuadPart - pending.deferred_qpc,
                      &native_nv12_admission_mailbox_pending_age_qpc_,
                      &native_nv12_admission_mailbox_pending_age_max_qpc_,
                      &native_nv12_admission_mailbox_pending_age_samples_);
    }

    if (SourceAgeExceedsNativeNv12AdmissionBudget(pending.source_qpc,
                                                  start.QuadPart)) {
      ++native_nv12_admission_mailbox_stale_dropped_frames_;
      Log("native_nv12_admission_mailbox_stale_dropped"
          " reason=" +
          std::string(reason == nullptr ? "unknown" : reason) +
          " sourceFrameIndex=" + std::to_string(pending.source_frame_index) +
          " sourceAgeMs=" +
          SourceAgeMsLabel(pending.source_qpc, start.QuadPart) +
          " staleDropped=" +
          std::to_string(native_nv12_admission_mailbox_stale_dropped_frames_));
      return false;
    }

    RecordSourceAge(
        pending.source_qpc, start.QuadPart,
        &native_nv12_admission_mailbox_submit_source_age_qpc_,
        &native_nv12_admission_mailbox_submit_source_age_max_qpc_,
        &native_nv12_admission_mailbox_submit_source_age_samples_);

    if (!SubmitTexture(pending.texture.Get(), pending.source_frame_index,
                       pending.source_qpc, pending.repeated)) {
      return false;
    }

    ++native_nv12_admission_mailbox_submitted_frames_;
    if (submitted_source_frame_index != nullptr) {
      *submitted_source_frame_index = pending.source_frame_index;
    }
    if (submitted_source_qpc != nullptr) {
      *submitted_source_qpc = pending.source_qpc;
    }
    if (native_nv12_admission_mailbox_submitted_frames_ <= 5 ||
        native_nv12_admission_mailbox_submitted_frames_ % 60 == 0) {
      Log("native_nv12_admission_mailbox_submitted"
          " reason=" +
          std::string(reason == nullptr ? "unknown" : reason) +
          " sourceFrameIndex=" + std::to_string(pending.source_frame_index) +
          " sourceAgeMs=" +
          SourceAgeMsLabel(pending.source_qpc, start.QuadPart) +
          " submitted=" +
          std::to_string(native_nv12_admission_mailbox_submitted_frames_));
    }
    return true;
  }

  bool DeferNativeNv12ForSingleInFlight(uint64_t attempt,
                                        uint64_t source_frame_index,
                                        uint64_t source_qpc, int64_t now_qpc) {
    if (!native_nv12_single_in_flight_enabled_ || !HasPendingGpuNv12Slots()) {
      return false;
    }

    const size_t pending_count = PendingGpuNv12SlotCount();
    native_nv12_single_in_flight_pending_max_ = std::max<uint64_t>(
        native_nv12_single_in_flight_pending_max_, pending_count);
    ++native_nv12_single_in_flight_deferred_frames_;
    RecordSourceAge(source_qpc, now_qpc,
                    &native_nv12_single_in_flight_deferred_source_age_qpc_,
                    &native_nv12_single_in_flight_deferred_source_age_max_qpc_,
                    &native_nv12_single_in_flight_deferred_source_age_samples_);
    if (!SourceAgeExceedsNativeNv12Budget(source_qpc, now_qpc)) {
      ++native_nv12_single_in_flight_deferred_fresh_frames_;
    }

    if (native_nv12_single_in_flight_deferred_frames_ <= 5 ||
        native_nv12_single_in_flight_deferred_frames_ % 60 == 0) {
      LogGpuNv12Stage(
          attempt,
          "single_in_flight_deferred pending=" + std::to_string(pending_count) +
              " sourceFrameIndex=" + std::to_string(source_frame_index) +
              " sourceAgeMs=" + SourceAgeMsLabel(source_qpc, now_qpc) +
              " deferred=" +
              std::to_string(native_nv12_single_in_flight_deferred_frames_) +
              " fresh=" +
              std::to_string(
                  native_nv12_single_in_flight_deferred_fresh_frames_));
    }
    return true;
  }

  bool DeferNativeNv12ForGpuQueueBackoff(uint64_t attempt,
                                         uint64_t source_frame_index,
                                         uint64_t source_qpc, int64_t now_qpc) {
    if (!native_nv12_gpu_queue_backoff_enabled_ ||
        source_mode_ != GameCaptureSourceMode::kHelperD3d11 ||
        native_nv12_gpu_queue_backoff_until_qpc_ <= 0) {
      return false;
    }
    if (now_qpc >= native_nv12_gpu_queue_backoff_until_qpc_) {
      native_nv12_gpu_queue_backoff_until_qpc_ = 0;
      return false;
    }

    ++native_nv12_gpu_queue_backoff_suppressed_frames_;
    RecordSourceAge(
        source_qpc, now_qpc,
        &native_nv12_gpu_queue_backoff_suppressed_source_age_qpc_,
        &native_nv12_gpu_queue_backoff_suppressed_source_age_max_qpc_,
        &native_nv12_gpu_queue_backoff_suppressed_source_age_samples_);
    if (!SourceAgeExceedsNativeNv12Budget(source_qpc, now_qpc)) {
      ++native_nv12_gpu_queue_backoff_suppressed_fresh_frames_;
    }

    if (native_nv12_gpu_queue_backoff_suppressed_frames_ <= 5 ||
        native_nv12_gpu_queue_backoff_suppressed_frames_ % 60 == 0) {
      LogGpuNv12Stage(
          attempt,
          "gpu_queue_backoff_suppressed sourceFrameIndex=" +
              std::to_string(source_frame_index) + " sourceAgeMs=" +
              SourceAgeMsLabel(source_qpc, now_qpc) + " remainingMs=" +
              std::to_string(TicksToMs(
                  native_nv12_gpu_queue_backoff_until_qpc_ - now_qpc)) +
              " suppressed=" +
              std::to_string(native_nv12_gpu_queue_backoff_suppressed_frames_) +
              " fresh=" +
              std::to_string(
                  native_nv12_gpu_queue_backoff_suppressed_fresh_frames_));
    }
    return true;
  }

  void TrimPendingGpuNv12Slots(size_t max_pending_after_trim,
                               uint64_t attempt) {
    std::vector<GpuNv12Slot*> pending_slots;
    for (auto& slot : gpu_nv12_slots_) {
      if (slot.pending) {
        pending_slots.push_back(&slot);
      }
    }
    if (pending_slots.size() <= max_pending_after_trim) {
      return;
    }

    std::sort(pending_slots.begin(), pending_slots.end(),
              [](const GpuNv12Slot* a, const GpuNv12Slot* b) {
                return a->sequence < b->sequence;
              });
    const size_t drop_count = pending_slots.size() - max_pending_after_trim;
    LARGE_INTEGER overwrite_now{};
    QueryPerformanceCounter(&overwrite_now);
    for (size_t i = 0; i < drop_count; ++i) {
      RecordNativeNv12Overwrite(*pending_slots[i], overwrite_now.QuadPart);
      pending_slots[i]->pending = false;
      ResetGpuNv12BltTimestampQueries(*pending_slots[i], true);
      ++gpu_nv12_overwritten_frames_;
    }
    LogGpuNv12Stage(
        attempt,
        "trimmed_pending count=" + std::to_string(drop_count) +
            " remaining=" + std::to_string(PendingGpuNv12SlotCount()) +
            " maxPending=" + std::to_string(native_nv12_max_pending_slots_));
  }

  GpuNv12Slot* NextGpuNv12WriteSlot() {
    for (size_t attempts = 0; attempts < kGpuNv12RingDepth; ++attempts) {
      auto& slot = gpu_nv12_slots_[gpu_nv12_write_index_];
      if (!slot.pending) {
        return &slot;
      }
      if (IsGpuNv12SlotReady(slot)) {
        QueueReadyGpuNv12Slot(slot, GpuNv12ReadyObservation::kWriteSlotScan);
        gpu_nv12_write_index_ = (gpu_nv12_write_index_ + 1) % kGpuNv12RingDepth;
        continue;
      }
      LARGE_INTEGER overwrite_now{};
      QueryPerformanceCounter(&overwrite_now);
      RecordNativeNv12Overwrite(slot, overwrite_now.QuadPart);
      slot.pending = false;
      ResetGpuNv12BltTimestampQueries(slot, true);
      ++gpu_nv12_overwritten_frames_;
      return &slot;
    }
    auto& slot = gpu_nv12_slots_[gpu_nv12_write_index_];
    LARGE_INTEGER overwrite_now{};
    QueryPerformanceCounter(&overwrite_now);
    RecordNativeNv12Overwrite(slot, overwrite_now.QuadPart);
    slot.pending = false;
    ResetGpuNv12BltTimestampQueries(slot, true);
    ++gpu_nv12_overwritten_frames_;
    return &slot;
  }

  ID3D11ShaderResourceView* SourceShaderViewFor(ID3D11Texture2D* texture) {
    if (device_ == nullptr || texture == nullptr) {
      return nullptr;
    }
    const auto existing = gpu_source_shader_views_.find(texture);
    if (existing != gpu_source_shader_views_.end()) {
      return existing->second.Get();
    }

    ComPtr<ID3D11ShaderResourceView> view;
    const HRESULT hr =
        device_->CreateShaderResourceView(texture, nullptr, &view);
    if (FAILED(hr)) {
      LogGpuScaleFailure("source_srv_create_failed", hr);
      RecoverConsumerD3dDevice("gpu_scale.source_srv_create", hr, true);
      return nullptr;
    }
    auto* raw = view.Get();
    gpu_source_shader_views_[texture] = std::move(view);
    return raw;
  }

  bool SubmitGpuScaledNv12(ID3D11Texture2D* texture,
                           const D3D11_TEXTURE2D_DESC& source_desc,
                           int output_width, int output_height,
                           uint64_t source_frame_index, uint64_t source_qpc,
                           bool repeated, const LARGE_INTEGER& start,
                           bool* deferred) {
    if (deferred != nullptr) {
      *deferred = false;
    }
    const uint64_t attempt = ++gpu_nv12_attempts_;
    LogGpuNv12Stage(attempt,
                    "begin source=" + std::to_string(source_desc.Width) + "x" +
                        std::to_string(source_desc.Height) +
                        " output=" + std::to_string(output_width) + "x" +
                        std::to_string(output_height) +
                        " format=" + std::to_string(source_desc.Format));
    DrainReadyGpuNv12Slots(GpuNv12ReadyObservation::kPreSubmit);
    RecordSourceAge(source_qpc, start.QuadPart,
                    &gpu_nv12_conversion_start_age_qpc_,
                    &gpu_nv12_conversion_start_age_max_qpc_,
                    &gpu_nv12_conversion_start_age_samples_);
    if (SourceAgeExceedsNativeNv12AdmissionBudget(source_qpc, start.QuadPart)) {
      ++gpu_nv12_stale_before_queue_frames_;
      LogGpuNv12Stage(
          attempt,
          "skipped_stale_before_queue sourceAgeMs=" +
              SourceAgeMsLabel(source_qpc, start.QuadPart) +
              " admissionMaxSourceAgeMs=" +
              std::to_string(native_nv12_admission_max_source_age_ms_) +
              " skipped=" +
              std::to_string(gpu_nv12_stale_before_queue_frames_));
      return true;
    }
    if (shared_state_ != nullptr && source_frame_index != 0 &&
        shared_state_->latest_frame_index > source_frame_index) {
      ++gpu_nv12_stale_before_queue_frames_;
      LogGpuNv12Stage(
          attempt,
          "skipped_stale_before_queue reason=newer_source_frame latest=" +
              std::to_string(shared_state_->latest_frame_index) +
              " sourceFrameIndex=" + std::to_string(source_frame_index) +
              " skipped=" +
              std::to_string(gpu_nv12_stale_before_queue_frames_));
      return true;
    }
    if (DeferNativeNv12ForSingleInFlight(attempt, source_frame_index,
                                         source_qpc, start.QuadPart)) {
      if (deferred != nullptr) {
        *deferred = true;
      }
      return false;
    }
    if (DeferNativeNv12ForGpuQueueBackoff(attempt, source_frame_index,
                                          source_qpc, start.QuadPart)) {
      if (deferred != nullptr) {
        *deferred = true;
      }
      return false;
    }
    if (!EnsureGpuScaledBgraResources(source_desc, output_width,
                                      output_height)) {
      return false;
    }
    LogGpuNv12Stage(attempt, "gpu_scale_resources_ready");
    ID3D11ShaderResourceView* source_view = SourceShaderViewFor(texture);
    if (source_view == nullptr) {
      return false;
    }
    LogGpuNv12Stage(attempt, "source_srv_ready");

    D3D11_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(output_width);
    viewport.Height = static_cast<float>(output_height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    ID3D11RenderTargetView* render_targets[] = {gpu_scaled_rtv_.Get()};
    ID3D11ShaderResourceView* shader_resources[] = {source_view};
    ID3D11SamplerState* samplers[] = {gpu_sampler_state_.Get()};
    context_->RSSetViewports(1, &viewport);
    context_->OMSetRenderTargets(1, render_targets, nullptr);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(gpu_vertex_shader_.Get(), nullptr, 0);
    context_->PSSetShader(gpu_pixel_shader_.Get(), nullptr, 0);
    if (!SetGpuScaleMarkerConstants(output_width, output_height,
                                    source_frame_index, source_qpc)) {
      return false;
    }
    context_->PSSetShaderResources(0, 1, shader_resources);
    context_->PSSetSamplers(0, 1, samplers);
    context_->Draw(3, 0);
    ID3D11Buffer* null_constants[] = {nullptr};
    context_->PSSetConstantBuffers(0, 1, null_constants);
    ID3D11ShaderResourceView* null_srv[] = {nullptr};
    context_->PSSetShaderResources(0, 1, null_srv);
    ID3D11RenderTargetView* null_rtv[] = {nullptr};
    context_->OMSetRenderTargets(1, null_rtv, nullptr);
    LARGE_INTEGER after_gpu{};
    QueryPerformanceCounter(&after_gpu);
    LogGpuNv12Stage(attempt, "bgra_scale_drawn");

    if (native_nv12_render_convert_enabled_) {
      if (!native_nv12_render_convert_logged_) {
        native_nv12_render_convert_logged_ = true;
        LogSync(
            "native_nv12_render_convert_enabled "
            "path=bgra_scale_plus_two_pass_plane_render "
            "replacing=video_processor_blt color=bt709_limited_range "
            "default=off");
      }
      return SubmitGpuNv12RenderConvert(source_desc, output_width, output_height,
                                        source_frame_index, source_qpc, repeated,
                                        start, after_gpu, attempt);
    }

    if (!EnsureGpuNv12Resources(output_width, output_height)) {
      return false;
    }
    LogGpuNv12Stage(attempt, "nv12_resources_ready");

    if (native_nv12_max_pending_slots_ < kGpuNv12MaxPendingSlots) {
      TrimPendingGpuNv12Slots(native_nv12_max_pending_slots_ - 1, attempt);
    }

    GpuNv12Slot* slot = NextGpuNv12WriteSlot();
    if (slot == nullptr || slot->output_view == nullptr ||
        slot->ready_query == nullptr) {
      LogGpuNv12Failure("native_slot_unavailable", E_FAIL);
      return false;
    }
    if (native_nv12_ready_policy_ == NativeNv12ReadyPolicy::kFence) {
      if (!PrepareDirectEncoderGpuNv12Slot(*slot, output_width, output_height,
                                           attempt)) {
        return false;
      }
    } else {
      slot->direct_encoder_output = false;
    }
    RECT rect{
        0,
        0,
        static_cast<LONG>(output_width),
        static_cast<LONG>(output_height),
    };
    nv12_video_context_->VideoProcessorSetStreamFrameFormat(
        nv12_video_processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    nv12_video_context_->VideoProcessorSetStreamSourceRect(
        nv12_video_processor_.Get(), 0, TRUE, &rect);
    nv12_video_context_->VideoProcessorSetStreamDestRect(
        nv12_video_processor_.Get(), 0, TRUE, &rect);
    nv12_video_context_->VideoProcessorSetOutputTargetRect(
        nv12_video_processor_.Get(), TRUE, &rect);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = nv12_video_input_view_.Get();
    BeginGpuNv12BltTimestampQueries(*slot, attempt);
    const HRESULT hr = nv12_video_context_->VideoProcessorBlt(
        nv12_video_processor_.Get(), slot->output_view.Get(), 0, 1, &stream);
    LARGE_INTEGER after_nv12{};
    QueryPerformanceCounter(&after_nv12);
    EndGpuNv12BltTimestampQueries(*slot);
    if (FAILED(hr)) {
      LogGpuNv12Failure("video_processor_blt_failed", hr);
      RecoverConsumerD3dDevice("gpu_nv12.video_processor_blt", hr, true);
      ResetGpuNv12BltTimestampQueries(*slot, true);
      return false;
    }
    LogGpuNv12Stage(attempt, "video_processor_blt_queued");

    slot->pending = true;
    slot->sequence = ++gpu_nv12_sequence_;
    slot->attempt = attempt;
    slot->source_frame_index = source_frame_index;
    slot->source_qpc = source_qpc;
    slot->repeated = repeated;
    slot->source_width = static_cast<int>(source_desc.Width);
    slot->source_height = static_cast<int>(source_desc.Height);
    slot->output_width = output_width;
    slot->output_height = output_height;
    slot->source_format = source_desc.Format;
    slot->start_qpc = start.QuadPart;
    slot->after_gpu_qpc = after_gpu.QuadPart;
    slot->after_blt_qpc = after_nv12.QuadPart;
    slot->ready_fence.Reset();
    slot->ready_fence_value = 0;
    ++gpu_nv12_queued_frames_;
    gpu_scale_us_ += after_gpu.QuadPart - start.QuadPart;
    gpu_nv12_write_index_ = (gpu_nv12_write_index_ + 1) % kGpuNv12RingDepth;
    if (native_nv12_ready_policy_ == NativeNv12ReadyPolicy::kFence) {
      if (!SignalNativeNv12FenceValue(
              &slot->ready_fence, &slot->ready_fence_value, attempt,
              "direct_native_ready_fence_signal_failed_after_blt",
              "direct_native_ready_fence_signaled_after_blt")) {
        slot->pending = false;
        return false;
      }
      if (IsGpuNv12FenceReady(*slot, false)) {
        LogGpuNv12Stage(attempt, "native_frame_ready_direct_queued_after_blt");
        QueueReadyGpuNv12Slot(*slot,
                              GpuNv12ReadyObservation::kImmediateAfterBlt);
      } else if (RegisterNativeNv12FenceWake(*slot)) {
        DrainReadyGpuNv12Slots(GpuNv12ReadyObservation::kPostFenceRegistration);
      } else {
        LogGpuNv12Stage(
            attempt,
            "native_frame_ready_fence_wake_unavailable_queued_after_blt");
        QueueReadyGpuNv12Slot(*slot,
                              GpuNv12ReadyObservation::kImmediateAfterBlt);
      }
      return true;
    }
    if (native_nv12_ready_policy_ == NativeNv12ReadyPolicy::kQueueAfterBlt) {
      LogGpuNv12Stage(attempt, "native_frame_ready_queued_after_blt");
      QueueReadyGpuNv12Slot(*slot, GpuNv12ReadyObservation::kImmediateAfterBlt);
      return true;
    }
    context_->End(slot->ready_query.Get());
    LogGpuNv12Stage(attempt, "native_frame_ready_pending");
    DrainReadyGpuNv12Slots(GpuNv12ReadyObservation::kPostFenceRegistration);
    return true;
  }

  bool QueueGpuScaledBgraReadback(
      ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC& source_desc,
      int output_width, int output_height, uint64_t source_frame_index,
      uint64_t source_qpc, bool repeated, const LARGE_INTEGER& start,
      LARGE_INTEGER* after_gpu, LARGE_INTEGER* after_copy) {
    if (!EnsureGpuScaledBgraResources(source_desc, output_width,
                                      output_height)) {
      return false;
    }
    ID3D11ShaderResourceView* source_view = SourceShaderViewFor(texture);
    if (source_view == nullptr) {
      return false;
    }

    D3D11_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(output_width);
    viewport.Height = static_cast<float>(output_height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    ID3D11RenderTargetView* render_targets[] = {gpu_scaled_rtv_.Get()};
    ID3D11ShaderResourceView* shader_resources[] = {source_view};
    ID3D11SamplerState* samplers[] = {gpu_sampler_state_.Get()};
    context_->RSSetViewports(1, &viewport);
    context_->OMSetRenderTargets(1, render_targets, nullptr);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(gpu_vertex_shader_.Get(), nullptr, 0);
    context_->PSSetShader(gpu_pixel_shader_.Get(), nullptr, 0);
    if (!SetGpuScaleMarkerConstants(output_width, output_height,
                                    source_frame_index, source_qpc)) {
      return false;
    }
    context_->PSSetShaderResources(0, 1, shader_resources);
    context_->PSSetSamplers(0, 1, samplers);
    context_->Draw(3, 0);
    ID3D11Buffer* null_constants[] = {nullptr};
    context_->PSSetConstantBuffers(0, 1, null_constants);
    ID3D11ShaderResourceView* null_srv[] = {nullptr};
    context_->PSSetShaderResources(0, 1, null_srv);
    ID3D11RenderTargetView* null_rtv[] = {nullptr};
    context_->OMSetRenderTargets(1, null_rtv, nullptr);
    QueryPerformanceCounter(after_gpu);

    GpuReadbackSlot& slot = gpu_readback_slots_[gpu_readback_write_index_];
    if (slot.pending) {
      ++gpu_readback_overwritten_frames_;
      slot.pending = false;
    }
    context_->CopyResource(slot.texture.Get(), gpu_scaled_texture_.Get());
    context_->Flush();
    QueryPerformanceCounter(after_copy);
    slot.pending = true;
    slot.sequence = ++gpu_readback_sequence_;
    slot.source_frame_index = source_frame_index;
    slot.source_qpc = source_qpc;
    slot.repeated = repeated;
    slot.source_width = static_cast<int>(source_desc.Width);
    slot.source_height = static_cast<int>(source_desc.Height);
    slot.output_width = output_width;
    slot.output_height = output_height;
    slot.source_format = source_desc.Format;
    slot.start_qpc = start.QuadPart;
    slot.after_gpu_qpc = after_gpu->QuadPart;
    slot.after_copy_qpc = after_copy->QuadPart;
    gpu_readback_write_index_ =
        (gpu_readback_write_index_ + 1) % kGpuReadbackRingDepth;
    ++gpu_readback_queued_frames_;
    gpu_scale_us_ += after_gpu->QuadPart - start.QuadPart;
    readback_copy_us_ += after_copy->QuadPart - after_gpu->QuadPart;
    return true;
  }

  bool ConvertMappedToArgb(const D3D11_MAPPED_SUBRESOURCE& mapped,
                           DXGI_FORMAT format, int width, int height,
                           const uint8_t** argb_data, int* argb_stride) {
    if (IsBgra(format)) {
      *argb_data = static_cast<const uint8_t*>(mapped.pData);
      *argb_stride = static_cast<int>(mapped.RowPitch);
      return true;
    }

    argb_buffer_.assign(static_cast<size_t>(width) * height * 4, 0);
    uint8_t* out = argb_buffer_.data();
    const auto* in = static_cast<const uint8_t*>(mapped.pData);
    if (IsRgba(format)) {
      for (int y = 0; y < height; ++y) {
        const uint8_t* row = in + static_cast<size_t>(y) * mapped.RowPitch;
        uint8_t* dst = out + static_cast<size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
          dst[x * 4 + 0] = row[x * 4 + 2];
          dst[x * 4 + 1] = row[x * 4 + 1];
          dst[x * 4 + 2] = row[x * 4 + 0];
          dst[x * 4 + 3] = row[x * 4 + 3];
        }
      }
    } else if (IsR10G10B10A2(format)) {
      for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(
            in + static_cast<size_t>(y) * mapped.RowPitch);
        uint8_t* dst = out + static_cast<size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
          const uint32_t pixel = row[x];
          const uint32_t r = pixel & 0x3ffu;
          const uint32_t g = (pixel >> 10) & 0x3ffu;
          const uint32_t b = (pixel >> 20) & 0x3ffu;
          dst[x * 4 + 0] = static_cast<uint8_t>((b * 255u + 511u) / 1023u);
          dst[x * 4 + 1] = static_cast<uint8_t>((g * 255u + 511u) / 1023u);
          dst[x * 4 + 2] = static_cast<uint8_t>((r * 255u + 511u) / 1023u);
          dst[x * 4 + 3] = 255;
        }
      }
    } else {
      return false;
    }

    *argb_data = argb_buffer_.data();
    *argb_stride = width * 4;
    return true;
  }

  GpuReadbackSlot* OldestPendingReadbackSlot(uint64_t max_sequence) {
    GpuReadbackSlot* oldest = nullptr;
    for (auto& slot : gpu_readback_slots_) {
      if (!slot.pending || slot.sequence == 0 || slot.sequence > max_sequence) {
        continue;
      }
      if (max_sequence > slot.sequence + kMaxGpuReadbackLatencyFrames) {
        ++gpu_readback_latency_dropped_frames_;
        slot.pending = false;
        continue;
      }
      if (oldest == nullptr || slot.sequence < oldest->sequence) {
        oldest = &slot;
      }
    }
    return oldest;
  }

  bool TrySubmitReadyGpuReadback(bool include_latest) {
    if (gpu_readback_sequence_ == 0 || context_ == nullptr) {
      return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    LARGE_INTEGER map_start{};
    LARGE_INTEGER after_map{};
    LARGE_INTEGER after_convert{};
    const uint64_t max_sequence =
        include_latest ? gpu_readback_sequence_ : gpu_readback_sequence_ - 1;
    if (max_sequence == 0) {
      return false;
    }
    std::vector<GpuReadbackSlot*> candidates;
    for (auto& candidate : gpu_readback_slots_) {
      if (!candidate.pending || candidate.sequence == 0 ||
          candidate.sequence > max_sequence || candidate.texture == nullptr) {
        continue;
      }
      if (last_submitted_source_frame_index_ != 0 &&
          candidate.source_frame_index != 0 &&
          candidate.source_frame_index <= last_submitted_source_frame_index_) {
        ++gpu_readback_stale_dropped_frames_;
        candidate.pending = false;
        continue;
      }
      if (max_sequence > candidate.sequence + kMaxGpuReadbackLatencyFrames) {
        ++gpu_readback_latency_dropped_frames_;
        candidate.pending = false;
        continue;
      }
      candidates.push_back(&candidate);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const GpuReadbackSlot* a, const GpuReadbackSlot* b) {
                if (a->source_frame_index != 0 && b->source_frame_index != 0 &&
                    a->source_frame_index != b->source_frame_index) {
                  return a->source_frame_index > b->source_frame_index;
                }
                return a->sequence > b->sequence;
              });

    GpuReadbackSlot* slot = nullptr;
    for (GpuReadbackSlot* candidate : candidates) {
      QueryPerformanceCounter(&map_start);
      ++gpu_readback_map_attempts_;
      const HRESULT candidate_map_hr =
          context_->Map(candidate->texture.Get(), 0, D3D11_MAP_READ,
                        D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
      QueryPerformanceCounter(&after_map);
      if (candidate_map_hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        ++gpu_readback_not_ready_frames_;
        continue;
      }
      if (FAILED(candidate_map_hr)) {
        ++map_failures_;
        candidate->pending = false;
        Log("scaled_readback_map_failed hr=" + HResultHex(candidate_map_hr));
        if (RecoverConsumerD3dDevice("gpu_readback.map", candidate_map_hr,
                                     true)) {
          return false;
        }
        continue;
      }
      RecordQpcMetric(map_start.QuadPart - candidate->after_copy_qpc,
                      &readback_queue_to_map_qpc_,
                      &readback_queue_to_map_max_qpc_,
                      &readback_queue_to_map_samples_);
      if (candidate->source_qpc > 0 &&
          after_map.QuadPart > static_cast<int64_t>(candidate->source_qpc)) {
        RecordQpcMetric(
            after_map.QuadPart - static_cast<int64_t>(candidate->source_qpc),
            &source_to_readback_ready_qpc_, &source_to_readback_ready_max_qpc_,
            &source_to_readback_ready_samples_);
      }
      slot = candidate;
      break;
    }
    if (slot == nullptr) {
      return false;
    }

    if (ShouldDropRegressingSourceFrame(slot->source_frame_index,
                                        "scaled_readback")) {
      context_->Unmap(slot->texture.Get(), 0);
      slot->pending = false;
      return false;
    }

    const auto* scaled_argb = static_cast<const uint8_t*>(mapped.pData);
    const int scaled_stride = static_cast<int>(mapped.RowPitch);
    const BgraVisibilityStats scaled_visibility = AnalyzeBgra(
        scaled_argb, slot->output_width, slot->output_height, scaled_stride);
    MaybeWriteProof(scaled_argb, slot->output_width, slot->output_height,
                    scaled_stride, slot->source_width, slot->source_height,
                    slot->source_format, scaled_visibility);

    if (!visible_source_seen_) {
      if (scaled_visibility.visible) {
        visible_source_seen_ = true;
        if (initial_black_skipped_frames_ > 0) {
          Log("startup_visible_after_black skipped=" +
              std::to_string(initial_black_skipped_frames_) +
              " source=" + std::to_string(slot->source_width) + "x" +
              std::to_string(slot->source_height) +
              " output=" + std::to_string(slot->output_width) + "x" +
              std::to_string(slot->output_height) +
              " format=" + std::to_string(slot->source_format) +
              " minLuma=" + std::to_string(scaled_visibility.min_luma) +
              " maxLuma=" + std::to_string(scaled_visibility.max_luma) +
              " nonzeroSamples=" +
              std::to_string(scaled_visibility.nonzero_samples) +
              " samples=" + std::to_string(scaled_visibility.samples));
        }
      } else {
        const uint64_t max_initial_black_skips =
            std::max<uint64_t>(1, target_fps_ * 2);
        if (initial_black_skipped_frames_ < max_initial_black_skips) {
          ++initial_black_skipped_frames_;
          if (initial_black_skipped_frames_ == 1 ||
              initial_black_skipped_frames_ == max_initial_black_skips ||
              initial_black_skipped_frames_ % target_fps_ == 0) {
            Log("skip_initial_black frame=" +
                std::to_string(initial_black_skipped_frames_) +
                " source=" + std::to_string(slot->source_width) + "x" +
                std::to_string(slot->source_height) +
                " output=" + std::to_string(slot->output_width) + "x" +
                std::to_string(slot->output_height) +
                " format=" + std::to_string(slot->source_format) +
                " visible=false minLuma=" +
                std::to_string(scaled_visibility.min_luma) +
                " maxLuma=" + std::to_string(scaled_visibility.max_luma) +
                " nonzeroSamples=" +
                std::to_string(scaled_visibility.nonzero_samples) +
                " samples=" + std::to_string(scaled_visibility.samples) +
                " initialBlackSkipped=" +
                std::to_string(initial_black_skipped_frames_));
          }
          context_->Unmap(slot->texture.Get(), 0);
          slot->pending = false;
          return false;
        }
        if (!startup_black_skip_limit_logged_) {
          startup_black_skip_limit_logged_ = true;
          Log("startup_black_skip_limit_reached skipped=" +
              std::to_string(initial_black_skipped_frames_) +
              " source=" + std::to_string(slot->source_width) + "x" +
              std::to_string(slot->source_height) +
              " output=" + std::to_string(slot->output_width) + "x" +
              std::to_string(slot->output_height) +
              " format=" + std::to_string(slot->source_format));
        }
      }
    }

    webrtc::scoped_refptr<webrtc::I420Buffer> i420 =
        webrtc::I420Buffer::Create(slot->output_width, slot->output_height);
    const int rc = libyuv::ARGBToI420(
        scaled_argb, scaled_stride, i420->MutableDataY(), i420->StrideY(),
        i420->MutableDataU(), i420->StrideU(), i420->MutableDataV(),
        i420->StrideV(), slot->output_width, slot->output_height);
    context_->Unmap(slot->texture.Get(), 0);
    QueryPerformanceCounter(&after_convert);
    if (rc != 0) {
      ++convert_failures_;
      slot->pending = false;
      return false;
    }
    RecordQpcMetric(after_convert.QuadPart - after_map.QuadPart,
                    &map_to_i420_qpc_, &map_to_i420_max_qpc_,
                    &map_to_i420_samples_);
    if (slot->source_qpc > 0 &&
        after_convert.QuadPart > static_cast<int64_t>(slot->source_qpc)) {
      RecordQpcMetric(
          after_convert.QuadPart - static_cast<int64_t>(slot->source_qpc),
          &source_to_i420_ready_qpc_, &source_to_i420_ready_max_qpc_,
          &source_to_i420_ready_samples_);
    }

    MaybeWriteI420Proof(i420, slot->output_width, slot->output_height,
                        slot->source_width, slot->source_height,
                        slot->source_format, scaled_visibility.visible);

    ++gpu_readback_ready_frames_;
    readback_map_us_ += after_map.QuadPart - map_start.QuadPart;
    readback_latency_us_ += map_start.QuadPart - slot->after_copy_qpc;
    const uint64_t latency_frames =
        gpu_readback_sequence_ > slot->sequence
            ? gpu_readback_sequence_ - slot->sequence
            : 0;
    readback_latency_frames_total_ += latency_frames;
    readback_latency_frames_max_ =
        std::max<uint64_t>(readback_latency_frames_max_, latency_frames);
    convert_us_ += after_convert.QuadPart - after_map.QuadPart;
    PendingVideoFrame frame;
    frame.buffer = i420;
    frame.source_width = slot->source_width;
    frame.source_height = slot->source_height;
    frame.output_width = slot->output_width;
    frame.output_height = slot->output_height;
    frame.source_format = slot->source_format;
    frame.source_frame_index = slot->source_frame_index;
    frame.source_qpc = slot->source_qpc;
    frame.ready_qpc = after_convert.QuadPart;
    frame.repeated = slot->repeated;
    frame.gpu_scaled = true;
    slot->pending = false;
    QueueFrameForDelivery(std::move(frame));
    return true;
  }

  bool SubmitTextureViaGpuScaledBgra(ID3D11Texture2D* texture,
                                     const D3D11_TEXTURE2D_DESC& desc,
                                     uint64_t source_frame_index,
                                     uint64_t source_qpc, bool repeated,
                                     bool* should_fallback) {
    if (should_fallback != nullptr) {
      *should_fallback = false;
    }
    const int source_width = static_cast<int>(desc.Width);
    const int source_height = static_cast<int>(desc.Height);
    const int effective_max_width =
        max_width_ > 1280
            ? std::min<int>(max_width_, kMaxStableGameHookReadbackWidth)
            : static_cast<int>(max_width_);
    const int effective_max_height =
        max_height_ > 720
            ? std::min<int>(max_height_, kMaxStableGameHookReadbackHeight)
            : static_cast<int>(max_height_);
    const double scale = std::min(
        {static_cast<double>(effective_max_width) / source_width,
         static_cast<double>(effective_max_height) / source_height, 1.0});
    const int output_width =
        ContainFitDimension(source_width, effective_max_width, scale);
    const int output_height =
        ContainFitDimension(source_height, effective_max_height, scale);
    last_source_width_ = source_width;
    last_source_height_ = source_height;
    last_source_format_ = desc.Format;

    const uint64_t recoveries_before_submit = consumer_device_recoveries_;
    const bool is_r10_source = IsR10G10B10A2(desc.Format);
    const bool native_nv12_source_supported =
        !is_r10_source || !native_nv12_r10_runtime_disabled_;
    const bool native_nv12_suspended_after_onframe_backpressure =
        native_nv12_suspended_after_onframe_backpressure_.load();
    if (kEnableNativeNv12EncoderHandoff && !native_nv12_force_disabled_ &&
        !native_nv12_suspended_after_device_loss_ &&
        !native_nv12_suspended_after_onframe_backpressure &&
        native_nv12_source_supported) {
      const bool native_nv12_warmup =
          delivery_submitted_frames_ < native_nv12_warmup_i420_frames_;
      if (!native_nv12_warmup) {
        if (is_r10_source && !native_nv12_r10_attempt_logged_) {
          native_nv12_r10_attempt_logged_ = true;
          LogSync(
              "native_nv12_encoder_handoff_enabled "
              "source_format=r10g10b10a2 "
              "via_gpu_scale_bgra_intermediate=true "
              "fallback_on_failure=true");
        }
        LARGE_INTEGER native_start{};
        QueryPerformanceCounter(&native_start);
        bool native_nv12_deferred = false;
        if (SubmitGpuScaledNv12(texture, desc, output_width, output_height,
                                source_frame_index, source_qpc, repeated,
                                native_start, &native_nv12_deferred)) {
          native_nv12_r10_consecutive_failures_ = 0;
          last_output_width_ = output_width;
          last_output_height_ = output_height;
          return true;
        }
        if (native_nv12_deferred) {
          StorePendingNativeNv12Admission(texture, source_frame_index,
                                          source_qpc, repeated,
                                          native_start.QuadPart,
                                          "native_nv12_deferred");
          return false;
        }
        if (consumer_device_recoveries_ != recoveries_before_submit) {
          return false;
        }
        if (is_r10_source) {
          ++native_nv12_r10_consecutive_failures_;
          if (native_nv12_r10_consecutive_failures_ >=
              kNativeNv12R10FailureDisableThreshold) {
            native_nv12_r10_runtime_disabled_ = true;
            native_nv12_r10_failure_disable_logged_ = false;
          } else {
            Log("native_nv12_encoder_handoff_retryable_failure "
                "reason=r10g10b10a2_native_nv12_attempt_failed "
                "consecutiveFailures=" +
                std::to_string(native_nv12_r10_consecutive_failures_) +
                " disableThreshold=" +
                std::to_string(kNativeNv12R10FailureDisableThreshold) +
                " using_gpu_scale_i420_readback_for_frame=true");
          }
        }
      } else if (!native_nv12_warmup_logged_) {
        native_nv12_warmup_logged_ = true;
        Log("native_nv12_encoder_warmup using_i420_readback frames=" +
            std::to_string(native_nv12_warmup_i420_frames_));
      }
    } else if (native_nv12_force_disabled_) {
      if (!native_nv12_disabled_logged_) {
        native_nv12_disabled_logged_ = true;
        Log("native_nv12_encoder_handoff_disabled "
            "reason=env_disabled "
            "using_gpu_scale_i420_readback=true");
      }
    } else if (native_nv12_suspended_after_device_loss_) {
      if (!native_nv12_device_loss_suspend_logged_) {
        native_nv12_device_loss_suspend_logged_ = true;
        Log("native_nv12_encoder_handoff_suspended "
            "reason=consumer_d3d_device_loss "
            "using_gpu_scale_i420_readback=true");
      }
    } else if (native_nv12_suspended_after_onframe_backpressure) {
      if (!native_nv12_onframe_backpressure_suspend_logged_.exchange(true)) {
        Log("native_nv12_encoder_handoff_suspended "
            "reason=live_onframe_backpressure "
            "thresholdMs=" +
            std::to_string(native_nv12_onframe_backpressure_threshold_ms_) +
            " frameLimit=" +
            std::to_string(native_nv12_onframe_backpressure_frame_limit_) +
            " using_gpu_scale_i420_readback=true");
      }
    } else if (is_r10_source && native_nv12_r10_runtime_disabled_) {
      if (!native_nv12_r10_failure_disable_logged_) {
        native_nv12_r10_failure_disable_logged_ = true;
        Log("native_nv12_encoder_handoff_disabled "
            "reason=r10g10b10a2_native_nv12_attempt_failed "
            "consecutiveFailures=" +
            std::to_string(native_nv12_r10_consecutive_failures_) +
            " disableThreshold=" +
            std::to_string(kNativeNv12R10FailureDisableThreshold) +
            " "
            "using_gpu_scale_i420_readback=true");
      }
    } else if (!native_nv12_disabled_logged_) {
      native_nv12_disabled_logged_ = true;
      Log("native_nv12_encoder_handoff_disabled "
          "reason=mf_async_not_accepting_stall_guard "
          "using_gpu_scale_i420_readback=true");
    }

    const bool submitted_pending_before_queue = TrySubmitReadyGpuReadback(true);
    if (consumer_device_recoveries_ != recoveries_before_submit) {
      return false;
    }

    LARGE_INTEGER start{};
    LARGE_INTEGER after_gpu{};
    LARGE_INTEGER after_copy{};
    QueryPerformanceCounter(&start);

    if (!QueueGpuScaledBgraReadback(texture, desc, output_width, output_height,
                                    source_frame_index, source_qpc, repeated,
                                    start, &after_gpu, &after_copy)) {
      if (consumer_device_recoveries_ != recoveries_before_submit) {
        return false;
      }
      if (should_fallback != nullptr) {
        *should_fallback = true;
      }
      return false;
    }

    last_output_width_ = output_width;
    last_output_height_ = output_height;
    if (!submitted_pending_before_queue) {
      TrySubmitReadyGpuReadback(false);
    }
    return true;
  }

  bool SubmitTexture(ID3D11Texture2D* texture, uint64_t source_frame_index,
                     uint64_t source_qpc, bool repeated) {
    if (texture == nullptr || context_ == nullptr) {
      return false;
    }
    if (ShouldDropRegressingSourceFrame(source_frame_index, "submit")) {
      return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    const DXGI_FORMAT format = static_cast<DXGI_FORMAT>(desc.Format);
    if (!IsBgra(format) && !IsRgba(format) && !IsR10G10B10A2(format)) {
      if (unsupported_format_ != desc.Format) {
        unsupported_format_ = desc.Format;
        Log("unsupported_format=" + std::to_string(desc.Format));
      }
      return false;
    }

    bool should_fallback_to_cpu = false;
    if (SubmitTextureViaGpuScaledBgra(texture, desc, source_frame_index,
                                      source_qpc, repeated,
                                      &should_fallback_to_cpu)) {
      return true;
    }
    if (!should_fallback_to_cpu) {
      return false;
    }
    ++cpu_fallback_frames_;

    if (!EnsureStagingTexture(desc)) {
      return false;
    }

    LARGE_INTEGER start{};
    LARGE_INTEGER after_copy{};
    LARGE_INTEGER after_map{};
    LARGE_INTEGER after_convert{};
    QueryPerformanceCounter(&start);
    context_->CopyResource(staging_texture_.Get(), texture);
    context_->Flush();
    QueryPerformanceCounter(&after_copy);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr =
        context_->Map(staging_texture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    QueryPerformanceCounter(&after_map);
    if (FAILED(hr)) {
      ++map_failures_;
      Log("map_failed hr=" + HResultHex(hr));
      RecoverConsumerD3dDevice("cpu_readback.map", hr, true);
      return false;
    }

    const int source_width = static_cast<int>(desc.Width);
    const int source_height = static_cast<int>(desc.Height);
    last_source_width_ = source_width;
    last_source_height_ = source_height;
    last_source_format_ = desc.Format;
    const double scale =
        std::min({static_cast<double>(max_width_) / source_width,
                  static_cast<double>(max_height_) / source_height, 1.0});
    const int output_width =
        ContainFitDimension(source_width, static_cast<int>(max_width_), scale);
    const int output_height = ContainFitDimension(
        source_height, static_cast<int>(max_height_), scale);

    const uint8_t* argb_data = nullptr;
    int argb_stride = 0;
    const bool converted = ConvertMappedToArgb(
        mapped, format, source_width, source_height, &argb_data, &argb_stride);
    if (!converted) {
      context_->Unmap(staging_texture_.Get(), 0);
      return false;
    }

    const uint8_t* scaled_argb = argb_data;
    int scaled_stride = argb_stride;
    if (output_width != source_width || output_height != source_height) {
      scaled_argb_buffer_.assign(
          static_cast<size_t>(output_width) * output_height * 4, 0);
      if (libyuv::ARGBScale(argb_data, argb_stride, source_width, source_height,
                            scaled_argb_buffer_.data(), output_width * 4,
                            output_width, output_height,
                            libyuv::kFilterBox) != 0) {
        context_->Unmap(staging_texture_.Get(), 0);
        ++convert_failures_;
        return false;
      }
      scaled_argb = scaled_argb_buffer_.data();
      scaled_stride = output_width * 4;
    }

    const BgraVisibilityStats scaled_visibility =
        AnalyzeBgra(scaled_argb, output_width, output_height, scaled_stride);
    MaybeWriteProof(scaled_argb, output_width, output_height, scaled_stride,
                    source_width, source_height, desc.Format,
                    scaled_visibility);

    if (!visible_source_seen_) {
      if (scaled_visibility.visible) {
        visible_source_seen_ = true;
        if (initial_black_skipped_frames_ > 0) {
          Log("startup_visible_after_black skipped=" +
              std::to_string(initial_black_skipped_frames_) +
              " source=" + std::to_string(source_width) + "x" +
              std::to_string(source_height) +
              " output=" + std::to_string(output_width) + "x" +
              std::to_string(output_height) +
              " format=" + std::to_string(desc.Format) +
              " minLuma=" + std::to_string(scaled_visibility.min_luma) +
              " maxLuma=" + std::to_string(scaled_visibility.max_luma) +
              " nonzeroSamples=" +
              std::to_string(scaled_visibility.nonzero_samples) +
              " samples=" + std::to_string(scaled_visibility.samples));
        }
      } else {
        const uint64_t max_initial_black_skips =
            std::max<uint64_t>(1, target_fps_ * 2);
        if (initial_black_skipped_frames_ < max_initial_black_skips) {
          ++initial_black_skipped_frames_;
          if (initial_black_skipped_frames_ == 1 ||
              initial_black_skipped_frames_ == max_initial_black_skips ||
              initial_black_skipped_frames_ % target_fps_ == 0) {
            Log("skip_initial_black frame=" +
                std::to_string(initial_black_skipped_frames_) +
                " source=" + std::to_string(source_width) + "x" +
                std::to_string(source_height) +
                " output=" + std::to_string(output_width) + "x" +
                std::to_string(output_height) + " format=" +
                std::to_string(desc.Format) + " visible=false minLuma=" +
                std::to_string(scaled_visibility.min_luma) +
                " maxLuma=" + std::to_string(scaled_visibility.max_luma) +
                " nonzeroSamples=" +
                std::to_string(scaled_visibility.nonzero_samples) +
                " samples=" + std::to_string(scaled_visibility.samples) +
                " initialBlackSkipped=" +
                std::to_string(initial_black_skipped_frames_));
          }
          context_->Unmap(staging_texture_.Get(), 0);
          return false;
        }
        if (!startup_black_skip_limit_logged_) {
          startup_black_skip_limit_logged_ = true;
          Log("startup_black_skip_limit_reached skipped=" +
              std::to_string(initial_black_skipped_frames_) +
              " source=" + std::to_string(source_width) + "x" +
              std::to_string(source_height) +
              " output=" + std::to_string(output_width) + "x" +
              std::to_string(output_height) +
              " format=" + std::to_string(desc.Format));
        }
      }
    }

    webrtc::scoped_refptr<webrtc::I420Buffer> i420 =
        webrtc::I420Buffer::Create(output_width, output_height);
    const int rc = libyuv::ARGBToI420(
        scaled_argb, scaled_stride, i420->MutableDataY(), i420->StrideY(),
        i420->MutableDataU(), i420->StrideU(), i420->MutableDataV(),
        i420->StrideV(), output_width, output_height);
    context_->Unmap(staging_texture_.Get(), 0);
    QueryPerformanceCounter(&after_convert);
    if (rc != 0) {
      ++convert_failures_;
      return false;
    }
    if (source_qpc > 0 &&
        after_map.QuadPart > static_cast<int64_t>(source_qpc)) {
      RecordQpcMetric(after_map.QuadPart - static_cast<int64_t>(source_qpc),
                      &source_to_readback_ready_qpc_,
                      &source_to_readback_ready_max_qpc_,
                      &source_to_readback_ready_samples_);
    }
    RecordQpcMetric(after_convert.QuadPart - after_map.QuadPart,
                    &map_to_i420_qpc_, &map_to_i420_max_qpc_,
                    &map_to_i420_samples_);
    if (source_qpc > 0 &&
        after_convert.QuadPart > static_cast<int64_t>(source_qpc)) {
      RecordQpcMetric(after_convert.QuadPart - static_cast<int64_t>(source_qpc),
                      &source_to_i420_ready_qpc_,
                      &source_to_i420_ready_max_qpc_,
                      &source_to_i420_ready_samples_);
    }

    MaybeWriteI420Proof(i420, output_width, output_height, source_width,
                        source_height, desc.Format, scaled_visibility.visible);

    readback_copy_us_ += after_copy.QuadPart - start.QuadPart;
    readback_map_us_ += after_map.QuadPart - after_copy.QuadPart;
    convert_us_ += after_convert.QuadPart - after_map.QuadPart;
    PendingVideoFrame frame;
    frame.buffer = i420;
    frame.source_width = source_width;
    frame.source_height = source_height;
    frame.output_width = output_width;
    frame.output_height = output_height;
    frame.source_format = desc.Format;
    frame.source_frame_index = source_frame_index;
    frame.source_qpc = source_qpc;
    frame.ready_qpc = after_convert.QuadPart;
    frame.repeated = repeated;
    frame.gpu_scaled = false;
    QueueFrameForDelivery(std::move(frame));
    return true;
  }

  void MaybeLogStats(int source_width, int source_height, DXGI_FORMAT format) {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (stats_start_qpc_ == 0) {
      stats_start_qpc_ = now.QuadPart;
      return;
    }
    const int64_t elapsed = now.QuadPart - stats_start_qpc_;
    if (elapsed < frequency_.QuadPart * 5) {
      return;
    }
    const double elapsed_seconds =
        static_cast<double>(elapsed) / static_cast<double>(frequency_.QuadPart);
    const uint64_t submitted = submitted_frames_ - stats_start_frames_;
    const uint64_t queued =
        gpu_readback_queued_frames_ - stats_start_readback_queued_frames_;
    const double fps =
        elapsed_seconds > 0.0 ? submitted / elapsed_seconds : 0.0;
    const double copy_ms = queued == 0
                               ? 0.0
                               : (static_cast<double>(readback_copy_us_) *
                                  1000.0 / frequency_.QuadPart / queued);
    const double gpu_scale_ms = queued == 0
                                    ? 0.0
                                    : (static_cast<double>(gpu_scale_us_) *
                                       1000.0 / frequency_.QuadPart / queued);
    const double map_ms = submitted == 0
                              ? 0.0
                              : (static_cast<double>(readback_map_us_) *
                                 1000.0 / frequency_.QuadPart / submitted);
    const double convert_ms = submitted == 0
                                  ? 0.0
                                  : (static_cast<double>(convert_us_) * 1000.0 /
                                     frequency_.QuadPart / submitted);
    const double readback_latency_ms =
        submitted == 0 ? 0.0
                       : (static_cast<double>(readback_latency_us_) * 1000.0 /
                          frequency_.QuadPart / submitted);
    const double readback_latency_frames_avg =
        submitted == 0 ? 0.0
                       : static_cast<double>(readback_latency_frames_total_) /
                             static_cast<double>(submitted);
    const double timestamp_delta_ms =
        timestamp_delta_window_samples_ == 0
            ? 0.0
            : static_cast<double>(timestamp_delta_window_us_total_) / 1000.0 /
                  static_cast<double>(timestamp_delta_window_samples_);
    const double timestamp_delta_max_ms =
        static_cast<double>(timestamp_delta_window_us_max_) / 1000.0;
    const double timestamp_delta_total_ms =
        timestamp_delta_samples_ == 0
            ? 0.0
            : static_cast<double>(timestamp_delta_us_total_) / 1000.0 /
                  static_cast<double>(timestamp_delta_samples_);
    const double timestamp_delta_total_max_ms =
        static_cast<double>(timestamp_delta_us_max_) / 1000.0;
    const double delivery_wall_delta_ms =
        delivery_wall_delta_window_samples_ == 0
            ? 0.0
            : static_cast<double>(delivery_wall_delta_window_us_total_) /
                  1000.0 /
                  static_cast<double>(delivery_wall_delta_window_samples_);
    const double delivery_wall_delta_max_ms =
        static_cast<double>(delivery_wall_delta_window_us_max_) / 1000.0;
    const double delivery_wall_delta_min_ms =
        static_cast<double>(delivery_wall_delta_window_us_min_) / 1000.0;
    const double delivery_submit_prep_ms = AverageMetricMs(
        delivery_submit_prep_qpc_, delivery_submit_prep_samples_);
    const double delivery_submit_prep_max_ms =
        TicksToMs(delivery_submit_prep_max_qpc_);
    const double delivery_on_frame_call_ms = AverageMetricMs(
        delivery_on_frame_call_qpc_, delivery_on_frame_call_samples_);
    const double delivery_on_frame_call_max_ms =
        TicksToMs(delivery_on_frame_call_max_qpc_);
    const double delivery_post_on_frame_ms = AverageMetricMs(
        delivery_post_on_frame_qpc_, delivery_post_on_frame_samples_);
    const double delivery_post_on_frame_max_ms =
        TicksToMs(delivery_post_on_frame_max_qpc_);
    const double native_buffer_release_ms = AverageMetricMs(
        native_buffer_release_qpc_, native_buffer_release_samples_);
    const double native_buffer_release_max_ms =
        TicksToMs(native_buffer_release_max_qpc_);
    const double ready_to_queue_ms =
        AverageMetricMs(ready_to_queue_qpc_, ready_to_queue_samples_);
    const double ready_to_queue_max_ms = TicksToMs(ready_to_queue_max_qpc_);
    const double delivery_queue_wait_ms =
        AverageMetricMs(delivery_queue_wait_qpc_, delivery_queue_wait_samples_);
    const double delivery_queue_wait_max_ms =
        TicksToMs(delivery_queue_wait_max_qpc_);
    const double delivery_repeat_source_age_ms = AverageMetricMs(
        delivery_repeat_source_age_qpc_, delivery_repeat_source_age_samples_);
    const double delivery_repeat_source_age_max_ms =
        TicksToMs(delivery_repeat_source_age_max_qpc_);
    const double delivery_overwrite_age_ms = AverageMetricMs(
        delivery_overwrite_age_qpc_, delivery_overwrite_age_samples_);
    const double delivery_overwrite_age_max_ms =
        TicksToMs(delivery_overwrite_age_max_qpc_);
    const double ready_to_submit_ms =
        AverageMetricMs(ready_to_submit_qpc_, ready_to_submit_samples_);
    const double ready_to_submit_max_ms = TicksToMs(ready_to_submit_max_qpc_);
    const double source_to_submit_ms =
        AverageMetricMs(source_to_submit_qpc_, source_to_submit_samples_);
    const double source_to_submit_max_ms = TicksToMs(source_to_submit_max_qpc_);
    const double source_to_readback_ready_ms = AverageMetricMs(
        source_to_readback_ready_qpc_, source_to_readback_ready_samples_);
    const double source_to_readback_ready_max_ms =
        TicksToMs(source_to_readback_ready_max_qpc_);
    const double readback_queue_to_map_ms = AverageMetricMs(
        readback_queue_to_map_qpc_, readback_queue_to_map_samples_);
    const double readback_queue_to_map_max_ms =
        TicksToMs(readback_queue_to_map_max_qpc_);
    const double map_to_i420_ms =
        AverageMetricMs(map_to_i420_qpc_, map_to_i420_samples_);
    const double map_to_i420_max_ms = TicksToMs(map_to_i420_max_qpc_);
    const double source_to_i420_ready_ms = AverageMetricMs(
        source_to_i420_ready_qpc_, source_to_i420_ready_samples_);
    const double source_to_i420_ready_max_ms =
        TicksToMs(source_to_i420_ready_max_qpc_);
    const double source_to_queue_ms =
        AverageMetricMs(source_to_queue_qpc_, source_to_queue_samples_);
    const double source_to_queue_max_ms = TicksToMs(source_to_queue_max_qpc_);
    const double source_duplicate_skip_age_ms = AverageMetricMs(
        source_duplicate_skip_age_qpc_, source_duplicate_skip_age_samples_);
    const double source_duplicate_skip_age_max_ms =
        TicksToMs(source_duplicate_skip_age_max_qpc_);
    const double source_qpc_delta_ms =
        source_qpc_delta_samples_ == 0
            ? 0.0
            : static_cast<double>(source_qpc_delta_us_total_) / 1000.0 /
                  static_cast<double>(source_qpc_delta_samples_);
    const double source_qpc_delta_max_ms =
        static_cast<double>(source_qpc_delta_us_max_) / 1000.0;
    const double source_latest_qpc_delta_ms = AverageTicksMs(
        source_latest_qpc_delta_, source_latest_qpc_delta_samples_);
    const double source_latest_qpc_delta_max_ms =
        TicksToMs(source_latest_qpc_delta_max_);
    const double source_latest_observation_delta_ms =
        AverageTicksMs(source_latest_observation_delta_,
                       source_latest_observation_delta_samples_);
    const double source_latest_observation_delta_max_ms =
        TicksToMs(source_latest_observation_delta_max_);
    const double source_latest_event_age_ms = AverageTicksMs(
        source_latest_event_age_, source_latest_event_age_samples_);
    const double source_latest_event_age_max_ms =
        TicksToMs(source_latest_event_age_max_);
    const double source_publish_observation_age_ms =
        AverageTicksMs(source_publish_observation_age_,
                       source_publish_observation_age_samples_);
    const double source_publish_observation_age_max_ms =
        TicksToMs(source_publish_observation_age_max_);
    const double producer_present_gap_ms =
        shared_state_ == nullptr
            ? 0.0
            : AverageSharedTicksMs(
                  shared_state_->producer_present_gap_qpc_total,
                  shared_state_->producer_present_gap_samples);
    const double producer_present_gap_max_ms =
        shared_state_ == nullptr
            ? 0.0
            : SharedTicksToMs(shared_state_->producer_present_gap_qpc_max);
    const double producer_capture_gap_ms =
        shared_state_ == nullptr
            ? 0.0
            : AverageSharedTicksMs(
                  shared_state_->producer_capture_gap_qpc_total,
                  shared_state_->producer_capture_gap_samples);
    const double producer_capture_gap_max_ms =
        shared_state_ == nullptr
            ? 0.0
            : SharedTicksToMs(shared_state_->producer_capture_gap_qpc_max);
    const double producer_present_to_publish_ms =
        shared_state_ == nullptr
            ? 0.0
            : AverageSharedTicksMs(
                  shared_state_->producer_present_to_publish_qpc_total,
                  shared_state_->producer_present_to_publish_samples);
    const double producer_present_to_publish_max_ms =
        shared_state_ == nullptr
            ? 0.0
            : SharedTicksToMs(
                  shared_state_->producer_present_to_publish_qpc_max);
    const double producer_copy_ms =
        shared_state_ == nullptr
            ? 0.0
            : AverageSharedTicksMs(shared_state_->producer_copy_qpc_total,
                                   shared_state_->producer_copy_samples);
    const double producer_copy_max_ms =
        shared_state_ == nullptr
            ? 0.0
            : SharedTicksToMs(shared_state_->producer_copy_qpc_max);
    const double producer_resolve_ms =
        shared_state_ == nullptr
            ? 0.0
            : AverageSharedTicksMs(shared_state_->producer_resolve_qpc_total,
                                   shared_state_->producer_resolve_samples);
    const double producer_resolve_max_ms =
        shared_state_ == nullptr
            ? 0.0
            : SharedTicksToMs(shared_state_->producer_resolve_qpc_max);
    const double native_admission_deadline_lateness_ms =
        AverageMetricMs(native_admission_deadline_lateness_qpc_,
                        native_admission_deadline_lateness_samples_);
    const double native_admission_deadline_lateness_max_ms =
        TicksToMs(native_admission_deadline_lateness_max_qpc_);
    const double native_nv12_convert_ms =
        AverageMetricMs(gpu_nv12_convert_qpc_, gpu_nv12_convert_samples_);
    const double native_nv12_convert_max_ms =
        TicksToMs(gpu_nv12_convert_max_qpc_);
    const double native_nv12_bgra_scale_draw_ms = AverageMetricMs(
        gpu_nv12_bgra_scale_draw_qpc_, gpu_nv12_bgra_scale_draw_samples_);
    const double native_nv12_bgra_scale_draw_max_ms =
        TicksToMs(gpu_nv12_bgra_scale_draw_max_qpc_);
    const double native_nv12_blt_submit_ms =
        AverageMetricMs(gpu_nv12_blt_submit_qpc_, gpu_nv12_blt_submit_samples_);
    const double native_nv12_blt_submit_max_ms =
        TicksToMs(gpu_nv12_blt_submit_max_qpc_);
    const double native_nv12_blt_to_ready_ms = AverageMetricMs(
        gpu_nv12_blt_to_ready_qpc_, gpu_nv12_blt_to_ready_samples_);
    const double native_nv12_blt_to_ready_max_ms =
        TicksToMs(gpu_nv12_blt_to_ready_max_qpc_);
    const double native_nv12_blt_gpu_execution_ms = AverageMetricMs(
        gpu_nv12_blt_gpu_execution_ms_, gpu_nv12_blt_gpu_execution_samples_);
    const double native_nv12_blt_gpu_execution_max_ms =
        gpu_nv12_blt_gpu_execution_max_ms_;
    const double native_nv12_blt_gpu_queue_delay_ms =
        AverageMetricMs(gpu_nv12_blt_gpu_queue_delay_ms_,
                        gpu_nv12_blt_gpu_queue_delay_samples_);
    const double native_nv12_blt_gpu_queue_delay_max_ms =
        gpu_nv12_blt_gpu_queue_delay_max_ms_;
    const double native_nv12_buffer_create_ms = AverageMetricMs(
        gpu_nv12_buffer_create_qpc_, gpu_nv12_buffer_create_samples_);
    const double native_nv12_buffer_create_max_ms =
        TicksToMs(gpu_nv12_buffer_create_max_qpc_);
    const double native_nv12_frame_ready_to_queue_ms =
        AverageMetricMs(gpu_nv12_frame_ready_to_queue_qpc_,
                        gpu_nv12_frame_ready_to_queue_samples_);
    const double native_nv12_frame_ready_to_queue_max_ms =
        TicksToMs(gpu_nv12_frame_ready_to_queue_max_qpc_);
    const double native_nv12_overwrite_age_ms = AverageMetricMs(
        gpu_nv12_overwrite_age_qpc_, gpu_nv12_overwrite_age_samples_);
    const double native_nv12_overwrite_age_max_ms =
        TicksToMs(gpu_nv12_overwrite_age_max_qpc_);
    const double native_nv12_ready_drop_age_ms = AverageMetricMs(
        gpu_nv12_ready_drop_age_qpc_, gpu_nv12_ready_drop_age_samples_);
    const double native_nv12_ready_drop_age_max_ms =
        TicksToMs(gpu_nv12_ready_drop_age_max_qpc_);
    const double native_nv12_late_ready_drop_age_ms =
        AverageMetricMs(gpu_nv12_late_ready_drop_age_qpc_,
                        gpu_nv12_late_ready_drop_age_samples_);
    const double native_nv12_late_ready_drop_age_max_ms =
        TicksToMs(gpu_nv12_late_ready_drop_age_max_qpc_);
    const double native_nv12_late_ready_drop_blt_to_ready_ms =
        AverageMetricMs(gpu_nv12_late_ready_drop_blt_to_ready_qpc_,
                        gpu_nv12_late_ready_drop_blt_to_ready_samples_);
    const double native_nv12_late_ready_drop_blt_to_ready_max_ms =
        TicksToMs(gpu_nv12_late_ready_drop_blt_to_ready_max_qpc_);
    const double native_nv12_conversion_start_age_ms =
        AverageMetricMs(gpu_nv12_conversion_start_age_qpc_,
                        gpu_nv12_conversion_start_age_samples_);
    const double native_nv12_conversion_start_age_max_ms =
        TicksToMs(gpu_nv12_conversion_start_age_max_qpc_);
    const double native_nv12_gpu_queue_backoff_ms =
        AverageMetricMs(native_nv12_gpu_queue_backoff_duration_qpc_,
                        native_nv12_gpu_queue_backoff_duration_samples_);
    const double native_nv12_gpu_queue_backoff_max_ms =
        TicksToMs(native_nv12_gpu_queue_backoff_duration_max_qpc_);
    const double native_nv12_gpu_queue_backoff_trigger_blt_to_ready_ms =
        AverageMetricMs(
            native_nv12_gpu_queue_backoff_trigger_blt_to_ready_qpc_,
            native_nv12_gpu_queue_backoff_trigger_blt_to_ready_samples_);
    const double native_nv12_gpu_queue_backoff_trigger_blt_to_ready_max_ms =
        TicksToMs(native_nv12_gpu_queue_backoff_trigger_blt_to_ready_max_qpc_);
    const double native_nv12_gpu_queue_backoff_suppressed_source_age_ms =
        AverageMetricMs(
            native_nv12_gpu_queue_backoff_suppressed_source_age_qpc_,
            native_nv12_gpu_queue_backoff_suppressed_source_age_samples_);
    const double native_nv12_gpu_queue_backoff_suppressed_source_age_max_ms =
        TicksToMs(native_nv12_gpu_queue_backoff_suppressed_source_age_max_qpc_);
    const double native_nv12_owned_copy_ms =
        AverageMetricMs(gpu_nv12_owned_copy_qpc_, gpu_nv12_owned_copy_samples_);
    const double native_nv12_owned_copy_max_ms =
        TicksToMs(gpu_nv12_owned_copy_max_qpc_);
    const uint32_t backend_contract_version =
        shared_state_ ? shared_state_->version : 0;
    const auto source_api =
        shared_state_ ? static_cast<CaptureBackend>(shared_state_->source_api)
                      : CaptureBackend::kUnknown;
    const auto source_format =
        shared_state_ && shared_state_->source_format != 0
            ? static_cast<SourceFormat>(shared_state_->source_format)
            : SourceFormatFromDxgiFormat(static_cast<uint32_t>(format));
    const auto color_space =
        shared_state_ ? static_cast<ColorSpace>(shared_state_->color_space)
                      : ColorSpace::kUnknown;
    const auto sync_kind = shared_state_
                               ? static_cast<SyncKind>(shared_state_->sync_kind)
                               : SyncKind::kUnknown;
    const auto ready_state =
        shared_state_ ? static_cast<FrameReadyState>(shared_state_->ready_state)
                      : FrameReadyState::kUnknown;
    const auto failure_reason =
        shared_state_
            ? static_cast<FailureReason>(shared_state_->failure_reason)
            : FailureReason::kNone;
    Log("stats source=" + std::to_string(source_width) + "x" +
        std::to_string(source_height) +
        " output=" + std::to_string(last_output_width_) + "x" +
        std::to_string(last_output_height_) +
        " format=" + std::to_string(format) +
        " sourceMode=" + GameCaptureSourceModeName(source_mode_) +
        " fps=" + std::to_string(fps) +
        " backendContractVersion=" + std::to_string(backend_contract_version) +
        " sourceApi=" + CaptureBackendName(source_api) +
        " sourceApiId=" + std::to_string(static_cast<uint32_t>(source_api)) +
        " sourceFormat=" + SourceFormatName(source_format) +
        " sourceFormatId=" +
        std::to_string(static_cast<uint32_t>(source_format)) + " colorSpace=" +
        ColorSpaceName(color_space) + " syncKind=" + SyncKindName(sync_kind) +
        " readyState=" + FrameReadyStateName(ready_state) +
        " failureReason=" + FailureReasonName(failure_reason) +
        " nativeAdmissionStrictDeadlineEnabled=" +
        std::string(native_admission_strict_deadline_enabled_ ? "true"
                                                              : "false") +
        " nativeAdmissionSourceDrivenFreshDue=" +
        std::to_string(native_admission_source_driven_fresh_due_frames_) +
        " nativeAdmissionSourceQpcDue=" +
        std::to_string(native_admission_source_qpc_due_frames_) +
        " nativeAdmissionEarlySourceDueSuppressed=" +
        std::to_string(native_admission_early_source_due_suppressed_frames_) +
        " nativeAdmissionDeadlineDue=" +
        std::to_string(native_admission_deadline_due_frames_) +
        " nativeAdmissionDeadlineLatenessMs=" +
        std::to_string(native_admission_deadline_lateness_ms) +
        " nativeAdmissionDeadlineLatenessMaxMs=" +
        std::to_string(native_admission_deadline_lateness_max_ms) +
        " nativeAdmissionDeadlineLatenessSamples=" +
        std::to_string(native_admission_deadline_lateness_samples_) +
        " nativeAdmissionDeadlineOver1x=" +
        std::to_string(native_admission_deadline_over_1x_frames_) +
        " nativeAdmissionDeadlineOver2x=" +
        std::to_string(native_admission_deadline_over_2x_frames_) +
        " nativeAdmissionDeadlineOver3x=" +
        std::to_string(native_admission_deadline_over_3x_frames_) +
        " nativeAdmissionNoSourceOnDeadline=" +
        std::to_string(native_admission_no_source_on_deadline_frames_) +
        " nativeAdmissionRepeatedOnDeadline=" +
        std::to_string(native_admission_repeated_on_deadline_frames_) +
        " nativeAdmissionSubmitOnDeadline=" +
        std::to_string(native_admission_submit_on_deadline_frames_) +
        " nativeAdmissionSubmitOnEarlySource=" +
        std::to_string(native_admission_submit_on_early_source_frames_) +
        " nativeNv12PendingOnDeadline=" +
        std::to_string(native_nv12_pending_on_deadline_frames_) +
        " nativeNv12NoPendingOnDeadline=" +
        std::to_string(native_nv12_no_pending_on_deadline_frames_) +
        " nativeNv12ReadyOnDeadline=" +
        std::to_string(native_nv12_ready_on_deadline_frames_) +
        " nativeNv12NoReadyOnDeadline=" +
        std::to_string(native_nv12_no_ready_on_deadline_frames_) +
        " consumerAdapterLuid=" + consumer_adapter_diagnostics_.luid +
        " consumerAdapterVendorId=" +
        std::to_string(consumer_adapter_diagnostics_.vendor_id) +
        " consumerAdapterDeviceId=" +
        std::to_string(consumer_adapter_diagnostics_.device_id) +
        " consumerGpuThreadPriorityRequested=" +
        std::string(consumer_gpu_thread_priority_requested_ ? "true"
                                                            : "false") +
        " consumerGpuThreadPriorityRequestedValue=" +
        std::to_string(consumer_gpu_thread_priority_requested_value_) +
        " consumerGpuThreadPriorityApplied=" +
        std::string(consumer_gpu_thread_priority_applied_ ? "true" : "false") +
        " consumerGpuThreadPriorityBefore=" +
        std::to_string(consumer_gpu_thread_priority_before_) +
        " consumerGpuThreadPriorityAfter=" +
        std::to_string(consumer_gpu_thread_priority_after_) +
        " consumerGpuThreadPriorityHr=" +
        HResultHex(consumer_gpu_thread_priority_hr_) +
        " sourceAdapterLuid=unknown crossAdapterSuspected=unknown" +
        " submitted=" + std::to_string(submitted_frames_) +
        " repeated=" + std::to_string(repeated_frames_) +
        " duplicateSkipped=" + std::to_string(duplicate_source_frame_skips_) +
        " deliveryQueued=" + std::to_string(delivery_queued_frames_) +
        " deliverySubmitted=" + std::to_string(delivery_submitted_frames_) +
        " deliveryOverwritten=" + std::to_string(delivery_overwritten_frames_) +
        " deliveryPacerResyncs=" + std::to_string(delivery_pacer_resyncs_) +
        " deliveryPacerLagMaxMs=" + std::to_string(delivery_pacer_lag_ms_max_) +
        " deliveryRepeatNoQueued=" +
        std::to_string(delivery_repeat_no_queued_frames_) +
        " deliverySkipNoQueued=" +
        std::to_string(delivery_skip_no_queued_frames_) +
        " deliveryFreshWakeAfterSkip=" +
        std::to_string(delivery_fresh_wake_after_skip_frames_) +
        " deliveryFreshImmediate=" +
        std::to_string(delivery_fresh_immediate_frames_) +
        " deliveryRepeatPolicy=" +
        DeliveryRepeatPolicyName(delivery_repeat_policy_) +
        " deliveryQueueDepth=" + std::to_string(delivery_queue_depth_) +
        " nativeNv12ReadyPolicy=" +
        NativeNv12ReadyPolicyName(native_nv12_ready_policy_) +
        " nativeNv12FenceAvailable=" +
        std::string(native_nv12_fence_available_ ? "true" : "false") +
        " nativeNv12PendingPollMs=" +
        std::to_string(native_nv12_pending_poll_ms_) +
        " nativeNv12MaxPendingSlots=" +
        std::to_string(native_nv12_max_pending_slots_) +
        " nativeNv12ReadyDrainDepth=" +
        std::to_string(native_nv12_ready_drain_depth_) +
        " nativeNv12FrameOwnership=" +
        std::string(NativeNv12FrameOwnershipName()) +
        " nativeNv12OnFrameBackpressureEnabled=" +
        std::string(native_nv12_onframe_backpressure_suspend_enabled_
                        ? "true"
                        : "false") +
        " nativeNv12OnFrameBackpressureThresholdMs=" +
        std::to_string(native_nv12_onframe_backpressure_threshold_ms_) +
        " nativeNv12OnFrameBackpressureFrameLimit=" +
        std::to_string(native_nv12_onframe_backpressure_frame_limit_) +
        " nativeNv12SingleInFlightEnabled=" +
        std::string(native_nv12_single_in_flight_enabled_ ? "true" : "false") +
        " nativeNv12GpuQueueBackoffEnabled=" +
        std::string(native_nv12_gpu_queue_backoff_enabled_ ? "true" : "false") +
        " nativeNv12GpuQueueBackoffThresholdFrames=" +
        std::to_string(native_nv12_gpu_queue_backoff_threshold_frames_) +
        " nativeNv12GpuQueueBackoffDurationFrames=" +
        std::to_string(native_nv12_gpu_queue_backoff_duration_frames_) +
        " nativeNv12LateReadyDropEnabled=" +
        std::string(native_nv12_drop_late_ready_enabled_ ? "true" : "false") +
        " nativeNv12LateReadyDropThresholdMs=" +
        std::to_string(native_nv12_late_ready_drop_threshold_ms_) +
        " nativeNv12AdmissionMaxSourceAgeMs=" +
        std::to_string(native_nv12_admission_max_source_age_ms_) +
        " nativeNv12WarmupI420Frames=" +
        std::to_string(native_nv12_warmup_i420_frames_) +
        " deliveryRepeatSourceAgeMs=" +
        std::to_string(delivery_repeat_source_age_ms) +
        " deliveryRepeatSourceAgeMaxMs=" +
        std::to_string(delivery_repeat_source_age_max_ms) +
        " deliveryRepeatSourceAgeSamples=" +
        std::to_string(delivery_repeat_source_age_samples_) +
        " deliveryOnFrameMs=" +
        std::to_string(AverageTicksMs(delivery_on_frame_qpc_,
                                      delivery_submitted_frames_)) +
        " deliveryOnFrameMaxMs=" +
        std::to_string(TicksToMs(delivery_on_frame_max_qpc_)) +
        " deliverySubmitPrepMs=" + std::to_string(delivery_submit_prep_ms) +
        " deliverySubmitPrepMaxMs=" +
        std::to_string(delivery_submit_prep_max_ms) +
        " deliverySubmitPrepSamples=" +
        std::to_string(delivery_submit_prep_samples_) +
        " deliveryOnFrameCallMs=" + std::to_string(delivery_on_frame_call_ms) +
        " deliveryOnFrameCallMaxMs=" +
        std::to_string(delivery_on_frame_call_max_ms) +
        " deliveryOnFrameCallSamples=" +
        std::to_string(delivery_on_frame_call_samples_) +
        " deliveryPostOnFrameMs=" + std::to_string(delivery_post_on_frame_ms) +
        " deliveryPostOnFrameMaxMs=" +
        std::to_string(delivery_post_on_frame_max_ms) +
        " deliveryPostOnFrameSamples=" +
        std::to_string(delivery_post_on_frame_samples_) +
        " nativeBufferReleaseMs=" + std::to_string(native_buffer_release_ms) +
        " nativeBufferReleaseMaxMs=" +
        std::to_string(native_buffer_release_max_ms) +
        " nativeBufferReleaseSamples=" +
        std::to_string(native_buffer_release_samples_) +
        " readyToQueueMs=" + std::to_string(ready_to_queue_ms) +
        " readyToQueueMaxMs=" + std::to_string(ready_to_queue_max_ms) +
        " readyToQueueSamples=" + std::to_string(ready_to_queue_samples_) +
        " deliveryQueueWaitMs=" + std::to_string(delivery_queue_wait_ms) +
        " deliveryQueueWaitMaxMs=" +
        std::to_string(delivery_queue_wait_max_ms) +
        " deliveryQueueWaitSamples=" +
        std::to_string(delivery_queue_wait_samples_) +
        " deliveryOverwriteAgeMs=" + std::to_string(delivery_overwrite_age_ms) +
        " deliveryOverwriteAgeMaxMs=" +
        std::to_string(delivery_overwrite_age_max_ms) +
        " deliveryOverwriteAgeSamples=" +
        std::to_string(delivery_overwrite_age_samples_) +
        " deliveryOverwrittenFresh=" +
        std::to_string(delivery_overwritten_fresh_frames_) +
        " readyToSubmitMs=" + std::to_string(ready_to_submit_ms) +
        " readyToSubmitMaxMs=" + std::to_string(ready_to_submit_max_ms) +
        " readyToSubmitSamples=" + std::to_string(ready_to_submit_samples_) +
        " sourceToSubmitMs=" + std::to_string(source_to_submit_ms) +
        " sourceToSubmitMaxMs=" + std::to_string(source_to_submit_max_ms) +
        " sourceToSubmitSamples=" + std::to_string(source_to_submit_samples_) +
        " sourceToReadbackReadyMs=" +
        std::to_string(source_to_readback_ready_ms) +
        " sourceToReadbackReadyMaxMs=" +
        std::to_string(source_to_readback_ready_max_ms) +
        " sourceToReadbackReadySamples=" +
        std::to_string(source_to_readback_ready_samples_) +
        " readbackQueueToMapMs=" + std::to_string(readback_queue_to_map_ms) +
        " readbackQueueToMapMaxMs=" +
        std::to_string(readback_queue_to_map_max_ms) +
        " readbackQueueToMapSamples=" +
        std::to_string(readback_queue_to_map_samples_) +
        " mapToI420Ms=" + std::to_string(map_to_i420_ms) +
        " mapToI420MaxMs=" + std::to_string(map_to_i420_max_ms) +
        " mapToI420Samples=" + std::to_string(map_to_i420_samples_) +
        " sourceToI420ReadyMs=" + std::to_string(source_to_i420_ready_ms) +
        " sourceToI420ReadyMaxMs=" +
        std::to_string(source_to_i420_ready_max_ms) +
        " sourceToI420ReadySamples=" +
        std::to_string(source_to_i420_ready_samples_) +
        " sourceToQueueMs=" + std::to_string(source_to_queue_ms) +
        " sourceToQueueMaxMs=" + std::to_string(source_to_queue_max_ms) +
        " sourceToQueueSamples=" + std::to_string(source_to_queue_samples_) +
        " sourceDuplicateSkipAgeMs=" +
        std::to_string(source_duplicate_skip_age_ms) +
        " sourceDuplicateSkipAgeMaxMs=" +
        std::to_string(source_duplicate_skip_age_max_ms) +
        " sourceDuplicateSkipAgeSamples=" +
        std::to_string(source_duplicate_skip_age_samples_) + " copied=" +
        std::to_string(shared_state_ ? shared_state_->copied_frames : 0) +
        " dropped=" +
        std::to_string(shared_state_ ? shared_state_->dropped_frames : 0) +
        " overwritten=" +
        std::to_string(shared_state_ ? shared_state_->overwritten_frames : 0) +
        " gpuScaled=" + std::to_string(gpu_scaled_frames_) +
        " gpuScaleFailures=" + std::to_string(gpu_scale_failures_) +
        " nativeNv12Submitted=" + std::to_string(gpu_nv12_submitted_frames_) +
        " nativeNv12Failures=" + std::to_string(gpu_nv12_failures_) +
        " nativeNv12Queued=" + std::to_string(gpu_nv12_queued_frames_) +
        " nativeNv12Ready=" + std::to_string(gpu_nv12_ready_frames_) +
        " nativeNv12NotReadyPolls=" +
        std::to_string(gpu_nv12_not_ready_polls_) +
        " nativeNv12FenceSignaled=" +
        std::to_string(gpu_nv12_fence_signaled_frames_) +
        " nativeNv12FenceReady=" +
        std::to_string(gpu_nv12_fence_ready_frames_) +
        " nativeNv12FenceSignalFailures=" +
        std::to_string(gpu_nv12_fence_signal_failures_) +
        " nativeNv12OwnedCopies=" +
        std::to_string(gpu_nv12_owned_copy_frames_) +
        " nativeNv12OwnedCopyMs=" + std::to_string(native_nv12_owned_copy_ms) +
        " nativeNv12OwnedCopyMaxMs=" +
        std::to_string(native_nv12_owned_copy_max_ms) +
        " nativeNv12OwnedCopySamples=" +
        std::to_string(gpu_nv12_owned_copy_samples_) +
        " nativeNv12Overwritten=" +
        std::to_string(gpu_nv12_overwritten_frames_) +
        " nativeNv12OverwriteAgeMs=" +
        std::to_string(native_nv12_overwrite_age_ms) +
        " nativeNv12OverwriteAgeMaxMs=" +
        std::to_string(native_nv12_overwrite_age_max_ms) +
        " nativeNv12OverwriteAgeSamples=" +
        std::to_string(gpu_nv12_overwrite_age_samples_) +
        " nativeNv12OverwrittenFresh=" +
        std::to_string(gpu_nv12_overwritten_fresh_frames_) +
        " nativeNv12ReadyDropped=" +
        std::to_string(gpu_nv12_ready_dropped_frames_) +
        " nativeNv12ReadyDropAgeMs=" +
        std::to_string(native_nv12_ready_drop_age_ms) +
        " nativeNv12ReadyDropAgeMaxMs=" +
        std::to_string(native_nv12_ready_drop_age_max_ms) +
        " nativeNv12ReadyDropAgeSamples=" +
        std::to_string(gpu_nv12_ready_drop_age_samples_) +
        " nativeNv12ReadyDroppedFresh=" +
        std::to_string(gpu_nv12_ready_dropped_fresh_frames_) +
        " nativeNv12LateReadyDropped=" +
        std::to_string(gpu_nv12_late_ready_dropped_frames_) +
        " nativeNv12LateReadyDropAgeMs=" +
        std::to_string(native_nv12_late_ready_drop_age_ms) +
        " nativeNv12LateReadyDropAgeMaxMs=" +
        std::to_string(native_nv12_late_ready_drop_age_max_ms) +
        " nativeNv12LateReadyDropAgeSamples=" +
        std::to_string(gpu_nv12_late_ready_drop_age_samples_) +
        " nativeNv12LateReadyDroppedFresh=" +
        std::to_string(gpu_nv12_late_ready_dropped_fresh_frames_) +
        " nativeNv12LateReadyDropBltToReadyMs=" +
        std::to_string(native_nv12_late_ready_drop_blt_to_ready_ms) +
        " nativeNv12LateReadyDropBltToReadyMaxMs=" +
        std::to_string(native_nv12_late_ready_drop_blt_to_ready_max_ms) +
        " nativeNv12LateReadyDropBltToReadySamples=" +
        std::to_string(gpu_nv12_late_ready_drop_blt_to_ready_samples_) +
        " nativeNv12ConversionStartAgeMs=" +
        std::to_string(native_nv12_conversion_start_age_ms) +
        " nativeNv12ConversionStartAgeMaxMs=" +
        std::to_string(native_nv12_conversion_start_age_max_ms) +
        " nativeNv12ConversionStartAgeSamples=" +
        std::to_string(gpu_nv12_conversion_start_age_samples_) +
        " nativeNv12SingleInFlightDeferred=" +
        std::to_string(native_nv12_single_in_flight_deferred_frames_) +
        " nativeNv12SingleInFlightDeferredFresh=" +
        std::to_string(native_nv12_single_in_flight_deferred_fresh_frames_) +
        " nativeNv12SingleInFlightPendingMax=" +
        std::to_string(native_nv12_single_in_flight_pending_max_) +
        " nativeNv12SingleInFlightDeferredSourceAgeMs=" +
        std::to_string(AverageMetricMs(
            native_nv12_single_in_flight_deferred_source_age_qpc_,
            native_nv12_single_in_flight_deferred_source_age_samples_)) +
        " nativeNv12SingleInFlightDeferredSourceAgeMaxMs=" +
        std::to_string(TicksToMs(
            native_nv12_single_in_flight_deferred_source_age_max_qpc_)) +
        " nativeNv12SingleInFlightDeferredSourceAgeSamples=" +
        std::to_string(
            native_nv12_single_in_flight_deferred_source_age_samples_) +
        " nativeNv12AdmissionMailboxEnabled=" +
        std::string(native_nv12_admission_mailbox_enabled_ ? "true"
                                                           : "false") +
        " nativeNv12GpuQueueBackoffTriggered=" +
        std::to_string(native_nv12_gpu_queue_backoff_triggered_frames_) +
        " nativeNv12GpuQueueBackoffSuppressed=" +
        std::to_string(native_nv12_gpu_queue_backoff_suppressed_frames_) +
        " nativeNv12GpuQueueBackoffSuppressedFresh=" +
        std::to_string(native_nv12_gpu_queue_backoff_suppressed_fresh_frames_) +
        " nativeNv12GpuQueueBackoffMs=" +
        std::to_string(native_nv12_gpu_queue_backoff_ms) +
        " nativeNv12GpuQueueBackoffMaxMs=" +
        std::to_string(native_nv12_gpu_queue_backoff_max_ms) +
        " nativeNv12GpuQueueBackoffSamples=" +
        std::to_string(native_nv12_gpu_queue_backoff_duration_samples_) +
        " nativeNv12GpuQueueBackoffTriggerBltToReadyMs=" +
        std::to_string(native_nv12_gpu_queue_backoff_trigger_blt_to_ready_ms) +
        " nativeNv12GpuQueueBackoffTriggerBltToReadyMaxMs=" +
        std::to_string(
            native_nv12_gpu_queue_backoff_trigger_blt_to_ready_max_ms) +
        " nativeNv12GpuQueueBackoffTriggerBltToReadySamples=" +
        std::to_string(
            native_nv12_gpu_queue_backoff_trigger_blt_to_ready_samples_) +
        " nativeNv12GpuQueueBackoffSuppressedSourceAgeMs=" +
        std::to_string(native_nv12_gpu_queue_backoff_suppressed_source_age_ms) +
        " nativeNv12GpuQueueBackoffSuppressedSourceAgeMaxMs=" +
        std::to_string(
            native_nv12_gpu_queue_backoff_suppressed_source_age_max_ms) +
        " nativeNv12GpuQueueBackoffSuppressedSourceAgeSamples=" +
        std::to_string(
            native_nv12_gpu_queue_backoff_suppressed_source_age_samples_) +
        " nativeNv12AdmissionMailboxPendingActive=" +
        std::string(HasPendingNativeNv12Admission() ? "true" : "false") +
        " nativeNv12AdmissionMailboxStored=" +
        std::to_string(native_nv12_admission_mailbox_stored_frames_) +
        " nativeNv12AdmissionMailboxReplaced=" +
        std::to_string(native_nv12_admission_mailbox_replaced_frames_) +
        " nativeNv12AdmissionMailboxSubmitted=" +
        std::to_string(native_nv12_admission_mailbox_submitted_frames_) +
        " nativeNv12AdmissionMailboxStaleDropped=" +
        std::to_string(native_nv12_admission_mailbox_stale_dropped_frames_) +
        " nativeNv12AdmissionMailboxPendingAgeMs=" +
        std::to_string(AverageMetricMs(
            native_nv12_admission_mailbox_pending_age_qpc_,
            native_nv12_admission_mailbox_pending_age_samples_)) +
        " nativeNv12AdmissionMailboxPendingAgeMaxMs=" +
        std::to_string(
            TicksToMs(native_nv12_admission_mailbox_pending_age_max_qpc_)) +
        " nativeNv12AdmissionMailboxPendingAgeSamples=" +
        std::to_string(native_nv12_admission_mailbox_pending_age_samples_) +
        " nativeNv12AdmissionMailboxSubmitSourceAgeMs=" +
        std::to_string(AverageMetricMs(
            native_nv12_admission_mailbox_submit_source_age_qpc_,
            native_nv12_admission_mailbox_submit_source_age_samples_)) +
        " nativeNv12AdmissionMailboxSubmitSourceAgeMaxMs=" +
        std::to_string(TicksToMs(
            native_nv12_admission_mailbox_submit_source_age_max_qpc_)) +
        " nativeNv12AdmissionMailboxSubmitSourceAgeSamples=" +
        std::to_string(
            native_nv12_admission_mailbox_submit_source_age_samples_) +
        " nativeNv12ConvertMs=" + std::to_string(native_nv12_convert_ms) +
        " nativeNv12ConvertMaxMs=" +
        std::to_string(native_nv12_convert_max_ms) +
        " nativeNv12ConvertSamples=" +
        std::to_string(gpu_nv12_convert_samples_) +
        " nativeNv12BgraScaleDrawMs=" +
        std::to_string(native_nv12_bgra_scale_draw_ms) +
        " nativeNv12BgraScaleDrawMaxMs=" +
        std::to_string(native_nv12_bgra_scale_draw_max_ms) +
        " nativeNv12BgraScaleDrawSamples=" +
        std::to_string(gpu_nv12_bgra_scale_draw_samples_) +
        " nativeNv12VideoProcessorBltSubmitMs=" +
        std::to_string(native_nv12_blt_submit_ms) +
        " nativeNv12VideoProcessorBltSubmitMaxMs=" +
        std::to_string(native_nv12_blt_submit_max_ms) +
        " nativeNv12VideoProcessorBltSubmitSamples=" +
        std::to_string(gpu_nv12_blt_submit_samples_) +
        " nativeNv12VideoProcessorBltCpuSubmitMs=" +
        std::to_string(native_nv12_blt_submit_ms) +
        " nativeNv12VideoProcessorBltCpuSubmitMaxMs=" +
        std::to_string(native_nv12_blt_submit_max_ms) +
        " nativeNv12VideoProcessorBltCpuSubmitSamples=" +
        std::to_string(gpu_nv12_blt_submit_samples_) +
        " nativeNv12VideoProcessorBltToReadyMs=" +
        std::to_string(native_nv12_blt_to_ready_ms) +
        " nativeNv12VideoProcessorBltToReadyMaxMs=" +
        std::to_string(native_nv12_blt_to_ready_max_ms) +
        " nativeNv12VideoProcessorBltToReadySamples=" +
        std::to_string(gpu_nv12_blt_to_ready_samples_) +
        " nativeNv12VideoProcessorBltSubmitToFenceMs=" +
        std::to_string(native_nv12_blt_to_ready_ms) +
        " nativeNv12VideoProcessorBltSubmitToFenceMaxMs=" +
        std::to_string(native_nv12_blt_to_ready_max_ms) +
        " nativeNv12VideoProcessorBltSubmitToFenceSamples=" +
        std::to_string(gpu_nv12_blt_to_ready_samples_) +
        " nativeNv12VideoProcessorBltGpuExecutionMs=" +
        std::to_string(native_nv12_blt_gpu_execution_ms) +
        " nativeNv12VideoProcessorBltGpuExecutionMaxMs=" +
        std::to_string(native_nv12_blt_gpu_execution_max_ms) +
        " nativeNv12VideoProcessorBltGpuExecutionSamples=" +
        std::to_string(gpu_nv12_blt_gpu_execution_samples_) +
        " nativeNv12VideoProcessorBltEstimatedGpuQueueDelayMs=" +
        std::to_string(native_nv12_blt_gpu_queue_delay_ms) +
        " nativeNv12VideoProcessorBltEstimatedGpuQueueDelayMaxMs=" +
        std::to_string(native_nv12_blt_gpu_queue_delay_max_ms) +
        " nativeNv12VideoProcessorBltEstimatedGpuQueueDelaySamples=" +
        std::to_string(gpu_nv12_blt_gpu_queue_delay_samples_) +
        " nativeNv12VideoProcessorBltGpuTimestampFailures=" +
        std::to_string(gpu_nv12_blt_gpu_timestamp_failures_) +
        " nativeNv12VideoProcessorBltGpuTimestampNotReady=" +
        std::to_string(gpu_nv12_blt_gpu_timestamp_not_ready_) +
        " nativeNv12VideoProcessorBltGpuTimestampDisjoint=" +
        std::to_string(gpu_nv12_blt_gpu_timestamp_disjoint_) +
        " nativeNv12ReadyObservedImmediate=" +
        std::to_string(gpu_nv12_ready_observed_immediate_after_blt_frames_) +
        " nativeNv12ReadyObservedPostFenceRegistration=" +
        std::to_string(
            gpu_nv12_ready_observed_post_fence_registration_frames_) +
        " nativeNv12ReadyObservedFenceEvent=" +
        std::to_string(gpu_nv12_ready_observed_fence_event_frames_) +
        " nativeNv12ReadyObservedSourceEvent=" +
        std::to_string(gpu_nv12_ready_observed_source_event_frames_) +
        " nativeNv12ReadyObservedWaitOther=" +
        std::to_string(gpu_nv12_ready_observed_wait_other_frames_) +
        " nativeNv12ReadyObservedLoopIdle=" +
        std::to_string(gpu_nv12_ready_observed_loop_idle_frames_) +
        " nativeNv12ReadyObservedDuplicateSkip=" +
        std::to_string(gpu_nv12_ready_observed_duplicate_skip_frames_) +
        " nativeNv12ReadyObservedPreSubmit=" +
        std::to_string(gpu_nv12_ready_observed_pre_submit_frames_) +
        " nativeNv12ReadyObservedWriteSlotScan=" +
        std::to_string(gpu_nv12_ready_observed_write_slot_scan_frames_) +
        " nativeNv12ReadyObservedUnknown=" +
        std::to_string(gpu_nv12_ready_observed_unknown_frames_) +
        " nativeNv12BltToReadyOver1x=" +
        std::to_string(gpu_nv12_blt_to_ready_over_1x_frames_) +
        " nativeNv12BltToReadyOver2x=" +
        std::to_string(gpu_nv12_blt_to_ready_over_2x_frames_) +
        " nativeNv12BltToReadyOver3x=" +
        std::to_string(gpu_nv12_blt_to_ready_over_3x_frames_) +
        " nativeNv12BufferCreateMs=" +
        std::to_string(native_nv12_buffer_create_ms) +
        " nativeNv12BufferCreateMaxMs=" +
        std::to_string(native_nv12_buffer_create_max_ms) +
        " nativeNv12BufferCreateSamples=" +
        std::to_string(gpu_nv12_buffer_create_samples_) +
        " nativeNv12FrameReadyToQueueMs=" +
        std::to_string(native_nv12_frame_ready_to_queue_ms) +
        " nativeNv12FrameReadyToQueueMaxMs=" +
        std::to_string(native_nv12_frame_ready_to_queue_max_ms) +
        " nativeNv12FrameReadyToQueueSamples=" +
        std::to_string(gpu_nv12_frame_ready_to_queue_samples_) +
        " nativeNv12StaleBeforeQueue=" +
        std::to_string(gpu_nv12_stale_before_queue_frames_) +
        " consumerDeviceRecoveries=" +
        std::to_string(consumer_device_recoveries_) +
        " nativeNv12SuspendedAfterDeviceLoss=" +
        std::string(native_nv12_suspended_after_device_loss_ ? "true"
                                                             : "false") +
        " nativeNv12SuspendedAfterOnFrameBackpressure=" +
        std::string(native_nv12_suspended_after_onframe_backpressure_.load()
                        ? "true"
                        : "false") +
        " nativeNv12OnFrameBackpressureFrames=" +
        std::to_string(native_nv12_onframe_backpressure_frames_) +
        " nativeNv12OnFrameBackpressureStreak=" +
        std::to_string(native_nv12_onframe_backpressure_streak_) +
        " nativeNv12OnFrameBackpressureMaxMs=" +
        std::to_string(native_nv12_onframe_backpressure_max_ms_) +
        " cpuFallback=" + std::to_string(cpu_fallback_frames_) +
        " readbackQueued=" + std::to_string(gpu_readback_queued_frames_) +
        " readbackReady=" + std::to_string(gpu_readback_ready_frames_) +
        " readbackNotReady=" + std::to_string(gpu_readback_not_ready_frames_) +
        " readbackOverwritten=" +
        std::to_string(gpu_readback_overwritten_frames_) +
        " readbackStaleDropped=" +
        std::to_string(gpu_readback_stale_dropped_frames_) +
        " readbackLatencyDropped=" +
        std::to_string(gpu_readback_latency_dropped_frames_) +
        " readbackMapAttempts=" + std::to_string(gpu_readback_map_attempts_) +
        " gpuScaleMs=" + std::to_string(gpu_scale_ms) + " copyMs=" +
        std::to_string(copy_ms) + " mapMs=" + std::to_string(map_ms) +
        " readbackLatencyMs=" + std::to_string(readback_latency_ms) +
        " readbackLatencyFramesAvg=" +
        std::to_string(readback_latency_frames_avg) +
        " readbackLatencyFramesMax=" +
        std::to_string(readback_latency_frames_max_) + " sourceFrameIndex=" +
        std::to_string(shared_state_ ? shared_state_->latest_frame_index : 0) +
        " lastSubmittedSourceFrameIndex=" +
        std::to_string(last_submitted_source_frame_index_) +
        " frame_id=" + std::to_string(last_submitted_source_frame_index_) +
        " source_frame_id=" +
        std::to_string(last_submitted_source_frame_index_) +
        " previous_frame_id=" +
        std::to_string(previous_submitted_source_frame_index_) +
        " source_qpc=" + std::to_string(last_observed_source_qpc_) +
        " sourceFrameRegressions=" + std::to_string(source_frame_regressions_) +
        " sourceFrameDuplicates=" +
        std::to_string(source_frame_duplicate_submissions_) +
        " sourceFrameGaps=" + std::to_string(source_frame_gaps_) +
        " sharedSlotMismatches=" +
        std::to_string(shared_slot_mismatch_frames_) + " timestampMode=" +
        std::string(TimestampModeLabel()) + " timestampSourceQpcFrames=" +
        std::to_string(timestamp_source_qpc_frames_) +
        " timestampPacedFallbackFrames=" +
        std::to_string(timestamp_paced_fallback_frames_) +
        " timestampRepeatedFrames=" +
        std::to_string(timestamp_repeated_frames_) +
        " timestampDeltaMs=" + std::to_string(timestamp_delta_ms) +
        " timestampDeltaMaxMs=" + std::to_string(timestamp_delta_max_ms) +
        " timestampSamples=" + std::to_string(timestamp_delta_window_samples_) +
        " timestampTotalDeltaMs=" + std::to_string(timestamp_delta_total_ms) +
        " timestampTotalDeltaMaxMs=" +
        std::to_string(timestamp_delta_total_max_ms) +
        " timestampTotalSamples=" + std::to_string(timestamp_delta_samples_) +
        " timestampAdjustments=" + std::to_string(timestamp_adjustments_) +
        " deliveryWallDeltaMs=" + std::to_string(delivery_wall_delta_ms) +
        " deliveryWallDeltaMaxMs=" +
        std::to_string(delivery_wall_delta_max_ms) +
        " deliveryWallDeltaMinMs=" +
        std::to_string(delivery_wall_delta_min_ms) + " deliveryWallSamples=" +
        std::to_string(delivery_wall_delta_window_samples_) +
        " deliveryWallOver2x=" +
        std::to_string(delivery_wall_window_over_2x_frames_) +
        " deliveryWallOver3x=" +
        std::to_string(delivery_wall_window_over_3x_frames_) +
        " deliveryWallUnderHalf=" +
        std::to_string(delivery_wall_window_under_half_frames_) +
        " sourceQpcDeltaMs=" + std::to_string(source_qpc_delta_ms) +
        " sourceQpcDeltaMaxMs=" + std::to_string(source_qpc_delta_max_ms) +
        " sourceQpcSamples=" + std::to_string(source_qpc_delta_samples_) +
        " sourceQpcRegressions=" + std::to_string(source_qpc_regressions_) +
        " sourceQpcOver2x=" + std::to_string(source_qpc_over_2x_frames_) +
        " sourceQpcOver3x=" + std::to_string(source_qpc_over_3x_frames_) +
        " sourceQpcUnderHalf=" + std::to_string(source_qpc_under_half_frames_) +
        " sourceLatestObservedFrames=" +
        std::to_string(source_latest_observed_frames_) +
        " sourceLatestFrameGaps=" + std::to_string(source_latest_frame_gaps_) +
        " sourceLatestFrameRegressions=" +
        std::to_string(source_latest_frame_regressions_) +
        " sourceLatestQpcDeltaMs=" +
        std::to_string(source_latest_qpc_delta_ms) +
        " sourceLatestQpcDeltaMaxMs=" +
        std::to_string(source_latest_qpc_delta_max_ms) +
        " sourceLatestQpcSamples=" +
        std::to_string(source_latest_qpc_delta_samples_) +
        " sourceLatestQpcRegressions=" +
        std::to_string(source_latest_qpc_regressions_) +
        " sourceLatestQpcOver2x=" +
        std::to_string(source_latest_qpc_over_2x_frames_) +
        " sourceLatestQpcOver3x=" +
        std::to_string(source_latest_qpc_over_3x_frames_) +
        " sourceLatestQpcUnderHalf=" +
        std::to_string(source_latest_qpc_under_half_frames_) +
        " sourceLatestObservationDeltaMs=" +
        std::to_string(source_latest_observation_delta_ms) +
        " sourceLatestObservationDeltaMaxMs=" +
        std::to_string(source_latest_observation_delta_max_ms) +
        " sourceLatestObservationSamples=" +
        std::to_string(source_latest_observation_delta_samples_) +
        " sourceLatestObservationOver2x=" +
        std::to_string(source_latest_observation_over_2x_frames_) +
        " sourceLatestObservationOver3x=" +
        std::to_string(source_latest_observation_over_3x_frames_) +
        " sourceLatestEventAgeMs=" +
        std::to_string(source_latest_event_age_ms) +
        " sourceLatestEventAgeMaxMs=" +
        std::to_string(source_latest_event_age_max_ms) +
        " sourceLatestEventAgeSamples=" +
        std::to_string(source_latest_event_age_samples_) +
        " sourceLatestEventAgeOver1x=" +
        std::to_string(source_latest_event_age_over_1x_frames_) +
        " sourceLatestEventAgeOver2x=" +
        std::to_string(source_latest_event_age_over_2x_frames_) +
        " sourceLatestEventAgeOver3x=" +
        std::to_string(source_latest_event_age_over_3x_frames_) +
        " sourcePublishObservationAgeMs=" +
        std::to_string(source_publish_observation_age_ms) +
        " sourcePublishObservationAgeMaxMs=" +
        std::to_string(source_publish_observation_age_max_ms) +
        " sourcePublishObservationAgeSamples=" +
        std::to_string(source_publish_observation_age_samples_) +
        " sourcePublishObservationAgeOver1x=" +
        std::to_string(source_publish_observation_age_over_1x_frames_) +
        " sourcePublishObservationAgeOver2x=" +
        std::to_string(source_publish_observation_age_over_2x_frames_) +
        " sourcePublishObservationAgeOver3x=" +
        std::to_string(source_publish_observation_age_over_3x_frames_) +
        " producerPresentGapMs=" + std::to_string(producer_present_gap_ms) +
        " producerPresentGapMaxMs=" +
        std::to_string(producer_present_gap_max_ms) +
        " producerPresentGapSamples=" +
        std::to_string(
            shared_state_ ? shared_state_->producer_present_gap_samples : 0) +
        " producerCaptureGapMs=" + std::to_string(producer_capture_gap_ms) +
        " producerCaptureGapMaxMs=" +
        std::to_string(producer_capture_gap_max_ms) +
        " producerCaptureGapSamples=" +
        std::to_string(
            shared_state_ ? shared_state_->producer_capture_gap_samples : 0) +
        " producerPresentToPublishMs=" +
        std::to_string(producer_present_to_publish_ms) +
        " producerPresentToPublishMaxMs=" +
        std::to_string(producer_present_to_publish_max_ms) +
        " producerPresentToPublishSamples=" +
        std::to_string(shared_state_
                           ? shared_state_->producer_present_to_publish_samples
                           : 0) +
        " producerCopyMs=" + std::to_string(producer_copy_ms) +
        " producerCopyMaxMs=" + std::to_string(producer_copy_max_ms) +
        " producerCopySamples=" +
        std::to_string(shared_state_ ? shared_state_->producer_copy_samples
                                     : 0) +
        " producerResolveMs=" + std::to_string(producer_resolve_ms) +
        " producerResolveMaxMs=" + std::to_string(producer_resolve_max_ms) +
        " producerResolveSamples=" +
        std::to_string(shared_state_ ? shared_state_->producer_resolve_samples
                                     : 0) +
        " producerThrottledFrames=" +
        std::to_string(shared_state_ ? shared_state_->producer_throttled_frames
                                     : 0) +
        " convertMs=" + std::to_string(convert_ms) +
        " mapFailures=" + std::to_string(map_failures_) +
        " convertFailures=" + std::to_string(convert_failures_) +
        " proofFrames=" + std::to_string(proof_frames_written_) +
        " visibleProofFrames=" + std::to_string(visible_proof_frames_) +
        " i420ProofFrames=" + std::to_string(i420_proof_frames_written_) +
        " visibleI420ProofFrames=" +
        std::to_string(visible_i420_proof_frames_) + " initialBlackSkipped=" +
        std::to_string(initial_black_skipped_frames_) + " visibleSourceSeen=" +
        std::string(visible_source_seen_ ? "true" : "false"));
    stats_start_qpc_ = now.QuadPart;
    stats_start_frames_ = submitted_frames_;
    stats_start_readback_queued_frames_ = gpu_readback_queued_frames_;
    readback_copy_us_ = 0;
    readback_map_us_ = 0;
    readback_latency_us_ = 0;
    readback_latency_frames_total_ = 0;
    readback_latency_frames_max_ = 0;
    convert_us_ = 0;
    gpu_scale_us_ = 0;
    gpu_nv12_convert_qpc_ = 0;
    gpu_nv12_convert_max_qpc_ = 0;
    gpu_nv12_convert_samples_ = 0;
    gpu_nv12_bgra_scale_draw_qpc_ = 0;
    gpu_nv12_bgra_scale_draw_max_qpc_ = 0;
    gpu_nv12_bgra_scale_draw_samples_ = 0;
    gpu_nv12_blt_submit_qpc_ = 0;
    gpu_nv12_blt_submit_max_qpc_ = 0;
    gpu_nv12_blt_submit_samples_ = 0;
    gpu_nv12_blt_to_ready_qpc_ = 0;
    gpu_nv12_blt_to_ready_max_qpc_ = 0;
    gpu_nv12_blt_to_ready_samples_ = 0;
    gpu_nv12_blt_gpu_execution_ms_ = 0.0;
    gpu_nv12_blt_gpu_execution_max_ms_ = 0.0;
    gpu_nv12_blt_gpu_execution_samples_ = 0;
    gpu_nv12_blt_gpu_queue_delay_ms_ = 0.0;
    gpu_nv12_blt_gpu_queue_delay_max_ms_ = 0.0;
    gpu_nv12_blt_gpu_queue_delay_samples_ = 0;
    gpu_nv12_blt_gpu_timestamp_failures_ = 0;
    gpu_nv12_blt_gpu_timestamp_not_ready_ = 0;
    gpu_nv12_blt_gpu_timestamp_disjoint_ = 0;
    gpu_nv12_buffer_create_qpc_ = 0;
    gpu_nv12_buffer_create_max_qpc_ = 0;
    gpu_nv12_buffer_create_samples_ = 0;
    gpu_nv12_frame_ready_to_queue_qpc_ = 0;
    gpu_nv12_frame_ready_to_queue_max_qpc_ = 0;
    gpu_nv12_frame_ready_to_queue_samples_ = 0;
    gpu_nv12_conversion_start_age_qpc_ = 0;
    gpu_nv12_conversion_start_age_max_qpc_ = 0;
    gpu_nv12_conversion_start_age_samples_ = 0;
    timestamp_delta_window_us_total_ = 0;
    timestamp_delta_window_us_max_ = 0;
    timestamp_delta_window_samples_ = 0;
    delivery_wall_delta_window_us_total_ = 0;
    delivery_wall_delta_window_us_max_ = 0;
    delivery_wall_delta_window_us_min_ = 0;
    delivery_wall_delta_window_samples_ = 0;
    delivery_wall_window_over_2x_frames_ = 0;
    delivery_wall_window_over_3x_frames_ = 0;
    delivery_wall_window_under_half_frames_ = 0;
  }

  void MaybeWriteProof(const uint8_t* bgra, int output_width, int output_height,
                       int stride, int source_width, int source_height,
                       DXGI_FORMAT format, const BgraVisibilityStats& stats) {
    if (proof_frames_written_ >= 2) {
      return;
    }
    const std::wstring directory = WebrtcProofDirectory();
    std::wstring path;
    bool wrote = false;
    if (!directory.empty()) {
      wchar_t name[256];
      swprintf_s(name, L"\\%S-%03llu.bmp", session_id_.c_str(),
                 static_cast<unsigned long long>(proof_frames_written_ + 1));
      path = directory + name;
      wrote = WriteBgraBmp(path, output_width, output_height, stride, bgra);
    }
    ++proof_frames_written_;
    if (stats.visible) {
      ++visible_proof_frames_;
    }
    std::string path_utf8(path.begin(), path.end());
    Log("proof frame=" + std::to_string(proof_frames_written_) + " source=" +
        std::to_string(source_width) + "x" + std::to_string(source_height) +
        " output=" + std::to_string(output_width) + "x" +
        std::to_string(output_height) + " format=" + std::to_string(format) +
        " visible=" + std::string(stats.visible ? "true" : "false") +
        " minLuma=" + std::to_string(stats.min_luma) +
        " maxLuma=" + std::to_string(stats.max_luma) +
        " nonzeroSamples=" + std::to_string(stats.nonzero_samples) +
        " samples=" + std::to_string(stats.samples) + " wrote=" +
        std::string(wrote ? "true" : "false") + " path=\"" + path_utf8 + "\"");
  }

  void MaybeWriteI420Proof(
      const webrtc::scoped_refptr<webrtc::I420Buffer>& i420, int output_width,
      int output_height, int source_width, int source_height,
      DXGI_FORMAT format, bool source_visible) {
    if (i420 == nullptr) {
      return;
    }
    const bool needs_first_proof = i420_proof_frames_written_ == 0;
    const bool needs_visible_proof =
        source_visible && visible_i420_proof_frames_ == 0;
    if (!needs_first_proof && !needs_visible_proof) {
      return;
    }

    i420_proof_bgra_buffer_.assign(
        static_cast<size_t>(output_width) * output_height * 4, 0);
    const int stride = output_width * 4;
    const int rc = libyuv::I420ToARGB(
        i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(),
        i420->DataV(), i420->StrideV(), i420_proof_bgra_buffer_.data(), stride,
        output_width, output_height);
    if (rc != 0) {
      ++i420_proof_frames_written_;
      Log("i420_proof frame=" + std::to_string(i420_proof_frames_written_) +
          " source=" + std::to_string(source_width) + "x" +
          std::to_string(source_height) +
          " output=" + std::to_string(output_width) + "x" +
          std::to_string(output_height) + " format=" + std::to_string(format) +
          " visible=false minLuma=0 maxLuma=0 nonzeroSamples=0 samples=0 "
          "wrote=false conversionFailed=true path=\"\"");
      return;
    }

    const BgraVisibilityStats stats = AnalyzeBgra(
        i420_proof_bgra_buffer_.data(), output_width, output_height, stride);
    const std::wstring directory = WebrtcProofDirectory();
    std::wstring path;
    bool wrote = false;
    if (!directory.empty()) {
      wchar_t name[256];
      swprintf_s(
          name, L"\\%S-i420-%03llu.bmp", session_id_.c_str(),
          static_cast<unsigned long long>(i420_proof_frames_written_ + 1));
      path = directory + name;
      wrote = WriteBgraBmp(path, output_width, output_height, stride,
                           i420_proof_bgra_buffer_.data());
    }

    ++i420_proof_frames_written_;
    if (stats.visible) {
      ++visible_i420_proof_frames_;
    }
    std::string path_utf8(path.begin(), path.end());
    Log("i420_proof frame=" + std::to_string(i420_proof_frames_written_) +
        " source=" + std::to_string(source_width) + "x" +
        std::to_string(source_height) +
        " output=" + std::to_string(output_width) + "x" +
        std::to_string(output_height) + " format=" + std::to_string(format) +
        " visible=" + std::string(stats.visible ? "true" : "false") +
        " minLuma=" + std::to_string(stats.min_luma) +
        " maxLuma=" + std::to_string(stats.max_luma) +
        " nonzeroSamples=" + std::to_string(stats.nonzero_samples) +
        " samples=" + std::to_string(stats.samples) +
        " wrote=" + std::string(wrote ? "true" : "false") +
        " conversionFailed=false path=\"" + path_utf8 + "\"");
  }

  void Run() {
    QueryPerformanceFrequency(&frequency_);
    SetCaptureThreadPriority("capture_thread");
    auto mmcss = RegisterMmcssThread("capture_thread");
    session_id_ = HexSessionId();
    if (!EnsureD3dDevice()) {
      startup_failure_reason_ = "ensure_d3d_device_failed";
      Cleanup();
      started_.store(false);
      return;
    }
    if (source_mode_ == GameCaptureSourceMode::kDummyNv12LiveSender) {
      RunDummyNv12LiveSenderSource();
      Cleanup();
      started_.store(false);
      return;
    }
    if (!LaunchHelper(session_id_)) {
      startup_failure_reason_ = "launch_helper_failed";
      Cleanup();
      started_.store(false);
      return;
    }
    if (!OpenSharedState(session_id_)) {
      if (startup_failure_reason_.empty()) {
        startup_failure_reason_ = "open_shared_state_failed";
      }
      Cleanup();
      started_.store(false);
      return;
    }
    StartDeliveryThread();

    const int64_t frame_interval_qpc =
        std::max<int64_t>(1, frequency_.QuadPart / target_fps_);
    int64_t next_due_qpc = 0;
    uint64_t last_frame_index = 0;
    ComPtr<ID3D11Texture2D> latest_texture;
    uint64_t latest_frame_index = 0;
    uint64_t latest_frame_qpc = 0;
    uint64_t last_submitted_source_qpc = 0;
    bool waiting_for_source_fresh = false;
    auto try_submit_pending_native_nv12 = [&](const char* reason) {
      uint64_t submitted_source_frame_index = 0;
      uint64_t submitted_source_qpc = 0;
      if (!TrySubmitPendingNativeNv12Admission(
              reason, &submitted_source_frame_index,
              &submitted_source_qpc)) {
        return false;
      }
      if (submitted_source_frame_index != 0) {
        last_frame_index = submitted_source_frame_index;
      }
      if (submitted_source_qpc != 0) {
        last_submitted_source_qpc = submitted_source_qpc;
      }
      waiting_for_source_fresh = false;
      return true;
    };

    while (!stop_requested_.load()) {
      DWORD wait_ms = 250;
      LARGE_INTEGER now{};
      QueryPerformanceCounter(&now);
      if (waiting_for_source_fresh) {
        wait_ms = static_cast<DWORD>(std::clamp<int64_t>(
            (frame_interval_qpc * 1000 + frequency_.QuadPart - 1) /
                frequency_.QuadPart,
            1, 250));
      } else if (next_due_qpc != 0 && latest_texture != nullptr &&
                 now.QuadPart >= next_due_qpc) {
        wait_ms = 0;
      } else if (next_due_qpc != 0 && latest_texture != nullptr) {
        const int64_t delta_qpc = next_due_qpc - now.QuadPart;
        wait_ms = static_cast<DWORD>(std::clamp<int64_t>(
            (delta_qpc * 1000 + frequency_.QuadPart - 1) / frequency_.QuadPart,
            1, 250));
      }
      if (wait_ms > native_nv12_pending_poll_ms_ && HasPendingGpuNv12Slots() &&
          (native_nv12_ready_policy_ != NativeNv12ReadyPolicy::kFence ||
           native_nv12_ready_event_ == nullptr)) {
        wait_ms = native_nv12_pending_poll_ms_;
      }
      if (wait_ms > native_nv12_pending_poll_ms_ &&
          HasPendingNativeNv12Admission()) {
        wait_ms = native_nv12_pending_poll_ms_;
      }

      HANDLE wait_handles[2] = {frame_event_, native_nv12_ready_event_};
      const DWORD wait_handle_count =
          native_nv12_ready_event_ != nullptr ? 2 : 1;
      const DWORD wait_result = WaitForMultipleObjects(
          wait_handle_count, wait_handles, FALSE, wait_ms);
      if (wait_result == WAIT_OBJECT_0) {
        if (RefreshLatestSharedTexture(&latest_texture, &latest_frame_index,
                                       &latest_frame_qpc, frame_interval_qpc,
                                       "source_event") ==
            SourceRefreshResult::kSlotMismatch) {
          DrainReadyGpuNv12Slots(GpuNv12ReadyObservation::kSourceEvent);
          TrySubmitReadyGpuReadback(true);
          try_submit_pending_native_nv12("source_event_slot_mismatch");
          continue;
        }
      } else if (wait_handle_count > 1 && wait_result == WAIT_OBJECT_0 + 1) {
        DrainReadyGpuNv12Slots(GpuNv12ReadyObservation::kFenceEvent);
        TrySubmitReadyGpuReadback(true);
      } else if (wait_result != WAIT_TIMEOUT) {
        DrainReadyGpuNv12Slots(GpuNv12ReadyObservation::kWaitOther);
        TrySubmitReadyGpuReadback(true);
        try_submit_pending_native_nv12("wait_other");
        continue;
      }

      QueryPerformanceCounter(&now);
      if (RefreshLatestSharedTexture(&latest_texture, &latest_frame_index,
                                     &latest_frame_qpc, frame_interval_qpc,
                                     "pre_admission_poll") ==
          SourceRefreshResult::kSlotMismatch) {
        DrainReadyGpuNv12Slots(GpuNv12ReadyObservation::kLoopIdle);
        TrySubmitReadyGpuReadback(true);
        try_submit_pending_native_nv12("pre_admission_slot_mismatch");
        continue;
      }
      try_submit_pending_native_nv12("pre_admission_poll");
      if (next_due_qpc == 0) {
        next_due_qpc = now.QuadPart;
      }

      const bool repeated =
          latest_texture != nullptr && latest_frame_index == last_frame_index;
      const bool source_driven_fresh_due =
          latest_texture != nullptr && !repeated &&
          delivery_repeat_policy_ == DeliveryRepeatPolicy::kSkipOnMiss &&
          (delivery_waiting_for_fresh_.load() || waiting_for_source_fresh);
      const bool source_qpc_interval_due =
          latest_texture != nullptr && !repeated &&
          delivery_repeat_policy_ == DeliveryRepeatPolicy::kSkipOnMiss &&
          latest_frame_qpc != 0 &&
          (last_submitted_source_qpc == 0 ||
           latest_frame_qpc >= last_submitted_source_qpc +
                                   static_cast<uint64_t>(frame_interval_qpc));
      const bool early_source_due =
          source_driven_fresh_due || source_qpc_interval_due;
      const bool deadline_due = now.QuadPart >= next_due_qpc;
      const size_t nv12_pending_before_deadline =
          deadline_due ? PendingGpuNv12SlotCount() : 0;
      const uint64_t nv12_ready_before_deadline =
          deadline_due ? NativeNv12ReadyObservationTotal() : 0;
      if (deadline_due) {
        RecordNativeAdmissionDeadlineDue(
            std::max<int64_t>(0, now.QuadPart - next_due_qpc),
            frame_interval_qpc);
        if (latest_texture == nullptr) {
          ++native_admission_no_source_on_deadline_frames_;
        }
      }
      if (latest_texture != nullptr &&
          (deadline_due ||
           (!native_admission_strict_deadline_enabled_ && early_source_due))) {
        const bool source_driven_fresh_admitted =
            !deadline_due && source_driven_fresh_due;
        const bool source_qpc_interval_admitted =
            !deadline_due && source_qpc_interval_due;
        if (source_driven_fresh_admitted) {
          ++native_admission_source_driven_fresh_due_frames_;
        }
        if (source_qpc_interval_admitted) {
          ++native_admission_source_qpc_due_frames_;
        }
        bool keep_due_for_fresh = false;
        bool reset_due_from_now = false;
        if (repeated) {
          if (deadline_due) {
            ++native_admission_repeated_on_deadline_frames_;
          }
          ++duplicate_source_frame_skips_;
          RecordSourceAge(latest_frame_qpc, now.QuadPart,
                          &source_duplicate_skip_age_qpc_,
                          &source_duplicate_skip_age_max_qpc_,
                          &source_duplicate_skip_age_samples_);
          DrainReadyGpuNv12Slots(GpuNv12ReadyObservation::kDuplicateSkip);
          TrySubmitReadyGpuReadback(true);
          try_submit_pending_native_nv12("duplicate_skip");
          if (deadline_due) {
            RecordNativeAdmissionDeadlineNv12Outcome(
                nv12_pending_before_deadline, nv12_ready_before_deadline);
          }
          if (delivery_repeat_policy_ == DeliveryRepeatPolicy::kSkipOnMiss) {
            waiting_for_source_fresh = true;
            keep_due_for_fresh = true;
          }
        } else if (SubmitTexture(latest_texture.Get(), latest_frame_index,
                                 latest_frame_qpc, false)) {
          if (deadline_due) {
            ++native_admission_submit_on_deadline_frames_;
          } else {
            ++native_admission_submit_on_early_source_frames_;
          }
          waiting_for_source_fresh = false;
          last_frame_index = latest_frame_index;
          last_submitted_source_qpc = latest_frame_qpc;
          reset_due_from_now =
              source_qpc_interval_admitted && now.QuadPart < next_due_qpc;
          TrySubmitReadyGpuReadback(false);
          if (deadline_due) {
            RecordNativeAdmissionDeadlineNv12Outcome(
                nv12_pending_before_deadline, nv12_ready_before_deadline);
          }
        } else if (deadline_due) {
          RecordNativeAdmissionDeadlineNv12Outcome(nv12_pending_before_deadline,
                                                   nv12_ready_before_deadline);
        }
        if (reset_due_from_now) {
          LARGE_INTEGER reset_now{};
          QueryPerformanceCounter(&reset_now);
          next_due_qpc = reset_now.QuadPart + frame_interval_qpc;
        } else if (!keep_due_for_fresh) {
          next_due_qpc += frame_interval_qpc;
          while (next_due_qpc <= now.QuadPart - frame_interval_qpc) {
            next_due_qpc += frame_interval_qpc;
          }
        }
      } else {
        if (native_admission_strict_deadline_enabled_ &&
            latest_texture != nullptr && early_source_due &&
            latest_frame_index !=
                native_admission_last_suppressed_source_frame_index_) {
          ++native_admission_early_source_due_suppressed_frames_;
          native_admission_last_suppressed_source_frame_index_ =
              latest_frame_index;
        }
        DrainReadyGpuNv12Slots(GpuNv12ReadyObservation::kLoopIdle);
        TrySubmitReadyGpuReadback(true);
        try_submit_pending_native_nv12("loop_idle");
        if (deadline_due) {
          RecordNativeAdmissionDeadlineNv12Outcome(nv12_pending_before_deadline,
                                                   nv12_ready_before_deadline);
        }
      }
    }

    Log("stop requested submitted=" + std::to_string(submitted_frames_) +
        " deliveryQueued=" + std::to_string(delivery_queued_frames_) +
        " deliverySubmitted=" + std::to_string(delivery_submitted_frames_) +
        " deliveryOverwritten=" + std::to_string(delivery_overwritten_frames_) +
        " sharedSlotMismatches=" + std::to_string(shared_slot_mismatch_frames_) +
        " sharedStateSeqRetries=" + std::to_string(shared_state_seq_retries_) +
        " sharedStateSeqGiveups=" + std::to_string(shared_state_seq_giveups_) +
        " keyedMutexEnabledByEnv=" +
        std::string(native_keyed_mutex_enabled_ ? "true" : "false") +
        " keyedMutexSync=" +
        std::string(consumer_sync_is_keyed_mutex_ ? "true" : "false") +
        " keyedMutexAcquires=" + std::to_string(keyed_mutex_acquires_) +
        " keyedMutexAcquireTimeouts=" +
        std::to_string(keyed_mutex_acquire_timeouts_) +
        " keyedMutexAcquireFailures=" +
        std::to_string(keyed_mutex_acquire_failures_) +
        " keyedMutexReleaseFailures=" +
        std::to_string(keyed_mutex_release_failures_) +
        " keyedMutexCopyFailures=" + std::to_string(keyed_mutex_copy_failures_) +
        " nv12RenderConvert=" +
        std::string(native_nv12_render_convert_enabled_ ? "true" : "false") +
        " nv12RenderConvertFrames=" +
        std::to_string(nv12_render_convert_frames_));
    Cleanup();
    started_.store(false);
  }

  void Cleanup() {
    StopDeliveryThread();
    HANDLE stop_event = stop_event_;
    if (stop_event != nullptr) {
      Log("cleanup signaling_stop_event");
      SetEvent(stop_event);
    }
    if (helper_process_ != nullptr) {
      const DWORD wait_result =
          WaitForSingleObject(helper_process_, kHelperStopWaitMs);
      if (wait_result == WAIT_OBJECT_0) {
        helper_exit_code_available_ =
            GetExitCodeProcess(helper_process_, &helper_exit_code_);
        Log("cleanup helper_exited");
      } else {
        Log("cleanup helper_stop_timeout result=" +
            std::to_string(wait_result) +
            " waitMs=" + std::to_string(kHelperStopWaitMs) +
            " leaving_helper_for_safe_unload=true");
      }
      CloseHandle(helper_process_);
      helper_process_ = nullptr;
    }
    if (shared_state_ != nullptr) {
      UnmapViewOfFile(shared_state_);
      shared_state_ = nullptr;
    }
    ClearPendingNativeNv12Admission();
    if (shared_mapping_ != nullptr) {
      CloseHandle(shared_mapping_);
      shared_mapping_ = nullptr;
    }
    if (frame_event_ != nullptr) {
      CloseHandle(frame_event_);
      frame_event_ = nullptr;
    }
    if (native_nv12_ready_event_ != nullptr) {
      CloseHandle(native_nv12_ready_event_);
      native_nv12_ready_event_ = nullptr;
    }
    if (stop_event_ != nullptr) {
      CloseHandle(stop_event_);
      stop_event_ = nullptr;
    }
    ResetSharedTextures();
    keyed_copy_texture_.Reset();
    keyed_copy_width_ = 0;
    keyed_copy_height_ = 0;
    keyed_copy_format_ = DXGI_FORMAT_UNKNOWN;
    staging_texture_.Reset();
    ResetGpuScaleResources();
    context_.Reset();
    device_.Reset();
    if (d3dcompiler_module_ != nullptr) {
      FreeLibrary(d3dcompiler_module_);
      d3dcompiler_module_ = nullptr;
      d3d_compile_ = nullptr;
    }
  }

  std::wstring helper_path_;
  std::wstring helper_output_root_;
  uint32_t target_process_id_ = 0;
  GameCaptureSourceMode source_mode_ = GameCaptureSourceMode::kHelperD3d11;
  size_t max_width_ = 1280;
  size_t max_height_ = 720;
  size_t target_fps_ = 30;
  bool native_admission_strict_deadline_enabled_ = false;
  uint64_t native_admission_deadline_due_frames_ = 0;
  int64_t native_admission_deadline_lateness_qpc_ = 0;
  int64_t native_admission_deadline_lateness_max_qpc_ = 0;
  uint64_t native_admission_deadline_lateness_samples_ = 0;
  uint64_t native_admission_deadline_over_1x_frames_ = 0;
  uint64_t native_admission_deadline_over_2x_frames_ = 0;
  uint64_t native_admission_deadline_over_3x_frames_ = 0;
  uint64_t native_admission_no_source_on_deadline_frames_ = 0;
  uint64_t native_admission_repeated_on_deadline_frames_ = 0;
  uint64_t native_admission_submit_on_deadline_frames_ = 0;
  uint64_t native_admission_submit_on_early_source_frames_ = 0;
  uint64_t native_nv12_pending_on_deadline_frames_ = 0;
  uint64_t native_nv12_no_pending_on_deadline_frames_ = 0;
  uint64_t native_nv12_ready_on_deadline_frames_ = 0;
  uint64_t native_nv12_no_ready_on_deadline_frames_ = 0;
  uint64_t native_admission_source_driven_fresh_due_frames_ = 0;
  uint64_t native_admission_source_qpc_due_frames_ = 0;
  uint64_t native_admission_early_source_due_suppressed_frames_ = 0;
  uint64_t native_admission_last_suppressed_source_frame_index_ = 0;
  size_t delivery_queue_depth_ = 1;
  DeliveryRepeatPolicy delivery_repeat_policy_ =
      DeliveryRepeatPolicy::kSkipOnMiss;
  NativeNv12ReadyPolicy native_nv12_ready_policy_ =
      NativeNv12ReadyPolicy::kFence;
  DWORD native_nv12_pending_poll_ms_ = 8;
  size_t native_nv12_max_pending_slots_ = 2;
  size_t native_nv12_ready_drain_depth_ = 1;
  std::string session_id_;
  std::array<ComPtr<ID3D11Texture2D>, kDummyNv12RingDepth> dummy_nv12_textures_;
  std::vector<uint8_t> dummy_nv12_frame_data_;
  int dummy_nv12_width_ = 0;
  int dummy_nv12_height_ = 0;
  size_t dummy_nv12_index_ = 0;
  std::atomic<bool> started_{false};
  std::atomic<bool> stop_requested_{false};
  std::thread worker_;
  std::thread delivery_worker_;
  std::mutex delivery_mutex_;
  std::condition_variable delivery_cv_;
  std::deque<PendingVideoFrame> delivery_frames_;
  PendingVideoFrame last_delivered_frame_;
  std::atomic<bool> delivery_stop_{false};
  std::atomic<bool> delivery_waiting_for_fresh_{false};
  HANDLE helper_process_ = nullptr;
  DWORD helper_exit_code_ = STILL_ACTIVE;
  bool helper_exit_code_available_ = false;
  bool helper_exited_before_shared_state_ = false;
  DWORD open_shared_state_wait_ms_ = 0;
  HANDLE stop_event_ = nullptr;
  HANDLE frame_event_ = nullptr;
  HANDLE native_nv12_ready_event_ = nullptr;
  HANDLE shared_mapping_ = nullptr;
  SharedTextureState* shared_state_ = nullptr;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  D3dAdapterDiagnostics consumer_adapter_diagnostics_;
  bool consumer_gpu_thread_priority_requested_ = false;
  int consumer_gpu_thread_priority_requested_value_ = 0;
  bool consumer_gpu_thread_priority_applied_ = false;
  int consumer_gpu_thread_priority_before_ = 0;
  int consumer_gpu_thread_priority_after_ = 0;
  HRESULT consumer_gpu_thread_priority_hr_ = S_FALSE;
  HMODULE d3dcompiler_module_ = nullptr;
  D3DCompileProc d3d_compile_ = nullptr;
  ComPtr<ID3D11Texture2D> textures_[kRingDepth];
  ComPtr<IDXGIKeyedMutex> keyed_mutexes_[kRingDepth];
  ComPtr<ID3D11Texture2D> keyed_copy_texture_;
  UINT keyed_copy_width_ = 0;
  UINT keyed_copy_height_ = 0;
  DXGI_FORMAT keyed_copy_format_ = DXGI_FORMAT_UNKNOWN;
  bool consumer_sync_is_keyed_mutex_ = false;
  bool keyed_mutex_query_warning_logged_ = false;
  uint64_t keyed_mutex_acquires_ = 0;
  uint64_t keyed_mutex_acquire_timeouts_ = 0;
  uint64_t keyed_mutex_acquire_failures_ = 0;
  uint64_t keyed_mutex_release_failures_ = 0;
  uint64_t keyed_mutex_copy_failures_ = 0;
  ComPtr<ID3D11Texture2D> staging_texture_;
  ComPtr<ID3D11VertexShader> gpu_vertex_shader_;
  ComPtr<ID3D11PixelShader> gpu_pixel_shader_;
  ComPtr<ID3D11Buffer> gpu_scale_marker_constants_;
  ComPtr<ID3D11SamplerState> gpu_sampler_state_;
  ComPtr<ID3D11RenderTargetView> gpu_scaled_rtv_;
  ComPtr<ID3D11Texture2D> gpu_scaled_texture_;
  // Phase 2 render-convert (experimental, default off): SRV over the scaled
  // BGRA texture plus the two plane-writing pixel shaders that replace
  // VideoProcessorBlt when native_nv12_render_convert_enabled_.
  ComPtr<ID3D11ShaderResourceView> gpu_scaled_srv_;
  ComPtr<ID3D11PixelShader> nv12_y_pixel_shader_;
  ComPtr<ID3D11PixelShader> nv12_uv_pixel_shader_;
  uint32_t nv12_render_convert_width_ = 0;
  uint32_t nv12_render_convert_height_ = 0;
  uint64_t nv12_render_convert_frames_ = 0;
  GpuReadbackSlot gpu_readback_slots_[kGpuReadbackRingDepth];
  ComPtr<ID3D11VideoDevice> nv12_video_device_;
  ComPtr<ID3D11VideoContext> nv12_video_context_;
  ComPtr<ID3D11VideoProcessorEnumerator> nv12_video_processor_enumerator_;
  ComPtr<ID3D11VideoProcessor> nv12_video_processor_;
  ComPtr<ID3D11VideoProcessorInputView> nv12_video_input_view_;
  ComPtr<ID3D11DeviceContext4> gpu_nv12_fence_context_;
  ComPtr<ID3D11Fence> gpu_nv12_ready_fence_;
  GpuNv12Slot gpu_nv12_slots_[kGpuNv12RingDepth];
  std::map<ID3D11Texture2D*, ComPtr<ID3D11ShaderResourceView>>
      gpu_source_shader_views_;
  uint64_t opened_generation_ = 0;
  UINT staging_width_ = 0;
  UINT staging_height_ = 0;
  DXGI_FORMAT staging_format_ = DXGI_FORMAT_UNKNOWN;
  UINT last_source_width_ = 0;
  UINT last_source_height_ = 0;
  std::string startup_failure_reason_;
  DXGI_FORMAT last_source_format_ = DXGI_FORMAT_UNKNOWN;
  UINT gpu_input_width_ = 0;
  UINT gpu_input_height_ = 0;
  UINT gpu_output_width_ = 0;
  UINT gpu_output_height_ = 0;
  DXGI_FORMAT gpu_input_format_ = DXGI_FORMAT_UNKNOWN;
  std::vector<uint8_t> argb_buffer_;
  std::vector<uint8_t> scaled_argb_buffer_;
  std::vector<uint8_t> i420_proof_bgra_buffer_;
  LARGE_INTEGER frequency_{};
  int64_t stats_start_qpc_ = 0;
  uint64_t stats_start_frames_ = 0;
  uint64_t stats_start_readback_queued_frames_ = 0;
  uint64_t submitted_frames_ = 0;
  uint64_t repeated_frames_ = 0;
  uint64_t duplicate_source_frame_skips_ = 0;
  uint64_t delivery_queued_frames_ = 0;
  uint64_t delivery_submitted_frames_ = 0;
  uint64_t delivery_overwritten_frames_ = 0;
  uint64_t delivery_pacer_resyncs_ = 0;
  int64_t delivery_pacer_lag_ms_max_ = 0;
  uint64_t delivery_repeat_no_queued_frames_ = 0;
  uint64_t delivery_skip_no_queued_frames_ = 0;
  uint64_t delivery_fresh_wake_after_skip_frames_ = 0;
  uint64_t delivery_fresh_immediate_frames_ = 0;
  int64_t delivery_on_frame_qpc_ = 0;
  int64_t delivery_on_frame_max_qpc_ = 0;
  int64_t delivery_submit_prep_qpc_ = 0;
  int64_t delivery_submit_prep_max_qpc_ = 0;
  uint64_t delivery_submit_prep_samples_ = 0;
  int64_t delivery_on_frame_call_qpc_ = 0;
  int64_t delivery_on_frame_call_max_qpc_ = 0;
  uint64_t delivery_on_frame_call_samples_ = 0;
  int64_t delivery_post_on_frame_qpc_ = 0;
  int64_t delivery_post_on_frame_max_qpc_ = 0;
  uint64_t delivery_post_on_frame_samples_ = 0;
  int64_t native_buffer_release_qpc_ = 0;
  int64_t native_buffer_release_max_qpc_ = 0;
  uint64_t native_buffer_release_samples_ = 0;
  int64_t delivery_repeat_source_age_qpc_ = 0;
  int64_t delivery_repeat_source_age_max_qpc_ = 0;
  uint64_t delivery_repeat_source_age_samples_ = 0;
  int64_t ready_to_queue_qpc_ = 0;
  int64_t ready_to_queue_max_qpc_ = 0;
  uint64_t ready_to_queue_samples_ = 0;
  int64_t delivery_queue_wait_qpc_ = 0;
  int64_t delivery_queue_wait_max_qpc_ = 0;
  uint64_t delivery_queue_wait_samples_ = 0;
  int64_t delivery_overwrite_age_qpc_ = 0;
  int64_t delivery_overwrite_age_max_qpc_ = 0;
  uint64_t delivery_overwrite_age_samples_ = 0;
  uint64_t delivery_overwritten_fresh_frames_ = 0;
  int64_t ready_to_submit_qpc_ = 0;
  int64_t ready_to_submit_max_qpc_ = 0;
  uint64_t ready_to_submit_samples_ = 0;
  int64_t source_to_submit_qpc_ = 0;
  int64_t source_to_submit_max_qpc_ = 0;
  uint64_t source_to_submit_samples_ = 0;
  int64_t source_to_readback_ready_qpc_ = 0;
  int64_t source_to_readback_ready_max_qpc_ = 0;
  uint64_t source_to_readback_ready_samples_ = 0;
  int64_t readback_queue_to_map_qpc_ = 0;
  int64_t readback_queue_to_map_max_qpc_ = 0;
  uint64_t readback_queue_to_map_samples_ = 0;
  int64_t map_to_i420_qpc_ = 0;
  int64_t map_to_i420_max_qpc_ = 0;
  uint64_t map_to_i420_samples_ = 0;
  int64_t source_to_i420_ready_qpc_ = 0;
  int64_t source_to_i420_ready_max_qpc_ = 0;
  uint64_t source_to_i420_ready_samples_ = 0;
  int64_t source_to_queue_qpc_ = 0;
  int64_t source_to_queue_max_qpc_ = 0;
  uint64_t source_to_queue_samples_ = 0;
  int64_t source_duplicate_skip_age_qpc_ = 0;
  int64_t source_duplicate_skip_age_max_qpc_ = 0;
  uint64_t source_duplicate_skip_age_samples_ = 0;
  uint64_t proof_frames_written_ = 0;
  uint64_t visible_proof_frames_ = 0;
  uint64_t i420_proof_frames_written_ = 0;
  uint64_t visible_i420_proof_frames_ = 0;
  uint64_t initial_black_skipped_frames_ = 0;
  bool visible_source_seen_ = false;
  bool startup_black_skip_limit_logged_ = false;
  uint64_t map_failures_ = 0;
  uint64_t convert_failures_ = 0;
  uint64_t gpu_scaled_frames_ = 0;
  uint64_t gpu_scale_failures_ = 0;
  uint64_t gpu_nv12_submitted_frames_ = 0;
  uint64_t gpu_nv12_failures_ = 0;
  uint64_t gpu_nv12_queued_frames_ = 0;
  uint64_t gpu_nv12_ready_frames_ = 0;
  uint64_t gpu_nv12_not_ready_polls_ = 0;
  uint64_t gpu_nv12_overwritten_frames_ = 0;
  int64_t gpu_nv12_overwrite_age_qpc_ = 0;
  int64_t gpu_nv12_overwrite_age_max_qpc_ = 0;
  uint64_t gpu_nv12_overwrite_age_samples_ = 0;
  uint64_t gpu_nv12_overwritten_fresh_frames_ = 0;
  uint64_t gpu_nv12_ready_dropped_frames_ = 0;
  int64_t gpu_nv12_ready_drop_age_qpc_ = 0;
  int64_t gpu_nv12_ready_drop_age_max_qpc_ = 0;
  uint64_t gpu_nv12_ready_drop_age_samples_ = 0;
  uint64_t gpu_nv12_ready_dropped_fresh_frames_ = 0;
  uint64_t gpu_nv12_late_ready_dropped_frames_ = 0;
  int64_t gpu_nv12_late_ready_drop_age_qpc_ = 0;
  int64_t gpu_nv12_late_ready_drop_age_max_qpc_ = 0;
  uint64_t gpu_nv12_late_ready_drop_age_samples_ = 0;
  uint64_t gpu_nv12_late_ready_dropped_fresh_frames_ = 0;
  int64_t gpu_nv12_late_ready_drop_blt_to_ready_qpc_ = 0;
  int64_t gpu_nv12_late_ready_drop_blt_to_ready_max_qpc_ = 0;
  uint64_t gpu_nv12_late_ready_drop_blt_to_ready_samples_ = 0;
  uint64_t gpu_nv12_fence_signaled_frames_ = 0;
  uint64_t gpu_nv12_fence_ready_frames_ = 0;
  uint64_t gpu_nv12_fence_signal_failures_ = 0;
  uint64_t gpu_nv12_owned_copy_frames_ = 0;
  int64_t gpu_nv12_owned_copy_qpc_ = 0;
  int64_t gpu_nv12_owned_copy_max_qpc_ = 0;
  uint64_t gpu_nv12_owned_copy_samples_ = 0;
  uint64_t consumer_device_recoveries_ = 0;
  uint64_t cpu_fallback_frames_ = 0;
  bool gpu_scale_failure_logged_ = false;
  bool gpu_nv12_failure_logged_ = false;
  bool native_nv12_warmup_logged_ = false;
  bool native_nv12_disabled_logged_ = false;
  bool native_nv12_force_disabled_ = false;
  // Phase 2 experimental: convert scaled BGRA -> NV12 with two pixel-shader
  // passes writing plane RTVs instead of VideoProcessorBlt. Default off; the
  // build machine validates color correctness and whether it schedules better
  // than the video engine before this could become default.
  bool native_nv12_render_convert_enabled_ = false;
  bool native_nv12_render_convert_logged_ = false;
  bool native_nv12_render_convert_failed_logged_ = false;
  // Keyed-mutex ring ownership is opt-in and default off. The rotation-clean
  // BG3 A/B (`192558` keyed mutex vs `193046` event) measured keyed mutex
  // strictly worse on every gate: send 14.83 -> 23.17 FPS, decoded p50
  // 13.99 -> 22.05, presentation p95 130 -> 101 ms. The cost is the consumer
  // copy-under-lock (a full-source copy per frame) plus a forced cross-device
  // rendezvous behind the game's GPU work; the uncontended producer acquire was
  // free (`keyedMutexProducerTimeouts=0`). The 1a seqlock stays on either way
  // and is what fixes the tracked torn-read/slot-mismatch bug.
  bool native_keyed_mutex_enabled_ = false;
  uint32_t native_nv12_warmup_i420_frames_ = kDefaultNativeNv12WarmupI420Frames;
  bool native_nv12_suspended_after_device_loss_ = false;
  bool native_nv12_device_loss_suspend_logged_ = false;
  bool native_nv12_onframe_backpressure_suspend_enabled_ = true;
  uint32_t native_nv12_onframe_backpressure_threshold_ms_ = 20;
  uint32_t native_nv12_onframe_backpressure_frame_limit_ = 1;
  bool native_nv12_single_in_flight_enabled_ = true;
  bool native_nv12_admission_mailbox_enabled_ = false;
  bool native_nv12_gpu_queue_backoff_enabled_ = true;
  uint32_t native_nv12_gpu_queue_backoff_threshold_frames_ =
      kDefaultNativeNv12GpuQueueBackoffThresholdFrames;
  uint32_t native_nv12_gpu_queue_backoff_duration_frames_ =
      kDefaultNativeNv12GpuQueueBackoffDurationFrames;
  bool source_frame_visual_marker_enabled_ = false;
  bool native_nv12_drop_late_ready_enabled_ = false;
  uint32_t native_nv12_late_ready_drop_threshold_ms_ =
      kDefaultNativeNv12LateReadyDropThresholdMs;
  uint32_t native_nv12_admission_max_source_age_ms_ = 0;
  std::atomic<bool> native_nv12_suspended_after_onframe_backpressure_{false};
  std::atomic<bool> native_nv12_onframe_backpressure_suspend_logged_{false};
  uint64_t native_nv12_onframe_backpressure_frames_ = 0;
  uint64_t native_nv12_onframe_backpressure_streak_ = 0;
  double native_nv12_onframe_backpressure_max_ms_ = 0.0;
  uint64_t native_nv12_single_in_flight_deferred_frames_ = 0;
  uint64_t native_nv12_single_in_flight_deferred_fresh_frames_ = 0;
  uint64_t native_nv12_single_in_flight_pending_max_ = 0;
  int64_t native_nv12_single_in_flight_deferred_source_age_qpc_ = 0;
  int64_t native_nv12_single_in_flight_deferred_source_age_max_qpc_ = 0;
  uint64_t native_nv12_single_in_flight_deferred_source_age_samples_ = 0;
  int64_t native_nv12_gpu_queue_backoff_until_qpc_ = 0;
  uint64_t native_nv12_gpu_queue_backoff_triggered_frames_ = 0;
  uint64_t native_nv12_gpu_queue_backoff_suppressed_frames_ = 0;
  uint64_t native_nv12_gpu_queue_backoff_suppressed_fresh_frames_ = 0;
  int64_t native_nv12_gpu_queue_backoff_duration_qpc_ = 0;
  int64_t native_nv12_gpu_queue_backoff_duration_max_qpc_ = 0;
  uint64_t native_nv12_gpu_queue_backoff_duration_samples_ = 0;
  int64_t native_nv12_gpu_queue_backoff_trigger_blt_to_ready_qpc_ = 0;
  int64_t native_nv12_gpu_queue_backoff_trigger_blt_to_ready_max_qpc_ = 0;
  uint64_t native_nv12_gpu_queue_backoff_trigger_blt_to_ready_samples_ = 0;
  int64_t native_nv12_gpu_queue_backoff_suppressed_source_age_qpc_ = 0;
  int64_t native_nv12_gpu_queue_backoff_suppressed_source_age_max_qpc_ = 0;
  uint64_t native_nv12_gpu_queue_backoff_suppressed_source_age_samples_ = 0;
  NativeNv12PendingAdmission native_nv12_pending_admission_;
  uint64_t native_nv12_admission_mailbox_stored_frames_ = 0;
  uint64_t native_nv12_admission_mailbox_replaced_frames_ = 0;
  uint64_t native_nv12_admission_mailbox_submitted_frames_ = 0;
  uint64_t native_nv12_admission_mailbox_stale_dropped_frames_ = 0;
  int64_t native_nv12_admission_mailbox_pending_age_qpc_ = 0;
  int64_t native_nv12_admission_mailbox_pending_age_max_qpc_ = 0;
  uint64_t native_nv12_admission_mailbox_pending_age_samples_ = 0;
  int64_t native_nv12_admission_mailbox_submit_source_age_qpc_ = 0;
  int64_t native_nv12_admission_mailbox_submit_source_age_max_qpc_ = 0;
  uint64_t native_nv12_admission_mailbox_submit_source_age_samples_ = 0;
  bool native_nv12_r10_attempt_logged_ = false;
  bool native_nv12_r10_runtime_disabled_ = false;
  bool native_nv12_r10_failure_disable_logged_ = false;
  bool native_nv12_fence_available_ = false;
  bool native_nv12_fence_unavailable_logged_ = false;
  uint64_t native_nv12_r10_consecutive_failures_ = 0;
  size_t gpu_readback_write_index_ = 0;
  uint64_t gpu_readback_sequence_ = 0;
  size_t gpu_nv12_write_index_ = 0;
  uint64_t gpu_nv12_sequence_ = 0;
  uint64_t gpu_nv12_attempts_ = 0;
  uint64_t gpu_nv12_ready_fence_value_ = 0;
  uint64_t gpu_readback_queued_frames_ = 0;
  uint64_t gpu_readback_ready_frames_ = 0;
  uint64_t gpu_readback_not_ready_frames_ = 0;
  uint64_t gpu_readback_overwritten_frames_ = 0;
  uint64_t gpu_readback_stale_dropped_frames_ = 0;
  uint64_t gpu_readback_latency_dropped_frames_ = 0;
  uint64_t gpu_readback_map_attempts_ = 0;
  uint64_t last_submitted_source_frame_index_ = 0;
  uint64_t previous_submitted_source_frame_index_ = 0;
  uint64_t source_frame_regressions_ = 0;
  uint64_t source_frame_duplicate_submissions_ = 0;
  uint64_t source_frame_gaps_ = 0;
  uint64_t shared_slot_mismatch_frames_ = 0;
  uint64_t shared_state_seq_retries_ = 0;
  uint64_t shared_state_seq_giveups_ = 0;
  bool shared_state_version_mismatch_logged_ = false;
  bool source_frame_regression_logged_ = false;
  bool shared_slot_mismatch_logged_ = false;
  int64_t readback_copy_us_ = 0;
  int64_t readback_map_us_ = 0;
  int64_t readback_latency_us_ = 0;
  uint64_t readback_latency_frames_total_ = 0;
  uint64_t readback_latency_frames_max_ = 0;
  uint64_t timestamp_base_source_qpc_ = 0;
  int64_t timestamp_base_us_ = 0;
  int64_t last_frame_timestamp_us_ = 0;
  int64_t last_delivery_wall_timestamp_us_ = 0;
  uint64_t last_timestamp_source_qpc_ = 0;
  int64_t timestamp_delta_us_total_ = 0;
  int64_t timestamp_delta_us_max_ = 0;
  uint64_t timestamp_delta_samples_ = 0;
  int64_t timestamp_delta_window_us_total_ = 0;
  int64_t timestamp_delta_window_us_max_ = 0;
  uint64_t timestamp_delta_window_samples_ = 0;
  uint64_t timestamp_adjustments_ = 0;
  uint64_t timestamp_source_qpc_frames_ = 0;
  uint64_t timestamp_paced_fallback_frames_ = 0;
  uint64_t timestamp_repeated_frames_ = 0;
  int64_t delivery_wall_delta_us_total_ = 0;
  int64_t delivery_wall_delta_us_max_ = 0;
  int64_t delivery_wall_delta_us_min_ = 0;
  uint64_t delivery_wall_delta_samples_ = 0;
  int64_t delivery_wall_delta_window_us_total_ = 0;
  int64_t delivery_wall_delta_window_us_max_ = 0;
  int64_t delivery_wall_delta_window_us_min_ = 0;
  uint64_t delivery_wall_delta_window_samples_ = 0;
  uint64_t delivery_wall_over_2x_frames_ = 0;
  uint64_t delivery_wall_over_3x_frames_ = 0;
  uint64_t delivery_wall_under_half_frames_ = 0;
  uint64_t delivery_wall_window_over_2x_frames_ = 0;
  uint64_t delivery_wall_window_over_3x_frames_ = 0;
  uint64_t delivery_wall_window_under_half_frames_ = 0;
  int64_t source_qpc_delta_us_total_ = 0;
  int64_t source_qpc_delta_us_max_ = 0;
  uint64_t source_qpc_delta_samples_ = 0;
  uint64_t source_qpc_regressions_ = 0;
  uint64_t source_qpc_over_2x_frames_ = 0;
  uint64_t source_qpc_over_3x_frames_ = 0;
  uint64_t source_qpc_under_half_frames_ = 0;
  uint64_t source_latest_observed_frames_ = 0;
  uint64_t source_latest_frame_gaps_ = 0;
  uint64_t source_latest_frame_regressions_ = 0;
  int64_t source_latest_qpc_delta_ = 0;
  int64_t source_latest_qpc_delta_max_ = 0;
  uint64_t source_latest_qpc_delta_samples_ = 0;
  uint64_t source_latest_qpc_regressions_ = 0;
  uint64_t source_latest_qpc_over_2x_frames_ = 0;
  uint64_t source_latest_qpc_over_3x_frames_ = 0;
  uint64_t source_latest_qpc_under_half_frames_ = 0;
  int64_t source_latest_observation_delta_ = 0;
  int64_t source_latest_observation_delta_max_ = 0;
  uint64_t source_latest_observation_delta_samples_ = 0;
  uint64_t source_latest_observation_over_2x_frames_ = 0;
  uint64_t source_latest_observation_over_3x_frames_ = 0;
  int64_t source_latest_event_age_ = 0;
  int64_t source_latest_event_age_max_ = 0;
  uint64_t source_latest_event_age_samples_ = 0;
  uint64_t source_latest_event_age_over_1x_frames_ = 0;
  uint64_t source_latest_event_age_over_2x_frames_ = 0;
  uint64_t source_latest_event_age_over_3x_frames_ = 0;
  int64_t source_publish_observation_age_ = 0;
  int64_t source_publish_observation_age_max_ = 0;
  uint64_t source_publish_observation_age_samples_ = 0;
  uint64_t source_publish_observation_age_over_1x_frames_ = 0;
  uint64_t source_publish_observation_age_over_2x_frames_ = 0;
  uint64_t source_publish_observation_age_over_3x_frames_ = 0;
  uint64_t last_observed_source_frame_index_ = 0;
  uint64_t last_observed_source_qpc_ = 0;
  int64_t last_source_observation_qpc_ = 0;
  int64_t convert_us_ = 0;
  int64_t gpu_scale_us_ = 0;
  int64_t gpu_nv12_convert_qpc_ = 0;
  int64_t gpu_nv12_convert_max_qpc_ = 0;
  uint64_t gpu_nv12_convert_samples_ = 0;
  int64_t gpu_nv12_bgra_scale_draw_qpc_ = 0;
  int64_t gpu_nv12_bgra_scale_draw_max_qpc_ = 0;
  uint64_t gpu_nv12_bgra_scale_draw_samples_ = 0;
  int64_t gpu_nv12_blt_submit_qpc_ = 0;
  int64_t gpu_nv12_blt_submit_max_qpc_ = 0;
  uint64_t gpu_nv12_blt_submit_samples_ = 0;
  int64_t gpu_nv12_blt_to_ready_qpc_ = 0;
  int64_t gpu_nv12_blt_to_ready_max_qpc_ = 0;
  uint64_t gpu_nv12_blt_to_ready_samples_ = 0;
  double gpu_nv12_blt_gpu_execution_ms_ = 0.0;
  double gpu_nv12_blt_gpu_execution_max_ms_ = 0.0;
  uint64_t gpu_nv12_blt_gpu_execution_samples_ = 0;
  double gpu_nv12_blt_gpu_queue_delay_ms_ = 0.0;
  double gpu_nv12_blt_gpu_queue_delay_max_ms_ = 0.0;
  uint64_t gpu_nv12_blt_gpu_queue_delay_samples_ = 0;
  uint64_t gpu_nv12_blt_gpu_timestamp_failures_ = 0;
  uint64_t gpu_nv12_blt_gpu_timestamp_not_ready_ = 0;
  uint64_t gpu_nv12_blt_gpu_timestamp_disjoint_ = 0;
  uint64_t gpu_nv12_ready_observed_immediate_after_blt_frames_ = 0;
  uint64_t gpu_nv12_ready_observed_post_fence_registration_frames_ = 0;
  uint64_t gpu_nv12_ready_observed_fence_event_frames_ = 0;
  uint64_t gpu_nv12_ready_observed_source_event_frames_ = 0;
  uint64_t gpu_nv12_ready_observed_wait_other_frames_ = 0;
  uint64_t gpu_nv12_ready_observed_loop_idle_frames_ = 0;
  uint64_t gpu_nv12_ready_observed_duplicate_skip_frames_ = 0;
  uint64_t gpu_nv12_ready_observed_pre_submit_frames_ = 0;
  uint64_t gpu_nv12_ready_observed_write_slot_scan_frames_ = 0;
  uint64_t gpu_nv12_ready_observed_unknown_frames_ = 0;
  uint64_t gpu_nv12_blt_to_ready_over_1x_frames_ = 0;
  uint64_t gpu_nv12_blt_to_ready_over_2x_frames_ = 0;
  uint64_t gpu_nv12_blt_to_ready_over_3x_frames_ = 0;
  int64_t gpu_nv12_buffer_create_qpc_ = 0;
  int64_t gpu_nv12_buffer_create_max_qpc_ = 0;
  uint64_t gpu_nv12_buffer_create_samples_ = 0;
  int64_t gpu_nv12_frame_ready_to_queue_qpc_ = 0;
  int64_t gpu_nv12_frame_ready_to_queue_max_qpc_ = 0;
  uint64_t gpu_nv12_frame_ready_to_queue_samples_ = 0;
  int64_t gpu_nv12_conversion_start_age_qpc_ = 0;
  int64_t gpu_nv12_conversion_start_age_max_qpc_ = 0;
  uint64_t gpu_nv12_conversion_start_age_samples_ = 0;
  uint64_t gpu_nv12_stale_before_queue_frames_ = 0;
  int last_output_width_ = 0;
  int last_output_height_ = 0;
  DXGI_FORMAT unsupported_format_ = DXGI_FORMAT_UNKNOWN;
};

}  // namespace

std::shared_ptr<webrtc::internal::VideoCapturer>
CreateIntergalacticGameCaptureVideoCapturer(webrtc::Thread* worker_thread,
                                            const char* helper_path,
                                            uint32_t target_process_id,
                                            size_t max_width, size_t max_height,
                                            size_t target_fps,
                                            const char* source_mode) {
  (void)worker_thread;
  const GameCaptureSourceMode parsed_source_mode =
      GameCaptureSourceModeFromString(source_mode);
  const bool dummy_source =
      parsed_source_mode == GameCaptureSourceMode::kDummyNv12LiveSender;
  const std::wstring requested_helper =
      dummy_source ? std::wstring() : Utf8ToWide(helper_path);
  const std::wstring resolved_helper =
      dummy_source ? std::wstring() : ResolveHelperPath(requested_helper);
  if (!dummy_source && (target_process_id == 0 || resolved_helper.empty())) {
    AppendNativeDiagnosticLine(
        "game_capture_webrtc_source unavailable reason=missing_pid_or_helper");
    return nullptr;
  }
  return std::make_shared<IntergalacticGameCaptureVideoCapturer>(
      resolved_helper, target_process_id, max_width, max_height, target_fps,
      parsed_source_mode);
}

extern "C"
    __declspec(dllexport) int __stdcall InterGalacticGameCaptureWebrtcSourceSmoke(
        const wchar_t* helper_path, uint32_t target_process_id,
        uint32_t max_width, uint32_t max_height, uint32_t target_fps,
        uint32_t duration_ms, const wchar_t* output_json_path) {
  if (output_json_path == nullptr || output_json_path[0] == L'\0') {
    return 1;
  }

  const std::wstring requested_helper =
      helper_path == nullptr ? std::wstring() : std::wstring(helper_path);
  const std::wstring resolved_helper = ResolveHelperPath(requested_helper);
  if (target_process_id == 0 || resolved_helper.empty()) {
    const std::string json =
        "{\n"
        "  \"schema\": \"intergalactic.gameCaptureWebrtcSourceSmoke.v1\",\n"
        "  \"status\": \"missing_pid_or_helper\",\n"
        "  \"gpuScaled\": 0,\n"
        "  \"gpuScaleFailures\": 0,\n"
        "  \"cpuFallback\": 0\n"
        "}\n";
    WriteUtf8File(output_json_path, json);
    return 2;
  }

  const std::wstring output_directory = ParentDirectory(output_json_path);
  const std::wstring helper_output_root =
      JoinPath(output_directory, L"game-capture-helper");
  IntergalacticGameCaptureVideoCapturer capturer(
      resolved_helper, target_process_id, max_width, max_height, target_fps,
      GameCaptureSourceMode::kHelperD3d11, helper_output_root);
  if (!capturer.StartCapture()) {
    WriteUtf8File(output_json_path, capturer.SmokeStatsJson("start_failed"));
    return 3;
  }

  Sleep(std::clamp<uint32_t>(duration_ms, 1000, 30000));
  capturer.StopCapture();
  const bool startup_failed = !capturer.StartupFailureReason().empty();
  const std::string json =
      capturer.SmokeStatsJson(startup_failed ? "startup_failed" : "completed");
  if (!WriteUtf8File(output_json_path, json)) {
    return 4;
  }
  return startup_failed ? 5 : 0;
}

}  // namespace libwebrtc
