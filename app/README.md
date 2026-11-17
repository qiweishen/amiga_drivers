# app 代码分层

应用入口是 `main.py`。传感器数据处理集中在 `decoder/`，采集控制与异步调度放在
`services/`，页面和图表放在 `ui/`。

`amiga_drivers` 负责传感器驱动、原始数据流落盘，以及 GUI 所需的实时解析、
快照和参考数据临时展示。离线导出、格式转换及离线校准由 `amiga_decoders` 承接；
本项目不提供相应的命令行工具。

| 位置 | 职责 |
| --- | --- |
| `decoder/ebus/` | `ebus-raw` 记录读取、完整性检查（CRC-32C、序号、session_end 计数）、PFNC 像素解包；GOX 与 FX10 共用。`pfnc.py` 只登记实际使用的 7 种格式 |
| `decoder/exposure.py` | ETTR 通用运算（线性拟合、曝光建议、帧周期封顶、复核），GOX 与 FX10 共用 |
| `decoder/fx10/` | FX10 元数据、预览数据立方体、原始 DN 光谱统计和白参考 ETTR 测量点 |
| `decoder/gox/` | GOX 图像读取、预览编码、对焦指标、色卡检测与矩阵拟合、色卡 ETTR 测量点 |
| `decoder/asterx/` | 当前采集会话的 CSV 遥测解析、增量读取及历史扫描轨迹读取 |
| `services/` | 进程和设备操作生命周期、配置、会话状态、实时预览及历史轨迹读取调度 |
| `ui/` | 页面交互、图表选项和结果展示 |
| `tests/` | 解码、服务和界面的测试源码 |
| `resource/` | 界面静态资源 |
| `_runtime/` | 应用运行时文件与工具输出，不属于源码层 |

`services/` 中 GoX/FX10 共用的模块：

| 模块 | 内容 |
| --- | --- |
| `snapshot_tool.py` | 运行 snapshot 类工具：先解析 `SNAPSHOT: OK/FAIL` 标记再看退出码、输出目录与清理 |
| `timing.py` | 两台相机的帧率/曝光上限与 90% 帧周期规则（对应两个驱动的 `app_config.h`） |
| `exposure_series.py` | 曝光序列的保存、复核点与建议值的公共部分 |
| `gox_chart.py` | GOX 色卡候选的登记与操作者选择校验 |
| `camera_operations.py` | 相机互斥：录制占用的相机不能被工具使用（`guard_reason` + `snapshot_busy`） |

`ui/components.py` 放共享控件（`info`/`hint`/`heading`、`preview_image`、`metric`、
`raw_output`、`confirm`、`busy`）和共享文案；`ui/ettr_section.py` 是两个参考页共用的
ETTR 曝光序列面板。

## 依赖方向

```text
main.py → ui → services → decoder
             └──────────→ decoder 的数据类型与纯辅助函数
decoder/fx10、decoder/gox → decoder/ebus
```

`decoder/` 不依赖 `services/`、`ui/` 或全局应用状态，不控制设备或启动采集进程。
读取和统计函数不修改采集文件；GUI 图像编码仅用于快照和实时展示。
GOX 快照保留原始 RAW，预览图和原始 DN 直方图在内存中生成，不另存预览文件。
GOX 色卡参考也保留 RAW；色彩校正预览只在内存中生成，拟合结果写入参考元数据。
图表配置保留在 `ui/`，请求互斥、任务取消、会话切换和结果发布保留在 `services/`。
新增传感器数据算法时，应放入对应的 `decoder/<sensor>/`。
当前 app 没有 LMS payload 解码器，因此不建立空的 LMS 模块。

## 数据保护规则

简化代码时下列规则逐条保留；它们保护采集数据或配置，不是可删的“防御代码”：

- 相机互斥：`camera_operations.guard_reason` 与 `snapshot_busy` 预留；录制开始时
  记录的 enables 决定哪台相机被占用。`tool_jobs.check_idle`（pgrep 互斥）、作业日志、
  启动门、按 PID 身份停止工具。
- 采集进程只发 TERM，从不 KILL；GUI 关闭时等待采集排空。重新附着按进程身份三元组
  （PID、启动时间、boot id）匹配；`control_owner` 用 flock 保证单一控制者。
- 配置写入：`_require_writable`（录制中锁定）、mtime 冲突检测、`.bak` 备份、写后读回
  （`_verified_text`）、正式与 snapshot 两个配置的成对写入与回滚、备份路径重叠检查。
