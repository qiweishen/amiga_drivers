# app 代码分层

应用入口是 `main.py`。传感器数据处理集中在 `decoder/`，采集控制与异步调度放在
`services/`，页面和图表放在 `ui/`。

`amiga_drivers` 负责传感器驱动、原始数据流落盘，以及 GUI 所需的实时解析、
快照和参考数据临时展示。离线导出、格式转换及离线校准由 `amiga_decoders` 承接；
本项目不提供相应的命令行工具。

| 位置 | 职责 |
| --- | --- |
| `decoder/ebus/` | `ebus-raw` 记录读取、完整性检查、PFNC 像素解包；GOX 与 FX10 共用 |
| `decoder/fx10/` | FX10 元数据、预览数据立方体和原始 DN 光谱统计 |
| `decoder/gox/` | GOX 图像读取、快照和实时图像编码，以及参考色卡检测和色彩矩阵拟合 |
| `decoder/asterx/` | 当前采集会话的 CSV 遥测解析、增量读取及历史扫描轨迹读取 |
| `services/` | 进程和设备操作生命周期、配置、会话状态、实时预览及历史轨迹读取调度 |
| `ui/` | 页面交互、图表选项和结果展示 |
| `tests/` | 解码、服务和界面的测试源码 |
| `resource/` | 界面静态资源 |
| `_runtime/` | 应用运行时文件与工具输出，不属于源码层 |

## 依赖方向

```text
main.py → ui → services → decoder
             └──────────→ decoder 的数据类型与纯辅助函数
decoder/fx10、decoder/gox → decoder/ebus
```

`decoder/` 不依赖 `services/`、`ui/` 或全局应用状态，不控制设备或启动采集进程。
读取和统计函数不修改采集文件；GUI 图像编码仅用于快照和实时展示。
GOX 快照保留原始 RAW，16-bit PNG 预览和原始 DN 直方图在内存中生成，不另存预览文件。
GOX 色卡参考也保留 RAW；色彩校正预览只在内存中生成，拟合结果写入参考元数据。
图表配置保留在 `ui/`，请求互斥、任务取消、会话切换和结果发布保留在 `services/`。
新增传感器数据算法时，应放入对应的 `decoder/<sensor>/`。
当前 app 没有 LMS payload 解码器，因此不建立空的 LMS 模块。

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
  快照、实时与参考原图预览先在 `uint16` 下去马赛克，再以 `DN << 4` 映射到
  16-bit PNG，保留每个去马赛克后 DN 代码，不经过 8-bit 图像或 JPEG。
  这描述应用输出的图像数据；浏览器、操作系统合成及显示器的实际显示位深尚未验证。
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
的曝光、增益，或 FX10 的曝光、空间及光谱 binning，同时保存到对应的正式录制配置
和 snapshot 配置，供下一次录制与参考采集使用。snapshot 配置仍不在 Configuration
页中展示；Reference capture 继续从各自的 snapshot 配置读取采集参数。

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
sRGB，校正预览使用 sRGB 传递函数后编码为 16-bit PNG。原图和检测标注图使用
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

GOX 可复用色彩处理的第一阶段接口位于 `decoder/gox/color.py`：

- `CameraRGBNormalization` 明确区分未经黑电平校正的 `dn_over_full_scale`
  与已测标量黑电平的 `pedestal_subtracted`。当前参考拟合继续采用原有
  `DN / 4095`，黑电平状态记录为 `unknown`，不会把文档推导的 128 DN 当实测值。
- `normalize_camera_rgb` 从原生 `uint16` RGB 生成不裁剪的 `float32`；
  `normalize_patch_means` 对先用 `float64` 聚合的色块均值使用同一归一化公式，
  并保留 `float64` 精度。负值不会被静默截零。
- `ColorCalibration` 提供带版本、来源和适用条件的 `to_dict` / `from_dict`，
  不自行读写文件。`correct_linear` 按行向量约定应用矩阵，返回未裁剪的
  `float32` linear sRGB；负值和大于 1 的值属于分析结果，不能先转换成 `uint16`。
  调用者仍须确认待处理帧与标定的设备和采集条件相符。
- 四值 `[R, Gr, Gb, B]` 黑电平可保存在对象中，但当前 RGB 接口拒绝应用；
  它需要独立的 Bayer 域处理，不能在去马赛克后随意平均两个绿色偏置。
  标量黑电平默认取文档值 128 DN（`pedestal_status="documented"`，手册 Ver. 1.3
  p.172 “8LSB@8bit” 按 8×16 换算），来源写入 provenance `black_level`。
  `scale_by_exposure_ratio` 按 (t_cal·g_cal)/(t·g) 缩放归一化值后再乘矩阵。
- 全流程没有 EV、色调曲线或逐图统计：校色预览与下载图只把 linear sRGB 截到
  [0, 1] 后做 sRGB OETF（provenance `display.exposure_adjustment="none"`）。
  逐块色差和批量分析导出尚未接入。

新增数值契约的测试源码为 `tests/test_gox_color.py`。第一阶段尚未通过运行验证；
既有参考的色块均值运算也不能代替整帧回放、色差验证或真实设备测试。
