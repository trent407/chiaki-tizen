// chiaki-tizen: bridge between chiaki-ng's core library and the Tizen web
// app's JavaScript. Exposes a small C ABI (ct_*) that JS calls via
// Module.ccall, and pushes events back to JS as JSON through
// window.ChiakiTizen._onEvent().
//
// Threading model:
//   - The module is linked with -s PROXY_TO_PTHREAD, so main() and every
//     ct_* entry point run off the browser main thread (a requirement of
//     the Tizen Sockets Extension).
//   - chiaki spawns its own pthreads for session/ctrl/takion/discovery.
//   - Events cross back to the JS main thread via MAIN_THREAD_EM_ASM
//     (synchronous — see the comment in EmitJson() for why the async
//     variant silently corrupted every event this bridge sends).

#include <chiaki/session.h>
#include <chiaki/discoveryservice.h>
#include <chiaki/regist.h>
#include <chiaki/opusdecoder.h>
#include <chiaki/base64.h>
#include <chiaki/log.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <string>
#include <memory>
#include <functional>
#include <thread>
#include <vector>
#include <chrono>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <mbedtls/base64.h>

#include "emss_player.hh"
#include "holepunch_bridge.h"
// PSN over-the-internet transport (promoted from the verified spikes)
#include "net/ws_client.h"
#include "net/stun.h"
#include "net/punch.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#define CT_EXPORT extern "C" EMSCRIPTEN_KEEPALIVE
// TEMPORARY: routes a checkpoint string into the same on-screen HUD trail
// driven by window.__ctReport in index.html. ct_init() runs on a proxied
// pthread worker, where `window`/`__ctReport` don't exist (worker global
// scope is `self`), so this has to cross back to the main thread the same
// way EmitJson() below already does. Remove once the "unreachable" crash
// inside ct_init()'s call chain is diagnosed.
#define CT_CHECKPOINT(msg) MAIN_THREAD_ASYNC_EM_ASM({ \
		if (typeof __ctReport === 'function') \
			__ctReport('ct_init', UTF8ToString($0)); \
	}, msg)
// TEMPORARY: every ct_* entry point except ct_init() is called directly via
// Module.cwrap from JS UI handlers (button presses, etc.), i.e. on the real
// browser main thread — not the pthread worker main()/ct_init() run on.
// Confirmed for real: Samsung's Tizen Sockets Extension explicitly throws
// "Cannot call host binding function from JS" when chiaki_regist_start()
// (reached from ct_regist_start(), called this way) tries to open a socket.
//
// First attempt used emscripten_sync_run_in_main_runtime_thread_(), which
// turned out to be a no-op here: per its own doc comment, "if this thread
// is the main thread, the operation is immediately performed" — and we ARE
// already on the main thread when calling it from ct_regist_start(), so it
// just ran inline, same broken thread as before.
//
// What we actually need is emscripten_async_queue_on_thread_(), which can
// target an arbitrary specific pthread_t (g_worker_thread) rather than "the
// main thread". It's async-only, and that's a hard requirement, not just
// an inconvenience: the browser main thread can never synchronously block
// waiting on a worker (same restriction that made blocking calls dangerous
// everywhere else in this investigation) — it has to stay free to receive
// the worker's response. So every dispatched trampoline must report its
// result back via EmitEvent (safe now that EmitJson is synchronous the
// other direction) instead of returning a value to the caller.
//
// argsPtr must be heap-allocated by the caller (not stack/local) — this
// call returns before the worker ever runs the trampoline, so anything
// stack-scoped at the call site would be long gone by then. The trampoline
// is responsible for freeing it once done.
#define CT_DISPATCH_TO_WORKER(trampoline, argsPtr) \
	emscripten_async_queue_on_thread(g_worker_thread, EM_FUNC_SIG_VI, (trampoline), nullptr, (void *)(argsPtr))
#else
// Native syntax-check build: EM_ASM becomes a no-op.
#define CT_EXPORT extern "C"
#define MAIN_THREAD_ASYNC_EM_ASM(code, ...) do {} while(0)
#define MAIN_THREAD_EM_ASM(code, ...) do {} while(0)
#define CT_CHECKPOINT(msg) do {} while(0)
#define CT_DISPATCH_TO_WORKER(trampoline, argsPtr) (trampoline)((void *)(argsPtr))
#endif

namespace
{

// ---------------------------------------------------------------------------
// Event channel to JS
// ---------------------------------------------------------------------------

void EmitJson(const std::string &json)
{
#ifdef __EMSCRIPTEN__
	// Was MAIN_THREAD_ASYNC_EM_ASM, which queues the JS callback and returns
	// immediately without waiting for the main thread to run it. json.c_str()
	// points into this function's own stack frame (small strings like
	// {"type":"ready"} live inline via SSO), which becomes invalid the
	// instant EmitJson() returns — i.e. before the main thread ever gets to
	// it. By the time UTF8ToString($0) actually runs, it's reading
	// stack memory that's very likely already been overwritten by whatever
	// the caller did next (e.g. the very next CT_CHECKPOINT call). The
	// resulting garbage fails JSON.parse() in _onEvent(), which is caught
	// and silently swallowed — explaining why "ready" (and every other
	// event this function sends: discoveryHosts, registFinished, etc.)
	// could complete on the C++ side but never actually arrive in JS.
	// MAIN_THREAD_EM_ASM blocks this worker until the main thread has
	// actually finished running the callback, keeping json alive throughout.
	MAIN_THREAD_EM_ASM({
		if(typeof window !== 'undefined' && window.ChiakiTizen && window.ChiakiTizen._onEvent)
			window.ChiakiTizen._onEvent(UTF8ToString($0));
	}, json.c_str());
#else
	std::fprintf(stderr, "[event] %s\n", json.c_str());
#endif
}

std::string JsonEscape(const char *s)
{
	std::string out;
	if(!s)
		return out;
	for(const char *p = s; *p; p++)
	{
		switch(*p)
		{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if(static_cast<unsigned char>(*p) < 0x20)
				{
					char buf[8];
					std::snprintf(buf, sizeof(buf), "\\u%04x", *p);
					out += buf;
				}
				else
					out += *p;
		}
	}
	return out;
}

void EmitEvent(const char *type, const std::string &payload_json_fields = "")
{
	std::string json = "{\"type\":\"";
	json += type;
	json += "\"";
	if(!payload_json_fields.empty())
	{
		json += ",";
		json += payload_json_fields;
	}
	json += "}";
	EmitJson(json);
}

// ---------------------------------------------------------------------------
// Global state (a Tizen TV app streams exactly one session at a time)
// ---------------------------------------------------------------------------

struct BridgeState
{
	ChiakiLog log;

