# mtl_sdk 与 encode_sdk 上层应用使用说明

本文说明上层应用如何接入 **mtl_sdk**（ST2110 收发）与 **encode_sdk**（编码封装），以及如何满足需求中的 **PTPv2** 与 **“收发使用最多 CPU”** 以提升性能。

---

## 一、需求对应（需求.md）

| 需求 | 对应实现 |
|------|----------|
| 视频流/音频流收发均支持 **PTPv2（IEEE 1588）** | mtl_sdk：`MtlSdkConfig::enable_builtin_ptp = true`（默认），或 `ptp_mode = ExternalFn` + `external_ptp_time_fn` |
| 收发使用最多 CPU、性能尽量高 | mtl_sdk：`MtlSdkConfig` 中配置 `lcores`、`tasklets_nb_per_sch`、`data_quota_mbs_per_sch`（见下文） |

满足「**收发都使用最多的 CPU**」且「**启用 PTPv2**」时，ST2110 收发性能最佳；编码侧由 encode_sdk 负责，时间戳与 ST2110 流同步即可。

---

## 二、上层应用如何使用 mtl_sdk

### 2.1 典型流程

1. **配置并创建 Context**（含 PTP、网卡、lcore/tasklet 等）
2. **start()** 启动 MTL
3. **创建会话**：`create_video_tx` / `create_video_rx`、`create_audio_tx` / `create_audio_rx`
4. **收发数据**：TX 侧 `put_video` / `put_audio`，RX 侧 `poll` + `release`
5. **stop()** 停止

### 2.2 配置结构：MtlSdkConfig

```cpp
#include "mtl_sdk/mtl_sdk.hpp"

mtl_sdk::MtlSdkConfig cfg;
cfg.ports.push_back({ "0000:04:00.0", "192.168.10.1" });  // 网卡 BDF + 本机 IP
cfg.tx_queues = 1;   // 发送队列数
cfg.rx_queues = 1;   // 接收队列数

// ---------- PTPv2（需求：收发均支持）----------
cfg.enable_builtin_ptp = true;   // 使用 MTL 内置 PTP 客户端（推荐）
// 或外部 PTP：cfg.ptp_mode = PtpMode::ExternalFn; cfg.external_ptp_time_fn = my_ptp_fn;

// ---------- “跑满网卡”/ 使用最多 CPU ----------
cfg.lcores = "0-3";              // DPDK 使用的 lcore 列表，如 "0-3" 或 "2,3,4,5"
cfg.main_lcore = 0;              // 主 lcore（可选，<0 表示 MTL 自动选）
cfg.tasklets_nb_per_sch = 16;    // 每个 scheduler 的 tasklet 数，0=自动（可试 16 提升吞吐）
cfg.data_quota_mbs_per_sch = 0;  // 每 lcore 数据配额 MB/s，0=自动

auto ctx = mtl_sdk::Context::create(cfg);
ctx->start();
```

- **PTP**：`enable_builtin_ptp = true` 即满足需求中的「视频流和音频流收发均支持 PTPv2」。
- **CPU/性能**：`lcores`、`tasklets_nb_per_sch`、`data_quota_mbs_per_sch` 用于让收发尽量占用更多 CPU、跑满网卡能力；示例程序通过命令行 `--lcores`、`--tasklets`、`--data-quota-mbs` 传入并写入 `MtlSdkConfig`。

### 2.3 发送端（TX）示例片段

```cpp
mtl_sdk::VideoFormat vf{ 1920, 1080, 59.94, mtl_sdk::VideoPixFmt::YUV422_10BIT };
mtl_sdk::St2110Endpoint vep{ "239.0.0.1", 5004, 96 };
auto v_tx = ctx->create_video_tx(vf, vep);

mtl_sdk::VideoFrame frame;
frame.fmt = vf;
frame.timestamp_ns = ctx->now_ptp_ns();  // PTP 时间戳（或外部 PTP 回调）
frame.planes[0].data = y_plane; frame.planes[1].data = u_plane; frame.planes[2].data = v_plane;
// ... 设置 linesize、num_planes、bytes_total

v_tx->put_video(frame);
```

