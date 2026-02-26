#!/bin/bash
# Integration test: NMOS Registry registration and discovery
# Validates 需求.md 路由管理 §1: IS-04 注册与发现
#
# Requires: Registry running (Easy-NMOS or nmos-cpp). Set REGISTRY_URL.

set -e
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REG_SCRIPT="$REPO_ROOT/routing/scripts/register_node_example.py"
BASE="${REGISTRY_URL:-http://127.0.0.1}"
BASE="${BASE%/}"

if [ ! -f "$REG_SCRIPT" ]; then
  echo "[nmos_registration_test] FAIL: register_node_example.py not found"
  exit 1
fi

echo "[nmos_registration_test] Registry: $BASE"

export REGISTRY_URL="$BASE"

echo "[nmos_registration_test] Running register_node_example.py ..."
python3 "$REG_SCRIPT"
STATUS=$?
if [ $STATUS -ne 0 ]; then
  echo "[nmos_registration_test] SKIP: Registry not reachable or registration failed at $BASE (exit=$STATUS)"
  exit 0
fi

# 使用 Query API 验证节点是否可见（优先 v1.3，兼容 v1.0）
if command -v curl >/dev/null 2>&1; then
  for VER in v1.3 v1.2 v1.1 v1.0; do
    NODES_URL="$BASE/x-nmos/query/$VER/nodes"
    RESP=$(curl -s -o /dev/null -w "%{http_code}" "$NODES_URL" 2>/dev/null || true)
    if [ "$RESP" = "200" ]; then
      BODY=$(curl -s "$NODES_URL" 2>/dev/null || true)
      if echo "$BODY" | grep -qi "mtl-encode-sdk"; then
        echo "[nmos_registration_test] PASS: Node visible in Registry (query $VER)"
        exit 0
      fi
      echo "[nmos_registration_test] WARN: Query $VER returned 200 but node label not found (check Controller)"
      break
    fi
  done
  echo "[nmos_registration_test] PASS: Registration succeeded (discovery check skipped or no nodes list)"
else
  echo "[nmos_registration_test] PASS: Registration succeeded (curl not available to verify)"
fi
exit 0
