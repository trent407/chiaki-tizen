#!/usr/bin/env bash
# chiaki-tizen — hole-punch spike runner (native, on your Mac).
# Double-click in Finder. Compiles the candidate-check punch (pure UDP), runs
# the packet unit tests, then a full punch round trip over UDP loopback against
# an in-process "fake console" that echoes the request id.

set -o pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

echo "==> spike dir: $HERE"
echo "==> compiling hole-punch spike…"
if ! cc -std=gnu11 -Wall "$HERE/punch.c" "$HERE/punch_test.c" -o "$HERE/punch_test" -lpthread; then
  echo "!! compile failed"; echo; read -n 1 -s -r -p "Press any key to close…"; exit 1
fi

echo
"$HERE/punch_test"

echo
echo "Note: this proves the wire format + send/select/match/confirm mechanics."
echo "The real PS5 punch (console answering, Tizen socket extension) is on-device."
echo
read -n 1 -s -r -p "Press any key to close this window…"
echo
