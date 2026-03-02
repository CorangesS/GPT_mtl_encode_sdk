#!/usr/bin/env python3
"""
IS-05 Connection API server (single mode, v1.1).
On PATCH activate_immediate: parses transport_params or transport_file (SDP),
writes connection state to CONNECTION_STATE_FILE for the C++ receiver daemon.
"""

import json
import os
import re
import sys
import time
import threading
from datetime import datetime, timezone

try:
    from flask import Flask, request, jsonify, Response
except ImportError:
    print("Install Flask: pip install flask", file=sys.stderr)
    sys.exit(1)

app = Flask(__name__)


@app.after_request
def _cors_headers(response):
    """允许 Easy-NMOS 从其他主机打开 admin 时，浏览器能跨域访问本节点 IS-05。"""
    response.headers["Access-Control-Allow-Origin"] = "*"
    response.headers["Access-Control-Allow-Methods"] = "GET, PATCH, POST, OPTIONS"
    response.headers["Access-Control-Allow-Headers"] = "Content-Type"
    return response


@app.route("/", methods=["OPTIONS"])
@app.route("/<path:path>", methods=["OPTIONS"])
def _cors_preflight(path=""):
    return "", 204


# Receiver ID must match IS-04 registration (from .nmos_node.json or env)
CONFIG_FILE = os.environ.get("NMOS_NODE_CONFIG", ".nmos_node.json")
CONNECTION_STATE_FILE = os.environ.get("CONNECTION_STATE_FILE", "connection_state.json")
BIND = os.environ.get("IS05_BIND", "0.0.0.0")
PORT = int(os.environ.get("IS05_PORT", "9090"))

# In-memory state for staged/active (also persisted to file on activate)
_staged = {}
_active = {}
_sender_staged = {}
_sender_active = {}
_scheduled_receiver_activations = {}  # receiver_id -> activation_time_ns
_scheduled_sender_activations = {}    # sender_id -> activation_time_ns


def _load_receiver_id():
    rid = os.environ.get("IS05_RECEIVER_ID")
    if rid:
        return rid
    if os.path.isfile(CONFIG_FILE):
        try:
            with open(CONFIG_FILE, "r", encoding="utf-8") as f:
                data = json.load(f)
                return data.get("receiver_id")
        except Exception:
            pass
    return None


def _load_sender_id():
    sid = os.environ.get("IS05_SENDER_ID")
    if sid:
        return sid
    if os.path.isfile(CONFIG_FILE):
        try:
            with open(CONFIG_FILE, "r", encoding="utf-8") as f:
                data = json.load(f)
                return data.get("sender_id")
        except Exception:
            pass
    return None


def _load_node_id():
    nid = os.environ.get("NMOS_NODE_ID")
    if nid:
        return nid
    if os.path.isfile(CONFIG_FILE):
        try:
            with open(CONFIG_FILE, "r", encoding="utf-8") as f:
                data = json.load(f)
                return data.get("node_id")
        except Exception:
            pass
    return None


def _load_device_id():
    did = os.environ.get("NMOS_DEVICE_ID")
    if did:
        return did
    if os.path.isfile(CONFIG_FILE):
        try:
            with open(CONFIG_FILE, "r", encoding="utf-8") as f:
                data = json.load(f)
                return data.get("device_id")
        except Exception:
            pass
    return None


def _parse_sdp_media(sdp_text):
    """Extract first video and first audio media from SDP text (minimal parser)."""
    video = None
    audio = None
    connection_ip = None
    for line in sdp_text.splitlines():
        line = line.strip()
        if not line or line[1] != "=":
            continue
        k, v = line[0], line[2:]
        if k == "c":
            # c=IN IP4 239.0.0.1/32
            parts = v.split()
            if len(parts) >= 3:
                connection_ip = parts[2].split("/")[0]
        elif k == "m":
            parts = v.split()
            if len(parts) >= 4:
                media, port, proto, pt = parts[0], int(parts[1]), parts[2], int(parts[3])
                ep = {"udp_port": port, "payload_type": pt, "ip": connection_ip or "0.0.0.0"}
                if media == "video":
                    video = {"endpoint": ep, "width": 1920, "height": 1080, "fps": 59.94}
                elif media == "audio":
                    audio = {"endpoint": ep, "sample_rate": 48000, "channels": 2}
        elif k == "a" and video is not None and "fmtp:" in v:
            # a=fmtp:96 width=1920; height=1080; ...
            for token in v.split(";"):
                token = token.strip()
                if "width=" in token:
                    video["width"] = int(token.split("=")[1].strip())
                elif "height=" in token:
                    video["height"] = int(token.split("=")[1].strip())
    return video, audio


