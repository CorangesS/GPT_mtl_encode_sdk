#!/usr/bin/env python3
"""
向 NMOS Registry（IS-04）注册自研 Node/Device/Receiver，
以便在 NMOS Controller（Easy-NMOS /admin）或 NMOS-JS 中发现该节点并进行连接管理。

支持：
  - Easy-NMOS（默认 http://127.0.0.1，端口 80）
  - nmos-cpp 等（端口常为 8235，通过 REGISTRY_URL 指定）
  - 心跳模式（--heartbeat）保持注册有效

使用前：
  1. 启动 NMOS Registry（如 Easy-NMOS 或 nmos-cpp registry）。
  2. 设置环境变量 REGISTRY_URL，例如：
       export REGISTRY_URL=http://192.168.6.101        # Easy-NMOS
       export REGISTRY_URL=http://127.0.0.1:8235      # nmos-cpp 默认
  3. 可选：pip install requests
"""

import argparse
import json
import os
import signal
import sys
import time
import uuid

try:
    import requests
    def post(url, data):
        r = requests.post(url, json=data, timeout=5)
        r.raise_for_status()
        return r.json() if r.content else None
except ImportError:
    import urllib.request

    def post(url, data):
        req = urllib.request.Request(
            url,
            data=json.dumps(data).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=5) as resp:
            body = resp.read().decode("utf-8")
            return json.loads(body) if body else None


def make_resources(node_href, node_hostname):
    """生成 IS-04 Node/Device/Receiver 资源。"""
    node_id = "mtl-encode-sdk-node-" + str(uuid.uuid4())[:8]
    device_id = "mtl-encode-sdk-device-" + str(uuid.uuid4())[:8]
    receiver_id = "mtl-encode-sdk-video-rx-" + str(uuid.uuid4())[:8]
    t = int(time.time())
    v = f"{t}:0"

    node = {
        "id": node_id,
        "version": v,
        "label": "MTL-Encode-SDK Node (Receiver)",
        "href": node_href,
        "hostname": node_hostname,
        "api": {"versions": ["v1.2", "v1.1", "v1.0"]},
        "caps": {},
        "services": [],
        "clocks": [],
        "interfaces": [{"name": "eth0", "chassis_id": "00:00:00:00:00:00", "port_id": "1"}],
    }
    device = {
        "id": device_id,
        "version": v,
        "label": "ST2110 RX + Encode Device",
        "node_id": node_id,
        "receivers": [receiver_id],
        "senders": [],
    }
    receiver = {
        "id": receiver_id,
        "version": v,
        "label": "Video Receiver (ST2110)",
        "description": "MTL SDK video RX; IS-05 activation drives St2110Endpoint",
        "device_id": device_id,
        "format": "video/raw",
        "caps": {},
        "subscription": {"sender_id": None, "active": False},
    }
    return node, device, receiver, node_id, device_id, receiver_id


def register_once(base, node, device, receiver):
    """执行一次注册。"""
    nodes_url = f"{base}/x-nmos/registration/v1.2/resource/nodes"
    devices_url = f"{base}/x-nmos/registration/v1.2/resource/devices"
    receivers_url = f"{base}/x-nmos/registration/v1.2/resource/receivers"
    post(nodes_url, node)
    post(devices_url, device)
    post(receivers_url, receiver)


def main():
    parser = argparse.ArgumentParser(
        description="Register MTL-Encode-SDK Node/Device/Receiver to NMOS Registry (IS-04)"
    )
    parser.add_argument(
        "--heartbeat",
        action="store_true",
        help="Keep registration alive by re-registering periodically",
    )
    parser.add_argument(
        "--interval",
        type=int,
        default=10,
        help="Heartbeat interval in seconds (default: 10)",
    )
    parser.add_argument(
        "--href",
        default="http://127.0.0.1:9090/",
        help="Node href for IS-05 (default: http://127.0.0.1:9090/)",
    )
    parser.add_argument(
        "--hostname",
        default="mtl-encode-sdk-host",
        help="Node hostname (default: mtl-encode-sdk-host)",
    )
    args = parser.parse_args()

    base = os.environ.get("REGISTRY_URL", "http://127.0.0.1").rstrip("/")

    node, device, receiver, nid, did, rid = make_resources(args.href, args.hostname)

    # 首次注册
    print("Registering Node...")
    try:
        register_once(base, node, device, receiver)
    except Exception as e:
        print("Failed to register:", e, file=sys.stderr)
        sys.exit(1)

    print("Done. Node:", nid, "Receiver:", rid)
    print("Controller:", base + "/admin")
    print("To make connections drive MTL SDK, implement IS-05 (see docs/ROUTING.md).")

    if not args.heartbeat:
        return

    # 心跳：定期更新 version 并重新注册
    running = [True]

    def stop(sig, frame):
        running[0] = False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    print("Heartbeat enabled, interval=%ds. Ctrl+C to stop." % args.interval)
    while running[0]:
        time.sleep(args.interval)
        if not running[0]:
            break
        t = int(time.time())
        v = f"{t}:0"
        node["version"] = v
        device["version"] = v
        receiver["version"] = v
        try:
            register_once(base, node, device, receiver)
            print("[%s] Heartbeat OK" % time.strftime("%H:%M:%S"))
        except Exception as e:
            print("[%s] Heartbeat failed: %s" % (time.strftime("%H:%M:%S"), e), file=sys.stderr)


if __name__ == "__main__":
    main()
