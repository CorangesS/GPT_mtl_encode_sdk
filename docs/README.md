# 文档导航

| 文档 | 内容 |
|------|------|
| [SDK_USAGE.md](SDK_USAGE.md) | **mtl_sdk / encode_sdk 上层应用使用说明**（PTPv2、跑满网卡参数、接入方式） |
| [COMPLIANCE.md](COMPLIANCE.md) | 需求符合性逐项检查 |
| [TESTING.md](TESTING.md) | 本机/双机收发测试、PTP/端口模式、frame_cnt、参数说明 |
| [ROUTING.md](ROUTING.md) | NMOS 路由管理与 MTL-Encode-SDK 对接 |
| [EASY_NMOS_IMPLEMENTATION.md](EASY_NMOS_IMPLEMENTATION.md) | **路由管理实现指南（基于 Easy-NMOS）** |
| [NMOS_JS_DEPLOY.md](NMOS_JS_DEPLOY.md) | NMOS-JS 部署与配置（nmos-cpp 单独部署时使用） |
| [../tests/README.md](../tests/README.md) | 测试套件说明（需求符合性测试） |

**当前实现要点**：基于标准 MTL 库；示例支持 `--port`（kernel/DPDK）、`--no-ptp`；PTP 不可用时自动回退；Easy-NMOS 集成见 [EASY_NMOS_IMPLEMENTATION.md](EASY_NMOS_IMPLEMENTATION.md)。
