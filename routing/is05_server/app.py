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

try:
    from flask import Flask, request, jsonify
except ImportError:
    print("Install Flask: pip install flask", file=sys.stderr)
    sys.exit(1)

app = Flask(__name__)

# Receiver ID must match IS-04 registration (from .nmos_node.json or env)
CONFIG_FILE = os.environ.get("NMOS_NODE_CONFIG", ".nmos_node.json")
CONNECTION_STATE_FILE = os.environ.get("CONNECTION_STATE_FILE", "connection_state.json")
BIND = os.environ.get("IS05_BIND", "0.0.0.0")
PORT = int(os.environ.get("IS05_PORT", "9090"))

# In-memory state for staged/active (also persisted to file on activate)
_staged = {}
_active = {}


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
    tf = body.get("transport_file")
    if tf and isinstance(tf, dict):
        sdp_data = tf.get("data") or ""
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


# ---- IS-05 routes ----
BASE = "/x-nmos/connection/v1.1/single"


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
    return jsonify(["staged", "active", "constraints", "transporttype"])


@app.route(f"{BASE}/receivers/<path:receiver_id_path>/staged", methods=["GET"])
def get_staged(receiver_id_path):
    rid = receiver_id_path.rstrip("/")
    if rid != _load_receiver_id():
        return jsonify({"error": "receiver not found"}), 404
    return jsonify(_staged.get(rid, _default_staged(rid)))


@app.route(f"{BASE}/receivers/<path:receiver_id_path>/staged", methods=["PATCH"])
def patch_staged(receiver_id_path):
    rid = receiver_id_path.rstrip("/")
    if rid != _load_receiver_id():
        return jsonify({"error": "receiver not found"}), 404
    body = request.get_json(force=True, silent=True) or {}
    activation = body.get("activation") or {}
    mode = activation.get("mode")

    staged = _staged.get(rid, _default_staged(rid))
    staged["sender_id"] = body.get("sender_id")
    staged["master_enable"] = body.get("master_enable", True)
    staged["activation"] = activation
    if "transport_params" in body:
        staged["transport_params"] = body["transport_params"]
    if "transport_file" in body:
        staged["transport_file"] = body["transport_file"]
    _staged[rid] = staged

    if mode == "activate_immediate":
        video, audio = _parse_patch_body(body)
        _active[rid] = {
            "sender_id": staged["sender_id"],
            "master_enable": staged["master_enable"],
            "activation": {"mode": "activate_immediate", "requested_time": None},
            "transport_params": staged.get("transport_params"),
            "transport_file": staged.get("transport_file"),
        }
        try:
            path = _write_connection_state(
                rid, staged["master_enable"], staged.get("sender_id"), video, audio
            )
            print(f"Wrote connection state to {path}", flush=True)
        except Exception as e:
            return jsonify({"error": str(e)}), 500

    return jsonify(staged), 200


def _default_staged(rid):
    return {
        "sender_id": None,
        "master_enable": False,
        "activation": {"mode": None, "requested_time": None},
        "transport_params": [],
        "transport_file": None,
    }


@app.route(f"{BASE}/receivers/<path:receiver_id_path>/active", methods=["GET"])
def get_active(receiver_id_path):
    rid = receiver_id_path.rstrip("/")
    if rid != _load_receiver_id():
        return jsonify({"error": "receiver not found"}), 404
    return jsonify(_active.get(rid, _default_staged(rid)))


@app.route(f"{BASE}/receivers/<path:receiver_id_path>/constraints", methods=["GET"])
def get_constraints(receiver_id_path):
    rid = receiver_id_path.rstrip("/")
    if rid != _load_receiver_id():
        return jsonify({"error": "receiver not found"}), 404
    return jsonify([])


@app.route(f"{BASE}/receivers/<path:receiver_id_path>/transporttype", methods=["GET"])
def get_transporttype(receiver_id_path):
    rid = receiver_id_path.rstrip("/")
    if rid != _load_receiver_id():
        return jsonify({"error": "receiver not found"}), 404
    return jsonify("urn:x-nmos:transport:rtp.ucast")


if __name__ == "__main__":
    rid = _load_receiver_id()
    if not rid:
        print("Warning: IS05_RECEIVER_ID not set and no .nmos_node.json; run register_node_example.py --save-config", file=sys.stderr)
    else:
        print(f"IS-05 server using receiver_id={rid}", flush=True)
    print(f"Connection state file: {os.path.abspath(CONNECTION_STATE_FILE)}", flush=True)
    app.run(host=BIND, port=PORT, threaded=True)
