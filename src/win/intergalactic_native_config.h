#ifndef SRC_WIN_INTERGALACTIC_NATIVE_CONFIG_H_
#define SRC_WIN_INTERGALACTIC_NATIVE_CONFIG_H_

#include <cstddef>
#include <cstdint>

namespace intergalactic {
namespace win {

enum class EnvironmentValueStatus {
  kMissing,
  kOverlong,
  kPresent,
};

EnvironmentValueStatus ReadEnvironmentValue(const char* name, char* buffer,
                                            size_t buffer_size,
                                            size_t* value_length = nullptr,
                                            bool log_overlong = false);

bool TryReadPositiveUnsignedEnvironment(const char* name, unsigned long* value);

size_t ReadClampedSizeEnvironment(const char* name, size_t default_value,
                                  size_t min_value, size_t max_value);

bool ReadBooleanEnvironment(const char* name);

bool ReadEnvironmentFlag(const char* name, bool default_value);

uint32_t ReadEnvironmentUint32(const char* name, uint32_t default_value,
                               uint32_t minimum_value, uint32_t maximum_value);

}  // namespace win
}  // namespace intergalactic

#endif  // SRC_WIN_INTERGALACTIC_NATIVE_CONFIG_H_
