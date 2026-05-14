# NMOS「节点 + 连线」可视化界面 — 实现指南

本文说明如何在本项目与 **Easy-NMOS** 体系下，实现类似 **ComfyUI** 的「发送节点 / 接收节点 + 拖拽或点击连线」控制面，以及它与 **IS-04（发现）**、**IS-05（连接）** 的对应关系。实现目标：用户在画布上选定 **Sender** 与 **Receiver** 并建立一条逻辑连线，底层完成与现有 `/admin` 中 **CONNECT** 等价的 **NMOS 连接**。

---

## 一、目标与边界

### 1.1 目标

- **左侧（或一侧）**：展示 Registry 中可见的 **发送端（Senders）**，每个 Sender 对应画布上的一个「发送节点」。
- **右侧**：展示 **接收端（Receivers）**，每个 Receiver 对应一个「接收节点」。
- **连线**：表示「将该 Receiver 连接到该 Sender」的路由意图；保存或确认时，通过 **IS-05** 对 **Receiver** 的 **staged** 资源执行 **PATCH**（含 `sender_id`、`transport_file` 或 `transport_params` 与 **activation**），与 Easy-NMOS 中手动 CONNECT 的语义一致。
- **可选**：总线式布局（中间竖线、左右分支），仅为视觉布局，不改变 NMOS 语义。

### 1.2 边界说明

