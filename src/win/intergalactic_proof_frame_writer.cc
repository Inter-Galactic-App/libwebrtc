#include "src/win/intergalactic_proof_frame_writer.h"

#include <windows.h>

#include <algorithm>
#include <vector>

namespace intergalactic {
namespace win {
namespace {

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

}  // namespace

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

bool WriteBgraBmp(const std::wstring& path, int width, int height, int stride,
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

  HANDLE file =
      CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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

BgraVisibilityStats AnalyzeBgra(const uint8_t* bgra, int width, int height,
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

}  // namespace win
}  // namespace intergalactic