	// session
	ChiakiSession session;
	bool session_active = false;
	ChiakiOpusDecoder opus_decoder;
	std::unique_ptr<chiaki_tizen::EmssPlayer> player;
	ChiakiControllerState controller_state;

	// discovery
	ChiakiDiscoveryService discovery;
	bool discovery_active = false;

	// registration
	ChiakiRegist regist;
	bool regist_active = false;

	// PSN remote-play transport (over-the-internet)
	ws_client *psn_ws = nullptr;
	std::thread psn_ws_thread;
	std::atomic<bool> psn_ws_running{false};
	int psn_ctrl_fd = -1;
	int psn_data_fd = -1;

	// live stream stats (for the on-screen HUD)
	unsigned stream_w = 0, stream_h = 0, stream_fps = 0;
	bool stream_hdr = false;
	uint64_t stat_bytes = 0, stat_frames = 0;
	std::chrono::steady_clock::time_point stat_t0;
};

BridgeState *g_state = nullptr;

// TEMPORARY: the pthread worker main()/ct_init() run on — every ct_* entry
// point that can touch a socket needs to actually execute here, not
// wherever JS happened to call it from (see CT_DISPATCH_TO_WORKER).
// Captured once in main(), which is on this exact thread by construction.
#ifdef __EMSCRIPTEN__
pthread_t g_worker_thread;
#endif

// Generic closure dispatch onto the worker: capture argument copies
// (std::string by value for anything the caller only guarantees for the
// duration of the cwrap call) and run the whole body over there. chiaki-ng
// copies what it keeps (regist strdups info.host, discoveryservice strdups
// send_host, session resolves the host to addrinfos during init), so
// captures only need to outlive the dispatched call itself — which the
// heap-allocated std::function guarantees.
void ClosureTrampoline(void *p)
{
	auto *fn = static_cast<std::function<void()> *>(p);
	(*fn)();
	delete fn;
}

void DispatchToWorker(std::function<void()> fn)
{
	CT_DISPATCH_TO_WORKER(ClosureTrampoline, new std::function<void()>(std::move(fn)));
}

void LogCb(ChiakiLogLevel level, const char *msg, void *user)
{
	(void)user;
	// The Takion reliable-UDP layer can emit these every 100-200ms per
	// packet while the stream is running. On TV the debug HUD is tiny and
	// expensive to repaint, so keep the high-value logs and collapse this
	// expected transport chatter to an occasional heartbeat.
	static std::atomic<unsigned> suppressed_transport_logs{0};
	if(msg && (std::strstr(msg, "Takion Send Buffer re-sending packet") ||
		std::strstr(msg, "Takion dropping data with seq num") ||
		std::strstr(msg, "Detected missing or corrupt frame") ||
		std::strstr(msg, "StreamConnection reporting corrupt frame") ||
		std::strstr(msg, "Missing unit") ||
		std::strstr(msg, "Video FEC failure") ||
		std::strstr(msg, "Failed to complete frame") ||
		std::strstr(msg, "Video receiver could not flush frame") ||
		std::strstr(msg, "Frame Processor received") ||
		std::strstr(msg, "FEC failed") ||
		std::strstr(msg, "Clamping reported packet loss") ||
		std::strstr(msg, "Requested key stream for key pos") ||
		std::strstr(msg, "Already requested a higher key pos") ||
		std::strstr(msg, "Takion Send Buffer overflow") ||
		std::strstr(msg, "CTRL RECEIVED") ||
		std::strstr(msg, "Ctrl received Heartbeat") ||
		std::strstr(msg, "offset 0  1  2  3  4  5  6") ||
		std::strstr(msg, "     0 ") ||
		std::strstr(msg, "0123456789abcdef")))
	{
		unsigned count = suppressed_transport_logs.fetch_add(1, std::memory_order_relaxed) + 1;
		if((count & 0x7f) == 0)
			std::fprintf(stderr, "[chiaki:%c] suppressed %u repeated stream recovery logs\n",
				chiaki_log_level_char(level), count);
		return;
	}
	std::fprintf(stderr, "[chiaki:%c] %s\n", chiaki_log_level_char(level), msg);
}

// ---------------------------------------------------------------------------
// Session callbacks (invoked on chiaki's threads)
// ---------------------------------------------------------------------------

bool VideoSampleCb(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user)
{
	auto *state = static_cast<BridgeState *>(user);
	if(!state->player)
		return true;

	// Live stats for the HUD: accumulate bytes/frames, emit ~1×/sec.
	state->stat_bytes += buf_size;
	state->stat_frames++;
	auto now = std::chrono::steady_clock::now();
	double elapsed = std::chrono::duration<double>(now - state->stat_t0).count();
	if(elapsed >= 1.0)
	{
		int kbps = (int)((state->stat_bytes * 8.0 / 1000.0) / elapsed + 0.5);
		int fps = (int)(state->stat_frames / elapsed + 0.5);
		char fields[224];
		std::snprintf(fields, sizeof(fields),
			"\"kbps\":%d,\"fps\":%d,\"width\":%u,\"height\":%u,\"hdr\":%s",
			kbps, fps, state->stream_w, state->stream_h,
			state->stream_hdr ? "true" : "false");
		EmitEvent("streamStats", fields);
		state->stat_bytes = 0;
		state->stat_frames = 0;
		state->stat_t0 = now;
	}

	// Returning false makes chiaki report frame corruption and request an
	// IDR frame from the console — exactly what we want both before the
	// first key frame and after an append failure.
	bool ok = state->player->PushVideoAccessUnit(buf, buf_size, frames_lost);
	if(!ok || frames_lost || frame_recovered)
		std::fprintf(stderr,
			"[ct_av] video sample issue size=%zu ok=%d lost=%d recovered=%d\n",
			buf_size, ok ? 1 : 0, frames_lost, frame_recovered ? 1 : 0);
	return ok;
}

void OpusSettingsCb(uint32_t channels, uint32_t rate, void *user)
{
	auto *state = static_cast<BridgeState *>(user);
	if(!state->player)
		return;
	chiaki_tizen::AudioConfig cfg;
	cfg.channels = channels;
	cfg.sample_rate = rate;
	state->player->SetAudioConfig(cfg);
	char fields[96];
	std::snprintf(fields, sizeof(fields), "\"channels\":%u,\"rate\":%u", channels, rate);
	EmitEvent("audioSettings", fields);
}

void OpusFrameCb(int16_t *buf, size_t samples_count, void *user)
{
	auto *state = static_cast<BridgeState *>(user);
	if(!state->player)
		return;
	unsigned channels = state->opus_decoder.audio_header.channels;
	if(channels == 0)
		channels = 2;
	// opus_decode() returns samples per channel; the PCM buffer itself is
	// interleaved, and EmssPlayer expects this per-channel sample count.
	state->player->PushAudioPcm(buf, samples_count);
}

void SessionEventCb(ChiakiEvent *event, void *user)
{
	(void)user;
	switch(event->type)
	{
		case CHIAKI_EVENT_CONNECTED:
			EmitEvent("connected");
			break;
		case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
		{
			std::string fields = "\"incorrect\":";
			fields += event->login_pin_request.pin_incorrect ? "true" : "false";
			EmitEvent("loginPinRequest", fields);
			break;
		}
		case CHIAKI_EVENT_RUMBLE:
		{
			char fields[64];
			std::snprintf(fields, sizeof(fields), "\"left\":%u,\"right\":%u",
				event->rumble.left, event->rumble.right);
			EmitEvent("rumble", fields);
			break;
		}
		case CHIAKI_EVENT_QUIT:
		{
			std::string fields = "\"reason\":\"";
			fields += JsonEscape(chiaki_quit_reason_string(event->quit.reason));
			fields += "\",\"isError\":";
			fields += chiaki_quit_reason_is_error(event->quit.reason) ? "true" : "false";
			EmitEvent("quit", fields);
			break;
		}
		case CHIAKI_EVENT_NICKNAME_RECEIVED:
		{
			std::string fields = "\"nickname\":\"";
			fields += JsonEscape(event->server_nickname);
			fields += "\"";
			EmitEvent("nickname", fields);
			break;
		}
		default:
			break;
	}
}

// ---------------------------------------------------------------------------
// Discovery callback
// ---------------------------------------------------------------------------

void DiscoveryCb(ChiakiDiscoveryHost *hosts, size_t hosts_count, void *user)
{
	(void)user;
	if(g_state)
		CHIAKI_LOGI(&g_state->log, "discovery callback: %zu host(s) reported", hosts_count);
	std::string json = "{\"type\":\"discoveryHosts\",\"hosts\":[";
	for(size_t i = 0; i < hosts_count; i++)
	{
		const ChiakiDiscoveryHost &h = hosts[i];
		if(i)
			json += ",";
		json += "{";
		json += "\"state\":\"";
		json += (h.state == CHIAKI_DISCOVERY_HOST_STATE_READY) ? "ready"
			: (h.state == CHIAKI_DISCOVERY_HOST_STATE_STANDBY) ? "standby" : "unknown";
		json += "\"";
		auto add = [&json](const char *key, const char *val) {
			json += ",\"";
			json += key;
			json += "\":\"";
			json += JsonEscape(val ? val : "");
			json += "\"";
		};
		add("hostAddr", h.host_addr);
		add("hostName", h.host_name);
		add("hostType", h.host_type);
		add("hostId", h.host_id);
		add("runningApp", h.running_app_name);
		json += "}";
	}
	json += "]}";
	EmitJson(json);
}

// ---------------------------------------------------------------------------
// Registration callback
// ---------------------------------------------------------------------------

void RegistCb(ChiakiRegistEvent *event, void *user)
{
	// TEMPORARY: chiaki-ng runs this from its own internal regist thread, not
	// from ct_regist_start()'s caller — confirms whether that thread ever
	// gets anywhere at all. Remove once diagnosed.
	CT_CHECKPOINT("RegistCb fired");
	auto *state = static_cast<BridgeState *>(user);
	switch(event->type)
	{
		case CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS:
		{
			CT_CHECKPOINT("RegistCb success");
			ChiakiRegisteredHost *host = event->registered_host;
			char rp_key_b64[CHIAKI_RPCRYPT_KEY_SIZE * 2 + 8] = {0};
			char regist_key_b64[sizeof(host->rp_regist_key) * 2 + 8] = {0};
			chiaki_base64_encode(host->rp_key, sizeof(host->rp_key),
				rp_key_b64, sizeof(rp_key_b64));
			chiaki_base64_encode(reinterpret_cast<uint8_t *>(host->rp_regist_key),
				sizeof(host->rp_regist_key), regist_key_b64, sizeof(regist_key_b64));

			std::string fields = "\"success\":true";
			fields += ",\"serverNickname\":\"";
			fields += JsonEscape(host->server_nickname);
			fields += "\",\"rpKeyB64\":\"";
			fields += rp_key_b64;
			fields += "\",\"registKeyB64\":\"";
			fields += regist_key_b64;
			fields += "\",\"target\":";
			fields += std::to_string(static_cast<int>(host->target));
			EmitEvent("registFinished", fields);
			break;
		}
		case CHIAKI_REGIST_EVENT_TYPE_FINISHED_FAILED:
			CT_CHECKPOINT("RegistCb failed");
			EmitEvent("registFinished", "\"success\":false");
			break;
		case CHIAKI_REGIST_EVENT_TYPE_FINISHED_CANCELED:
			CT_CHECKPOINT("RegistCb canceled");
			EmitEvent("registFinished", "\"success\":false,\"canceled\":true");
			break;
		default:
			break;
	}
	if(state->regist_active &&
		event->type != CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS)
	{
		// keep object alive until ct_regist_stop() / next start
	}
}

} // namespace

