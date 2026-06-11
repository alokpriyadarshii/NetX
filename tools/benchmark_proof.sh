#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-benchmark}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/build/benchmarks}"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -D CMAKE_BUILD_TYPE=Release \
  -D NETX_BUILD_BENCHMARKS=ON \
  -D USE_CLANG_TIDY=OFF

cmake --build "$BUILD_DIR" --target benchmark_ring_buffer

mkdir -p "$OUT_DIR"
"$BUILD_DIR/benchmark_ring_buffer" > "$OUT_DIR/netx_benchmark_proof.json"
printf '%s\n' "$OUT_DIR/netx_benchmark_proof.json"