音频 TX 类似：`create_audio_tx`，再 `put_audio(AudioFrame)`，时间戳同样建议用 PTP（`ctx->now_ptp_ns()` 或外部 PTP）。

### 2.4 接收端（RX）示例片段

```cpp
auto v_rx = ctx->create_video_rx(vf, vep);
mtl_sdk::VideoFrame frame;
if (v_rx->poll(frame, 10)) {
  // 使用 frame（含 PTP 时间戳 frame.timestamp_ns）
  v_rx->release(frame);
}
```

---

## 三、上层应用如何使用 encode_sdk

### 3.1 典型流程

1. **配置 EncodeParams**（视频/音频编码参数、封装格式、输出路径）
2. **Session::open(params)** 打开编码会话
3. **push_video** / **push_audio** 送入帧（帧的时间戳与 ST2110/PTP 同步）
4. **close()** 冲刷并写出文件

### 3.2 配置结构：EncodeParams

```cpp
#include "encode_sdk/encode_sdk.hpp"

encode_sdk::EncodeParams params;
params.video.codec = encode_sdk::VideoCodec::H264;
params.video.hw = encode_sdk::HwAccel::Auto;  // NVENC → QSV → 软件
params.video.bitrate_kbps = 8000;
params.video.gop = 60;
params.video.input_fmt = mtl_sdk::VideoPixFmt::YUV422_10BIT;  // 与 MTL RX 输出一致

params.audio = encode_sdk::AudioEncodeParams{};
params.audio->codec = encode_sdk::AudioCodec::AAC;
params.audio->bitrate_kbps = 192;

params.mux.container = encode_sdk::Container::MP4;
params.mux.output_path = "out.mp4";

auto session = encode_sdk::Session::open(params);
```

encode_sdk 的 API 面向**编码与封装**（码率、GOP、编码器、容器），**没有**“网卡 / lcore / tasklet”等参数；跑满网卡、占用更多 CPU 的配置在 **mtl_sdk** 的 `MtlSdkConfig` 中完成。

### 3.3 与 mtl_sdk 对接（时间戳同步）

接收端从 MTL 拿到带 PTP 时间戳的帧后，直接交给编码器即可，需求中的「时间戳与 ST2110 原始流同步」由帧上的 `timestamp_ns` 保证：

```cpp
// RX 线程
if (v_rx->poll(frame, 10)) {
  queue.push(FrameCopy::from(frame));
  v_rx->release(frame);
}
// 编码线程
session->push_video(copy.to_video_frame());  // copy 中保留 timestamp_ns
```

---

## 四、“跑满网卡”参数归属

| SDK | 是否有“跑满网卡”/ 多用 CPU 的参数 | 说明 |
|-----|-----------------------------------|------|
| **mtl_sdk** | **有** | `MtlSdkConfig::lcores`、`main_lcore`、`tasklets_nb_per_sch`、`data_quota_mbs_per_sch`。上层应用在创建 Context 时传入，即可让收发使用更多 lcore、更多 tasklet，提升吞吐、减轻 build timeout。 |
| **encode_sdk** | **无** | 仅编码/封装参数（码率、GOP、编码器、容器）。编码并行度由内部（如 NVENC/FFmpeg）决定，不提供“网卡占满”或 lcore 配置。 |

因此：**“跑满网卡”和“收发使用最多 CPU”只由 mtl_sdk 的配置控制**；encode_sdk 负责在收到 ST2110 流后高效编码并保持时间戳同步。

---

## 五、满足需求.md 的推荐用法小结

1. **PTPv2**  
   - 创建 `Context` 时设置 `cfg.enable_builtin_ptp = true`（或使用外部 PTP 回调）。  
   - 收发时间戳统一使用 PTP（`ctx->now_ptp_ns()` 或 `frame.timestamp_ns`）。

