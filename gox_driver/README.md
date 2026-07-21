# JAI Go-X GigE Vision 原始数据采集 Driver

基于 **eBUS SDK for JAI 6.x（C++ API）** 的原始数据采集 driver，面向 JAI Go-X 系列 GigE Vision 相机：

- 通过一份 `config.json` 控制相机全部采集参数（GenICam 特性逐条下发 + 写后回读校验）
- 相机时间同步走 **PTP (IEEE 1588)**：网络中已有 grandmaster，driver 只负责使能相机 PTP
  从模式并校验同步状态（时间戳为 TAI 纳秒，原样落盘）
- **只采集原始数据**：GVSP payload 原样落盘，不解包、不去马赛克、不做像素转换，
  解包由后续独立脚本完成（见 `scripts/inspect_raw.py` / `scripts/unpack_raw.py`）
- 落盘格式：分段大文件（默认 2 GiB 轮转）+ 每帧 96 字节二进制帧头 + JSONL 索引
- 多相机由**一个** driver 实例管理（`cameras[]` 数组、每相机独立线程与目录）

运行环境：Docker devcontainer（`network_mode: host`），C++17。

## 与 AmigaDrivers 统一入口的集成

本 driver 已并入仓库根部的 **AmigaDrivers** 统一工程（与 ins401_driver / lms4xxx_driver 同级）：

- **构建**：由根 `CMakeLists.txt` 的 `add_subdirectory(gox_driver)` 引入，不再独立构建
  （原 `CMakePresets.json` / `scripts/build.sh` 已移除）。eBUS SDK 是硬依赖：
  devcontainer 镜像已从 `gox_driver/resource/` 安装其 .deb；非标准路径用
  `-DEBUS_SDK_ROOT=/path/to/sdk` 指定。
- **运行**：`config/config-main.yaml` 中设 `Enable GOX: true`，
  `GOX Driver Config Path` 指向本目录 `config/config-gox.json`。
  `recording.output_dir` / `session_name` 被覆盖为
  `<Output Directory>/<时间戳>/bin/gox/`（与其他传感器数据同会话目录），
  gox 日志并入统一 log 文件，`acquisition` 上限到达会关停整个 AmigaDrivers 进程。
- **调试工具**（standalone 采集入口已移除）：`build/bin/jai_discover`（GigE 设备
  枚举）、`build/bin/jai_snapshot`（GUI 单张拍照预览）与 `build/bin/jai_fake_capture`
  （无相机验证存储链路）。
- 日志走进程唯一的 spdlog 实例（父工程 FetchContent v1.17.0）；nlohmann/json
  与 doctest vendored 于仓库级 `3rd_party/`。

## 快速开始（3 步）

**1. 在容器内安装 eBUS SDK for JAI**

从 JAI 官网下载 `eBUS_SDK_JAI_Ubuntu-<发行版>-x86_64-<版本>.deb`，在仓库根部的
`.devcontainer/Dockerfile` 中加入安装步骤后 **Rebuild Container**（或在容器内手动
`dpkg -i` 安装）。容器内提示"内核驱动编译失败/跳过"是正常且无害的——本 driver 走
用户态接收器。

**2. 构建（仓库根目录）**

```bash
mkdir -p build && cd build
cmake .. && make -j$(nproc)
```

单元测试（SDK-free，Debug 构建默认开启）：

```bash
cmake -DCMAKE_BUILD_TYPE=Debug .. && make -j$(nproc) && ctest --output-on-failure
```

**3. 运行**

```bash
./build/bin/AmigaDrivers                             # 统一入口（config-main.yaml 中 Enable GOX: true）
./build/bin/jai_discover                             # 枚举网络中的 GigE Vision 设备
```

采集结果位于 `<Output Directory>/<时间戳>/bin/gox/`。可用
`python3 scripts/inspect_raw.py verify <会话目录>/<相机 id>` 做完整性校验。

## 仓库布局

| 路径 | 内容 |
|---|---|
| `CMakeLists.txt` | 构建入口（作为父工程 AmigaDrivers 的子目录被引入） |
| `cmake/FindeBUS.cmake` | 定位 eBUS SDK（`EBUS_SDK_ROOT` → `/opt/jai/ebus_sdk/*` → `/opt/pleora/ebus_sdk/*`），导出 `eBUS::eBUS`（含 RPATH 传递） |
| `config/config-gox.json` | 统一模式默认配置；`config/config.example.json` 为带注释的完整示例（含多相机示例） |
| `include/` / `src/gox_driver_app.cpp` | **统一入口适配层** `GoxDriverApp`（init/run/shutdown + TerminateFlag，供根 main.cpp 使用，目标 `gox_lib`） |
| `src/core/` | **SDK-free 层**：config 解析、落盘格式、Recorder、队列/缓冲池、统计、日志、信号处理——不 include 任何 `Pv*.h`，可脱离 SDK 单测（目标 `jai_core`） |
| `src/ebus/` + `src/capture_runner.cpp` | **SDK 依赖层**：设备发现、连接控制、GenICam 参数下发、PTP、收流 + 采集编排 `CaptureRunner`（目标 `jai_ebus`） |
| `tools/` | `jai_discover`（设备枚举）、`jai_snapshot`（GUI 单张拍照预览）、`jai_fake_capture`（合成帧灌 Recorder，无相机验证存储链路） |
| `tests/` | doctest 单元测试（只链 `jai_core`，无 SDK/无相机可跑；开关 `GOX_BUILD_TESTS`，Debug 默认开） |
| `scripts/` | `setup_host_network.sh`（宿主机网络调优）、`inspect_raw.py`（格式校验/索引重建/单帧导出）、`unpack_raw.py`、`env_ebus.sh` |
| `docs/` | eBUS SDK 官方 PDF |

## 会话结束码

进程退出码由统一入口决定；下表的 code 出现在 gox 的日志
（"session ended with issues (standalone exit code N)"）与 `session.json` 中，
用于判断一次采集会话的质量：

| code | 含义 |
|---|---|
| 0 | 干净完成且零丢帧 |
| 1 | 采集完成，但存在**任何**丢帧/缺包帧/网络层丢块（零容忍；数据已在盘上，明细见最终统计与 session.json） |
| 2 | 配置错误（未知键、取值非法、文件无法解析） |
| 3 | 设备发现、连接或 GenICam 参数应用/回读校验失败 |
| 4 | PTP 同步超时（`ptp.on_timeout = "abort"` 策略下） |
| 5 | 流建立失败（打开流/包大小协商/buffer 分配失败），或 Preflight 出现 ERROR 且 `preflight.fail_on_error=true` |
| 6 | 运行期致命错误（相机断线、磁盘 IO 错误、剩余空间不足、其他未分类致命异常） |
| 130 | 外部中断（Ctrl+C / 统一入口关停请求；段文件仍可被 `rebuild-index` 恢复） |
