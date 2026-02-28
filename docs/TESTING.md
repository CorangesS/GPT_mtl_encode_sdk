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

### 2.1 使用 Kernel 模式（两机已能 ping 通时）

无需 DPDK，直接指定直连网卡和本机 IP 即可。假设：A 机 IP `192.168.10.1`，B 机 IP `192.168.10.2`，网卡名均为 `enp3s0`（以 `ip link` 实际为准）。

**发送端（A 机，本机）**——从 `build/yuv420p10le_1080p.yuv` 发送（1920×1080，默认 59.94fps）：

```bash
cd build   # 或项目根目录，保证能访问到 build/yuv420p10le_1080p.yuv
./st2110_send --url build/yuv420p10le_1080p.yuv --width 1920 --height 1080 \
  --duration 30 --audio-port 0 --ip 239.0.0.1 --video-port 5004 \
  --port kernel:enp3s0 --sip 192.168.10.1 --no-ptp
```

**接收端（B 机）**——先启动：

```bash
cd build
./st2110_record --ip 239.0.0.1 --video-port 5004 --audio-port 0 \
  --width 1920 --height 1080 --max-frames 1800 recv.mp4 \
  --port kernel:enp3s0 --sip 192.168.10.2 --no-ptp
```

若 YUV 在项目根目录的 `build/` 下，在项目根执行时用 `--url build/yuv420p10le_1080p.yuv`；若在 `build/` 下执行则用 `--url yuv420p10le_1080p.yuv`。

#### 2.1.1 示例：本机 enp4s0、接收端 enp6s0（网络配置与命令详解）

**拓扑**：发送端（本机）用网卡 **enp4s0**，接收端用网卡 **enp6s0**，两机直连或在同一网段。组播流从本机发出，接收端订阅同一组播地址接收。

**第一步：本机（发送端）配置 enp4s0**（若已按下方「固定两台电脑 IP」用 Netplan 配置，可跳过）

临时配置：给 enp4s0 配置静态 IP，使 MTL 能从该网卡读到源 IP：

```bash
# 本机执行（发送端）
sudo ip addr add 192.168.10.1/24 dev enp4s0
```

验证：`ip -4 addr show enp4s0` 应看到 `inet 192.168.10.1/24`。

**第二步：接收端配置 enp6s0**（若已按下方「固定两台电脑 IP」用 Netplan 配置，可跳过）

临时配置：在接收端机器上给 enp6s0 配置同一网段的 IP（且不能与发送端冲突）：

```bash
# 在接收端机器上执行
sudo ip addr add 192.168.10.2/24 dev enp6s0
```

验证：`ip -4 addr show enp6s0` 应看到 `inet 192.168.10.2/24`。

**第三步：连通性测试（可选）**

两机直连时，在本机执行：`ping -c 2 192.168.10.2`；在接收端执行：`ping -c 2 192.168.10.1`。能 ping 通说明网络正常。

**第四步：接收端先启动**

在**接收端**机器上进入 build 目录，先启动接收程序（先订阅组播，再发流效果更好）：

```bash
cd /path/to/GPT_mtl_encode_sdk/build

./st2110_record --ip 239.0.0.1 --video-port 5004 --audio-port 0 \
  --width 1920 --height 1080 --max-frames 1800 recv.mp4 \
  --port kernel:enp6s0 --sip 192.168.10.2 --no-ptp
```

**第五步：本机（发送端）再启动发送**

在本机执行：

```bash
cd /home/aa/GPT_mtl_encode_sdk/build

./st2110_send --url yuv420p10le_1080p.yuv --width 1920 --height 1080 \
  --duration 30 --audio-port 0 --ip 239.0.0.1 --video-port 5004 \
  --port kernel:enp4s0 --sip 192.168.10.1 --no-ptp
```

**参数详解**

