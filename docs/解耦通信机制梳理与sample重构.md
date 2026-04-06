# 解耦 `mtl_sdk`（视频收发）与 `encode_sdk`（编解码）：通信机制定位 + B 端可选解码与进度条

## 1. 背景（当前是一体化工作流）
当前你的系统运行形态是：
- A 机器：只负责发送 YUV（这点保持不变）。
- B 机器：同时完成“接收 + 编解码”，并把接收结果转换为可指定的视频格式（你希望把这条链路拆开）。

你要实现的目标是“解耦”：
- 接收与发送链路仍然存在，但 **B 端接收 SDK 不再与编解码 SDK 绑定**；
- B 端在接收后可以“选择是否解码为对应视频格式”（也就是接收侧可单独运行；解码侧可插拔）；
- 增加“解码进度条”（用于可视化当前解码/编码推进程度），且进度更新不能影响实时接收。

## 2. 已知线索（来自现有 README，需要在代码中逐条核实）
对照代码（`mtl_sdk`/`encode_sdk`/`samples`），目前这些“线索”落到具体实现上如下（这是你文档里问题的答案依据）：
- `mtl_sdk`（`MtlVideoRx`）确实使用 MTL External Frame：`ops_.query_ext_frame`（填充 ext_frame 指针）+ `ops_.notify_frame_available`（通知帧就绪）+ 应用侧 `poll()` 取帧 + 调用方 `release()` 归还 buffer；
- `mtl_sdk` 的 **SPSC 无锁 ring** 实际用于：MTL 回调线程触发“帧就绪”时，把一个索引 `idx` 放入 `SpscRing<int> ready_`，让应用线程 `poll()` 非阻塞轮询/取出；
- `mtl_sdk` 的 `VideoFrame`/`AudioFrame` 都包含 `timestamp_ns`；
- `encode_sdk` 的确有 `wrap_video_frame(const VideoFrame&)`：对 `HostPtr` 输入创建 `AVFrame`，其 `data[]/linesize[]` 直接指向 `VideoFrame` planes（不在 `wrap_video_frame` 中拷贝）；
- `encode_sdk` 的编码入口是 `SessionImpl::push_video/push_audio`，并在 `push_video` 内部完成 `sws_scale`/NVENC 写入，函数返回前已经把输入数据用掉；
- 当前没有看到“跨进程共享内存 IPC（`shm_open/mmap`）”的实现：视频缓冲池在 `MtlVideoRx` 内部（`std::vector<uint8_t>`），通过指针传给 `VideoFrame` 再由应用在同进程调用 `encode_sdk`。

额外提醒：`mtl_backend_mtl.cpp` 里 `MtlAudioRx::poll` 当前是 stub（直接返回 `false`），所以“音频路径”在当前代码里并不完整；本次解耦主要围绕视频链路。

## 3. 必答问题：当前两个功能如何通信？
你需要我们明确“现在是怎么通信的”，并在解耦时给出替换点与契约。下面把问题拆成三类，并直接给出基于代码的结论（同时指出耦合点在哪里）。

### 3.1 数据通路（Data Plane）：帧是怎么从接收进入编解码的？
1. 当前 B 端视频接收->编码的典型链路（`samples/is05_receiver_daemon.cpp`）是：应用线程 `video_rx->poll(frame)` 取到 `VideoFrame`，然后**直接同步**调用 `enc->push_video(frame)`，接着由应用调用 `video_rx->release(frame)`。
2. `encode_sdk` 的 `wrap_video_frame` 对 `HostPtr` 输入只创建 `AVFrame` 指向 planes 指针，并在 `push_video` 内部完成 `sws_scale`/NVENC 编码写出；因此 `push_video()` 返回前输入数据已被消费。
3. 因此，当前并不是“共享内存/跨进程 IPC”传递视频帧；唯一的“环形缓冲”是 `mtl_sdk` 内部用来在 MTL 回调线程与应用 `poll` 线程之间传递就绪索引 `idx`。

### 3.2 生命周期契约（Ownership/Lifecycle）：谁负责 release？
1. `release()` 由**调用方（接收侧/应用侧）**触发，而不是由 `encode_sdk` 触发：`Context::VideoRxSession::release(VideoFrame&)` 只是把 buffer index 归还给 MTL RX 的 `free_` 池，并调用 `st20p_rx_put_frame`。
2. 在 `MtlVideoRx` 中，如果不调用 `release()`，`free_` 会耗尽（`query_ext_frame` 返回 -1），导致 `poll()` 获取不到新帧；这就是“生命周期契约”的硬约束。
3. 在 `samples/is05_receiver_daemon.cpp` 中，`release()` 的顺序是固定的：`push_video(frame)` 返回后立刻 `release(frame)`（同线程同步消费契约）。

