#ifndef INTERGALACTIC_D3D11_NV12_BUFFER_H_
#define INTERGALACTIC_D3D11_NV12_BUFFER_H_

#include <d3d11.h>
#include <d3d11_3.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

#include "api/scoped_refptr.h"
#include "api/video/video_frame_buffer.h"
#if __has_include("src/win/nativehandlebuffer.h")
#include "src/win/nativehandlebuffer.h"
#else
#include "libwebrtc/src/win/nativehandlebuffer.h"
#endif

namespace owt {
namespace base {

class IntergalacticD3D11Nv12Buffer : public NativeHandleBuffer {
 public:
  struct Handle {
    uint32_t magic = 0;
    ID3D11Device* device = nullptr;
    ID3D11Texture2D* texture = nullptr;
    ID3D11Fence* ready_fence = nullptr;
    uint64_t ready_fence_value = 0;
    int width = 0;
    int height = 0;
    uint32_t source_format = 0;
    uint64_t source_frame_index = 0;
    uint64_t source_qpc = 0;
    int64_t created_qpc = 0;
    double source_age_at_create_ms = 0.0;
  };

  struct Metadata {
    std::string source_mode;
    uint32_t source_format = 0;
    uint64_t source_frame_index = 0;
    uint64_t source_qpc = 0;
    int64_t created_qpc = 0;
    double source_age_at_create_ms = 0.0;
  };

  static constexpr uint32_t kMagic = 0x49474E56u;  // IGNV

  static webrtc::scoped_refptr<IntergalacticD3D11Nv12Buffer> Create(
      Microsoft::WRL::ComPtr<ID3D11Device> device,
      Microsoft::WRL::ComPtr<ID3D11Texture2D> texture, int width, int height,
      Microsoft::WRL::ComPtr<ID3D11Fence> ready_fence,
      uint64_t ready_fence_value, Metadata metadata);

  static IntergalacticD3D11Nv12Buffer* FromVideoFrameBuffer(
      webrtc::VideoFrameBuffer* buffer);

  IntergalacticD3D11Nv12Buffer(Microsoft::WRL::ComPtr<ID3D11Device> device,
                               Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
                               int width, int height,
                               Microsoft::WRL::ComPtr<ID3D11Fence> ready_fence,
                               uint64_t ready_fence_value, Metadata metadata);
  ~IntergalacticD3D11Nv12Buffer() override;

  NativeHandleBufferKind native_handle_kind() const override {
    return NativeHandleBufferKind::kIntergalacticD3D11Nv12;
  }
  webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override;
  HRESULT WaitForReadyFence(DWORD timeout_ms) const;

  ID3D11Device* device() const { return device_.Get(); }
  ID3D11Texture2D* texture() const { return texture_.Get(); }
  ID3D11Fence* ready_fence() const { return ready_fence_.Get(); }
  uint64_t ready_fence_value() const { return ready_fence_value_; }
  const std::string& source_mode() const { return metadata_.source_mode; }
  uint32_t source_format() const { return metadata_.source_format; }
  uint64_t source_frame_index() const { return metadata_.source_frame_index; }
  uint64_t source_qpc() const { return metadata_.source_qpc; }
  int64_t created_qpc() const { return metadata_.created_qpc; }
  double source_age_at_create_ms() const {
    return metadata_.source_age_at_create_ms;
  }

 private:
  Handle handle_;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
  Microsoft::WRL::ComPtr<ID3D11Fence> ready_fence_;
  uint64_t ready_fence_value_ = 0;
  Metadata metadata_;
};

}  // namespace base
}  // namespace owt

#endif  // INTERGALACTIC_D3D11_NV12_BUFFER_H_
