# 路由管理配置：Easy-NMOS 独立部署（192.168.1.200）

当 **Easy-NMOS 运行在单独一台机器**（192.168.1.200），本机为发送端（192.168.1.100）、第二台为接收端（192.168.1.110）时，按以下步骤配置，使 Easy-NMOS 的 admin 能发现发送与接收节点。

---

## 一、角色与地址

| 机器 | IP | 角色 |
|------|-----|------|
| **Easy-NMOS 服务器** | 192.168.1.200 | Registry + admin（发现节点、执行连接） |
| **本机** | 192.168.1.100 | 自研发送：注册为 Sender，运行 `st2110_send` |
| **第二台电脑** | 192.168.1.110 | 自研接收：注册为 Receiver，运行 IS-05、收流 |

要求：三台机器**网络互通**（同网段或路由可达）；本机和第二台都能访问 `http://192.168.1.200`；第二台的 IS-05 端口（如 9090）对 192.168.1.200 或本机可达（admin 做 Connect 时会访问该端口）。

---

## 二、本机（192.168.1.100）— 发送端

### 2.1 向 Easy-NMOS Registry 注册为 Sender

**所有注册都指向 Easy-NMOS 的地址**，即 `REGISTRY_URL=http://192.168.1.200`。

在本机项目目录执行：

```bash
cd /path/to/GPT_mtl_encode_sdk
export REGISTRY_URL=http://192.168.1.200

python3 routing/scripts/register_node_example.py --mode sender \
  --heartbeat --interval 30 \
  --href http://192.168.1.100:9090/
```

- `REGISTRY_URL` 必须是 **Easy-NMOS 的地址**（192.168.1.200），不是本机。
- `--href` 填**本机的 IP + IS-05 端口**，供 admin/Controller 访问本机节点；若 9090 被占用可改为 9091 等。
- 保持该脚本**常驻或后台运行**，否则 Registry 会因心跳超时剔除节点。

### 2.2 本机网卡与发流

为本机发流网卡配置 IP（若尚未配置），例如：

```bash
# 网卡名以 ip link show 为准，例如 enp4s0
sudo ip addr add 192.168.1.100/24 dev enp4s0
```

发流时组播地址/端口需与接收端一致（如 **239.0.0.1:5004**）：

```bash
cd /path/to/GPT_mtl_encode_sdk/build

./st2110_send --url yuv420p10le_1080p.yuv --width 1920 --height 1080 \
  --duration 60 --audio-port 0 \
  --ip 239.0.0.1 --video-port 5004 \
  --port kernel:enp4s0 --sip 192.168.1.100 --no-ptp
```

---

## 三、第二台电脑（192.168.1.110）— 接收端

### 3.1 网络与项目

- 第二台能 **ping 通 192.168.1.200** 和 **192.168.1.100**，能访问 **http://192.168.1.200**。
- 在第二台克隆本仓库，安装依赖：`pip install flask requests`。
- 为收流网卡配置 IP，例如：`sudo ip addr add 192.168.1.110/24 dev enp6s0`（接口名以实际为准）。

### 3.2 向 Easy-NMOS Registry 注册为 Receiver

在**第二台**执行，Registry 同样指向 **192.168.1.200**：

```bash
cd /path/to/GPT_mtl_encode_sdk
export REGISTRY_URL=http://192.168.1.200

python3 routing/scripts/register_node_example.py --mode receiver \
  --heartbeat --interval 30 \
  --href http://192.168.1.110:9090/ \
  --save-config .nmos_node.json
```

- `--href` 必须是**第二台的 IP + IS-05 端口**，这样在 admin 中做 Connect 时，请求会发到第二台。
- 建议常驻或后台运行以维持心跳。

### 3.3 第二台运行 IS-05 服务

在第二台另开终端：

```bash
cd /path/to/GPT_mtl_encode_sdk
python3 routing/is05_server/app.py
```

默认监听 9090；第二台防火墙需放行 9090，以便 192.168.1.200 或本机通过 admin 访问。

### 3.4 第二台收流（任选其一）

**方式 A：直接 st2110_record（与发送端参数一致）**

```bash
cd /path/to/GPT_mtl_encode_sdk/build

./st2110_record --ip 239.0.0.1 --video-port 5004 --audio-port 0 \
  --width 1920 --height 1080 --max-frames 3000 recv.mp4 \
  --port kernel:enp6s0 --sip 192.168.1.110 --no-ptp
```

**方式 B：admin 里 Connect + is05_receiver_daemon**

1. 第二台运行：`./build/is05_receiver_daemon`
2. 在 **http://192.168.1.200/admin** 对第二台 Receiver 执行 Connect，目标组播 239.0.0.1、端口 5004。
3. 本机按上面步骤启动 `st2110_send`。

---

## 四、在 Easy-NMOS 中查看与操作

- 在任意能访问 192.168.1.200 的浏览器打开：**http://192.168.1.200/admin**。
- **Nodes**：应能看到本机（发送节点）和第二台（接收节点）。
- **Senders**：本机的「Video Sender (ST2110)」。
- **Receivers**：第二台的「Video Receiver (ST2110)」。
- 在 Receivers 中可对第二台执行 **Connect**，选择本机 Sender 或填写 239.0.0.1:5004。

若看不到节点：确认本机/第二台注册脚本在跑、`REGISTRY_URL=http://192.168.1.200`、本机与第二台能访问 192.168.1.200。

---

## 五、操作顺序速查（Easy-NMOS 在 200、本机发、第二台收）

| 步骤 | 位置 | 操作 |
|------|------|------|
| 1 | 任意 | 确认 Easy-NMOS 运行，浏览器可打开 **http://192.168.1.200/admin** |
| 2 | 本机 192.168.1.100 | `export REGISTRY_URL=http://192.168.1.200`，运行注册脚本 `--mode sender`，保持心跳 |
| 3 | 本机 | 网卡配 IP（若需要），如 192.168.1.100/24 on enp4s0 |
| 4 | 第二台 192.168.1.110 | 网卡配 IP，能 ping 通 192.168.1.200 和 192.168.1.100 |
| 5 | 第二台 | `export REGISTRY_URL=http://192.168.1.200`，运行注册脚本 `--mode receiver`，`--href http://192.168.1.110:9090/`，保持心跳 |
| 6 | 第二台 | 运行 IS-05：`python3 routing/is05_server/app.py`，放行 9090 |
| 7 | 第二台 | 启动收流：`st2110_record --ip 239.0.0.1 --video-port 5004 ...` |
| 8 | 本机 | 启动发流：`st2110_send --url ... --ip 239.0.0.1 --video-port 5004 ...` |
| 9 | 浏览器 | 打开 **http://192.168.1.200/admin**，在 Nodes/Senders/Receivers 中查看并操作 |

---

## 六、关键点小结

- **REGISTRY_URL**：两台机器都设为 **http://192.168.1.200**（Easy-NMOS 的地址）。
- **Admin 界面**：**http://192.168.1.200/admin**（不是本机 100）。
- **本机注册**：`--href http://192.168.1.100:9090/`。
- **第二台注册**：`--href http://192.168.1.110:9090/`。
- 收发组播参数一致：例如 **239.0.0.1:5004**。
