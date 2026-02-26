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

    def get_json(url):
        r = requests.get(url, timeout=3)
        r.raise_for_status()
        return r.json() if r.content else None

    def check_exists(url):
        try:
            r = requests.get(url, timeout=3)
            # 2xx/3xx 认为该 URL 有效（存在该 API）
            return 200 <= r.status_code < 400
        except Exception:
            return False

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

    def get_json(url):
        with urllib.request.urlopen(url, timeout=3) as resp:
            body = resp.read().decode("utf-8")
            return json.loads(body) if body else None

    def check_exists(url):
        try:
            with urllib.request.urlopen(url, timeout=3):
                return True
        except urllib.error.HTTPError as e:
            # 4xx 表示该版本不存在，返回 False
            if 400 <= e.code < 500:
                return False
            return False
        except Exception:
            return False


def _parse_href(href):
    """从 href 解析 host/port/protocol，用于 api.endpoints（IS-04 v1.3 必需）。"""
    try:
        from urllib.parse import urlparse
        u = urlparse(href)
        host = u.hostname or "127.0.0.1"
        port = u.port if u.port is not None else (443 if u.scheme == "https" else 9090)
        protocol = u.scheme or "http"
        return host, port, protocol
    except Exception:
        return "127.0.0.1", 9090, "http"


def make_resources(node_href, node_hostname):
    """生成 IS-04 Node/Device/Receiver 资源（符合 IS-04 v1.3 schema）。"""
    node_id = str(uuid.uuid4())
    device_id = str(uuid.uuid4())
    receiver_id = str(uuid.uuid4())
    t = int(time.time())
    v = f"{t}:0"

    host, port, protocol = _parse_href(node_href)
    node = {
        "id": node_id,
        "version": v,
        "label": "MTL-Encode-SDK Node (Receiver)",
        "description": "MTL-Encode-SDK node for ST2110 receive and encode",
        "tags": {},
        "href": node_href,
        "hostname": node_hostname,
        "api": {
            "versions": ["v1.3", "v1.2", "v1.1", "v1.0"],
            "endpoints": [{"host": host, "port": port, "protocol": protocol}],
        },
        "caps": {},
        "services": [],
        "clocks": [],
        "interfaces": [
            {"name": "eth0", "chassis_id": "00-00-00-00-00-00", "port_id": "00-00-00-00-00-01"}
        ],
    }
    device = {
        "id": device_id,
        "version": v,
        "label": "ST2110 RX + Encode Device",
        "description": "ST2110 receive and encode device",
        "tags": {},
        "node_id": node_id,
        "receivers": [receiver_id],
        "senders": [],
        "type": "urn:x-nmos:device:generic",
        "controls": [],
    }
    # Receiver 必须符合 receiver_video + receiver_core：transport、interface_bindings 必需
    receiver = {
        "id": receiver_id,
        "version": v,
        "label": "Video Receiver (ST2110)",
        "description": "MTL SDK video RX; IS-05 activation drives St2110Endpoint",
        "tags": {},
        "device_id": device_id,
        "format": "urn:x-nmos:format:video",
        "caps": {"media_types": ["video/raw"]},
        "transport": "urn:x-nmos:transport:rtp.ucast",
        "interface_bindings": ["eth0"],
        "subscription": {"sender_id": None, "active": False},
    }
    return node, device, receiver, node_id, device_id, receiver_id


def detect_registration_base(base):
    """
    探测 Registry 支持的 registration 版本，返回完整 registration base URL。

    优先级：v1.3 > v1.2 > v1.1 > v1.0；若探测失败则回退到 v1.2。
    """
    root = f"{base}/x-nmos/registration"
    preferred = ["v1.3", "v1.2", "v1.1", "v1.0"]

    # 1) 优先尝试规范定义的版本列表：GET /x-nmos/registration
    try:
        versions = get_json(root)
    except Exception:
        versions = None

    if isinstance(versions, list) and versions:
        normalized = [str(v).rstrip("/") for v in versions]
        for v in preferred:
            if v in normalized:
                return f"{root}/{v}"
        return f"{root}/{normalized[0]}"

    # 2) 若上一步失败（某些实现不返回版本列表），依次探测各版本根 URL 是否存在
    for v in preferred:
        candidate = f"{root}/{v}"
        if check_exists(candidate):
            return candidate

    # 3) 仍失败则回退到原脚本的默认 v1.2
    return f"{root}/v1.2"


def register_once(reg_base, node, device, receiver):
    """执行一次注册。

    根据 IS-04 规范，通过 Registration API 的通用 /resource 端点注册不同类型资源，
    而不是访问 /resource/nodes 这类路径。
    """
    resource_url = f"{reg_base}/resource"

    # 按顺序注册 Node、Device、Receiver，避免引用未注册的资源。
    post(resource_url, {"type": "node", "data": node})
    post(resource_url, {"type": "device", "data": device})
    post(resource_url, {"type": "receiver", "data": receiver})


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
    reg_base = detect_registration_base(base)

    node, device, receiver, nid, did, rid = make_resources(args.href, args.hostname)

    # 首次注册
    print("Registering Node...")
    print("Registry base:", base)
    print("Registration API:", reg_base)
    try:
        register_once(reg_base, node, device, receiver)
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
            register_once(reg_base, node, device, receiver)
            print("[%s] Heartbeat OK" % time.strftime("%H:%M:%S"))
        except Exception as e:
            print("[%s] Heartbeat failed: %s" % (time.strftime("%H:%M:%S"), e), file=sys.stderr)


if __name__ == "__main__":
    main()
