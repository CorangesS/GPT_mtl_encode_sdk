#!/bin/bash
# Run all tests: unit tests (from build), integration tests, scripts
# Run from repo root after building.

set -e
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
cd "$REPO_ROOT"

echo "=== MTL-Encode-SDK Test Suite ==="
echo ""

FAIL=0

run_unit() {
  local name="$1"
  local bin="$BUILD_DIR/tests/$name"
  if [ -x "$bin" ]; then
    echo "--- $name ---"
    if "$bin"; then
      echo ""
      return 0
    else
      FAIL=$((FAIL + 1))
      return 1
    fi
  else
    echo "--- $name --- SKIP (not built)"
    echo ""
    return 0
  fi
}

run_script() {
  local name="$1"
  local script="$REPO_ROOT/tests/scripts/$name"
  if [ -x "$script" ]; then
    echo "--- $name ---"
    if "$script"; then
      echo ""
      return 0
    else
      FAIL=$((FAIL + 1))
      return 1
    fi
  else
    echo "--- $name --- SKIP (not found)"
    echo ""
    return 0
  fi
}

run_unit sdp_test
run_unit sdp_to_session_test
run_unit encode_format_test
run_unit encode_reconfigure_test
run_unit ptp_behavior_test
run_script st2110_roundtrip_test.sh
# NMOS Registry 默认 192.168.1.101:8080；可通过环境变量 REGISTRY_URL 覆盖
export REGISTRY_URL="${REGISTRY_URL:-http://192.168.1.101:8080}"
run_script nmos_registration_test.sh

echo "=== Done: $FAIL failure(s) ==="
exit $FAIL
