#!/usr/bin/env python3
"""
向 NMOS Registry（IS-04）注册一个示例 Node/Device/Receiver，
以便在 NMOS-JS 等客户端中发现该节点并进行连接管理。

使用前：
  1. 启动 NMOS Registry（如 nmos-cpp 的 registry）。
  2. 设置环境变量 REGISTRY_URL，例如：export REGISTRY_URL=http://127.0.0.1:8235
  3. 可选：pip install requests（若未安装，脚本会尝试使用标准库 urllib）。

本示例不修改 NMOS-JS，仅通过 Registry API 注册资源。
"""

import json
import os
import sys
import uuid

try:
    import requests
    def post(url, data):
        r = requests.post(url, json=data, timeout=5)
        r.raise_for_status()
        return r.json() if r.content else None
except ImportError:
    import urllib.request
    import urllib.error

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


def main():
    base = os.environ.get("REGISTRY_URL", "http://127.0.0.1:8235").rstrip("/")
    # IS-04 Registration API: resource type is in path
    nodes_url = f"{base}/x-nmos/registration/v1.2/resource/nodes"
    devices_url = f"{base}/x-nmos/registration/v1.2/resource/devices"
    receivers_url = f"{base}/x-nmos/registration/v1.2/resource/receivers"

    node_id = "mtl-encode-sdk-node-" + str(uuid.uuid4())[:8]
    device_id = "mtl-encode-sdk-device-" + str(uuid.uuid4())[:8]
    receiver_id = "mtl-encode-sdk-video-rx-" + str(uuid.uuid4())[:8]

    node = {
        "id": node_id,
        "version": "1634567890:0",
        "label": "MTL-Encode-SDK Node (example)",
        "href": f"http://127.0.0.1:9090/",
        "hostname": "mtl-encode-sdk-host",
        "api": {"versions": ["v1.2", "v1.1", "v1.0"]},
        "caps": {},
        "services": [],
        "clocks": [],
        "interfaces": [
            {
                "name": "eth0",
                "chassis_id": "00:00:00:00:00:00",
                "port_id": "1",
            }
        ],
    }

    device = {
        "id": device_id,
        "version": "1634567891:0",
        "label": "ST2110 RX + Encode Device",
        "node_id": node_id,
        "receivers": [receiver_id],
        "senders": [],
    }

    receiver = {
        "id": receiver_id,
        "version": "1634567892:0",
        "label": "Video Receiver (ST2110)",
        "description": "MTL SDK video RX; IS-05 activation drives St2110Endpoint",
        "device_id": device_id,
        "format": "video/raw",
        "caps": {},
        "subscription": {"sender_id": None, "active": False},
    }

    print("Registering Node...")
    try:
        post(nodes_url, node)
    except Exception as e:
        print("Failed to register node:", e, file=sys.stderr)
        sys.exit(1)
    print("Registering Device...")
    try:
        post(devices_url, device)
    except Exception as e:
        print("Failed to register device:", e, file=sys.stderr)
        sys.exit(1)
    print("Registering Receiver...")
    try:
        post(receivers_url, receiver)
    except Exception as e:
        print("Failed to register receiver:", e, file=sys.stderr)
        sys.exit(1)

    print("Done. Open NMOS-JS and point it to", base)
    print("You should see node:", node_id, "with receiver", receiver_id)
    print("To make connections actually drive MTL SDK, implement IS-05 in your app (see docs/ROUTING.md).")


if __name__ == "__main__":
    main()
