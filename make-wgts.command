#!/usr/bin/env bash
# chiaki-tizen — build both WGTs from the current app/.
#   dist/chiaki-tizen-generic.wgt   (never includes prefill.json — for sharing)
#   dist/chiaki-tizen-personal.wgt  (includes app/prefill.json, if present)
# Run ./build-hdr.command first to refresh the wasm, then this to package both.

set -o pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$HERE/app"; DIST="$HERE/dist"; mkdir -p "$DIST"

echo "==> generic (no prefill)…"
rm -f "$DIST/chiaki-tizen-generic.wgt"
( cd "$APP" && zip -qr "$DIST/chiaki-tizen-generic.wgt" . \
    -x "*.wgt" "prefill.json" "prefill.example.json" ".DS_Store" "*/.DS_Store" "author-signature.xml" "signature*.xml" )
echo "   -> dist/chiaki-tizen-generic.wgt"

if [ -f "$APP/prefill.json" ]; then
  echo "==> personal (bundling app/prefill.json)…"
  rm -f "$DIST/chiaki-tizen-personal.wgt"
  ( cd "$APP" && zip -qr "$DIST/chiaki-tizen-personal.wgt" . \
      -x "*.wgt" "prefill.example.json" ".DS_Store" "*/.DS_Store" "author-signature.xml" "signature*.xml" )
  echo "   -> dist/chiaki-tizen-personal.wgt"
else
  echo "==> personal skipped: no app/prefill.json"
  echo "    (copy app/prefill.example.json to app/prefill.json, fill it in, rerun)"
fi

echo
read -n 1 -s -r -p "Press any key to close…"
echo
