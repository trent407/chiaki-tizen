#include "emss_player.hh"

#include <cstdio>
#include <cstring>
#include <vector>

namespace chiaki_tizen
{

namespace
{

using samsung::wasm::ElementaryMediaPacket;
using samsung::wasm::ElementaryMediaStreamSource;
using samsung::wasm::ElementaryMediaTrack;

using Seconds = samsung::wasm::Seconds;

constexpr const char *kH264Mime = "video/mp4; codecs=\"avc1.640028\"";  // H.264 High@L4.0
constexpr const char *kH265Mime = "video/mp4; codecs=\"hev1.1.6.L153.B0\"";      // HEVC Main@L5.1
constexpr const char *kH265HdrMime = "video/mp4; codecs=\"hev1.2.4.L153.B0\"";   // HEVC Main10@L5.1 (HDR10)
constexpr const char *kPcmMime = "audio/webm; codecs=\"pcm\"";

// The HDR10 metadata (SMPTE ST 2084 PQ transfer + mastering-display /
// content-light SEI) travels in-band in the HEVC bitstream; selecting the
// Main10 codec string is what tells the TV decoder to honour it. Only reached
// when the bridge requested CHIAKI_CODEC_H265_HDR, which is PS5-only.
const char *VideoMime(const VideoConfig &cfg)
{
	if(cfg.hdr)
		return kH265HdrMime;
	if(cfg.h265)
		return kH265Mime;
	return kH264Mime;
}

// Minimal Annex-B scan: does this access unit contain an IDR slice?
// (nal_unit_type 5 for H.264, 19/20 for H.265)
bool AccessUnitHasKeyFrame(const uint8_t *buf, size_t size, bool h265)
{
	for(size_t i = 0; i + 4 < size; i++)
	{
		if(buf[i] != 0 || buf[i + 1] != 0)
			continue;
		size_t nal_off;
		if(buf[i + 2] == 1)
			nal_off = i + 3;
		else if(buf[i + 2] == 0 && buf[i + 3] == 1)
			nal_off = i + 4;
		else
			continue;
		if(nal_off >= size)
			break;
		if(h265)
		{
			uint8_t nal_type = (buf[nal_off] >> 1) & 0x3f;
			if(nal_type == 19 || nal_type == 20) // IDR_W_RADL / IDR_N_LP
				return true;
		}
		else
		{
			uint8_t nal_type = buf[nal_off] & 0x1f;
			if(nal_type == 5)
				return true;
		}
	}
	return false;
}

bool AccessUnitHasCodecHeader(const uint8_t *buf, size_t size, bool h265)
{
	for(size_t i = 0; i + 4 < size; i++)
	{
		if(buf[i] != 0 || buf[i + 1] != 0)
			continue;
		size_t nal_off;
		if(buf[i + 2] == 1)
			nal_off = i + 3;
		else if(buf[i + 2] == 0 && buf[i + 3] == 1)
			nal_off = i + 4;
		else
			continue;
		if(nal_off >= size)
			break;
		if(h265)
		{
			uint8_t nal_type = (buf[nal_off] >> 1) & 0x3f;
			if(nal_type == 32 || nal_type == 33 || nal_type == 34) // VPS/SPS/PPS
				return true;
		}
		else
		{
			uint8_t nal_type = buf[nal_off] & 0x1f;
			if(nal_type == 7 || nal_type == 8) // SPS/PPS
				return true;
		}
	}
	return false;
}

} // namespace

EmssPlayer::EmssPlayer(const char *media_element_id)
	: media_element_(media_element_id),
	  source_(ElementaryMediaStreamSource::LatencyMode::kUltraLow,
	          ElementaryMediaStreamSource::RenderingMode::kMediaElement)
{
	source_.SetListener(this);
	media_element_.SetListener(this);
}

EmssPlayer::~EmssPlayer()
{
	Stop();
}

bool EmssPlayer::Start(const VideoConfig &video, const AudioConfig &audio)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if(running_)
			return true;

