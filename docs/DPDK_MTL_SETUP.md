# 双机直连下 DPDK/MTL 配置指南

本文说明在两台电脑直连网卡的情况下，如何配置网络与系统，使 **MTL（Media Transport Library）** 通过 **DPDK** 进行 ST2110 收发。配置完成后，发送端用 `st2110_send`，接收端用 `st2110_record`，`--port` 使用网卡 BDF（如 `0000:af:01.0`）。

**前置**：两台机器已能互相 ping 通（若未通，先按 [TESTING.md](TESTING.md) 或 README 配置静态 IP 与直连网卡）。

---

## 一、两种传输模式简述

| 模式 | `--port` 示例 | 是否需要下列配置 |
|------|----------------|------------------|
| **Kernel** | `kernel:enp3s0` | 否，只需本机 IP 与网卡名 |
| **DPDK/MTL** | `0000:af:01.0`（BDF） | 是，需 IOMMU、大页、VFIO、网卡绑定 |

若你已能 ping 通，可先用 **Kernel 模式** 验证收发（见 [TESTING.md 2.1](TESTING.md#21-使用-kernel-模式两机已能-ping-通时)）。需要更高性能或必须走 DPDK 时，再按本文做 DPDK/MTL 配置。

**常见网卡**：**Intel I226-V**（及 I225 等 IGC 系）在 DPDK 中由 IGC PMD 支持，可按本文用 **vfio-pci 绑定** 使用 DPDK 模式；Intel E810/E830 等 E800 系列则常用 MTL 的 SR-IOV 脚本。绑定步骤见下文 §2.4。

---

## 二、DPDK/MTL 配置（两台机器均需执行）

以下步骤在 **发送端和接收端** 各做一遍（直连使用的那块网卡所在机器上做即可）。涉及 BIOS、内核、权限、大页、网卡绑定，参考 [MTL Run Guide](https://openvisualcloud.github.io/Media-Transport-Library/doc/run.html)。

### 2.1 启用 IOMMU

1. **BIOS**：启用 VT-d（Intel）或等效 IOMMU 选项并保存重启。
2. **内核参数**（Linux）  
   - Ubuntu/Debian：编辑 `/etc/default/grub`，在 `GRUB_CMDLINE_LINUX_DEFAULT` 中追加：
     ```text
     intel_iommu=on iommu=pt
     ```
     然后执行：
     ```bash
     sudo update-grub
     sudo reboot
     ```
   - CentOS/RHEL9：
     ```bash
     sudo grubby --update-kernel=ALL --args="intel_iommu=on iommu=pt"
     sudo reboot
     ```
3. **验证**：重启后执行：
   ```bash
   ls /sys/kernel/iommu_groups/
   ```
   应能看到若干目录；若为空，检查 `cat /proc/cmdline` 是否含上述参数。

### 2.2 允许当前用户访问 VFIO 设备

```bash
getent group 2110 || sudo groupadd -g 2110 vfio
sudo usermod -aG vfio $USER
```

创建 udev 规则（例如 `/etc/udev/rules.d/10-vfio.rules`）：

```text
SUBSYSTEM=="vfio", GROUP="vfio", MODE="0660"
```

重载 udev 并重新登录使 vfio 组生效：

```bash
sudo udevadm control --reload-rules
sudo udevadm trigger
# 重新登录或重启
```

### 2.3 大页（Huge pages）

DPDK/MTL 使用大页减少 TLB 未命中、提升收发性能。**每次重启后需重新配置**（或写入启动脚本 / systemd）。

**2M 大页**（最常用）：

```bash
# 2048 个 2MB 页 ≈ 4GB（常规负载）
sudo sysctl -w vm.nr_hugepages=2048

# 高负载（如多路 1080p60、或遇内存分配失败）可增至 8GB
sudo sysctl -w vm.nr_hugepages=4096
```

若运行时报内存分配失败，可适当增大该值。

**1G 大页**（可选，性能更好，需内核支持）：  
若希望使用 1GB 大页（日志中 “No free 1048576 kB hugepages” 会消失），可分配 1G 大页（数量按需，例如 4 个 = 4GB）：

```bash
# 查看是否支持 1G 大页
grep -E 'pdpe1gb|pse1gb' /proc/cpuinfo

# 分配 1G 大页（例：4 个）
echo 4 | sudo tee /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages
```

**持久化**（重启后自动生效）：在 `/etc/sysctl.d/` 下新增配置文件（如 `90-hugepages.conf`）：

```ini
# 2M 大页，4GB
vm.nr_hugepages = 2048
```

然后执行 `sudo sysctl -p /etc/sysctl.d/90-hugepages.conf` 或重启。

### 2.4 绑定网卡到 DPDK（VFIO）

直连用的 **同一块网卡** 需绑定到 `vfio-pci`，内核将不再使用该网卡（无法 ping 等），故用 **专门用于 MTL 直连的那块网卡**。

0. **加载 vfio-pci 模块**（绑定前必须先执行，否则会报 `Driver 'vfio-pci' is not loaded`）：
   ```bash
   sudo modprobe vfio-pci
   ```
   若需 IOMMU 支持，可先加载：`sudo modprobe vfio`、`sudo modprobe vfio_iommu_type1`。用 `lsmod | grep vfio` 确认已加载。

1. **查看网卡与 BDF**：
   ```bash
   lshw -c network -businfo
   ```
   记下直连网卡对应的 **BDF**（如 `0000:04:00.0`）。也可用 `ip link` 看接口名后，用 `ethtool -i enp4s0` 查看对应 PCI 地址。

2. **先将网卡 down**（否则会报 `routing table indicates that interface is active. Not modifying`）：
   ```bash
   # 把 enp4s0 换成你直连网卡的实际接口名（与 BDF 对应）
   sudo ip link set enp4s0 down
   ```
   若不确定 BDF 对应的接口名：`ls /sys/bus/pci/devices/0000:04:00.0/net/` 会列出该 PCI 设备对应的内核网卡名。

3. **绑定**（按网卡类型选一种）：
   - **Intel I225 / I226-V（IGC）**：DPDK 通过 IGC PMD 支持，使用 **vfio-pci 绑定**（无需 SR-IOV/VF）。在 DPDK 源码目录下执行（MTL 依赖的 DPDK 通常位于 MTL 的 submodule 或 `$MTL_ROOT` 同级的 `dpdk`）：
     ```bash
     # 假设 BDF 为 0000:04:00.0（请换成 lshw 看到的实际 BDF）
     sudo $MTL_ROOT/dpdk/usertools/dpdk-devbind.py --bind=vfio-pci 0000:04:00.0
     ```
     若 DPDK 在系统路径，也可：`sudo dpdk-devbind.py --bind=vfio-pci 0000:04:00.0`。绑定后 `--port` 即用该 BDF（如 `0000:04:00.0`）。
   - **Intel E800 系列（E810/E830）**：若 MTL 提供脚本（如 `script/nicctl.sh`），在 MTL 源码目录下：
     ```bash
     cd $MTL_SOURCE   # 或 /path/to/Media-Transport-Library
     sudo -E ./script/nicctl.sh create_vf 0000:af:00.0
     ```
     脚本会创建 VF 并绑定，输出中会给出 VF 的 BDF（如 `0000:af:01.0`），本程序 `--port` 用该 BDF。
   - **其他 DPDK 支持的网卡**：使用 MTL 或 DPDK 的绑定脚本（如 `nicctl.sh bind_pmd 0000:32:00.0`），或 DPDK 的 `dpdk-devbind.py --bind=vfio-pci <BDF>`。

4. **确认**：
   ```bash
   ls -l /dev/vfio/
   ```
   应能看到对应设备节点，且组为 `vfio`。

绑定后，该网卡不再由内核管理，**两台机器直连的 IP 需在 DPDK 使用前就配置好**（若之前用该网卡 ping 通，则已配置；若改用另一块网卡做 DPDK，则需在绑定前为该网卡配置好 IP，或使用 MTL/DPDK 文档中的静态 IP 配置方式）。本 SDK 通过 `--sip` 传入本机源 IP，MTL 会使用该 IP 做组播发送/接收。

### 2.5（可选）非 root 运行时的 RLIMIT_MEMLOCK

CentOS/RHEL 等若以非 root 运行 MTL 报错，可在 `/etc/security/limits.conf` 中为对应用户增加：

```text
<用户名>  hard  memlock  unlimited
<用户名>  soft  memlock  unlimited
```

然后重新登录或重启。

### 2.6 CPU（lcore）与 Tasklet 配置

MTL 的发送/接收由 DPDK **EAL** 管理的 **lcore** 上运行的 **tasklet** 完成。lcore 数量与分配影响吞吐和延迟。

**DPDK EAL 常用参数**（需在进程启动时传入，由 MTL 内部转给 EAL）：

| 参数 | 含义 | 示例 |
|------|------|------|
| `-l <corelist>` / `--lcores <list>` | 指定使用的 CPU 核（lcore 列表） | `--lcores 0-3` 使用 0、1、2、3 核 |
| `-c <coremask>` | 用十六进制位掩码指定核 | `-c 0xf` 表示 0～3 核 |
| `-m <MB>` | 预分配大页内存（MB） | `-m 1024` 预分配 1GB |
| `--main-lcore <id>` | 主 lcore | `--main-lore 0` |

**本 SDK 示例（st2110_send / st2110_record）**：已支持通过命令行传入 **lcore 与 tasklet** 参数，并写入 `mtl_init_params`，可提升发送/接收速度、缓解 “build timeout” 或 put_video 失败。

| 参数 | 含义 | 示例 |
|------|------|------|
| `--lcores <list>` | 指定 MTL 使用的 DPDK lcore 列表 | `--lcores 0-3` 或 `--lcores 2,3,4,5` |
| `--main-lcore <id>` | 主 lcore id；不设则 MTL 自动选择 | `--main-lcore 0` |
| `--tasklets <n>` | 每个 lcore 的 tasklet 数；0=自动（遇 build timeout 可试 16） | `--tasklets 16` |
| `--data-quota-mbs <n>` | 每个 lcore 最大数据配额（MB/s）；0=自动 | `--data-quota-mbs 4096` |

**示例（DPDK 模式下发得更稳、更快）**：

```bash
# 发送端：固定 lcore 0,1,2,3，每 lcore 16 个 tasklet
./st2110_send --url yuv420p10le_1080p.yuv --width 1920 --height 1080 \
  --duration 30 --audio-port 0 --ip 239.0.0.1 --video-port 5004 \
  --port 0000:04:00.0 --sip 192.168.10.1 --no-ptp \
  --lcores 0-3 --tasklets 16

# 接收端：同样指定 lcore 与 tasklet
./st2110_record --ip 239.0.0.1 --video-port 5004 --audio-port 0 \
  --width 1920 --height 1080 --max-frames 1800 recv.mp4 \
  --port 0000:06:00.0 --sip 192.168.10.2 --no-ptp \
  --lcores 0-3 --tasklets 16
```

大页仍按 §2.3 在系统侧配置（如 `vm.nr_hugepages=2048` 或 1G 大页）；EAL 的 `-m` 等参数由 MTL 内部使用，本示例通过 `--lcores`/`--tasklets` 等传递 lcore 与调度相关配置。

**Tasklet**：MTL 内部用 **scheduler（sch）** 管理 tasklet（视频 TX/RX、转换等），每个 sch 绑定到若干 lcore。增加 `--tasklets` 可提高并发度；过多可能增加调度开销，可按实际负载调节。更多细节见 [MTL Run Guide](https://openvisualcloud.github.io/Media-Transport-Library/doc/run.html)。

**跑满 2.5G（如 I226-V）**：单路 1080p59.94 4:2:2 10-bit 约 2.1～2.3 Gbps，已接近 2.5G 链路。瓶颈多为 **单核 TX**（MTL 单会话单 sch 单 lcore），易出现 build timeout 和 put_video 失败。st2110_send 已支持 **背压重试** 与 **预填帧**，可尽量发完、提高实际占用：

- `--put-retry <n>`：put_video 失败时重试次数（默认 150），每次间隔 2ms，用背压等管道排空后再发。
- `--prefill-frames <n>`：启动时先无节奏推送 n 帧填满 TX 环（默认 4，仅在使用 `--url` 时生效），减轻前期 build timeout。

示例（尽量跑满 2.5G、少丢帧）：
```bash
./st2110_send --url yuv420p10le_1080p.yuv --width 1920 --height 1080 \
  --duration 30 --audio-port 0 --ip 239.0.0.1 --video-port 5004 \
  --port 0000:04:00.0 --sip 192.168.10.1 --no-ptp \
  --lcores 0-3 --tasklets 16 --put-retry 200 --prefill-frames 8
```
若仍不稳，可配置 1G 大页（§2.3）并做 CPU 隔离（如将 sch 所在 lcore 与系统隔离），或适当降低分辨率/帧率以减轻单核负载。

### 2.7 多 MTL 进程时运行 MtlManager

若 **同一台机器** 上会运行多个 MTL 应用进程，需先在该机启动 MTL 的 Manager：

```bash
sudo MtlManager
```

再在其它终端启动 `st2110_send` / `st2110_record`。单进程单机收发可省略。

---

## 三、双机收发命令示例（DPDK 模式）

- 发送端（A 机）IP：`192.168.10.1`，BDF：`0000:af:01.0`
- 接收端（B 机）IP：`192.168.10.2`，BDF：`0000:af:01.0`（或本机实际 VF BDF）
- 组播：`239.0.0.1`，视频端口：`5004`，无音频：`--audio-port 0`
- 使用项目内 `build/yuv420p10le_1080p.yuv`（1920×1080，59.94fps）

**接收端（B 机）先启动：**

```bash
cd /path/to/GPT_mtl_encode_sdk/build
./st2110_record --ip 239.0.0.1 --video-port 5004 --audio-port 0 \
  --width 1920 --height 1080 --max-frames 1800 recv.mp4 \
  --port 0000:06:00.0 --sip 192.168.10.2 --no-ptp
```

**发送端（A 机）后启动：**

```bash
cd /path/to/GPT_mtl_encode_sdk/build
# 若当前目录为 build，YUV 在 build 下则用 --url yuv420p10le_1080p.yuv
./st2110_send --url yuv420p10le_1080p.yuv --width 1920 --height 1080 \
  --duration 30 --audio-port 0 --ip 239.0.0.1 --video-port 5004 \
  --port 0000:04:00.0 --sip 192.168.10.1 --no-ptp
```

若在项目根目录执行，则发送端用 `--url build/yuv420p10le_1080p.yuv`。两机 `--ip`、`--video-port`、`--audio-port`、分辨率、帧率需一致；`--port` 为各自直连网卡的 BDF，`--sip` 为各自直连网口 IP。

---

## 四、故障排查

- **mtl_init 失败**：检查大页是否足够（§2.3，可试 4096 或 1G 大页）、是否已绑定 vfio、当前用户是否在 `vfio` 组、是否有 RLIMIT_MEMLOCK 限制。
- **build timeout / put_video 失败**：多为发送队列积压。可：增大大页（§2.3）、为 MTL 指定 lcore/tasklet（§2.6）、使用 `--put-retry` 与 `--prefill-frames`（背压重试与预填帧，见 §2.6「跑满 2.5G」）。
- **收不到包**：确认两机 `--ip`/`--video-port` 一致、`--sip` 为直连网口 IP、`--port` 为实际用于直连的 BDF；防火墙/组播路由在直连场景一般不需改。
- **网卡绑定后无法 ping**：正常，DPDK 占用后内核不再使用该网卡；若需同时用该网卡 ping，请改用 Kernel 模式（`--port kernel:接口名`）。

更多细节见 [MTL Run Guide](https://openvisualcloud.github.io/Media-Transport-Library/doc/run.html) 与 [TESTING.md](TESTING.md)。
