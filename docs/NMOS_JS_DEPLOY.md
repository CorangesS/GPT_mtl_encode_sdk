# NMOS-JS 部署与配置：满足路由管理软件详细需求

本文说明 **NMOS-JS 安装到哪里**、需要哪些**配置与依赖**，以及如何与 ST2110 收发测试、自研节点配合，完成 **需求.md** 中的「路由管理软件详细需求」。

---

## 一、路由管理软件详细需求（需求.md 摘录）

1. 符合 AMWA NMOS 标准体系（IS-04 / IS-05 / IS-06 / IS-08）  
2. 支持对 ST 2110 音视频 IP 流的集中注册、发现、调度、路由与监控管理  
3. 需要有**可视化操作界面**，支撑播出系统、ST 2110 网关与 IP 化播控环境的统一管理：支持**自研发送 SDK 端、接收 SDK 端**和**外购标准 ST2110 编解码器（支持 NMOS）**的发现、连接与管理  

---

## 二、NMOS-JS 在整体中的角色

- **NMOS-JS** 是 NMOS 的**客户端 Web 应用**（[sony/nmos-js](https://github.com/sony/nmos-js)），提供：
  - **IS-04**：发现（浏览 Registry 中的 Node / Device / Sender / Receiver 等）
  - **IS-05**：连接管理（在界面上执行 Sender–Receiver 连接，即“路由”）
  - **可视化界面**：即需求中的“可视化操作界面”
- 它**不实现** Registry，也不实现设备；需要与 **NMOS Registry** 和**自研/外购节点**配合使用。

因此：  
**把 NMOS-JS 安装到“能通过浏览器访问”的任意位置即可**（例如某台机器上的静态站点目录或与 nmos-cpp-registry 的 admin 目录）。关键不是“安装路径”本身，而是：  
1）NMOS-JS 的**构建产物**被某 Web 服务托管；  
2）NMOS-JS 的**配置**指向同一套 **NMOS Registry**；  
3）自研收发端与外购设备都向该 Registry **注册**，并在 NMOS-JS 中可见、可连接。

---

## 三、NMOS-JS 安装到哪里、如何部署

### 3.1 “安装”的含义

NMOS-JS 是前端项目（Node.js 构建），“安装”包含：**克隆 → 安装依赖 → 构建 → 将构建结果放到可被 Web 服务器访问的目录**。

### 3.2 推荐安装位置（二选一或并存）

| 方式 | 安装/部署位置 | 说明 |
|------|----------------|------|
| **A. 独立 Web 服务** | 任意目录，例如 `/var/www/nmos-js` 或 `C:\inetpub\wwwroot\nmos-js` | 将 `nmos-js` 的 **build 输出** 拷贝到该目录，用 Nginx/Apache/IIS 等提供静态访问。浏览器访问 `http://<该机IP>/nmos-js/` 即可打开界面。 |
| **B. 与 nmos-cpp-registry 一起** | nmos-cpp-registry 的 **admin 目录**（与 registry 可执行文件同级的 `admin/`） | 许多 NMOS Registry 实现（如 [nmos-cpp](https://github.com/sony/nmos-cpp)）会从 `admin/` 提供控制面 UI。将 NMOS-JS 的 **build 内容** 拷贝到该 `admin/` 目录后，访问 Registry 的 admin 地址即可打开 NMOS-JS 界面。 |

**结论**：  
- 若你**只有 NMOS-JS**：安装到**任意 Web 可访问目录**（如 `/var/www/nmos-js`），保证该机端口（如 80/443）可被操作员浏览器访问即可。  
- 若你**使用 nmos-cpp-registry**：把 NMOS-JS 的 build 拷贝到 **Registry 的 admin 目录**，即可与 Registry 一起提供“注册 + 发现 + 连接管理”的可视化界面，满足需求中的“集中注册、发现、调度、路由与监控”和“可视化操作界面”。

### 3.3 构建与部署命令示例（Linux）

```bash
# 1) 克隆 NMOS-JS（可放在任意目录，例如 /opt 或 本仓库 routing 外）
git clone https://github.com/sony/nmos-js.git
cd nmos-js

# 2) 安装依赖（需 Node.js 14+）
npm install

# 3) 配置 Registry 地址（见下节）后构建
npm run build

# 4) 部署：将 build 目录内容拷贝到目标位置
# 方式 A：独立站点
sudo mkdir -p /var/www/nmos-js && sudo cp -r build/* /var/www/nmos-js/

# 方式 B：nmos-cpp-registry 的 admin
# 假设 nmos-cpp-registry 在 /opt/nmos-cpp-registry，则：
# sudo cp -r build/* /opt/nmos-cpp-registry/admin/
```

Windows 下用相同 `git clone`、`npm install`、`npm run build`，再将 `build\*` 拷贝到 IIS 或 Nginx 的站点目录即可。

---

## 四、NMOS-JS 配置与依赖（适配收发测试与路由需求）

### 4.1 必须配置：Registry 地址

NMOS-JS 需要知道 **NMOS Registry** 的 Base URL，才能做 IS-04 发现与 IS-05 连接。

- 常见方式（视 nmos-js 版本而定）：
  - **环境变量**：如 `REST_API_HOST=http://<Registry 主机>:8235` 或 `REGISTRY_URL=...`（以 nmos-js 仓库文档或源码为准）。
  - **构建时配置**：在 `npm run build` 前设置上述环境变量，或修改项目内的 config 文件（若有）。
- **推荐**：Registry 与 NMOS-JS 部署在同一内网；Registry 使用固定 IP 或主机名，NMOS-JS 配置为该地址（如 `http://192.168.1.100:8235`），以便所有操作员浏览器和自研节点都能访问同一 Registry。

### 4.2 与 ST2110 收发测试的适配关系

- **收发测试**（本机两进程或两机）：使用本仓库的 **st2110_send** / **st2110_record** 与标准 MTL 库即可，**不依赖 NMOS-JS**。  
- **路由管理需求**：需要 **Registry + NMOS-JS + 自研节点注册 + IS-05**。  
  - NMOS-JS **不需要改动源码**即可适配：只要配置好 Registry URL，并在同一 Registry 上注册自研的 Sender/Receiver（以及外购设备），即可在 NMOS-JS 中看到并做连接管理。  
  - 自研收发端（st2110_send / st2110_record 或集成 mtl_sdk 的应用）需通过 **routing 适配层**向 Registry 注册（IS-04），并在收到 IS-05 连接请求时驱动 MTL SDK 的 `St2110Endpoint` 与收发会话（参见 `docs/ROUTING.md`）。

### 4.3 依赖小结

| 依赖 | 说明 |
|------|------|
| **Node.js** | NMOS-JS 构建需要（通常 14+ 或 18+，见 nmos-js 文档）。 |
| **NMOS Registry** | 必须先行部署（如 nmos-cpp Registry），NMOS-JS 仅作为其客户端。 |
| **浏览器** | 操作员通过浏览器访问 NMOS-JS 提供的界面。 |
| **自研节点** | 本仓库 mtl_sdk 收发端需向同一 Registry 注册并实现/代理 IS-05，才能在 NMOS-JS 中“发现、连接与管理”。 |

---

## 五、如何完成“路由管理软件详细需求”

1. **部署 NMOS Registry**（如 nmos-cpp-registry），保证其 IS-04/IS-05 接口可用。  
2. **安装并配置 NMOS-JS**：  
   - 构建后将 build 部署到 **Web 可访问目录**（如 `/var/www/nmos-js` 或 Registry 的 `admin/`）；  
   - 配置 Registry Base URL，使 NMOS-JS 仅通过配置即可连接该 Registry（无需修改 NMOS-JS 源码）。  
3. **自研发送/接收 SDK 端**：  
   - 使用本仓库 **mtl_sdk**（含 TX/RX）完成 ST2110 收发测试；  
   - 通过 **routing 适配层**（或脚本/服务）向 Registry 注册 Node/Device/Sender/Receiver（IS-04），并实现或代理 IS-05，使 NMOS-JS 上的“连接”操作能驱动 MTL 的 `St2110Endpoint` 与收发会话。  
4. **外购 ST2110 编解码器**：将其配置为向**同一 Registry** 注册，即可在 NMOS-JS 中与自研节点一起发现、连接与管理。  

按以上步骤，**NMOS-JS 无需修改**，只需**正确安装到可访问目录并配置 Registry 地址**，即可与 Registry、自研节点、外购设备一起满足需求.md 中的路由管理软件详细需求。