- 失败的工具保留 RAW；`reference_storage` 不覆盖已有目录；参考校准校验 payload CRC-32C
  并写入 provenance。
- 与 C++ 共享的契约字符串一字不改：`amiga-run-v1`、`SNAPSHOT: OK/FAIL`、
  `REFERENCE: CONFIG/SESSION/PHASE/OK/FAIL`、`AMIGA_*` 环境变量、`AmigaDrivers`
  二进制名，以及 `tools/check_contracts.py` 检查的 `driver_stats` 正则。

字节上限、严格 JSON、重复复验、节流等不保护数据的检查已删除。

## GUI 文案规则

1. 每个区块只有标题和至多一行可见提示（约 60 字符，英文短语）；原理、公式和来源放进
   ⓘ（`components.info`，点击/触屏打开，不超过 3 句），完整说明写在本 README。
2. 数值引用常量，例如 `f"{timing.PERIOD_FRACTION:.0%}"`、`ettr.TOLERANCE`。
3. 多处共用的文案只定义一次（`components.NEXT_RUN`、`components.PREVIEW_DETAILS`）。
4. notify 一句话（约 80 字符），其后附原始错误。
5. 界面不出现内部名称（AmigaDrivers、amiga-run-v1、build/bin、函数名）；平台名显示为
   Phenomate。

## 格式与显示约定

- GOX 和 FX10 的采集输入仅支持 `ebus-raw-eBUS_SDK_v<SDK version>`，对应的设备
  元数据标识为 `ebus-device-eBUS_SDK_v<SDK version>`。例如 SDK `6.5.1.6797`
  对应 `ebus-raw-eBUS_SDK_v6.5.1.6797`。两个标识使用相同的编译时 Pleora SDK
  版本，与 `sdk.header_version` 一致；来源为 `PvVersion.h` 的 `NVERSION_STRING`，
  不是运行时 SDK 查询结果或 AmigaDrivers 项目版本。RAW 文件头的数字布局版本
  仍为 `1`，读取时独立检查。旧 `-v1`、无版本后缀的标识，以及旧 JAI RAW 和
  旧 FX10 ENVI/BIL 均不支持；已有数据不做自动转换。
- FX10 快照、白/暗参考与实时光谱的统计值均为原始 DN，不进行归一化、暗场扣除或
  反射率换算。饱和比例仍是百分比，满量程参考线依据记录的 PixelFormat。
- GOX 采集与 GUI 读取统一且只支持原生 `BayerRG12Packed`（GVSP 12Packed，2 像素占
  3 字节），相机回读必须同时满足 `PixelFormat=BayerRG12Packed` 和
  `SensorDigitizationBits=12`。RAW 保留 SDK 原始打包字节；GUI 无损解包后的 Bayer
  样本为 `uint16` 容器中的 12-bit DN（0–4095）。未打包 `BayerRG12` 等其他格式的
  记录不被读取。
  预览编码：先在 `uint16` 下去马赛克。屏幕预览为线性 DN / 4095，面积平均缩小到长边
  ≤ 1600 px 后量化为 8-bit PNG（浏览器和显示器本就以 8 bit 显示）；下载为全分辨率
  16-bit PNG，以 `DN << 4`（DN × 16）保留每个去马赛克后的 DN 代码，首次请求时从 RAW
  重新解码生成。两者都不参与测量。浏览器、操作系统合成及显示器的实际显示位深未验证。
- `BayerRG12Packed` 每像素占 12 bit（手册 “Pixel Format” 的 PixelSize 表）。
  4128×3008 完整帧为 18,625,536 字节，为未打包 `BayerRG12` 的 3/4。主配置 5 fps
  的图像 payload 约 93.13 MB/s。现有采集记录的链路为 125,000,000 B/s，带宽裕量
  参数为 92%，对应约 115 MB/s，尚需计入协议开销；该配置的持续采集能力未验证。
- 本项目不读取或导出 ENVI；需要离线导出或转换时，由 `amiga_decoders` 处理。
- AsteRx 读取当前采集会话的 CSV 遥测；相机 RAW 格式限制不适用于 AsteRx。
  History 页面保留会话浏览和可缩放的扫描轨迹地图，便于查看扫描范围；不提供
  时间轴播放、逐帧历史遥测或轨迹导出。

原始记录的字段、完整性检查及完成状态约定见 [eBUS 文档](../ebus_driver/README.md)。

## Camera Tools 参数保存

