#include "src/win/intergalactic_d3d11_nv12_buffer.h"

#include <mutex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

#include "api/video/i420_buffer.h"
#include "rtc_base/logging.h"
#include "rtc_base/ref_counted_object.h"
#include "third_party/libyuv/include/libyuv/convert.h"

namespace owt {
namespace base {

namespace {

std::mutex& IntergalacticNativeBufferRegistryMutex() {
  static auto* mutex = new std::mutex();
  return *mutex;
}

std::unordered_set<const webrtc::VideoFrameBuffer*>&
IntergalacticNativeBufferRegistry() {
  static auto* registry =
      new std::unordered_set<const webrtc::VideoFrameBuffer*>();
  return *registry;
}

void RegisterIntergalacticNativeBuffer(
    const IntergalacticD3D11Nv12Buffer* buffer) {
  std::lock_guard<std::mutex> lock(IntergalacticNativeBufferRegistryMutex());
  IntergalacticNativeBufferRegistry().insert(buffer);
}

void UnregisterIntergalacticNativeBuffer(
    const IntergalacticD3D11Nv12Buffer* buffer) {
  std::lock_guard<std::mutex> lock(IntergalacticNativeBufferRegistryMutex());
  IntergalacticNativeBufferRegistry().erase(buffer);
}

bool IsRegisteredIntergalacticNativeBuffer(webrtc::VideoFrameBuffer* buffer) {
  std::lock_guard<std::mutex> lock(IntergalacticNativeBufferRegistryMutex());
  return IntergalacticNativeBufferRegistry().find(buffer) !=
         IntergalacticNativeBufferRegistry().end();
}

class ForeignNativeBufferForSmoke : public webrtc::VideoFrameBuffer {
 public:
  Type type() const override { return Type::kNative; }
  int width() const override { return 16; }
  int height() const override { return 16; }
  webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override {
    return nullptr;
  }
};

}  // namespace

webrtc::scoped_refptr<IntergalacticD3D11Nv12Buffer>
IntergalacticD3D11Nv12Buffer::Create(
    Microsoft::WRL::ComPtr<ID3D11Device> device,
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture, int width, int height,
    Microsoft::WRL::ComPtr<ID3D11Fence> ready_fence, uint64_t ready_fence_value,
    Metadata metadata) {
  if (device == nullptr || texture == nullptr || width <= 0 || height <= 0) {
    return nullptr;
  }
  if (ready_fence == nullptr) {
    ready_fence_value = 0;
  }
  return webrtc::scoped_refptr<IntergalacticD3D11Nv12Buffer>(
      new webrtc::RefCountedObject<IntergalacticD3D11Nv12Buffer>(
          std::move(device), std::move(texture), width, height,
          std::move(ready_fence), ready_fence_value, std::move(metadata)));
}

IntergalacticD3D11Nv12Buffer*
IntergalacticD3D11Nv12Buffer::FromVideoFrameBuffer(
    webrtc::VideoFrameBuffer* buffer) {
  if (buffer == nullptr ||
      buffer->type() != webrtc::VideoFrameBuffer::Type::kNative) {
    return nullptr;
  }

  if (!IsRegisteredIntergalacticNativeBuffer(buffer)) {
    return nullptr;
  }

  auto* native = static_cast<IntergalacticD3D11Nv12Buffer*>(buffer);
  if (native->native_handle_kind() !=
      NativeHandleBufferKind::kIntergalacticD3D11Nv12) {
    return nullptr;
  }
  auto* handle = static_cast<Handle*>(native->native_handle());
  if (handle == nullptr || handle != &native->handle_ ||
      handle->magic != kMagic || handle->device == nullptr ||
      handle->texture == nullptr) {
    return nullptr;
  }
  return static_cast<IntergalacticD3D11Nv12Buffer*>(native);
}

IntergalacticD3D11Nv12Buffer::IntergalacticD3D11Nv12Buffer(
    Microsoft::WRL::ComPtr<ID3D11Device> device,
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture, int width, int height,
    Microsoft::WRL::ComPtr<ID3D11Fence> ready_fence, uint64_t ready_fence_value,
    Metadata metadata)
    : NativeHandleBuffer(&handle_, width, height),
      device_(std::move(device)),
      texture_(std::move(texture)),
      ready_fence_(std::move(ready_fence)),
      ready_fence_value_(ready_fence_ == nullptr ? 0 : ready_fence_value),
      metadata_(std::move(metadata)) {
  handle_.magic = kMagic;
  handle_.device = device_.Get();
  handle_.texture = texture_.Get();
  handle_.ready_fence = ready_fence_.Get();
  handle_.ready_fence_value = ready_fence_value_;
  handle_.width = width;
  handle_.height = height;
  handle_.source_format = metadata_.source_format;
  handle_.source_frame_index = metadata_.source_frame_index;
  handle_.source_qpc = metadata_.source_qpc;
  handle_.created_qpc = metadata_.created_qpc;
  handle_.source_age_at_create_ms = metadata_.source_age_at_create_ms;
  RegisterIntergalacticNativeBuffer(this);
}

IntergalacticD3D11Nv12Buffer::~IntergalacticD3D11Nv12Buffer() {
  UnregisterIntergalacticNativeBuffer(this);
}

HRESULT IntergalacticD3D11Nv12Buffer::WaitForReadyFence(
    DWORD timeout_ms) const {
  if (ready_fence_ == nullptr || ready_fence_value_ == 0) {
    return S_FALSE;
  }
  const UINT64 completed_value = ready_fence_->GetCompletedValue();
  if (completed_value == static_cast<UINT64>(-1)) {
    return DXGI_ERROR_DEVICE_REMOVED;
  }
  if (completed_value >= ready_fence_value_) {
    return S_OK;
  }

  HANDLE ready_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (ready_event == nullptr) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  const HRESULT hr =
      ready_fence_->SetEventOnCompletion(ready_fence_value_, ready_event);
  if (FAILED(hr)) {
    CloseHandle(ready_event);
    return hr;
  }

  const DWORD wait_result = WaitForSingleObject(ready_event, timeout_ms);
  CloseHandle(ready_event);
  if (wait_result == WAIT_OBJECT_0) {
    return S_OK;
  }
  if (wait_result == WAIT_TIMEOUT) {
    return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
  }
  if (wait_result == WAIT_FAILED) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  return E_FAIL;
}

webrtc::scoped_refptr<webrtc::I420BufferInterface>
IntergalacticD3D11Nv12Buffer::ToI420() {
  if (device_ == nullptr || texture_ == nullptr || width() <= 0 ||
      height() <= 0) {
    return nullptr;
  }

  const HRESULT fence_hr = WaitForReadyFence(50);
  if (FAILED(fence_hr)) {
    RTC_LOG(LS_WARNING)
        << "Inter Galactic: native NV12 buffer fallback fence wait failed";
    return nullptr;
  }

  D3D11_TEXTURE2D_DESC desc{};
  texture_->GetDesc(&desc);
  if (desc.Format != DXGI_FORMAT_NV12 ||
      desc.Width != static_cast<UINT>(width()) ||
      desc.Height != static_cast<UINT>(height())) {
    RTC_LOG(LS_WARNING)
        << "Inter Galactic: native NV12 buffer fallback saw unexpected "
           "texture format/size";
    return nullptr;
  }

  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  device_->GetImmediateContext(&context);
  if (context == nullptr) {
    return nullptr;
  }

  D3D11_TEXTURE2D_DESC staging_desc = desc;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  staging_desc.MiscFlags = 0;

  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  HRESULT hr = device_->CreateTexture2D(&staging_desc, nullptr, &staging);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING)
        << "Inter Galactic: native NV12 fallback staging texture failed";
    return nullptr;
  }