// ===========================================================================
// Exported C ABI — called from JavaScript
// ===========================================================================

CT_EXPORT int ct_init()
{
	CT_CHECKPOINT("begin");
	if(g_state)
	{
		CT_CHECKPOINT("already initialized");
		return 0;
	}
	CT_CHECKPOINT("new BridgeState begin");
	g_state = new BridgeState();
	CT_CHECKPOINT("new BridgeState done");
	CT_CHECKPOINT("chiaki_log_init begin");
	chiaki_log_init(&g_state->log, CHIAKI_LOG_ALL & ~CHIAKI_LOG_VERBOSE, LogCb, nullptr);
	CT_CHECKPOINT("chiaki_log_init done");
	CT_CHECKPOINT("controller_state_set_idle begin");
	chiaki_controller_state_set_idle(&g_state->controller_state);
	CT_CHECKPOINT("controller_state_set_idle done");
	CT_CHECKPOINT("chiaki_lib_init begin");
	ChiakiErrorCode err = chiaki_lib_init();
	CT_CHECKPOINT("chiaki_lib_init done");
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(&g_state->log, "chiaki_lib_init failed: %s", chiaki_error_string(err));
		CT_CHECKPOINT("chiaki_lib_init failed");
		return -1;
	}
	CT_CHECKPOINT("EmitEvent ready begin");
	EmitEvent("ready");
	CT_CHECKPOINT("EmitEvent ready done");
	return 0;
}

