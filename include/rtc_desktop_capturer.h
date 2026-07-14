/**
 * @file rtc_desktop_capturer.h
 * This header file defines the interface for capturing desktop media.
 */

#ifndef LIB_WEBRTC_RTC_DESKTOP_CAPTURER_HXX
#define LIB_WEBRTC_RTC_DESKTOP_CAPTURER_HXX

#include "rtc_desktop_media_list.h"
#include "rtc_types.h"
#include "rtc_video_device.h"

namespace libwebrtc {

class DesktopCapturerObserver;

/**
 * @brief The interface for capturing desktop media.
 *
 * This interface defines methods for registering and deregistering observer
 * for desktop capture events, starting and stopping desktop capture, and
 * retrieving the current capture state and media source.
 */
class RTCDesktopCapturer : public RefCountInterface {
 public:
  /**
   * @brief Enumeration for the possible states of desktop capture.
   */
  enum CaptureState { CS_RUNNING, CS_STOPPED, CS_FAILED };

 public:
  /**
   * @brief Registers the given observer for desktop capture events.
   *
   * @param observer Pointer to the observer to be registered.
   */
  virtual void RegisterDesktopCapturerObserver(
      DesktopCapturerObserver* observer) = 0;

  /**
   * @brief Deregisters the currently registered desktop capture observer.
   */
  virtual void DeRegisterDesktopCapturerObserver() = 0;

  /**
   * @brief Starts desktop capture with the given frame rate.
   *
   * @param fps The desired frame rate.
   *
   * @return The current capture state after attempting to start capture.
   */
  virtual CaptureState Start(uint32_t fps) = 0;

  /**
   * @brief Starts desktop capture with the given frame rate and capture
   *        dimensions.
   *
   * @param fps The desired frame rate.
   * @param x The left-most pixel coordinate of the capture region.
   * @param y The top-most pixel coordinate of the capture region.
   * @param w The width of the capture region.
   * @param h The height of the capture region.
   *
   * @return The current capture state after attempting to start capture.
   */
  virtual CaptureState Start(uint32_t fps, uint32_t x, uint32_t y, uint32_t w,
                             uint32_t h) = 0;

  /**
   * @brief Starts full-frame desktop capture and scales captured frames down
   *        to fit inside the given maximum dimensions before encoding.
   *
   * Unlike Start(fps, x, y, w, h), this method does not crop the source.
   * The source aspect ratio is preserved. Display captures emit exact
   * contain-fit dimensions; window captures use the requested maximum as a
   * stable encoder canvas and center the full source inside it when the
   * captured window/client area does not match the requested aspect ratio.
   *
   * @param fps The desired frame rate.
   * @param max_w The maximum encoded frame width.
   * @param max_h The maximum encoded frame height.
   *
   * @return The current capture state after attempting to start capture.
   */
  virtual CaptureState StartWithMaxFrameSize(uint32_t fps, uint32_t max_w,
                                             uint32_t max_h) = 0;

  /**
   * @brief Selects a Windows desktop capture backend mode before capture
   *        starts. Unsupported platforms ignore this value.
   *
   * Accepted Windows values are implementation-defined debug diagnostics
   * controls such as "default", "wgc-only", "directx-only", and
   * "window-crop".
   *
   * @param mode Null-terminated backend mode string.
   */
  virtual void SetWindowsCaptureBackendMode(const char* mode) = 0;

  /**
   * @brief Selects a Windows dirty-region diagnostic mode before capture
   *        starts. Unsupported platforms ignore this value.
   *
   * Accepted Windows values are "auto" and "force-full-frame". The latter is
   * a debug/test harness control that disables updated-region differ wrappers
   * where applicable and labels delivered frames as full-frame updates for
   * diagnostics. It must not crop or otherwise change the captured content.
   *
   * @param mode Null-terminated dirty-region mode string.
   */
  virtual void SetWindowsCaptureDirtyRegionMode(const char* mode) = 0;

  /**
   * @brief Selects a Windows window-GDI capture method diagnostic mode before
   *        capture starts. Unsupported platforms ignore this value.
   *
   * Accepted Windows values are "default", "print-full-first",
   * "print-window-first", "bitblt-first", and "bitblt-only". This is a
   * debug/test harness control for identifying whether slow or black window
   * capture comes from PW_RENDERFULLCONTENT, plain PrintWindow, or BitBlt.
   *
   * @param mode Null-terminated window-GDI mode string.
   */
  virtual void SetWindowsWindowGdiCaptureMode(const char* mode) = 0;

  /**
   * @brief Enables a Windows diagnostic mode that decouples desktop capture
   *        acquisition from frame submission by keeping only the latest
   *        captured frame and pacing OnFrame calls on the requested cadence.
   *
   * This is an Inter Galactic debug/test harness control. Normal desktop
   * capture should leave it disabled unless a stream-test run explicitly
   * enables it for frame-cadence comparison.
   *
   * @param enabled True to enable latest-frame pacing.
   */
  virtual void SetLatestFramePacingEnabled(bool enabled) = 0;

  /**
   * @brief Stops desktop capture.
   */
  virtual void Stop() = 0;

  /**
   * @brief Checks if desktop capture is currently running.
   *
   * @return True if capture is running, false otherwise.
   */
  virtual bool IsRunning() = 0;

  /**
   * @brief Retrieves the media source for the current desktop capture.
   *
   * @return A scoped_refptr<MediaSource> representing the current capture
   *         media source.
   */
  virtual scoped_refptr<MediaSource> source() = 0;

  /**
   * @brief Destroys the RTCDesktopCapturer object.
   */
  virtual ~RTCDesktopCapturer() {}
};

/**
 * @brief Observer interface for desktop capturer events.
 *
 * This class defines the interface for an observer of the DesktopCapturer
 * class, allowing clients to be notified of events such as when capturing
 * begins or ends, and when an error occurs.
 */
class DesktopCapturerObserver {
 public:
  /**
   * @brief Called when desktop capture starts.
   *
   * @param capturer A reference to the capturer that started capturing.
   */
  virtual void OnStart(scoped_refptr<RTCDesktopCapturer> capturer) = 0;

  /**
   * @brief Called when desktop capture is paused.
   *
   * @param capturer A reference to the capturer that paused capturing.
   */
  virtual void OnPaused(scoped_refptr<RTCDesktopCapturer> capturer) = 0;

  /**
   * @brief Called when desktop capture stops.
   *
   * @param capturer A reference to the capturer that stopped capturing.
   */
  virtual void OnStop(scoped_refptr<RTCDesktopCapturer> capturer) = 0;

  /**
   * @brief Called when an error occurs during desktop capture.
   *
   * @param capturer A reference to the capturer that encountered an error.
   */
  virtual void OnError(scoped_refptr<RTCDesktopCapturer> capturer) = 0;

 protected:
  ~DesktopCapturerObserver() {}
};

}  // namespace libwebrtc

#endif  // LIB_WEBRTC_RTC_DESKTOP_CAPTURER_HXX
