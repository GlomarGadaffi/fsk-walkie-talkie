#!/usr/bin/env bash
# Fetch (for the ESP-IDF build) sh123's Codec2 port (LGPL-2.1) into components/codec2/upstream.
# It is plain C with no Arduino dependency; only its packaging is Arduino.
# Not vendored: LGPL code stays out of this MIT repo's tree (gitignored).
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=https://github.com/sh123/esp32_codec2_arduino.git
TAG=1.0.7
DEST=components/codec2/upstream
rm -rf "$DEST"
git clone -q --depth 1 --branch "$TAG" "$REPO" "$DEST"
rm -rf "$DEST/.git"
echo "$DEST ready ($(grep ^version "$DEST/library.properties"))"
