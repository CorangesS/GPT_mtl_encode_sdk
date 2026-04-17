#!/usr/bin/env python3
"""
IS-05 Connection API server (receiver-only, single mode, v1.1).
用于接收端：处理 Receiver STAGED/ACTIVE/CONSTRAINTS，并在激活时写入 connection_state.json。
"""

import json
import os
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

_CORS_ALLOW_METHODS = "GET, PATCH, POST, OPTIONS, PUT, DELETE, HEAD"
_CORS_DEFAULT_HEADERS = (
    "Content-Type, Accept, Authorization, X-Requested-With, NMOS-API-KEY, Api-Key"
)


@app.before_request
def _cors_options_preflight():
    if request.method != "OPTIONS":
        return None
    if not request.path.startswith("/x-nmos"):
        return None
    return Response(status=204)


def _apply_cors_headers(response):
    origin = request.headers.get("Origin")
    if origin:
        response.headers["Access-Control-Allow-Origin"] = origin
        response.headers["Access-Control-Allow-Credentials"] = "true"
        prev = response.headers.get("Vary")
        response.headers["Vary"] = f"{prev}, Origin" if prev else "Origin"
    else:
        response.headers["Access-Control-Allow-Origin"] = "*"
    response.headers["Access-Control-Allow-Methods"] = _CORS_ALLOW_METHODS
    req_h = request.headers.get("Access-Control-Request-Headers")
    if req_h:
        response.headers["Access-Control-Allow-Headers"] = req_h
    else:
        response.headers["Access-Control-Allow-Headers"] = _CORS_DEFAULT_HEADERS
    response.headers["Access-Control-Max-Age"] = "86400"
    return response


@app.after_request
def _cors_headers(response):
    return _apply_cors_headers(response)


@app.route("/", methods=["OPTIONS"])
@app.route("/<path:path>", methods=["OPTIONS"])
def _cors_preflight(path=""):
    return "", 204


CONFIG_FILE = os.environ.get("NMOS_NODE_CONFIG", ".nmos_node.json")
CONNECTION_STATE_FILE = os.environ.get("CONNECTION_STATE_FILE", "connection_state.json")
BIND = os.environ.get("IS05_BIND", "0.0.0.0")
PORT = int(os.environ.get("IS05_PORT", "9090"))

_staged = {}
_active = {}
_scheduled_receiver_activations = {}  # receiver_id -> activation_time_ns


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


def _is_multicast_ip(ip):
    try:
        parts = ip.split(".")
        if len(parts) != 4:
            return False
        first = int(parts[0])
        return 224 <= first <= 239
    except (ValueError, AttributeError):
        return False


def _coerce_rtp_port(val, default=5004):
    if val is None:
        return default
    if isinstance(val, str):
        s = val.strip().lower()
        if s in ("auto", "automatic", ""):
            return default
    try:
        return int(val)
    except (TypeError, ValueError):
        return default


def _sdp_text_from_transport_file(tf):
    if tf is None:
        return ""
    if isinstance(tf, str):
        return tf.strip()
    if isinstance(tf, dict):
        return (tf.get("data") or "").strip()
    return ""


def _parse_sdp_media(sdp_text):
    video = None
    audio = None
    current_media = None
    session_connection_ip = None
    for line in sdp_text.splitlines():
        line = line.strip()
        if len(line) < 3 or line[1] != "=":
            continue
        k, v = line[0], line[2:]
        if k == "m":
            parts = v.split()
            if len(parts) >= 4:
                media, port, _, pt = parts[0], _coerce_rtp_port(parts[1]), parts[2], int(parts[3])
                ep = {"udp_port": port, "payload_type": pt, "ip": session_connection_ip or "0.0.0.0"}
                current_media = media
                if media == "video":
                    video = {"endpoint": ep, "width": 1920, "height": 1080, "fps": 59.94}
                elif media == "audio":
                    audio = {"endpoint": ep, "sample_rate": 48000, "channels": 2}
        elif k == "c":
            parts = v.split()
            if len(parts) >= 3:
                ip = parts[2].split("/")[0]
                # SDP 常见顺序是先 m= 再 c=，因此需要回填到当前 media。
                if current_media == "video" and video is not None:
                    video["endpoint"]["ip"] = ip
                elif current_media == "audio" and audio is not None:
                    audio["endpoint"]["ip"] = ip
                else:
                    session_connection_ip = ip
        elif k == "a" and video is not None and "fmtp:" in v:
            for token in v.split(";"):
                token = token.strip()
                if "width=" in token:
                    video["width"] = int(token.split("=")[1].strip())
                elif "height=" in token:
                    video["height"] = int(token.split("=")[1].strip())
    return video, audio