		video_config_ = video;
		audio_config_ = audio;
		setup_pending_ = true;
		video_frame_index_ = 0;
		audio_sample_index_ = 0;
		video_waiting_count_ = 0;
		audio_waiting_count_ = 0;
		audio_debug_count_ = 0;
		video_codec_header_.clear();
		video_session_id_ = samsung::wasm::kIgnoreSessionId;
		audio_session_id_ = samsung::wasm::kIgnoreSessionId;
		seen_key_frame_ = false;
		video_track_open_ = false;
		audio_track_open_ = false;
		running_ = true;
	}

	// TEMPORARY: one-shot platform capability + object validity dump. If
	// has_ultra_low_latency is 0, the kUltraLow source constructed above is
	// the reason everything downstream fails, and the latency mode needs a
	// runtime fallback (kLow / kNormal) on this model.
	auto ver = samsung::wasm::EmssVersionInfo::Create();
	std::fprintf(stderr,
		"[emss] caps: emss=%d legacy=%d ultra_low=%d low_lat_tex=%d decoding_mode=%d "
		"cfg_validation=%d | media_element valid=%d source valid=%d\n",
		ver.has_emss ? 1 : 0, ver.has_legacy_emss ? 1 : 0,
		ver.has_ultra_low_latency ? 1 : 0, ver.has_low_latency_video_texture ? 1 : 0,
		ver.has_decoding_mode ? 1 : 0, ver.has_config_validation ? 1 : 0,
		media_element_.IsValid() ? 1 : 0, source_.IsValid() ? 1 : 0);

	std::fprintf(stderr, "[emss] media element SetSrc begin\n");
	auto src_result = media_element_.SetSrc(&source_);
	std::fprintf(stderr, "[emss] media element SetSrc returned op=%d\n",
		static_cast<int>(src_result.operation_result));
	if(!src_result)
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			setup_pending_ = false;
			running_ = false;
		}
		std::fprintf(stderr, "[emss] media element SetSrc failed op=%d\n",
			static_cast<int>(src_result.operation_result));
		return false;
	}

	// SetSrc's kDetached -> kClosed transition is asynchronous — confirmed on
	// hardware: GetReadyState() still returned kDetached(0) right after
	// SetSrc succeeded, AddTrack (legal only in kClosed) failed, and the
	// "source closed" listener event arrived just after — that event IS the
	// attach completing, not a teardown. So the rest of the pipeline setup
	// (AddTrack/Open/Play) is deferred to OnSourceClosed() -> FinishSetup().
	return true;
}

void EmssPlayer::FinishSetup()
{
	VideoConfig video_config;
	AudioConfig audio_config;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if(!running_)
			return;
		video_config = video_config_;
		audio_config = audio_config_;
	}

	auto state_result = source_.GetReadyState();
	if(!state_result || *state_result != ElementaryMediaStreamSource::ReadyState::kClosed)
		std::fprintf(stderr, "[emss] source ready state before AddTrack: %d (op=%d)\n",
			state_result ? static_cast<int>(*state_result) : -1,
			static_cast<int>(state_result.operation_result));

	const char *video_mime = VideoMime(video_config);
	samsung::wasm::ElementaryVideoTrackConfig video_track_config{
		video_mime,
		{}, // extradata: none — SPS/PPS arrive in-band in Annex-B stream
		video_config.width,
		video_config.height,
		video_config.fps, // framerate numerator
		1,                 // framerate denominator
	};
	auto video_result = source_.AddTrack(video_track_config);
	if(!video_result)
	{
		std::fprintf(stderr, "[emss] AddTrack(video) failed op=%d (%ux%u@%u %s)\n",
			static_cast<int>(video_result.operation_result),
			video_config.width, video_config.height, video_config.fps, video_mime);
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if(!running_)
			return;
		video_track_ = std::move(*video_result);
	}
	video_track_.SetListener(this);

	samsung::wasm::ElementaryAudioTrackConfig audio_track_config{
		kPcmMime,
		{}, // extradata
		samsung::wasm::SampleFormat::kS16,
		audio_config.channels == 1 ? samsung::wasm::ChannelLayout::kMono
		                            : samsung::wasm::ChannelLayout::kStereo,
		audio_config.sample_rate,
	};
	auto audio_validation = ElementaryMediaTrack::ValidateAudioConfig(audio_track_config);
	std::fprintf(stderr, "[emss] audio config ch=%u rate=%u validation op=%d\n",
		audio_config.channels, audio_config.sample_rate,
		static_cast<int>(audio_validation.operation_result));
	auto audio_result = source_.AddTrack(audio_track_config);
	if(!audio_result)
	{
		std::fprintf(stderr, "[emss] AddTrack(audio) failed op=%d (ch=%u rate=%u)\n",
			static_cast<int>(audio_result.operation_result),
			audio_config.channels, audio_config.sample_rate);
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if(!running_)
			return;
		audio_track_ = std::move(*audio_result);
	}
	audio_track_.SetListener(this);

	source_.Open([](samsung::wasm::OperationResult r) {
		if(r != samsung::wasm::OperationResult::kSuccess)
			std::fprintf(stderr, "[emss] source Open failed op=%d\n", static_cast<int>(r));
		else
			std::fprintf(stderr, "[emss] source Open callback success\n");
	});
	media_element_.Play([](samsung::wasm::OperationResult r) {
		if(r != samsung::wasm::OperationResult::kSuccess)
			std::fprintf(stderr, "[emss] media element Play failed op=%d\n", static_cast<int>(r));
		else
			std::fprintf(stderr, "[emss] media element Play callback success\n");
	});
	std::fprintf(stderr, "[emss] setup dispatched, source opening\n");
}

