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

## 四、运行本项目的 3 个硬性要求（务必先满足）

1. **网络与端口必须“对得上”**  
   - **发送端与接收端**的 `--ip`（组播地址）与 `--video-port` 必须一致。  
   - `--port` 必须匹配你实际使用的链路：  
     - `kernel:lo`：本机回环  
     - `kernel:<网卡名>`：内核网卡（Socket）  
     - `<BDF>`（如 `0000:04:00.0`）：DPDK 网卡（VFIO 绑定后内核不可用）
   - `--sip` 必须填写“该链路的源 IP”（Kernel 模式=网卡实际 IP；DPDK 模式=写入报文源 IP，**不要求**在内核上配置，但建议仍按同网段规划）。

2. **不要把用于测试的直连网卡配置成永久静态 IP**  
   - 仅用 `ip addr add ...` 的**临时 IP**做测试；不要用 netplan/NM 做“永久静态”，否则常见会触发 NetworkManager 报错、连接反复 flap、路由异常等问题。  
   - README 下文给出“添加/删除临时 IP”的标准命令。

3. **DPDK 模式要满足 DPDK/MTL 的运行条件**  
   - 已完成：IOMMU、HugePages、`vfio-pci`、网卡绑定到 DPDK（见 `docs/需求1_视频流收发部署与使用.md`、`docs/DPDK_MTL_SETUP.md`）。  
   - DPDK 模式下网卡被 VFIO 接管后，Linux 内核无法 `ping`/无法 `kernel:<iface>` 使用，这是正常现象。

---

## 五、构建与运行

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
| **av_txrx_demo** | 音视频收发统一示例，支持 PTP、lcores、tasklets 等；DPDK 发送默认 `build/yuv420p10le_1080p.yuv`，见 `--mode send` / `--mode recv` |

---

## 六、网卡临时 IP（推荐做法：只用于测试，不写永久静态）

### 6.1 两块“支持 DPDK 的直连网卡”如何配置临时 IP

> 这里的“两块网卡”指 **你用于直连收发**的两口（可能同一台机器两口直连，也可能 A/B 两机各一口直连）。  
> **原则**：只用临时 IP，测试完可删；不要写永久静态 IP，避免 NetworkManager/netplan 相关错误。

假设两口分别为 `enp4s0` 与 `enp6s0`，计划网段为 `192.168.10.0/24`：

```bash
# 查看网卡名
ip -br link

# 给两口添加“临时 IP”（不会写入永久配置）
sudo ip addr add 192.168.10.1/24 dev enp4s0
sudo ip addr add 192.168.10.2/24 dev enp6s0
sudo ip link set enp4s0 up
sudo ip link set enp6s0 up

# 验证
ip -br addr show dev enp4s0
ip -br addr show dev enp6s0
```

测试结束后删除临时 IP：

```bash
sudo ip addr del 192.168.10.1/24 dev enp4s0
sudo ip addr del 192.168.10.2/24 dev enp6s0
```

> 若某网卡已被绑定到 DPDK（`vfio-pci`），该口不会再出现在内核网络栈里，因此无法 `ip addr add`，这也是正常的。Kernel 模式需要网卡处于内核驱动；DPDK 模式则用 `--port <BDF>`。

---

## 七、收发运行命令（按场景直接照抄）

### 7.1 单机 Kernel 回环（`kernel:lo`）如何运行

先启动接收端，再启动发送端（同一台机器，走 loopback）：
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

### 7.2 单机 DPDK “双口回环”（同机两块/两口 DPDK 网卡直连）如何运行

该模式不是 `lo`，而是**同一台机器的两口网卡互相直连**（用网线把两口插在一起），一口跑 TX、一口跑 RX。

前提（只列关键点，完整见 `docs/DPDK_MTL_SETUP.md`）：

- 两口网卡都已绑定到 `vfio-pci`（DPDK 接管后内核不可用）
- 已配置 hugepages / IOMMU / VFIO

示例：同机两口 BDF 为 `0000:04:00.0`（TX）与 `0000:06:00.0`（RX），你规划两个源 IP（仅用于写入报文源地址）：

