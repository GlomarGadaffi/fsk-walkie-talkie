#!/usr/bin/env bash
# Fetch LilyGo's Arduino_DriveBus library (GPL-3.0) into lib/ via sparse checkout.
# It is not on the PlatformIO registry; it only ships inside the board repo.
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=https://github.com/Xinyuan-LilyGO/T3-S3-MVSRBoard.git
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
git clone -q --depth 1 --filter=blob:none --sparse "$REPO" "$TMP"
git -C "$TMP" sparse-checkout set libraries/Arduino_DriveBus
rm -rf lib/Arduino_DriveBus
mkdir -p lib
cp -r "$TMP/libraries/Arduino_DriveBus" lib/
echo "lib/Arduino_DriveBus ready ($(cat lib/Arduino_DriveBus/library.properties | grep ^version))"
