#include "src/win/intergalactic_game_capture_video_capturer.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <map>
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
#include "third_party/libyuv/include/libyuv.h"

namespace libwebrtc {
namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t kProtocolMagic = 0x43474749u;
constexpr uint32_t kSharedTextureStateVersion = 1;
constexpr int kRingDepth = 3;
constexpr int kGpuReadbackRingDepth = 4;
constexpr int kMaxHelperStartWaitMs = 6000;
constexpr int kLiveDurationMs = 60 * 60 * 1000;

using D3DCompileProc = HRESULT(WINAPI*)(
    LPCVOID src_data,
    SIZE_T src_data_size,
    LPCSTR source_name,
    const D3D_SHADER_MACRO* defines,
    ID3DInclude* include,
    LPCSTR entrypoint,
    LPCSTR target,
    UINT flags1,
    UINT flags2,
    ID3DBlob** code,
    ID3DBlob** error_messages);

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
  uint32_t reserved = 0;
  SharedTextureSlotState slots[kRingDepth];
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
  const BOOL ok =
      WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written,
                nullptr);
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
      static_cast<uint64_t>(
          std::chrono::high_resolution_clock::now().time_since_epoch().count()) ^
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
  if (line.empty()) {
    return;
  }
  RTC_LOG(LS_INFO) << "Inter Galactic " << line;
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
  if (GetFileSizeEx(file, &size) && size.QuadPart > 1024 * 1024) {
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
  CloseHandle(file);
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

void WriteLe16(uint8_t* out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value & 0xffu);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xffu);
}

void WriteLe32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value & 0xffu);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xffu);
  out[2] = static_cast<uint8_t>((value >> 16) & 0xffu);
  out[3] = static_cast<uint8_t>((value >> 24) & 0xffu);
}

std::wstring WebrtcProofDirectory() {
  wchar_t temp_path[MAX_PATH + 1] = {};
  const DWORD temp_length = GetTempPathW(MAX_PATH + 1, temp_path);
  if (temp_length == 0 || temp_length > MAX_PATH) {
    return {};
  }
  std::wstring path(temp_path, temp_length);
  if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
    path.push_back(L'\\');
  }
  path.append(L"intergalactic-game-capture-webrtc-proof");
  CreateDirectoryW(path.c_str(), nullptr);
  return path;
}

bool WriteBgraBmp(const std::wstring& path,
                  int width,
                  int height,
                  int stride,
                  const uint8_t* bgra) {
  if (path.empty() || width <= 0 || height <= 0 || stride <= 0 ||
      bgra == nullptr) {
    return false;
  }
  constexpr uint32_t kHeaderSize = 54;
  const uint32_t row_bytes = static_cast<uint32_t>(width) * 4u;
  const uint32_t pixel_bytes = row_bytes * static_cast<uint32_t>(height);
  const uint32_t file_size = kHeaderSize + pixel_bytes;

  std::vector<uint8_t> header(kHeaderSize, 0);
  header[0] = 'B';
  header[1] = 'M';
  WriteLe32(header.data() + 2, file_size);
  WriteLe32(header.data() + 10, kHeaderSize);
  WriteLe32(header.data() + 14, 40);
  WriteLe32(header.data() + 18, static_cast<uint32_t>(width));
  // Negative height stores rows top-down so the proof matches the stream.
  WriteLe32(header.data() + 22, static_cast<uint32_t>(-height));
  WriteLe16(header.data() + 26, 1);
  WriteLe16(header.data() + 28, 32);
  WriteLe32(header.data() + 34, pixel_bytes);

  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD written = 0;
  bool ok = WriteFile(file, header.data(), static_cast<DWORD>(header.size()),
                      &written, nullptr) &&
            written == header.size();
  for (int y = 0; ok && y < height; ++y) {
    const uint8_t* row = bgra + static_cast<size_t>(y) * stride;
    written = 0;
    ok = WriteFile(file, row, row_bytes, &written, nullptr) &&
         written == row_bytes;
  }
  CloseHandle(file);
  return ok;
}

struct BgraVisibilityStats {
  int min_luma = 255;
  int max_luma = 0;
  uint64_t nonzero_samples = 0;
  uint64_t samples = 0;
  bool visible = false;
};

BgraVisibilityStats AnalyzeBgra(const uint8_t* bgra,
                                int width,
                                int height,
                                int stride) {
  BgraVisibilityStats stats;
  if (bgra == nullptr || width <= 0 || height <= 0 || stride <= 0) {
    stats.min_luma = 0;
    return stats;
  }
  const int step_x = std::max(1, width / 32);
  const int step_y = std::max(1, height / 32);
  for (int y = 0; y < height; y += step_y) {
    const uint8_t* row = bgra + static_cast<size_t>(y) * stride;
    for (int x = 0; x < width; x += step_x) {
      const uint8_t* pixel = row + static_cast<size_t>(x) * 4;
      const int b = pixel[0];
      const int g = pixel[1];
      const int r = pixel[2];
      const int luma = (r + g + b) / 3;
      stats.min_luma = std::min(stats.min_luma, luma);
      stats.max_luma = std::max(stats.max_luma, luma);
      if (luma > 4) {
        ++stats.nonzero_samples;
      }
      ++stats.samples;
    }
  }
  stats.visible = stats.samples > 0 &&
                  stats.nonzero_samples > stats.samples / 16 &&
                  stats.max_luma - stats.min_luma > 8;
  return stats;
}

