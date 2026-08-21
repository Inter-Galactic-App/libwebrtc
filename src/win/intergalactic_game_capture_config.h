#ifndef SRC_WIN_INTERGALACTIC_GAME_CAPTURE_CONFIG_H_
#define SRC_WIN_INTERGALACTIC_GAME_CAPTURE_CONFIG_H_

#include <cstddef>
#include <cstdint>

namespace intergalactic {
namespace win {

enum class DeliveryRepeatPolicy {
  kRepeatLastFrame,
  kSkipOnMiss,
};

enum class NativeNv12ReadyPolicy {
  kEventQuery,
  kFence,
  kQueueAfterBlt,
};

enum class GameCaptureSourceMode {
  kHelperD3d11,
  kDummyNv12LiveSender,
};

DeliveryRepeatPolicy ReadDeliveryRepeatPolicy();

NativeNv12ReadyPolicy ReadNativeNv12ReadyPolicy();

size_t ReadDeliveryQueueDepth();

size_t ReadNativeNv12ReadyDrainDepth();

uint32_t ReadNativeNv12PendingPollMs();

size_t ReadNativeNv12MaxPendingSlots();

bool ReadNativeNv12OnFrameBackpressureSuspendEnabled();

uint32_t ReadNativeNv12OnFrameBackpressureThresholdMs();

uint32_t ReadNativeNv12OnFrameBackpressureFrameLimit();

bool ReadNativeNv12SingleInFlightEnabled();

const char* DeliveryRepeatPolicyName(DeliveryRepeatPolicy policy);

const char* NativeNv12ReadyPolicyName(NativeNv12ReadyPolicy policy);

GameCaptureSourceMode GameCaptureSourceModeFromString(const char* source_mode);

const char* GameCaptureSourceModeName(GameCaptureSourceMode mode);

}  // namespace win
}  // namespace intergalactic

#endif  // SRC_WIN_INTERGALACTIC_GAME_CAPTURE_CONFIG_H_
