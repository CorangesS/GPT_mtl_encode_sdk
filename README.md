# MTL-Encode-SDK

基于 [Media Transport Library (MTL)](https://github.com/OpenVisualCloud/Media-Transport-Library) 与 FFmpeg，实现 **ST2110 收发**、**编码封装** 与 **NMOS 路由对接**，满足 [需求.md](需求.md) 中的全部功能需求。

---

## 一、需求与实现对应

### ST2110 收发 SDK（需求.md §1）

| 需求 | 实现 |
|------|------|
| PTPv2/IEEE 1588 精准时钟同步 | `MtlSdkConfig` 支持 Built-in PTP 与 External `ptp_get_time_fn`；示例应用 `--no-ptp` 可禁用，网卡/模式不支持时自动回退到人工计算时间戳 |
| SDP 解析、导入/导出 | `parse_sdp`、`to_sdp`、`load_sdp_file`、`save_sdp_file`（s/o/c/m、a=rtpmap/fmtp/ts-refclk/mediaclk） |
| 性能指标 | 按实际讨论确定 |

### 编码 SDK（需求.md §2）

| 需求 | 实现 |
|------|------|
| NVIDIA NVENC | 自适应：NVENC → QSV → libx264/libx265 |
| H.264/AVC、H.265/HEVC | `VideoCodec::H264`、`H265` |
| AAC、MP2、PCM、AC3 | `AudioCodec::AAC|MP2|PCM|AC3` |
| MP4、MXF | `Container::MP4|MXF` |
| 编码参数可调 | `bitrate_kbps`、`gop`、`profile`、`set_video_bitrate_kbps`、`apply_reconfigure` |
| 零拷贝送入编码器 | `wrap_video_frame` 对 HostPtr 直接引用 planes；对 CudaDevice 使用 AV_PIX_FMT_CUDA 送 NVENC 零拷贝；DmaBufFd 预留 |
| 音视频时间戳同步 | `first_ts_ns` + `av_rescale_q` 统一 time_base |

### 路由管理（需求.md §3）

| 需求 | 实现 |
|------|------|
| AMWA NMOS (IS-04/IS-05/IS-06/IS-08) | NMOS Registry + Controller（推荐 Easy-NMOS 一站式部署） |
| 集中注册、发现、调度、路由与监控 | Registry 注册；Controller 发现与 IS-05 连接 |
| 可视化界面 | Easy-NMOS Controller（`/admin`）或 NMOS-JS |
| 自研/外购设备管理 | routing 适配层向 Registry 注册；外购设备同 Registry |

---

## 二、项目组成

| 模块 | 说明 |
|------|------|
| **mtl_sdk** | ST2110 视频/音频 TX/RX，PTPv2，SDP 解析，External Frame API；基于标准 MTL 库 |
| **encode_sdk** | H.264/H.265、AAC/MP2/PCM/AC3，MP4/MXF；自适应 GPU/CPU 编码；与 `VideoFrame`/`AudioFrame` 对接 |
| **routing** | NMOS 适配：自研节点注册（IS-04）、IS-05 对接；见 [routing/README.md](routing/README.md) |

**设计要点**（来自 MTL_SDK、编码 SDK 规格）：
- **MTL SDK**：`Context::create_video_rx/tx`、`create_audio_rx/tx`；`VideoFrame`/`AudioFrame` 含 `timestamp_ns`；External Frame 生命周期 query→notify→poll→release；SPSC 无锁 ring；使用标准 MTL 库。
- **编码 SDK**：`Session::open`、`push_video`/`push_audio`、`close`；`EncodeParams` 可配置；HostPtr 零拷贝包装；首帧 timestamp 同步。

---

## 三、前置条件

- **CMake 3.20+**，**C++17**
- **FFmpeg**：`sudo apt install libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev pkg-config`
- **真实 MTL 收发**：已编译 MTL/DPDK，配置大页、网卡（见 MTL 文档）

---

## 四、构建与运行

### 构建

**需要标准 MTL 库**（MTL/DPDK 已编译并配置）：
```bash
mkdir -p build && cd build
cmake .. -DMTL_ROOT=/path/to/MTL   # 可选覆盖，默认 /home/aa/Media-Transport-Library
cmake --build . -j
```

### 运行示例

| 程序 | 作用 |
|------|------|
| **st2110_send** | 发送 ST2110 组播 |
| **st2110_record** | 接收组播并编码为 MP4 |

**本机回环测试**（先启动接收端再启动发送端，使用回环口 `kernel:lo`）：
```bash
# 终端 1（接收端）
./st2110_record --ip 239.0.0.1 --video-port 5004 --audio-port 0 --max-frames 600 recv.mp4 --port kernel:lo

# 终端 2（发送端）
./st2110_send --ip 239.0.0.1 --video-port 5004 --audio-port 0 --duration 10 --port kernel:lo
```

从 YUV 文件做本机回环发送时，发送端可用：
```bash
./st2110_send --url build/yuv420p10le_1080p.yuv --width 1920 --height 1080 --duration 30 --audio-port 0 --ip 239.0.0.1 --video-port 5004 --port kernel:lo
```

**双机收发测试（Kernel 模式）**：双机收发需先**配置网络**（发送端、接收端网卡与 IP 一致），再在两机分别运行发送端与接收端。

- **发送端网卡**：`enp4s0`，IP `192.168.10.1`
- **接收端网卡**：`enp6s0`，IP `192.168.10.2`
- **发送文件**：`build/yuv420p10le_1080p.yuv`（1920×1080）

网络配置（任选其一）：
ethtool enp4s0
ethtool enp6s0
- **临时**：发送端执行 `sudo ip addr add 192.168.10.1/24 dev enp4s0`；接收端执行 `sudo ip addr add 192.168.10.2/24 dev enp6s0`
- **固定**：见 [docs/netplan/README.md](docs/netplan/README.md)，发送端用 `99-st2110-sender-enp4s0.yaml`，接收端用 `99-st2110-receiver-enp6s0.yaml`，然后 `sudo netplan apply`

**接收端（B 机，先启动）：**
```bash
cd build
./st2110_record --ip 239.0.0.1 --video-port 5004 --audio-port 0 \
  --width 1920 --height 1080 --max-frames 200 recv.mp4 \
  --port kernel:enp6s0 --sip 192.168.10.2 --no-ptp
```

**发送端（A 机，后启动）：**
```bash
cd build
./st2110_send --url build/yuv420p10le_1080p.yuv --width 1920 --height 1080 \
  --duration 30 --audio-port 0 --ip 239.0.0.1 --video-port 5004 \
  --port kernel:enp4s0 --sip 192.168.10.1 --no-ptp
```

> 若在 `build` 目录下执行，发送端可用 `--url yuv420p10le_1080p.yuv`。更多双机说明见 [docs/TESTING.md](docs/TESTING.md)。

#### 双机收发（DPDK 模式，高性能）

在按 [docs/DPDK_MTL_SETUP.md](docs/DPDK_MTL_SETUP.md) 完成 **IOMMU、大页、VFIO 组、网卡绑定到 DPDK** 后，可以使用 DPDK/MTL 模式进行 ST2110 高性能收发。

- **发送端（A 机）**：直连网卡 BDF 如 `0000:04:00.0`，本机 IP `192.168.10.1`
- **接收端（B 机）**：直连网卡 BDF 如 `0000:06:00.0`，本机 IP `192.168.10.2`
- **组播地址**：`239.0.0.1`，**视频端口**：`5004`
- **YUV 文件**：`yuv420p10le_1080p.yuv`（1920×1080，59.94fps，示例自带）
- **说明**：`--port` 使用网卡 **BDF** 表示 DPDK 模式，`--sip` 为该直连网口的静态 IP。

**接收端（B 机，先启动）：**

```bash
cd /path/to/GPT_mtl_encode_sdk/build

./st2110_record --ip 239.0.0.1 --video-port 5004 --audio-port 0 \
  --width 1920 --height 1080 --max-frames 1800 recv.mp4 \
  --port 0000:06:00.0 --sip 192.168.10.2 --no-ptp
```

**发送端（A 机，后启动）：**

```bash
cd /path/to/GPT_mtl_encode_sdk/build

./st2110_send --url yuv420p10le_1080p.yuv --width 1920 --height 1080 \
  --duration 30 --audio-port 0 --ip 239.0.0.1 --video-port 5004 \
  --port 0000:04:00.0 --sip 192.168.10.1 --no-ptp
```

> 若在项目根目录执行，将 `--url` 改为 `--url build/yuv420p10le_1080p.yuv`。  
> 完整的 DPDK 配置步骤（开启 IOMMU、配置大页、绑定网卡到 `vfio-pci`、多进程场景下的 `MtlManager` 等）见 [docs/DPDK_MTL_SETUP.md](docs/DPDK_MTL_SETUP.md)。

---

## 五、主要参数

| 参数 | st2110_send | st2110_record |
|------|-------------|---------------|
| `--port` | 端口/网卡：`kernel:lo`、`kernel:eth0`、`0000:xx:xx.x`(DPDK) | 须一致 |
| `--sip` | 本机源 IP | 须一致 |
| `--ip` | 组播 IP | 须一致 |
| `--video-port` | 视频端口 | 须一致 |
| `--audio-port` | 音频端口（0=无） | 须一致 |
| `--duration` | 发送秒数 | - |
| `--max-frames` | - | 接收帧数 |
| `--width` `--height` `--fps` | 分辨率/帧率 | 须一致 |
| `--url` | YUV 文件路径 | - |
| `--no-ptp` | 禁用 PTP，使用人工计算时间戳（网卡不支持 PTP 时回退） | 收发一致 |

---

## 六、注意事项

1. **MTL 路径**：`MTL_ROOT` 需含 `include/`、`build/` 或 `lib/`
2. **网卡/端口**：`--port` 指定传输模式；`kernel:lo` 本机回环、`kernel:eth0` 物理网卡（Socket）、`0000:af:01.0` DPDK 模式（BDF）；跨机需 `--port kernel:eth0`、`--sip <本机IP>`
3. **组播**：两机需组播互通；PTP 时需同一 PTP 域
4. **PTP**：默认启用 PTP；若网卡或模式不支持（如 kernel:lo），自动回退到人工计算时间戳，或使用 `--no-ptp` 显式禁用
5. **编码**：自适应 NVENC/QSV/CPU；启动时输出 `encode_sdk: using h264_nvenc` 等

---

## 七、文档索引

| 文档 | 内容 |
|------|------|
| [需求.md](需求.md) | 详细需求 |
| [docs/README.md](docs/README.md) | 文档导航 |
| [docs/COMPLIANCE.md](docs/COMPLIANCE.md) | 需求符合性检查 |
| [docs/TESTING.md](docs/TESTING.md) | 本机/双机测试、参数、frame_cnt 说明 |
| [docs/DPDK_MTL_SETUP.md](docs/DPDK_MTL_SETUP.md) | 双机直连下 DPDK/MTL 配置（IOMMU、大页、VFIO、网卡绑定） |
| [docs/ROUTING.md](docs/ROUTING.md) | NMOS 路由对接 |
| [docs/EASY_NMOS_IMPLEMENTATION.md](docs/EASY_NMOS_IMPLEMENTATION.md) | **路由管理实现指南（Easy-NMOS）** |
| [docs/IS05_SERVER_IMPLEMENTATION.md](docs/IS05_SERVER_IMPLEMENTATION.md) | **自研节点 IS-05 服务端实现详解** |
| [docs/NMOS_JS_DEPLOY.md](docs/NMOS_JS_DEPLOY.md) | NMOS-JS 部署与配置 |
| [routing/README.md](routing/README.md) | 路由模块说明 |

## 八、路由管理快速开始（Easy-NMOS）

若已部署 Easy-NMOS，快速接入自研节点：

```bash
export REGISTRY_URL=http://<Easy-NMOS-IP>   # 如 http://192.168.6.101
python3 routing/scripts/register_node_example.py --heartbeat --interval 10 --save-config .nmos_node.json &
python3 routing/is05_server/app.py &        # IS-05 服务（可选，使 Controller 连接能驱动收流）
./build/is05_receiver_daemon &               # 或 ./build/st2110_record ... 收流编码
```

或仅注册 + 手动收流：`routing/scripts/run_with_nmos.sh`。IS-05 服务端与 daemon 说明见 [routing/is05_server/README.md](routing/is05_server/README.md)。详见 [docs/EASY_NMOS_IMPLEMENTATION.md](docs/EASY_NMOS_IMPLEMENTATION.md)。

## 九、测试

需求符合性测试见 [tests/README.md](tests/README.md)：

```bash
cmake --build build -j
./tests/scripts/run_all_tests.sh
```