def _sdp_text_from_transport_file(tf):
    """从 PATCH body 的 transport_file 取出 SDP 字符串。支持 { data, type } 或纯字符串。"""
    if tf is None:
        return ""
    if isinstance(tf, str):
        return tf.strip()
    if isinstance(tf, dict):
        return (tf.get("data") or "").strip()
    return ""


def _transport_params_from_sdp(sdp_text):
    """从 SDP 解析出 IS-05 Receiver 单路 transport_params（用于 STAGED/ACTIVE 展示）。"""
    video, audio = _parse_sdp_media(sdp_text)
    leg = dict(_default_rtp_receiver_transport_params()[0])
    leg["rtp_enabled"] = True
    if video:
        leg["destination_port"] = video.get("udp_port", 5004)
        ip = video.get("ip", "0.0.0.0")
        if ip and ip != "0.0.0.0":
            leg["multicast_ip"] = ip if _is_multicast_ip(ip) else None
            if not leg["multicast_ip"]:
                leg["source_ip"] = ip
    return [leg]


def _is_multicast_ip(ip):
    """简单判断是否为 IPv4 组播地址（224.0.0.0–239.255.255.255）。"""
    try:
        parts = ip.split(".")
        if len(parts) != 4:
            return False
        first = int(parts[0])
        return 224 <= first <= 239
    except (ValueError, AttributeError):
        return False


def _parse_patch_body(body):
    """Return (video_dict, audio_dict) from transport_params or transport_file."""
    video = None
    audio = None
    tp = body.get("transport_params")
    if tp and isinstance(tp, list) and len(tp) > 0:
        p = tp[0]
        ip = p.get("multicast_ip") or p.get("destination_ip") or "0.0.0.0"
        port = int(p.get("destination_port", 5004))
        video = {
            "ip": ip,
            "udp_port": port,
            "payload_type": 96,
            "width": 1920,
            "height": 1080,
            "fps": 59.94,
        }
    sdp_data = _sdp_text_from_transport_file(body.get("transport_file"))
    if sdp_data:
        v, a = _parse_sdp_media(sdp_data)
        if v:
            video = {
                "ip": v["endpoint"]["ip"],
                "udp_port": v["endpoint"]["udp_port"],
                "payload_type": v["endpoint"].get("payload_type", 96),
                "width": v.get("width", 1920),
                "height": v.get("height", 1080),
                "fps": v.get("fps", 59.94),
            }
        if a:
            audio = {
                "ip": a["endpoint"]["ip"],
                "udp_port": a["endpoint"]["udp_port"],
                "payload_type": a["endpoint"].get("payload_type", 97),
                "sample_rate": a.get("sample_rate", 48000),
                "channels": a.get("channels", 2),
            }
    return video, audio


def _write_connection_state(receiver_id, master_enable, sender_id, video, audio):
    state = {
        "receiver_id": receiver_id,
        "master_enable": master_enable,
        "sender_id": sender_id,
        "video": video,
        "audio": audio,
    }
    path = os.path.abspath(CONNECTION_STATE_FILE)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(state, f, indent=2)
    return path


def _video_audio_from_staged(staged):
    """从 staged 中的 transport_file 解析出 video/audio，用于定时激活写 connection_state。"""
    tf = staged.get("transport_file")
    sdp_text = _sdp_text_from_transport_file(tf)
    if not sdp_text:
        return None, None
    v, a = _parse_sdp_media(sdp_text)
    video = None
    audio = None
    if v:
        video = {
            "ip": v["endpoint"]["ip"],
            "udp_port": v["endpoint"]["udp_port"],
            "payload_type": v["endpoint"].get("payload_type", 96),
            "width": v.get("width", 1920),
            "height": v.get("height", 1080),
            "fps": v.get("fps", 59.94),
        }
    if a:
        audio = {
            "ip": a["endpoint"]["ip"],
            "udp_port": a["endpoint"]["udp_port"],
            "payload_type": a["endpoint"].get("payload_type", 97),
            "sample_rate": a.get("sample_rate", 48000),
            "channels": a.get("channels", 2),
        }
    return video, audio