// --- Discovery -------------------------------------------------------------

// Dispatched to the worker like every socket-touching entry point (the
// discovery socket is created on the calling thread inside
// chiaki_discovery_service_init). Return value is just "queued".
// broadcast_addr: subnet-directed broadcast (e.g. "192.168.1.255") or global.
// hosts_csv: comma-separated known console IPs to ALSO ping directly (unicast),
// so the online/offline status works even where UDP broadcast is filtered or
// unsupported by the TV's socket extension.
CT_EXPORT int ct_discovery_start(const char *broadcast_addr, const char *hosts_csv)
{
	std::string addr = broadcast_addr ? broadcast_addr : "";
	std::string hosts = hosts_csv ? hosts_csv : "";
	DispatchToWorker([addr, hosts]() {
		if(!g_state || g_state->discovery_active)
			return;

		std::string bcast = addr.empty() ? "255.255.255.255" : addr;

		// send_addr MUST be a real sockaddr with the family set — the service
		// reads sa_family from it at init and resolves send_host into it. (This
		// was previously left zeroed, which broke discovery entirely.)
		struct sockaddr_in send_sin;
		memset(&send_sin, 0, sizeof(send_sin));
		send_sin.sin_family = AF_INET;
		send_sin.sin_addr.s_addr = inet_addr(bcast.c_str());

		// Per-host unicast targets from the saved console list.
		std::vector<struct sockaddr_storage> targets;
		size_t p = 0;
		while(p < hosts.size())
		{
			size_t comma = hosts.find(',', p);
			std::string ip = hosts.substr(p, comma == std::string::npos ? std::string::npos : comma - p);
			while(!ip.empty() && (ip.front() == ' ' || ip.front() == '\t')) ip.erase(ip.begin());
			while(!ip.empty() && (ip.back() == ' ' || ip.back() == '\t')) ip.pop_back();
			if(!ip.empty())
			{
				in_addr_t a = inet_addr(ip.c_str());
				if(a != INADDR_NONE)
				{
					struct sockaddr_in si;
					memset(&si, 0, sizeof(si));
					si.sin_family = AF_INET;
					si.sin_addr.s_addr = a;
					struct sockaddr_storage ss;
					memset(&ss, 0, sizeof(ss));
					memcpy(&ss, &si, sizeof(si));
					targets.push_back(ss);
				}
			}
			if(comma == std::string::npos) break;
			p = comma + 1;
		}

		ChiakiDiscoveryServiceOptions options = {};
		options.hosts_max = 32;
		options.host_drop_pings = 3;
		options.ping_ms = 500;
		options.ping_initial_ms = 100;
		options.cb = DiscoveryCb;
		options.cb_user = g_state;
		options.send_addr = reinterpret_cast<struct sockaddr_storage *>(&send_sin);
		options.send_addr_size = sizeof(send_sin);
		options.send_host = const_cast<char *>(bcast.c_str());
		if(!targets.empty())
		{
			options.broadcast_addrs = targets.data();
			options.broadcast_num = targets.size();
		}

		ChiakiErrorCode err = chiaki_discovery_service_init(&g_state->discovery, &options, &g_state->log);
		if(err != CHIAKI_ERR_SUCCESS)
		{
			CHIAKI_LOGE(&g_state->log, "discovery init failed: %s", chiaki_error_string(err));
			return;
		}
		g_state->discovery_active = true;
		CHIAKI_LOGI(&g_state->log,
			"discovery started; broadcast %s + %zu unicast target(s) (ports %d/%d)",
			bcast.c_str(), targets.size(), CHIAKI_DISCOVERY_PORT_PS4, CHIAKI_DISCOVERY_PORT_PS5);
	});
	return 0;
}