  context->CopyResource(staging.Get(), texture_.Get());
  context->Flush();

  D3D11_MAPPED_SUBRESOURCE mapped{};
  hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "Inter Galactic: native NV12 fallback map failed";
    return nullptr;
  }

  auto i420 = webrtc::I420Buffer::Create(width(), height());
  const auto* y_plane = static_cast<const uint8_t*>(mapped.pData);
  const auto* uv_plane =
      y_plane + static_cast<size_t>(mapped.RowPitch) * height();
  const int result = libyuv::NV12ToI420(
      y_plane, static_cast<int>(mapped.RowPitch), uv_plane,
      static_cast<int>(mapped.RowPitch), i420->MutableDataY(), i420->StrideY(),
      i420->MutableDataU(), i420->StrideU(), i420->MutableDataV(),
      i420->StrideV(), width(), height());
  context->Unmap(staging.Get(), 0);
  if (result != 0) {
    RTC_LOG(LS_WARNING)
        << "Inter Galactic: native NV12 fallback conversion failed";
    return nullptr;
  }
  return i420;
}

bool WriteUtf8File(const wchar_t* path, const std::string& content) {
  if (path == nullptr || path[0] == L'\0') {
    return false;
  }
  HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD written = 0;
  const BOOL ok =
      WriteFile(file, content.data(), static_cast<DWORD>(content.size()),
                &written, nullptr);
  CloseHandle(file);
  return ok && written == content.size();
}