def _parse_requested_time_to_ns(requested_time):
    """将 IS-05 activation.requested_time 转为 epoch ns。

    支持：
    - TAI 风格 \"<seconds>:<nanoseconds>\"
    - ISO8601（例如 2025-03-03T10:00:00Z）
    - 其它/无法解析时返回 None（调用方可回退为立即激活）
    """
    if not requested_time:
        return None
    if not isinstance(requested_time, str):
        return None
    s = requested_time.strip()
    if not s or s.lower() in ("now", "immediate"):
        return None

    # TAI-like seconds:nanoseconds
    if ":" in s:
        parts = s.split(":", 1)
        if parts[0].isdigit():
            try:
                sec = int(parts[0])
                nano = int(parts[1]) if parts[1].isdigit() else 0
                return sec * 1_000_000_000 + nano
            except ValueError:
                pass

    # ISO8601 (treat as UTC)
    try:
        txt = s.replace("Z", "+00:00")
        dt = datetime.fromisoformat(txt)
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        sec = int(dt.timestamp())
        return sec * 1_000_000_000
    except Exception:
        return None


def _activation_scheduler():
    """后台轮询定时激活队列，实现 activate_scheduled。"""
    while True:
        now_ns = int(time.time() * 1_000_000_000)

        # Receiver 端定时激活：写 ACTIVE + connection_state，驱动收流
        for rid, ts in list(_scheduled_receiver_activations.items()):
            if now_ns >= ts:
                staged = _staged.get(rid, _default_staged(rid))
                activation_time = f"{now_ns}:0"
                tp = staged.get("transport_params")
                if not tp or (isinstance(tp, list) and len(tp) == 0):
                    tf = staged.get("transport_file")
                    if tf:
                        sdp_text = _sdp_text_from_transport_file(tf)
                        if sdp_text:
                            tp = _transport_params_from_sdp(sdp_text)
                if not tp or (isinstance(tp, list) and len(tp) == 0):
                    tp = _default_rtp_receiver_transport_params()

                _active[rid] = {
                    "sender_id": staged.get("sender_id"),
                    "master_enable": staged.get("master_enable", False),
                    "activation": {
                        "mode": "activate_immediate",
                        "requested_time": staged.get("activation", {}).get("requested_time"),
                        "activation_time": activation_time,
                    },
                    "transport_params": tp,
                    "transport_file": staged.get("transport_file"),
                }

                video, audio = _video_audio_from_staged(staged)
                try:
                    _write_connection_state(
                        rid, staged.get("master_enable", False), staged.get("sender_id"), video, audio
                    )
                except Exception as e:
                    print(f"Failed to write scheduled connection state for {rid}: {e}", file=sys.stderr)

                staged.setdefault("activation", {})
                staged["activation"]["mode"] = "activate_immediate"
                staged["activation"]["activation_time"] = activation_time
                _staged[rid] = staged
                del _scheduled_receiver_activations[rid]

        # Sender 端定时激活：仅更新 ACTIVE/STAGED，未来可扩展驱动真实发流
        for sid, ts in list(_scheduled_sender_activations.items()):
            if now_ns >= ts:
                staged = _sender_staged.get(sid, _default_sender_staged(sid))
                activation_time = f"{now_ns}:0"
                tp = staged.get("transport_params")
                if not tp or (isinstance(tp, list) and len(tp) == 0):
                    tp = _default_rtp_sender_transport_params()
                p = tp[0] if tp and isinstance(tp, list) and len(tp) > 0 else {}
                dest_ip = p.get("destination_ip") or p.get("multicast_ip") or "239.0.0.1"
                video_port = int(p.get("destination_port", 5004))
                source_ip = p.get("source_ip", "0.0.0.0")
                sdp = _make_sdp(sid, source_ip=source_ip, dest_ip=dest_ip, video_port=video_port)
                transport_file = {"data": sdp, "type": "application/sdp"}
                _sender_active[sid] = {
                    "receiver_id": staged.get("receiver_id"),
                    "master_enable": staged.get("master_enable", False),
                    "activation": {
                        "mode": "activate_immediate",
                        "requested_time": staged.get("activation", {}).get("requested_time"),
                        "activation_time": activation_time,
                    },
                    "transport_params": tp,
                    "transport_file": transport_file,
                }
                staged.setdefault("activation", {})
                staged["activation"]["mode"] = "activate_immediate"
                staged["activation"]["activation_time"] = activation_time
                _sender_staged[sid] = staged
                del _scheduled_sender_activations[sid]

        time.sleep(0.1)