CT_EXPORT void ct_discovery_stop()
{
	// chiaki_discovery_service_fini joins the service thread — blocking,
	// which is illegal on the browser main thread; must run on the worker.
	DispatchToWorker([]() {
		if(!g_state || !g_state->discovery_active)
			return;
		chiaki_discovery_service_fini(&g_state->discovery);
		g_state->discovery_active = false;
	});
}

// --- Wakeup ----------------------------------------------------------------

CT_EXPORT int ct_wakeup(const char *host, const char *regist_key_b64, int ps5)
{
	std::string h = host ? host : "";
	std::string key_b64 = regist_key_b64 ? regist_key_b64 : "";
	DispatchToWorker([h, key_b64, ps5]() {
		if(!g_state)
			return;

		// The user credential is the regist key interpreted as a hex string.
		uint8_t regist_key[CHIAKI_SESSION_AUTH_SIZE] = {0};
		size_t key_size = sizeof(regist_key);
		if(chiaki_base64_decode(key_b64.c_str(), key_b64.size(), regist_key, &key_size) != CHIAKI_ERR_SUCCESS)
		{
			CHIAKI_LOGE(&g_state->log, "wakeup: bad regist key");
			return;
		}
		char key_str[CHIAKI_SESSION_AUTH_SIZE + 1] = {0};
		memcpy(key_str, regist_key, CHIAKI_SESSION_AUTH_SIZE);
		uint64_t credential = strtoull(key_str, nullptr, 16);

		ChiakiErrorCode err = chiaki_discovery_wakeup(&g_state->log, nullptr, h.c_str(), credential, ps5 != 0);
		if(err != CHIAKI_ERR_SUCCESS)
			CHIAKI_LOGE(&g_state->log, "wakeup failed: %s", chiaki_error_string(err));
	});
	return 0;
}

// --- Registration (pairing) -------------------------------------------------

namespace
{

// Every exit path emits "registFinished" on failure — since ct_regist_start()
// below now dispatches asynchronously and can't return a synchronous result
// (see CT_DISPATCH_TO_WORKER), this is the only way JS finds out an attempt
// never actually started. Success/failure of the pairing exchange itself
// still arrives later via RegistCb -> "registFinished" same as before.
void RegistStartImpl(const char *host, int ps5, const char *psn_account_id_b64, uint32_t pin)
{
	CT_CHECKPOINT("regist_start begin");
	if(!g_state || g_state->regist_active)
	{
		CT_CHECKPOINT("regist_start bad state");
		EmitEvent("registFinished", "\"success\":false,\"error\":\"internal error (bad state)\"");
		return;
	}

	ChiakiRegistInfo info = {};
	info.target = ps5 ? CHIAKI_TARGET_PS5_1 : CHIAKI_TARGET_PS4_10;
	info.host = host;
	info.broadcast = false;
	info.psn_online_id = nullptr;
	info.pin = pin;
	info.console_pin = 0;
	info.holepunch_info = nullptr;
	info.rudp = nullptr;

	size_t account_id_size = sizeof(info.psn_account_id);
	CT_CHECKPOINT("regist_start base64 decode begin");
	if(chiaki_base64_decode(psn_account_id_b64, strlen(psn_account_id_b64),
		info.psn_account_id, &account_id_size) != CHIAKI_ERR_SUCCESS
		|| account_id_size != CHIAKI_PSN_ACCOUNT_ID_SIZE)
	{
		CT_CHECKPOINT("regist_start bad PSN account id");
		EmitEvent("registFinished", "\"success\":false,\"error\":\"bad PSN account id\"");
		return;
	}
	CT_CHECKPOINT("regist_start base64 decode done");

	CT_CHECKPOINT("chiaki_regist_start begin");
	ChiakiErrorCode err = chiaki_regist_start(&g_state->regist, &g_state->log, &info, RegistCb, g_state);
	CT_CHECKPOINT("chiaki_regist_start done");
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CT_CHECKPOINT("chiaki_regist_start failed");
		EmitEvent("registFinished", "\"success\":false,\"error\":\"could not start pairing\"");
		return;
	}
	g_state->regist_active = true;
	CT_CHECKPOINT("regist_start dispatched");
}

} // namespace

CT_EXPORT int ct_regist_start(const char *host, int ps5, const char *psn_account_id_b64, uint32_t pin)
{
	// std::string copies in the capture — the caller's pointers (from
	// Module.cwrap's string marshaling) are only valid for the duration of
	// this call, long gone by the time the closure runs on the worker.
	std::string h = host ? host : "";
	std::string account = psn_account_id_b64 ? psn_account_id_b64 : "";
	DispatchToWorker([h, ps5, account, pin]() {
		RegistStartImpl(h.c_str(), ps5, account.c_str(), pin);
	});
	return 0; // queued — actual outcome always arrives via "registFinished"
}

CT_EXPORT void ct_regist_stop()
{
	// chiaki_regist_stop/fini join the regist thread — blocking, illegal on
	// the browser main thread; must run on the worker.
	DispatchToWorker([]() {
		if(!g_state || !g_state->regist_active)
			return;
		chiaki_regist_stop(&g_state->regist);
		chiaki_regist_fini(&g_state->regist);
		g_state->regist_active = false;
	});
}

// --- Streaming session -------------------------------------------------------

