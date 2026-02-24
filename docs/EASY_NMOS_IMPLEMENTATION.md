# 路由管理软件实现指南（基于 Easy-NMOS）

本文档提供 **需求.md** 中「路由管理软件部分」的完整实现流程，基于已部署的 **Easy-NMOS**。

---

## 一、需求回顾

| 需求 | 说明 |
|------|------|
| 1 | 符合 AMWA NMOS 标准体系（IS-04 / IS-05 / IS-06 / IS-08） |
| 2 | ST2110 音视频 IP 流的集中注册、发现、调度、路由与监控 |
| 3 | 可视化界面，支撑自研发送/接收 SDK、外购 ST2110 编解码器的发现、连接与管理 |

---

## 二、Easy-NMOS 与整体架构

### 2.1 Easy-NMOS 已提供的能力

Easy-NMOS 一键部署后包含：

| 组件 | 端口 | 说明 |
|------|------|------|
| **NMOS Registry** | 80 | IS-04 注册与发现，IS-05 连接管理 |
| **NMOS Controller** | 80/admin | 可视化 Web 界面（替代 NMOS-JS） |
| **NMOS Virtual Node** | 80 | 虚拟节点，用于测试 |
| **AMWA NMOS Testing Tool** | 5000 | 规范合规性测试 |

访问方式：

- Controller（可视化界面）：`http://<Easy-NMOS-IP>/admin`
- Registry API：`http://<Easy-NMOS-IP>/x-nmos/...`

### 2.2 实现架构

```
┌─────────────────────────────────────────────────────────────────┐
│                    Easy-NMOS（已部署）                            │
│  ┌──────────────────────┐    ┌────────────────────────────────┐  │
│  │ NMOS Registry        │    │ NMOS Controller（/admin）       │  │
│  │ IS-04 注册/发现      │◄───│ 可视化：发现、连接、管理        │  │
│  │ IS-05 连接 API       │    │ 无需再部署 nmos-js              │  │
│  └──────────┬───────────┘    └────────────────────────────────┘  │
└─────────────┼────────────────────────────────────────────────────┘
              │ 注册 / 查询 / 连接
    ┌─────────┼─────────┬────────────────────┐
    ▼         ▼         ▼                    ▼
┌──────────────┐ ┌──────────────┐ ┌──────────────────────┐
│ 自研接收节点 │ │ 自研发送节点 │ │ Easy-NMOS 虚拟节点   │
│ st2110_record│ │ st2110_send  │ │ （自带，可忽略）     │
│ + 注册脚本   │ │ + 注册脚本   │ │                      │
└──────────────┘ └──────────────┘ └──────────────────────┘
```

---

## 三、完整实现流程

### 阶段一：确认 Easy-NMOS 可用（约 5 分钟）

1. **获取 Easy-NMOS 的 IP 地址**
   - 若在本机：`127.0.0.1` 或 `localhost`
   - 若在局域网：如 `192.168.6.101`（以 docker-compose 配置为准）

2. **验证 Registry 与 Controller**
   ```bash
   # 测试 Registry 是否响应
   curl -s http://<Easy-NMOS-IP>/x-nmos/registration/v1.2/resource/nodes | head -c 200

   # 在浏览器打开 Controller
   # http://<Easy-NMOS-IP>/admin
   ```

3. **确认 Controller 可访问**
   - 打开 `http://<Easy-NMOS-IP>/admin`，应能看到 NMOS 管理界面
   - 若已启动 Virtual Node，可看到虚拟节点

---

### 阶段二：自研节点 IS-04 注册（约 10 分钟）

自研节点需向 Registry 注册，才能在 Controller 中被发现。

#### 步骤 2.1：注册接收节点（st2110_record 对应）

```bash
cd /root/GPT_mtl_encode_sdk/routing/scripts

# 设置 Registry 地址（Easy-NMOS 默认端口 80）
export REGISTRY_URL=http://<Easy-NMOS-IP>
# 例如：export REGISTRY_URL=http://192.168.6.101

# 运行注册脚本
python3 register_node_example.py
```

脚本会注册：
- 1 个 Node（MTL-Encode-SDK Node）
- 1 个 Device（ST2110 RX + Encode Device）
- 1 个 Receiver（Video Receiver）

