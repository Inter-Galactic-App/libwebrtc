#ifndef SRC_WIN_INTERGALACTIC_NATIVE_DIAGNOSTICS_H_
#define SRC_WIN_INTERGALACTIC_NATIVE_DIAGNOSTICS_H_

#include <cstddef>
#include <string>

namespace intergalactic {
namespace win {

struct NativeDiagnosticOptions {
  size_t max_file_bytes = 512 * 1024;
  bool flush_file = true;
  bool log_to_rtc = false;
};

void AppendNativeWebrtcDiagnosticLine(
    const std::string& line,
    const NativeDiagnosticOptions& options = NativeDiagnosticOptions());

}  // namespace win
}  // namespace intergalactic

#endif  // SRC_WIN_INTERGALACTIC_NATIVE_DIAGNOSTICS_H_