namespace
{

// Dispatched to the worker: chiaki_session_init resolves the host via
// getaddrinfo (needs working host bindings) and chiaki_session_start spawns
// the session thread. Like RegistStartImpl, every failure path must emit an
// event ("quit") — the async ct_session_start below can't return a result,
// so app.js learns of start failures the same way it learns of runtime
// disconnects.
void SessionStartImpl(const char *host, int ps5,
	const char *regist_key_b64, const char *morning_b64,
	int resolution_preset, int fps_preset, int hdr,
	ChiakiHolepunchSession holepunch)
{
	// HDR is HEVC Main10, which only PS5 sends; never request it for a PS4.
	bool want_hdr = hdr != 0 && ps5 != 0;
	if(!g_state || g_state->session_active)
	{
		EmitEvent("quit", "\"reason\":\"internal error (bad state)\",\"isError\":true");
		return;
	}

	ChiakiConnectInfo connect_info = {};
	connect_info.ps5 = ps5 != 0;
	connect_info.host = host;
	// The EMSS player's codec is fixed up-front from the *requested* profile, so
	// a silent server-side downgrade (HEVC -> H.264) would feed the wrong codec
	// into an already-configured decoder. Allow downgrade only on the plain
	// H.264 path, where there is nothing lower to fall back to anyway.
	connect_info.video_profile_auto_downgrade = !want_hdr;
	// Remote (over-internet) play: the holepunch session carries the punched
	// CTRL/DATA sockets + console address. NULL on the local-network path, in
	// which case chiaki connects to `host` directly (unchanged behavior).
	connect_info.holepunch_session = holepunch;
	connect_info.enable_keyboard = false;
	connect_info.enable_dualsense = false;
	connect_info.rudp_sock = nullptr;
	connect_info.packet_loss_max = 0.05;
	connect_info.enable_idr_on_fec_failure = true;

	size_t key_size = sizeof(connect_info.regist_key);
	if(chiaki_base64_decode(regist_key_b64, strlen(regist_key_b64),
		reinterpret_cast<uint8_t *>(connect_info.regist_key), &key_size) != CHIAKI_ERR_SUCCESS)
	{
		EmitEvent("quit", "\"reason\":\"bad regist key\",\"isError\":true");
		return;
	}
	size_t morning_size = sizeof(connect_info.morning);
	if(chiaki_base64_decode(morning_b64, strlen(morning_b64),
		connect_info.morning, &morning_size) != CHIAKI_ERR_SUCCESS
		|| morning_size != sizeof(connect_info.morning))
	{
		EmitEvent("quit", "\"reason\":\"bad rp key\",\"isError\":true");
		return;
	}

	chiaki_connect_video_profile_preset(&connect_info.video_profile,
		static_cast<ChiakiVideoResolutionPreset>(resolution_preset),
		static_cast<ChiakiVideoFPSPreset>(fps_preset));

	// TVs are H.264-safe everywhere; HDR is the opt-in HEVC Main10 path (PS5
	// only, see want_hdr above). The bitstream carries HDR10 static metadata
	// in-band; requesting CHIAKI_CODEC_H265_HDR tells the console to send it and
	// picks the Main10 mime in EmssPlayer. Experimental — verify on hardware.
	if(want_hdr)
		connect_info.video_profile.codec = CHIAKI_CODEC_H265_HDR;

	chiaki_tizen::VideoConfig video_cfg;
	video_cfg.width = connect_info.video_profile.width;
	video_cfg.height = connect_info.video_profile.height;
	video_cfg.fps = static_cast<unsigned>(fps_preset);
	video_cfg.h265 = want_hdr;
	video_cfg.hdr = want_hdr;

	// Seed HUD stats for this session.
	g_state->stream_w = video_cfg.width;
	g_state->stream_h = video_cfg.height;
	g_state->stream_fps = video_cfg.fps;
	g_state->stream_hdr = video_cfg.hdr;
	g_state->stat_bytes = 0;
	g_state->stat_frames = 0;
	g_state->stat_t0 = std::chrono::steady_clock::now();

	g_state->player.reset(new chiaki_tizen::EmssPlayer("stream-video"));
	if(!g_state->player->Start(video_cfg, chiaki_tizen::AudioConfig{}))
	{
		g_state->player.reset();
		EmitEvent("quit", "\"reason\":\"video player failed to start\",\"isError\":true");
		return;
	}

	ChiakiErrorCode err = chiaki_session_init(&g_state->session, &connect_info, &g_state->log);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(&g_state->log, "session init failed: %s", chiaki_error_string(err));
		g_state->player.reset();
		EmitEvent("quit", "\"reason\":\"session init failed\",\"isError\":true");
		return;
	}

	chiaki_opus_decoder_init(&g_state->opus_decoder, &g_state->log);
	chiaki_opus_decoder_set_cb(&g_state->opus_decoder, OpusSettingsCb, OpusFrameCb, g_state);
	ChiakiAudioSink audio_sink;
	chiaki_opus_decoder_get_sink(&g_state->opus_decoder, &audio_sink);
	chiaki_session_set_audio_sink(&g_state->session, &audio_sink);

	chiaki_session_set_video_sample_cb(&g_state->session, VideoSampleCb, g_state);
	chiaki_session_set_event_cb(&g_state->session, SessionEventCb, g_state);

	err = chiaki_session_start(&g_state->session);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(&g_state->log, "session start failed: %s", chiaki_error_string(err));
		chiaki_opus_decoder_fini(&g_state->opus_decoder);
		chiaki_session_fini(&g_state->session);
		g_state->player.reset();
		EmitEvent("quit", "\"reason\":\"session start failed\",\"isError\":true");
		return;
	}

	g_state->session_active = true;
}

} // namespace

CT_EXPORT int ct_session_start(const char *host, int ps5,
	const char *regist_key_b64, const char *morning_b64,
	int resolution_preset, int fps_preset, int hdr)
{
	std::string h = host ? host : "";
	std::string key_b64 = regist_key_b64 ? regist_key_b64 : "";
	std::string morning = morning_b64 ? morning_b64 : "";
	DispatchToWorker([h, ps5, key_b64, morning, resolution_preset, fps_preset, hdr]() {
		SessionStartImpl(h.c_str(), ps5, key_b64.c_str(), morning.c_str(),
			resolution_preset, fps_preset, hdr, /*holepunch=*/nullptr);
	});
	return 0; // queued — failures arrive via the "quit" event
}

