# 自研节点 IS-05 服务端实现详解

本文说明如何在自研节点上实现 **IS-05 Connection API 服务端**，使 NMOS Controller 上的「Sender → Receiver 连接」能真正驱动 MTL SDK 的收流（创建/更新 `St2110Endpoint` 与 RX 会话）。

---

## 一、IS-05 与自研节点的关系

- **IS-05** 定义的是「设备连接管理」：Controller 通过 HTTP 对**节点**下发「把某 Receiver 接到某 Sender 的流」的指令。
- **谁调谁**：Controller（或通过 Registry 解析 Node 的 `api.endpoints`）向 **Node 的 Base URL**（即注册时填的 `href`，如 `http://本机IP:9090/`）发起 HTTP 请求，访问的是 **Node 上运行的 Connection API**，而不是 Registry。
- **自研节点** 必须：
  1. 在 `href` 指向的地址上起一个 **HTTP 服务**；
  2. 实现 IS-05 规定的 **Connection API**（见下节路径与方法）；
  3. 在收到 PATCH「激活 Receiver」时，解析传输参数，并调用 **MTL SDK** 的 `create_video_rx` / `create_audio_rx`（或更新已有会话）。

因此：**IS-05 服务端 = 本机 HTTP 服务 + 对 PATCH 的处理逻辑 + 对 MTL Context/RX 的创建或更新**。

---

## 二、Connection API 必须实现的端点（Receiver 侧）

IS-05 规定 Node 暴露 **Connection API**，常用为 **single** 模式。以下路径均相对于 Node 的 Base URL（例如 `http://192.168.1.100:9090/`），且规范中通常带版本前缀（如 `x-nmos/connection/v1.1`）。实际以 AMWA 规范为准，这里以 v1.1 为例。

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `.../x-nmos/connection/v1.1/single/receivers` | 返回 receiver 列表（含尾部 `/` 的 ID） |
| GET | `.../x-nmos/connection/v1.1/single/receivers/{receiverId}/` | 该 receiver 的子资源列表 |
| GET | `.../x-nmos/connection/v1.1/single/receivers/{receiverId}/staged` | 当前「待生效」的传输参数 |
| **PATCH** | `.../x-nmos/connection/v1.1/single/receivers/{receiverId}/staged` | **下发传输参数并可选立即激活** |
| GET | `.../x-nmos/connection/v1.1/single/receivers/{receiverId}/active` | 当前已生效的传输参数 |
| GET | `.../x-nmos/connection/v1.1/single/receivers/{receiverId}/constraints` | 传输参数约束（可选，可返回空数组） |
| GET | `.../x-nmos/connection/v1.1/single/receivers/{receiverId}/transporttype` | 传输类型 URN（如 `urn:x-nmos:transport:rtp.ucast`） |

**核心**：Controller 在用户点击「连接」时，会向 **PATCH `.../receivers/{receiverId}/staged`** 发送 JSON 请求体；节点需解析该 body，并在 `activation.mode == "activate_immediate"` 时把参数应用到 MTL 并返回 200。

---

## 三、PATCH /staged 的请求体格式

Controller 可能发送两种常见形态（可同时出现）：

### 3.1 使用 transport_params（RTP 单播/组播）

```json
{
  "sender_id": "5709255c-c0ae-4e1e-99a0-e872e83e48e0",
  "master_enable": true,
  "activation": { "mode": "activate_immediate", "requested_time": null },
  "transport_params": [{
    "interface_ip": "192.168.200.15",
    "multicast_ip": "232.105.26.177",
    "source_ip": "192.168.200.10",
    "destination_port": 5000,
    "rtp_enabled": true
  }]
}
```

- **multicast_ip**（或 destination_ip）：目标组播/单播 IP → 对应 MTL `St2110Endpoint::ip`。
- **destination_port**：目标端口 → `St2110Endpoint::udp_port`。
- **interface_ip**：本机接收用的接口 IP，MTL 侧用 `MtlSdkConfig::ports[].sip` 或绑定网卡时使用。
- **source_ip**：发送端源 IP，部分场景用于过滤或显示；MTL 创建 RX 时可不填。

### 3.2 使用 transport_file（SDP）

```json
{
  "sender_id": "5709255c-c0ae-4e1e-99a0-e872e83e48e0",
  "master_enable": true,
  "activation": { "mode": "activate_immediate", "requested_time": null },
  "transport_file": {
    "data": "v=0\r\no=- 1496222842 1496222842 IN IP4 172.29.226.25\r\ns=...\r\nm=video 5010 RTP/AVP 103\r\nc=IN IP4 232.250.98.80/32\r\n...",
    "type": "application/sdp"
  }
}
```

- **transport_file.data**：完整 SDP 文本。可用本项目 **mtl_sdk** 的 `parse_sdp(data)` 得到 `SdpSession`，再遍历 `sdp.media` 得到每条 `SdpMedia` 的 `endpoint`（ip / udp_port / payload_type）以及从 `fmtp_kv`/`rtpmap` 解析出 width、height、fps、采样率等，映射到 `VideoFormat` / `AudioFormat` 与 `St2110Endpoint`。