class IntergalacticGameCaptureVideoCapturer
    : public webrtc::internal::VideoCapturer {
 public:
  IntergalacticGameCaptureVideoCapturer(std::wstring helper_path,
                                        uint32_t target_process_id,
                                        size_t max_width,
                                        size_t max_height,
                                        size_t target_fps)
      : helper_path_(std::move(helper_path)),
        target_process_id_(target_process_id),
        max_width_(std::max<size_t>(2, max_width)),
        max_height_(std::max<size_t>(2, max_height)),
        target_fps_(std::clamp<size_t>(target_fps, 1, 60)) {}

  ~IntergalacticGameCaptureVideoCapturer() override { StopCapture(); }

  bool StartCapture() override {
    if (started_.load()) {
      return true;
    }
    if (target_process_id_ == 0 || helper_path_.empty()) {
      Log("start_failed reason=missing_pid_or_helper");
      return false;
    }
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
    if (worker_.joinable() &&
        worker_.get_id() != std::this_thread::get_id()) {
      worker_.join();
      Log("stop_capture worker_joined");
    }
  }

  std::string SmokeStatsJson(const std::string& status) const {
    const uint64_t submitted = submitted_frames_;
    const double submitted_denominator = submitted == 0 ? 1.0 : submitted;
    const double queued_denominator = gpu_readback_queued_frames_ == 0
                                          ? 1.0
                                          : gpu_readback_queued_frames_;
    const double frequency = frequency_.QuadPart == 0
                                 ? 1.0
                                 : static_cast<double>(frequency_.QuadPart);
    const double gpu_scale_ms =
        static_cast<double>(gpu_scale_us_) * 1000.0 / frequency /
        queued_denominator;
    const double copy_ms =
        static_cast<double>(readback_copy_us_) * 1000.0 / frequency /
        queued_denominator;
    const double map_ms =
        static_cast<double>(readback_map_us_) * 1000.0 / frequency /
        submitted_denominator;
    const double convert_ms =
        static_cast<double>(convert_us_) * 1000.0 / frequency /
        submitted_denominator;
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

    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"intergalactic.gameCaptureWebrtcSourceSmoke.v1\",\n";
    out << "  \"status\": \"" << status << "\",\n";
    out << "  \"targetProcessId\": " << target_process_id_ << ",\n";
    out << "  \"requestedMaxWidth\": " << max_width_ << ",\n";
    out << "  \"requestedMaxHeight\": " << max_height_ << ",\n";
    out << "  \"requestedTargetFps\": " << target_fps_ << ",\n";
    out << "  \"sourceWidth\": " << last_source_width_ << ",\n";
    out << "  \"sourceHeight\": " << last_source_height_ << ",\n";
    out << "  \"outputWidth\": " << last_output_width_ << ",\n";
    out << "  \"outputHeight\": " << last_output_height_ << ",\n";
    out << "  \"format\": " << last_source_format_ << ",\n";
    out << "  \"submitted\": " << submitted_frames_ << ",\n";
    out << "  \"repeated\": " << repeated_frames_ << ",\n";
    out << "  \"gpuScaled\": " << gpu_scaled_frames_ << ",\n";
    out << "  \"gpuScaleFailures\": " << gpu_scale_failures_ << ",\n";
    out << "  \"cpuFallback\": " << cpu_fallback_frames_ << ",\n";
    out << "  \"readbackQueued\": " << gpu_readback_queued_frames_ << ",\n";
    out << "  \"readbackReady\": " << gpu_readback_ready_frames_ << ",\n";
    out << "  \"readbackNotReady\": " << gpu_readback_not_ready_frames_
        << ",\n";
    out << "  \"readbackOverwritten\": " << gpu_readback_overwritten_frames_
        << ",\n";
    out << "  \"readbackMapAttempts\": " << gpu_readback_map_attempts_
        << ",\n";
    out << "  \"gpuScaleMs\": " << gpu_scale_ms << ",\n";
    out << "  \"copyMs\": " << copy_ms << ",\n";
    out << "  \"mapMs\": " << map_ms << ",\n";
    out << "  \"readbackLatencyMs\": " << readback_latency_ms << ",\n";
    out << "  \"readbackLatencyFramesAvg\": " << readback_latency_frames_avg
        << ",\n";
    out << "  \"readbackLatencyFramesMax\": "
        << readback_latency_frames_max_ << ",\n";
    out << "  \"convertMs\": " << convert_ms << ",\n";
    out << "  \"mapFailures\": " << map_failures_ << ",\n";
    out << "  \"convertFailures\": " << convert_failures_ << ",\n";
    out << "  \"proofFrames\": " << proof_frames_written_ << ",\n";
    out << "  \"visibleProofFrames\": " << visible_proof_frames_ << ",\n";
    out << "  \"i420ProofFrames\": " << i420_proof_frames_written_ << ",\n";
    out << "  \"visibleI420ProofFrames\": "
        << visible_i420_proof_frames_ << ",\n";
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

  void Log(const std::string& message) {
    AppendNativeDiagnosticLine("game_capture_webrtc_source " + message);
  }

  void LogGpuScaleFailure(const std::string& reason, HRESULT hr) {
    ++gpu_scale_failures_;
    if (!gpu_scale_failure_logged_ || gpu_scale_failures_ % 30 == 0) {
      gpu_scale_failure_logged_ = true;
      Log("gpu_scale_failed reason=" + reason + " hr=" + HResultHex(hr) +
          " failures=" + std::to_string(gpu_scale_failures_));
    }
  }

  bool LaunchHelper(const std::string& session_id) {
    std::wstring command = QuoteArg(helper_path_);
    command += L" --pid " + std::to_wstring(target_process_id_);
    command += L" --session-id " + WideFromAscii(session_id);
    command += L" --duration-ms " + std::to_wstring(kLiveDurationMs);
    command += L" --max-saved-frames 0";
    command += L" --host-consume-frames false";
    command += L" --external-consumer true";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    std::vector<wchar_t> command_buffer(command.begin(), command.end());
    command_buffer.push_back(L'\0');
    const BOOL ok = CreateProcessW(nullptr, command_buffer.data(), nullptr,
                                   nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                   nullptr, &startup, &info);
    if (!ok) {
      Log("helper_launch_failed error=" + std::to_string(GetLastError()));
      return false;
    }
    helper_process_ = info.hProcess;
    CloseHandle(info.hThread);
    Log("helper_launched pid=" + std::to_string(target_process_id_) +
        " target=" + std::to_string(max_width_) + "x" +
        std::to_string(max_height_) + "@" + std::to_string(target_fps_));
    return true;
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
      if (stop_event_ == nullptr) {
        stop_event_ =
            OpenEventW(EVENT_MODIFY_STATE, FALSE, stop_name.c_str());
      }
      if (frame_event_ == nullptr) {
        frame_event_ = OpenEventW(SYNCHRONIZE, FALSE, frame_name.c_str());
      }
      if (shared_mapping_ == nullptr) {
        shared_mapping_ =
            OpenFileMappingW(FILE_MAP_READ, FALSE, shared_name.c_str());
      }
      if (shared_state_ == nullptr && shared_mapping_ != nullptr) {
        shared_state_ = reinterpret_cast<SharedTextureState*>(
            MapViewOfFile(shared_mapping_, FILE_MAP_READ, 0, 0,
                          sizeof(SharedTextureState)));
      }
      if (stop_event_ != nullptr && frame_event_ != nullptr &&
          shared_state_ != nullptr) {
        Log("shared_state_opened session=" + session_id);
        return true;
      }
      Sleep(50);
    }
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
    const HRESULT hr =
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                          D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                          ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_,
                          &level, &context_);
    if (FAILED(hr)) {
      Log("d3d11_create_failed hr=" + HResultHex(hr));
      return false;
    }
    return true;
  }

  void ResetSharedTextures() {
    for (auto& texture : textures_) {
      texture.Reset();
    }
    opened_generation_ = 0;
  }

  void OpenTexturesForGeneration() {
    if (shared_state_ == nullptr || device_ == nullptr) {
      return;
    }
    if (shared_state_->generation == opened_generation_) {
      return;
    }
    ResetSharedTextures();
    opened_generation_ = shared_state_->generation;
    const uint32_t depth =
        std::min<uint32_t>(shared_state_->ring_depth, kRingDepth);
    uint32_t opened = 0;
    for (uint32_t i = 0; i < depth; ++i) {
      const uint64_t handle_value = shared_state_->slots[i].shared_handle;
      if (handle_value == 0) {
        continue;
      }
      HANDLE handle =
          reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle_value));
      const HRESULT hr =
          device_->OpenSharedResource(handle, IID_PPV_ARGS(&textures_[i]));
      if (SUCCEEDED(hr)) {
        ++opened;
      }
    }
    Log("shared_textures_opened generation=" +
        std::to_string(opened_generation_) + " count=" +
        std::to_string(opened) + " source=" +
        std::to_string(shared_state_->backbuffer_width) + "x" +
        std::to_string(shared_state_->backbuffer_height) + " format=" +
        std::to_string(shared_state_->backbuffer_format));
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
    gpu_sampler_state_.Reset();
    gpu_scaled_rtv_.Reset();
    gpu_scaled_texture_.Reset();
    ResetGpuReadbackSlots();
    gpu_source_shader_views_.clear();
    gpu_input_width_ = 0;
    gpu_input_height_ = 0;
    gpu_output_width_ = 0;
    gpu_output_height_ = 0;
    gpu_input_format_ = DXGI_FORMAT_UNKNOWN;
  }

  void ResetGpuScaleOutputResources() {
    gpu_scaled_rtv_.Reset();
    gpu_scaled_texture_.Reset();
    ResetGpuReadbackSlots();
    gpu_source_shader_views_.clear();
    gpu_input_width_ = 0;
    gpu_input_height_ = 0;
    gpu_output_width_ = 0;
    gpu_output_height_ = 0;
    gpu_input_format_ = DXGI_FORMAT_UNKNOWN;
  }

  void ResetGpuReadbackSlots() {
    for (auto& slot : gpu_readback_slots_) {
      slot.texture.Reset();
      slot.pending = false;
      slot.sequence = 0;
    }
    gpu_readback_write_index_ = 0;
    gpu_readback_sequence_ = 0;
  }

  bool GpuReadbackSlotsReady(UINT width, UINT height, DXGI_FORMAT format) const {
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

  bool EnsureGpuScaleShaders() {
    if (device_ == nullptr) {
      return false;
    }
    if (gpu_vertex_shader_ != nullptr && gpu_pixel_shader_ != nullptr &&
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

float4 PSMain(VSOut input) : SV_TARGET {
  return sourceTexture.Sample(sourceSampler, input.uv);
}
)";

    ComPtr<ID3DBlob> vertex_blob;
    ComPtr<ID3DBlob> pixel_blob;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = d3d_compile_(kShaderSource, std::strlen(kShaderSource),
                              "game_capture_webrtc_scale", nullptr, nullptr,
                              "VSMain", "vs_4_0", 0, 0, &vertex_blob,
                              &errors);
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
                                    int output_width,
                                    int output_height) {
    if (device_ == nullptr || context_ == nullptr || output_width <= 0 ||
        output_height <= 0) {
      return false;
    }
    if (!EnsureGpuScaleShaders()) {
      return false;
    }

    const bool matching =
        gpu_scaled_rtv_ != nullptr &&
        gpu_scaled_texture_ != nullptr &&
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
    HRESULT hr = device_->CreateTexture2D(&output_desc, nullptr,
                                          &gpu_scaled_texture_);
    if (FAILED(hr)) {
      LogGpuScaleFailure("output_texture_create_failed", hr);
      return false;
    }

    hr = device_->CreateRenderTargetView(gpu_scaled_texture_.Get(), nullptr,
                                         &gpu_scaled_rtv_);
    if (FAILED(hr)) {
      LogGpuScaleFailure("output_rtv_create_failed", hr);
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
        " outputFormat=" +
        std::to_string(DXGI_FORMAT_B8G8R8A8_UNORM));
    return true;
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
      return nullptr;
    }
    auto* raw = view.Get();
    gpu_source_shader_views_[texture] = std::move(view);
    return raw;
  }

  bool QueueGpuScaledBgraReadback(ID3D11Texture2D* texture,
                                  const D3D11_TEXTURE2D_DESC& source_desc,
                                  int output_width,
                                  int output_height,
                                  bool repeated,
                                  const LARGE_INTEGER& start,
                                  LARGE_INTEGER* after_gpu,
                                  LARGE_INTEGER* after_copy) {
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
    context_->PSSetShaderResources(0, 1, shader_resources);
    context_->PSSetSamplers(0, 1, samplers);
    context_->Draw(3, 0);
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
                           DXGI_FORMAT format,
                           int width,
                           int height,
                           const uint8_t** argb_data,
                           int* argb_stride) {
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
      if (!slot.pending || slot.sequence == 0 ||
          slot.sequence > max_sequence) {
        continue;
      }
      if (oldest == nullptr || slot.sequence < oldest->sequence) {
        oldest = &slot;
      }
    }
    return oldest;
  }

  bool TrySubmitReadyGpuReadback() {
    if (gpu_readback_sequence_ <= 1 || context_ == nullptr) {
      return false;
    }

    GpuReadbackSlot* slot =
        OldestPendingReadbackSlot(gpu_readback_sequence_ - 1);
    if (slot == nullptr || slot->texture == nullptr) {
      return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    LARGE_INTEGER map_start{};
    LARGE_INTEGER after_map{};
    LARGE_INTEGER after_convert{};
    QueryPerformanceCounter(&map_start);
    ++gpu_readback_map_attempts_;
    const HRESULT map_hr =
        context_->Map(slot->texture.Get(), 0, D3D11_MAP_READ,
                      D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    QueryPerformanceCounter(&after_map);
    if (map_hr == DXGI_ERROR_WAS_STILL_DRAWING) {
      ++gpu_readback_not_ready_frames_;
      return false;
    }
    if (FAILED(map_hr)) {
      ++map_failures_;
      slot->pending = false;
      Log("scaled_readback_map_failed hr=" + HResultHex(map_hr));
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
              std::to_string(initial_black_skipped_frames_) + " source=" +
              std::to_string(slot->source_width) + "x" +
              std::to_string(slot->source_height) + " output=" +
              std::to_string(slot->output_width) + "x" +
              std::to_string(slot->output_height) + " format=" +
              std::to_string(slot->source_format) + " minLuma=" +
              std::to_string(scaled_visibility.min_luma) + " maxLuma=" +
              std::to_string(scaled_visibility.max_luma) +
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
                std::to_string(initial_black_skipped_frames_) + " source=" +
                std::to_string(slot->source_width) + "x" +
                std::to_string(slot->source_height) + " output=" +
                std::to_string(slot->output_width) + "x" +
                std::to_string(slot->output_height) + " format=" +
                std::to_string(slot->source_format) +
                " visible=false minLuma=" +
                std::to_string(scaled_visibility.min_luma) + " maxLuma=" +
                std::to_string(scaled_visibility.max_luma) +
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
              std::to_string(initial_black_skipped_frames_) + " source=" +
              std::to_string(slot->source_width) + "x" +
              std::to_string(slot->source_height) + " output=" +
              std::to_string(slot->output_width) + "x" +
              std::to_string(slot->output_height) + " format=" +
              std::to_string(slot->source_format));
        }
      }
    }

    webrtc::scoped_refptr<webrtc::I420Buffer> i420 =
        webrtc::I420Buffer::Create(slot->output_width, slot->output_height);
    const int rc =
        libyuv::ARGBToI420(scaled_argb, scaled_stride, i420->MutableDataY(),
                           i420->StrideY(), i420->MutableDataU(),
                           i420->StrideU(), i420->MutableDataV(),
                           i420->StrideV(), slot->output_width,
                           slot->output_height);
    context_->Unmap(slot->texture.Get(), 0);
    QueryPerformanceCounter(&after_convert);
    if (rc != 0) {
      ++convert_failures_;
      slot->pending = false;
      return false;
    }

    MaybeWriteI420Proof(i420, slot->output_width, slot->output_height,
                        slot->source_width, slot->source_height,
                        slot->source_format, scaled_visibility.visible);

    OnFrame(webrtc::VideoFrame::Builder()
                .set_video_frame_buffer(i420)
                .set_rotation(webrtc::kVideoRotation_0)
                .set_timestamp_us(webrtc::TimeMicros())
                .build());

    ++submitted_frames_;
    ++gpu_scaled_frames_;
    ++gpu_readback_ready_frames_;
    if (slot->repeated) {
      ++repeated_frames_;
    }
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
    last_source_width_ = slot->source_width;
    last_source_height_ = slot->source_height;
    last_source_format_ = slot->source_format;
    last_output_width_ = slot->output_width;
    last_output_height_ = slot->output_height;
    const int source_width = slot->source_width;
    const int source_height = slot->source_height;
    const DXGI_FORMAT source_format = slot->source_format;
    slot->pending = false;
    MaybeLogStats(source_width, source_height, source_format);
    return true;
  }

  bool SubmitTextureViaGpuScaledBgra(ID3D11Texture2D* texture,
                                     const D3D11_TEXTURE2D_DESC& desc,
                                     bool repeated,
                                     bool* should_fallback) {
    if (should_fallback != nullptr) {
      *should_fallback = false;
    }
    const int source_width = static_cast<int>(desc.Width);
    const int source_height = static_cast<int>(desc.Height);
    const double scale = std::min(
        {static_cast<double>(max_width_) / source_width,
         static_cast<double>(max_height_) / source_height, 1.0});
    const int output_width =
        ContainFitDimension(source_width, static_cast<int>(max_width_), scale);
    const int output_height =
        ContainFitDimension(source_height, static_cast<int>(max_height_), scale);
    last_source_width_ = source_width;
    last_source_height_ = source_height;
    last_source_format_ = desc.Format;

    LARGE_INTEGER start{};
    LARGE_INTEGER after_gpu{};
    LARGE_INTEGER after_copy{};
    QueryPerformanceCounter(&start);

    if (!QueueGpuScaledBgraReadback(texture, desc, output_width, output_height,
                                    repeated, start, &after_gpu, &after_copy)) {
      if (should_fallback != nullptr) {
        *should_fallback = true;
      }
      return false;
    }

    last_output_width_ = output_width;
    last_output_height_ = output_height;
    TrySubmitReadyGpuReadback();
    return true;
  }

  bool SubmitTexture(ID3D11Texture2D* texture, bool repeated) {
    if (texture == nullptr || context_ == nullptr) {
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
    if (SubmitTextureViaGpuScaledBgra(texture, desc, repeated,
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
      return false;
    }

    const int source_width = static_cast<int>(desc.Width);
    const int source_height = static_cast<int>(desc.Height);
    last_source_width_ = source_width;
    last_source_height_ = source_height;
    last_source_format_ = desc.Format;
    const double scale = std::min(
        {static_cast<double>(max_width_) / source_width,
         static_cast<double>(max_height_) / source_height, 1.0});
    const int output_width =
        ContainFitDimension(source_width, static_cast<int>(max_width_), scale);
    const int output_height =
        ContainFitDimension(source_height, static_cast<int>(max_height_), scale);

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
      if (libyuv::ARGBScale(argb_data, argb_stride, source_width,
                            source_height, scaled_argb_buffer_.data(),
                            output_width * 4, output_width, output_height,
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
              std::to_string(initial_black_skipped_frames_) + " source=" +
              std::to_string(source_width) + "x" +
              std::to_string(source_height) + " output=" +
              std::to_string(output_width) + "x" +
              std::to_string(output_height) + " format=" +
              std::to_string(desc.Format) + " minLuma=" +
              std::to_string(scaled_visibility.min_luma) + " maxLuma=" +
              std::to_string(scaled_visibility.max_luma) +
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
              initial_black_skipped_frames_ ==
                  max_initial_black_skips ||
              initial_black_skipped_frames_ % target_fps_ == 0) {
            Log("skip_initial_black frame=" +
                std::to_string(initial_black_skipped_frames_) + " source=" +
                std::to_string(source_width) + "x" +
                std::to_string(source_height) + " output=" +
                std::to_string(output_width) + "x" +
                std::to_string(output_height) + " format=" +
                std::to_string(desc.Format) + " visible=false minLuma=" +
                std::to_string(scaled_visibility.min_luma) + " maxLuma=" +
                std::to_string(scaled_visibility.max_luma) +
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
              std::to_string(initial_black_skipped_frames_) + " source=" +
              std::to_string(source_width) + "x" +
              std::to_string(source_height) + " output=" +
              std::to_string(output_width) + "x" +
              std::to_string(output_height) + " format=" +
              std::to_string(desc.Format));
        }
      }
    }

    webrtc::scoped_refptr<webrtc::I420Buffer> i420 =
        webrtc::I420Buffer::Create(output_width, output_height);
    const int rc =
        libyuv::ARGBToI420(scaled_argb, scaled_stride, i420->MutableDataY(),
                           i420->StrideY(), i420->MutableDataU(),
                           i420->StrideU(), i420->MutableDataV(),
                           i420->StrideV(), output_width, output_height);
    context_->Unmap(staging_texture_.Get(), 0);
    QueryPerformanceCounter(&after_convert);
    if (rc != 0) {
      ++convert_failures_;
      return false;
    }

    MaybeWriteI420Proof(i420, output_width, output_height, source_width,
                        source_height, desc.Format,
                        scaled_visibility.visible);

    OnFrame(webrtc::VideoFrame::Builder()
                .set_video_frame_buffer(i420)
                .set_rotation(webrtc::kVideoRotation_0)
                .set_timestamp_us(webrtc::TimeMicros())
                .build());

    ++submitted_frames_;
    if (repeated) {
      ++repeated_frames_;
    }
    readback_copy_us_ += after_copy.QuadPart - start.QuadPart;
    readback_map_us_ += after_map.QuadPart - after_copy.QuadPart;
    convert_us_ += after_convert.QuadPart - after_map.QuadPart;
    last_output_width_ = output_width;
    last_output_height_ = output_height;
    MaybeLogStats(source_width, source_height, desc.Format);
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
                                        1000.0 / frequency_.QuadPart /
                                        queued);
    const double map_ms = submitted == 0
                              ? 0.0
                              : (static_cast<double>(readback_map_us_) *
                                 1000.0 / frequency_.QuadPart / submitted);
    const double convert_ms = submitted == 0
                                  ? 0.0
                                  : (static_cast<double>(convert_us_) *
                                     1000.0 / frequency_.QuadPart / submitted);
    const double readback_latency_ms =
        submitted == 0 ? 0.0
                       : (static_cast<double>(readback_latency_us_) * 1000.0 /
                          frequency_.QuadPart / submitted);
    const double readback_latency_frames_avg =
        submitted == 0 ? 0.0
                       : static_cast<double>(readback_latency_frames_total_) /
                             static_cast<double>(submitted);
    Log("stats source=" + std::to_string(source_width) + "x" +
        std::to_string(source_height) + " output=" +
        std::to_string(last_output_width_) + "x" +
        std::to_string(last_output_height_) + " format=" +
        std::to_string(format) + " fps=" + std::to_string(fps) +
        " submitted=" + std::to_string(submitted_frames_) +
        " repeated=" + std::to_string(repeated_frames_) +
        " copied=" + std::to_string(shared_state_ ? shared_state_->copied_frames
                                                    : 0) +
        " dropped=" + std::to_string(shared_state_ ? shared_state_->dropped_frames
                                                    : 0) +
        " overwritten=" +
        std::to_string(shared_state_ ? shared_state_->overwritten_frames : 0) +
        " gpuScaled=" + std::to_string(gpu_scaled_frames_) +
        " gpuScaleFailures=" + std::to_string(gpu_scale_failures_) +
        " cpuFallback=" + std::to_string(cpu_fallback_frames_) +
        " readbackQueued=" + std::to_string(gpu_readback_queued_frames_) +
        " readbackReady=" + std::to_string(gpu_readback_ready_frames_) +
        " readbackNotReady=" +
        std::to_string(gpu_readback_not_ready_frames_) +
        " readbackOverwritten=" +
        std::to_string(gpu_readback_overwritten_frames_) +
        " readbackMapAttempts=" +
        std::to_string(gpu_readback_map_attempts_) +
        " gpuScaleMs=" + std::to_string(gpu_scale_ms) +
        " copyMs=" + std::to_string(copy_ms) +
        " mapMs=" + std::to_string(map_ms) +
        " readbackLatencyMs=" + std::to_string(readback_latency_ms) +
        " readbackLatencyFramesAvg=" +
        std::to_string(readback_latency_frames_avg) +
        " readbackLatencyFramesMax=" +
        std::to_string(readback_latency_frames_max_) +
        " convertMs=" + std::to_string(convert_ms) +
        " mapFailures=" + std::to_string(map_failures_) +
        " convertFailures=" + std::to_string(convert_failures_) +
        " proofFrames=" + std::to_string(proof_frames_written_) +
        " visibleProofFrames=" + std::to_string(visible_proof_frames_) +
        " i420ProofFrames=" + std::to_string(i420_proof_frames_written_) +
        " visibleI420ProofFrames=" +
        std::to_string(visible_i420_proof_frames_) +
        " initialBlackSkipped=" +
        std::to_string(initial_black_skipped_frames_) +
        " visibleSourceSeen=" +
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
  }

  void MaybeWriteProof(const uint8_t* bgra,
                       int output_width,
                       int output_height,
                       int stride,
                       int source_width,
                       int source_height,
                       DXGI_FORMAT format,
                       const BgraVisibilityStats& stats) {
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
    Log("proof frame=" + std::to_string(proof_frames_written_) +
        " source=" + std::to_string(source_width) + "x" +
        std::to_string(source_height) + " output=" +
        std::to_string(output_width) + "x" + std::to_string(output_height) +
        " format=" + std::to_string(format) +
        " visible=" + std::string(stats.visible ? "true" : "false") +
        " minLuma=" + std::to_string(stats.min_luma) +
        " maxLuma=" + std::to_string(stats.max_luma) +
        " nonzeroSamples=" + std::to_string(stats.nonzero_samples) +
        " samples=" + std::to_string(stats.samples) +
        " wrote=" + std::string(wrote ? "true" : "false") +
        " path=\"" + path_utf8 + "\"");
  }

  void MaybeWriteI420Proof(const webrtc::scoped_refptr<webrtc::I420Buffer>& i420,
                           int output_width,
                           int output_height,
                           int source_width,
                           int source_height,
                           DXGI_FORMAT format,
                           bool source_visible) {
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
        i420->DataV(), i420->StrideV(), i420_proof_bgra_buffer_.data(),
        stride, output_width, output_height);
    if (rc != 0) {
      ++i420_proof_frames_written_;
      Log("i420_proof frame=" + std::to_string(i420_proof_frames_written_) +
          " source=" + std::to_string(source_width) + "x" +
          std::to_string(source_height) + " output=" +
          std::to_string(output_width) + "x" + std::to_string(output_height) +
          " format=" + std::to_string(format) +
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
      swprintf_s(name, L"\\%S-i420-%03llu.bmp", session_id_.c_str(),
                 static_cast<unsigned long long>(
                     i420_proof_frames_written_ + 1));
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
        std::to_string(source_height) + " output=" +
        std::to_string(output_width) + "x" + std::to_string(output_height) +
        " format=" + std::to_string(format) +
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
    session_id_ = HexSessionId();
    if (!EnsureD3dDevice() || !LaunchHelper(session_id_) ||
        !OpenSharedState(session_id_)) {
      Cleanup();
      started_.store(false);
      return;
    }

    const int64_t frame_interval_qpc =
        std::max<int64_t>(1, frequency_.QuadPart / target_fps_);
    int64_t next_due_qpc = 0;
    uint64_t last_frame_index = 0;
    ComPtr<ID3D11Texture2D> latest_texture;
    uint64_t latest_frame_index = 0;

    while (!stop_requested_.load()) {
      DWORD wait_ms = 250;
      LARGE_INTEGER now{};
      QueryPerformanceCounter(&now);
      if (next_due_qpc != 0 && now.QuadPart >= next_due_qpc) {
        wait_ms = 0;
      } else if (next_due_qpc != 0) {
        const int64_t delta_qpc = next_due_qpc - now.QuadPart;
        wait_ms = static_cast<DWORD>(std::clamp<int64_t>(
            (delta_qpc * 1000 + frequency_.QuadPart - 1) /
                frequency_.QuadPart,
            1, 250));
      }

      const DWORD wait_result = WaitForSingleObject(frame_event_, wait_ms);
      if (wait_result == WAIT_OBJECT_0) {
        if (shared_state_ != nullptr &&
            shared_state_->magic == kProtocolMagic &&
            shared_state_->version == kSharedTextureStateVersion) {
          OpenTexturesForGeneration();
          const uint32_t slot = shared_state_->latest_slot_index;
          const uint64_t frame_index = shared_state_->latest_frame_index;
          if (slot < kRingDepth && frame_index != 0 &&
              textures_[slot] != nullptr) {
            latest_texture = textures_[slot];
            latest_frame_index = frame_index;
          }
        }
      }

      QueryPerformanceCounter(&now);
      if (next_due_qpc == 0) {
        next_due_qpc = now.QuadPart;
      }
      if (latest_texture != nullptr && now.QuadPart >= next_due_qpc) {
        const bool repeated = latest_frame_index == last_frame_index;
        if (SubmitTexture(latest_texture.Get(), repeated)) {
          last_frame_index = latest_frame_index;
        }
        next_due_qpc = now.QuadPart + frame_interval_qpc;
      }
    }

    Log("stop requested submitted=" + std::to_string(submitted_frames_));
    Cleanup();
    started_.store(false);
  }

  void Cleanup() {
    HANDLE stop_event = stop_event_;
    if (stop_event != nullptr) {
      Log("cleanup signaling_stop_event");
      SetEvent(stop_event);
    }
    if (helper_process_ != nullptr) {
      const DWORD wait_result = WaitForSingleObject(helper_process_, 5000);
      if (wait_result == WAIT_OBJECT_0) {
        Log("cleanup helper_exited");
      } else {
        Log("cleanup helper_stop_timeout result=" +
            std::to_string(wait_result) + " terminating=true");
        TerminateProcess(helper_process_, 0);
        WaitForSingleObject(helper_process_, 1000);
      }
      CloseHandle(helper_process_);
      helper_process_ = nullptr;
    }
    if (shared_state_ != nullptr) {
      UnmapViewOfFile(shared_state_);
      shared_state_ = nullptr;
    }
    if (shared_mapping_ != nullptr) {
      CloseHandle(shared_mapping_);
      shared_mapping_ = nullptr;
    }
    if (frame_event_ != nullptr) {
      CloseHandle(frame_event_);
      frame_event_ = nullptr;
    }
    if (stop_event_ != nullptr) {
      CloseHandle(stop_event_);
      stop_event_ = nullptr;
    }
    ResetSharedTextures();
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
  uint32_t target_process_id_ = 0;
  size_t max_width_ = 1280;
  size_t max_height_ = 720;
  size_t target_fps_ = 30;
  std::string session_id_;
  std::atomic<bool> started_{false};
  std::atomic<bool> stop_requested_{false};
  std::thread worker_;
  HANDLE helper_process_ = nullptr;
  HANDLE stop_event_ = nullptr;
  HANDLE frame_event_ = nullptr;
  HANDLE shared_mapping_ = nullptr;
  SharedTextureState* shared_state_ = nullptr;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  HMODULE d3dcompiler_module_ = nullptr;
  D3DCompileProc d3d_compile_ = nullptr;
  ComPtr<ID3D11Texture2D> textures_[kRingDepth];
  ComPtr<ID3D11Texture2D> staging_texture_;
  ComPtr<ID3D11VertexShader> gpu_vertex_shader_;
  ComPtr<ID3D11PixelShader> gpu_pixel_shader_;
  ComPtr<ID3D11SamplerState> gpu_sampler_state_;
  ComPtr<ID3D11RenderTargetView> gpu_scaled_rtv_;
  ComPtr<ID3D11Texture2D> gpu_scaled_texture_;
  GpuReadbackSlot gpu_readback_slots_[kGpuReadbackRingDepth];
  std::map<ID3D11Texture2D*, ComPtr<ID3D11ShaderResourceView>>
      gpu_source_shader_views_;
  uint64_t opened_generation_ = 0;
  UINT staging_width_ = 0;
  UINT staging_height_ = 0;
  DXGI_FORMAT staging_format_ = DXGI_FORMAT_UNKNOWN;
  UINT last_source_width_ = 0;
  UINT last_source_height_ = 0;
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
  uint64_t cpu_fallback_frames_ = 0;
  bool gpu_scale_failure_logged_ = false;
  size_t gpu_readback_write_index_ = 0;
  uint64_t gpu_readback_sequence_ = 0;
  uint64_t gpu_readback_queued_frames_ = 0;
  uint64_t gpu_readback_ready_frames_ = 0;
  uint64_t gpu_readback_not_ready_frames_ = 0;
  uint64_t gpu_readback_overwritten_frames_ = 0;
  uint64_t gpu_readback_map_attempts_ = 0;
  int64_t readback_copy_us_ = 0;
  int64_t readback_map_us_ = 0;
  int64_t readback_latency_us_ = 0;
  uint64_t readback_latency_frames_total_ = 0;
  uint64_t readback_latency_frames_max_ = 0;
  int64_t convert_us_ = 0;
  int64_t gpu_scale_us_ = 0;
  int last_output_width_ = 0;
  int last_output_height_ = 0;
  DXGI_FORMAT unsupported_format_ = DXGI_FORMAT_UNKNOWN;
};

}  // namespace

std::shared_ptr<webrtc::internal::VideoCapturer>
CreateIntergalacticGameCaptureVideoCapturer(webrtc::Thread* worker_thread,
                                            const char* helper_path,
                                            uint32_t target_process_id,
                                            size_t max_width,
                                            size_t max_height,
                                            size_t target_fps) {
  (void)worker_thread;
  const std::wstring requested_helper = Utf8ToWide(helper_path);
  const std::wstring resolved_helper = ResolveHelperPath(requested_helper);
  if (target_process_id == 0 || resolved_helper.empty()) {
    AppendNativeDiagnosticLine(
        "game_capture_webrtc_source unavailable reason=missing_pid_or_helper");
    return nullptr;
  }
  return std::make_shared<IntergalacticGameCaptureVideoCapturer>(
      resolved_helper, target_process_id, max_width, max_height, target_fps);
}

extern "C" __declspec(dllexport) int __stdcall
InterGalacticGameCaptureWebrtcSourceSmoke(const wchar_t* helper_path,
                                          uint32_t target_process_id,
                                          uint32_t max_width,
                                          uint32_t max_height,
                                          uint32_t target_fps,
                                          uint32_t duration_ms,
                                          const wchar_t* output_json_path) {
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

  IntergalacticGameCaptureVideoCapturer capturer(
      resolved_helper, target_process_id, max_width, max_height, target_fps);
  if (!capturer.StartCapture()) {
    WriteUtf8File(output_json_path, capturer.SmokeStatsJson("start_failed"));
    return 3;
  }

  Sleep(std::clamp<uint32_t>(duration_ms, 1000, 30000));
  capturer.StopCapture();
  const std::string json = capturer.SmokeStatsJson("completed");
  if (!WriteUtf8File(output_json_path, json)) {
    return 4;
  }
  return 0;
}

}  // namespace libwebrtc