void EmssPlayer::Stop()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if(!running_)
			return;
		running_ = false;
		setup_pending_ = false;
		video_track_open_ = false;
		audio_track_open_ = false;
	}
	media_element_.Pause();
	source_.Close([](samsung::wasm::OperationResult) {});
}

bool EmssPlayer::PushVideoAccessUnit(const uint8_t *buf, size_t buf_size)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if(!running_)
		return true; // swallow silently while the pipeline spins up
	if(!video_track_open_)
	{
		video_waiting_count_++;
		if((video_waiting_count_ % 60) == 0)
			std::fprintf(stderr,
				"[emss] video waiting for open track samples=%llu last_size=%zu setup_pending=%d\n",
				static_cast<unsigned long long>(video_waiting_count_), buf_size,
				setup_pending_ ? 1 : 0);
		return true;
	}

	bool key = AccessUnitHasKeyFrame(buf, buf_size, video_config_.h265);
	if(!seen_key_frame_)
	{
		if(!key && AccessUnitHasCodecHeader(buf, buf_size, video_config_.h265))
		{
			video_codec_header_.assign(buf, buf + buf_size);
			std::fprintf(stderr, "[emss] cached video codec header size=%zu\n", buf_size);
			return true;
		}
		if(!key)
			return false; // ask chiaki for an IDR; decoder must start on one
		seen_key_frame_ = true;
	}

	double const frame_duration = 1.0 / static_cast<double>(video_config_.fps);
	double const pts = static_cast<double>(video_frame_index_) * frame_duration;

	std::vector<uint8_t> first_key_with_header;
	const uint8_t *packet_buf = buf;
	size_t packet_size = buf_size;
	if(key && video_frame_index_ == 0 && !video_codec_header_.empty())
	{
		first_key_with_header.reserve(video_codec_header_.size() + buf_size);
		first_key_with_header.insert(first_key_with_header.end(),
			video_codec_header_.begin(), video_codec_header_.end());
		first_key_with_header.insert(first_key_with_header.end(), buf, buf + buf_size);
		packet_buf = first_key_with_header.data();
		packet_size = first_key_with_header.size();
		std::fprintf(stderr,
			"[emss] prepended video header to first key frame header=%zu frame=%zu total=%zu\n",
			video_codec_header_.size(), buf_size, packet_size);
	}

	ElementaryMediaPacket pkt{
		Seconds(pts),            // pts
		Seconds(pts),            // dts (no B-frames in low-latency RP stream)
		Seconds(frame_duration), // duration
		key,
		packet_size,
		packet_buf,
		video_config_.width,
		video_config_.height,
		video_config_.fps, // framerate num
		1,                 // framerate den
		video_session_id_,
	};

	auto append_result = video_track_.AppendPacket(pkt);
	if(!append_result)
	{
		std::fprintf(stderr, "[emss] video AppendPacket failed at frame %llu op=%d\n",
			static_cast<unsigned long long>(video_frame_index_),
			static_cast<int>(append_result.operation_result));
		return false;
	}
	video_frame_index_++;
	return true;
}