无论哪种形式，**激活（activate_immediate）时**：用解析出的 `St2110Endpoint` + `VideoFormat`/`AudioFormat` 调用 MTL 的 `create_video_rx` / `create_audio_rx`，或先释放旧会话再创建新会话（即「更新」收流）。

---

## 四、传输参数到 MTL 的映射

| IS-05 / SDP | MTL 类型 / 字段 |
|-------------|------------------|
| transport_params[].multicast_ip 或 destination_ip | `St2110Endpoint::ip` |
| transport_params[].destination_port | `St2110Endpoint::udp_port` |
| RTP payload type（SDP 或默认 96） | `St2110Endpoint::payload_type` |
| SDP fmtp width/height、exactframerate | `VideoFormat::width`、`height`、`fps` |
| SDP 视频 sampling（如 YCbCr-4:2:2） | `VideoFormat::pix_fmt`（如 YUV422_10BIT） |
| SDP 音频 rtpmap（如 L24/48000/2） | `AudioFormat::sample_rate`、`channels`、`bits_per_sample` |

- 若只有 **transport_params** 而无 SDP：可只填 `St2110Endpoint`（ip、destination_port、payload_type 默认 96），`VideoFormat` 使用默认（如 1920x1080、59.94、YUV422_10BIT），或从 Controller 下发的其它字段扩展。
- 若有 **transport_file.data**（SDP）：优先用 **mtl_sdk::parse_sdp** 得到 `SdpSession`，再按 `sdp_to_session_test.cpp` 的方式从 `m.fmtp_kv`、`m.rtpmap` 解析出宽高、帧率、音频参数，与 `m.endpoint` 一起填到 `VideoFormat`、`AudioFormat`、`St2110Endpoint`。

---

## 五、实现形态建议

### 5.1 单进程（推荐）

- **一个进程**内同时：
  - 运行 **HTTP 服务器**（暴露 IS-05 Connection API）；
  - 持有 **mtl_sdk::Context** 及当前的 **VideoRxSession** / **AudioRxSession**（可为一对一：一个 Receiver ID 对应一组 video_rx + 可选 audio_rx）。
- 收到 PATCH 且 `activation.mode == "activate_immediate"` 时：
  - 解析 `transport_params` 或 `transport_file.data`；
  - 若已有该 receiverId 的 RX 会话，先释放再创建（或按实现支持「更新」）；
  - 调用 `ctx->create_video_rx(vf, ep)`（及可选 `create_audio_rx`），保存返回的 `unique_ptr`；
  - 将当前参数存为「active」状态，GET `/active` 时返回该状态；
  - PATCH 响应返回 200 及完整 staged 状态（含 transport_params 等）。

**优点**：逻辑简单、无跨进程通信；MTL Context 与 RX 生命周期清晰。  
**缺点**：收流与编码若要在同一进程内做，需在该进程内再接 **encode_sdk**（如 st2110_record 的循环：poll → push_video → release）。

### 5.2 与 st2110_record 分离（多进程）

- **进程 A**：只跑 **IS-05 HTTP 服务**，不持 MTL；收到 PATCH 后把「要收的 endpoint + 格式」通过 **本地配置/IPC/共享文件** 发给进程 B。
- **进程 B**：跑 **st2110_record** 或类似逻辑，轮询配置或监听 IPC，发现变更后**重建** MTL Context 与 RX（或调用某「控制接口」触发重建）。

这种方式需要自行设计配置或 IPC 协议，实现和调试更复杂；通常更推荐单进程内同时跑 IS-05 与 MTL。

---

## 六、单进程实现结构示例（C++）

以下为**逻辑结构**，不依赖具体 HTTP 库（可用 cpp-httplib、libmicrohttpd、或 Python 的 Flask/FastAPI 等）。

### 6.1 状态与 MTL 持有

```cpp
// 每个 IS-04 Receiver ID 对应一组 MTL RX 会话 + 当前传输参数
struct ReceiverState {
  std::string id;  // 与 IS-04 注册的 receiver_id 一致
  mtl_sdk::St2110Endpoint video_ep;
  mtl_sdk::VideoFormat video_fmt;
  std::unique_ptr<mtl_sdk::Context::VideoRxSession> video_rx;

  std::optional<mtl_sdk::St2110Endpoint> audio_ep;
  std::optional<mtl_sdk::AudioFormat> audio_fmt;
  std::unique_ptr<mtl_sdk::Context::AudioRxSession> audio_rx;

  // IS-05 staged/active 的 JSON 状态（用于 GET staged/active 响应；可用任意 JSON 库或手拼）
  // 例如：std::string staged_json; 或 nlohmann::json staged;
  bool master_enable = false;
};

std::unique_ptr<mtl_sdk::Context> g_ctx;
std::map<std::string, ReceiverState> g_receivers;
```

