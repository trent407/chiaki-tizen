#!/usr/bin/env bash
# chiaki-tizen — one-click HDR rebuild.
# Double-click in Finder. Sources the Samsung Emscripten SDK, then runs the
# full build (wasm + app + unsigned .wgt) with the correct chiaki-ng path.

REPO="$(cd "$(dirname "$0")" && pwd)"
EMSDK="/Users/trentconnor/tizen-emsdk"
EMSDK_ENV="$EMSDK/env.sh"   # Samsung's activation wrapper (sets EM_CONFIG + PATH)
# Build against the repo's VENDORED chiaki-ng (nanopb 0.4.9.1, patches applied),
# which matches the pre-generated wasm/gen/takion.pb.* headers. NOT ~/chiaki-ng
# (a stale 0.3.x checkout whose streamconnection.c won't compile against them).
export CHIAKI_NG_ROOT="${CHIAKI_NG_ROOT:-$REPO/external/chiaki-ng}"

cd "$REPO" || { echo "!! cannot cd to $REPO"; read -n 1 -s -r -p "Press any key…"; exit 1; }

echo "==> repo:      $REPO"
echo "==> emsdk:     $EMSDK"
echo "==> chiaki-ng: $CHIAKI_NG_ROOT"
echo

if [ ! -f "$EMSDK_ENV" ]; then
  echo "!! env.sh not found at $EMSDK_ENV"
  echo "   Edit the EMSDK= line near the top of this script to point at your"
  echo "   Samsung Emscripten SDK, then double-click again."
  echo; read -n 1 -s -r -p "Press any key to close…"; exit 1
fi

echo "==> sourcing Samsung emsdk env: $EMSDK_ENV"
# shellcheck disable=SC1091
source "$EMSDK_ENV"

if ! command -v emcmake >/dev/null 2>&1; then
  echo "!! emcmake still not on PATH after sourcing env.sh — SDK may be incomplete."
  echo; read -n 1 -s -r -p "Press any key to close…"; exit 1
fi
echo "==> emcc: $(command -v emcc)"

echo "==> building (wasm + app + unsigned wgt)…"
echo
if ./build.sh all; then
  echo
  echo "✅ Done — rebuilt HDR-capable wasm and repackaged:"
  echo "     $REPO/dist/chiaki-tizen-unsigned.wgt"
  echo
  echo "   Next: sign + install with Jellyfin2Samsung, then in the app"
  echo "   go to Settings → HDR → \"On — HEVC HDR10\" and stream your PS5."
else
  status=$?
  echo
  echo "❌ build failed (exit $status). Scroll up to the FIRST error line."
  echo "   Copy it (or screenshot this window) and I can help debug."
fi

echo
read -n 1 -s -r -p "Press any key to close this window…"
echo
