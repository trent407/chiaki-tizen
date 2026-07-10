// MOCK of Samsung Emscripten SDK types — native compile-check only.
// The real headers ship with the Samsung Emscripten SDK.
#pragma once
#include <cstdint>
#include <functional>
namespace samsung { namespace wasm {
enum class OperationResult { kSuccess, kFailed, kAlreadyDestroyed };

// Real Results carry both a bool-ish success and the detailed operation_result
// code; emss_player.cc reads .operation_result on every call, so the mock must
// expose it too.
template<typename T> struct Result {
  bool ok = false;
  T value{};
  OperationResult operation_result = OperationResult::kSuccess;
  explicit operator bool() const { return ok; }
  T &operator*() { return value; }
};
template<> struct Result<void> {
  bool ok = false;
  OperationResult operation_result = OperationResult::kSuccess;
  explicit operator bool() const { return ok; }
};

using SessionId = int32_t;
constexpr SessionId kIgnoreSessionId = -1;

struct Seconds { double v; explicit Seconds(double x = 0) : v(x) {} };

enum class SampleFormat { kS16, kS32, kF32 };
enum class ChannelLayout { kMono, kStereo };

// Pipeline error passed to ElementaryMediaStreamSourceListener::OnPipelineError.
enum class MediaPipelineError {
  kNoError,
  kNetworkError,
  kDecoderError,
  kAudioDecoderError,
  kVideoDecoderError,
  kUnknownError
};

// Active decoding mode reported by ElementaryMediaTrack::GetActiveDecodingMode.
enum class DecodingMode { kHardware, kHardwareWithFallback, kSoftware };

// Platform capability probe (EmssVersionInfo::Create()); emss_player.cc dumps
// these once at Start() to diagnose ultra-low-latency support on a given model.
struct EmssVersionInfo {
  bool has_emss = true;
  bool has_legacy_emss = false;
  bool has_ultra_low_latency = true;
  bool has_low_latency_video_texture = true;
  bool has_decoding_mode = true;
  bool has_config_validation = true;
  static EmssVersionInfo Create() { return {}; }
};
}} // namespace