| 参数 | 发送端（本机） | 接收端 | 说明 |
|------|----------------|--------|------|
| `--port` | `kernel:enp4s0` | `kernel:enp6s0` | 使用的网卡：本机用 enp4s0，接收端用 enp6s0。`kernel:` 表示走内核协议栈（Socket），不占用 DPDK。 |
| `--sip` | `192.168.10.1` | `192.168.10.2` | 本机在该网卡上的源 IP，需与上面配置的 IP 一致；MTL 会据此校验并从该网卡收发。 |
| `--ip` | `239.0.0.1` | `239.0.0.1` | 组播地址，两端必须一致，发送端发往该地址，接收端订阅该地址。 |
| `--video-port` | `5004` | `5004` | 视频 RTP 端口，两端一致。 |
| `--audio-port` | `0` | `0` | 0 表示无音频；若需音频则改为同一非零端口（如 5006）。 |
| `--width` / `--height` | 1920 / 1080 | 1920 / 1080 | 分辨率必须一致。 |
| `--no-ptp` | 使用 | 使用 | 禁用 PTP，用程序内部时间戳；两机都加则行为一致。 |

**固定两台电脑 IP（重启后不消失）**

项目内已提供 Netplan 配置，复制到系统后应用即可固定 IP，无需每次手动 `ip addr add`。

- **发送端（本机，enp4s0）**：固定为 `192.168.10.1`
- **接收端（enp6s0）**：固定为 `192.168.10.2`

**发送端执行（本机）：**

```bash
cd /home/aa/GPT_mtl_encode_sdk
sudo cp docs/netplan/99-st2110-sender-enp4s0.yaml /etc/netplan/
sudo netplan apply
```

**接收端执行（在接收端机器上）：**

```bash
cd /path/to/GPT_mtl_encode_sdk   # 或把 99-st2110-receiver-enp6s0.yaml 拷到接收端任意目录
sudo cp docs/netplan/99-st2110-receiver-enp6s0.yaml /etc/netplan/
sudo netplan apply
```

完成后用 `ip -4 addr show enp4s0`（本机）和 `ip -4 addr show enp6s0`（接收端）确认 IP 已生效；重启后仍会保持，无需再执行上面的第一步、第二步临时配置。

### 2.2 使用 DPDK/MTL 模式（双机直连）

DPDK 模式需要：两台机器均完成 **IOMMU、大页、VFIO 权限、网卡绑定**，且 `--port` 使用网卡的 **BDF**（如 `0000:af:01.0`），`--sip` 仍为对应网口 IP。配置步骤见 [docs/DPDK_MTL_SETUP.md](DPDK_MTL_SETUP.md)。配置完成后，发送/接收示例：

```bash
# A 机（发送端，BDF 与 sip 按实际直连网卡修改）
./st2110_send --url build/yuv420p10le_1080p.yuv --width 1920 --height 1080 \
  --duration 30 --audio-port 0 --ip 239.0.0.1 --video-port 5004 \
  --port 0000:af:01.0 --sip 192.168.10.1 --no-ptp

# B 机（接收端，先启动）
./st2110_record --ip 239.0.0.1 --video-port 5004 --audio-port 0 \
  --width 1920 --height 1080 --max-frames 1800 recv.mp4 \
  --port 0000:af:01.0 --sip 192.168.10.2 --no-ptp
```

若一机多 MTL 进程，需先在该机运行 `sudo MtlManager`（见 MTL 官方 Run Guide）。

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

---

## 七、本机测试快、双机测试慢的原因说明

**现象**：本机用 `kernel:lo` 收发时速度很快；双机用 `kernel:eth0` 时发送/接收明显变慢。

### 7.1 发送端：始终按「实时」节奏发

`st2110_send` 内部按配置的帧率（如 59.94 fps）做 **实时 pacing**：每发一帧就 `sleep_until(next)`，保证「发 10 秒内容就花 10 秒」。因此：

- **本机**：发 10 秒流 → 发送端大约跑 10 秒。
- **双机**：同样是发 10 秒流 → 发送端仍然大约跑 10 秒。

也就是说，发送端在本机/双机下 **节奏一样**，都是「实时」，并没有在双机时故意变慢。感觉双机「慢」，主要来自 **网络和接收端**。

### 7.2 本机为什么感觉很快？

1. **回环（loopback）几乎无延迟、无丢包**
   - 数据从发送进程经 `kernel:lo` 直接到本机协议栈，不经过网卡、线缆。
   - 接收端几乎「立刻」拿到包，没有网络 RTT、抖动、丢包。
   - 收发在同一台机器上，CPU/内存共享，接收 + 编码能跟上发送节奏，所以整体很快、很顺。

