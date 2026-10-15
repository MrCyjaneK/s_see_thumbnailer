#!/usr/bin/env bash
# Build thumbnailer.xcframework for iOS device, iOS simulator, and macOS.
#
# Prereqs (run separately, once):
#   make simplybs
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
# shellcheck source=../apple.sh
source "$SCRIPT_DIR/../apple.sh"

apple_cmake_xcframework thumbnailer 17.0 15.6 include/thumbnailer.h -DBUILD_TEST_THUMBNAILER=OFF