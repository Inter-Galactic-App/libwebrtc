#include "src/win/intergalactic_game_capture_config.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include "src/win/intergalactic_native_config.h"

namespace intergalactic {
namespace win {

namespace {

constexpr size_t kGpuNv12RingDepth = 16;
constexpr size_t kDefaultDeliveryQueueDepth = 1;
constexpr size_t kMaxDiagnosticDeliveryQueueDepth = 2;
constexpr size_t kDefaultGpuNv12ReadyDrainDepth = 1;
constexpr size_t kMaxDiagnosticGpuNv12ReadyDrainDepth = 2;
constexpr uint32_t kGpuNv12PendingPollMs = 8;
constexpr size_t kGpuNv12MaxPendingSlots = kGpuNv12RingDepth;
constexpr size_t kDefaultGpuNv12MaxPendingSlots = 2;
constexpr uint32_t kDefaultNativeNv12OnFrameBackpressureThresholdMs = 20;
constexpr uint32_t kDefaultNativeNv12OnFrameBackpressureFrameLimit = 3;

constexpr NativeNv12ReadyPolicy kDefaultNativeNv12ReadyPolicy =
    NativeNv12ReadyPolicy::kFence;
constexpr DeliveryRepeatPolicy kDefaultDeliveryRepeatPolicy =
    DeliveryRepeatPolicy::kSkipOnMiss;

std::string LowerAscii(const char* value, size_t length) {
  std::string text(value, value + length);
  std::transform(text.begin(), text.end(), text.begin(), [](char ch) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  });
  return text;
}

}  // namespace

DeliveryRepeatPolicy ReadDeliveryRepeatPolicy() {
  char buffer[64] = {};
  size_t length = 0;
  if (ReadEnvironmentValue("INTERGALACTIC_GAME_CAPTURE_REPEAT_POLICY", buffer,
                           sizeof(buffer),
                           &length) != EnvironmentValueStatus::kPresent) {
    return kDefaultDeliveryRepeatPolicy;
  }
  const std::string value = LowerAscii(buffer, length);
  if (value == "skip-on-miss" || value == "skip") {
    return DeliveryRepeatPolicy::kSkipOnMiss;
  }
  if (value == "repeat-last-frame" || value == "repeat") {
    return DeliveryRepeatPolicy::kRepeatLastFrame;
  }
  return kDefaultDeliveryRepeatPolicy;
}

NativeNv12ReadyPolicy ReadNativeNv12ReadyPolicy() {
  char buffer[64] = {};
  size_t length = 0;
  if (ReadEnvironmentValue("INTERGALACTIC_GAME_CAPTURE_NV12_READY_POLICY",
                           buffer, sizeof(buffer),
                           &length) != EnvironmentValueStatus::kPresent) {
    return kDefaultNativeNv12ReadyPolicy;
  }
  const std::string value = LowerAscii(buffer, length);
  if (value == "queue-after-blt" || value == "immediate" ||
      value == "immediate-after-blt") {
    return NativeNv12ReadyPolicy::kQueueAfterBlt;
  }
  if (value == "event-query" || value == "query") {
    return NativeNv12ReadyPolicy::kEventQuery;
  }
  if (value == "fence" || value == "explicit-fence") {
    return NativeNv12ReadyPolicy::kFence;
  }
  return kDefaultNativeNv12ReadyPolicy;
}

size_t ReadDeliveryQueueDepth() {
  return ReadClampedSizeEnvironment(
      "INTERGALACTIC_GAME_CAPTURE_DELIVERY_QUEUE_DEPTH",
      kDefaultDeliveryQueueDepth, 1, kMaxDiagnosticDeliveryQueueDepth);
}

size_t ReadNativeNv12ReadyDrainDepth() {
  return ReadClampedSizeEnvironment(
      "INTERGALACTIC_GAME_CAPTURE_NV12_READY_DRAIN_DEPTH",
      kDefaultGpuNv12ReadyDrainDepth, 1, kMaxDiagnosticGpuNv12ReadyDrainDepth);
}

uint32_t ReadNativeNv12PendingPollMs() {
  unsigned long parsed = 0;
  if (!TryReadPositiveUnsignedEnvironment(
          "INTERGALACTIC_GAME_CAPTURE_NV12_PENDING_POLL_MS", &parsed)) {
    return kGpuNv12PendingPollMs;
  }
  return static_cast<uint32_t>(std::clamp<unsigned long>(parsed, 1, 50));
}

size_t ReadNativeNv12MaxPendingSlots() {
  unsigned long parsed = 0;
  if (!TryReadPositiveUnsignedEnvironment(
          "INTERGALACTIC_GAME_CAPTURE_NV12_MAX_PENDING", &parsed)) {
    return kDefaultGpuNv12MaxPendingSlots;
  }
  return static_cast<size_t>(std::clamp<unsigned long>(
      parsed, 1, static_cast<unsigned long>(kGpuNv12MaxPendingSlots)));
}

bool ReadNativeNv12OnFrameBackpressureSuspendEnabled() {
  return ReadEnvironmentFlag(
      "INTERGALACTIC_GAME_CAPTURE_NV12_ONFRAME_BACKPRESSURE_SUSPEND", true);
}

uint32_t ReadNativeNv12OnFrameBackpressureThresholdMs() {
  return ReadEnvironmentUint32(
      "INTERGALACTIC_GAME_CAPTURE_NV12_ONFRAME_BACKPRESSURE_MS",
      kDefaultNativeNv12OnFrameBackpressureThresholdMs, 0, 1000);
}

uint32_t ReadNativeNv12OnFrameBackpressureFrameLimit() {
  return ReadEnvironmentUint32(
      "INTERGALACTIC_GAME_CAPTURE_NV12_ONFRAME_BACKPRESSURE_FRAMES",
      kDefaultNativeNv12OnFrameBackpressureFrameLimit, 1, 120);
}

bool ReadNativeNv12SingleInFlightEnabled() {
  return ReadEnvironmentFlag(
      "INTERGALACTIC_GAME_CAPTURE_NV12_SINGLE_IN_FLIGHT", true);
}

const char* DeliveryRepeatPolicyName(DeliveryRepeatPolicy policy) {
  switch (policy) {
    case DeliveryRepeatPolicy::kSkipOnMiss:
      return "skip-on-miss";
    case DeliveryRepeatPolicy::kRepeatLastFrame:
    default:
      return "repeat-last-frame";
  }
}

const char* NativeNv12ReadyPolicyName(NativeNv12ReadyPolicy policy) {
  switch (policy) {
    case NativeNv12ReadyPolicy::kQueueAfterBlt:
      return "queue-after-blt";
    case NativeNv12ReadyPolicy::kFence:
      return "fence";
    case NativeNv12ReadyPolicy::kEventQuery:
    default:
      return "event-query";
  }
}

GameCaptureSourceMode GameCaptureSourceModeFromString(const char* source_mode) {
  if (source_mode == nullptr || source_mode[0] == '\0') {
    return GameCaptureSourceMode::kHelperD3d11;
  }
  const std::string value = LowerAscii(source_mode, std::strlen(source_mode));
  if (value == "dummy-nv12-live-sender" || value == "dummy-nv12" ||
      value == "synthetic-nv12-live-sender" || value == "synthetic-nv12") {
    return GameCaptureSourceMode::kDummyNv12LiveSender;
  }
  return GameCaptureSourceMode::kHelperD3d11;
}

const char* GameCaptureSourceModeName(GameCaptureSourceMode mode) {
  switch (mode) {
    case GameCaptureSourceMode::kDummyNv12LiveSender:
      return "dummy-nv12-live-sender";
    case GameCaptureSourceMode::kHelperD3d11:
    default:
      return "helper-d3d11";
  }
}

}  // namespace win
}  // namespace intergalactic