### 3.3 控制通路（Control Plane）：启动/停止/错误如何联动？
1. `routing`（`routing/is05_server/app.py`）不会传输视频帧；它通过 HTTP/IS-05 API 把连接参数写入文件 `connection_state.json`（其中包含 `video`/`audio` 子对象）。
2. `samples/is05_receiver_daemon.cpp` 在主循环里周期读取 `connection_state.json`，检测到 `state_changed && state.valid` 后才会创建/重置 `mtl_sdk` 的 `video_rx/audio_rx` 以及 `encode_sdk::Session`；当 `state.valid` 变为 false 时销毁这些对象并调用 `enc->close()`。
3. 在视频解码阶段中（当前 sample 行为），退出条件是由 `run_frames`（`--frames`）控制：达到指定帧数后 `enc->close()`，随后重置 `enc`。

## 4. 目标架构（解耦后的模块与可插拔接口）
解耦目标是让每个单元独立，不互相影响。建议把 B 端拆成下面几类单元，并用统一契约连接：

### 4.1 单元划分
1. `St2110Sender`（A 端）
   - 输入固定为 YUV（保持你现有能力与命令形态，sample 只负责发送）。
2. `St2110Receiver`（B 端）
   - 只负责从 MTL 接收并输出“帧句柄/帧对象”给下游；
   - 不关心要不要解码，也不关心输出封装格式。
3. `FrameTransport`（连接层，可替换实现）
   - 把 `St2110Receiver` 产生的帧交给下游；
   - 第一步建议提供“进程内实现”（基于现有 ring/queue 机制抽象），后续如果你要跨进程再引入共享内存/IPC。
4. `DecoderPipeline`/`EncoderPipeline`（B 端可选）
   - 只消费来自 `FrameTransport` 的帧句柄；
   - 对外提供：`start(params)`、`push(frame)`、`stop()`；
   - 由参数决定“是否做转码/解码到指定视频格式”。
5. `OutputWriter`（可选落盘）
   - 负责写 MP4/MXF 等封装（若你把“写封装”和“编码”分离，会更容易做单元测试）。
6. `Orchestrator`（sample/应用层）
   - 负责把 Receiver 和（可选）DecoderPipeline 拼起来；
   - 负责解析命令行选项：例如 `--decode` / `--output-format` / `--max-frames` 等。

### 4.2 通信接口契约（Contract）
为避免耦合，建议在接口层明确这些内容：
1. 帧输入契约（Receiver -> DecoderPipeline）
   - `FrameTransport` 对外只暴露 `IFrameSource`（拉取）或 `IFrameSink`（推送）之一；
   - 帧句柄包含：plane 访问、格式信息、`timestamp_ns`、以及明确的 `release()` 语义。
2. 错误与生命周期契约
   - 每个单元都有 `start/stop`；
   - 失败时通过统一错误类型与状态通道通知 orchestrator，避免“单元之间直接调用对方内部资源”。

## 5. 解耦工程化实施步骤（按你要求的“详细步骤 + 可工程化”写）
下面是建议的执行顺序，每一步都能验证“解耦是否真的生效”。

### 5.1 第一步：确认当前通信机制（必须完成）
现在你文档里这一步已经可以给出“代码结论”（后续解耦时要围绕这些契约改造）：
1. **帧在 B 端通过函数调用同步传递**：`poll()` 得到 `VideoFrame` 后直接 `enc->push_video(frame)`；
2. **`release()` 由接收侧应用调用**：在 `push_video()` 返回后立即 `release(frame)`；
3. **`wrap_video_frame` 运行在编码函数内部**：`push_video()` 调用 `wrap_video_frame()` 并在返回前完成读取/转换/写出；
4. `mtl_sdk` 内部 `SpscRing<int>` 仅用于 `MTL 回调线程 -> 应用 poll 线程` 的“帧就绪索引通知”，不是跨模块通信队列。

验收标准（以“代码证据”为准）：
- 能写出一条清晰链路图：`MTL Rx(External Frame + ready_ ring) -> VideoFrame(planes指针) -> encode_sdk::push_video(wrap_video_frame读取) -> app侧release()`。

