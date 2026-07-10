#!/usr/bin/env bash
# chiaki-tizen — STUN spike runner (native, on your Mac).
# Double-click in Finder. Compiles the STUN client (pure UDP, no deps), runs the
# offline unit tests, then does a LIVE query to real STUN servers to print your
# public IP:port mapping and the local source port used.

set -o pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

echo "==> spike dir: $HERE"
echo "==> compiling STUN spike…"
if ! cc -std=gnu11 -Wall "$HERE/stun.c" "$HERE/stun_test.c" -o "$HERE/stun_test"; then
  echo "!! compile failed"; echo; read -n 1 -s -r -p "Press any key to close…"; exit 1
fi

echo
echo "======================== offline unit tests ========================"
"$HERE/stun_test"

echo
echo "======================= live STUN query ============================"
OK=0
while IFS='|' read -r host port; do
  [ -z "$host" ] && continue
  echo
  echo "-- querying $host:$port --"
  if "$HERE/stun_test" live "$host" "$port" | sed -n '/live. STUN/,$p'; then
    OK=1; break
  fi
done <<'SERVERS'
stun.l.google.com|19302
stun1.l.google.com|19302
stun.cloudflare.com|3478
SERVERS

if [ "$OK" != "1" ]; then
  echo
  echo "!! No STUN server answered. If unit tests passed, the CLIENT is fine —"
  echo "   this is network/UDP being blocked (some networks drop outbound UDP)."
fi

echo
read -n 1 -s -r -p "Press any key to close this window…"
echo