# ---- Node API base (GET /)：Easy-NMOS 等 Controller 会先 GET 节点根，期望 IS-04 nodeapi-base 格式，否则报 "no api found" ----
# 规范示例 nodeapi-base-get-200：返回 ["self/", "sources/", "flows/", "devices/", "senders/", "receivers/"]
@app.route("/", methods=["GET"])
def node_base():
    """返回 Node API 根（IS-04 格式），便于 Controller 识别本节点并继续用 device.controls 发现 Connection API。"""
    return jsonify(["self/", "sources/", "flows/", "devices/", "senders/", "receivers/"])


# ---- Node API 桩路由：Controller 可能接着请求这些路径验证节点，任一 404 会导致 "no api found" ----
@app.route("/self/", methods=["GET"])
@app.route("/self", methods=["GET"])
def node_self():
    """IS-04 Node API self：返回本节点资源（最小字段），供 Controller 校验节点可用。"""
    nid = _load_node_id()
    if not nid:
        return jsonify({"error": "node_id not configured"}), 500
    return jsonify({
        "id": nid,
        "version": "0:0",
        "label": "MTL SDK Node",
        "description": "MTL encode SDK IS-05 node",
        "tags": {},
        "api": {"endpoints": [{"host": "0.0.0.0", "port": PORT, "protocol": "http"}]},
        "services": [],
        "clocks": [],
        "interfaces": [{"name": "eth0", "chassis_id": "00-00-00-00-00-00", "port_id": "00-00-00-00-00-01"}],
    })


@app.route("/devices/", methods=["GET"])
@app.route("/devices", methods=["GET"])
def node_devices():
    """IS-04 Node API devices：返回设备 ID 列表。"""
    did = _load_device_id()
    if not did:
        return jsonify([])
    return jsonify([f"{did}/"])


@app.route("/senders/", methods=["GET"])
@app.route("/senders", methods=["GET"])
def node_senders():
    """IS-04 Node API senders：返回 sender ID 列表。"""
    sid = _load_sender_id()
    if not sid:
        return jsonify([])
    return jsonify([f"{sid}/"])


@app.route("/receivers/", methods=["GET"])
@app.route("/receivers", methods=["GET"])
def node_receivers():
    """IS-04 Node API receivers：返回 receiver ID 列表。"""
    rid = _load_receiver_id()
    if not rid:
        return jsonify([])
    return jsonify([f"{rid}/"])


@app.route("/sources/", methods=["GET"])
@app.route("/sources", methods=["GET"])
def node_sources():
    """IS-04 Node API sources：本服务不暴露 source 详情，返回空列表即可。"""
    return jsonify([])


@app.route("/flows/", methods=["GET"])
@app.route("/flows", methods=["GET"])
def node_flows():
    """IS-04 Node API flows：本服务不暴露 flow 详情，返回空列表即可。"""
    return jsonify([])


# ---- IS-05 Connection API 基路径：Controller 会 GET 此 URL 探测节点是否支持 IS-05 ----
# Device.controls[].href = http://<node>:9090/x-nmos/connection/v1.1/ ，必须返回 200 才能显示 ACTIVE/TRANSPORT FILE
@app.route("/x-nmos/connection/v1.1/", methods=["GET"])
@app.route("/x-nmos/connection/v1.1", methods=["GET"])
def connection_api_base():
    """IS-05 v1.1 基路径，返回 bulk/ 与 single/，与规范 base-get-200 一致。"""
    return jsonify(["bulk/", "single/"])


# ---- IS-05 routes ----
BASE = "/x-nmos/connection/v1.1/single"