- TX：`--port 0000:04:00.0 --sip 192.168.10.1`
- RX：`--port 0000:06:00.0 --sip 192.168.10.2`

运行（同机两个终端）：

```bash
# 终端 1：接收端（先启动）
cd build
./st2110_record --ip 239.0.0.1 --video-port 5004 --audio-port 0 \
  --width 1920 --height 1080 --max-frames 600 recv.mp4 \
  --port 0000:06:00.0 --sip 192.168.10.2 --no-ptp

# 终端 2：发送端（后启动）
cd build
./st2110_send --url yuv420p10le_1080p.yuv --width 1920 --height 1080 \
  --duration 10 --audio-port 0 --ip 239.0.0.1 --video-port 5004 \
  --port 0000:04:00.0 --sip 192.168.10.1 --no-ptp
```

> 说明：DPDK 模式下 `--sip` 是写进报文的源 IP，不要求 Linux 真实配置该 IP（因为网卡已被 VFIO 接管），但两端仍应使用同网段规划，避免上层控制/排障混乱。

### 7.3 双机 Kernel 模式如何运行（A 发送、B 接收）

双机收发需先用 **临时 IP** 配好两机直连口（不要永久静态），再在两机分别运行接收与发送。

- **发送端网卡**：`enp4s0`，IP `192.168.10.1`
- **接收端网卡**：`enp6s0`，IP `192.168.10.2`
- **发送文件**：`build/yuv420p10le_1080p.yuv`（1920×1080）

两机分别执行（临时 IP）：

```bash
# A（发送端）
sudo ip addr add 192.168.10.1/24 dev enp4s0
sudo ip link set enp4s0 up

# B（接收端）
sudo ip addr add 192.168.10.2/24 dev enp6s0
sudo ip link set enp6s0 up
```

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

> 若在 `build` 目录下执行，发送端可用 `--url yuv420p10le_1080p.yuv`。网卡配置、双机 Kernel/DPDK 流程及故障排查见 [docs/需求1_视频流收发部署与使用.md](docs/需求1_视频流收发部署与使用.md)。

#### 双机收发（DPDK 模式，高性能）

