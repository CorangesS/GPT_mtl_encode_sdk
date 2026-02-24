# MTL-Encode-SDK 测试

按方案 C 分层结构组织的需求符合性测试。

## 目录结构

```
tests/
├── unit/              # 单元测试（不依赖 MTL 运行时，sdp/encode 除外）
│   ├── sdp_test                 # SDP 解析、to_sdp、load/save 文件
│   ├── sdp_to_session_test      # SDP → 会话参数映射
│   ├── encode_format_test       # H.264/H.265、MP4/MXF、AAC/MP2/PCM/AC3
│   └── encode_reconfigure_test  # 编码参数运行时调整
├── integration/       # 集成测试（需要 MTL 运行时）
│   └── ptp_behavior_test        # PTP 行为验证
└── scripts/           # 脚本类测试
    ├── st2110_roundtrip_test.sh # 收发 + 编码端到端
    ├── nmos_registration_test.sh# NMOS Registry 注册与发现
    └── run_all_tests.sh         # 运行全部测试
```

## 需求对应

| 测试 | 需求.md |
|------|---------|
| sdp_test | §4 SDP 解析、导入/导出 |
| sdp_to_session_test | §4 SDP → 会话参数 |
| encode_format_test | §2-4 编码器、容器、音频格式 |
| encode_reconfigure_test | §5 编码参数可调 |
| ptp_behavior_test | §3 PTPv2 时钟同步 |
| st2110_roundtrip_test.sh | §1-4 收发 + 编码全链路 |
| nmos_registration_test.sh | 路由 §1 IS-04 注册发现 |

## 构建与运行

### 构建测试

```bash
cd build
cmake ..
cmake --build . -j
```

生成的可执行文件在 `build/` 下：`sdp_test`、`sdp_to_session_test`、`encode_format_test`、`encode_reconfigure_test`、`ptp_behavior_test`。

### 运行单元测试

```bash
./build/sdp_test
./build/sdp_to_session_test
./build/encode_format_test
./build/encode_reconfigure_test
```

### 运行集成测试

```bash
./build/ptp_behavior_test
chmod +x tests/scripts/*.sh
./tests/scripts/st2110_roundtrip_test.sh
REGISTRY_URL=http://<Easy-NMOS-IP> ./tests/scripts/nmos_registration_test.sh
```

### 运行全部测试

```bash
chmod +x tests/scripts/*.sh
./tests/scripts/run_all_tests.sh
```

## 说明

- **unit/**：sdp_test、sdp_to_session_test 仅用 mtl_sdk SDP API，无需 MTL 网卡；encode_* 需 FFmpeg，使用合成帧。
- **integration/**：ptp_behavior_test 需 MTL，MTL 不可用时自动 SKIP。
- **scripts/**：st2110_roundtrip_test 需 MTL；nmos_registration_test 需 Registry 运行。
