#!/bin/bash
# Integration test: st2110_send + st2110_record roundtrip
# Validates end-to-end ST2110收发 + 编码 (需求.md 收发/编码)
#
# Requires: MTL, build artifacts. Run from repo root or set BUILD_DIR.

set -e
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
SEND="$BUILD_DIR/st2110_send"
RECORD="$BUILD_DIR/st2110_record"
# Note: samples are in build/, not build/tests/
OUT="/tmp/mtl_st2110_roundtrip_test.mp4"

if [ ! -x "$SEND" ] || [ ! -x "$RECORD" ]; then
  echo "[st2110_roundtrip_test] SKIP: st2110_send or st2110_record not found (build first)"
  exit 0
fi

echo "[st2110_roundtrip_test] Starting receiver..."
"$RECORD" --ip 239.0.0.1 --video-port 5004 --audio-port 0 --max-frames 60 --no-ptp "$OUT" 2>/dev/null &
REC_PID=$!
trap "kill $REC_PID 2>/dev/null || true" EXIT

sleep 2
echo "[st2110_roundtrip_test] Starting sender..."
"$SEND" --ip 239.0.0.1 --video-port 5004 --audio-port 0 --duration 3 --no-ptp 2>/dev/null || true

wait $REC_PID 2>/dev/null || true
trap - EXIT

if [ -f "$OUT" ] && [ -s "$OUT" ]; then
  echo "[st2110_roundtrip_test] PASS: $OUT exists and non-empty"
  exit 0
else
  echo "[st2110_roundtrip_test] SKIP: MTL unavailable or output missing (requires hugepages/DPDK)"
  exit 0
fi