std::string NativeBufferInteropSmokeJson(bool null_rejected, bool i420_rejected,
                                         bool generic_native_rejected,
                                         bool foreign_native_rejected,
                                         bool forged_magic_rejected,
                                         bool valid_buffer_accepted,
                                         bool invalid_create_rejected) {
  const bool passed = null_rejected && i420_rejected &&
                      generic_native_rejected && foreign_native_rejected &&
                      forged_magic_rejected && valid_buffer_accepted &&
                      invalid_create_rejected;
  std::ostringstream out;
  out << "{\n"
      << "  \"schema\": \"intergalactic.d3d11Nv12BufferInteropSmoke.v1\",\n"
      << "  \"status\": \"" << (passed ? "completed" : "failed") << "\",\n"
      << "  \"nativeHandleKindGuard\": \"enabled\",\n"
      << "  \"nullRejected\": " << (null_rejected ? "true" : "false") << ",\n"
      << "  \"i420Rejected\": " << (i420_rejected ? "true" : "false") << ",\n"
      << "  \"genericNativeRejected\": "
      << (generic_native_rejected ? "true" : "false") << ",\n"
      << "  \"foreignNativeRejected\": "
      << (foreign_native_rejected ? "true" : "false") << ",\n"
      << "  \"forgedMagicGenericNativeRejected\": "
      << (forged_magic_rejected ? "true" : "false") << ",\n"
      << "  \"validIntergalacticNativeAccepted\": "
      << (valid_buffer_accepted ? "true" : "false") << ",\n"
      << "  \"invalidCreateRejected\": "
      << (invalid_create_rejected ? "true" : "false") << "\n"
      << "}\n";
  return out.str();
}

bool TryCreateSmokeDevice(Microsoft::WRL::ComPtr<ID3D11Device>* device_out) {
  if (device_out == nullptr) {
    return false;
  }
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                      D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL selected_level = D3D_FEATURE_LEVEL_11_0;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  HRESULT hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
      D3D11_SDK_VERSION, device_out->GetAddressOf(), &selected_level, &context);
  if (FAILED(hr)) {
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                           ARRAYSIZE(levels), D3D11_SDK_VERSION,
                           device_out->GetAddressOf(), &selected_level,
                           &context);
  }
  return SUCCEEDED(hr) && *device_out != nullptr;
}

bool TryCreateSmokeNv12Texture(
    ID3D11Device* device,
    Microsoft::WRL::ComPtr<ID3D11Texture2D>* texture_out) {
  if (device == nullptr || texture_out == nullptr) {
    return false;
  }
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = 64;
  desc.Height = 64;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_NV12;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  const HRESULT hr =
      device->CreateTexture2D(&desc, nullptr, texture_out->GetAddressOf());
  return SUCCEEDED(hr) && *texture_out != nullptr;
}