void EmssPlayer::SetAudioConfig(const AudioConfig &audio)
{
	std::lock_guard<std::mutex> lock(mutex_);
	audio_config_ = audio;
}

void EmssPlayer::PushAudioPcm(const int16_t *pcm, size_t samples_per_channel)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if(!running_)
		return;
	if(!audio_track_open_)
	{
		audio_waiting_count_++;
		if((audio_waiting_count_ % 200) == 0)
			std::fprintf(stderr,
				"[emss] audio waiting for open track frames=%llu samples=%zu setup_pending=%d\n",
				static_cast<unsigned long long>(audio_waiting_count_), samples_per_channel,
				setup_pending_ ? 1 : 0);
		return;
	}

	double const rate = static_cast<double>(audio_config_.sample_rate);
	double const pts = static_cast<double>(audio_sample_index_) / rate;
	double const duration = static_cast<double>(samples_per_channel) / rate;
	size_t const data_size = samples_per_channel * audio_config_.channels * sizeof(int16_t);
	if(audio_debug_count_ < 8)
	{
		int peak = 0;
		size_t const sample_count = samples_per_channel * audio_config_.channels;
		for(size_t i = 0; i < sample_count; i++)
		{
			int sample = pcm[i];
			int abs_sample = sample < 0 ? -sample : sample;
			if(abs_sample > peak)
				peak = abs_sample;
		}
		std::fprintf(stderr,
			"[emss] audio pcm #%u samples=%zu ch=%u bytes=%zu pts=%.3f dur=%.3f peak=%d session=%d\n",
			audio_debug_count_ + 1, samples_per_channel, audio_config_.channels,
			data_size, pts, duration, peak, static_cast<int>(audio_session_id_));
		audio_debug_count_++;
	}

	ElementaryMediaPacket pkt{
		Seconds(pts),
		Seconds(pts),
		Seconds(duration),
		true, // every PCM packet is independently decodable
		data_size,
		pcm,
		0, 0, // width/height unused for audio
		0, 1, // framerate unused; den=1 avoids div-by-zero
		audio_session_id_,
	};

	auto append_result = audio_track_.AppendPacket(pkt);
	if(append_result)
		audio_sample_index_ += samples_per_channel;
	else
		std::fprintf(stderr, "[emss] audio AppendPacket failed op=%d samples=%zu session=%d\n",
			static_cast<int>(append_result.operation_result),
			samples_per_channel, static_cast<int>(audio_session_id_));
}

void EmssPlayer::OnSourceDetached()
{
	std::fprintf(stderr, "[emss] source detached\n");
}

void EmssPlayer::OnSourceOpenPending()
{
	std::fprintf(stderr, "[emss] source open pending\n");
}

void EmssPlayer::OnSourceOpen()
{
	std::fprintf(stderr, "[emss] source open\n");
}

void EmssPlayer::OnSourceEnded()
{
	std::fprintf(stderr, "[emss] source ended\n");
}

void EmssPlayer::OnPipelineError(samsung::wasm::MediaPipelineError error_code,
	const char *error_message)
{
	std::fprintf(stderr, "[emss] pipeline error code=%d message=%s\n",
		static_cast<int>(error_code), error_message ? error_message : "unknown");
}

