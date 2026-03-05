#!/bin/bash
# 一键运行：先向 Registry 注册并写入 .nmos_node.json，再启动 IS-05 服务（可选后台心跳）。
#
# 用法:
#   export REGISTRY_URL=http://<Easy-NMOS-IP>   # 必选
#   export NODE_HREF=http://<本机IP>:9090/      # 可选，默认 http://127.0.0.1:9090/
#   ./run_routing_node.sh [选项]
#
# 选项:
#   --heartbeat           后台以心跳模式运行注册，前台运行 IS-05；Ctrl+C 会同时停止
#   --mode receiver|sender|both   注册模式（默认 both）
#   --href URL            本节点 IS-05 地址（覆盖 NODE_HREF）
#   --interval N          心跳间隔秒数（默认 10）
#   --config PATH         配置文件路径（默认项目根目录 .nmos_node.json）
#   --help                显示此帮助
#
# 示例:
#   ./run_routing_node.sh
#   ./run_routing_node.sh --heartbeat --mode both
#   NODE_HREF=http://192.168.1.110:9090/ ./run_routing_node.sh --heartbeat

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

REG_PID=""
cleanup() {
  if [ -n "$REG_PID" ]; then
    kill "$REG_PID" 2>/dev/null || true
    wait "$REG_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

# 默认值
MODE="both"
NODE_HREF="${NODE_HREF:-http://127.0.0.1:9090/}"
INTERVAL=10
CONFIG_PATH=".nmos_node.json"
HEARTBEAT=""

while [ $# -gt 0 ]; do
  case "$1" in
    --heartbeat)   HEARTBEAT=1; shift ;;
    --mode)        MODE="$2"; shift 2 ;;
    --href)        NODE_HREF="$2"; shift 2 ;;
    --interval)    INTERVAL="$2"; shift 2 ;;
    --config)      CONFIG_PATH="$2"; shift 2 ;;
    --help|-h)
      head -n 30 "$SCRIPT_DIR/run_routing_node.sh" | grep -E '^#( |$)'
      exit 0
      ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

if [ -z "$REGISTRY_URL" ]; then
  echo "Error: REGISTRY_URL not set. Example: export REGISTRY_URL=http://192.168.1.200" >&2
  exit 1
fi

echo "Step 1: Register node (mode=$MODE, href=$NODE_HREF, config=$CONFIG_PATH)..."
python3 "$SCRIPT_DIR/register_node_example.py" \
  --mode "$MODE" \
  --href "$NODE_HREF" \
  --save-config "$CONFIG_PATH"

if [ -n "$HEARTBEAT" ]; then
  echo "Step 2: Starting registration heartbeat in background (interval=${INTERVAL}s)..."
  python3 "$SCRIPT_DIR/register_node_example.py" \
    --mode "$MODE" \
    --href "$NODE_HREF" \
    --save-config "$CONFIG_PATH" \
    --heartbeat --interval "$INTERVAL" &
  REG_PID=$!
  sleep 2
fi

echo "Step $([ -n "$HEARTBEAT" ] && echo 3 || echo 2): Starting IS-05 server (config=$CONFIG_PATH)..."
if [[ "$CONFIG_PATH" == /* ]]; then
  export NMOS_NODE_CONFIG="$CONFIG_PATH"
else
  export NMOS_NODE_CONFIG="$REPO_ROOT/$CONFIG_PATH"
fi
export CONNECTION_STATE_FILE="${CONNECTION_STATE_FILE:-$REPO_ROOT/connection_state.json}"
python3 "$REPO_ROOT/routing/is05_server/app.py"
