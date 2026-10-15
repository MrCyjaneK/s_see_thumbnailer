#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
# shellcheck source=../android-ndk.sh
source "$SCRIPT_DIR/../android-ndk.sh"

android_cmake_module thumbnailer -DBUILD_TEST_THUMBNAILER=OFF