// Remote (over-the-internet) session start. The JS signaling layer + WASM
// transport have, by this point, punched a CTRL and a DATA socket to the
// console and learned its address; their fds are passed here. We wrap them in
// the holepunch adapter (holepunch_bridge.c) and run the same session path as
// local play — chiaki drives the adapter for the control/data sockets.
//
// NOTE: the transport that produces ctrl_fd/data_fd (STUN + candidate check
// over the Tizen Sockets Extension, driven by the JS signaling state machine)
// is the next integration step; until it exists nothing calls this. The
// regist-info fields are zeroed here — an already-paired console (we hold its
// regist_key/morning) does not need the remote-registration data1/data2.
CT_EXPORT int ct_session_start_remote(int ps5,
	const char *regist_key_b64, const char *morning_b64,
	int resolution_preset, int fps_preset, int hdr,
	int ctrl_fd, int data_fd, const char *ps_ip, int ps_ctrl_port)
{
	std::string key_b64 = regist_key_b64 ? regist_key_b64 : "";
	std::string morning = morning_b64 ? morning_b64 : "";
	std::string ip = ps_ip ? ps_ip : "";
	DispatchToWorker([ps5, key_b64, morning, resolution_preset, fps_preset, hdr,
		ctrl_fd, data_fd, ip, ps_ctrl_port]() {
		ChiakiHolepunchRegistInfo rinfo;
		memset(&rinfo, 0, sizeof(rinfo));
		ChiakiHolepunchSession hp = ct_holepunch_bridge_new(
			ctrl_fd, data_fd, ip.c_str(), (uint16_t)ps_ctrl_port, &rinfo);
		if(!hp)
		{
			EmitEvent("quit", "\"reason\":\"holepunch bridge alloc failed\",\"isError\":true");
			return;
		}
		// On success chiaki owns the holepunch session and finis it via
		// chiaki_session_fini; on an early failure inside SessionStartImpl the
		// adapter is cleaned up there in a follow-up (TODO: wire fini on the
		// pre-session_init error paths).
		SessionStartImpl(ip.c_str(), ps5, key_b64.c_str(), morning.c_str(),
			resolution_preset, fps_preset, hdr, hp);
	});
	return 0;
}

// ===========================================================================
// PSN remote-play transport surface (the WASM half of the signaling).
// The JS state machine (app/js/psn-signaling.js) owns OAuth + the REST calls
// and drives these for the parts JS can't do: the authenticated push
// WebSocket, STUN, and the UDP hole-punch. All run on the worker thread.
// ===========================================================================

static std::string b64(const uint8_t *data, size_t len)
{
	if(!len)
		return std::string();
	size_t olen = 0;
	std::string out(((len + 2) / 3) * 4 + 1, '\0');
	if(mbedtls_base64_encode(reinterpret_cast<unsigned char *>(&out[0]), out.size(),
		&olen, data, len) != 0)
		return std::string();
	out.resize(olen);
	return out;
}

// Runs on its own worker thread for the lifetime of the push channel.
static void PsnWsListen(std::string token, std::string fqdn)
{
	std::string auth = "Authorization: Bearer " + token;
	const char *headers[] = {
		auth.c_str(),
		"Sec-WebSocket-Protocol: np-pushpacket",
		"User-Agent: WebSocket++/0.8.2",
		"X-PSN-APP-TYPE: REMOTE_PLAY",
		"X-PSN-APP-VER: RemotePlay/1.0",
		"X-PSN-KEEP-ALIVE-STATUS-TYPE: 3",
		"X-PSN-OS-VER: Windows/10.0",
		"X-PSN-PROTOCOL-VERSION: 2.1",
		"X-PSN-RECONNECTION: false",
	};
	ws_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.host = fqdn.c_str();
	cfg.port = "443";
	cfg.path = "/np/pushNotification";
	cfg.headers = headers;
	cfg.n_headers = (int)(sizeof(headers) / sizeof(headers[0]));
	cfg.verify_peer = 0; // TODO: bundle Sony CA roots and verify
	cfg.timeout_ms = 0;

	ws_client *c = nullptr;
	ws_status s = ws_connect(&cfg, &c);
	if(s != WS_OK)
	{
		EmitEvent("psnWsClosed", std::string("\"reason\":\"") + ws_strerror(s) + "\"");
		return;
	}
	if(g_state)
		g_state->psn_ws = c;
	EmitEvent("psnWsOpen");

	std::vector<uint8_t> buf(65536);
	int opcode = 0;
	size_t n = 0;
	while(g_state && g_state->psn_ws_running.load())
	{
		ws_status r = ws_recv(c, buf.data(), buf.size(), &opcode, &n);
		if(r == WS_ERR_CLOSED)
			break;
		if(r != WS_OK)
			continue;
		if(opcode == WS_OP_TEXT || opcode == WS_OP_BIN)
			EmitEvent("psnNotification", "\"data\":\"" + b64(buf.data(), n) + "\"");
	}
	ws_close(c);
	if(g_state)
		g_state->psn_ws = nullptr;
	EmitEvent("psnWsClosed", "\"reason\":\"ended\"");
}

// Open the authenticated push WebSocket (JS cannot set the required headers).
CT_EXPORT int ct_psn_ws_open(const char *token, const char *fqdn)
{
	std::string t = token ? token : "";
	std::string f = fqdn ? fqdn : "";
	DispatchToWorker([t, f]() {
		if(!g_state || g_state->psn_ws_running.load())
			return;
		g_state->psn_ws_running = true;
		g_state->psn_ws_thread = std::thread(PsnWsListen, t, f);
	});
	return 0;
}

CT_EXPORT void ct_psn_ws_close()
{
	DispatchToWorker([]() {
		if(!g_state)
			return;
		g_state->psn_ws_running = false;
		if(g_state->psn_ws_thread.joinable())
			g_state->psn_ws_thread.detach(); // ws_recv unblocks and the thread exits
	});
}

