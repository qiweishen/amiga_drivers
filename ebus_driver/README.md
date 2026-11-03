# 统一 eBUS 驱动与 RAW 格式

GOX 与 FX10 共用 `EbusCameraDriver`、接收线程、写盘线程和 `RawRecorder`。
`gox_config.cpp` 与 `fx10_config.cpp` 保留各自的 YAML、GenICam 配置顺序、
回读、PTP、触发、快门和健康检查。两个设备目录保留配置与工具入口，不再实现采集或写盘循环。

## 生命周期与所有权

`Init` 连接设备，执行 profile 准备动作，打开 stream，应用配置，按实际 PayloadSize
分配有界 SDK 缓冲和应用缓冲，发布设备元数据，注册共享 SensorSync。
`Run` 启动两个 worker，执行 profile 的开始前动作，发送 AcquisitionStart，
打开接收门闩并 Arm SensorSync；所有相机注册后才可能启动共享触发。

接收线程先读取 SDK 状态及主机接收时间，检查有效范围，把
`PvBuffer::GetDataPointer()` 的 `GetAcquiredSize()` 字节复制到独立拥有的内存，
随后 requeue SDK buffer，再交给有界写盘队列。写盘线程不访问 SDK 对象。
这里的原始数据是 **SDK 交付层 payload**，不是以太网/GVSP 抓包；
SDK 在交付前的重组或处理不能由此逆向恢复。

停止顺序为 Disarm → AcquisitionStop → 限时接收排空 → join 接收 →
队列排空并 join 写盘 → 最后健康/触发/stream 核对 → stream 关闭和设备释放 →
RAW 最终落盘与结果汇总。停止命令、排空期限、I/O、关闭或对账失败均使失败状态保持。
停止后的接收排空有五秒上限；超限报告失败，不能视作所有设备在途数据均已记录。

每台相机一组 worker、一套队列和一个目录。SDK 与应用 payload 内存受预算约束，
实际队列容量及收缩情况写入 device.json；JSON/SDK 内部开销不在该预算内。
接收 socket 单独按配置请求容量。依据本地 eBUS 6.5.1 的 PvStreamGEV.h，
请求超过 Linux rmem_max 时，SDK 已自动降至该上限并返回 INVALID_PARAMETER；
驱动告警后继续，在 Open 后读取实际容量，不再用原请求值拒绝降级结果。
元数据保留请求值、SDK 返回、可读取的 rmem_max 快照及实际回读；
回读不可用时保持 null，回读为零或其他 SDK 错误仍使启动失败。
回读可能含内核 bookkeeping，不能直接等同 payload 容量；驱动不修改系统参数。
满队列停止会话，保存已接纳数据并记录被拒 buffer 的身份，不覆盖旧记录。
异常/不完整 buffer 在范围安全时保留原字节和状态；无法证明范围安全时仅记录拒绝事件。
max_frames 统计有效 image buffer，停止排空中的额外有效帧记为明确的 limit_excluded 事件。

## 新产物与兼容

每台相机目录只有三个主要产物：

- `device.json`：ebus-device-v1，实际回读、设备身份、编译时 SDK 版本、配置来源、
  profile、缓冲预算和时间语义；运行时 SDK 版本未查询则为 null。
- `seg_00001.raw`、后续分段：ebus-raw-v1，每条记录自带完整 JSON 元数据与 payload。
- `events.jsonl`：初始化、开始前动作结果、健康采样、拒绝/排除、分段关闭、
  SensorSync 核对、最终统计和 session_end。

`device.json` 只在 Init 完成配置后写一次，保存设备身份、配置请求及回读、SDK、
缓冲配置和记录语义。Run 不再重新读取并复制整份设备元数据。
开始前动作的结果直接写入 `profile_start.actions`：GOX 的 `trigger_counter` 保存
复位是否尝试/确认、基线采样是否执行及基线值；device.json 中该块仅保留配置时的绑定结果。
FX10 的 `shutter_preparation` 保存快门脉冲写入结果，`missed_triggers` 保存计数基线和绑定结果；
device.json 中的 `profile.shutter_request` 仅表示配置意图，快门命令确认不等于机械位置已验证。
基线读不到时保持 null。`profile_start` 表示开始前动作已返回，实际采集启动另由
`acquisition_started` 记录；任一步骤失败仍沿原异常路径结束会话。
最终设备读数在 `device_health` 的 stop 事件，落盘完成状态与计数对账保留在 `session_end`。
旧记录中的 `profile_start_metadata.device` 不迁写；现有读取端不依赖该重复快照，
继续分别读取 device.json 和 session_end。
bytes/records 是记录数，valid_frames 是有效图像数，不能混用。
设备 timestamp 保留 SDK 原整数，单位、纪元无法确认时写 unverified；
主机 realtime/monotonic 是接收诊断时间，不是曝光时间，也不证明跨设备同步。
外触发且由共享 SensorSync 控制时核对帧数与曝光边沿；freerun 的采集窗口与
观察日志窗口不同，只检查日志完整性，帧/边沿数量对应关系明确记为 unknown。

