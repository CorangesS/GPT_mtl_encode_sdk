# 需求 16–18 具体实现拆解与 routing 现状分析

对应 **需求.md** 第 16–18 行：路由管理软件的两个顶层需求。本文逐一说明每个子需求需要**实现什么**、**输入/输出/操作**，并结合 NMOS 标准；最后对照当前 `routing/` 实现，说明已做与待补。

---

## 一、需求 1：符合 AMWA NMOS 标准体系（IS-04 / IS-05 / IS-06 / IS-08）

### 1.1 IS-04 注册与发现

| 子项 | 需实现内容 | 输入 | 输出 | 操作说明 |
|------|------------|------|------|----------|
| **节点注册** | 自研节点向 Registry 注册 Node、Device、Source/Flow/Sender/Receiver 等资源 | Registry Base URL、节点 href、hostname、模式(receiver/sender/both) | 注册成功/失败；可选保存 node_id/device_id/receiver_id/sender_id 到配置文件 | 通过 Registration API `POST /resource` 按顺序注册 node→device→(source→flow→)receiver/sender；版本号符合 IS-04 规范 |
| **心跳保活** | 定期刷新注册，防止 TTL 过期被 Registry 剔除 | 心跳间隔(秒)、同上注册参数 | 持续运行，定时重新注册并更新 version | 按间隔(建议≤10s)更新 version 并再次 POST 各资源 |
| **发现** | Controller/UI 从 Registry 查询已注册的 Nodes、Devices、Senders、Receivers | Registry Query API base（如 `/x-nmos/query`）或通过 Controller 间接查询 | 资源列表（含 id、label、subscription 等） | 通常由 Registry + Query API 提供；UI 展示 Nodes/Devices/Senders/Receivers 列表，供连接与监控 |

**NMOS 要点**：IS-04 定义资源模型与 Registration/Query API；自研端只需实现「注册」与「Node API 根/桩」以便 Controller 探测；发现由 Registry+Query 完成。

---

### 1.2 IS-05 连接管理（Connection API）

| 子项 | 需实现内容 | 输入 | 输出 | 操作说明 |
|------|------------|------|------|----------|
| **Receiver STAGED** | 获取/更新 Receiver 的待生效连接参数 | receiver_id；PATCH 时：sender_id、transport_params 或 transport_file(SDP)、master_enable、activation | GET：当前 staged JSON；PATCH：更新 staged，若 activation.mode=activate_immediate 则立即生效并写连接状态 | 控制器先 GET 再 PATCH；PATCH 激活时解析 SDP 或 transport_params，写入 connection_state 供底层收流 |
| **Receiver ACTIVE** | 获取当前已生效连接 | receiver_id | 当前 active 的 sender_id、transport_params、transport_file、activation_time | 只读；用于 UI 显示「当前连到哪个 Sender、参数是什么」 |
| **Receiver constraints** | 约束（如端口、IP 范围） | receiver_id | 约束 JSON（如 interface_ip、destination_port、rtp_enabled 的 enum/范围） | Controller 在 CONNECT 时用于校验或展示可填参数 |
| **Receiver transporttype** | 传输类型 | receiver_id | 如 `urn:x-nmos:transport:rtp.ucast` | 标识该 Receiver 支持的传输类型 |
| **Sender STAGED/ACTIVE** | Sender 端待生效/已生效连接 | sender_id；PATCH 时：receiver_id、transport_params、activation | 同 Receiver 对称 | 用于「从发送端发起的连接」配置 |
| **Sender transportfile** | 获取 Sender 的 SDP | sender_id | SDP 文件（application/sdp） | Controller 在 CONNECT 时取 Sender 的 SDP，再 PATCH 到 Receiver 的 staged |
| **连接激活** | 将 STAGED 立即生效并驱动底层 | PATCH staged 且 activation.mode=activate_immediate | 写 connection_state.json；返回 200 及更新后的 staged/active | 服务端解析 transport_file/transport_params，写入约定路径；C++ daemon 轮询该文件，创建/更新 MTL RX 并编码 |

**NMOS 要点**：IS-05 定义 Connection API（single/bulk）；本处为 single 模式，Receiver 端「PATCH 激活 → 写 connection_state → daemon 驱动 MTL」是核心闭环。

---

### 1.3 IS-06 网络控制（已弃用，可选）

| 子项 | 需实现内容 | 输入 | 输出 | 操作说明 |
|------|------------|------|------|----------|
| **拓扑与流授权** | 网络拓扑发现、流授权、带宽保障 | 网络控制器 Northbound API 的请求 | 拓扑/流/带宽等数据 | IS-06 已弃用；若需「网络层」管理，可仅做最小兼容或由交换机/控制器另行实现，非当前路由管理必需 |

---

### 1.4 IS-08 音频声道映射

