你是一个资深 C++/网络媒体系统工程师。请实现一个 “MTL SDK”（不包含路由管理/UI），用于封装 Media Transport Library (MTL) 的 ST 2110 收发能力，并对上层暴露稳定、易用的 API。

目标能力：
1) ST2110 视频/音频收发会话（至少视频 RX 必须可用；音频 RX 可实现为框架+TODO，但 API 要完整）
2) 支持 PTPv2/IEEE1588 时间：两种模式
   - Built-in PTP：通过 MTL_FLAG_PTP_ENABLE 或等效机制启用
   - External 时间源：应用提供 ptp_get_time_fn 回调
   对上层统一输出 timestamp_ns（PTP 纳秒）。
3) 支持 SDP 解析与导入/导出：
   - 解析 v/o/s/c/t/m/a=rtpmap/a=fmtp/a=ts-refclk/a=mediaclk
   - 能把内部会话配置导出成 SDP 文本，并能从 SDP 文本导入生成会话配置
4) 使用 External Frame API：让 MTL 直接写入应用提供的 buffer pool，减少一次 memcpy
   - 需要 lock-free / SPSC ring，MTL 回调线程不得阻塞
   - frame 生命周期：query_ext_frame 提供 buffer -> notify_frame_ready 推送索引 -> poll() 取帧 -> release() 归还

数据模型要求：
- MtlSdkConfig/NetPortConfig/PtpMode/PtpGetTimeFn
- VideoFormat(AudioFormat) + St2110Endpoint
- VideoFrame：timestamp_ns、planes、linesize、bytes_total、MemoryType、opaque(归还句柄)
- AudioFrame：timestamp_ns、PCM payload

工程要求：
- CMake 构建
- 默认提供 mock backend（不依赖真实 MTL），可生成合成 NV12 帧用于联调
- 提供真实 MTL backend 的实现文件，用宏开关编译（MTL_SDK_WITH_MTL），并兼容 st_* 与 mtl_* 两套 API 命名（通过 MTL_SDK_USE_ST_API 开关）

输出：
- 完整源码目录结构（include/src/samples）
- 可编译的 mock 模式 + 可落地改造的真实 MTL 模式