@app.route(f"{BASE}/", methods=["GET"])
@app.route(f"{BASE}", methods=["GET"])
def single_root():
    """IS-05 single 根：返回 senders/ 与 receivers/，便于 Controller 发现 STAGED/ACTIVE/TRANSPORTFILE。"""
    return jsonify(["senders/", "receivers/"])


@app.route(f"{BASE}/receivers", methods=["GET"])
def get_receivers():
    rid = _load_receiver_id()
    if not rid:
        return jsonify({"error": "receiver_id not configured"}), 500
    return jsonify([f"{rid}/"])


@app.route(f"{BASE}/receivers/<path:receiver_id_path>", methods=["GET"])
def get_receiver_sub(receiver_id_path):
    rid = receiver_id_path.rstrip("/")
    if rid != _load_receiver_id():
        return jsonify({"error": "receiver not found"}), 404
    return jsonify(["staged/", "active/", "constraints/", "transporttype/"])


@app.route(f"{BASE}/receivers/<path:receiver_id_path>/staged/", methods=["GET"])
@app.route(f"{BASE}/receivers/<path:receiver_id_path>/staged", methods=["GET"])
def get_staged(receiver_id_path):
    rid = receiver_id_path.rstrip("/")
    if rid != _load_receiver_id():
        return jsonify({"error": "receiver not found"}), 404
    return jsonify(_staged.get(rid, _default_staged(rid)))


@app.route(f"{BASE}/receivers/<path:receiver_id_path>/staged/", methods=["PATCH"])
@app.route(f"{BASE}/receivers/<path:receiver_id_path>/staged", methods=["PATCH"])
def patch_staged(receiver_id_path):
    rid = receiver_id_path.rstrip("/")
    if rid != _load_receiver_id():
        return jsonify({"error": "receiver not found"}), 404
    body = request.get_json(force=True, silent=True) or {}
    activation = body.get("activation") or {}
    mode = activation.get("mode")

    staged = _staged.get(rid, _default_staged(rid))
    staged["sender_id"] = body.get("sender_id") if "sender_id" in body else staged.get("sender_id")
    staged["master_enable"] = body.get("master_enable", True) if "master_enable" in body else staged.get("master_enable", True)
    staged["activation"] = activation
    if "transport_params" in body:
        staged["transport_params"] = body["transport_params"]
    if "transport_file" in body:
        staged["transport_file"] = body["transport_file"]
        # 若未提供 transport_params，从 SDP 推导便于 ACTIVE/STAGED 界面展示
        if "transport_params" not in body:
            sdp_text = _sdp_text_from_transport_file(body["transport_file"])
            if sdp_text:
                staged["transport_params"] = _transport_params_from_sdp(sdp_text)
    _staged[rid] = staged

    if mode == "activate_immediate":
        activation_time = f"{int(time.time() * 1e9)}:0"
        video, audio = _parse_patch_body(body)
        tp = staged.get("transport_params")
        if not tp or (isinstance(tp, list) and len(tp) == 0):
            tp = _default_rtp_receiver_transport_params()
        _active[rid] = {
            "sender_id": staged["sender_id"],
            "master_enable": staged["master_enable"],
            "activation": {"mode": "activate_immediate", "requested_time": None, "activation_time": activation_time},
            "transport_params": tp,
            "transport_file": staged.get("transport_file"),
        }
        staged["activation"]["mode"] = "activate_immediate"
        staged["activation"]["activation_time"] = activation_time
        try:
            path = _write_connection_state(
                rid, staged["master_enable"], staged.get("sender_id"), video, audio
            )
            print(f"Wrote connection state to {path}", flush=True)
        except Exception as e:
            return jsonify({"error": str(e)}), 500
    elif mode == "activate_scheduled":
        # 定时激活：仅记录时间点，由后台调度线程在到时后执行真正激活
        requested = activation.get("requested_time")
        ts_ns = _parse_requested_time_to_ns(requested)
        if ts_ns is None:
            # 无法解析 requested_time 时，回退为立即激活，避免 Controller 报错
            activation_time = f"{int(time.time() * 1e9)}:0"
            video, audio = _parse_patch_body(body)
            tp = staged.get("transport_params")
            if not tp or (isinstance(tp, list) and len(tp) == 0):
                tp = _default_rtp_receiver_transport_params()
            _active[rid] = {
                "sender_id": staged["sender_id"],
                "master_enable": staged["master_enable"],
                "activation": {"mode": "activate_immediate", "requested_time": requested, "activation_time": activation_time},
                "transport_params": tp,
                "transport_file": staged.get("transport_file"),
            }
            staged["activation"]["mode"] = "activate_immediate"
            staged["activation"]["activation_time"] = activation_time
            try:
                path = _write_connection_state(
                    rid, staged["master_enable"], staged.get("sender_id"), video, audio
                )
                print(f"Wrote connection state to {path}", flush=True)
            except Exception as e:
                return jsonify({"error": str(e)}), 500
        else:
            _scheduled_receiver_activations[rid] = ts_ns

    return jsonify(staged), 200


