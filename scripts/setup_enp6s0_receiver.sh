#!/bin/bash
# 本机接收端网卡 enp6s0 配置脚本
# 用法: sudo ./scripts/setup_enp6s0_receiver.sh

set -e
IFACE="${1:-enp6s0}"
IP="${2:-192.168.100.2}"
PREFIX=24

echo "=== 配置 $IFACE 为接收端: $IP/$PREFIX ==="

# 1. 不让 NetworkManager 管理该接口，避免被自动 down
if command -v nmcli &>/dev/null; then
  nmcli dev set "$IFACE" managed no 2>/dev/null || true
fi

# 2. 清空并拉起接口
ip link set "$IFACE" down 2>/dev/null || true
ip addr flush dev "$IFACE" 2>/dev/null || true
ip link set "$IFACE" up

# 3. 配置静态 IP（若已存在则跳过）
if ! ip -o addr show "$IFACE" | grep -q "$IP"; then
  ip addr add "$IP/$PREFIX" dev "$IFACE"
fi

# 4. 显示状态
echo ""
echo "当前状态:"
ip link show "$IFACE"
echo ""
ip -brief addr show "$IFACE"

if ip link show "$IFACE" | grep -q "NO-CARRIER"; then
  echo ""
  echo "注意: 接口显示 NO-CARRIER 表示物理链路未接通（网线未插或对端未接）。"
  echo "请检查: 1) 网线两端是否插好  2) 对端机器 enp4s0 是否 up 并配置 192.168.100.1/24"
fi
