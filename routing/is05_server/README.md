# IS-05 Connection API 服务端

自研节点上的 **IS-05 Connection API** 实现，使 NMOS Controller 的「Sender → Receiver 连接」能驱动 MTL SDK 收流。

## 实现方式

- **本目录（Python）**：提供完整 IS-05 HTTP API（single 模式）。收到 PATCH 激活时解析 `transport_params` 或 `transport_file`（SDP），将连接状态写入 **connection_state.json**，供 C++ 收流 daemon 读取并创建/更新 MTL RX。
- **C++ 收流 daemon**（`samples/is05_receiver_daemon`）：轮询 connection_state.json，根据当前连接创建 `create_video_rx`/`create_audio_rx`，收流并编码为 MP4。

## 前置

- **Receiver 端**：注册时使用 `--mode receiver --save-config .nmos_node.json`，IS-05 服务读取 `.nmos_node.json` 中的 `receiver_id`；或设置环境变量 `IS05_RECEIVER_ID`。
- **Sender 端**：注册时使用 `--mode sender --save-config .nmos_node.json`，IS-05 服务读取 `.nmos_node.json` 中的 `sender_id`；或设置环境变量 `IS05_SENDER_ID`。同一 app 可同时支持 receiver 与 sender（配置中同时有 receiver_id 与 sender_id 时两端都会暴露）。

## 运行

```bash
# 1) 注册节点并保存配置（与 IS-05 共用 receiver_id）
export REGISTRY_URL=http://<Easy-NMOS-IP>
python3 routing/scripts/register_node_example.py --heartbeat --interval 10 \
  --href http://本机IP:9090/ --save-config .nmos_node.json

# 2) 启动 IS-05 服务（默认 0.0.0.0:9090）
export CONNECTION_STATE_FILE=/path/to/connection_state.json   # 可选，默认 ./connection_state.json
python3 routing/is05_server/app.py

# 3) 启动 C++ 收流 daemon（读取 CONNECTION_STATE_FILE，创建 MTL RX 并编码）
./build/is05_receiver_daemon
```

Controller 访问 `http://<本机IP>/admin`，对自研 Receiver 执行「连接」后，PATCH 会落到本服务；本服务写 connection_state.json，daemon 检测到变更后创建/更新收流并写 MP4。

## API 端点（IS-05 v1.1 single）

- **GET /x-nmos/connection/v1.1/single/**：返回 `["senders/", "receivers/"]`，便于 Controller 发现 STAGED/ACTIVE/TRANSPORT FILE。
- **Receivers**：GET/PATCH staged、GET active、constraints、transporttype（见下表）。
- **Senders**：GET/PATCH staged、GET active、GET transportfile（SDP）、constraints、transporttype。

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | .../single/receivers | 返回 receiver 列表 |
| GET/PATCH | .../single/receivers/{id}/staged | 待生效参数（STAGED） |
| GET | .../single/receivers/{id}/active | 已生效参数（ACTIVE） |
| GET | .../single/senders | 返回 sender 列表 |
| GET/PATCH | .../single/senders/{id}/staged | Sender 待生效（STAGED） |
| GET | .../single/senders/{id}/active | Sender 已生效（ACTIVE） |
| GET | .../single/senders/{id}/transportfile | SDP 传输文件（TRANSPORT FILE） |
| GET | .../single/receivers/{id}/constraints | RTP 约束（CONNECT 时参数校验） |

## Receiver 的 ACTIVE / STAGED / CONNECT

在 Easy-NMOS 的 **Receivers** 界面中，每个 Receiver 支持：

- **ACTIVE**：GET 当前已生效连接（sender_id、master_enable、activation、transport_params、transport_file）。连接后此处会显示实际 Sender 与传输参数。
- **STAGED**：GET 当前待生效配置；PATCH 可提交新连接（如选择 Sender、传入 transport_file 或 transport_params，并设置 `activation.mode: "activate_immediate"` 立即激活）。
- **CONNECT**：Controller 会先拉取 Sender 列表与各 Sender 的 **transportfile**（SDP），再对 Receiver 的 **staged** 发起 PATCH（sender_id + transport_file + activation），完成「选择已有 Sender 并连接」。

本服务已实现：

- **constraints**：返回 RTP 单路约束（interface_ip、destination_port、source_ip、multicast_ip、rtp_enabled），便于 Controller 展示/校验 CONNECT 参数。
- **transport_file**：支持 `{ "data": "<SDP 字符串>", "type": "application/sdp" }` 或直接传 SDP 字符串；连接时会从 SDP 解析并写入 connection_state.json，同时推导 transport_params 供 ACTIVE/STAGED 展示。

在 Receiver 节点界面手动连接已有 Sender 的流程：点 **CONNECT** → 选择要连接的 Sender → Controller 会取该 Sender 的 transportfile 并 PATCH 到本 Receiver 的 staged（含 activate_immediate）→ 本服务解析 SDP、写入 connection_state.json，收流 daemon 即可按新连接收流。

## 依赖

- Python 3.6+
- Flask：`pip install flask`（或 `pip install -r requirements.txt`）

## 出现 "no api found" 或 Sender/Receiver 无 ACTIVE/TRANSPORT FILE/CONNECT 时

1. **节点根与 Node API**：部分 Controller（如 Easy-NMOS）会先请求节点根 `GET http://<node>:port/`，期望得到 IS-04 Node API base 数组 `["self/", "sources/", "flows/", "devices/", "senders/", "receivers/"]`，并可能继续请求 `/self/`、`/devices/` 等。本服务已实现这些桩路由，返回 200 与最小合法 JSON，避免因 404 导致 "no api found"。请确保 **`--save-config` 写入的 `.nmos_node.json` 含有 `node_id`、`device_id`**（注册脚本已自动写入）。
2. **Sender/Receiver 机器必须运行本 IS-05 服务**：在对应机器上执行 `python3 routing/is05_server/app.py`，且 `.nmos_node.json` 中有对应的 `sender_id` 或 `receiver_id`（用 `--mode sender` 或 `--mode receiver` 并 `--save-config` 注册一次即可）。
3. **Connection API 基路径**：Controller 会请求 `GET http://<node>:port/x-nmos/connection/v1.1/`，本服务已实现并返回 `["bulk/", "single/"]`，否则会 404 导致不显示按钮。
4. **跨域**：若 admin 在浏览器中从 Easy-NMOS 主机（如 192.168.1.200）打开，浏览器会跨域请求节点地址，本服务已加 CORS 头。若仍被拦截，请确认防火墙放行对应端口，且从 admin 所在机器能访问节点根与 `.../x-nmos/connection/v1.1/`。
