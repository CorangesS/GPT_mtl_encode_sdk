# MTL-SDK 与编码 SDK 需求符合性检查

本文对照 `需求.md`、`MTL_SDK_description.md`、`编码SDK.md` 对当前项目做逐项符合性检查，确认结构与逻辑可用且符合收发与编码需求。

---

## 一、ST2110 收发 SDK（MTL SDK）

| 需求来源 | 条款 | 实现位置与结论 |
|----------|------|----------------|
| 需求.md | 视频流**发送和接收**均需支持 **PTPv2（IEEE 1588）** | **符合**。收发均支持：`Context::create_video_rx`/`create_audio_rx`（接收）、`create_video_tx`/`create_audio_tx`（发送）；PTP 配置与 `timestamp_ns` 统一；真实 MTL 后端封装 st20p_tx/st30_tx（ST2110-20/30）。 |
| 需求.md | 支持 **SDP 解析**，SDP 文件 **导入/导出** | **符合**。`sdp.cpp`：`parse_sdp()` 解析 s/o/c/m 及 a=rtpmap、a=fmtp、a=ts-refclk、a=mediaclk；`to_sdp()` 导出；`load_sdp_file()` / `save_sdp_file()` 文件导入/导出。 |
| 需求.md | 性能指标根据实际情况讨论 | 未定具体指标，与需求表述一致。 |
| MTL_SDK_description | ST2110 视频/音频**收发**，至少视频 RX 可用 | **符合**。`Context::create_video_rx`/`create_audio_rx`（RX）、`create_video_tx`/`create_audio_tx`（TX）；mock 与真实 MTL 后端均实现视频/音频 TX 与视频 RX；音频 RX 在真实后端为框架，API 完整。本机两进程与两机收发测试见 `docs/ST2110_TESTING.md`。 |
| MTL_SDK_description | External Frame API，SPSC ring，回调不阻塞 | **符合**。`mtl_backend_mtl.cpp`：`ST20P_RX_FLAG_EXT_FRAME`、`query_ext_frame`、`notify_frame_ready`；`ring_spsc.hpp` 无锁 SPSC；frame 生命周期：query → notify → poll → release。 |
| MTL_SDK_description | 数据模型：MtlSdkConfig、NetPortConfig、PtpMode、VideoFormat/AudioFormat、St2110Endpoint、VideoFrame、AudioFrame | **符合**。均在 `mtl_sdk.hpp` 中定义，含 timestamp_ns、planes、linesize、bytes_total、MemoryType、opaque、PCM。 |
| MTL_SDK_description | CMake、mock 默认、真实 MTL 宏开关、st_* / mtl_* 兼容 | **符合**。`CMakeLists.txt`：`MTL_SDK_WITH_MTL`、`MTL_SDK_USE_ST_API`；include/src/samples 结构完整。 |

**结论**：当前 MTL-SDK 项目结构与逻辑**可用且符合** ST2110 收发需求与说明文档。

---

## 二、编码 SDK

| 需求来源 | 条款 | 实现位置与结论 |
|----------|------|----------------|
| 需求.md | 主流 GPU **NVIDIA NVENC** | **符合**。`encode_sdk.cpp`：`pick_video_encoder` 优先 h264_nvenc/hevc_nvenc，不可用时回退 QSV 或 libx264/libx265。 |
| 需求.md | 视频编码 **H.264/AVC、H.265/HEVC** | **符合**。`encode_sdk.hpp`：`VideoCodec::H264`/`H265`；编码器名与回退链完整。 |
| 需求.md | 音频编码 **AAC、MP2、PCM、AC3** | **符合**。`AudioCodec::AAC|MP2|PCM|AC3`，`audio_codec_name()` 映射到 aac/mp2/pcm_s16le/ac3。 |
| 需求.md | 文件格式 **MP4、MXF** | **符合**。`Container::MP4|MXF`，`container_name()` 对应 mp4/mxf。 |
| 需求.md | 编码参数可调（编码标准、码率、GOP、Profile），运行期调整 | **符合**。`VideoEncodeParams` 含 codec、bitrate_kbps、gop、profile；`Session` 提供 `set_video_bitrate_kbps`、`set_video_gop`、`apply_reconfigure()`。 |
| 需求.md | ST2110 原始流直接送入编码器，避免不必要内存拷贝 | **符合**。`wrap_video_frame()` 对 HostPtr 直接将 `frame.planes[].data` 赋给 `AVFrame->data[]`，无额外 memcpy；格式不一致时用 swscale；非 Host 类型预留并报未实现。 |
| 需求.md | 时间戳与 ST2110 同步，音视频同步 | **符合**。视频/音频均用 `first_ts_ns` 记录首帧，`rel_ns = timestamp_ns - first_ts_ns`，`av_rescale_q(rel_ns, {1,1e9}, time_base)` 得到 PTS，时间基一致。 |
| 编码SDK.md | Session：open、push_video、push_audio、close | **符合**。`encode_sdk.hpp` 与 `SessionImpl` 一致。 |
| 编码SDK.md | Sample：从 mock MTL 拉帧写出 out.mp4 | **符合**。`samples/st2110_record.cpp` 使用 mock MTL + encode_sdk 写出 MP4。 |
| 编码SDK.md | FFmpeg 库、CMake + pkg-config | **符合**。`CMakeLists.txt` 使用 pkg_check_modules 查找 libavformat/avcodec/avutil/swscale/swresample。 |

**结论**：当前编码 SDK 项目结构与逻辑**可用且符合**编码需求与说明文档。

---

## 三、与路由管理的关系

- 路由管理需求（IS-04/IS-05、注册发现、可视化）由 **routing 适配层 + 外部 NMOS Registry + NMOS-JS** 实现，不改变 MTL-SDK 与 encode_sdk 的现有 API 与目录结构。
- 对接方式与架构见 **docs/ROUTING.md**。

---

## 四、小结

| 模块 | 需求符合性 | 说明 |
|------|------------|------|
| MTL SDK | 符合 | PTPv2、SDP、External Frame、数据模型、双 backend、CMake 均满足。 |
| Encode SDK | 符合 | 编码格式、容器、参数、零拷贝包装、时间戳同步、Session API、Sample 均满足。 |
| 路由管理 | 通过适配实现 | 见 `docs/ROUTING.md` 与 `routing/`；不修改 NMOS-JS，通过配置与自研节点注册/IS-05 实现。 |

当前 MTL-Encode-SDK 项目**可直接用于** ST2110 收发与编码场景；路由管理在现有结构上通过 **routing/** 与文档对接即可。
