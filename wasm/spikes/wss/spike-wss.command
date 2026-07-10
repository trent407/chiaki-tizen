#!/usr/bin/env bash
# chiaki-tizen — WSS spike runner (native, on your Mac).
# Double-click in Finder. Compiles the mbedTLS + WebSocket client natively
# against the mbedTLS 2.28 already fetched by your wasm build, runs the offline
# unit tests, then does a LIVE TLS + WebSocket handshake to a public echo server
# to prove the handshake works end-to-end on a real network.
#
# This validates everything except the Tizen-socket BIO swap and Sony's auth,
# which are only testable on the TV (see docs/remote-play-phase1-design.md §7).

set -o pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"                 # spikes/wss -> repo root
MB="$REPO/wasm/build/_deps/mbedtls-src"              # fetched by ./build.sh wasm
CACHE="$HERE/.mbedtls-native.a"

echo "==> spike dir: $HERE"
echo "==> mbedTLS:   $MB"
echo

if [ ! -d "$MB/library" ]; then
  echo "!! mbedTLS sources not found at $MB"
  echo "   Run ./build.sh wasm once first (it fetches mbedTLS 2.28), then retry."
  echo; read -n 1 -s -r -p "Press any key to close…"; exit 1
fi

# Build a native mbedTLS static lib once, cache it for fast re-runs.
if [ ! -f "$CACHE" ]; then
  echo "==> compiling mbedTLS 2.28 natively (one-time, ~30s)…"
  TMP="$(mktemp -d)"
  ( cd "$TMP" && cc -O1 -w -I "$MB/include" -I "$MB/library" -c "$MB"/library/*.c )
  if [ $? -ne 0 ]; then
    echo "!! mbedTLS compile failed"; rm -rf "$TMP"
    echo; read -n 1 -s -r -p "Press any key to close…"; exit 1
  fi
  ar rcs "$CACHE" "$TMP"/*.o && rm -rf "$TMP"
  echo "   cached: $CACHE"
else
  echo "==> using cached native mbedTLS lib ($CACHE)"
fi

echo "==> compiling the WSS spike…"
if ! cc -std=gnu11 -Wall -I "$MB/include" \
      "$HERE/ws_client.c" "$HERE/ws_client_test.c" "$CACHE" \
      -o "$HERE/ws_client_test"; then
  echo "!! spike compile failed"
  echo; read -n 1 -s -r -p "Press any key to close…"; exit 1
fi

echo
echo "======================== offline unit tests ========================"
"$HERE/ws_client_test"
echo
echo "==================== live TLS + WebSocket handshake ================="
# Try a few reliable public echo servers; stop at the first that connects.
LIVE_OK=0
while IFS='|' read -r host port path; do
  [ -z "$host" ] && continue
  echo
  echo "-- trying wss://$host:$port$path --"
  if "$HERE/ws_client_test" live "$host" "$port" "$path" | sed -n '/live. TLS/,$p'; then
    LIVE_OK=1; break
  fi
done <<'SERVERS'
ws.postman-echo.com|443|/raw
echo.websocket.org|443|/
ws.ifelse.io|443|/
echo.websocket.events|443|/
SERVERS

if [ "$LIVE_OK" != "1" ]; then
  echo
  echo "!! No echo server connected. If unit tests passed above, the CLIENT is"
  echo "   fine — this is network/DNS (VPN, firewall, or all echo hosts down)."
  echo "   Try from another network, or point it at any wss server yourself:"
  echo "     $HERE/ws_client_test live <host> <port> <path>"
fi

echo
echo "Tip: point it at any wss server:  ./ws_client_test live <host> <port> <path>"
echo
read -n 1 -s -r -p "Press any key to close this window…"
echo