def _transport_params_from_sdp(sdp_text):
    video, _ = _parse_sdp_media(sdp_text)
    leg = dict(_default_rtp_receiver_transport_params()[0])
    leg["rtp_enabled"] = True
    if video:
        leg["destination_port"] = video.get("udp_port", 5004)
        ip = video.get("ip", "0.0.0.0")
        if ip and ip != "0.0.0.0":
            if _is_multicast_ip(ip):
                leg["multicast_ip"] = ip
                leg["source_ip"] = None
            else:
                leg["multicast_ip"] = None
                leg["source_ip"] = ip
    return [leg]


def _parse_patch_body(body):
    video = None
    audio = None
    tp = body.get("transport_params")
    if tp and isinstance(tp, list) and len(tp) > 0:
        p = tp[0]
        ip = p.get("multicast_ip") or p.get("destination_ip") or "0.0.0.0"
        if isinstance(ip, str) and ip.strip().lower() in ("auto", "automatic", ""):
            ip = "239.0.0.1"
        port = _coerce_rtp_port(p.get("destination_port"), 5004)
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
    if not requested_time or not isinstance(requested_time, str):
        return None
    s = requested_time.strip()
    if not s or s.lower() in ("now", "immediate"):
        return None

    if ":" in s:
        parts = s.split(":", 1)
        if parts[0].isdigit():
            try:
                sec = int(parts[0])
                nano = int(parts[1]) if parts[1].isdigit() else 0
                return sec * 1_000_000_000 + nano
            except ValueError:
                pass

    try:
        txt = s.replace("Z", "+00:00")
        dt = datetime.fromisoformat(txt)
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        sec = int(dt.timestamp())
        return sec * 1_000_000_000
    except Exception:
        return None


def _default_rtp_receiver_transport_params():
    return [
        {
            "interface_ip": "auto",
            "destination_port": "auto",
            "rtp_enabled": False,
            # Easy-NMOS 前端在 activate 阶段会对地址字段执行 string.match。
            # 若返回 null/None，会导致 "address is undefined" / "can't access property match"。
            "source_ip": "auto",
            "multicast_ip": "auto",
        }
    ]


def _normalize_receiver_transport_params(tp):
    """将 PATCH 里的 transport_params 规范化，避免出现 [{}] 导致前端识别失败。"""
    default_leg = _default_rtp_receiver_transport_params()[0]
    if not isinstance(tp, list) or len(tp) == 0:
        return [dict(default_leg)]
    out = []
    for leg in tp:
        merged = dict(default_leg)
        if isinstance(leg, dict):
            merged.update(leg)
        # 归一化：保证地址字段永远是字符串，避免前端 match 时报错
        if merged.get("source_ip") is None:
            merged["source_ip"] = "auto"
        if merged.get("multicast_ip") is None:
            merged["multicast_ip"] = "auto"
        out.append(merged)
    return out


def _default_staged(rid):
    return {
        "sender_id": None,
        "master_enable": False,
        "activation": {"mode": None, "requested_time": None, "activation_time": None},
        "transport_params": _default_rtp_receiver_transport_params(),
        "transport_file": None,
    }


def _receiver_rtp_constraints():
    return [
        {
            "interface_ip": {"enum": ["auto"], "description": "Network interface for RTP receive"},
            "destination_port": {"minimum": 1, "maximum": 65535, "description": "RTP destination port"},
            "source_ip": {},
            "multicast_ip": {},
            "rtp_enabled": {"enum": [True, False]},
        }
    ]


def _constraints_response(legs):
    return {
        "params": legs,
        "constraints": legs,
    }