2. **收发使用最多 CPU、性能尽量快**  
   - 在 `MtlSdkConfig` 中设置 `lcores`（如 `"0-3"`）、`tasklets_nb_per_sch`（如 `16`）、必要时 `data_quota_mbs_per_sch`。  
   - 示例程序：`st2110_send` / `st2110_record` 通过 `--lcores`、`--tasklets`、`--data-quota-mbs` 传入；上层应用等价地写入 `MtlSdkConfig` 即可。

3. **编码与时间戳**  
   - 使用 encode_sdk 的 `Session::open` / `push_video` / `push_audio` / `close`，保证送入的帧带 PTP 派生时间戳，即可满足「时间戳与 ST2110 原始流同步」。

参考示例：`samples/st2110_send.cpp`（发送）、`samples/st2110_record.cpp`（接收 + 编码）。

---

## 六、音视频收发实例程序 av_txrx_demo

**av_txrx_demo** 为统一音视频收发示例，对应本文档中的 MtlSdkConfig 与流参数，**可指定文档内所有参数**（含 PTP、lcores、tasklets、data-quota、port、sip、tx/rx-queues、bind-numa 等），便于 DPDK 模式发送、多核跑满网卡。

- **编译**：与 st2110_send / st2110_record 一同生成，可执行文件为 `build/av_txrx_demo`。
- **模式**：`--mode send` 或 `--mode recv`。
- **发送默认 YUV**：发送模式下默认读取 **build/yuv420p10le_1080p.yuv**，可通过 `--url` 覆盖。

### 6.1 参数一览（与文档一致）

| 类别 | 参数 | 说明 |
|------|------|------|
| MtlSdkConfig | `--port` `--sip` `--tx-queues` `--rx-queues` | 端口（DPDK BDF 或 kernel:xxx）、本机 IP、收发队列数 |
| 跑满网卡/多用 CPU | `--lcores` `--main-lcore` `--tasklets` `--data-quota-mbs` | DPDK lcore 列表、主 lcore、tasklet 数、数据配额 |
| PTP | `--no-ptp` | 禁用 PTP（默认启用） |
| 流参数 | `--ip` `--video-port` `--audio-port` `--width` `--height` `--fps` | 组播 IP、端口、分辨率、帧率 |
| 仅 send | `--url` `--fmt` `--duration` `--sdp-out` `--put-retry` `--prefill-frames` | YUV 路径（默认 build/yuv420p10le_1080p.yuv）、格式、时长、SDP 导出、重试与预填帧 |
| 仅 recv | `--max-frames` `--sdp` `[output.mp4]` | 最大帧数、SDP 文件、输出路径 |

### 6.2 示例：DPDK 模式发送 build/yuv，指定 16 核

**发送端（A 机）：**

```bash
./av_txrx_demo --mode send \
  --port 0000:04:00.0 --sip 192.168.10.1 \
  --url build/yuv420p10le_1080p.yuv \
  --ip 239.0.0.1 --video-port 5004 --audio-port 0 \
  --width 1920 --height 1080 --fps 59.94 --duration 30 \
  --lcores 0-15 --tasklets 16 --no-ptp \
  --put-retry 200 --prefill-frames 8
```

**接收端（B 机）：**

```bash
./av_txrx_demo --mode recv \
  --port 0000:06:00.0 --sip 192.168.10.2 \
  --ip 239.0.0.1 --video-port 5004 --audio-port 0 \
  --width 1920 --height 1080 --fps 59.94 --max-frames 1800 \
  --lcores 0-15 --tasklets 16 --no-ptp \
  recv.mp4
```

若在项目根目录运行发送端，将 `--url` 改为 `build/yuv420p10le_1080p.yuv` 或使用绝对路径。完整选项见 `av_txrx_demo --help`。
