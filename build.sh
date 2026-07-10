#!/usr/bin/env bash
# chiaki-tizen build script.
#
# Prerequisites (one-time):
#   1. Samsung Emscripten SDK (NOT vanilla emsdk) — the fork with the Tizen
#      Sockets Extension and WASM Player API:
#      https://developer.samsung.com/smarttv/develop/extension-libraries/webassembly.html
#      Activate it so `emcmake`/`emcc` are on PATH:
#         source /path/to/emsdk/emsdk_env.sh
#   2. chiaki-ng checked out next to this repo (or set CHIAKI_NG_ROOT):
#         git clone --recurse-submodules https://github.com/streetpea/chiaki-ng.git
#   3. Optional: Tizen Studio with TV extensions + a Samsung certificate
#      profile (`tizen` CLI on PATH) if you want this script to sign/install.
#
# Usage:
#   ./build.sh wasm      # build the WebAssembly module
#   ./build.sh app       # assemble the web app with the built module
#   ./build.sh unsigned-wgt  # zip an unsigned .wgt for external sign/install tools
#   ./build.sh wgt       # package a signed .wgt with the tizen CLI
#   ./build.sh install   # install to the TV in developer mode (set TV_IP)
#   ./build.sh all       # wasm + app + unsigned-wgt

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
CHIAKI_NG_ROOT="${CHIAKI_NG_ROOT:-$HERE/../chiaki-ng}"
BUILD_DIR="$HERE/wasm/build"
APP_DIR="$HERE/app"
DIST_DIR="$HERE/dist"
CERT_PROFILE="${CERT_PROFILE:-default}"
TV_IP="${TV_IP:-}"

