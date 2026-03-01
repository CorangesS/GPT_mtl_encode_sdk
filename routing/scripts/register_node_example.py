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
        if not r.ok:
            try:
                err_body = r.text
            except Exception:
                err_body = ""
            raise RuntimeError(f"HTTP {r.status_code} for {url}: {err_body}")
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


def make_resources(node_href, node_hostname, mode="receiver"):
    """生成 IS-04 资源（符合 IS-04 v1.3 schema）。

    mode: "receiver" = 仅接收节点; "sender" = 仅发送节点; "both" = 同一节点同时带 Receiver 与 Sender。
    """
    node_id = str(uuid.uuid4())
    device_id = str(uuid.uuid4())
    receiver_id = str(uuid.uuid4()) if mode in ("receiver", "both") else None
    source_id = str(uuid.uuid4()) if mode in ("sender", "both") else None
    flow_id = str(uuid.uuid4()) if mode in ("sender", "both") else None
    sender_id = str(uuid.uuid4()) if mode in ("sender", "both") else None

    t = int(time.time())
    v = f"{t}:0"

    host, port, protocol = _parse_href(node_href)
    node_label = "MTL-Encode-SDK Node (Receiver)" if mode == "receiver" else "MTL-Encode-SDK Node (Sender)" if mode == "sender" else "MTL-Encode-SDK Node (RX+TX)"
    node = {
        "id": node_id,
        "version": v,
        "label": node_label,
        "description": "MTL-Encode-SDK node for ST2110 receive/send and encode",
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
        "label": "ST2110 RX + Encode Device" if mode == "receiver" else "ST2110 TX Device" if mode == "sender" else "ST2110 RX+TX Device",
        "description": "ST2110 receive/send and encode device",
        "tags": {},
        "node_id": node_id,
        "receivers": [receiver_id] if receiver_id else [],
        "senders": [sender_id] if sender_id else [],
        "type": "urn:x-nmos:device:generic",
        "controls": [],
    }
    receiver = None
    if receiver_id:
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

    source = None
    flow = None
    sender = None
    if source_id and flow_id and sender_id:
        source = {
            "id": source_id,
            "version": v,
            "label": "ST2110 Video Source",
            "description": "MTL SDK video source for ST2110 send",
            "tags": {},
            "device_id": device_id,
            "format": "urn:x-nmos:format:video",
            "grain_rate": {"numerator": 60000, "denominator": 1001},
            "caps": {},
            "parents": [],
            "clock_name": "clk0",
        }
        flow = {
            "id": flow_id,
            "version": v,
            "label": "Video Flow (ST2110)",
            "description": "ST2110 video flow from MTL TX",
            "tags": {},
            "device_id": device_id,
            "source_id": source_id,
            "parents": [],
            "grain_rate": {"numerator": 60000, "denominator": 1001},
            "format": "urn:x-nmos:format:video",
            "frame_width": 1920,
            "frame_height": 1080,
            "colorspace": "BT709",
            "media_type": "video/raw",
            "components": [
                {"name": "Y", "width": 1920, "height": 1080, "bit_depth": 10},
                {"name": "Cb", "width": 960, "height": 1080, "bit_depth": 10},
                {"name": "Cr", "width": 960, "height": 1080, "bit_depth": 10},
            ],
        }
        sender = {
            "id": sender_id,
            "version": v,
            "label": "Video Sender (ST2110)",
            "description": "MTL SDK video TX; IS-05 can configure destination",
            "tags": {},
            "device_id": device_id,
            "flow_id": flow_id,
            "transport": "urn:x-nmos:transport:rtp.ucast",
            "manifest_href": None,
            "interface_bindings": ["eth0"],
            "subscription": {"receiver_id": None, "active": False},
        }

    return node, device, receiver, source, flow, sender, node_id, device_id, receiver_id, source_id, flow_id, sender_id


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


def register_once(reg_base, node, device, receiver, source=None, flow=None, sender=None):
    """执行一次注册。

    根据 IS-04 规范，通过 Registration API 的通用 /resource 端点注册不同类型资源。
    注册顺序：Node → Device → Source（若有）→ Flow（若有）→ Receiver（若有）→ Sender（若有）。
    """
    resource_url = f"{reg_base}/resource"

    post(resource_url, {"type": "node", "data": node})
    post(resource_url, {"type": "device", "data": device})
    if source:
        post(resource_url, {"type": "source", "data": source})
    if flow:
        post(resource_url, {"type": "flow", "data": flow})
    if receiver:
        post(resource_url, {"type": "receiver", "data": receiver})
    if sender:
        post(resource_url, {"type": "sender", "data": sender})


def main():
    parser = argparse.ArgumentParser(
        description="Register MTL-Encode-SDK Node/Device/Receiver (and optional Sender) to NMOS Registry (IS-04)"
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
        "--mode",
        choices=["receiver", "sender", "both"],
        default="receiver",
        help="Register receiver-only, sender-only, or both (default: receiver). 'sender'/'both' add Source+Flow+Sender for 需求3 自研发送SDK端.",
    )
    parser.add_argument(
        "--href",
        default="http://127.0.0.1:9090/",
        help="Node href for IS-05; must be reachable from browser (e.g. http://<node-ip>:9090/)",
    )
    parser.add_argument(
        "--hostname",
        default="mtl-encode-sdk-host",
        help="Node hostname (default: mtl-encode-sdk-host)",
    )
    parser.add_argument(
        "--save-config",
        metavar="PATH",
        default=None,
        help="Save node_id, device_id, receiver_id, sender_id, href, hostname to JSON for IS-05 (e.g. .nmos_node.json)",
    )
    args = parser.parse_args()

    base = os.environ.get("REGISTRY_URL", "http://127.0.0.1").rstrip("/")
    reg_base = detect_registration_base(base)

    node, device, receiver, source, flow, sender, nid, did, rid, sid_src, fid, sid = make_resources(
        args.href, args.hostname, args.mode
    )

    # 首次注册
    print("Registering Node (mode=%s)..." % args.mode)
    print("Registry base:", base)
    print("Registration API:", reg_base)
    try:
        register_once(reg_base, node, device, receiver, source, flow, sender)
    except Exception as e:
        print("Failed to register:", e, file=sys.stderr)
        print("Tip: set REGISTRY_URL to your Easy-NMOS IP (e.g. export REGISTRY_URL=http://192.168.1.100)", file=sys.stderr)
        sys.exit(1)

    print("Done. Node:", nid, end="")
    if rid:
        print(" Receiver:", rid, end="")
    if sid:
        print(" Sender:", sid, end="")
    print()
    print("Controller:", base + "/admin")
    if args.save_config:
        config = {
            "node_id": nid,
            "device_id": did,
            "href": args.href,
            "hostname": args.hostname,
        }
        if rid:
            config["receiver_id"] = rid
        if sid:
            config["sender_id"] = sid
        with open(args.save_config, "w", encoding="utf-8") as f:
            json.dump(config, f, indent=2)
        print("Saved config to:", args.save_config)
    if rid:
        print("To make connections drive MTL SDK, run IS-05 server: python3 routing/is05_server/app.py")

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
        if receiver:
            receiver["version"] = v
        if source:
            source["version"] = v
        if flow:
            flow["version"] = v
        if sender:
            sender["version"] = v
        try:
            register_once(reg_base, node, device, receiver, source, flow, sender)
            print("[%s] Heartbeat OK" % time.strftime("%H:%M:%S"))
        except Exception as e:
            print("[%s] Heartbeat failed: %s" % (time.strftime("%H:%M:%S"), e), file=sys.stderr)


if __name__ == "__main__":
    main()