| 子项 | 需实现内容 | 输入 | 输出 | 操作说明 |
|------|------------|------|------|----------|
| **Channel Mapping API** | 音频输入/输出、激活与映射关系 | Device/Node 上的 IS-08 API；请求 body 含 inputs/outputs/activations 等 | 当前 mapping 状态、constraints、capabilities | 多声道/多轨音频时，配置哪路输入映射到哪路输出；ST2110 纯视频或简单音频场景可后补 |

**说明**：当前 routing 以视频流连接为主；若播出系统需要精细音频声道映射，再实现 IS-08 的 Channel Mapping API。

---

## 二、需求 2：支持对 ST 2110 音视频 IP 流的集中注册、发现、调度、路由与监控管理

### 2.1 集中注册

| 子项 | 需实现内容 | 输入 | 输出 | 操作说明 |
|------|------------|------|------|----------|
| **统一 Registry** | 所有节点（自研 SDK、外购 ST2110 设备）向同一 Registry 注册 | 各节点的 Node/Device/Sender/Receiver 资源；REGISTRY_URL | 注册成功；Registry 中可见所有节点 | 自研端用 register_node_example.py；外购设备配置其 Registry 地址为同一 REGISTRY_URL |

---

### 2.2 发现

| 子项 | 需实现内容 | 输入 | 输出 | 操作说明 |
|------|------------|------|------|----------|
| **资源发现** | 在控制面查看所有 Nodes、Devices、Senders、Receivers | 无（或 Query API 过滤条件） | 资源列表及属性（id、label、format、transport、subscription 等） | 通过 Easy-NMOS Controller（/admin）或 Query API 展示；用户可看到自研 + 外购设备 |

---

### 2.3 调度

| 子项 | 需实现内容 | 输入 | 输出 | 操作说明 |
|------|------------|------|------|----------|
| **连接调度** | 在指定时间或立即执行「Sender → Receiver」连接 | 用户选择 Receiver + Sender；可选 scheduled time；transport_file 或 transport_params | 连接请求下发到节点 IS-05；返回成功/失败 | 当前实现为「立即激活」；若需定时调度，需在 Controller 或服务端增加「在 requested_time 执行 PATCH」的调度逻辑 |
| **激活模式** | 支持 activate_immediate / activate_scheduled | PATCH body 中的 activation.mode、requested_time | 同 IS-05 的 staged/active | 目前 routing 已实现 activate_immediate；activate_scheduled 需在节点侧按时间戳执行激活 |

---

### 2.4 路由

| 子项 | 需实现内容 | 输入 | 输出 | 操作说明 |
|------|------------|------|------|----------|
| **连接（路由）** | 将选定的 Sender 与 Receiver 建立连接，并下发传输参数 | Sender ID、Receiver ID、SDP 或 transport_params | 各节点 IS-05 的 active 状态更新；底层实际收/发流 | Controller 取 Sender transportfile，对 Receiver 的 staged 发 PATCH( sender_id + transport_file + activate_immediate )；Receiver 节点写 connection_state，daemon 起 MTL RX |
| **断开** | 取消连接 | 对 Receiver staged 发 PATCH(master_enable=false 或 sender_id=null) 并激活 | active 清空；底层停止收流 | 需在 UI 支持「Disconnect」并下发对应 PATCH |

---

### 2.5 监控管理

| 子项 | 需实现内容 | 输入 | 输出 | 操作说明 |
|------|------------|------|------|----------|
| **连接状态监控** | 查看各 Receiver/Sender 的 active 状态 | 无（或 receiver_id/sender_id） | active 的 sender_id/receiver_id、transport_params、activation_time | 通过 GET .../active 实现；Easy-NMOS admin 已展示 |
| **流健康监控** | 收流端丢包、误码、中断等 | 节点本地（MTL/硬件）统计 | 告警或指标上报 | 当前 routing 未实现；可扩展：节点暴露简单 HTTP 或通过 IS-04 扩展字段/自定义 API 上报健康度 |
| **资源存活监控** | 节点是否在线、是否从 Registry 消失 | Registry 心跳 / Query 结果 | 节点在线/离线状态 | 依赖 IS-04 心跳与 Registry TTL；Controller 列表自动反映离线节点 |

---

## 三、需求 3（第 18 行）与可视化

需求 3 要求可视化界面，支撑播出系统、ST2110 网关与 IP 化播控的统一管理；支持自研发送/接收 SDK 与外购 ST2110 编解码器（支持 NMOS）的发现、连接与管理。

您已实现 **Easy-NMOS 可视化界面**，对应地：

- **发现**：在 admin 查看 Nodes、Devices、Senders、Receivers（自研 + 外购，只要注册到同一 Registry）。
- **连接与管理**：在 Receivers 上 CONNECT，选择 Sender，取 transportfile 并 PATCH 到 Receiver，完成路由；查看 ACTIVE/STAGED。

