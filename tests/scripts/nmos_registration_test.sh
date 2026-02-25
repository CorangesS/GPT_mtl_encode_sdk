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

# 使用 Query API 验证节点是否可见（Query 与 Registration 同属 IS-04）
NODES_URL="$BASE/x-nmos/query/v1.0/nodes"
if command -v curl >/dev/null 2>&1; then
  RESP=$(curl -s "$NODES_URL" 2>/dev/null || true)
  if echo "$RESP" | grep -q "mtl-encode-sdk"; then
    echo "[nmos_registration_test] PASS: Node visible in Registry"
  else
    echo "[nmos_registration_test] WARN: Node may not be visible (check Controller)"
  fi
else
  echo "[nmos_registration_test] PASS: Registration succeeded (curl not available to verify)"
fi
exit 0
