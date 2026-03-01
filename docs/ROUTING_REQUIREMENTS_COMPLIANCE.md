# 路由管理需求符合性检查（需求.md 第 17–18 条）

本文对照 **需求.md** 第 17、18 条，检查当前路由管理部分的实现情况。  
前提：已部署 **Easy-NMOS**，具备可视化界面（`/admin`）。

---

## 需求原文

- **第 17 条**：支持对 ST 2110 音视频 IP 流的**集中注册、发现、调度、路由与监控管理**
- **第 18 条**：需要有**可视化操作界面**，支撑播出系统、ST 2110 网关与 IP 化播控环境的统一管理：支持**自研发送 SDK 端、接收 SDK 端和外购标准 ST2110 编解码器**（支持 NMOS）的**发现、连接与管理**

---

## 一、需求 2：集中注册、发现、调度、路由与监控

| 子项       | 实现情况 | 说明 |
|------------|----------|------|
| **集中注册** | ✅ 已实现 | 自研节点通过 `routing/scripts/register_node_example.py` 向同一 Registry（如 Easy-NMOS）注册；外购设备将 Registry 指向同一地址即可集中注册。 |
| **发现**     | ✅ 已实现 | Easy-NMOS Controller（`/admin`）通过 Registry 的 IS-04 查询展示所有 Node/Device/Sender/Receiver，自研接收节点、外购设备、虚拟节点均可被发现。 |
| **调度**     | ⚠️ 部分满足 | 需求中的「调度」在本项目中理解为「谁连谁」的调度，即通过 Controller 上的 **Sender→Receiver 连接（IS-05）** 完成。无独立「按时间表自动切换」等高级调度功能；若需求包含后者，需额外开发或依赖其他系统。 |
| **路由**     | ✅ 已实现 | 在 admin 中执行 Sender–Receiver 连接即 IS-05 路由；自研接收端提供 `routing/is05_server`，连接可驱动 MTL 收流（配合 `is05_receiver_daemon`）。 |
| **监控管理** | ✅ 已实现 | 通过 Easy-NMOS admin 界面查看节点列表、连接状态等，实现集中监控与管理。 |

**结论（需求 2）**：集中注册、发现、路由、监控已实现；「调度」若指连接级调度则已满足，若指时间表/策略调度则未实现。

---

## 二、需求 3：可视化界面 + 自研/外购设备的发现、连接与管理

| 子项               | 实现情况 | 说明 |
|--------------------|----------|------|
| **可视化操作界面** | ✅ 已实现 | 你已部署 Easy-NMOS，其 `/admin` 即可视化界面，支撑统一管理。 |
| **自研接收 SDK 端** | ✅ 已实现 | `register_node_example.py` 注册 Node/Device/**Receiver**；在 admin 中可发现、连接（IS-05）、管理；IS-05 服务端 + daemon 可实现「点击连接即收流」。 |
| **自研发送 SDK 端** | ✅ 已实现 | 使用 `register_node_example.py --mode sender` 或 `--mode both` 可注册 **Source + Flow + Sender**，自研发送端在 admin 的 Senders 列表中可见，满足需求3「自研发送SDK端…发现、连接与管理」。 |
| **外购 ST2110 编解码器** | ✅ 已支持 | 外购设备将 NMOS Registry 指向同一 Easy-NMOS 地址即可注册，在 admin 中与自研节点一起发现、连接与管理，无需本仓库额外开发。 |

**结论（需求 3）**：可视化界面、自研接收端、**自研发送端**、外购设备的发现/连接/管理均已满足。

**自研 Receiver 在 admin 中无法操作 Active/Staged/Connect** 时，多为 `--href` 不可达或未运行 IS-05 服务，详见 [RECEIVER_CONNECT_TROUBLESHOOTING.md](RECEIVER_CONNECT_TROUBLESHOOTING.md)。

---

## 三、汇总：是否「完整实现」

| 需求条款 | 完整实现？ | 说明 |
|----------|------------|------|
| **第 17 条**（集中注册、发现、调度、路由与监控） | 基本完整 | 仅「调度」若指时间表/策略级功能则未实现；连接级调度已具备。 |
| **第 18 条**（可视化 + 自研收发端 + 外购设备的发现、连接与管理） | ✅ 已实现 | 自研收发端通过 `--mode receiver|sender|both` 注册；Receiver 需正确 `--href` 并运行 IS-05 方可在 admin 中 Connect。 |

---

## 四、使用说明摘要

1. **自研接收节点**：`python3 routing/scripts/register_node_example.py [--mode receiver] --href http://<节点IP>:9090/ --save-config .nmos_node.json`，并运行 `routing/is05_server/app.py`，方可在 admin 中对 Receiver 做 Connect/Staged/Active。
2. **自研发送节点**：`python3 routing/scripts/register_node_example.py --mode sender --href http://<节点IP>:9090/`，在 admin 的 Senders 中可见。
3. **收发一体**：`--mode both` 可同时注册 Receiver 与 Sender。