- **g_ctx**：进程启动时根据 `MtlSdkConfig`（端口、PTP 等）创建并 `start()`，进程退出时 `stop()`。
- **g_receivers**：key 为注册时的 receiver_id；在首次 PATCH 或启动时可为已注册的 receiver 建空状态，PATCH 时填充并创建 RX。

### 6.2 启动时

1. 读取配置（端口、本机 IP、MTL 端口名等），创建 `MtlSdkConfig`，`Context::create(cfg)` 并 `start()`。
2. 根据 IS-04 注册时使用的 **receiver_id** 列表，在 `g_receivers` 中为每个 id 插入一个空的 `ReceiverState`（或从 IS-04 注册脚本传入的 id 列表一致）。
3. 启动 HTTP 服务，绑定到 `api.endpoints` 中的 host/port（即 `--href` 中的端口，如 9090）。

### 6.3 GET /single/receivers

- 返回 `["<receiver_id_1>/", "<receiver_id_2>/"]`（注意尾部 `/`，规范要求）。

### 6.4 GET /single/receivers/{receiverId}/staged

- 从 `g_receivers[receiverId].staged` 返回 JSON；若从未 PATCH 过，可返回 `master_enable: false`、空 `transport_params` 等默认值。

### 6.5 PATCH /single/receivers/{receiverId}/staged（核心）

1. 解析请求体 JSON。
2. **可选**：若带 `transport_file.data`，用 `mtl_sdk::parse_sdp(transport_file["data"])` 得到 `SdpSession`，遍历 `sdp.media`，填 `VideoFormat`/`AudioFormat`/`St2110Endpoint`（参见 `st2110_record` 中 `--sdp` 的解析逻辑）。
3. 若为 `transport_params` 数组：取第一个元素，`ip = multicast_ip 或 destination_ip`，`udp_port = destination_port`，构造 `St2110Endpoint`；`VideoFormat` 可用默认或从其它字段扩展。
4. 若 `activation.mode == "activate_immediate"`：
   - 对该 receiverId 的 `ReceiverState`：若已有 `video_rx`/`audio_rx`，先置空（释放）；
   - 调用 `g_ctx->create_video_rx(video_fmt, video_ep)` 并保存到 `ReceiverState.video_rx`；
   - 若有音频 media 或 transport 参数，同样 `create_audio_rx` 并保存；
   - 将当前参数写入 `active`，`staged` 更新为请求体合并后的状态。
5. 响应 200，body 为完整 staged 状态（与 IS-05 示例一致，包含 `sender_id`、`master_enable`、`activation`、`transport_params` 或 `transport_file`）。

### 6.6 GET /single/receivers/{receiverId}/active

- 返回 `g_receivers[receiverId].active`（即上次成功激活后的参数）。

### 6.7 收流与编码（同进程）

- 若该进程还要像 st2110_record 一样写文件：在**主循环**或**独立线程**中，对每个 `ReceiverState` 的 `video_rx` 做 `poll` → 拷贝或直接 `push_video` 到 `encode_sdk::Session` → `release`；音频同理。这样 IS-05 的「连接」即直接驱动 MTL 收流 + 编码。

---

## 七、与 IS-04 注册的配合

- 注册时 **node.api.endpoints** 和 **node.href** 必须指向**本机实际运行 IS-05 服务的地址**（如 `http://192.168.1.100:9090/`），Controller 才会向该 URL 发 PATCH。
- **receiver_id** 在 IS-04 注册时确定；PATCH 中的 `receiverId` 必须与注册的 id 一致，节点用该 id 在 `g_receivers` 中查找并更新对应 RX 会话。
- 若使用 **register_node_example.py**：`--href` 应设为该 IS-05 服务的基础 URL，`--hostname` 为本机 IP 或主机名；脚本生成的 receiver_id 需与 IS-05 服务端维护的 id 一致（例如脚本将 id 写入配置文件，IS-05 服务启动时读取）。

---

## 八、参考与扩展

- **IS-05 规范**：<https://specs.amwa.tv/is-05/>（Connection API、receiver staged/active、transport_params、transport_file）。
- **本项目 SDP→会话映射**：`tests/unit/sdp_to_session_test.cpp`、`samples/st2110_record.cpp` 中 `--sdp` 解析。
- **RTP transport_params 完整字段**：见 AMWA `receiver_transport_params_rtp` schema（如 interface_ip、multicast_ip、destination_port、rtp_enabled 等），按需全部映射到 MTL/配置。
- **错误处理**：PATCH 若参数非法或 MTL 创建失败，应返回 4xx 及 IS-05 规定的 error 格式，并将 `activation.mode` 置回 `null`，不改变现有 active 状态。

按上述实现后，在 Controller 中对自研 Receiver 执行「连接」即可驱动 MTL 实际订阅对应组播并收流（并可同时送入编码 SDK 写文件）。