#### 步骤 2.2：在 Controller 中验证

1. 打开 `http://<Easy-NMOS-IP>/admin`
2. 在「Nodes」或「Receivers」视图中应能看到刚注册的节点
3. 若未显示，检查 `REGISTRY_URL` 是否可达（自研节点与 Easy-NMOS 需网络互通）

#### 步骤 2.3：保持心跳（IS-04 续期）

IS-04 资源有过期时间，需定期续期。使用带心跳的注册脚本：

```bash
# 使用 --heartbeat 参数保持注册有效
python3 register_node_example.py --heartbeat --interval 30
```

---

### 阶段三：收发测试与 NMOS 协同（约 15 分钟）

#### 场景 A：本机收发（不依赖 NMOS 连接）

收发仍通过命令行参数指定组播地址，与 NMOS 注册并行：

```bash
# 终端 1：启动接收并录制
./st2110_record --ip 239.0.0.1 --video-port 5004 --audio-port 0 --max-frames 600 recv.mp4

# 终端 2：启动发送
./st2110_send --ip 239.0.0.1 --video-port 5004 --audio-port 0 --duration 10
```

#### 场景 B：NMOS 发现 + 手动连接

1. 在 Controller 中查看 Nodes 和 Receivers
2. 确认自研 Receiver 已注册
3. 当前阶段：**连接（IS-05）需自研节点实现 IS-05 服务端**；若未实现，在 Controller 中执行 Sender→Receiver 连接不会驱动 MTL SDK 实际收流
4. 因此，**实际收流仍通过命令行参数**（`--ip`、`--video-port` 等）指定，NMOS 当前主要用于**发现与监控**

---

### 阶段四：外购 ST2110 设备接入（可选）

若使用外购 ST2110 编解码器（支持 NMOS）：

1. 将其 NMOS Registry 地址配置为 `http://<Easy-NMOS-IP>`
2. 设备启动后会自动向 Registry 注册
3. 在 Controller 中可与自研节点一起被发现、连接与管理

---

## 四、操作步骤速查

| 步骤 | 命令 / 操作 |
|------|-------------|
| 1 | 确认 Easy-NMOS 运行：`curl http://<IP>/admin` |
| 2 | 设置 Registry：`export REGISTRY_URL=http://<Easy-NMOS-IP>` |
| 3 | 注册自研节点：`python3 routing/scripts/register_node_example.py` |
| 4 | 打开 Controller：浏览器访问 `http://<Easy-NMOS-IP>/admin` |
| 5 | 验证发现：在 Nodes/Receivers 中查看自研节点 |
| 6 | 收发测试：`st2110_record` + `st2110_send`（参数与 NMOS 独立） |

---

## 五、与 nmos-js 的关系

- **使用 Easy-NMOS**：已包含 Controller（`/admin`），**无需单独部署 nmos-js**
- **使用 nmos-cpp 单独部署**：无自带 UI 时，需部署 nmos-js 作为可视化界面

---

## 六、后续扩展：IS-05 驱动 MTL SDK

要实现在 Controller 中「点击连接」即驱动 MTL 实际收流，需：

1. 自研节点实现 IS-05 服务端（HTTP 接口）
2. 收到连接 PATCH 时，解析 `destination_ip`、`destination_port`、SDP 等
3. 构造 `St2110Endpoint`、`VideoFormat`/`AudioFormat`
4. 调用 `Context::create_video_rx` / `create_audio_rx` 创建/更新收流

实现方案见 [ROUTING.md](ROUTING.md) 第 3、4 节。

---

## 七、故障排查

| 现象 | 可能原因 | 处理 |
|------|----------|------|
| 注册脚本连接失败 | Registry URL 错误或网络不通 | 检查 `REGISTRY_URL`、防火墙、路由 |
| Controller 看不到节点 | 注册失败或心跳过期 | 确认注册成功，使用 `--heartbeat` 续期 |
| Easy-NMOS 端口非 80 | 自定义端口 | 设置 `REGISTRY_URL=http://<IP>:<端口>` |
| 跨机访问 Registry 失败 | 防火墙或 Docker 网络 | 确保 Registry 端口对外暴露 |