// Discover our public IP:port mapping (candidate) via STUN.
CT_EXPORT int ct_psn_stun_gather(const char *host, const char *port)
{
	std::string h = host ? host : "";
	std::string p = (port && *port) ? port : "3478";
	DispatchToWorker([h, p]() {
		char ip[64] = {0};
		uint16_t pub = 0, local = 0;
		stun_status s = stun_query(h.c_str(), p.c_str(), 4000, ip, sizeof(ip), &pub, &local);
		if(s == STUN_OK)
		{
			char fields[192];
			std::snprintf(fields, sizeof(fields),
				"\"ip\":\"%s\",\"port\":%u,\"localPort\":%u", ip, pub, local);
			EmitEvent("psnStun", fields);
		}
		else
			EmitEvent("psnStun", std::string("\"error\":\"") + stun_strerror(s) + "\"");
	});
	return 0;
}

// Hole-punch to the console's candidates. candidates_csv is "ip:port,ip:port".
// The signaling values come from psn-signaling.js: hashed_local is our random
// per-session 20-byte id (base64); hashed_console + sids are parsed from the
// console's answer message.
CT_EXPORT int ct_psn_punch(const char *candidates_csv, int port_type /*0=ctrl,1=data*/,
	const char *hashed_local_b64, const char *hashed_console_b64,
	int sid_local, int sid_console)
{
	std::string csv = candidates_csv ? candidates_csv : "";
	std::string hl_b64 = hashed_local_b64 ? hashed_local_b64 : "";
	std::string hc_b64 = hashed_console_b64 ? hashed_console_b64 : "";
	DispatchToWorker([csv, port_type, hl_b64, hc_b64, sid_local, sid_console]() {
		// parse "ip:port,ip:port,..."
		std::vector<std::string> ips;
		std::vector<uint16_t> ports;
		size_t i = 0;
		while(i < csv.size())
		{
			size_t comma = csv.find(',', i);
			std::string tok = csv.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
			size_t colon = tok.rfind(':');
			if(colon != std::string::npos)
			{
				ips.push_back(tok.substr(0, colon));
				ports.push_back((uint16_t)atoi(tok.c_str() + colon + 1));
			}
			if(comma == std::string::npos)
				break;
			i = comma + 1;
		}
		if(ips.empty())
		{
			EmitEvent("psnPunch", "\"error\":\"no candidates\"");
			return;
		}
		std::vector<punch_candidate> cands(ips.size());
		for(size_t k = 0; k < ips.size(); k++)
		{
			cands[k].ip = ips[k].c_str();
			cands[k].port = ports[k];
		}

		int fd = socket(AF_INET, SOCK_DGRAM, 0);
		if(fd < 0)
		{
			EmitEvent("psnPunch", "\"error\":\"socket failed\"");
			return;
		}

		punch_ids ids;
		memset(&ids, 0, sizeof(ids));
		ids.sid_local = (uint16_t)sid_local;
		ids.sid_console = (uint16_t)sid_console;
		size_t hl = sizeof(ids.hashed_id_local);
		chiaki_base64_decode(hl_b64.c_str(), hl_b64.size(), ids.hashed_id_local, &hl);
		size_t hc = sizeof(ids.hashed_id_console);
		chiaki_base64_decode(hc_b64.c_str(), hc_b64.size(), ids.hashed_id_console, &hc);
		int selected = -1;
		punch_status s = punch_run(fd, cands.data(), cands.size(),
			&ids, 500, 20, &selected);
		if(s == PUNCH_OK)
		{
			if(port_type == 0)
				g_state->psn_ctrl_fd = fd;
			else
				g_state->psn_data_fd = fd;
			char fields[128];
			std::snprintf(fields, sizeof(fields),
				"\"fd\":%d,\"selected\":%d,\"portType\":%d", fd, selected, port_type);
			EmitEvent("psnPunch", fields);
		}
		else
		{
			close(fd);
			EmitEvent("psnPunch", std::string("\"error\":\"") + punch_strerror(s) + "\"");
		}
	});
	return 0;
}

CT_EXPORT void ct_session_stop()
{
	// chiaki_session_join blocks until the session thread exits — illegal on
	// the browser main thread; must run on the worker.
	DispatchToWorker([]() {
		if(!g_state || !g_state->session_active)
			return;
		chiaki_session_stop(&g_state->session);
		chiaki_session_join(&g_state->session);
		chiaki_opus_decoder_fini(&g_state->opus_decoder);
		chiaki_session_fini(&g_state->session);
		if(g_state->player)
		{
			g_state->player->Stop();
			g_state->player.reset();
		}
		g_state->session_active = false;
	});
}

CT_EXPORT void ct_session_set_login_pin(const char *pin)
{
	std::string p = pin ? pin : "";
	DispatchToWorker([p]() {
		if(!g_state || !g_state->session_active)
			return;
		chiaki_session_set_login_pin(&g_state->session,
			reinterpret_cast<const uint8_t *>(p.c_str()), p.size());
	});
}

// --- Controller input --------------------------------------------------------

CT_EXPORT void ct_controller_state(uint32_t buttons,
	int l2, int r2, int left_x, int left_y, int right_x, int right_y)
{
	if(!g_state || !g_state->session_active)
		return;
	ChiakiControllerState &s = g_state->controller_state;
	s.buttons = buttons;
	s.l2_state = static_cast<uint8_t>(l2);
	s.r2_state = static_cast<uint8_t>(r2);
	s.left_x = static_cast<int16_t>(left_x);
	s.left_y = static_cast<int16_t>(left_y);
	s.right_x = static_cast<int16_t>(right_x);
	s.right_y = static_cast<int16_t>(right_y);
	chiaki_session_set_controller_state(&g_state->session, &s);
}

// --- Entry point --------------------------------------------------------------

int main()
{
	CT_CHECKPOINT("main begin");
#ifdef __EMSCRIPTEN__
	g_worker_thread = pthread_self();
	EM_ASM(noExitRuntime = true);
#endif
	CT_CHECKPOINT("main: noExitRuntime set");
	ct_init();
	CT_CHECKPOINT("main: ct_init returned");
	return 0;
}
