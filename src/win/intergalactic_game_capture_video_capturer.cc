#include "src/win/intergalactic_game_capture_video_capturer.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
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
constexpr int kMaxHelperStartWaitMs = 6000;
constexpr int kLiveDurationMs = 60 * 60 * 1000;

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
    started_.store(false);
    stop_requested_.store(true);
    HANDLE stop_event = stop_event_;
    if (stop_event != nullptr) {
      SetEvent(stop_event);
    }
    if (worker_.joinable() &&
        worker_.get_id() != std::this_thread::get_id()) {
      worker_.join();
    }
  }

 private:
  void Log(const std::string& message) {
    AppendNativeDiagnosticLine("game_capture_webrtc_source " + message);
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

    MaybeWriteProof(scaled_argb, output_width, output_height, scaled_stride,
                    source_width, source_height, desc.Format);

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
                        source_height, desc.Format);

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
    const double fps =
        elapsed_seconds > 0.0 ? submitted / elapsed_seconds : 0.0;
    const double copy_ms = submitted == 0
                               ? 0.0
                               : (static_cast<double>(readback_copy_us_) *
                                  1000.0 / frequency_.QuadPart / submitted);
    const double map_ms = submitted == 0
                              ? 0.0
                              : (static_cast<double>(readback_map_us_) *
                                 1000.0 / frequency_.QuadPart / submitted);
    const double convert_ms = submitted == 0
                                  ? 0.0
                                  : (static_cast<double>(convert_us_) *
                                     1000.0 / frequency_.QuadPart / submitted);
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
        " copyMs=" + std::to_string(copy_ms) +
        " mapMs=" + std::to_string(map_ms) +
        " convertMs=" + std::to_string(convert_ms) +
        " mapFailures=" + std::to_string(map_failures_) +
        " convertFailures=" + std::to_string(convert_failures_) +
        " proofFrames=" + std::to_string(proof_frames_written_) +
        " visibleProofFrames=" + std::to_string(visible_proof_frames_) +
        " i420ProofFrames=" + std::to_string(i420_proof_frames_written_) +
        " visibleI420ProofFrames=" +
        std::to_string(visible_i420_proof_frames_));
    stats_start_qpc_ = now.QuadPart;
    stats_start_frames_ = submitted_frames_;
    readback_copy_us_ = 0;
    readback_map_us_ = 0;
    convert_us_ = 0;
  }

  void MaybeWriteProof(const uint8_t* bgra,
                       int output_width,
                       int output_height,
                       int stride,
                       int source_width,
                       int source_height,
                       DXGI_FORMAT format) {
    if (proof_frames_written_ >= 2) {
      return;
    }
    const BgraVisibilityStats stats =
        AnalyzeBgra(bgra, output_width, output_height, stride);
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
                           DXGI_FORMAT format) {
    if (i420 == nullptr || i420_proof_frames_written_ >= 1) {
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
      SetEvent(stop_event);
    }
    if (helper_process_ != nullptr) {
      WaitForSingleObject(helper_process_, 5000);
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
    context_.Reset();
    device_.Reset();
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
  ComPtr<ID3D11Texture2D> textures_[kRingDepth];
  ComPtr<ID3D11Texture2D> staging_texture_;
  uint64_t opened_generation_ = 0;
  UINT staging_width_ = 0;
  UINT staging_height_ = 0;
  DXGI_FORMAT staging_format_ = DXGI_FORMAT_UNKNOWN;
  std::vector<uint8_t> argb_buffer_;
  std::vector<uint8_t> scaled_argb_buffer_;
  std::vector<uint8_t> i420_proof_bgra_buffer_;
  LARGE_INTEGER frequency_{};
  int64_t stats_start_qpc_ = 0;
  uint64_t stats_start_frames_ = 0;
  uint64_t submitted_frames_ = 0;
  uint64_t repeated_frames_ = 0;
  uint64_t proof_frames_written_ = 0;
  uint64_t visible_proof_frames_ = 0;
  uint64_t i420_proof_frames_written_ = 0;
  uint64_t visible_i420_proof_frames_ = 0;
  uint64_t map_failures_ = 0;
  uint64_t convert_failures_ = 0;
  int64_t readback_copy_us_ = 0;
  int64_t readback_map_us_ = 0;
  int64_t convert_us_ = 0;
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

}  // namespace libwebrtc