def _with_params_alias(obj):
    if not isinstance(obj, dict):
        return obj
    out = dict(obj)
    if "params" not in out and "transport_params" in out:
        out["params"] = out.get("transport_params")
    return out


def _activation_scheduler():
    while True:
        now_ns = int(time.time() * 1_000_000_000)
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
        time.sleep(0.1)


@app.route("/", methods=["GET"])
def node_base():
    return jsonify(["self/", "sources/", "flows/", "devices/", "senders/", "receivers/"])


@app.route("/self/", methods=["GET"])
@app.route("/self", methods=["GET"])
def node_self():
    nid = _load_node_id()
    if not nid:
        return jsonify({"error": "node_id not configured"}), 500
    return jsonify({
        "id": nid,
        "version": "0:0",
        "label": "MTL SDK Node",
        "description": "MTL encode SDK IS-05 receiver node",
        "tags": {},
        "api": {"endpoints": [{"host": "0.0.0.0", "port": PORT, "protocol": "http"}]},
        "services": [],
        "clocks": [],
        "interfaces": [{"name": "eth0", "chassis_id": "00-00-00-00-00-00", "port_id": "00-00-00-00-00-01"}],
    })


@app.route("/devices/", methods=["GET"])
@app.route("/devices", methods=["GET"])
def node_devices():
    did = _load_device_id()
    if not did:
        return jsonify([])
    return jsonify([f"{did}/"])


@app.route("/senders/", methods=["GET"])
@app.route("/senders", methods=["GET"])
def node_senders():
    return jsonify([])


@app.route("/receivers/", methods=["GET"])
@app.route("/receivers", methods=["GET"])
def node_receivers():
    rid = _load_receiver_id()
    if not rid:
        return jsonify([])
    return jsonify([f"{rid}/"])


@app.route("/sources/", methods=["GET"])
@app.route("/sources", methods=["GET"])
def node_sources():
    return jsonify([])


@app.route("/flows/", methods=["GET"])
@app.route("/flows", methods=["GET"])
def node_flows():
    return jsonify([])


@app.route("/x-nmos/connection/v1.1/", methods=["GET"])
@app.route("/x-nmos/connection/v1.1", methods=["GET"])
def connection_api_base():
    return jsonify(["bulk/", "single/"])


BASE = "/x-nmos/connection/v1.1/single"


@app.route(f"{BASE}/", methods=["GET"])
@app.route(f"{BASE}", methods=["GET"])
def single_root():
    return jsonify(["senders/", "receivers/"])


@app.route(f"{BASE}/senders", methods=["GET"])
def get_senders():
    return jsonify([])


