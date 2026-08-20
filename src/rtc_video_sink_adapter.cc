#include "rtc_video_sink_adapter.h"

#include "rtc_base/logging.h"
#include "rtc_video_frame_impl.h"
#include "rtc_video_track.h"

#include <cstdlib>
#include <string>

namespace libwebrtc {

namespace {

bool IntergalacticShouldDeferNativeRendererI420() {
  const char* value = std::getenv("INTERGALACTIC_NATIVE_RENDERER_DEFER_I420");
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  const std::string value_string(value);
  return value_string == "1" || value_string == "true" ||
         value_string == "yes";
}

}  // namespace

VideoSinkAdapter::VideoSinkAdapter(
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> track)
    : rtc_track_(track), crt_sec_(new webrtc::Mutex()) {
  rtc_track_->AddOrUpdateSink(this, webrtc::VideoSinkWants());
  RTC_LOG(LS_INFO) << __FUNCTION__ << ": ctor " << (void*)this;
}

VideoSinkAdapter::~VideoSinkAdapter() {
  rtc_track_->RemoveSink(this);
  RTC_LOG(LS_INFO) << __FUNCTION__ << ": dtor ";
}

// VideoSinkInterface implementation
void VideoSinkAdapter::OnFrame(const webrtc::VideoFrame& video_frame) {
  webrtc::scoped_refptr<webrtc::VideoFrameBuffer> render_buffer =
      video_frame.video_frame_buffer();
  if (!render_buffer) {
    RTC_LOG(LS_WARNING) << "VideoSinkAdapter: dropping frame with no buffer";
    return;
  }

  if (render_buffer->type() == webrtc::VideoFrameBuffer::Type::kNative &&
      !IntergalacticShouldDeferNativeRendererI420()) {
    webrtc::scoped_refptr<webrtc::I420BufferInterface> i420 =
        render_buffer->ToI420();
    if (!i420) {
      RTC_LOG(LS_WARNING)
          << "VideoSinkAdapter: dropping native frame that could not be "
             "converted for Flutter rendering";
      return;
    }
    render_buffer = i420;
  }

  scoped_refptr<VideoFrameBufferImpl> frame_buffer =
      scoped_refptr<VideoFrameBufferImpl>(
          new RefCountedObject<VideoFrameBufferImpl>(render_buffer));

  frame_buffer->set_rotation(video_frame.rotation());
  frame_buffer->set_timestamp_us(video_frame.timestamp_us());
  frame_buffer->set_id(video_frame.id());

  webrtc::MutexLock cs(crt_sec_.get());
  for (auto renderer : renderers_) {
    renderer->OnFrame(frame_buffer);
  }
}

void VideoSinkAdapter::AddRenderer(
    RTCVideoRenderer<scoped_refptr<RTCVideoFrame>>* renderer) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << ": AddRenderer " << (void*)renderer;
  webrtc::MutexLock cs(crt_sec_.get());
  renderers_.push_back(renderer);
}

void VideoSinkAdapter::RemoveRenderer(
    RTCVideoRenderer<scoped_refptr<RTCVideoFrame>>* renderer) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << ": RemoveRenderer " << (void*)renderer;
  webrtc::MutexLock cs(crt_sec_.get());
  renderers_.erase(
      std::remove_if(
          renderers_.begin(), renderers_.end(),
          [renderer](
              const RTCVideoRenderer<scoped_refptr<RTCVideoFrame>>* renderer_) {
            return renderer_ == renderer;
          }),
      renderers_.end());
}

void VideoSinkAdapter::AddRenderer(
    webrtc::VideoSinkInterface<webrtc::VideoFrame>* renderer) {
  rtc_track_->AddOrUpdateSink(renderer, webrtc::VideoSinkWants());
}
void VideoSinkAdapter::RemoveRenderer(
    webrtc::VideoSinkInterface<webrtc::VideoFrame>* renderer) {
  rtc_track_->RemoveSink(renderer);
}

}  // namespace libwebrtc