void EmssPlayer::OnSourceClosed()
{
	// Fires on every transition INTO kClosed — including the completion of
	// SetSrc's asynchronous attach, which is our cue to finish the pipeline
	// setup that Start() deferred (tracks can only be added in kClosed).
	bool do_setup = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		video_track_open_ = false;
		audio_track_open_ = false;
		if(setup_pending_)
		{
			setup_pending_ = false;
			do_setup = true;
		}
	}
	if(do_setup)
	{
		std::fprintf(stderr, "[emss] source attached — finishing setup\n");
		FinishSetup();
	}
	else
		std::fprintf(stderr, "[emss] source closed\n");
}

void EmssPlayer::OnTrackOpen()
{
	std::lock_guard<std::mutex> lock(mutex_);
	RefreshTrackOpenState();
	auto video_session = video_track_.GetSessionId();
	auto audio_session = audio_track_.GetSessionId();
	if(video_session)
		video_session_id_ = *video_session;
	if(audio_session)
		audio_session_id_ = *audio_session;
	auto video_mode = video_track_.GetActiveDecodingMode();
	auto audio_mode = audio_track_.GetActiveDecodingMode();
	std::fprintf(stderr,
		"[emss] track open (video=%d audio=%d waited_video=%llu waited_audio=%llu "
		"video_session=%d(op=%d) audio_session=%d(op=%d) video_mode=%d(op=%d) audio_mode=%d(op=%d))\n",
		video_track_open_ ? 1 : 0, audio_track_open_ ? 1 : 0,
		static_cast<unsigned long long>(video_waiting_count_),
		static_cast<unsigned long long>(audio_waiting_count_),
		video_session ? static_cast<int>(*video_session) : -1,
		static_cast<int>(video_session.operation_result),
		audio_session ? static_cast<int>(*audio_session) : -1,
		static_cast<int>(audio_session.operation_result),
		video_mode ? static_cast<int>(*video_mode) : -1,
		static_cast<int>(video_mode.operation_result),
		audio_mode ? static_cast<int>(*audio_mode) : -1,
		static_cast<int>(audio_mode.operation_result));
}

void EmssPlayer::OnTrackClosed(ElementaryMediaTrack::CloseReason)
{
	std::lock_guard<std::mutex> lock(mutex_);
	RefreshTrackOpenState();
	seen_key_frame_ = false;
}

void EmssPlayer::OnSessionIdChanged(samsung::wasm::SessionId session_id)
{
	// Both tracks share the source's session lifecycle; stamp both. Packets
	// carrying a stale session id are dropped by the platform, which is the
	// desired behavior across suspend/resume.
	std::lock_guard<std::mutex> lock(mutex_);
	video_session_id_ = session_id;
	audio_session_id_ = session_id;
	seen_key_frame_ = false; // decoder restarts on a new session
}

void EmssPlayer::OnAppendError(samsung::wasm::OperationResult operation_result)
{
	std::fprintf(stderr, "[emss] append async error op=%d\n",
		static_cast<int>(operation_result));
}

void EmssPlayer::OnPlaying()
{
	std::fprintf(stderr, "[emss] media element playing\n");
}

void EmssPlayer::OnLoadedMetadata()
{
	std::fprintf(stderr, "[emss] media element loadedmetadata\n");
}

void EmssPlayer::OnLoadedData()
{
	std::fprintf(stderr, "[emss] media element loadeddata\n");
}

void EmssPlayer::OnCanPlay()
{
	std::fprintf(stderr, "[emss] media element canplay\n");
}

void EmssPlayer::OnCanPlayThrough()
{
	std::fprintf(stderr, "[emss] media element canplaythrough\n");
}

void EmssPlayer::OnWaiting()
{
	std::fprintf(stderr, "[emss] media element waiting\n");
}

void EmssPlayer::OnError(samsung::html::MediaError, const char *error_msg)
{
	std::fprintf(stderr, "[emss] media element error: %s\n", error_msg ? error_msg : "unknown");
}

bool EmssPlayer::RefreshTrackOpenState()
{
	auto video_open = video_track_.IsOpen();
	auto audio_open = audio_track_.IsOpen();
	video_track_open_ = video_open && *video_open;
	audio_track_open_ = audio_open && *audio_open;
	return video_track_open_ || audio_track_open_;
}

} // namespace chiaki_tizen
