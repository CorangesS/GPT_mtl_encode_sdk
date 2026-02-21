# 路由管理模块

本目录提供将 **MTL-Encode-SDK** 与「路由管理软件需求」对接的说明与示例，使自研接收/发送端能在 NMOS 体系下被**发现、连接与管理**，且**无需修改 NMOS-JS** 库本身。

## 需求对应

- **AMWA NMOS**（IS-04 / IS-05 / IS-06 / IS-08）：通过接入 NMOS Registry 与 NMOS-JS 实现。
- **集中注册、发现、调度、路由与监控**：Registry 负责注册与发现；NMOS-JS 负责可视化与 IS-05 连接（路由）。
- **自研发送/接收 SDK 端、外购 ST2110 编解码器**：自研端通过本模块的适配方式向同一 Registry 注册；外购设备配置为向同一 Registry 注册即可。

## 架构要点

- **MTL-Encode-SDK** 保持为纯库（mtl_sdk + encode_sdk），不依赖 Registry 或 Web UI。
- **路由管理** = NMOS Registry（IS-04）+ 控制面 UI（如 [nmos-js](https://github.com/sony/nmos-js)）+ **自研节点的 NMOS 适配**。
- 自研节点（使用 MTL SDK 收流、可选 encode_sdk 编码）通过「适配层」向 Registry 注册 Node/Device/Receiver（IS-04），并实现或代理 IS-05，使 NMOS-JS 发起的连接请求能驱动 MTL SDK 的 `St2110Endpoint` 与 RX 会话。

详细对接方案、数据模型对应关系、不修改 NMOS-JS 的配置方式见：**[../docs/ROUTING.md](../docs/ROUTING.md)**。  
**NMOS-JS 安装到哪里、如何配置与依赖才能完成路由管理软件详细需求**见：**[../docs/NMOS_JS_DEPLOY.md](../docs/NMOS_JS_DEPLOY.md)**。

## 目录说明

- **README.md**（本文件）：路由模块用途与使用说明。
- **scripts/**：示例脚本，用于演示如何向 NMOS Registry 注册节点，以便在 NMOS-JS 中发现（无需改 nmos-js 源码）。

## 使用步骤概要

1. **部署 NMOS Registry**  
   例如使用 [nmos-cpp](https://github.com/sony/nmos-cpp) 的 Registry，或其它符合 IS-04 的实现。

2. **部署并配置 NMOS-JS**  
   将 NMOS-JS 的 build 部署到 Web 可访问目录（如 `/var/www/nmos-js` 或 nmos-cpp-registry 的 `admin/`），并配置 Registry Base URL。安装位置与配置详见 [../docs/NMOS_JS_DEPLOY.md](../docs/NMOS_JS_DEPLOY.md)。无需修改 NMOS-JS 代码。

3. **自研节点接入**  
   - 在启动时通过 IS-04 Registration API 向 Registry 注册 Node、Device、Receiver（及可选 Sender）。
   - 实现 IS-05 服务端（或通过适配服务代理）：收到连接激活请求时，解析传输参数（如目标 IP/端口或 SDP），创建或更新 MTL SDK 的 `St2110Endpoint` 与 RX 会话（参见 `mtl_sdk::Context::create_video_rx` / `create_audio_rx`）。

4. **外购 ST2110 设备**  
   将其配置为向同一 Registry 注册，即可在 NMOS-JS 中与自研节点一起发现、连接与管理。

## 示例脚本

`scripts/register_node_example.py` 演示如何向已有 Registry 注册一个简单的 NMOS Node（含 Device 与 Receiver），以便在 NMOS-JS 中看到该节点并做连接管理。运行前请安装依赖并设置 Registry 的 Base URL（见脚本内说明）。该示例**不修改 NMOS-JS**，仅通过 Registry API 注册资源。

## 与 MTL/Encode SDK 的边界

- **mtl_sdk**：提供 `St2110Endpoint`、`VideoFormat`/`AudioFormat`、SDP 解析、`create_video_rx`/`create_audio_rx`。适配层将 IS-05 的传输参数映射为这些类型与 API 调用。
- **encode_sdk**：提供编码与封装；录播类节点在 MTL RX 与 encode_sdk 之间串联，路由层可配置输出路径或编码参数，但不改动 encode_sdk 内部实现。