在按 [docs/需求1_视频流收发部署与使用.md](docs/需求1_视频流收发部署与使用.md) 完成 **IOMMU、大页、VFIO、网卡绑定到 DPDK** 后，可使用网卡 BDF 进行 ST2110 高性能收发。

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
| `--put-retry` `--prefill-frames` | 发送端：put_video 失败重试次数、启动预填帧数，利于跑满 2.5G、缓解 build timeout | 见 [DPDK_MTL_SETUP §2.6](docs/DPDK_MTL_SETUP.md#26-cpulcore-与-tasklet-配置跑满-25g如-i226-v) |

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
| [docs/需求1_视频流收发部署与使用.md](docs/需求1_视频流收发部署与使用.md) | **需求1** 本地回环、双机 Kernel/DPDK、网卡配置与查看、DPDK 解绑回 Kernel |
| [docs/需求2_编码SDK部署与使用.md](docs/需求2_编码SDK部署与使用.md) | **需求2** 编码 SDK 使用方式（EncodeParams、Session、与收流对接） |
| [docs/需求3_路由管理部署与使用.md](docs/需求3_路由管理部署与使用.md) | **需求3** 路由管理运行方式与双机测试流程 |
| [routing/README.md](routing/README.md) | 路由模块说明 |

## 八、路由管理（Easy-NMOS + IS-05）双机部署与命令（按你的场景 A=发送、B=接收）

本节只讲你关心的三件事：

- **Easy-NMOS（Registry + Controller/admin）怎么用 macvlan 跑起来，并且“运行 Easy-NMOS 的那台主机自己也能打开 admin 页面”**  
- **A 机作为 Sender、B 机作为 Receiver 时，IS-05 服务如何部署/配置（IP、端口）**  
- **`routing/scripts/register_node_example.py` 怎么传参（IP/PORT）并且每秒 heartbeat**

### 8.1 角色与 IP 约定（你可以按实际替换）

| 机器 | 角色 | 需要运行的东西 |
|------|------|----------------|
| **A 机** | Easy-NMOS + 自研 Sender（发音视频） | Docker Easy-NMOS（macvlan），`register_node_example.py --mode sender`，`routing/is05_server/app.py` |
| **B 机** | 自研 Receiver（收音视频） | `register_node_example.py --mode receiver`，`routing/is05_server/app.py`，`build/is05_receiver_daemon` |

核心 IP/端口变量：

- **Easy-NMOS Registry/Controller 地址**：`EASY_NMOS_IP=192.168.1.200`（来自 `/home/aa/easy-nmos/docker-compose.yml` 的 `nmos-registry.ipv4_address`）  
- **IS-05 默认端口**：`IS05_PORT=9090`（可改）  
- **A_HOST_IP**：A 机操作系统自身在局域网可达的 IP（不是容器 IP）  
- **B_HOST_IP**：B 机操作系统自身在局域网可达的 IP

### 8.2 在 A 机启动 Easy-NMOS（macvlan），并让 A 主机也能访问 admin

Easy-NMOS 的 compose 文件固定在：`/home/aa/easy-nmos/docker-compose.yml`。其中网络是 macvlan（`parent: eno1`，容器固定 IP `192.168.1.200/201/203`）。

在 **A 机**执行：

```bash
cd /home/aa/easy-nmos
docker compose up -d
```

macvlan 的限制：**宿主机默认不能访问 macvlan 容器 IP**。要让 A 主机也能打开 `http://192.168.1.200/admin`，在 A 机创建一个“宿主机侧 macvlan 子接口”（临时配置即可）：

```bash
# 按 docker-compose.yml：parent=eno1
PARENT_IF=eno1

# 给宿主机创建一个 macvlan 接口（名字可自定义）
sudo ip link add easy-nmos-host link ${PARENT_IF} type macvlan mode bridge

# 给这个宿主机 macvlan 接口配置一个同网段 IP（选个不冲突的）
sudo ip addr add 192.168.1.250/24 dev easy-nmos-host
sudo ip link set easy-nmos-host up

# 只对容器 IP 加 /32 路由（避免影响整网段路由）
sudo ip route add 192.168.1.200/32 dev easy-nmos-host
sudo ip route add 192.168.1.201/32 dev easy-nmos-host
sudo ip route add 192.168.1.203/32 dev easy-nmos-host
```

现在 A 主机、B 机、以及其他同网段电脑都应能访问：

- Easy-NMOS admin：`http://192.168.1.200/admin`

> 若你从“另一台电脑”的浏览器打开 admin，那么浏览器后续会直接访问 A/B 节点的 `--href`（IS-05 地址），因此 **A_HOST_IP / B_HOST_IP 与 9090 端口必须从浏览器所在机器可达**（防火墙放行）。

### 8.3 IS-05 的 IP/端口怎么配置（必须写在 `--href` 里）

这项目里，**IS-05 服务地址=注册脚本的 `--href`**，格式固定：

- `--href http://<节点机器IP>:<IS05_PORT>/`

并且 IS-05 服务端口由环境变量控制：

- `IS05_PORT=9090 python3 routing/is05_server/app.py`

**规则**：

- `--href` 里的 IP 必须填“运行 IS-05 服务那台机器的真实 IP”（A_HOST_IP 或 B_HOST_IP），不能填 `127.0.0.1`，否则从别处打开 admin 会访问不到。  
- `--href` 的端口必须与 `IS05_PORT` 一致，否则 admin 会报错（CONNECT/ACTIVE/TRANSPORT FILE 不可用）。

### 8.4 A 机（Sender）如何注册 + 1 秒心跳 + 启动 IS-05（给 admin 提供 transportfile）

在 **A 机**（项目根目录）执行：

```bash
cd /home/aa/GPT_mtl_encode_sdk

# Easy-NMOS Registry/Controller 在 192.168.1.200（容器 IP）
export REGISTRY_URL=http://192.168.1.200

# 关键：--href 里填 A 机自己可达的 IP（A_HOST_IP）+ IS05_PORT
python3 routing/scripts/register_node_example.py --mode sender \
  --heartbeat --interval 1 \
  --href http://<A_HOST_IP>:9090/ \
  --save-config .nmos_node.json

# 启动 IS-05（Sender 端也需要跑，否则 admin 拉不到 sender 的 transportfile / staged / active）
IS05_PORT=9090 python3 routing/is05_server/app.py
```

### 8.5 B 机（Receiver）如何注册 + 1 秒心跳 + 启动 IS-05 + 运行 daemon（让 CONNECT 真正驱动收流）

在 **B 机**执行：

```bash
cd /home/aa/GPT_mtl_encode_sdk
export REGISTRY_URL=http://192.168.1.200

python3 routing/scripts/register_node_example.py --mode receiver \
  --heartbeat --interval 1 \
  --href http://<B_HOST_IP>:9090/ \
  --save-config .nmos_node.json

# Receiver 侧 IS-05：PATCH activate_immediate 会写 connection_state.json
CONNECTION_STATE_FILE=./connection_state.json IS05_PORT=9090 \
  python3 routing/is05_server/app.py
```

另开一个终端在 **B 机**运行收流 daemon（读取上面写的 connection_state）：

```bash
cd /home/aa/GPT_mtl_encode_sdk/build
./is05_receiver_daemon
```

### 8.6 在 admin 里如何“用 IS-05 服务”（操作顺序）

1. 打开 `http://192.168.1.200/admin`  
2. 确认能看到 A 机注册出来的 **Sender**，以及 B 机注册出来的 **Receiver**  
3. 在 **Receivers** 里选 B 的 Receiver，点击 **CONNECT**，选择 A 的 Sender  
4. 确认后，浏览器会对 `http://<B_HOST_IP>:9090/x-nmos/connection/v1.1/single/receivers/<receiver_id>/staged` 发 PATCH（`activate_immediate`）  
5. B 上 IS-05 会写 `connection_state.json`，B 上 `is05_receiver_daemon` 轮询到变更后开始按新连接收流/编码

### 8.7 `register_node_example.py` 的输入参数（重点讲 IP 与 PORT 怎么填）

脚本：`routing/scripts/register_node_example.py`

- **`REGISTRY_URL`（环境变量）**：填 Easy-NMOS 的地址  
  - 你的场景固定为：`export REGISTRY_URL=http://192.168.1.200`
- **`--href`（必须正确）**：填“节点机器对外可达的 IS-05 地址”  
  - A（Sender）：`--href http://<A_HOST_IP>:9090/`  
  - B（Receiver）：`--href http://<B_HOST_IP>:9090/`
- **`--mode receiver|sender|both`**：注册的角色  
  - A 发：用 `sender`  
  - B 收：用 `receiver`  
  - 若某台机器同时发+收：用 `both`
- **`--save-config .nmos_node.json`（强烈建议）**：把 `node_id/device_id/receiver_id/sender_id` 保存下来  
  - IS-05 服务端会读取它（默认文件名就是 `.nmos_node.json`）  
  - 复用同一个 `.nmos_node.json` 可避免每次重跑导致 ID 变化，从而出现“Registry 里的 receiver_id 与 IS-05 里不一致”
- **`--heartbeat --interval 1`（你要求的每秒心跳）**：每 1 秒重新注册一次，保持节点不从 Registry 消失  
  - `--interval` 建议 **<= 10**（脚本内说明：避免 Registry TTL 到期导致节点闪烁）

如果你想一条命令在某台机器上“注册 + 1s 心跳 + 启动 IS-05”，也可以用脚本：

```bash
cd /home/aa/GPT_mtl_encode_sdk
export REGISTRY_URL=http://192.168.1.200
NODE_HREF=http://<本机IP>:9090/ ./routing/scripts/run_routing_node.sh --heartbeat --mode receiver --interval 1
```

> A 机做 sender 时，把 `--mode receiver` 改成 `--mode sender`；B 机做 receiver 时保持 receiver。

## 九、测试

需求符合性测试见 [tests/README.md](tests/README.md)：

```bash
cmake --build build -j
./tests/scripts/run_all_tests.sh
```