### 5.2 第二步：提取连接层（FrameTransport/Adapter）
实现一个“连接层抽象”，把 receiver 与 encoder 的关系从“硬编码耦合”变为“接口契约”。结合当前代码，解耦必须显式处理两点：
1. **同步消费契约是当前耦合点**：因为当前是 `push_video()` 同步调用 + `release()` 紧跟其后，如果后续把解码放到独立线程，必须避免在解码完成前把 MTL buffer 归还；
2. 解耦的可选实现路线：
   - 路线 A（最接近现有 sample 的做法）：像 `samples/st2110_record.cpp` 一样在 Receiver 侧把 planes **拷贝**到 `FrameCopy`（独立内存），然后立刻 `release()`，再把拷贝帧交给编码线程；
   - 路线 B（更复杂但可减少拷贝）：在 FrameTransport 里实现“引用计数/共享缓冲所有权”，让 `release()` 只能在解码端完成后发生。

建议你优先走路线 A（工程化最快、契约最清晰），再视性能再升级路线 B。

### 5.2.1 长时间“只收不解码”的建议方案：环形切片落盘（Ring Slice Store）
如果目标不是“实时接收 + 实时编码”，而是 **B 端长时间只接收、暂不解码、稍后统一处理**，那么不建议继续沿用 `FrameCopy` 小队列；更合适的做法是增加一个 **FrameStore/切片落盘层**，把接收到的原始帧按时间切成多个文件，写满后继续写新的切片，并按容量上限回收最旧切片。

#### 方案定位
1. `Receiver` 线程职责固定为：`poll(frame) -> 写入当前切片 -> release(frame)`；
2. `DecoderPipeline`/`EncoderPipeline` 不再是接收链路的必选项，而是后处理消费者；
3. 切片文件是“接收阶段的持久化缓存”，不是最终交付文件；最终交付文件仍由后处理阶段生成。

#### 目录结构建议
建议为每一路接收任务使用一个独立根目录，例如：

```text
ring_store/
  channel_main/
    manifest.json
    state/
      writer_state.json
      recycler_state.json
      decoder_state.json
    slices/
      20260331T153000Z_000001/
        slice.json
        video.frames
        audio.frames
        index.bin
      20260331T153100Z_000002/
        slice.json
        video.frames
        audio.frames
        index.bin
```

各文件/目录含义建议如下：
1. `manifest.json`
   - 保存该路任务的固定信息：`channel_id`、视频格式、音频格式、切片时长、容量上限、命名版本等。
2. `state/`
   - 保存运行状态与恢复信息，便于进程异常退出后恢复。
3. `slices/<slice_id>/`
   - 每个切片一个目录，便于原子落盘、重命名、删除和状态隔离。
4. `video.frames`
   - 顺序写入原始视频帧数据（建议保留 plane layout 与 timestamp）。
5. `audio.frames`
   - 顺序写入原始音频帧/块（若当前阶段不处理音频，可先预留）。
6. `index.bin`
   - 帧索引文件；记录每帧偏移、长度、时间戳、关键元信息，便于后处理按时间快速定位。
7. `slice.json`
   - 当前切片的元数据与生命周期状态。

#### 切片命名规则
建议切片目录名使用：

```text
<UTC开始时间>_<递增序号>
```

示例：

```text
20260331T153000Z_000001
20260331T153100Z_000002
```

这样做的好处：
1. 肉眼可直接看出切片起始时间；
2. 同一秒内仍可通过递增序号避免重名；
3. 删除最旧切片时，可按名称排序近似得到时间顺序。

#### 状态文件命名与职责
建议至少维护 3 个状态文件：

1. `state/writer_state.json`
   - 记录当前写入中的切片；
   - 字段建议包括：`active_slice_id`、`opened_at_ns`、`last_video_ts_ns`、`last_audio_ts_ns`、`video_frames_written`、`bytes_written`、`rotate_reason`。

2. `state/recycler_state.json`
   - 记录回收器视角的容量信息；
   - 字段建议包括：`total_bytes`、`retention_bytes_limit`、`retention_slice_limit`、`oldest_slice_id`、`newest_slice_id`、`last_recycle_at_ns`。

3. `state/decoder_state.json`
   - 记录后处理进度，避免“解码到一半的切片”被误删；
   - 字段建议包括：`current_slice_id`、`current_frame_index`、`last_committed_slice_id`、`mode`（idle/running/drain）、`lease_expires_at_ns`。

命名原则：
1. `writer_*` 只由接收/落盘进程更新；
2. `decoder_*` 只由后处理进程更新；
3. `recycler_*` 只由回收器更新；
4. 不同职责拆文件，避免多个进程频繁覆盖同一状态文件。

#### 切片状态机建议
`slice.json` 建议包含 `status` 字段，状态至少包括：
1. `writing`
   - 正在写入，禁止回收，后处理一般也不直接消费。
2. `sealed`
   - 已封口，表示当前切片不再写入，可供后处理消费。