apply_patches() {
	for patch in "$HERE"/patches/*.patch; do
		if ! git -C "$CHIAKI_NG_ROOT" apply --check "$patch" 2>/dev/null; then
			if git -C "$CHIAKI_NG_ROOT" apply --reverse --check "$patch" 2>/dev/null; then
				echo "[patch] already applied: $(basename "$patch")"
				continue
			fi
			echo "[patch] does not apply cleanly: $(basename "$patch")" >&2
			echo "        check chiaki-ng version or reset the external checkout" >&2
			exit 1
		fi
		git -C "$CHIAKI_NG_ROOT" apply "$patch"
		echo "[patch] applied: $(basename "$patch")"
	done
}

build_wasm() {
	command -v emcmake >/dev/null || { echo "emcmake not found — source the Samsung emsdk env first" >&2; exit 1; }
	apply_patches
	emcmake cmake -S "$HERE/wasm" -B "$BUILD_DIR" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release \
		-DCHIAKI_NG_ROOT="$CHIAKI_NG_ROOT"
	ninja -C "$BUILD_DIR"
	echo "[wasm] built: $BUILD_DIR/chiaki-tizen.{js,wasm}"
}

assemble_app() {
	mkdir -p "$APP_DIR/wasm_modules"
	{
		cat "$APP_DIR/js/tizen-socket-host-shim.js"
		printf '\n'
		cat "$BUILD_DIR/chiaki-tizen.js"
	} > "$APP_DIR/wasm_modules/chiaki-tizen.js"
	cp "$BUILD_DIR/chiaki-tizen.wasm" "$APP_DIR/wasm_modules/"
	python3 - "$APP_DIR/wasm_modules/chiaki-tizen.js" <<'EOF'
from pathlib import Path
import sys

import re

path = Path(sys.argv[1])
text = path.read_text()
old = 'worker.onerror=function(e){err("pthread sent an error! "+e.filename+":"+e.lineno+": "+e.message)};'
new = '''worker.onerror=function(e){
var detail="pthread error "+(e.filename||"worker")+":"+(e.lineno||0)+":"+(e.colno||0)+" "+(e.message||"Script error");
err(detail);
if(typeof Module!=="undefined"&&Module["printErr"]) Module["printErr"](detail);
};'''
if old in text:
    text = text.replace(old, new)
else:
    print("[app] warning: worker error hook pattern not found", file=sys.stderr)
text = text.replace('function addRunDependency(id){assert(!ENVIRONMENT_IS_PTHREAD,"addRunDependency cannot be used in a pthread worker");',
                    'function addRunDependency(id){if(!ENVIRONMENT_IS_PTHREAD&&typeof __ctReport==="function")__ctReport("runtime","add dep "+id);assert(!ENVIRONMENT_IS_PTHREAD,"addRunDependency cannot be used in a pthread worker");')
text = text.replace('function removeRunDependency(id){runDependencies--;',
                    'function removeRunDependency(id){if(!ENVIRONMENT_IS_PTHREAD&&typeof __ctReport==="function")__ctReport("runtime","remove dep "+id);runDependencies--;')
text = text.replace('function preRun(){if(ENVIRONMENT_IS_PTHREAD)return;',
                    'function preRun(){if(!ENVIRONMENT_IS_PTHREAD&&typeof __ctReport==="function")__ctReport("runtime","preRun");if(ENVIRONMENT_IS_PTHREAD)return;')
text = text.replace('function initRuntime(){runtimeInitialized=true;',
                    'function initRuntime(){if(!ENVIRONMENT_IS_PTHREAD&&typeof __ctReport==="function")__ctReport("runtime","initRuntime");runtimeInitialized=true;')
# Bracket callRuntimeCallbacks(__ATINIT__) — this is where wasm's own global
# constructors (dynCall_v/dynCall_vi for each __ATINIT__ entry) actually run,
# and it's the last thing initRuntime() does before doRun() moves on to
# preMain()/onRuntimeInitialized(). If boot stalls silently inside one of
# these ctors, "atinit begin" will show with no matching "atinit end".
text = text.replace('TTY.init();callRuntimeCallbacks(__ATINIT__)}',
                    'TTY.init();if(typeof __ctReport==="function")__ctReport("runtime","atinit begin "+__ATINIT__.length);callRuntimeCallbacks(__ATINIT__);if(typeof __ctReport==="function")__ctReport("runtime","atinit end")}')
text = text.replace('function doRun(){if(calledRun)return;',
                    'function doRun(){if(typeof __ctReport==="function")__ctReport("runtime","doRun");if(calledRun)return;')
# Bracket the rest of doRun() so we can see exactly which of
# initRuntime/preMain/onRuntimeInitialized/callMain never returns.
text = text.replace(
    'initRuntime();preMain();if(Module["onRuntimeInitialized"])Module["onRuntimeInitialized"]();if(shouldRunNow)callMain(args);postRun()}',
    'initRuntime();'
    'if(typeof __ctReport==="function")__ctReport("runtime","initRuntime returned");'
    'preMain();'
    'if(typeof __ctReport==="function")__ctReport("runtime","preMain returned");'
    'if(Module["onRuntimeInitialized"]){'
    'if(typeof __ctReport==="function")__ctReport("runtime","onRuntimeInitialized begin");'
    'Module["onRuntimeInitialized"]();'
    'if(typeof __ctReport==="function")__ctReport("runtime","onRuntimeInitialized end")'
    '}'
    'if(shouldRunNow){'
    'if(typeof __ctReport==="function")__ctReport("runtime","callMain begin");'
    'callMain(args);'
    'if(typeof __ctReport==="function")__ctReport("runtime","callMain end")'
    '}'
    'postRun()}')
# NOTE: this used to force the pool down to a hardcoded size of 1 (to get a
# fixed string to pattern-match below) before injecting the diagnostic log.
# That silently overrode PTHREAD_POOL_SIZE from wasm/CMakeLists.txt (16) in
# the shipped build, starving chiaki's own session/ctrl/takion/etc. threads
# and Tizen's WebKit cannot lazily create pthread workers beyond the
# pre-allocated pool once it's exhausted. Match the real size via regex
# instead, so the CMake-configured pool size reaches the device untouched.
pool_pattern = re.compile(r'PThread\.allocateUnusedWorkers\((\d+),function\(\)\{removeRunDependency\("pthreads"\)\}\)')
m = pool_pattern.search(text)
if m:
    pool_size = m.group(1)
    replacement = ('typeof __ctReport==="function"&&__ctReport("pthread","allocate pool ' + pool_size + '");'
                    'PThread.allocateUnusedWorkers(' + pool_size + ',function(){'
                    'typeof __ctReport==="function"&&__ctReport("pthread","pool ready");'
                    'removeRunDependency("pthreads")})')
    text = text[:m.start()] + replacement + text[m.end():]
else:
    print("[app] warning: pthread pool allocate pattern not found", file=sys.stderr)
path.write_text(text)
EOF
	# Emscripten pthread builds also emit a worker script
	if [ -f "$BUILD_DIR/chiaki-tizen.worker.js" ]; then
		{
			cat "$APP_DIR/js/tizen-socket-host-shim.js"
			printf '\n'
			cat "$APP_DIR/js/worker-error-reporting.js"
			printf '\n'
			cat "$BUILD_DIR/chiaki-tizen.worker.js"
		} > "$APP_DIR/wasm_modules/chiaki-tizen.worker.js"
	fi
	# Placeholder icon if none provided
	[ -f "$APP_DIR/icon.png" ] || python3 - "$APP_DIR/icon.png" <<'EOF'
import struct, sys, zlib
w = h = 512
raw = b''.join(b'\x00' + b'\x0b\x0e\x14\xff' * w for _ in range(h))
def chunk(t, d):
    c = struct.pack('>I', len(d)) + t + d
    return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = (b'\x89PNG\r\n\x1a\n'
    + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
    + chunk(b'IDAT', zlib.compress(raw))
    + chunk(b'IEND', b''))
open(sys.argv[1], 'wb').write(png)
EOF
	echo "[app] assembled in $APP_DIR"
}

package_unsigned_wgt() {
	command -v zip >/dev/null || { echo "zip not found" >&2; exit 1; }
	[ -f "$APP_DIR/config.xml" ] || { echo "missing app/config.xml" >&2; exit 1; }
	[ -f "$APP_DIR/wasm_modules/chiaki-tizen.js" ] || { echo "missing WASM JS bundle — run ./build.sh app first" >&2; exit 1; }
	[ -f "$APP_DIR/wasm_modules/chiaki-tizen.wasm" ] || { echo "missing WASM binary — run ./build.sh app first" >&2; exit 1; }
	mkdir -p "$DIST_DIR"
	rm -f "$DIST_DIR/chiaki-tizen-unsigned.wgt"
	(
		cd "$APP_DIR"
		zip -qr "$DIST_DIR/chiaki-tizen-unsigned.wgt" . \
			-x "*.wgt" "prefill.json" "prefill.example.json" ".DS_Store" "*/.DS_Store" "author-signature.xml" "signature*.xml"
	)
	echo "[wgt] unsigned package created: $DIST_DIR/chiaki-tizen-unsigned.wgt"
}

package_wgt() {
	command -v tizen >/dev/null || { echo "tizen CLI not found — install Tizen Studio" >&2; exit 1; }
	tizen package -t wgt -s "$CERT_PROFILE" -- "$APP_DIR"
	echo "[wgt] package created in $APP_DIR"
}

install_tv() {
	[ -n "$TV_IP" ] || { echo "set TV_IP=<tv address> (Developer Mode on, host IP whitelisted)" >&2; exit 1; }
	sdb connect "$TV_IP"
	tizen install -n "$(ls "$APP_DIR"/*.wgt | head -1)" -t "$(sdb devices | awk 'NR==2{print $1}')"
}

case "${1:-all}" in
	wasm) build_wasm ;;
	app) assemble_app ;;
	unsigned-wgt) package_unsigned_wgt ;;
	wgt) package_wgt ;;
	install) install_tv ;;
	all) build_wasm; assemble_app; package_unsigned_wgt ;;
	*) echo "usage: $0 [wasm|app|unsigned-wgt|wgt|install|all]"; exit 1 ;;
esac