尚未在 UI 或后端显式实现的（可选增强）：

- **调度**：定时激活（activate_scheduled）的配置与执行。
- **监控**：流健康、丢包率等指标在界面展示。
- **断开**：在 admin 明确提供 Disconnect 并下发 PATCH 清空连接。

---

## 四、当前 routing/ 已实现的功能与步骤

### 4.1 已实现的功能与操作流程

| 功能 | 输入 | 输出 | 步骤/操作流程 |
|------|------|------|----------------|
| **IS-04 注册** | REGISTRY_URL、--href、--mode、--save-config、--heartbeat、--interval | 注册成功；可选 .nmos_node.json | 1. 检测 Registration API 版本 2. 生成 node/device/receiver(及可选 source/flow/sender) 3. POST /resource 注册 4. 若 --heartbeat 则循环更新 version 并重新 POST |
| **IS-05 服务端（单机）** | 请求：GET/PATCH .../receivers/.../staged、.../active、.../constraints、.../transporttype；GET/PATCH .../senders/...、.../transportfile | JSON 或 SDP；PATCH 激活时写 connection_state.json | 1. 从 .nmos_node.json 或环境变量读 receiver_id/sender_id 2. 实现 Node API 根及桩(/self/, /devices/, /senders/, /receivers/, /sources/, /flows/) 3. 实现 IS-05 single 的 receivers/senders 全部端点 4. PATCH staged 且 activate_immediate 时解析 SDP/transport_params，写入 CONNECTION_STATE_FILE |
| **CONNECT 闭环（Receiver）** | Controller 在 UI 点 CONNECT → 选 Sender → 发 PATCH(staged) | connection_state.json 更新；daemon 收流 | 1. Controller 取 Sender transportfile 2. 对 Receiver staged 发 PATCH(sender_id + transport_file + activate_immediate) 3. app.py 解析 SDP，写 connection_state.json 4. is05_receiver_daemon 轮询该文件，创建/更新 video_rx 并编码为 MP4 |
| **is05_receiver_daemon** | CONNECTION_STATE_FILE、--port、--sip、--output、--state | 根据 connection_state 起/停 MTL video_rx，写 MP4 | 1. 轮询 connection_state.json 2. 若 master_enable 且 video 有效：创建 St2110Endpoint、create_video_rx、encode_sdk 写 MP4 3. 若连接清空则释放 session |
| **run_with_nmos.sh** | REGISTRY_URL、st2110_record 参数 | 后台注册 + 前台收流 | 1. 后台启动 register_node_example.py --heartbeat 2. 前台运行 st2110_record（注意：未与 connection_state 联动，仅为同机注册+收流示例） |

### 4.2 routing 中已实现的「步骤与操作」小结

1. **注册**：`register_node_example.py` — 向 Registry 注册 Node/Device/Receiver（及可选 Sender），支持心跳，可写 .nmos_node.json。
2. **Node API 桩**：`app.py` — 提供 `/`、`/self/`、`/devices/`、`/senders/`、`/receivers/`、`/sources/`、`/flows/`，满足 Controller 探测。
3. **IS-05 Connection API**：`app.py` — single 模式 Receivers/Senders 的 GET/PATCH staged、GET active、constraints、transporttype、transportfile；PATCH 激活时写 connection_state。
4. **连接驱动收流**：`app.py` 写 connection_state → `is05_receiver_daemon` 读文件并驱动 MTL RX + 编码。

---

## 五、可能还需要实现的具体功能与操作

### 5.1 断开连接（Disconnect）

| 项目 | 说明 |
|------|------|
| **输入** | receiver_id；或 UI 点击「Disconnect」 |
| **输出** | Receiver 的 active 清空；connection_state.json 中 master_enable=false 或无有效 video/audio；底层停止收流 |
| **操作流程** | 1. Controller 对 Receiver 的 staged 发 PATCH：master_enable=false 或 sender_id=null，activation.mode=activate_immediate 2. app.py 写 connection_state（master_enable=false 或清空 video/audio） 3. daemon 检测到无效状态，释放 video_rx/enc |
| **现状** | app.py 在 PATCH 时若未传 master_enable 会保留原值；若需显式断开，需支持「仅发断开」的 PATCH 并写清空状态；daemon 已能根据 state.valid 释放。 |

### 5.2 定时激活（activate_scheduled）

| 项目 | 说明 |
|------|------|
| **输入** | PATCH 中 activation.mode=activate_scheduled，requested_time=T（ISO8601） |
| **输出** | 在 T 时刻将当前 staged 变为 active，并写 connection_state |
| **操作流程** | 1. app.py 收到 PATCH 后若为 scheduled，不立即写 connection_state，仅保存 staged 与 requested_time 2. 后台在 requested_time 到达时执行与 activate_immediate 相同的写 connection_state 逻辑 3. 返回 200 时 activation 含 requested_time |
| **现状** | 当前仅实现 activate_immediate；scheduled 需增加定时任务或轮询时间戳。 |

