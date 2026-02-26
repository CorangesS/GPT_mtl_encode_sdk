# IS-05 Connection API 服务端

自研节点上的 **IS-05 Connection API** 实现，使 NMOS Controller 的「Sender → Receiver 连接」能驱动 MTL SDK 收流。

## 实现方式

- **本目录（Python）**：提供完整 IS-05 HTTP API（single 模式）。收到 PATCH 激活时解析 `transport_params` 或 `transport_file`（SDP），将连接状态写入 **connection_state.json**，供 C++ 收流 daemon 读取并创建/更新 MTL RX。
- **C++ 收流 daemon**（`samples/is05_receiver_daemon`）：轮询 connection_state.json，根据当前连接创建 `create_video_rx`/`create_audio_rx`，收流并编码为 MP4。

## 前置

1. 使用 `routing/scripts/register_node_example.py` 向 Registry 注册节点时，加上 `--save-config` 将 node/receiver 信息写入 `.nmos_node.json`，IS-05 服务会读取其中的 `receiver_id`。
2. 或通过环境变量 `IS05_RECEIVER_ID` 指定 receiver ID（须与 IS-04 注册的 id 一致）。

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

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | /x-nmos/connection/v1.1/single/receivers | 返回 receiver 列表 |
| GET | /x-nmos/connection/v1.1/single/receivers/{id}/staged | 待生效参数 |
| PATCH | /x-nmos/connection/v1.1/single/receivers/{id}/staged | 下发并可选立即激活 |
| GET | /x-nmos/connection/v1.1/single/receivers/{id}/active | 已生效参数 |
| GET | /x-nmos/connection/v1.1/single/receivers/{id}/constraints | 约束（空数组） |
| GET | /x-nmos/connection/v1.1/single/receivers/{id}/transporttype | 传输类型 URN |

## 依赖

- Python 3.6+
- Flask：`pip install flask`（或 `pip install -r requirements.txt`）