bool SmokeAcceptsValidIntergalacticNativeBuffer() {
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  if (!TryCreateSmokeDevice(&device)) {
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  if (!TryCreateSmokeNv12Texture(device.Get(), &texture)) {
    return false;
  }
  IntergalacticD3D11Nv12Buffer::Metadata metadata;
  metadata.source_mode = "interop-smoke";
  metadata.source_format = DXGI_FORMAT_NV12;
  auto buffer = IntergalacticD3D11Nv12Buffer::Create(device, texture, 64, 64,
                                                     nullptr, 0, metadata);
  return buffer != nullptr &&
         IntergalacticD3D11Nv12Buffer::FromVideoFrameBuffer(buffer.get()) ==
             buffer.get();
}

int RunNativeBufferInteropSmoke(const wchar_t* output_json_path) {
  const bool null_rejected =
      IntergalacticD3D11Nv12Buffer::FromVideoFrameBuffer(nullptr) == nullptr;

  auto i420 = webrtc::I420Buffer::Create(16, 16);
  const bool i420_rejected =
      IntergalacticD3D11Nv12Buffer::FromVideoFrameBuffer(i420.get()) == nullptr;

  int generic_handle = 0;
  webrtc::scoped_refptr<NativeHandleBuffer> generic_native(
      new webrtc::RefCountedObject<NativeHandleBuffer>(&generic_handle, 16,
                                                       16));
  const bool generic_native_rejected =
      IntergalacticD3D11Nv12Buffer::FromVideoFrameBuffer(
          generic_native.get()) == nullptr;

  webrtc::scoped_refptr<ForeignNativeBufferForSmoke> foreign_native(
      new webrtc::RefCountedObject<ForeignNativeBufferForSmoke>());
  const bool foreign_native_rejected =
      IntergalacticD3D11Nv12Buffer::FromVideoFrameBuffer(
          foreign_native.get()) == nullptr;

  IntergalacticD3D11Nv12Buffer::Handle forged_handle;
  forged_handle.magic = IntergalacticD3D11Nv12Buffer::kMagic;
  forged_handle.device = reinterpret_cast<ID3D11Device*>(1);
  forged_handle.texture = reinterpret_cast<ID3D11Texture2D*>(1);
  webrtc::scoped_refptr<NativeHandleBuffer> forged_magic_native(
      new webrtc::RefCountedObject<NativeHandleBuffer>(&forged_handle, 16, 16));
  const bool forged_magic_rejected =
      IntergalacticD3D11Nv12Buffer::FromVideoFrameBuffer(
          forged_magic_native.get()) == nullptr;

  const bool valid_buffer_accepted =
      SmokeAcceptsValidIntergalacticNativeBuffer();

  const bool invalid_create_rejected =
      IntergalacticD3D11Nv12Buffer::Create(
          nullptr, nullptr, 16, 16, nullptr, 0,
          IntergalacticD3D11Nv12Buffer::Metadata()) == nullptr;

  const bool passed = null_rejected && i420_rejected &&
                      generic_native_rejected && foreign_native_rejected &&
                      forged_magic_rejected && valid_buffer_accepted &&
                      invalid_create_rejected;
  const std::string json = NativeBufferInteropSmokeJson(
      null_rejected, i420_rejected, generic_native_rejected,
      foreign_native_rejected, forged_magic_rejected, valid_buffer_accepted,
      invalid_create_rejected);
  if (!WriteUtf8File(output_json_path, json)) {
    return 2;
  }
  return passed ? 0 : 1;
}

}  // namespace base
}  // namespace owt

extern "C"
    __declspec(dllexport) int __stdcall InterGalacticD3D11Nv12BufferInteropSmoke(
        const wchar_t* output_json_path) {
  return owt::base::RunNativeBufferInteropSmoke(output_json_path);
}
