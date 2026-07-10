// MOCK — native compile-check only.
#pragma once
#include <functional>
#include "../wasm/common.h"
namespace samsung { namespace wasm { class ElementaryMediaStreamSource; }}
namespace samsung { namespace html {
enum class MediaError { kMediaErrDecode };
class HTMLMediaElementListener;
class HTMLMediaElement {
 public:
  explicit HTMLMediaElement(const char *) {}
  samsung::wasm::Result<void> SetSrc(samsung::wasm::ElementaryMediaStreamSource *) { return {true}; }
  samsung::wasm::Result<void> SetListener(HTMLMediaElementListener *) { return {true}; }
  samsung::wasm::Result<void> Play(std::function<void(samsung::wasm::OperationResult)> cb) { if(cb) cb(samsung::wasm::OperationResult::kSuccess); return {true}; }
  samsung::wasm::Result<void> Pause() { return {true}; }
  bool IsValid() const { return true; }
};
class HTMLMediaElementListener {
 public:
  virtual ~HTMLMediaElementListener() = default;
  virtual void OnPlaying() {}
  virtual void OnLoadedMetadata() {}
  virtual void OnLoadedData() {}
  virtual void OnCanPlay() {}
  virtual void OnCanPlayThrough() {}
  virtual void OnWaiting() {}
  virtual void OnError(MediaError, const char *) {}
};
}}