3. `processing`
   - 已被后处理进程领取，正在解码/转码。
4. `processed`
   - 已完成后处理。
5. `recyclable`
   - 允许被回收器删除。
6. `corrupted`
   - 异常退出后恢复时发现不完整，需要修复、跳过或丢弃。

推荐规则：
1. 切片写满或到达时间阈值后，从 `writing` 原子切换到 `sealed`；
2. 后处理领取时切换到 `processing`；
3. 后处理成功后切换到 `processed` 或直接标记 `recyclable`；
4. 恢复逻辑中，旧的 `writing` 切片若没有正常封口，改成 `corrupted` 或尝试补封。

#### 回收规则建议
环形切片的核心是“**总容量受控，最旧优先回收**”。建议同时设置多个阈值：

1. `retention_bytes_limit`
   - 目录总容量上限，例如 `500GB`；
2. `retention_slice_limit`
   - 最多保留切片数，例如 `1440` 个（若每片 1 分钟则约 24 小时）；
3. `min_reserved_slices`
   - 即便容量吃紧，也至少保留最近 N 个切片，避免过度删除；
4. `min_unprocessed_slices`
   - 尚未处理的切片至少保留 N 个，防止后处理落后时全部被回收。

推荐删除顺序：
1. 只从 `recyclable` 或 `processed` 状态中挑选；
2. 按切片时间从旧到新删除；
3. 删除前校验该切片 **不是** `writer_state.active_slice_id`；
4. 删除前校验该切片 **不是** `decoder_state.current_slice_id`，且没有有效 lease；
5. 每删一个切片就更新 `recycler_state.json`。

不建议的删除策略：
1. “解码完立即删除”作为唯一规则；
2. 不区分 `processing`/`sealed`/`writing` 就直接删最老目录；
3. 由解码器直接 `rm -rf` 切片目录而不经过统一回收器。

#### 是否“解码之后立即删除切片”
不建议把“解码完成后立即删除”设为硬规则，更推荐：
1. **默认标记为 `processed` 或 `recyclable`**，由统一回收器按容量策略删除；
2. 若业务明确要求“处理后不保留原始帧”，可以提供 `delete_after_processed=true` 选项，但仍应通过回收器执行删除；
3. 若存在“复检/重跑/重新转码”需求，处理完成后应保留一段缓冲窗口，而不是立即删除。

理由是：
1. 后处理成功不代表最终产物已被上游系统确认接收；
2. 立即删除会让重试、补跑、排障变困难；
3. 统一回收器更容易保证“不会删掉正在写/正在处理/尚未提交结果”的切片。

#### 推荐的落地顺序
1. 第一步：先实现“切片目录 + `slice.json` + `writer_state.json`”；
2. 第二步：实现按时间或按大小封片（例如每 60 秒一个切片）；
3. 第三步：实现离线解码器读取 `sealed` 切片并更新 `decoder_state.json`；
4. 第四步：最后再加回收器，按容量策略回收 `recyclable` 切片。

验收标准：
- `DecoderPipeline` 可以用“文件帧/测试帧源”跑（不链接 `mtl_sdk`），证明其真正独立。

### 5.3 第三步：重写 sample（按 A/B 分离的运行形态）
目标是让 sample 体现“单元化”而不是“收发+编码耦合在一个程序里”：
- A 端 sample：只做 YUV -> ST2110 send（不再包含任何 encode/封装逻辑）。
- B 端 sample：拆成两个独立命令，而不是继续把“接收”和“解码”挂在同一个命令参数下：
  - `st2110_receive_store`：只做 ST2110 receive，并写入 ring slice；
  - `slice_decode`：离线读取 ring slice，启动 decoder/encoder pipeline，并选择输出格式。

当前落地形态：
1. `st2110_send`
   - 保持发送端职责不变：只发送 YUV/音视频到 ST2110。
2. `st2110_receive_store`
   - 只负责 `poll(frame) -> FramePacket::from(frame) -> release(frame) -> RingSliceStore::write_*()`；
   - 不链接 `encode_sdk`，不写 MP4/MXF。
3. `slice_decode`
   - 只负责读取 `manifest.json` + `slices/*/index.bin` + `video.frames`；
   - 将恢复出的 `VideoFrame` 送入 `encode_sdk::Session`，输出 `mp4/mxf`。
4. `st2110_record`
   - 保留为兼容 sample；后续文档与测试流程不再主推它作为“彻底解耦”的入口。

验收标准：
- `st2110_receive_store` 可以独立完成“纯接收并存切片”；
- `slice_decode` 可以在无 `mtl_sdk` 收流参与的情况下完成离线编码；
- 两个命令只通过切片文件目录通信，不通过进程内函数调用通信。

