#!/usr/bin/env bash
# Build libthumbnailer.so for Linux (amd64, arm64).
#
# Prereqs: make simplybs-x86_64-linux-gnu simplybs-aarch64-linux-gnu
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
# shellcheck source=../linux-gnu.sh
source "$SCRIPT_DIR/../linux-gnu.sh"

LIB_NAME=thumbnailer
SO_NAME=lib${LIB_NAME}.so
OUT_DIR=./dist/linux
WORK=./dist/_work-linux

require() { command -v "$1" &>/dev/null || { echo "[!] '$1' not found"; exit 1; }; }
require cmake

cmake_slice() {
  local arch="$1" sbs_triplet="$2" cc_prefix="$3"
  local sbs_prefix="$SBS_ROOT/$sbs_triplet"

  local cc cxx
  cc="$(linux_cc "$cc_prefix")"
  cxx="$(linux_cxx "$cc_prefix")"
  if ! command -v "$cc" &>/dev/null; then
    echo "[!] missing $cc" >&2
    exit 1
  fi

  local slice="$WORK/$arch"
  local build_dir="build-linux-$arch"
  rm -rf "$build_dir" "$slice"
  mkdir -p "$slice" "$OUT_DIR/$arch"

  echo "[+] CMake $arch (sbs=$sbs_triplet)..."
  cmake -S . -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$cc" \
    -DCMAKE_CXX_COMPILER="$cxx" \
    -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -g" \
    -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -g" \
    -DSBS_PREFIX="$sbs_prefix" \
    -DBUILD_TEST_THUMBNAILER=ON

  cmake --build "$build_dir" -j"$(linux_jobs)"

  cp -f "$build_dir/$SO_NAME" "$OUT_DIR/$arch/"
  echo "[+]  $OUT_DIR/$arch/$SO_NAME"
}

rm -rf "$WORK" "$OUT_DIR"
mkdir -p "$WORK" "$OUT_DIR"

arch="" sbs_triplet="" goarch="" cc_prefix=""
host_goarch="$(go env GOARCH)"
while read -r arch sbs_triplet goarch cc_prefix; do
  [[ -n "$arch" ]] || continue
  linux_want_sbs_slice "$arch" "$sbs_triplet" "$goarch" "$host_goarch" || continue
  cmake_slice "$arch" "$sbs_triplet" "$cc_prefix"
done < <(linux_arch_rows)

host_arch=""
case "$(uname -m)" in
  x86_64) host_arch=amd64 ;;
  aarch64|arm64) host_arch=arm64 ;;
esac
if [[ -n "$host_arch" && -f "$OUT_DIR/$host_arch/$SO_NAME" ]]; then
  mkdir -p dist
  cp -f "$OUT_DIR/$host_arch/$SO_NAME" dist/
  if [[ -f "build-linux-$host_arch/test_thumbnailer" ]]; then
    cp -f "build-linux-$host_arch/test_thumbnailer" dist/
  fi
  echo "[+]  dist/$SO_NAME (host)"
fi

rm -rf "$WORK" build-linux-*

echo ""
echo "[+] Done: $OUT_DIR"
find "$OUT_DIR" -name "*.so" | sort | sed 's/^/     /'
echo ""