@app.route(f"{BASE}/receivers/", methods=["GET"])
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
    return jsonify(_with_params_alias(_staged.get(rid, _default_staged(rid))))


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
    if "sender_id" in body:
        staged["sender_id"] = body.get("sender_id")
    if "master_enable" in body:
        staged["master_enable"] = body.get("master_enable", True)
    else:
        # Controller 的某些实现：PATCH staged 时只带 sender_id/activation，
        # 不显式下发 master_enable。此时 receiver.staged.master_enable 保持默认 False
        # 会导致前端 activate 分支异常或 UI 状态不一致。
        # 若 staged 已经选择了 sender_id，则默认开启 master_enable。
        if staged.get("sender_id") is not None:
            staged["master_enable"] = True
    staged["activation"] = activation
    if "transport_params" in body:
        staged["transport_params"] = _normalize_receiver_transport_params(body["transport_params"])
    if "transport_file" in body:
        staged["transport_file"] = body["transport_file"]
        if "transport_params" not in body:
            sdp_text = _sdp_text_from_transport_file(body["transport_file"])
            if sdp_text:
                staged["transport_params"] = _normalize_receiver_transport_params(
                    _transport_params_from_sdp(sdp_text)
                )
    # Activate 可能只提交 activation.mode，不再带 transport_params。
    # 无论是否携带 transport_params，都在此处强制归一化一次，避免 staged 里仍残留 None/undefined。
    staged["transport_params"] = _normalize_receiver_transport_params(staged.get("transport_params"))
    _staged[rid] = staged

    if mode == "activate_immediate":
        activation_time = f"{int(time.time() * 1e9)}:0"
        video, audio = _parse_patch_body(body)
        tp = _normalize_receiver_transport_params(staged.get("transport_params"))
        _active[rid] = {
            "sender_id": staged.get("sender_id"),
            "master_enable": staged.get("master_enable", False),
            "activation": {"mode": "activate_immediate", "requested_time": None, "activation_time": activation_time},
            "transport_params": tp,
            "transport_file": staged.get("transport_file"),
        }
        staged["activation"]["mode"] = "activate_immediate"
        staged["activation"]["activation_time"] = activation_time
        try:
            path = _write_connection_state(
                rid, staged.get("master_enable", False), staged.get("sender_id"), video, audio
            )
            print(f"Wrote connection state to {path}", flush=True)
        except Exception as e:
            return jsonify({"error": str(e)}), 500
    elif mode == "activate_scheduled":
        requested = activation.get("requested_time")
        ts_ns = _parse_requested_time_to_ns(requested)
        if ts_ns is None:
            activation_time = f"{int(time.time() * 1e9)}:0"
            video, audio = _parse_patch_body(body)
            tp = _normalize_receiver_transport_params(staged.get("transport_params"))
            _active[rid] = {
                "sender_id": staged.get("sender_id"),
                "master_enable": staged.get("master_enable", False),
                "activation": {"mode": "activate_immediate", "requested_time": requested, "activation_time": activation_time},
                "transport_params": tp,
                "transport_file": staged.get("transport_file"),
            }
            staged["activation"]["mode"] = "activate_immediate"
            staged["activation"]["activation_time"] = activation_time
            try:
                path = _write_connection_state(
                    rid, staged.get("master_enable", False), staged.get("sender_id"), video, audio
                )
                print(f"Wrote connection state to {path}", flush=True)
            except Exception as e:
                return jsonify({"error": str(e)}), 500
        else:
            _scheduled_receiver_activations[rid] = ts_ns

    return jsonify(_with_params_alias(staged)), 200


@app.route(f"{BASE}/receivers/<path:receiver_id_path>/active/", methods=["GET"])
@app.route(f"{BASE}/receivers/<path:receiver_id_path>/active", methods=["GET"])
def get_active(receiver_id_path):
    rid = receiver_id_path.rstrip("/")
    if rid != _load_receiver_id():
        return jsonify({"error": "receiver not found"}), 404
    return jsonify(_with_params_alias(_active.get(rid, _default_staged(rid))))


@app.route(f"{BASE}/receivers/<path:receiver_id_path>/constraints/", methods=["GET"])
@app.route(f"{BASE}/receivers/<path:receiver_id_path>/constraints", methods=["GET"])
def get_constraints(receiver_id_path):
    rid = receiver_id_path.rstrip("/")
    if rid != _load_receiver_id():
        return jsonify({"error": "receiver not found"}), 404
    return jsonify(_constraints_response(_receiver_rtp_constraints()))


@app.route(f"{BASE}/receivers/<path:receiver_id_path>/transporttype/", methods=["GET"])
@app.route(f"{BASE}/receivers/<path:receiver_id_path>/transporttype", methods=["GET"])
def get_transporttype(receiver_id_path):
    rid = receiver_id_path.rstrip("/")
    if rid != _load_receiver_id():
        return jsonify({"error": "receiver not found"}), 404
    return jsonify("urn:x-nmos:transport:rtp")


if __name__ == "__main__":
    rid = _load_receiver_id()
    if not rid:
        print(
            "Warning: IS05_RECEIVER_ID not set and no receiver_id in config; run register_node_example.py --save-config",
            file=sys.stderr,
        )
    else:
        print(f"IS-05 receiver server receiver_id={rid}", flush=True)
        print(f"Connection state file: {os.path.abspath(CONNECTION_STATE_FILE)}", flush=True)
    scheduler = threading.Thread(target=_activation_scheduler, daemon=True)
    scheduler.start()
    app.run(host=BIND, port=PORT, threaded=True)
