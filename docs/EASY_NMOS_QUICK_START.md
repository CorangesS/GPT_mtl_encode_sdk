# Easy-NMOS 快速使用指南（以 192.168.1.200 为例）

本文以你当前部署为例：**Registry 在 192.168.1.200**，**Controller（admin）** 同机，**nmos-virtnode 在 192.168.1.201**，**nmos-testing 在 192.168.1.203**。说明如何用 Easy-NMOS 做「路由管理相关设置和使用」，并对照 **需求.md** 路由管理部分。

---

## 一、当前环境与访问方式

| 组件 | 地址 | 用途 |
|------|------|------|
| **NMOS Registry** | 192.168.1.200（端口 80） | IS-04 注册/发现、IS-05 连接管理 |
| **Controller（可视化 admin）** | http://192.168.1.200/admin | 发现、连接、管理 Nodes/Senders/Receivers |
| **nmos-virtnode** | 192.168.1.201 | 虚拟节点，用于测试（可选） |
| **nmos-testing** | 192.168.1.203 | AMWA 规范测试工具（可选） |

**第一步：确认 Controller 可用**

- 浏览器打开：**http://192.168.1.200/admin**
- 应能看到 NMOS 管理界面；若已启动 virtnode，可在 Nodes/Devices 中看到虚拟节点。

---

## 二、自研节点注册（让发送/接收 SDK 在 Controller 里被「发现」）

要让 **自研发送 SDK 端**、**自研接收 SDK 端** 在 Controller 里可见，需要把它们作为 **Node** 向 **同一 Registry（192.168.1.200）** 注册。

### 2.1 注册「接收节点」（对应 st2110_record / av_txrx_demo recv）

在**能访问 192.168.1.200 的机器**上（本机或与 Registry 同网段的另一台）：

```bash
cd /path/to/GPT_mtl_encode_sdk
export REGISTRY_URL=http://192.168.1.200

# 一次性注册（默认 --mode receiver）
python3 routing/scripts/register_node_example.py

# 或带心跳 + 若要在 admin 里对该 Receiver 做 Connect/Staged/Active，必须用 --href 指向本机可达地址 + --save-config
python3 routing/scripts/register_node_example.py --heartbeat --interval 30 \
  --href http://192.168.1.201:9090/ --save-config .nmos_node.json
```

将上面的 `192.168.1.201` 换成**运行注册脚本和 IS-05 服务的那台机器的 IP**（从打开 admin 的浏览器所在网络要能访问）。注册成功后，在 **http://192.168.1.200/admin** 的 **Nodes / Receivers** 中应能看到该节点。

**若在 admin 里无法对自研 Receiver 做 Connect/Active/Staged**：多半是 `--href` 不可达（如用了默认 127.0.0.1 且从别处打开 admin）或未运行 IS-05 服务。详见 [RECEIVER_CONNECT_TROUBLESHOOTING.md](RECEIVER_CONNECT_TROUBLESHOOTING.md)。

### 2.2 注册「发送节点」或「收发一体节点」（需求3 自研发送SDK端）

若要在 Controller 里也看到**自研发送端**（Sender），使用 `--mode sender` 或 `--mode both`：

```bash
export REGISTRY_URL=http://192.168.1.200

# 仅发送节点（Node + Device + Source + Flow + Sender）
python3 routing/scripts/register_node_example.py --mode sender --heartbeat --interval 30 \
  --href http://<本机IP>:9090/

# 同一节点同时带 Receiver 与 Sender（需求3 完整：自研收发端都在 admin 可见）
python3 routing/scripts/register_node_example.py --mode both --heartbeat --interval 30 \
  --href http://<本机IP>:9090/ --save-config .nmos_node.json
```

只要自研的 Node 向 **REGISTRY_URL=http://192.168.1.200** 注册，就会和 virtnode、外购设备一起在 Controller 里被**发现**。

---

## 三、在 Controller 里做什么（发现、连接与管理）

1. **发现**  
   - 打开 http://192.168.1.200/admin，在 **Nodes**、**Devices**、**Senders**、**Receivers** 等视图中查看。  
   - 自研节点注册成功后，会出现在列表中；**nmos-virtnode** 的虚拟 Sender/Receiver 也会出现。

