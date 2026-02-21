# 路由管理软件需求与 MTL-Encode-SDK 对接方案

本文说明如何将当前 MTL-Encode-SDK 与「路由管理软件需求」对齐，以及如何在不修改 NMOS-JS 的前提下通过配置与适配层满足需求。

## 1. 路由管理软件需求回顾（需求.md）

- **符合 AMWA NMOS 标准体系**：IS-04 / IS-05 / IS-06 / IS-08  
- **集中注册、发现、调度、路由与监控**：ST 2110 音视频 IP 流  
- **可视化界面**：支撑自研发送/接收 SDK 端、外购 ST2110 编解码器（支持 NMOS）的发现、连接与管理  

## 2. 整体架构安排

```
                    ┌─────────────────────────────────────────┐
                    │         路由管理（NMOS 体系）             │
                    │  ┌─────────────┐    ┌─────────────────┐   │
                    │  │ NMOS       │    │ NMOS-JS         │   │
                    │  │ Registry   │◄───│ (可视化界面)     │   │
                    │  │ (IS-04)    │    │ IS-04 发现       │   │
                    │  └─────┬──────┘    │ IS-05 连接管理   │   │
                    │        │           └─────────────────┘   │
                    └────────┼──────────────────────────────────┘
                             │ 注册/查询/IS-05 激活
         ┌───────────────────┼───────────────────┐
         │                   │                   │
         ▼                   ▼                   ▼
┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐
│ 自研 接收 节点   │ │ 自研 发送 节点   │ │ 外购 ST2110     │
│ (本仓库应用)    │ │ (可选，待扩展)   │ │ 编解码器        │
│                 │ │                 │ │ (自带 NMOS)     │
│ MTL SDK (RX)    │ │ MTL SDK (TX)     │ │ 向同一 Registry │
│ + Encode SDK    │ │ + 可选编码       │ │ 注册即可        │
│ + NMOS 适配层   │ │ + NMOS 适配层    │ │                 │
└─────────────────┘ └─────────────────┘ └─────────────────┘
```

- **MTL-Encode-SDK** 保持为独立库，不直接依赖 Registry 或前端。  
- **路由管理** 通过「NMOS Registry + NMOS-JS（或其它 IS-04/IS-05 客户端）」实现；本仓库通过 **routing 适配层** 将「自研节点」接入该体系。  
- **自研节点**：使用 MTL SDK 收流、可选 Encode SDK 编码；通过适配层向 Registry 注册（IS-04），并响应 IS-05 连接请求，将「目标地址/端口/SDP」映射为 MTL SDK 的 `St2110Endpoint` 或会话创建/更新。

## 3. 与 NMOS 概念的对应关系

| NMOS 概念 | 本仓库对应 | 说明 |
|-----------|------------|------|
| **Node** | 运行中的进程（如 st2110_record 或自定义录播/网关进程） | 一个进程可暴露一个 Node，包含多个 Device/Receiver。 |
| **Device** | 逻辑设备（如「ST2110 接收+编码设备」） | 可对应一组 MTL RX 会话 + Encode Session。 |
| **Receiver** | 一个 ST2110 接收能力 | 对应 `Context::create_video_rx` / `create_audio_rx` 的会话；IS-05 激活时用传输参数（目标 IP/端口等）创建或更新会话。 |
| **Sender** | 一个 ST2110 发送能力 | 已实现：`Context::create_video_tx` / `create_audio_tx`（ST2110-20/30）；自研发送端可注册为 NMOS Sender。 |
| **Flow / Source** | 流描述（格式、SDP 等） | 与 `VideoFormat`/`AudioFormat`、`SdpSession` 对应；可从 SDP 导入或由 IS-05 传输文件提供。 |

**IS-05 与 MTL SDK 的衔接**：  
IS-05 激活 Receiver 时提供传输参数（如 `destination_ip`、`destination_port`、或 SDP）。适配层解析后填充 `St2110Endpoint`（ip / udp_port / payload_type），并调用 MTL SDK 创建/更新 RX 会话；可选将 SDP 通过 `parse_sdp` 得到 `SdpSession`，再映射到 `VideoFormat`/`AudioFormat` 与 `St2110Endpoint`。

## 4. 当前项目结构与路由的匹配方式

- **mtl_sdk**：提供 `Context`、`VideoRxSession`、`AudioRxSession`、`St2110Endpoint`、`VideoFormat`/`AudioFormat`、SDP 解析/导出。**不包含** Registry 或 HTTP 服务；路由层只「使用」这些类型和 API。  
- **encode_sdk**：提供 `Session::open`、`push_video`/`push_audio`、`close`。录播类节点在 MTL RX 与编码之间串联；路由层可配置编码参数或输出路径，但不改动 encode_sdk 内部。  
- **routing/**：新增「路由适配」目录，包含：  
  - 说明文档（README）：如何部署 Registry、NMOS-JS，如何将自研节点注册为 NMOS Node。  
  - 可选脚本/示例：向 Registry 注册 Node/Device/Receiver（IS-04），演示不修改 NMOS-JS 仅通过配置与注册即可在 NMOS-JS 中发现与管理。  
  - 后续可增加：轻量 IS-05 服务端（或调用现有 NMOS 库），在收到 PATCH 时更新 MTL 会话参数。

这样安排后：**MTL-Encode-SDK 的目录与逻辑保持不变**，路由需求由「routing 适配层 + 外部 Registry + NMOS-JS」共同满足。

## 5. 不修改 NMOS-JS 即可满足需求的方式

- **发现、连接、管理**：由 NMOS-JS 通过 Registry 的 IS-04 Query 与 IS-05 Connection API 完成；NMOS-JS 无需改源码，只需配置其指向**同一 Registry**。  
- **自研节点可见**：自研应用（或 routing 适配服务）向该 Registry 执行 IS-04 注册（Node/Device/Source/Flow/Sender/Receiver）；NMOS-JS 打开即能发现这些资源。  
- **连接（路由）**：在 NMOS-JS 中执行 Sender–Receiver 连接时，会向 Registry/Node 发起 IS-05 请求；自研节点需实现 IS-05 服务端或由适配层转发并驱动 MTL SDK（创建/更新 `St2110Endpoint` 与 RX 会话）。  

**结论**：不修改 NMOS-JS 库内容，通过「部署 Registry + 配置 NMOS-JS 的 Registry 地址 + 自研节点通过适配层注册并实现/代理 IS-05」即可满足需求文档中的发现、连接与管理及可视化界面。

## 6. 推荐实施步骤

1. **部署 NMOS Registry**（如 [nmos-cpp](https://github.com/sony/nmos-cpp) 的 Registry 或其它 IS-04 实现）。  
2. **部署 NMOS-JS**，配置 Registry Base URL，用于发现与连接管理。NMOS-JS **安装位置**、**配置与依赖**、以及如何满足路由管理软件详细需求见 **[NMOS_JS_DEPLOY.md](NMOS_JS_DEPLOY.md)**。  
3. **在自研应用中集成 routing 适配层**：  
   - 启动时向 Registry 注册 Node/Device/Receiver（及可选 Sender）；  
   - 实现或代理 IS-05 接收端：收到激活请求后，解析传输参数，创建/更新 MTL SDK 的 `St2110Endpoint` 与 RX 会话（及可选编码管道）。  
4. **外购 ST2110 编解码器**：将其配置为向同一 Registry 注册，即可在 NMOS-JS 中统一发现、连接与管理。  

以上步骤均无需修改 NMOS-JS 源码；仅需配置与自研侧适配实现。
