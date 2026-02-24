#!/bin/bash
# 路由管理端到端运行脚本
# 在后台启动 NMOS 注册（心跳模式），然后启动 st2110_record 进行收流编码。
# 需先部署 Easy-NMOS 或 nmos-cpp Registry，并设置 REGISTRY_URL。
#
# 用法:
#   export REGISTRY_URL=http://<Easy-NMOS-IP>   # 例如 http://192.168.6.101
#   ./run_with_nmos.sh [st2110_record 的参数...] [output.mp4]
#
# 示例:
#   ./run_with_nmos.sh --ip 239.0.0.1 --video-port 5004 --audio-port 0 --max-frames 600 recv.mp4

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
RECORD_BIN="$BUILD_DIR/st2110_record"

if [ -z "$REGISTRY_URL" ]; then
  echo "Error: REGISTRY_URL not set. Example: export REGISTRY_URL=http://192.168.6.101" >&2
  exit 1
fi

if [ ! -x "$RECORD_BIN" ]; then
  echo "Error: st2110_record not found at $RECORD_BIN. Build the project first." >&2
  exit 1
fi

# 后台启动注册脚本（心跳模式）
echo "Starting NMOS registration (heartbeat) in background..."
python3 "$SCRIPT_DIR/register_node_example.py" --heartbeat --interval 10 &
REG_PID=$!
trap "kill $REG_PID 2>/dev/null || true" EXIT

sleep 2
echo "Registry: $REGISTRY_URL | Controller: $REGISTRY_URL/admin"
echo "Running st2110_record..."
"$RECORD_BIN" "$@"