- **不修改 nmos-js 源码**：Sony [nmos-js](https://github.com/sony/nmos-js) 与 Easy-NMOS 自带的 `/admin` 是既有 Controller UI；实现新画布界面时，推荐 **独立前端应用** 调用同一套 Registry Query + 节点 IS-05，而不是把 ComfyUI 式交互硬分叉进 nmos-js（升级与合并成本极高）。详见下文「与 nmos-js / Easy-NMOS 的关系」。
- **本仓库已提供**：节点注册（`routing/scripts/register_node_example.py`）、节点侧 IS-05（`routing/is05_server/app.py`）、连接落地（`connection_state.json` + `is05_receiver_daemon` 等）。节点图界面主要新增 **浏览器端展示与编排层**，以及可选 **小型 BFF 代理**（解决跨域与凭证）。

---

## 二、NMOS 概念与界面元素的映射


| 界面概念    | NMOS 资源              | 说明                                                                                                                                                                                                                  |
| ------- | -------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 发送节点    | `Sender`（IS-04）      | 在 Registry Query 中列出；每个 Sender 有 `id`、`label`、`href`（指向节点 Connection API 等）。                                                                                                                                        |
| 接收节点    | `Receiver`（IS-04）    | 同上；连接操作主要针对 **Receiver** 的 staged/active。                                                                                                                                                                           |
| 一条连线    | Sender ↔ Receiver 连接 | 实现上 = 对 **Receiver** 的 `.../single/receivers/{id}/staged` 做 **PATCH**，填入 `sender_id`，并从 **Sender** 侧取 **transportfile（SDP）** 写入 `transport_file`（或等价 `transport_params`），`activation.mode` 常为 `activate_immediate`。 |
| 当前是否已连接 | Receiver **active**  | `GET .../receivers/{id}/active` 可得到当前 `sender_id`、传输参数等，用于画布上高亮或同步连线。                                                                                                                                               |
| 断开      | 再次 PATCH staged      | 例如 `master_enable: false` 或清空 `sender_id`（以实现为准），并激活；本仓库 IS-05 行为见 `routing/is05_server/README.md`。                                                                                                                 |


**要点**：NMOS 里「连接」是 **Receiver 订阅某个 Sender**；画布上 **有向边** 建议画成 **Sender → Receiver**，与媒体流方向一致。

---

## 三、总体架构

### 3.1 分层结构

```text
┌─────────────────────────────────────────────────────────┐
│  节点图 UI（浏览器）                                       │
│  画布：React Flow / Vue Flow / LiteGraph 等              │
│  状态：选中 Sender、选中 Receiver、边列表、与 active 同步   │
└───────────────────────────┬─────────────────────────────┘
                            │ HTTPS（可能跨域）
┌───────────────────────────▼─────────────────────────────┐
│  IS-04 Registry Query API                               │
│  列出 senders / receivers / nodes …                     │
└───────────────────────────┬─────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────┐
│  各节点的 IS-05 Connection API（通常在 Sender/Receiver   │
│  所在主机上，如 http://<节点IP>:9090/...）                 │
│  GET Sender transportfile → PATCH Receiver staged       │
└──────────────────────────────────────────────────────────┘
```

### 3.2 与现有 `/admin` 流程对齐

Easy-NMOS `/admin` 中对接收端执行 **CONNECT** 时，典型步骤为：

1. 从 Registry 或 Query 解析出 Sender、Receiver 的 **href**。
2. 向 **Sender** 节点请求 **transportfile**（SDP）。
3. 向 **Receiver** 节点的 `.../receivers/{receiver_id}/staged` 发起 **PATCH**，携带 `sender_id`、`transport_file`、`activation`（如 `activate_immediate`）。

自研节点侧具体端点与行为见：`routing/is05_server/README.md`（含 **CONNECT** 与 **CORS** 说明）。

节点图应用应 **复现上述顺序**，这样与当前 `connection_state.json`、收流 daemon 的闭环一致。

---

## 四、实现过程（分阶段）

### 阶段 A：环境与数据准备

1. **部署 Registry + Controller**
  - 使用 **Easy-NMOS**（推荐），保证 `http://<Easy-NMOS-IP>/admin` 可访问。  
  - 文档：`docs/需求3_路由管理部署与使用.md`。
2. **注册发送端 / 接收端节点**
  - 使用 `routing/scripts/register_node_example.py`，`--mode sender` / `receiver` / `both`，`--href` 为 **浏览器可达** 的节点 IS-05 根 URL（含端口），`--save-config .nmos_node.json`。  
  - 接收端需运行 `python3 routing/is05_server/app.py`，与 `--href` 端口一致。
3. **确认在 `/admin` 中能完成一次手动 CONNECT**
  - 若此处失败，应先按 `routing/is05_server/README.md` 排查（节点根、`/x-nmos/connection/v1.1/`、CORS 等），再开发自定义 UI。

### 阶段 B：发现 — 拉取 Senders / Receivers 列表

1. **确定 Registry 的 Query API 基地址**
  - 不同发行版路径可能为 `/x-nmos/query/v2.0/` 或经 Controller 转发；可从 Easy-NMOS 文档或实际 `GET` Registry 根响应中确认。
2. **请求示例（概念）**
  - `GET {query_base}/senders`  
  - `GET {query_base}/receivers`  
  - 返回 JSON 数组，元素含 `id`、`label`、`href`（或可从 `href` 推导节点基础 URL）。
3. **在画布上创建节点**
  - 每个 `sender.id` → 一个发送节点；每个 `receiver.id` → 一个接收节点。  
  - 节点上展示 `label` 或 `id` 短码；可折叠按 `device_id` / `node` 分组（需额外 `GET devices` / `nodes`）。
4. **刷新策略**
  - 定时轮询（如 5–10 s）刷新列表，或仅在用户点击「刷新」时拉取，避免过载。

### 阶段 C：连接 — 将「一条边」转为 IS-05 PATCH

以下为实现 **单次连接** 的推荐顺序（与 Controller 逻辑一致）：

1. **解析 Sender 节点基础 URL**
  - 从 Query 返回的 Sender 资源中得到 `href`，截取到节点根或 Connection API 前缀（依字段格式而定）。
2. **获取 SDP**
  - `GET {sender_node}/x-nmos/connection/v1.1/single/senders/{sender_id}/transportfile`  
  - 响应可能是 JSON（含 `data`、`type`）或纯 SDP；需与 `is05_server` 返回格式一致地解析。
3. **构造 PATCH body**
  - 目标：`PATCH {receiver_node}/x-nmos/connection/v1.1/single/receivers/{receiver_id}/staged`  
  - Body 通常包含：  
    - `sender_id`  
    - `transport_file`：`{ "type": "application/sdp", "data": "<SDP字符串>" }`（以实际 API 为准）  
    - `master_enable`: `true`  
    - `activation`: `{ "mode": "activate_immediate" }`
4. **发送 PATCH**
  - 成功则 Receiver **active** 更新，自研节点侧将连接写入 `connection_state.json`（见 `routing/is05_server`）。
5. **错误处理**
  - HTTP 4xx/5xx、SDP 为空、constraints 不满足时，在画布上提示并撤销「待连接」边。

### 阶段 D：画布交互设计

1. **交互模式（二选一或并存）**
  - **ComfyUI 式**：从 Sender 右侧端口拖到 Receiver 左侧端口，松开创建边。  
  - **两次点击式**：第一次点击选中 Sender，第二次点击 Receiver，确认后创建边并触发阶段 C。
2. **多连接与约束**
  - 同一 **Receiver** 在某一时刻通常只有一条 **active** 到某 **Sender**；若用户拖第二条边，策略可以是：覆盖前一条，或先提示断开。  
  - 同一 **Sender** 可被多个 **Receiver** 连接（组播场景常见），需以设备与网络能力为准。
3. **视觉状态**
  - **已连接**：根据 `GET .../receivers/{id}/active` 中的 `sender_id` 高亮对应边。  
  - **连接中 / 失败**：边上显示 loading 或错误色。

### 阶段 E：断开与同步

1. **断开**
  - 对 `.../receivers/{id}/staged` 发 PATCH，将连接清空或 `master_enable: false`，并 `activate_immediate`（具体字段以 IS-05 与本实现为准）。  
  - 实现细节可参考 `docs/需求16-18_具体实现拆解与routing现状.md` 中「断开」一节。
2. **定期同步 active**
  - 轮询各可见 Receiver 的 `active`，刷新画布边状态，避免与 `/admin` 或其他客户端操作冲突。

---

## 五、技术选型建议


| 层次         | 选项                                                                        | 说明                                                     |
| ---------- | ------------------------------------------------------------------------- | ------------------------------------------------------ |
| 画布 / 节点编辑器 | [React Flow](https://reactflow.dev/)（@xyflow/react）、Vue Flow、LiteGraph.js | 负责节点、边、缩放平移、自定义端口；不负责 NMOS。                            |
| 运行时        | React / Vue / Svelte 等                                                    | 与团队栈一致即可。                                              |
| HTTP       | `fetch` / axios                                                           | 调用 Query API 与节点 IS-05。                                |
| 可选 BFF     | 本仓库 small Flask/FastAPI 或 Nginx 反代                                        | 浏览器直连多节点 IS-05 时需处理 **CORS**；若统一走同域后端转发，可简化浏览器配置与密钥管理。 |


---

## 六、跨域（CORS）与部署注意

- 自定义页面若托管在 `http://192.168.1.200`（Easy-NMOS 同机），而 IS-05 在 `http://192.168.1.201:9090`，浏览器会发起 **跨域** 请求。  
- 本仓库 `is05_server` 已添加 CORS 响应头；若仍失败，检查防火墙、混合内容（HTTPS/HTTP）、以及是否需 **预检 OPTIONS**。  
- **生产环境**建议：节点图与 API 走 **HTTPS**，并通过 **BFF** 或 **反向代理** 统一到同域，避免在浏览器中暴露过多节点地址与鉴权复杂度。

---

## 七、与 nmos-js / Easy-NMOS 的关系


| 问题                           | 建议                                                                             |
| ---------------------------- | ------------------------------------------------------------------------------ |
| 是否必须改 nmos-js？               | **否**。nmos-js 是独立仓库的参考 UI；实现 ComfyUI 式界面不必 fork。                               |
| Easy-NMOS 只有一个 `/admin` 怎么办？ | 可 **并行** 部署你的静态站点（或同机另一路径 / 另一端口），共用同一 Registry 与节点；用户从「节点图」或「经典 admin」二选一或混用。 |
| 能否把页面嵌进 Easy-NMOS？           | 若 Easy-NMOS 支持自定义静态资源或反向代理，可将构建后的 SPA 挂到子路径；属部署集成问题，仍无需改 nmos-js 源码。           |


本仓库 `routing/README.md` 已明确：**路由对接可在不修改 NMOS-JS 的前提下完成**；节点图界面属于同一思路的 **新控制面**。

---

## 八、安全与运维

- **内网假设**：实验室可简化；生产应对 Registry Query、节点 IS-05 做 **认证、TLS、访问列表**。  
- **审计**：记录谁在何时对哪条 Receiver 执行了 PATCH（可在 BFF 层记日志）。  
- **幂等**：重复 PATCH 相同 `sender_id`+SDP 应安全；注意 UI 防抖与重试。

---

## 九、测试检查清单

- Query 能列出与 `/admin` 一致的 Senders/Receivers。  
- 对某 Receiver 从画布发起连接后，`GET .../active` 中 `sender_id` 正确。  
- `connection_state.json` 更新，收流 daemon 或 `st2110_record` 行为符合预期。  
- 在 `/admin` 手动改连接后，画布轮询能反映新状态（或手动刷新一致）。  
- 断开连接后 active 清空，底层停流。  
- 双机、跨网段、防火墙场景下浏览器无 CORS/网络错误。

---

## 十、相关文档索引（本仓库）


| 文档                                 | 内容                            |
| ---------------------------------- | ----------------------------- |
| `routing/README.md`                | 路由模块总览、Easy-NMOS 与 nmos-js 关系 |
| `routing/is05_server/README.md`    | IS-05 端点、CONNECT 流程、CORS、排查   |
| `docs/需求3_路由管理部署与使用.md`            | Easy-NMOS 部署与双机角色             |
| `docs/需求16-18_具体实现拆解与routing现状.md` | IS-04/IS-05 需求级拆解与 routing 现状 |
| `docs/双机路由管理配置-本机发送第二台接收.md`       | 双机场景示例                        |


---

## 十一、小结

- **实现过程**可概括为：**用 IS-04 Query 填充画布节点 → 用 IS-05 在 Receiver 上复现 CONNECT（transportfile + PATCH staged）→ 用 active 状态驱动边的显示与同步**。  
- **不需要**为了这种交互去改 nmos-js；建议 **独立 SPA + 可选 BFF**，与现有 Easy-NMOS、`routing/is05_server` 共存。  
- 按阶段 A→E 迭代：先保证 `/admin` 连通，再实现列表与单次连接，最后完善断开、轮询与权限。

---

*文档版本：与仓库 `GPT_mtl_encode_sdk` 路由说明一致；NMOS Query 路径以实际 Registry/Controller 版本为准。*



用一个本地静态服务打开（避免部分浏览器策略问题）：

cd /home/dd/GPT_mtl_encode_sdk/ui-stage-d-min

python3 -m http.server 8088

然后浏览器访问：

- [http://localhost:8088/nmos-stage-d-min.html](http://localhost:8088/nmos-stage-d-min.html)