工作台滑块和 **Take snapshot** 只临时覆盖预览参数。**Apply to config** 将 GOX
的曝光、帧率、增益，或 FX10 的曝光、帧率、空间及光谱 binning，同时保存到对应的
正式录制配置和 snapshot 配置，供下一次录制与参考采集使用。

FX10 时序限制与 `fx10_driver` 一致（`services/timing.py` 的 `FX10_*` 常量对应
`fx10_driver/include/app_config.h` 的 `CheckAcquisitionTiming`）：`frame_rate_hz`
在空间与光谱 binning 均为 1 时不超过 163.02 Hz，其余 binning 组合不超过
324.373 Hz；`exposure_ms` 为 0.01–419 ms，且不超过帧周期的 90%（含等号）。
工作台 Exposure 旁的 **Frame rate** 随曝光和 binning 自动取最大值（向下取到
0.001 Hz），操作者只能下调；Take snapshot 以 `--fps` 传给 `fx10_snapshot`
（工具在命令行覆盖后重新校验，不再静默降帧率），Apply 时写入两个配置的
`frame_rate_hz`。外触发且启用 SensorSync 的录制配置中，该值即脉冲频率，须为该组
PWM 能产生的频率（FX 组 17.882–9847.689 Hz，JAI 组 0.140–9847.689 Hz；
`services/sensor_sync_rates.py` 对应 `common/include/sensor_sync_rates.h`）。ETTR 曝光序列的每个点取配置帧率与该曝光可用最大帧率中的较小值；ETTR
建议的 Apply 只改曝光，帧率不变。

GOX 时序限制与 `gox_driver` 一致（`services/timing.py` 的 `GOX_*` 常量对应
`gox_driver/include/app_config.h`）：`frame_rate_hz` 为 0.125–6.054 Hz（全幅
`BayerRG12Packed` 的 AcquisitionFrameRate 上限），`exposure_ms` 不超过 399.405 ms
（相机回读的 ExposureTime 上限 399405 µs），且曝光不超过帧周期的 90%
（`exposure_ms <= 900 / frame_rate_hz`，含等号）。工作台 Exposure 旁的
**Frame rate** 自动取满足上述条件的最大值（向下取到 0.001 Hz），操作者只能下调；
Take snapshot 以 `--frame-rate-hz` 传给 `jai_snapshot`，Apply 时写入两个配置的
`frame_rate_hz`。外触发且启用 SensorSync 的录制配置中，该值即脉冲频率，须 ≥ 0.140 Hz
（SensorSync JAI 组 PWM 下限）。
ETTR 建议的 Apply 只改曝光，帧率不变。snapshot 配置仍不在 Configuration
页中展示；Reference capture 继续从各自的 snapshot 配置读取采集参数。

## GoX focus assist

**Camera Tools → GoX · Focus assist** 用于对焦。它以 GoX 工作台当前的目标相机、曝光和
增益持续采集，实时给出 ROI 清晰度相对本次峰值的百分比。对焦更看重帧率而非位深，所以
这条流固定使用 8-bit `BayerRG8`，并始终以最高帧率运行：
`floor(min(9.078, 900 / exposure_ms), 0.001)` Hz。全幅 BayerRG8 的
AcquisitionFrameRate 上限显示为 9.079 Hz，但相机内部是略小于它的浮点数，写入 9.079 会被
GenApi 拒绝（2026-11-17 真机报错“9.079000 must be smaller than or equal 9.079000”），所以
上限取低一档 9.078 Hz。曝光仍须 ≤ 帧周期的 90%（`timing.GOX_MAX_FOCUS_FRAME_RATE_HZ`
对应 C++ 的 `gox::kMaxFocusFrameRateHz`）。工作台的 Frame rate 框不参与对焦流。
流运行中修改曝光或增益不会生效，需停止后重新开始。

- 采集：`jai_snapshot --stream-s 1800`，使用 snapshot 配置的 freerun 档（PTP 和
  SensorSync 关闭）。与快照的区别有三点：不限帧数；`PixelFormat` 改为 `BayerRG8`；每个
  RAW 段只写一条记录。只有这条流可以使用 BayerRG8（代码内的
  `AcquisitionConfig::focus_preview` 标志，YAML 无法设置）。录制、快照和参考仍只接受
  `BayerRG12Packed`。A/D 深度保持出厂值，回读须为 10 或 12 位；8 位会使灵敏度变为 4 倍
  （手册 p.45），工作台曝光就不再对应录制。对焦流的队列按约 2 s 写盘停顿设置。
  `services/gox_focus.py` 跟随最新一条已提交记录进行分析，并删除其后已关闭的段，因此
  磁盘上只保留约 3 帧。流目录位于 `app/_runtime/focus/`，下一次开流时整体替换；结束后
  只保留 `device.json` 和 `events.jsonl` 供诊断。对焦流不写任何配置。