### 5.4 第四步：加入“解码进度条”（非阻塞、准确可控）
进度条建议基于“已处理帧数/已编码帧数”而不是每个 packet 字节数，原因是帧级语义更接近输入节奏。实现要点：
1. 进度数据源：
   - 若命令行提供 `--max-frames`：进度条直接用 `processed_frames / max_frames`；
   - 若只提供 duration：用 `duration * fps` 或使用时间戳跨度估算；
   - 若输入未知：给“已接收/已编码数量”的滚动式统计（进度百分比可缺省）。
2. 更新策略：
   - 采用原子计数器 + 定期刷新（例如每 200ms 刷新一次）；
   - 刷新在独立线程或定时器中完成，避免影响接收/编码线程吞吐。
3. 停止条件：
   - 编码结束（flush + close）后停止进度条，并输出最终统计。

验收标准：
- 开启进度条不会显著降低吞吐或引入丢帧；
- 进度条与最终输出帧数保持一致（在可预期场景下，如 `--max-frames`）。

### 5.5 第五步：综合验证（回环 + 双机）
建议用以下场景验证解耦后仍然正确：
- 本机回环：A 端 send + B 端 recv（可选 decode）；
- 双机 Kernel 模式：A send YUV -> B recv（可选 decode）。

验收标准：
- 时间戳同步与音视频对齐（如适用）与原行为一致；
- 不发生内存泄漏/崩溃（重点关注 release 与线程结束顺序）。

## 6. 后续我们会用到的“代码定位清单”
为了后续你让我“查看通信机制（共享内存还是其他）”时能快速给出结论，我会按以下清单去定位：
- `mtl_sdk`：External Frame 的 `query/notify/poll/release` 在何处触发；
- `mtl_sdk`：SPSC ring/queue 的创建与入队/出队位置；
- `encode_sdk`：`wrap_video_frame` 被调用点、`push_video/push_audio` 的线程归属；
- sample：当前“接收+编码一体”的主循环/回调结构；
- 是否出现共享内存 API（`shm_open/mmap` 等）或是否仅是进程内队列。

## 7. 文档索引
该文档将作为后续你要求的“通信机制定位 + 工程化解耦 + 重写 sample + 进度条实现”的总纲。


现在该怎么用
1. 接收并存切片
./st2110_receive_store \
  --store-root ./ring_store \
  --channel-id channel_main \
  --session-id run_a \
  --ip 239.0.0.1 --video-port 5004 --audio-port 0 \
  --max-frames 598 --port kernel:lo --no-ptp --progress

2.

3. 离线读取切片并输出 MP4
./slice_decode \
  --store-root ./ring_store \
  --channel-id channel_test2 \
  --session-id run_a \
  --progress recv_2.mp4

4.播放视频
ffplay -autoexit /home/dd/GPT_mtl_encode_sdk/build/recv_1.mp4



598 帧发完就停、仍设一个上限防误跑（推荐）：
./st2110_receive_store \
  --store-root ./ring_store \
  --channel-id channel_test3 \
  --ip 239.0.0.1 --video-port 5004 --audio-port 0 \
  --max-frames 100000 \
  --idle-exit-ms 2000 \
  --port kernel:lo --no-ptp --progress

发端停约 2 秒后进程会退出，并打印 Stored 598 frames ... (stopped: no video for 2000 ms)（帧数以实际为准）。
不设帧数上限，只靠发端停：
./st2110_receive_store \
  --store-root ./ring_store \
  --channel-id channel_test3 \
  ... \
  --max-frames 0 \
  --idle-exit-ms 2000 \
  ...

  ffplay -autoexit /home/dd/GPT_mtl_encode_sdk/build/recv_run_01.mp4

  ./slice_decode \
  --store-root ./ring_store \
  --channel-id channel_test3 \
  --progress recv_test5.mp4

  ./st2110_receive_store \
  --store-root ./ring_store \
  --channel-id channel_test2 \
  --session-id run_20260406_b \
  --ip 239.0.0.1 --video-port 5004 --audio-port 0 \
  --max-frames 100000 --idle-exit-ms 2000 \
  --port kernel:lo --no-ptp --progress


#不要session_id,默认最新id
  ./slice_decode \
  --store-root ./ring_store \
  --channel-id channel_test2 \
  --progress recv_b.mp4

#视频保存地址
  ./slice_decode \
  --store-root ./ring_store \
  --channel-id channel_test2 \
  --session-id run_1 \
  --progress /home/dd/GPT_mtl_encode_sdk/out/recv_run1.mp4