def _default_rtp_receiver_transport_params():
    """IS-05 RTP Receiver 单路默认参数，便于 Controller 识别为 RTP 而非 Unknown Type。"""
    return [
        {
            "interface_ip": "auto",
            "destination_port": "auto",
            "rtp_enabled": False,
            "source_ip": None,
            "multicast_ip": None,
        }
    ]


def _default_staged(rid):
    return {
        "sender_id": None,
        "master_enable": False,
        "activation": {"mode": None, "requested_time": None, "activation_time": None},
        "transport_params": _default_rtp_receiver_transport_params(),
        "transport_file": None,
    }


@app.route(f"{BASE}/receivers/<path:receiver_id_path>/active/", methods=["GET"])
@app.route(f"{BASE}/receivers/<path:receiver_id_path>/active", methods=["GET"])
def get_active(receiver_id_path):
    rid = receiver_id_path.rstrip("/")
    if rid != _load_receiver_id():
        return jsonify({"error": "receiver not found"}), 404
    return jsonify(_active.get(rid, _default_staged(rid)))


def _receiver_rtp_constraints():
    """IS-05 RTP Receiver 约束：单路，便于 Controller 在 CONNECT 时展示/校验参数。"""
    return [
        {
            "interface_ip": {"enum": ["auto"], "description": "Network interface for RTP receive"},
            "destination_port": {"minimum": 1, "maximum": 65535, "description": "RTP destination port"},
            "source_ip": {},
            "multicast_ip": {},
            "rtp_enabled": {"enum": [True, False]},
        }
    ]


@app.route(f"{BASE}/receivers/<path:receiver_id_path>/constraints/", methods=["GET"])
@app.route(f"{BASE}/receivers/<path:receiver_id_path>/constraints", methods=["GET"])
def get_constraints(receiver_id_path):
    rid = receiver_id_path.rstrip("/")
    if rid != _load_receiver_id():
        return jsonify({"error": "receiver not found"}), 404
    return jsonify(_receiver_rtp_constraints())


@app.route(f"{BASE}/receivers/<path:receiver_id_path>/transporttype/", methods=["GET"])
@app.route(f"{BASE}/receivers/<path:receiver_id_path>/transporttype", methods=["GET"])
def get_transporttype(receiver_id_path):
    rid = receiver_id_path.rstrip("/")
    if rid != _load_receiver_id():
        return jsonify({"error": "receiver not found"}), 404
    return jsonify("urn:x-nmos:transport:rtp.ucast")


# ---------- Sender 端 IS-05（STAGED / ACTIVE / TRANSPORTFILE）----------
def _default_rtp_sender_transport_params():
    """IS-05 RTP Sender 默认参数，便于 Controller 识别为 RTP 而非 Unknown Type。"""
    return [
        {
            "source_ip": "auto",
            "destination_ip": "auto",
            "source_port": "auto",
            "destination_port": "auto",
            "rtp_enabled": False,
        }
    ]


def _default_sender_staged(sid):
    return {
        "receiver_id": None,
        "master_enable": False,
        "activation": {"mode": None, "requested_time": None, "activation_time": None},
        "transport_params": _default_rtp_sender_transport_params(),
    }


