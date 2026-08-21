#ifndef SRC_WIN_INTERGALACTIC_PROOF_FRAME_WRITER_H_
#define SRC_WIN_INTERGALACTIC_PROOF_FRAME_WRITER_H_

#include <stdint.h>

#include <string>

namespace intergalactic {
namespace win {

struct BgraVisibilityStats {
  int min_luma = 255;
  int max_luma = 0;
  uint64_t nonzero_samples = 0;
  uint64_t samples = 0;
  bool visible = false;
};

std::wstring WebrtcProofDirectory();

bool WriteBgraBmp(const std::wstring& path, int width, int height, int stride,
                  const uint8_t* bgra);

BgraVisibilityStats AnalyzeBgra(const uint8_t* bgra, int width, int height,
                                int stride);

}  // namespace win
}  // namespace intergalactic

#endif  // SRC_WIN_INTERGALACTIC_PROOF_FRAME_WRITER_H_