新记录无需 idx 文件才能恢复布局。接收时不解包、不去 padding/Chunk、不 debayer、
不删除 FX10 status line、不做反射率校正，也不插入丢行补零。
相机端 PixelFormat、ROI、binning、AIE 等配置仍决定实际输出的信息量。
FX10 的旧 max_lines 对新 RAW 按 SDK 记录数轮转，max_mb 包含记录元数据；
原有 on_gap 不再控制补零，缺口作为记录元数据/事件保留并触发失败。
GOX 的 record_align、queue_on_full、on_buffer_error 继续接受旧 YAML，
但采用统一 RAW 策略；device.json 明确保存请求值与实际策略。统计周期继续按配置生效。

读取端同时支持旧 GOX jai-raw-seg 和旧 FX10 ENVI；旧采集文件不迁写。
CLI 名称 jai_snapshot、fx10_snapshot、fx10_reference 及 YAML 入口保留。
新增工具：

```text
ebus_driver/scripts/inspect_raw.py list CAMERA_DIRECTORY/seg_00001.raw
ebus_driver/scripts/inspect_raw.py verify CAMERA_DIRECTORY
ebus_driver/scripts/export_envi.py FX10_CAMERA_DIRECTORY NEW_EXPORT_DIRECTORY
```

具体参数见各工具源码；这些命令本次未执行。离线 ENVI 是 DN 派生产物，
包含来源记录映射，原始 RAW 保留用于重新解码；不声称从 ENVI 恢复被省略的 padding/Chunk。
不支持的 PFNC 仍可原样采集，预览/导出明确拒绝未知解码规则。

FX10 白/暗参考现在是 **两次独立连接的采集阶段**：同一不可变配置分别应用，
暗场开始前核对 serial/model、几何、PixelFormat、明确的成像回读与校准引用。
结果标记 `connection_scope="separate verified phases"`；这不证明未公开相机状态、
机械快门位置或物理白板位置一致。任一阶段失败不会发布成功参考结果。

## 二进制格式（v1）

所有整数为 little endian；不写 C++ 结构体内存，不做对齐填充。
CRC 使用 CRC32C/Castagnoli。每段按完整记录轮转，单条超过容量时独占一段。

| 文件头偏移 | 类型/长度 | 字段 |
|---|---|---|
| 0 | 8 bytes | EBUSRAW 后接 NUL |
| 8 | u16 | version=1 |
| 10 | u16 | header_size=64 |
| 12 | u32 | flags=0 |
| 16 | u64 | 创建主机 realtime ns |
| 24 | u64 | segment_index，从 1 开始 |
| 32 | 28 bytes | reserved=0 |
| 60 | u32 | 对前 60 字节的 CRC32C |

每条记录为 48-byte prefix + UTF-8 JSON + payload：

| 记录前缀偏移 | 类型/长度 | 字段 |
|---|---|---|
| 0 | 4 bytes | EBFR |
| 4 | u32 | prefix_size=48 |
| 8 | u32 | metadata_size，最多 1 MiB |
| 12 | u32 | flags=0 |
| 16 | u64 | payload_size |
| 24 | u64 | sequence，每相机跨分段连续，从 0 开始 |
| 32 | u32 | metadata CRC32C |
| 36 | u32 | payload CRC32C |
| 40 | u32 | 前 40 字节 CRC32C |
| 44 | u32 | reserved=0 |

JSON 含 block_id、device_timestamp_raw、host_realtime_ns、host_monotonic_ns、
acquired_size、payload_type、operation_result/name/ok、chunk_count、
valid_frame、block_id_anomaly、gap_before 和 image。
image 为 null 或包含 PixelFormat 数值、宽高、ROI offset、padding、
image_size/effective_image_size、相对整个 payload 的 payload_offset 及缺行/overrun 标志。
payload_offset 无法证明时为 null，读取端不得猜测。

## 完成与恢复边界

写入计数表示 write 完整返回，不表示逐记录已持久化。按配置周期 fdatasync，
分段及最终关闭也执行同步；目录创建和元数据发布采取不覆盖已有文件的方式。
磁盘错误保持失败；写入半条记录尽力截回完整前缀，截断失败仍可由长度及 CRC 定位损坏尾部。
文件系统/设备对 fsync 的实际保证不由静态代码证明。

实时读取允许最后文件暂时存在半条记录；正常离线读取必须验证完整记录、
序号、CRC 和 session_end 的计数/complete。无最终事件或损坏尾部不得当作完整成功会话；
恢复只能取明确完整的前缀。相机会话结果还须结合主程序 drivers.json 或工具最终结果，
因为最后事件写成后仍可能发生关闭/持久化失败。目录的存在或一张可预览图像不证明采集成功。

## 本次验证状态与后续检查

本次仅修改源码并做文本级静态核对，未执行 Git、依赖安装、CMake、C++ 编译/测试、
Python 项目脚本或硬件操作。增加了 RAW 布局/轮转/CRC/失败锁存、BlockID
及 Python 新旧格式/损坏输入/导出测试源码，尚未运行。

需要在另行允许的环境执行构建及测试，再用实际设备验证：两种相机快照及长时吞吐；
双相机共同 Arm/任一路失败；SIGINT、AcquisitionStop 失败与断链；满队列和磁盘错误；
packed 像素、padding、Chunk、错误 buffer、16/64 位 ID；白/暗独立阶段匹配；
新旧 GUI/CLI 解码和 ENVI 导出。静态核对不能证明硬件行为或无丢帧。
