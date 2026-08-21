#include "src/win/intergalactic_native_diagnostics.h"

#include <windows.h>

#include <cstdio>
#include <mutex>

#include "rtc_base/logging.h"

namespace intergalactic {
namespace win {

void AppendNativeWebrtcDiagnosticLine(const std::string& line,
                                      const NativeDiagnosticOptions& options) {
  if (line.empty()) {
    return;
  }
  if (options.log_to_rtc) {
    RTC_LOG(LS_INFO) << "Inter Galactic " << line;
  }

  static std::mutex file_mutex;
  std::lock_guard<std::mutex> lock(file_mutex);

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

  HANDLE file =
      CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }

  LARGE_INTEGER size = {};
  if (options.max_file_bytes > 0 && GetFileSizeEx(file, &size) &&
      size.QuadPart > static_cast<LONGLONG>(options.max_file_bytes)) {
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
           "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ native-webrtc ", now.wYear,
           now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
           now.wMilliseconds);

  std::string output(prefix);
  output.append(line);
  output.append("\r\n");

  DWORD written = 0;
  WriteFile(file, output.data(), static_cast<DWORD>(output.size()), &written,
            nullptr);
  if (options.flush_file) {
    FlushFileBuffers(file);
  }
  CloseHandle(file);
}

}  // namespace win
}  // namespace intergalactic
