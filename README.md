# MTL-Encode-SDK

本仓库提供 **ST2110 收发封装**、**编码封装** 与 **路由管理对接**，用于在标准 MTL/DPDK 与 FFmpeg 之上实现需求文档中的 ST2110 收发、编码及 NMOS 路由管理能力。

---

## 一、项目组成

| 模块 | 说明 |
|------|------|
| **mtl_sdk** | 封装 [Media Transport Library (MTL)](https://github.com/OpenVisualCloud/Media-Transport-Library) 的 ST2110 收发：视频/音频 TX/RX、PTPv2/IEEE1588、SDP 解析与文件导入/导出。支持 mock 后端（无 MTL 即可联调）与真实 MTL 后端。 |
| **encode_sdk** | 基于 FFmpeg 的编码与封装：H.264/H.265、AAC/MP2/PCM/AC3，容器 MP4/MXF，与 mtl_sdk 的 `VideoFrame`/`AudioFrame`（timestamp_ns）对接。 |
| **路由管理** | 通过 NMOS Registry + NMOS-JS 实现发现、连接管理与可视化界面；自研收发端通过 routing 适配层注册（IS-04）并响应 IS-05，无需修改 NMOS-JS。 |

需求文档见 [需求.md](需求.md)；逐项符合性见 [docs/COMPLIANCE.md](docs/COMPLIANCE.md)。

---

## 二、前置条件与依赖

- **CMake 3.20+**，**C++17** 编译器。
- **FFmpeg 开发包**（encode_sdk 必需）：  
  - Ubuntu/Debian：`sudo apt install libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev pkg-config`  
  - Fedora：`sudo dnf install ffmpeg-devel pkg-config`
- **使用真实 MTL 收发时**：已编译好的 **MTL** 与 **DPDK**（如 MTL 在 `/home/MTL`，DPDK 在 `/home/DPDK`）；运行前按 MTL/DPDK 文档配置大页、网卡等。

---

## 三、完整流程

### 1. 克隆仓库

```bash
git clone <本仓库 URL> GPT_mtl_encode_sdk
cd GPT_mtl_encode_sdk
```

### 2. 选择构建模式并编译

**方式 A：仅联调（不依赖 MTL/DPDK）**

```bash
mkdir -p build && cd build
cmake .. -DMTL_SDK_WITH_MTL=OFF
cmake --build . -j
```

此时 mtl_sdk 使用 mock 后端，可运行 `st2110_record` 生成测试帧并编码为 MP4，无法真实收发组播。

**方式 B：使用标准 MTL 库（收发测试）**

假设 MTL 根目录为 `/home/MTL`（包含 `include/` 与 `build/` 或 `lib/`）：

```bash
mkdir -p build && cd build
cmake .. \
  -DMTL_SDK_WITH_MTL=ON \
  -DMTL_SDK_USE_ST_API=ON \
  -DMTL_ROOT=/home/MTL
cmake --build . -j
```

若链接时报 DPDK 相关未定义符号，增加：

```bash
cmake .. ... -DDPDK_ROOT=/home/DPDK
```

并在 `CMakeLists.txt` 中按你本机 DPDK 的库名调整 `target_link_libraries`（见该文件中 `DPDK_ROOT` 注释）。

### 3. 运行示例

在 `build` 目录下：

| 程序 | 作用 | 示例命令 |
|------|------|----------|
| **st2110_send** | 向组播发送 ST2110 视频+音频测试流 | `./st2110_send --ip 239.0.0.1 --video-port 5004 --audio-port 5006 --duration 10` |
| **st2110_record** | 接收组播并编码为 MP4 | `./st2110_record --ip 239.0.0.1 --video-port 5004 --audio-port 5006 --max-frames 300 recv.mp4` |

**本机两进程收发测试**：先在一个终端运行 `st2110_send`，再在另一终端用相同 `--ip`/`--video-port`/`--audio-port` 运行 `st2110_record`，接收端会生成 `recv.mp4`。  
**两机测试**：A 机只跑 `st2110_send`，B 机只跑 `st2110_record`，两端组播地址与端口一致，且网络组播可达。

### 4. 路由管理（可选）

需单独部署 **NMOS Registry** 与 **NMOS-JS**，并将自研收发端通过 routing 适配层注册到 Registry。NMOS-JS 部署到任意 Web 可访问目录并配置 Registry 地址即可。详见 [docs/NMOS_JS_DEPLOY.md](docs/NMOS_JS_DEPLOY.md)、[docs/ROUTING.md](docs/ROUTING.md)、[routing/README.md](routing/README.md)。

---

## 四、需求与测试对应

| 需求类别（需求.md） | 测试方式 |
|--------------------|----------|
| **ST2110 收发**：PTPv2、SDP 解析/导入导出、收发能力 | 用方式 B 构建后做本机两进程或两机收发；SDP 需自写小程序调用 `load_sdp_file`/`parse_sdp`/`to_sdp`/`save_sdp_file` 验证。 |
| **编码**：NVENC、H.264/H.265、AAC/MP2/PCM/AC3、MP4/MXF、参数可调、零拷贝、音视频同步 | 运行 `st2110_record` 收流并生成 MP4/MXF，检查可播放与音画同步；修改 `EncodeParams` 可测不同编码格式与容器。 |
| **路由管理**：NMOS、注册发现、可视化界面、自研/外购设备管理 | 部署 Registry + NMOS-JS，自研节点注册（如使用 `routing/scripts/register_node_example.py`），在 NMOS-JS 中做发现与 IS-05 连接。 |

更细的测试步骤见 [docs/ST2110_TESTING.md](docs/ST2110_TESTING.md)。

---

## 五、示例参数说明

- **st2110_send**：`--ip` 组播 IP，`--video-port` / `--audio-port` 端口，`--duration` 发送秒数，`--width`/`--height`/`--fps` 可选；`--mock` 仅 mock 模式下有效。
- **st2110_record**：`--ip` / `--video-port` / `--audio-port` 须与发送端一致；`--max-frames` 收满多少帧后退出；最后可跟输出文件名（默认 `out.mp4`）。  
详细见运行 `./st2110_send --help`、`./st2110_record --help`。

---

## 六、注意事项

1. **MTL 路径**：`MTL_ROOT` 下需有 `include/`（含 `mtl_api.h`）以及 `build/` 或 `lib/`（含 libmtl）。若 MTL 安装在其它路径，请修改 `-DMTL_ROOT=...`。
2. **网卡与 SIP**：使用真实 MTL 时，示例中默认 `ports[0]` 为 `kernel:lo` / `127.0.0.1`，仅适用于本机回环。本机多网卡或两机测试时，需在代码或后续配置中改为实际网卡（如 DPDK BDF 或 `kernel:eth0`）及本机 SIP。
3. **组播**：两机测试时两台机器须组播互通（同二层或组播路由正确）；若用 PTP，两机需在同一 PTP 域。
4. **DPDK**：若 MTL 已静态链接 DPDK，通常无需设置 `DPDK_ROOT`；仅当链接报 `rte_*` 未定义时再设 `DPDK_ROOT` 并核对 `CMakeLists.txt` 中的库名。
5. **编码与输出**：ST2110 传输的是原始 YUV/PCM；`recv.mp4` 是接收端用 encode_sdk 将收到的流**编码后**写入的 MP4，用于验证收流与编码流水线。
6. **路由管理**：本仓库不包含 NMOS Registry 或 NMOS-JS 源码；路由需求通过“部署 Registry + 部署并配置 NMOS-JS + 自研节点注册与 IS-05”完成，详见上述路由文档。

---

## 七、文档索引

| 文档 | 内容 |
|------|------|
| [需求.md](需求.md) | ST2110 收发、编码、路由管理详细需求 |
| [docs/COMPLIANCE.md](docs/COMPLIANCE.md) | 与需求文档的逐项符合性检查 |
| [docs/ST2110_TESTING.md](docs/ST2110_TESTING.md) | 本机两进程与两机 ST2110 收发测试步骤 |
| [docs/ROUTING.md](docs/ROUTING.md) | 路由管理与 MTL-Encode-SDK 的对接方案 |
| [docs/NMOS_JS_DEPLOY.md](docs/NMOS_JS_DEPLOY.md) | NMOS-JS 安装位置、配置与路由管理需求满足方式 |
| [routing/README.md](routing/README.md) | 路由模块与自研节点注册概要 |
