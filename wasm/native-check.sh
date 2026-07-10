#!/usr/bin/env bash
# Native compile-and-link check for the chiaki-tizen wasm module.
# Verifies every chiaki API call and every stubbed symbol against a real
# chiaki-ng build, using mock Samsung SDK headers. No Samsung SDK required.
#
# Usage: ./native-check.sh /path/to/chiaki-ng
# chiaki-ng must have been configured+built once natively
# (cmake -B build -DCHIAKI_ENABLE_GUI=OFF -DCHIAKI_ENABLE_CLI=OFF ... && ninja -C build chiaki-lib)

set -euo pipefail
CH="${1:?usage: $0 /path/to/chiaki-ng}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

echo "[1/4] emss_player.cc (mock Samsung headers)"
g++ -std=gnu++17 -Wall -c "$HERE/src/emss_player.cc" \
	-I "$HERE/native-check" -I "$HERE/src" -o "$OUT/emss.o"

echo "[2/4] bridge.cc (real chiaki headers)"
g++ -std=gnu++17 -Wall -c "$HERE/src/bridge.cc" \
	-I "$HERE/native-check" -I "$HERE/src" \
	-I "$CH/lib/include" -I "$CH/build/lib/include" -o "$OUT/bridge.o"

echo "[3/4] holepunch_stub.c (real chiaki headers)"
gcc -std=gnu11 -Wall -c "$HERE/src/holepunch_stub.c" \
	-I "$CH/lib/include" -I "$CH/build/lib/include" -o "$OUT/stub.o"

echo "[4/4] full link against libchiaki.a (minus holepunch/rudp objects)"
mkdir "$OUT/objs" && cd "$OUT/objs"
ar x "$CH/build/lib/libchiaki.a"
rm -f holepunch.c.o rudp.c.o rudpsendbuffer.c.o
ar rcs "$OUT/libchiaki_core.a" ./*.o
g++ "$OUT/bridge.o" "$OUT/emss.o" "$OUT/stub.o" "$OUT/libchiaki_core.a" \
	"$CH/build/third-party/nanopb/libprotobuf-nanopb.a" \
	"$CH/build/third-party/libjerasure.a" \
	"$CH/build/third-party/libgf_complete.a" \
	-lssl -lcrypto -lopus -lpthread -o "$OUT/linktest"

echo "OK — all chiaki API usage and stub symbols verified."