2. **内核对本机回环做了优化**
   - 本机回环通常不经过完整网卡驱动和队列，拷贝/调度开销小，所以本机两进程之间吞吐高、延迟低。

### 7.3 双机为什么容易变慢？

1. **物理网络延迟与抖动**
   - 包要经过：发送机网卡 → 线缆/交换机 → 接收机网卡 → 内核协议栈。
   - 有 RTT、排队、抖动；首帧到达时间晚，后续帧也可能不匀速，接收端更容易出现「等包」或「突然来一大坨」的情况。

2. **Kernel 模式（Socket）开销大**
   - 双机常用 `kernel:eth0`（内核协议栈），相比 `kernel:lo` 多了：网卡驱动、中断、协议栈、多级队列。
   - 发送端：内核可能因缓冲区、调度等原因不能像回环那样「瞬时」把包发完。
   - 接收端：收包、拷贝、从内核到用户态的开销都更大，若 CPU 或缓冲不足，容易跟不上发送节奏，表现为「收得慢」或丢帧。

3. **组播与网络配置**
   - 组播依赖 IGMP、交换机组播表等；若未正确配置，可能退化为广播或表现异常，增加丢包/重传/延迟。
   - MTU 不足会导致分片；ST2110 大包一旦有一个分片丢失，整帧丢失，接收端会少帧，看起来就像「慢」或卡顿。

4. **接收端编码跟不上**
   - `st2110_record` 先收流再编码。若接收机 CPU/GPU 较弱，或编码参数较重，编码速度跟不上收包速度。
   - External Frame 的 buffer（如 frame_cnt_=8）用完后会出现「no slot」、丢帧，有效「接收速率」下降，整体就感觉慢。

5. **无 PTP 时两端时钟独立**
   - 双机若用 `--no-ptp`，发送端和接收端各用本机时钟。对「速度」影响主要是抖动和同步，一般不直接导致「整体变慢」，但会加重上面 1～4 的影响（例如缓冲、排队行为更不可预测）。

### 7.4 如何改善双机速度与稳定性？

| 手段 | 说明 |
|------|------|
| **双机直连 + DPDK** | 按 [docs/DPDK_MTL_SETUP.md](DPDK_MTL_SETUP.md) 配置 DPDK/MTL，`--port` 用网卡 BDF。绕过内核协议栈，延迟和吞吐更接近本机回环。 |
| **确认组播与 MTU** | 两机同网段、组播可达；避免 MTU 过小导致分片，必要时调大接口 MTU（如 9000）。 |
| **接收端资源** | 接收机保证 CPU/GPU 足够；可适当增大 frame_cnt_（需改源码）以减少因 buffer 不足导致的丢帧。 |
| **先确认链路** | 双机 `ping`、`iperf3` 等确认带宽和延迟正常；再用 `st2110_send` / `st2110_record` 做收发测试。 |

**小结**：本机快是因为回环无真实网络延迟和丢包，且内核优化好；双机慢主要来自真实网络的延迟/抖动/丢包、内核 Socket 的开销，以及接收端处理能力。要接近本机体验，建议在双机直连场景下使用 DPDK 模式。

---

## 八、故障排查

### `SIOCGIFADDR fail` / `get ip fail from if enp3s0`

使用 `--port kernel:enp3s0` 时，MTL 会从该网卡读取 IP 地址。若报错：

- **网卡名错误**：本机可能不是 `enp3s0`。先查看实际网卡名：
  ```bash
  ip link show
  # 或
  ls /sys/class/net/
  ```
  然后用实际名称，例如 `--port kernel:eth0`，且 `--sip` 填该网卡上配置的 IP。

- **网卡未配置 IP**：即使传了 `--sip 192.168.10.1`，MTL 仍会校验该 IP 是否属于指定网卡。需在对应网卡上配置好 IP，例如（发送端 A 机）：
  ```bash
  sudo ip addr add 192.168.10.1/24 dev enp3s0
  ```
  若用 DHCP，确认该网卡已拿到地址后再运行程序。

- **本机回环测试**：不依赖物理网卡时，可用回环口：
  ```bash
  ./st2110_send ... --port kernel:lo --sip 127.0.0.1
  ./st2110_record ... --port kernel:lo --sip 127.0.0.1
  ```
