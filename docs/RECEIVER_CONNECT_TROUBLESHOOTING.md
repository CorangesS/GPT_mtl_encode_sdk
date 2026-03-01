# 自研 Receiver 无法在 Admin 中操作 Active/Staged/Connect 的说明与解决

在 Easy-NMOS 的 **Receivers** 界面中，你可能看到：

- **easy-nmos-node** 下的虚拟 Receiver（如 c1、s1、b1）可以正常进行 **Active**、**Staged**、**Connect** 等操作；
- 自研的 **Video Receiver (ST2110)** 节点虽然能显示，但无法修改 Active/Staged 或无法完成 Connect。

本文说明原因及正确配置方式。

---

## 一、为什么 easy-nmos 的 c1/s1/b1 可以操作，自研的不行？

### 1. Controller 如何发起「连接」请求

Admin 界面（Controller）在浏览器里运行。当你在界面上对某个 **Receiver** 执行「连接」或修改 Staged/Active 时：

1. Controller 会从 **Registry** 查到该 Receiver 所属的 **Node**；
2. 根据 Node 的 **`href`**（或 `api.endpoints`）得到该节点的「连接 API 基地址」；
3. 由**浏览器**向该地址发起 IS-05 请求，例如：
   - `GET {href}x-nmos/connection/v1.1/single/receivers/{receiver_id}/staged`
   - `PATCH {href}x-nmos/connection/v1.1/single/receivers/{receiver_id}/staged`
   - `GET {href}x-nmos/connection/v1.1/single/receivers/{receiver_id}/active`

也就是说：**IS-05 的请求是从「你打开 admin 的那台浏览器」发到「Node 的 href 所指的那台机器」**。

### 2. easy-nmos 自带的 c1、s1、b1 为什么可以操作？

Easy-NMOS 的虚拟节点（nmos-virtnode）与 Registry/Controller 通常部署在同一套服务中。这些虚拟节点的 **Connection API（IS-05）由 Easy-NMOS 自身实现**，并且其 Node 的 `href` 指向本机或同一网络内可达的地址。因此：

- 浏览器请求的 `href` 会落到 Easy-NMOS 所在的服务；
- 该服务上有完整的 IS-05 实现，所以 Staged/Active/Connect 都能正常使用。

### 3. 自研「Video Receiver (ST2110)」为什么不能操作？

自研节点是由 `register_node_example.py` 注册的。注册时有一个关键参数：**`--href`**，默认是 `http://127.0.0.1:9090/`。

- **若你用默认 `--href http://127.0.0.1:9090/` 注册**：  
  Controller 会认为「该节点的 IS-05 接口在 `http://127.0.0.1:9090/`」。  
  - 请求是从**浏览器**发出的，因此 `127.0.0.1` 表示的是**你当前打开 admin 的那台电脑的本机**，而不是运行 `register_node_example.py` 或 IS-05 服务的那台机器。  
  - 若你是在**另一台 PC** 上打开 `http://192.168.1.200/admin`，则请求会发到**你那台 PC** 的 9090 端口，那里没有 IS-05 服务，连接会失败或无法操作。
- **若自研节点所在机器上没有运行 IS-05 服务**（`routing/is05_server/app.py`），即使 href 填对，也没有进程响应 IS-05，Controller 同样无法完成 Staged/Active/Connect。

因此会出现：**c1/s1/b1 能操作，自研 Receiver 不能操作**。

---

## 二、解决思路（三步）

要让自研 **Video Receiver (ST2110)** 在 admin 里也能正常使用 Active/Staged/Connect，需同时满足下面三点。

### 1. 注册时使用「对浏览器可达」的 href

`--href` 必须是**运行 IS-05 服务的那台机器的地址**，且该地址必须**从「打开 admin 的浏览器」所在网络可访问**。

- 若 **Registry/Easy-NMOS 和自研节点、IS-05 服务都在同一台机**（例如 192.168.1.200），且你**在同一台机上**用浏览器打开 admin，可以用：
  ```bash
  --href http://192.168.1.200:9090/
  ```
  或本机访问时用 `http://127.0.0.1:9090/`。
- 若 **自研节点和 IS-05 在另一台机**（例如 192.168.1.201），你从任意 PC 打开 `http://192.168.1.200/admin`，则必须用：
  ```bash
  --href http://192.168.1.201:9090/
  ```
  这样浏览器才会把 IS-05 请求发到 192.168.1.201:9090，即你的 IS-05 服务所在机器。

**不要**在「从其他 PC 访问 admin」的场景下使用默认的 `http://127.0.0.1:9090/`。

### 2. 在自研节点机器上运行 IS-05 服务

必须在 **href 所指的那台机器**上启动 IS-05 服务，并监听 9090 端口（或你在 href 里写的端口）：

```bash
cd /path/to/GPT_mtl_encode_sdk
# 若使用 --save-config .nmos_node.json，确保先完成一次带 --save-config 的注册，以便 IS-05 能读到 receiver_id
python3 routing/is05_server/app.py
```

服务默认监听 `0.0.0.0:9090`。若改端口，需与 `--href` 中的端口一致。

### 3. 让 IS-05 服务知道 receiver_id（与 Registry 一致）

IS-05 服务需要知道当前节点的 **receiver_id**，才能正确响应 Controller 对「该 Receiver」的 Staged/Active 请求。

- **推荐**：注册时使用 `--save-config .nmos_node.json`，并在**运行 IS-05 的同一目录**下启动 `app.py`，这样会自动从 `.nmos_node.json` 读取 `receiver_id`。
- 或设置环境变量：`export IS05_RECEIVER_ID=<注册时的 receiver_id>`。

完成以上三步后，**重新用正确的 `--href` 注册一次**（并保持心跳），再在 admin 的 Receivers 里对自研「Video Receiver (ST2110)」操作 Active/Staged/Connect，即可生效。

---

## 三、推荐注册命令示例（可操作 Connect 的完整流程）

在**运行 IS-05 服务的那台机器**上（例如 192.168.1.201）：

```bash
cd /path/to/GPT_mtl_encode_sdk
export REGISTRY_URL=http://192.168.1.200

# 使用本机在局域网中的 IP 作为 href，便于从任意 PC 打开的 admin 访问
python3 routing/scripts/register_node_example.py \
  --heartbeat --interval 30 \
  --href http://192.168.1.201:9090/ \
  --save-config .nmos_node.json
```

在**同一台机器**上另开终端启动 IS-05 服务：

```bash
cd /path/to/GPT_mtl_encode_sdk
python3 routing/is05_server/app.py
```

然后在**任意能访问 192.168.1.200 的 PC** 上打开 `http://192.168.1.200/admin`，在 Receivers 里找到自研「Video Receiver (ST2110)」，即可进行 Connect、Staged、Active 操作。

---

## 四、小结

| 现象 | 原因 |
|------|------|
| c1/s1/b1 能操作，自研 Receiver 不能 | 自研节点的 `href` 不可达（如用了 127.0.0.1 且从别处打开 admin），或未运行 IS-05 服务，或 IS-05 未配置正确的 receiver_id。 |
| 解决办法 | 用 `--href http://<节点本机 IP>:9090/` 注册；在该机运行 `routing/is05_server/app.py`；用 `--save-config .nmos_node.json` 保证 receiver_id 一致。 |

按上述方式配置后，自研 Receiver 在 admin 中的行为即可与 easy-nmos 自带的 Receiver 一致，支持 Active、Staged、Connect 等操作。