def _make_sdp(sender_id, source_ip="0.0.0.0", dest_ip="239.0.0.1", video_port=5004):
    """生成 ST2110 视频 SDP，供 Controller 取 TRANSPORT FILE。"""
    return (
        "v=0\r\n"
        f"o=- 0 0 IN IP4 {source_ip}\r\n"
        "s=MTL-Encode-SDK Sender\r\n"
        "t=0 0\r\n"
        f"a=group:DUP PRIMARY\r\n"
        f"m=video {video_port} RTP/AVP 96\r\n"
        f"c=IN IP4 {dest_ip}/32\r\n"
        "a=rtpmap:96 raw/90000\r\n"
        "a=fmtp:96 width=1920; height=1080; exactframerate=60000/1001; sampling=YCbCr-4:2:2; depth=10; colorimetry=BT709;\r\n"
        "a=mid:PRIMARY\r\n"
    )


@app.route(f"{BASE}/senders", methods=["GET"])
def get_senders():
    sid = _load_sender_id()
    if not sid:
        return jsonify([])
    return jsonify([f"{sid}/"])


@app.route(f"{BASE}/senders/<path:sender_id_path>", methods=["GET"])
def get_sender_sub(sender_id_path):
    sid = sender_id_path.rstrip("/")
    if sid != _load_sender_id():
        return jsonify({"error": "sender not found"}), 404
    return jsonify(["staged/", "active/", "constraints/", "transportfile/", "transporttype/"])


@app.route(f"{BASE}/senders/<path:sender_id_path>/staged/", methods=["GET"])
@app.route(f"{BASE}/senders/<path:sender_id_path>/staged", methods=["GET"])
def get_sender_staged(sender_id_path):
    sid = sender_id_path.rstrip("/")
    if sid != _load_sender_id():
        return jsonify({"error": "sender not found"}), 404
    return jsonify(_sender_staged.get(sid, _default_sender_staged(sid)))


@app.route(f"{BASE}/senders/<path:sender_id_path>/staged/", methods=["PATCH"])
@app.route(f"{BASE}/senders/<path:sender_id_path>/staged", methods=["PATCH"])
def patch_sender_staged(sender_id_path):
    sid = sender_id_path.rstrip("/")
    if sid != _load_sender_id():
        return jsonify({"error": "sender not found"}), 404
    body = request.get_json(force=True, silent=True) or {}
    activation = body.get("activation") or {}
    mode = activation.get("mode")

    staged = _sender_staged.get(sid, _default_sender_staged(sid))
    # 仅当 body 中包含该字段时才更新，与 Receiver 端逻辑一致，避免误清空
    if "receiver_id" in body:
        staged["receiver_id"] = body.get("receiver_id")
    if "master_enable" in body:
        staged["master_enable"] = body.get("master_enable", True)
    staged["activation"] = activation
    if "transport_params" in body:
        staged["transport_params"] = body["transport_params"]
    _sender_staged[sid] = staged

    if mode == "activate_immediate":
        activation_time = f"{int(time.time() * 1e9)}:0"
        tp = staged.get("transport_params")
        if not tp or (isinstance(tp, list) and len(tp) == 0):
            tp = _default_rtp_sender_transport_params()
        # ACTIVE 中附带 transport_file（由 transport_params 生成），便于 Controller 展示
        p = tp[0] if tp and isinstance(tp, list) and len(tp) > 0 else {}
        dest_ip = p.get("destination_ip") or p.get("multicast_ip") or "239.0.0.1"
        video_port = int(p.get("destination_port", 5004))
        source_ip = p.get("source_ip", "0.0.0.0")
        sdp = _make_sdp(sid, source_ip=source_ip, dest_ip=dest_ip, video_port=video_port)
        transport_file = {"data": sdp, "type": "application/sdp"}
        _sender_active[sid] = {
            "receiver_id": staged["receiver_id"],
            "master_enable": staged["master_enable"],
            "activation": {"mode": "activate_immediate", "requested_time": None, "activation_time": activation_time},
            "transport_params": tp,
            "transport_file": transport_file,
        }
        staged["activation"]["mode"] = "activate_immediate"
        staged["activation"]["activation_time"] = activation_time
    elif mode == "activate_scheduled":
        requested = activation.get("requested_time")
        ts_ns = _parse_requested_time_to_ns(requested)
        if ts_ns is None:
            # 回退为立即激活，保持行为一致
            activation_time = f"{int(time.time() * 1e9)}:0"
            tp = staged.get("transport_params")
            if not tp or (isinstance(tp, list) and len(tp) == 0):
                tp = _default_rtp_sender_transport_params()
            p = tp[0] if tp and isinstance(tp, list) and len(tp) > 0 else {}
            dest_ip = p.get("destination_ip") or p.get("multicast_ip") or "239.0.0.1"
            video_port = int(p.get("destination_port", 5004))
            source_ip = p.get("source_ip", "0.0.0.0")
            sdp = _make_sdp(sid, source_ip=source_ip, dest_ip=dest_ip, video_port=video_port)
            transport_file = {"data": sdp, "type": "application/sdp"}
            _sender_active[sid] = {
                "receiver_id": staged["receiver_id"],
                "master_enable": staged["master_enable"],
                "activation": {"mode": "activate_immediate", "requested_time": requested, "activation_time": activation_time},
                "transport_params": tp,
                "transport_file": transport_file,
            }
            staged["activation"]["mode"] = "activate_immediate"
            staged["activation"]["activation_time"] = activation_time
        else:
            _scheduled_sender_activations[sid] = ts_ns

    return jsonify(staged), 200