2. **连接（路由）**  
   - 在 Controller 中可执行 **Sender → Receiver** 的连接（IS-05）。  
   - 若**自研节点未实现 IS-05 服务端**：在 Controller 里点「连接」只会更新 Registry/Controller 状态，**不会**自动驱动本仓库的 `st2110_record`/`st2110_send` 起停或改组播参数。  
   - 实际收流仍用命令行参数（如 `st2110_record --ip 239.0.0.1 ...`）。  
   - 若要「点击连接即驱动 MTL 收流」，需在自研节点上实现 **IS-05 服务端**（见 [IS05_SERVER_IMPLEMENTATION.md](IS05_SERVER_IMPLEMENTATION.md)）。

3. **管理**  
   - 在 admin 界面可查看、管理所有已注册 Node/Sender/Receiver，实现「统一管理」的界面需求。

---

## 四、外购 ST2110 编解码器（支持 NMOS）的接入

若你有**外购 ST2110 编解码器**且支持 NMOS：

1. 在设备上将其 **NMOS Registry 地址** 配置为 **http://192.168.1.200**（或 `http://192.168.1.200:80`）。  
2. 设备启动后会向该 Registry 做 IS-04 注册。  
3. 在 **http://192.168.1.200/admin** 中，外购设备的 Node/Sender/Receiver 会与**自研节点**、**nmos-virtnode** 一起显示，实现「自研发送/接收 SDK 端与外购标准 ST2110 编解码器的**发现、连接与管理**」。

---

## 五、与需求.md 路由管理部分的对应关系

| 需求（需求.md 路由管理） | 实现方式 |
|--------------------------|----------|
| **符合 AMWA NMOS（IS-04/IS-05/IS-06/IS-08）** | Easy-NMOS 提供 Registry + Controller，支持 IS-04 注册/发现、IS-05 连接；本仓库通过 routing 脚本向同一 Registry 注册。 |
| **ST2110 音视频 IP 流的集中注册、发现、调度、路由与监控** | 注册到 192.168.1.200 的节点均在 Controller 中集中展示；路由通过 IS-05 连接操作完成；监控通过 Controller 界面查看。 |
| **可视化界面，支撑播出系统、ST2110 网关与 IP 化播控统一管理** | **http://192.168.1.200/admin** 即该可视化界面，用于统一管理。 |
| **支持自研发送 SDK 端、接收 SDK 端和外购 ST2110 编解码器（支持 NMOS）的发现、连接与管理** | **发现**：自研节点通过 `register_node_example.py` 向 192.168.1.200 注册后，在 admin 中可见；外购设备将 Registry 设为 192.168.1.200 后同样可见。**连接**：在 admin 中执行 Sender–Receiver 连接（IS-05）；若自研节点实现 IS-05 服务端，连接可驱动 MTL 实际收流。**管理**：所有已注册节点在 admin 中统一管理。 |

**结论**：在你已运行 Easy-NMOS（Registry 192.168.1.200、admin 可用）的前提下，按上述步骤完成**自研节点注册**和（可选）**外购设备 Registry 配置**，即可在「路由管理」层面满足需求.md 中关于发现、连接与统一管理的要求；若要「点击连接即驱动自研收流」，需再实现 IS-05 服务端（见 [IS05_SERVER_IMPLEMENTATION.md](IS05_SERVER_IMPLEMENTATION.md)）。

---

## 六、操作步骤速查（你的环境）

| 步骤 | 操作 |
|------|------|
| 1 | 浏览器打开 http://192.168.1.200/admin，确认 Controller 可用。 |
| 2 | `export REGISTRY_URL=http://192.168.1.200` |
| 3 | `cd /path/to/GPT_mtl_encode_sdk`，执行 `python3 routing/scripts/register_node_example.py --heartbeat --interval 30`（建议后台运行）。 |
| 4 | 在 admin 的 Nodes/Receivers 中确认自研接收节点出现。 |
| 5 | 实际收发仍用 `st2110_record` / `st2110_send` 或 `av_txrx_demo` 命令行参数；NMOS 用于发现与（实现 IS-05 后的）连接驱动。 |
| 6 | 外购设备：将其 Registry 配置为 http://192.168.1.200，即可在 admin 中与自研节点一起发现、连接与管理。 |

更多细节见 [EASY_NMOS_IMPLEMENTATION.md](EASY_NMOS_IMPLEMENTATION.md)、[ROUTING.md](ROUTING.md)。  
自研 Receiver 无法在 admin 中 Connect/Active/Staged 的排查见 [RECEIVER_CONNECT_TROUBLESHOOTING.md](RECEIVER_CONNECT_TROUBLESHOOTING.md)。
