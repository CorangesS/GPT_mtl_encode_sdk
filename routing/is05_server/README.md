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

## 依赖

- Python 3.6+
- Flask：`pip install flask`（或 `pip install -r requirements.txt`）
