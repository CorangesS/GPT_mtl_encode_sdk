# ST2110 收发与编码测试说明

本文说明本机两进程、双机收发测试，以及 frame_cnt、参数配置等要点。

---

## 一、本机两进程收发

本 SDK 使用标准 MTL 库，收发均为真实网络组播。本机回环（`kernel:lo`）下 PTP 通常不可用，会自动回退到人工计算时间戳；也可显式加 `--no-ptp`。

```bash
# 终端 1（先启动接收端）
./st2110_record --ip 239.0.0.1 --video-port 5004 --audio-port 5006 --max-frames 300 recv.mp4

# 终端 2（再启动发送端）
./st2110_send --ip 239.0.0.1 --video-port 5004 --audio-port 5006 --duration 10
```

---

## 二、双机收发

| 机器 | 程序 | 作用 |
|------|------|------|
| **A 机** | `st2110_send` | 仅发送 ST2110 组播 |
| **B 机** | `st2110_record` | 仅接收并 H.264 编码 |

**要求**：组播互通；`--ip`、`--video-port`、`--audio-port` 一致；`--port`、`--sip` 使用实际网卡与 IP。

**示例：**

```bash
# A 机
./st2110_send --url /path/to/yuv420p10le_1080p.yuv --width 1920 --height 1080 \
  --duration 60 --audio-port 0 --port kernel:eth0 --sip 192.168.1.10

# B 机
./st2110_record --ip 239.0.0.1 --video-port 5004 --audio-port 0 \
  --width 1920 --height 1080 --max-frames 3600 recv.mp4 \
  --port kernel:eth0 --sip 192.168.1.20
```

**拓扑：**

```
[A 机] st2110_send → ST2110 组播 239.0.0.1:5004 → [B 机] st2110_record → recv.mp4
```

---

## 三、参数对应

| 参数 | st2110_send | st2110_record |
|------|-------------|---------------|
| 端口/网卡 | `--port` | `--port` |
| 本机源 IP | `--sip` | `--sip` |
| 组播 IP | `--ip` | `--ip` |
| 视频端口 | `--video-port` | `--video-port` |
| 音频端口 | `--audio-port` | `--audio-port` |
| 发送时长 | `--duration` | - |
| 接收帧数 | - | `--max-frames` |
| PTP | `--no-ptp` 禁用 | `--no-ptp` 禁用 |

默认：`--port kernel:lo`，`--sip 127.0.0.1`，IP `239.0.0.1`，视频 5004，音频 5006。PTP 默认启用，网卡不支持时自动回退到人工计算时间戳。

---

## 四、frame_cnt_ 说明

**含义**：External Frame 的 buffer 数量。编码较慢时，若 buffer 不足会出现 “no slot” / “slot get frame fail”。

| 当前值 | 说明 |
|--------|------|
| **8**（mtl_backend_mtl 默认） | 1080p 约 66MB；编码跟不上时可能丢帧 |
| 32 / 48 / 64 | 需修改源码增大 frame_cnt_，内存充足时可减少丢帧 |
| **1024** | 不推荐；约 8.5GB，延迟大，MTL 可能不支持 |

---

## 五、与需求.md 对应

- **ST2110 收发**：本机/双机测试即收发能力验证；PTPv2、SDP 由 mtl_sdk 提供；示例支持 `--no-ptp` 及 PTP 不可用时的自动回退。
- **编码**：st2110_record 用 encode_sdk 收流并编码为 MP4/MXF，满足编码需求。

## 六、端口模式说明

| `--port` 值 | 模式 | 说明 |
|-------------|------|------|
| `kernel:lo` | Socket 回环 | 本机测试，PTP 通常不可用 |
| `kernel:eth0` | Socket 物理网卡 | 跨机或本机物理网卡 |
| `0000:af:01.0` | DPDK | 需网卡已绑定 DPDK（如 vfio-pci） |
