// MOCK — native compile-check only.
#pragma once
#include "common.h"
#include "elementary_media_packet.h"
#include "elementary_audio_track_config.h"  // ValidateAudioConfig parameter type
namespace samsung { namespace wasm {
class ElementaryMediaTrackListener;
class ElementaryMediaTrack {
 public:
  enum class CloseReason { kSourceClosed, kSourceError, kSourceDetached, kTrackDisabled, kTrackEnded, kUnknown };
  ElementaryMediaTrack() = default;
  Result<void> SetListener(ElementaryMediaTrackListener *) { return {true}; }
  Result<void> AppendPacket(const ElementaryMediaPacket &) { return {true}; }
  Result<bool> IsOpen() const { return {true, true}; }
  Result<SessionId> GetSessionId() const { return {true, kIgnoreSessionId}; }
  Result<DecodingMode> GetActiveDecodingMode() const { return {true, DecodingMode::kHardware}; }
  static Result<void> ValidateAudioConfig(const ElementaryAudioTrackConfig &) { return {true}; }
};
class ElementaryMediaTrackListener {
 public:
  virtual ~ElementaryMediaTrackListener() = default;
  virtual void OnTrackOpen() {}
  virtual void OnTrackClosed(ElementaryMediaTrack::CloseReason) {}
  virtual void OnSessionIdChanged(SessionId) {}
  virtual void OnAppendError(OperationResult) {}
};
}}
