# 需求2：编码 SDK — 部署与使用

对应 **需求.md**：H.264/H.265、AAC/MP2/PCM/AC3，MP4/MXF，编码参数可调，时间戳与 ST2110 同步。本文说明**编码 SDK 如何被使用**（API 流程、参数、与收流对接）。

---

## 一、编码 SDK 在项目中的位置

- **encode_sdk** 提供编码与封装，**不包含**网卡、组播、Registry 等；通常与 **mtl_sdk** 的接收端配合：先由 MTL 收流得到 `VideoFrame`/`AudioFrame`，再送入 encode_sdk 编码并写出文件。
- 示例程序 **st2110_record**：MTL RX 收流 → 每帧 `push_video`/`push_audio` → `Session::close()` 写出 MP4/MXF，即「编码 SDK 如何被使用」的完整示例。

---

## 二、典型使用流程

1. **配置 EncodeParams**（视频/音频编码格式、码率、GOP、容器、输出路径）
2. **Session::open(params)** 打开编码会话
3. **push_video** / **push_audio** 送入帧（帧须带与 ST2110/PTP 同步的 `timestamp_ns`）
4. **close()** 冲刷并写出文件

---

## 三、配置结构：EncodeParams

```cpp
#include "encode_sdk/encode_sdk.hpp"

encode_sdk::EncodeParams params;

// 视频：H.264/H.265，码率、GOP、输入像素格式（与 MTL RX 输出一致）
params.video.codec = encode_sdk::VideoCodec::H264;  // 或 H265
params.video.hw = encode_sdk::HwAccel::Auto;        // NVENC → QSV → 软件
params.video.bitrate_kbps = 8000;
params.video.gop = 60;
params.video.profile = "high";                       // 可选
params.video.input_fmt = mtl_sdk::VideoPixFmt::YUV422_10BIT;

// 音频：AAC/MP2/PCM/AC3
params.audio = encode_sdk::AudioEncodeParams{};
params.audio->codec = encode_sdk::AudioCodec::AAC;
params.audio->bitrate_kbps = 192;

// 封装：MP4 或 MXF，输出路径
params.mux.container = encode_sdk::Container::MP4;   // 或 MXF
params.mux.output_path = "out.mp4";

auto session = encode_sdk::Session::open(params);
```

- **需求对应**：视频 H.264/H.265、音频 AAC/MP2/PCM/AC3、容器 MP4/MXF、码率/GOP/Profile 可调，均由 EncodeParams 与 Session API 提供。
- **硬件**：`HwAccel::Auto` 时优先 NVENC，不可用时回退 QSV 或软件编码。

---

## 四、与 mtl_sdk 对接（时间戳同步）

接收端从 MTL 拿到带 PTP 时间戳的帧后，直接交给编码器即可；需求「时间戳与 ST2110 原始流同步」由帧上的 **timestamp_ns** 保证：

```cpp
// RX 线程：从 MTL 收帧
if (v_rx->poll(frame, 10)) {
  queue.push(FrameCopy::from(frame));  // 拷贝并保留 timestamp_ns
  v_rx->release(frame);
}
// 编码线程：送入 encode_sdk
session->push_video(copy.to_video_frame());
```

编码器内部按帧的 timestamp 计算 PTS，与 ST2110 流同步。

---

## 五、运行期参数调整

- **码率**：`session->set_video_bitrate_kbps(rate);` 后调用 `session->apply_reconfigure();`
- **GOP**：`session->set_video_gop(gop);` 后 `apply_reconfigure();`

---

## 六、直接运行示例（无需改代码）

**st2110_record** 已集成 MTL 收流 + encode_sdk 编码，直接运行即「编码 SDK 被使用」的完整流程：

```bash
cd build
./st2110_record --ip 239.0.0.1 --video-port 5004 --audio-port 0 \
  --width 1920 --height 1080 --max-frames 600 recv.mp4 --port kernel:lo
```

输出 `recv.mp4` 为 H.264 + AAC 的 MP4（编码器选择见启动日志，如 `encode_sdk: using h264_nvenc`）。若要 MXF 或改码率/GOP，需在 st2110_record 源码中设置对应 EncodeParams 或通过现有命令行参数（若已暴露）。

---

## 七、小结

| 需求项 | 使用方式 |
|--------|----------|
| H.264/H.265、AAC/MP2/PCM/AC3 | EncodeParams 中设置 `video.codec`、`audio->codec` |
| MP4/MXF | `params.mux.container`、`params.mux.output_path` |
| 编码参数可调 | `bitrate_kbps`、`gop`、`profile`；运行期 `set_video_bitrate_kbps`、`set_video_gop`、`apply_reconfigure` |
| 时间戳与 ST2110 同步 | 送入 `push_video`/`push_audio` 的帧保留 MTL 的 `timestamp_ns` |
| 零拷贝/硬件编码 | `HwAccel::Auto`，与 MTL 的 HostPtr/CUDA 帧对接见 SDK 与 st2110_record 示例 |

参考实现：**samples/st2110_record.cpp**。
