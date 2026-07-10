// chiaki-tizen: Samsung Tizen WASM Player (EMSS) pipeline.
//
// Video: H.264 Annex-B access units from chiaki's video sample callback are
//        appended as ElementaryMediaPackets in Ultra-Low latency mode; the
//        TV's hardware decoder renders straight onto the associated <video>.
// Audio: chiaki's Opus decoder emits interleaved S16 PCM frames which are
//        appended to a PCM audio track (mirrors Samsung's Moonlight port —
//        HW Opus decode in low-latency modes is unreliable on older models).

#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

#include "samsung/wasm/elementary_media_stream_source.h"
#include "samsung/wasm/elementary_media_stream_source_listener.h"
#include "samsung/wasm/elementary_audio_track_config.h"
#include "samsung/wasm/elementary_media_packet.h"
#include "samsung/wasm/elementary_media_track.h"
#include "samsung/wasm/elementary_media_track_listener.h"
#include "samsung/wasm/elementary_video_track_config.h"
#include "samsung/html/html_media_element.h"
#include "samsung/html/html_media_element_listener.h"

namespace chiaki_tizen
{

struct VideoConfig
{
	unsigned width = 1920;
	unsigned height = 1080;
	unsigned fps = 60;
	bool h265 = false; // PS5 can send HEVC; PS4 is H.264 only
	bool hdr = false;  // HDR10 over HEVC Main10 (implies h265); PS5 only
};

struct AudioConfig
{
	unsigned channels = 2;
	unsigned sample_rate = 48000;
	unsigned frame_samples = 480; // per channel, per opus frame (10 ms @ 48 kHz)
};

class EmssPlayer final
	: public samsung::wasm::ElementaryMediaStreamSourceListener,
	  public samsung::wasm::ElementaryMediaTrackListener,
	  public samsung::html::HTMLMediaElementListener
{
public:
	// media_element_id: DOM id of the <video> element in the host page.
	explicit EmssPlayer(const char *media_element_id);
	~EmssPlayer() override;

	EmssPlayer(const EmssPlayer &) = delete;
	EmssPlayer &operator=(const EmssPlayer &) = delete;

	bool Start(const VideoConfig &video, const AudioConfig &audio);
	void Stop();

	// Called from chiaki's video receiver thread. buf is a full Annex-B
	// access unit. Returns false if the packet could not be appended (the
	// caller can then request an IDR frame from the console).
	bool PushVideoAccessUnit(const uint8_t *buf, size_t buf_size);

	// Called from chiaki's audio thread with interleaved S16 PCM.
	void PushAudioPcm(const int16_t *pcm, size_t samples_per_channel);

	// Reconfigure the audio track lazily when the stream header announces a
	// different layout than the default (called before first PCM push).
	void SetAudioConfig(const AudioConfig &audio);

	bool running() const { return running_; }

	// --- ElementaryMediaStreamSourceListener ---
	void OnSourceDetached() override;
	void OnSourceOpen() override;
	void OnSourceOpenPending() override;
	void OnSourceClosed() override;
	void OnSourceEnded() override;
	void OnPipelineError(samsung::wasm::MediaPipelineError error_code,
		const char *error_message) override;

	// --- ElementaryMediaTrackListener ---
	void OnTrackOpen() override;
	void OnTrackClosed(samsung::wasm::ElementaryMediaTrack::CloseReason) override;
	void OnSessionIdChanged(samsung::wasm::SessionId session_id) override;
	void OnAppendError(samsung::wasm::OperationResult operation_result) override;

	// --- HTMLMediaElementListener ---
	void OnPlaying() override;
	void OnLoadedMetadata() override;
	void OnLoadedData() override;
	void OnCanPlay() override;
	void OnCanPlayThrough() override;
	void OnWaiting() override;
	void OnError(samsung::html::MediaError error, const char *error_msg) override;

private:
	bool RefreshTrackOpenState();
	// Second phase of Start(): AddTrack/Open/Play, run from OnSourceClosed()
	// once SetSrc's asynchronous attach has actually reached kClosed.
	void FinishSetup();

	samsung::html::HTMLMediaElement media_element_;
	samsung::wasm::ElementaryMediaStreamSource source_;
	samsung::wasm::ElementaryMediaTrack video_track_;
	samsung::wasm::ElementaryMediaTrack audio_track_;

	VideoConfig video_config_;
	AudioConfig audio_config_;

	std::mutex mutex_;
	bool running_ = false;
	// Set by Start() after SetSrc; consumed by OnSourceClosed() to run
	// FinishSetup() exactly once when the async attach completes.
	bool setup_pending_ = false;
	bool video_track_open_ = false;
	bool audio_track_open_ = false;
	bool seen_key_frame_ = false;
	std::vector<uint8_t> video_codec_header_;

	samsung::wasm::SessionId video_session_id_ = samsung::wasm::kIgnoreSessionId;
	samsung::wasm::SessionId audio_session_id_ = samsung::wasm::kIgnoreSessionId;

	uint64_t video_frame_index_ = 0;
	uint64_t audio_sample_index_ = 0;
	uint64_t video_waiting_count_ = 0;
	uint64_t audio_waiting_count_ = 0;
	uint32_t audio_debug_count_ = 0;
};

} // namespace chiaki_tizen