- 指标（`decoder/gox/focus.py`）：只取 BayerRG 的 Gr 感光点（2×2 的第 0 行第 1 列），
  直接读取 8-bit 载荷，不去马赛克。
  `score = var(Laplacian(G)) / (mean(G) − 8)²`。8 DN 是手册 p.172 的标称黑电平
  “8LSB@8bit”，与 12 位下的 128 DN 相同。Laplacian 不受常数黑电平影响，扣除黑电平后再
  归一化，可使分数不随光照和曝光变化。ROI 平均信号低于黑电平以上 4 DN 时不给分。
  显示值为最近 3 帧的中位数；峰值在移动或缩放 ROI、Reset peak 和重新开流时清零。
  ROI 内满量程（255）感光点超过 0.5% 时会提示，因为饱和边缘会抬高分数。
- 交互：点击概览图放置 ROI，点击放大镜微调中心。放大镜按“一个图像像素对应一个 Gr
  感光点”输出，浏览器放大时不做平滑，所以 ROI 越小，放大倍数越高；ROI 大于 512 px 时
  只显示中心 256×256 个感光点。概览与放大镜直接显示 8-bit 感光点值（线性 DN/255），
  不做拉伸。
- 占用：流运行期间持有相机预留。录制、快照、参考采集和配置保存均被锁定，直到
  Stop、页头 Stop tool、30 分钟上限或工具失败结束。所有 Camera Tools 页面关闭
  15 秒后，流自动停止。
- 操作：先调曝光，使 ROI 不饱和。缓慢转动对焦环，越过峰值直到百分比明显下降，再回调
  到 ≥ 95%，锁紧后确认仍 ≥ 95%。改变目标高度、光圈或光照后，先按 Reset peak。

本功能尚未经过完整的真机验证，包括 9.078 Hz 在 BayerRG8 与出厂 A/D 深度下能否写入和
维持。

## Reference capture

**Device Setup → Reference capture** 同时提供 FX10 白/暗参考和 GOX 色卡参考。
两类参考采集分别使用 `config-fx10-snapshot.yaml` 和 `config-gox-snapshot.yaml`，
均以 freerun 运行，不启用 PTP、外部触发或 SensorSync。参考页显示所用配置路径；
snapshot 配置不在 Configuration 页中展示，也不读取该页的录制配置作为参考采集参数。
FX10 每个阶段的时长读取 snapshot 配置的 `reference.duration_s`，缺省由驱动使用 5 秒；
页面同时显示当前配置的白/暗采集时长。

GOX 操作必须先显式选择 `cv.mcc.MCC24`、`cv.mcc.SG140` 或 `cv.mcc.VINYL18`；
点击 `resource/` 中对应的色卡示意图即可选择格式，选中卡片高亮，初始不预选。
**Capture & calibrate** 按 GOX snapshot 配置中的设备、曝光和增益采集一帧并检测
选定的色卡；参考页不提供 IP、曝光或增益覆盖值，也不使用 Camera Tools 的滑块值。

`cv.mcc.CCheckerDetector` 使用临时 8-bit 图像自动定位所选色卡，只返回色块几何位置；
这一步是图像位深的唯一例外。拟合取样来自完整分辨率的 12-bit DN 图像，Bayer 图像
先在 `uint16` 下去马赛克，再使用浮点数由 `cv.ccm` 拟合色彩矩阵。
检测用的 8-bit 像素不参与色块均值、校准拟合或 GUI 预览。
校准读取会校验所用 RAW payload 的 CRC32C；普通快照与实时预览
的读取策略保持不变。目标为 sRGB（D65），矩阵将归一化的线性相机 RGB 映射到线性
sRGB，校正图使用 sRGB 传递函数（屏幕 8-bit，下载 16-bit PNG）。原图和检测标注图的下载使用
上述 `DN << 4` 映射。设备记录必须提供所需的线性处理状态审计；
检测失败、色块饱和或拟合失败会显示失败，不生成成功的校准结果。
当前完整流程尚未进行运行或硬件验证。

