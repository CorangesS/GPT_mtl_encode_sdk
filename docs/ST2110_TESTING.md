# ST2110 收发测试说明

本文说明如何使用本仓库完成**本机两进程**与**两机**ST2110 数据流收发测试。

## 一、本机两进程收发测试（方式 A）

使用两个进程：一个只发送，一个只接收。

### 1. Mock 模式（无需真实 MTL/DPDK）

- **进程 1（发送）**：`./st2110_send --mock --duration 5`
- **进程 2（接收）**：`./st2110_record --max-frames 150 out.mp4`

在 mock 下，发送端不真正发网络包，接收端自行生成测试帧并编码，用于验证两程序均可独立运行。**真实本机自发自收需使用真实 MTL 后端。**

### 2. 真实 MTL 模式（需已安装 MTL + DPDK）

构建时开启 MTL 后端并配置 MTL/DPDK 路径：

```bash
cmake .. -DMTL_SDK_WITH_MTL=ON -DMTL_SDK_USE_ST_API=ON
# 在 CMakeLists.txt 中为 mtl_sdk 配置 include 与 link（如 /home/MTL/include、/home/MTL/build）
cmake --build .
```

本机两进程：

- **进程 1（发送）**：  
  `./st2110_send --ip 239.0.0.1 --video-port 5004 --audio-port 5006 --duration 10`  
  （若用 DPDK 网卡，需在配置中把 `ports[0]` 改为实际 BDF 或 kernel 网卡及 SIP）

- **进程 2（接收）**：  
  `./st2110_record --ip 239.0.0.1 --video-port 5004 --audio-port 5006 --max-frames 300 recv.mp4`

两进程使用相同组播地址与端口，接收端将收到的流编码为 `recv.mp4`。

## 二、两机收发测试

- **发送机（A 机）**：  
  `./st2110_send --ip 239.0.0.1 --video-port 5004 --audio-port 5006 --duration 60`  
  （`--ip` 为组播地址，需与接收机一致；A 机需配置正确网卡与 SIP）

- **接收机（B 机）**：  
  `./st2110_record --ip 239.0.0.1 --video-port 5004 --audio-port 5006 --max-frames 1800 recv.mp4`

要求：两机网络互通，组播可达（同一二层或组播路由正确）；若使用 PTP，两机需在同一 PTP 域。

## 三、参数对应关系

| 参数       | st2110_send     | st2110_record   | 说明           |
|------------|-----------------|-----------------|----------------|
| 组播 IP    | `--ip`          | `--ip`          | 两端必须一致   |
| 视频端口   | `--video-port`  | `--video-port`  | 两端必须一致   |
| 音频端口   | `--audio-port`  | `--audio-port`  | 两端必须一致   |
| 发送时长   | `--duration`    | -               | 发送端运行秒数 |
| 接收帧数   | -               | `--max-frames`  | 接收端收满后退出 |

默认：IP `239.0.0.1`，视频端口 5004，音频端口 5006。

## 四、与需求.md 的对应

- **ST2110 收发 SDK**：视频流发送与接收均支持 PTPv2（MTL 内置或外部时间源）；SDP 解析与导入/导出由 mtl_sdk 提供；本机两进程与两机测试即“收发”能力验证。
- **编码 SDK**：接收端使用 encode_sdk 将 ST2110 原始流编码为 MP4/MXF，满足需求文档编码部分。
