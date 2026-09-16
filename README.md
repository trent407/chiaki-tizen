# chiaki-tizen

A port of [chiaki-ng](https://github.com/streetpea/chiaki-ng) (open-source
PlayStation Remote Play) to Samsung Tizen Smart TVs, running chiaki's core
protocol library as a WebAssembly module inside a Tizen web application.

Status: **working local-network Remote Play on Samsung TV hardware** — video
(H.264, 720p/1080p at 30/60), audio, the on-screen console list / pairing /
quality settings and a toggleable debug overlay are all functional. The whole
WASM module also compiles and links against real chiaki-ng via the native check
(`wasm/native-check.sh`). Remote Play over the *internet* is not yet implemented
(local network only — see "Known gaps"). Experimental HDR for PS5 (HEVC Main10)
is available as an opt-in Settings toggle.

## Downloads

- Source code lives in this repository.
- Prebuilt unsigned WGTs are attached to GitHub Releases.
- Do not use a personal WGT for sharing. Personal packages may include
  `app/prefill.json`; release packages intentionally exclude it.

## Architecture

```
┌────────────────────────── Tizen web app (.wgt) ──────────────────────────┐
│  index.html + app.js            10-foot UI, D-pad nav, Gamepad API       │
│        │  Module.ccall(ct_*)                    ▲ JSON events            │
│  ┌─────▼──────────────────────────────────────────────────────────────┐  │
│  │ chiaki-tizen.wasm (Samsung Emscripten, PROXY_TO_PTHREAD)           │  │
│  │  bridge.cc ── chiaki-ng core (session/takion/ctrl/regist/discovery)│  │
│  │      │              │ UDP/TCP: Tizen Sockets Extension (POSIX)     │  │
│  │      │ H.264 AUs    │ Opus → PCM (libopus, in wasm)                │  │
│  │  ┌───▼──────────────▼───┐                                          │  │
│  │  │ emss_player.cc       │  ElementaryMediaStreamSource             │  │
│  │  │ (WASM Player, kUltraLow, kMediaElement)                         │  │
│  │  └───────────┬──────────┘                                          │  │
│  └──────────────┼─────────────────────────────────────────────────────┘  │
│         <video> │ hardware decode + render by the TV media pipeline      │
└─────────────────▼─────────────────────────────────────────────────────────┘
```

This is the same architecture Samsung used for their official Moonlight
(NVIDIA GameStream) port, which is the strongest evidence the approach works:
raw sockets via the Tizen Sockets Extension, video decoded in hardware via
the WASM Player in ultra-low latency mode, audio decoded to PCM in software.

## Repository layout

- `wasm/` — the WebAssembly module
  - `src/bridge.cc` — chiaki session/discovery/registration glue, `ct_*` C ABI
  - `src/emss_player.{hh,cc}` — Samsung WASM Player pipeline (video + audio)
  - `src/tizen_compat.h` — socket compat shim (no fcntl, WASI errno, SOCK_NONBLOCK)
  - `src/holepunch_stub.c` — stubs for PSN-over-internet code paths (local play only)
  - `gen/` — pre-generated `takion.pb.{c,h}` (nanopb) and `chiaki/config.h`,
    so the WASM build needs no protoc
  - `native-check/` — mock Samsung SDK headers for compiling/linking the whole
    thing natively as a type-and-symbol check
  - `CMakeLists.txt` — builds chiaki core + mbedTLS + opus + jerasure + nanopb
- `app/` — the Tizen web application (config.xml, UI, input handling)
- `patches/0001-tizen-wasm-compat.patch` — small chiaki-ng patch:
  - reuses the Switch UDP-socket stop pipe on Tizen (poll/select on Tizen
    cannot mix socket and file descriptors)
  - makes `chiaki_socket_set_nonblock` a safe no-op (sockets are created
    `SOCK_NONBLOCK`; the extension has no `fcntl`)
- `build.sh` — patch, build, assemble, and package either unsigned or signed WGTs

## Building

1. Install the **Samsung Emscripten SDK** (their fork — vanilla emsdk lacks
   the Tizen Sockets Extension and WASM Player headers).
2. `git clone --recurse-submodules https://github.com/streetpea/chiaki-ng.git`
   next to this repo (only `third-party/nanopb`, `jerasure`, `gf-complete`
   submodules are strictly needed).
3. `source <samsung-emsdk>/emsdk_env.sh`
4. `./build.sh all`
5. Sign and install `dist/chiaki-tizen-unsigned.wgt` with your preferred
   Samsung TV deployment tool, such as Jellyfin2Samsung.

