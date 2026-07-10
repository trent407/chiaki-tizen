// MOCK — native compile-check only.
#pragma once
#include <cstdint>
#include <functional>
#include "common.h"
#include "elementary_audio_track_config.h"
#include "elementary_media_track.h"
#include "elementary_video_track_config.h"
namespace samsung { namespace wasm {
class ElementaryMediaStreamSourceListener;
class ElementaryMediaStreamSource {
 public:
  enum class LatencyMode { kNormal, kLow, kUltraLow };
  enum class RenderingMode { kMediaElement, kVideoTexture };
  enum class ReadyState { kDetached, kClosed, kOpenPending, kOpen, kEnded };
  ElementaryMediaStreamSource(LatencyMode, RenderingMode) {}
  Result<ElementaryMediaTrack> AddTrack(const ElementaryVideoTrackConfig &) { return {true, {}}; }
  Result<ElementaryMediaTrack> AddTrack(const ElementaryAudioTrackConfig &) { return {true, {}}; }
  Result<void> SetListener(ElementaryMediaStreamSourceListener *) { return {true}; }
  Result<void> Open(std::function<void(OperationResult)> cb) { if(cb) cb(OperationResult::kSuccess); return {true}; }
  Result<void> Close(std::function<void(OperationResult)> cb) { if(cb) cb(OperationResult::kSuccess); return {true}; }
  Result<ReadyState> GetReadyState() const { return {true, ReadyState::kClosed}; }
  bool IsValid() const { return true; }
};
class ElementaryMediaStreamSourceListener {
 public:
  virtual ~ElementaryMediaStreamSourceListener() = default;
  virtual void OnSourceDetached() {}
  virtual void OnSourceOpen() {}
  virtual void OnSourceOpenPending() {}
  virtual void OnSourceClosed() {}
  virtual void OnSourceEnded() {}
  virtual void OnPipelineError(MediaPipelineError, const char *) {}
};
}}