@app.route(f"{BASE}/senders/<path:sender_id_path>/active/", methods=["GET"])
@app.route(f"{BASE}/senders/<path:sender_id_path>/active", methods=["GET"])
def get_sender_active(sender_id_path):
    sid = sender_id_path.rstrip("/")
    if sid != _load_sender_id():
        return jsonify({"error": "sender not found"}), 404
    return jsonify(_sender_active.get(sid, _default_sender_staged(sid)))


@app.route(f"{BASE}/senders/<path:sender_id_path>/transportfile/", methods=["GET"])
@app.route(f"{BASE}/senders/<path:sender_id_path>/transportfile", methods=["GET"])
def get_sender_transportfile(sender_id_path):
    sid = sender_id_path.rstrip("/")
    if sid != _load_sender_id():
        return jsonify({"error": "sender not found"}), 404
    # 从 staged/active 取 transport_params 生成 SDP，或使用默认组播
    staged = _sender_staged.get(sid, _default_sender_staged(sid))
    active = _sender_active.get(sid, staged)
    tp = (active or staged).get("transport_params") or []
    if tp and isinstance(tp, list) and len(tp) > 0:
        p = tp[0]
        dest_ip = p.get("destination_ip") or p.get("multicast_ip") or "239.0.0.1"
        video_port = int(p.get("destination_port", 5004))
        source_ip = p.get("source_ip", "0.0.0.0")
    else:
        dest_ip, video_port, source_ip = "239.0.0.1", 5004, "0.0.0.0"
    sdp = _make_sdp(sid, source_ip=source_ip, dest_ip=dest_ip, video_port=video_port)
    return Response(sdp, mimetype="application/sdp")


@app.route(f"{BASE}/senders/<path:sender_id_path>/constraints/", methods=["GET"])
@app.route(f"{BASE}/senders/<path:sender_id_path>/constraints", methods=["GET"])
def get_sender_constraints(sender_id_path):
    sid = sender_id_path.rstrip("/")
    if sid != _load_sender_id():
        return jsonify({"error": "sender not found"}), 404
    return jsonify([])


@app.route(f"{BASE}/senders/<path:sender_id_path>/transporttype/", methods=["GET"])
@app.route(f"{BASE}/senders/<path:sender_id_path>/transporttype", methods=["GET"])
def get_sender_transporttype(sender_id_path):
    sid = sender_id_path.rstrip("/")
    if sid != _load_sender_id():
        return jsonify({"error": "sender not found"}), 404
    return jsonify("urn:x-nmos:transport:rtp.ucast")


if __name__ == "__main__":
    rid = _load_receiver_id()
    sid = _load_sender_id()
    if not rid and not sid:
        print("Warning: IS05_RECEIVER_ID/IS05_SENDER_ID not set and no .nmos_node.json; run register_node_example.py --save-config", file=sys.stderr)
    if rid:
        print(f"IS-05 server receiver_id={rid}", flush=True)
    if sid:
        print(f"IS-05 server sender_id={sid}", flush=True)
    if rid:
        print(f"Connection state file: {os.path.abspath(CONNECTION_STATE_FILE)}", flush=True)
    # 启动定时激活调度线程（支持 activate_scheduled）
    scheduler = threading.Thread(target=_activation_scheduler, daemon=True)
    scheduler.start()
    app.run(host=BIND, port=PORT, threaded=True)