The `wgt` and `install` targets are still available for Tizen Studio users,
but the default `all` target intentionally creates an unsigned WGT for
external signing/install tooling.

### Native validation (no Samsung SDK needed)

The entire module — bridge, player, stubs — compiles and links against real
chiaki-ng on a Linux box using the mock headers:

```
cd wasm && ./native-check.sh /path/to/chiaki-ng
```

This is how every chiaki API call in this port was verified.

## Using it

1. Console and TV on the same network; Remote Play enabled on the console.
2. Get your PSN Account ID in base64 (e.g. via chiaki-ng's `psn-account-id`
   script) — enter it once, it is remembered.
3. Pair: console Settings › System › Remote Play › Link Device → enter the
   8-digit code in the app.
4. Connect a controller — see "Controller setup" below. The TV remote works
   for menus (color keys map to △○✕□).

### Debug reports and saved data

- Press the **Blue** remote key to open the on-screen debug log. If the app is
  stuck on the splash/blue screen, press **Blue**, then **Enter** on **Send
  debug**. The report redacts IPs and token-like strings, and also stays visible
  on screen if the TV cannot open a share/issue flow.
- Settings includes **Send debug report** for the same redacted share/issue flow
  from the normal UI.
- Settings also includes **Reset saved data**. Press it twice to clear saved
  consoles, pairing keys, PSN token, and stream settings. This is useful after
  reinstalling through a source that uses the same Tizen app id, because some
  TVs preserve app web storage across uninstall/reinstall.
- If your router gives the PlayStation a new IP address, discovery will try to
  relink saved paired profiles to the new address automatically. It first uses
  the console's discovery host id when available; for older saved entries, it
  only guesses when there is exactly one offline saved console of that type and
  exactly one newly discovered console of that type.

## Controller setup

Two ways to use a DualShock 4 / DualSense, and the choice matters:

**Recommended — pair the controller directly to the PlayStation.** Put the
controller in pairing mode and pair it to the console itself (not the TV). Input
then travels controller → console over Bluetooth, and the TV app is purely the
video/audio surface. This is the better setup for same-room / local-network play
because it unlocks everything the TV-side path cannot: **rumble, haptics, the
mic, motion controls, and the touchpad** all work natively, and — importantly —
the **PS button goes to the console** (opens the PS menu) instead of exiting the
app. Requires being in Bluetooth range of the console.

**Alternative — pair the controller to the TV** over Bluetooth. The app reads it
via the browser Gamepad API and forwards inputs over the network. This works
anywhere the app runs, but is limited to buttons/sticks/triggers (no rumble,
mic, motion, or touchpad), and the **physical PS/Guide button will bounce you to
the Samsung TV home screen** — Tizen intercepts it at the OS level before the app
sees it, and there is no app-side override (the Gamepad API is polling-only, and
Home is a reserved system key). If you use this path, just avoid the PS button.

## Known gaps / next steps

- **PSN remote connection (holepunch) is stubbed** — local network only.
  The real implementation needs PSN signaling plus NAT traversal; the RUDP
  transport is separable and cheap to un-stub, only `holepunch.c` needs the
  heavy libraries. Full scoped plan in
  `docs/remote-play-over-internet-plan.md`.
- HDR for PS5 (HEVC Main10) is an **experimental opt-in** toggle in Settings
  (`VideoConfig.hdr` → `CHIAKI_CODEC_H265_HDR`). Off by default, which leaves
  the known-good H.264 path untouched; on that path auto-downgrade is disabled
  so the console can't silently hand back H.264 into an HEVC-configured decoder.
  Unverified on hardware — enable only on a PS5 + HDR-capable TV.
- Controller paired to the TV: **rumble works** (chiaki rumble events drive the
  Gamepad API's `vibrationActuator`), but mic / motion / touchpad / haptics /
  adaptive triggers do not — those need raw HID, which Tizen's web sandbox
  doesn't expose. Pairing the controller directly to the console restores all of
  them natively — see "Controller setup".
- Audio/video sync in kUltraLow mode is render-on-arrival; if audio drifts,
  the next step is dropping PCM packets when the queue depth grows.
- `PTHREAD_POOL_SIZE=16` is an estimate with headroom; if session start
  hangs, raise it (threads cannot spawn lazily once the pool is exhausted).

## Licensing

chiaki-ng is AGPL-3.0 (with OpenSSL exception). This port links against it
and is therefore AGPL-3.0 as well. Keep source availability in mind if you
distribute the .wgt.
