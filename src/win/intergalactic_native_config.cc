#include "src/win/intergalactic_native_config.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

#include "rtc_base/logging.h"

namespace intergalactic {
namespace win {

EnvironmentValueStatus ReadEnvironmentValue(const char* name, char* buffer,
                                            size_t buffer_size,
                                            size_t* value_length,
                                            bool log_overlong) {
  if (value_length != nullptr) {
    *value_length = 0;
  }
  if (buffer == nullptr || buffer_size == 0) {
    return EnvironmentValueStatus::kMissing;
  }
  buffer[0] = '\0';

  const DWORD length =
      GetEnvironmentVariableA(name, buffer, static_cast<DWORD>(buffer_size));
  if (length == 0) {
    return EnvironmentValueStatus::kMissing;
  }
  if (length >= buffer_size) {
    buffer[buffer_size - 1] = '\0';
    if (log_overlong) {
      RTC_LOG(LS_WARNING) << "Inter Galactic: ignoring overlong " << name
                          << " value";
    }
    return EnvironmentValueStatus::kOverlong;
  }
  if (value_length != nullptr) {
    *value_length = static_cast<size_t>(length);
  }
  return EnvironmentValueStatus::kPresent;
}

bool TryReadPositiveUnsignedEnvironment(const char* name,
                                        unsigned long* value) {
  char buffer[64] = {};
  size_t length = 0;
  if (ReadEnvironmentValue(name, buffer, sizeof(buffer), &length) !=
      EnvironmentValueStatus::kPresent) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    if (!std::isdigit(static_cast<unsigned char>(buffer[i]))) {
      return false;
    }
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(buffer, &end, 10);
  if (end == buffer || *end != '\0' || parsed == 0) {
    return false;
  }
  *value = parsed;
  return true;
}

size_t ReadClampedSizeEnvironment(const char* name, size_t default_value,
                                  size_t min_value, size_t max_value) {
  unsigned long parsed = 0;
  if (!TryReadPositiveUnsignedEnvironment(name, &parsed)) {
    return default_value;
  }
  return static_cast<size_t>(
      std::clamp<unsigned long>(parsed, static_cast<unsigned long>(min_value),
                                static_cast<unsigned long>(max_value)));
}

bool ReadBooleanEnvironment(const char* name) {
  char buffer[16] = {};
  size_t length = 0;
  if (ReadEnvironmentValue(name, buffer, sizeof(buffer), &length) !=
      EnvironmentValueStatus::kPresent) {
    return false;
  }
  std::string value(buffer, buffer + length);
  std::transform(value.begin(), value.end(), value.begin(), [](char ch) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  });
  return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool ReadEnvironmentFlag(const char* name, bool default_value) {
  char buffer[32] = {};
  const EnvironmentValueStatus status =
      ReadEnvironmentValue(name, buffer, sizeof(buffer), nullptr, true);
  if (status == EnvironmentValueStatus::kMissing) {
    return default_value;
  }
  if (status == EnvironmentValueStatus::kOverlong) {
    return default_value;
  }
  if (_stricmp(buffer, "1") == 0 || _stricmp(buffer, "true") == 0 ||
      _stricmp(buffer, "yes") == 0 || _stricmp(buffer, "on") == 0) {
    return true;
  }
  if (_stricmp(buffer, "0") == 0 || _stricmp(buffer, "false") == 0 ||
      _stricmp(buffer, "no") == 0 || _stricmp(buffer, "off") == 0) {
    return false;
  }
  RTC_LOG(LS_WARNING) << "Inter Galactic: ignoring invalid " << name
                      << " value '" << buffer << "'";
  return default_value;
}

uint32_t ReadEnvironmentUint32(const char* name, uint32_t default_value,
                               uint32_t minimum_value, uint32_t maximum_value) {
  char buffer[32] = {};
  const EnvironmentValueStatus status =
      ReadEnvironmentValue(name, buffer, sizeof(buffer), nullptr, true);
  if (status == EnvironmentValueStatus::kMissing) {
    return default_value;
  }
  if (status == EnvironmentValueStatus::kOverlong) {
    return default_value;
  }

  char* end = nullptr;
  const unsigned long parsed = std::strtoul(buffer, &end, 10);
  if (end == buffer || end == nullptr || *end != '\0' ||
      parsed < minimum_value || parsed > maximum_value) {
    RTC_LOG(LS_WARNING) << "Inter Galactic: ignoring invalid " << name
                        << " value '" << buffer << "'";
    return default_value;
  }
  return static_cast<uint32_t>(parsed);
}

}  // namespace win
}  // namespace intergalactic
