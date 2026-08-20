#include "rtc_intergalactic_audio_ducking.h"

#if defined(WEBRTC_WIN)

// Declared in modules/audio_device/win/audio_device_core_win.h. Forward
// declared here rather than included so this translation unit does not pull
// wmcodecdsp.h/audioclient.h and the audio_device target's private headers
// into the wrapper. If either signature changes, change both sides together.
namespace webrtc {
void SetWindowsCallAudioDuckingEnabled(bool enabled);
bool WindowsCallAudioDuckingSupported();
}  // namespace webrtc

extern "C" void IntergalacticSetCallAudioDuckingEnabled(bool enabled) {
  webrtc::SetWindowsCallAudioDuckingEnabled(enabled);
}

extern "C" bool IntergalacticCallAudioDuckingSupported() {
  return webrtc::WindowsCallAudioDuckingSupported();
}

#else

// Ducking is a Windows audio-engine behaviour; nothing to opt out of
// elsewhere. The symbols still exist so a cross-platform caller links.
extern "C" void IntergalacticSetCallAudioDuckingEnabled(bool /*enabled*/) {}

extern "C" bool IntergalacticCallAudioDuckingSupported() {
  return false;
}

#endif  // defined(WEBRTC_WIN)