采集前检查项目当前使用的 OpenCV 5 `mcc`/`ccm` API 能力；缺失时显示原因并阻止
采集，不自动安装或更改环境。原始 `ebus-raw` 文件、采集配置副本和
`reference.json` 保存在已配置的输出目录下的 `reference_gox_<UTC>_<ID>/`，
不受临时快照清理影响；分析失败也保留已采集的 RAW。元数据记录色卡类型、
色彩矩阵、拟合指标和原始文件相对路径。原图、色卡检测图和校正图仅在 GUI 中展示。
拟合出的矩阵不写回相机，也不自动应用于后续采集或原始记录；离线批量校正仍由
`amiga_decoders` 负责。

### 色彩数值接口（`decoder/gox/color.py`）

本应用只拟合、保存和预览校准；把矩阵应用到其他帧（曝光比缩放、批量校正）由
`amiga_decoders` 离线完成，本应用不提供该接口。

- `CameraRGBNormalization`：`dn_over_full_scale`（DN / 4095，黑电平未知或为零）或
  `pedestal_subtracted`（(DN − B) / (4095 − B)，B 为单个 `measured` 或 `documented`
  标量）。参考拟合使用文档黑电平 128 DN（`documented`）。
- `normalize_camera_rgb` 把原生 `uint16` RGB 转成不裁剪的 `float32`，低于黑电平的
  值保持为负；`normalize_patch_means` 对 `float64` 聚合的色块均值用同一公式。
- `ColorCalibration.to_dict()` 写入 `reference.json`，键名为 `schema_version`、
  `calibration_id`、`matrix`、`matrix_convention`（行向量 `camera_rgb @ matrix →
  linear sRGB D65`）、`normalization`、`applicability`（设备、积分时间、增益、
  像素格式、线性审计、`exposure_compensation`）、`fit`、`source`。构造时要求矩阵
  为有限 3×3、满秩且条件数 ≤ 1e6。
- `frame_exposure_from_device` 从 device.json 读取实际积分时间（ExposureTime 回读 +
  驱动记录的偏移）和 Gain 回读，缺失时报错，不默认 ×1。

### 黑电平

GOX 手册 Ver. 1.3 p.172 “Black Level Adjustment” 给出默认黑电平 8LSB@8bit，驱动按
8 × 16 换算为 12 bit 的 128 DN（`gox/recording.DOCUMENTED_PEDESTAL_DN`）。没有寄存器
报告该值，因此是文档值而非实测值。参考校准在拟合与校正前扣除它，并与 device.json 的
`derived.black_level` 交叉核对：派生值不是 128 或 `BlackLevel` 寄存器不为 0 时给出
警告（此时应拍暗帧实测）。对焦流为 8 bit，同一电平是 8 DN。

### ETTR 曝光建议

两台相机的被测量都与曝光线性相关：`peak_dn = offset + slope × (t × g)`，`t` 为实际
积分时间，`g` 为增益倍数（FX10 为 1）。在初始曝光两侧各拍一次（GOX ±50 ms，FX10
±15 ms；初始值不大于步长时低端取一半），只用未饱和的点拟合直线（达到满量程 98% 即
视为接近饱和，不参与拟合），求出被测量达到目标的曝光：GOX 为最亮色块的最亮通道
达到 80% 满量程，FX10 为最亮波段的 p90 达到可调目标（默认 60%）。建议曝光超过
配置帧周期允许的上限时封顶在上限的 98%，增益和 binning 永不改变。之后的参考采集
自动作为复核点：偏差在 ±5% 内即通过，否则用全部点重新拟合给出新建议；饱和的复核点
只能给出保守的下调值。

### 色差 ΔE00

拟合损失为 OpenCV `cv.ccm` 的 CIEDE2000 均方根。逐块 ΔE00 比较 OpenCV 参考 linear
sRGB 与“扣除黑电平的色块均值 @ 矩阵”，两者都换算到 CIE L*a*b*（D65/2°），
kL = kC = kH = 1；对拟合色块求均方根即等于拟合损失。校色预览把 linear sRGB 截到
[0, 1] 后做 sRGB OETF，不做 EV、色调曲线或逐图统计。

数值契约的测试源码为 `tests/test_gox_color.py` 与 `tests/test_gox_delta_e.py`。
2026-11-17 对本地参考 `data/tmp/reference_gox_20261110T114217Z_4e0bd0d8` 重算，
色块均值、矩阵、损失与逐块 ΔE00 与当时保存的结果逐位一致；这不能代替真实设备上的
完整采集验证。
