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
   - **推荐**：使用 [Easy-NMOS](https://github.com/rhastie/easy-nmos)，一键部署 Registry + Controller（`/admin`），无需单独部署 nmos-js。  
   - 或使用 [nmos-cpp](https://github.com/sony/nmos-cpp) 的 Registry 等 IS-04 实现。

2. **部署并配置 Web 界面**  
   - 若用 **Easy-NMOS**：访问 `http://<Easy-NMOS-IP>/admin` 即可，无需额外配置。  
   - 若用 **nmos-cpp 单独部署**：需部署 [nmos-js](https://github.com/sony/nmos-js) 并配置 Registry Base URL，详见 [../docs/NMOS_JS_DEPLOY.md](../docs/NMOS_JS_DEPLOY.md)。

3. **自研节点接入**  
   - 运行 `scripts/register_node_example.py` 向 Registry 注册 Node/Device/Receiver；支持 `--heartbeat` 保持注册有效。  
   - 可选：使用 `scripts/run_with_nmos.sh` 在收流时同时进行 NMOS 注册。  
   - 实现 IS-05 服务端（后续扩展）：收到连接激活请求时，解析传输参数，创建或更新 MTL SDK 的 `St2110Endpoint` 与 RX 会话。

4. **外购 ST2110 设备**  
   将其配置为向同一 Registry 注册，即可在 Controller 中与自研节点一起发现、连接与管理。

**完整实现流程见 [../docs/EASY_NMOS_IMPLEMENTATION.md](../docs/EASY_NMOS_IMPLEMENTATION.md)。**

## 脚本说明

| 脚本 | 说明 |
|------|------|
| `scripts/register_node_example.py` | 向 Registry 注册 Node/Device/Receiver；`--heartbeat` 保持注册有效；`REGISTRY_URL` 指定 Registry 地址。 |
| `scripts/run_with_nmos.sh` | 在后台启动 NMOS 注册，并运行 st2110_record 收流编码。 |

## 与 MTL/Encode SDK 的边界

- **mtl_sdk**：提供 `St2110Endpoint`、`VideoFormat`/`AudioFormat`、SDP 解析、`create_video_rx`/`create_audio_rx`。适配层将 IS-05 的传输参数映射为这些类型与 API 调用。
- **encode_sdk**：提供编码与封装；录播类节点在 MTL RX 与 encode_sdk 之间串联，路由层可配置输出路径或编码参数，但不改动 encode_sdk 内部实现。
