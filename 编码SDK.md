你是一个资深 C++/FFmpeg/硬件编码工程师。请实现一个“编码 SDK”（不包含路由管理/UI），用于把来自 ST2110 的原始音视频帧编码并封装为文件。

输入来自上层（MTL SDK）：
- VideoFrame：planes 指针 + linesize + width/height/pix_fmt + timestamp_ns(PTP 纳秒)
- AudioFrame：PCM interleaved bytes + sample_rate/channels/bits_per_sample + timestamp_ns

功能要求：
1) 视频编码：H.264/AVC 与 H.265/HEVC
2) GPU 支持：优先支持 NVIDIA NVENC 与 Intel QSV；若不可用，自动回退软件编码（libx264/libx265）
3) 音频编码：AAC、MP2、PCM、AC3
4) 容器：MP4、MXF
5) 参数：编码标准、码率、GOP、Profile 可配置；提供运行期调整接口
6) 数据拷贝策略：
   - CPU 内存层面：不要把 VideoFrame 再 memcpy 一次；直接把 planes 包装成 AVFrame
   - 若输入 pix_fmt 与编码器要求不同，允许 swscale 转换（可配置是否启用）
   - 为未来 GPU 真零拷贝预留 MemoryType=DMABUF/CUDA hooks（默认实现可先报未实现或走最可移植路径）
7) 时间戳同步：
   - 使用 timestamp_ns 作为 PTS 来源：以首帧为零点，转换到各自 time_base
   - 保证音视频 pts 在同一时间基下对齐，A/V 同步稳定

工程要求：
- 使用 libavformat/libavcodec/libavutil/libswscale/libswresample
- CMake + pkg-config 查找 FFmpeg
- 提供 Session 类：open(params)、push_video(frame)、push_audio(frame)、close()
- 提供 sample：从 mock MTL 拉帧并写出 out.mp4

输出：
- 完整源码目录结构与实现