### 5.3 音频轨（当前 SDP/connection_state 已有字段，daemon 未用）

| 项目 | 说明 |
|------|------|
| **输入** | transport_file 中含 m=audio；connection_state 中 audio 块 |
| **输出** | 同时起 video_rx + audio_rx，编码为带音轨的 MP4 |
| **操作流程** | 1. app.py 已解析 SDP 的 audio，并写入 connection_state 的 audio 2. is05_receiver_daemon 当前仅读 video 块；需扩展：若存在 audio 则 create_audio_rx，并在 encode_sdk 中传入音频 |
| **现状** | 服务端已写 audio；daemon 未实现 audio_rx 与编码音轨。 |

### 5.4 流健康/监控上报（可选）

| 项目 | 说明 |
|------|------|
| **输入** | 节点本地 MTL 或硬件统计（丢包、误码、收包数等） |
| **输出** | 供 Controller 或监控 UI 展示的接口（如 GET /stats 或 IS-04 扩展） |
| **操作流程** | 1. daemon 或单独进程从 MTL 取统计 2. 通过 HTTP API 或 Registry 扩展字段暴露 3. Easy-NMOS 或自研页面拉取并展示 |
| **现状** | 未实现。 |

### 5.5 IS-08 音频声道映射（可选）

| 项目 | 说明 |
|------|------|
| **输入** | IS-08 API 的 inputs/outputs/activations 等 |
| **输出** | 当前 mapping、constraints、capabilities |
| **操作流程** | 按 IS-08 规范实现 Channel Mapping API；多声道场景下再接入。 |
| **现状** | 未实现。 |

### 5.6 Sender 端真实发流（可选）

| 项目 | 说明 |
|------|------|
| **输入** | Sender 的 STAGED/ACTIVE 中的 transport_params（目的 IP/端口等） |
| **输出** | 本机按 ST2110 实际发送 RTP 流 |
| **操作流程** | 1. IS-05 在 Sender 激活时，将 destination_ip/destination_port 等写入某 state 文件或 API 2. 自研发送进程（如 MTL TX）读取该 state，创建 video_tx 并发流 |
| **现状** | app.py 已实现 Sender 的 STAGED/ACTIVE/transportfile；无对应「发流 daemon」读状态并驱动 MTL TX。 |

---

## 六、汇总表：各功能的输入/输出与操作流程

| 功能 | 输入 | 输出 | 操作流程概要 |
|------|------|------|--------------|
| **注册节点** | REGISTRY_URL, href, mode, save-config, heartbeat, interval | 注册结果；.nmos_node.json | 检测 Registration 版本 → 生成资源 → POST /resource → 可选心跳循环 |
| **IS-05 Receiver STAGED GET/PATCH** | receiver_id；PATCH: sender_id, transport_file/params, activation | staged JSON；激活时写 connection_state | GET 返回内存；PATCH 更新并可选立即激活 → 解析 SDP → 写文件 |
| **IS-05 Receiver ACTIVE GET** | receiver_id | active JSON | 返回内存中 active |
| **IS-05 Sender transportfile GET** | sender_id | SDP 文本 | 从 staged/active 的 transport_params 生成 SDP 或默认 |
| **CONNECT（UI）** | 用户选 Receiver + Sender | 连接建立；connection_state 更新 | Controller 取 Sender transportfile → PATCH Receiver staged(activate_immediate) → 节点写 connection_state → daemon 起 RX |
| **daemon 收流** | connection_state.json 路径 | MP4 文件；底层 video_rx 生命周期 | 轮询 state → 若有效则 create_video_rx + encode；若无效则释放 |
| **断开**（待完善） | Disconnect 请求或 PATCH(master_enable=false) | active 清空；connection_state 无效 | PATCH staged 断开并激活 → 写 state → daemon 释放 |
| **定时激活**（未实现） | activation.mode=activate_scheduled, requested_time | 在 T 时刻生效 | 保存 staged+time → 到点执行与 immediate 相同逻辑 |
| **音频收流**（部分实现） | SDP 含 m=audio；connection_state.audio | 带音轨 MP4 | app 已写 audio；daemon 需增加 audio_rx + 编码音轨 |
| **Sender 发流**（未实现） | Sender active transport_params | 实际 RTP 发送 | 发流进程读 state/API → create video_tx 并发送 |

---

以上为需求 16–18 的逐条实现拆解，以及当前 routing 已实现与建议补全的功能与输入/输出/操作流程说明。若你指定优先实现哪几项（如「断开」「定时激活」「音频收流」），可以再按模块写出具体接口与实现要点。
