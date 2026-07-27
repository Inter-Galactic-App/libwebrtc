#ifndef LIB_WEBRTC_RTC_INTERGALACTIC_AUDIO_DUCKING_HXX
#define LIB_WEBRTC_RTC_INTERGALACTIC_AUDIO_DUCKING_HXX

#include "rtc_types.h"

// Inter Galactic: Windows call-audio stream attenuation ("ducking") control.
//
// Windows attenuates other applications' audio while a communications stream
// is active. The Windows ADM inside this DLL opens its render stream with the
// communications category/role, so Inter Galactic calls duck whatever else the
// user is listening to. These entry points let the app opt that render stream
// out of *causing* the attenuation, via
// IAudioClientDuckingControl::SetDuckingOptionsForCurrentStream().
//
// This only asks Windows not to duck on Inter Galactic's behalf. It cannot
// turn on ducking that the user disabled in mmsys.cpl > Communications, and it
// does not change any system setting.
//
// Requires Windows 10 build 20348 or newer. On older builds the call is
// accepted and recorded but the system keeps ducking; the native log records
// the E_NOINTERFACE once.
//
// Resolved from Dart by name through the already-loaded libwebrtc.dll, so the
// C linkage and the exact spelling of these symbols are load-bearing.

extern "C" {

// enabled == true  -> AUDIO_DUCKING_OPTIONS_DEFAULT (Windows may duck others)
// enabled == false -> AUDIO_DUCKING_OPTIONS_DO_NOT_DUCK_OTHER_STREAMS
//
// Applies to render streams opened from now on, and immediately to any render
// stream that is already live, so a settings toggle does not need a call
// restart. Safe to call before, during, or outside a call, and on any thread.
LIB_WEBRTC_API void IntergalacticSetCallAudioDuckingEnabled(bool enabled);

// Whether this binary was built against an SDK that exposes
// IAudioClientDuckingControl at all. False means the toggle can never take
// effect with this artifact regardless of the running Windows build.
LIB_WEBRTC_API bool IntergalacticCallAudioDuckingSupported();
}

#endif  // LIB_WEBRTC_RTC_INTERGALACTIC_AUDIO_DUCKING_HXX
