# 传感器驱动优化项目 - Agent Teams 配置

## 项目概述

本项目包含两个传感器驱动子系统：
- **INS401 驱动**（参考模板，基本定型）：GNSS/IMU 融合设备的高性能 C++ 驱动
- **SICK LMS4XXX 驱动**（待优化目标）：SICK 2D LiDAR 扫描仪驱动，需按 INS401 风格重构

核心场景：农业 3D 扫描与植物表型分析，多传感器融合系统（2D LiDAR 600Hz + 多个高频来自INS401的数据包）

## 关键技术约束

### 数据流架构（必须遵守）
1. **INS401 特例**：部分数据包需实时解析二进制（如 GNSS 定位结果，和用于初始静态初始化的 IMU 结果），其余均直接写入磁盘
2. **SICK LMS4XXX（及其他传感器）**：所有数据先以二进制写入磁盘，用户终止驱动时再调用解包接口输出 ASCII 文件
3. **零丢包设计**：高频数据采集（LiDAR 600Hz，多个高频来自INS401的数据包）必须保证数据完整性

### C++ 并发与性能要求
- 使用现代 C++17 标准
- 数据采集线程与磁盘 I/O 线程分离，通过无锁队列（lock-free queue）或双缓冲（double buffer）解耦
- 网络接收使用 epoll/select 或独立接收线程，避免主线程阻塞
- 磁盘写入使用大块缓冲（≥64KB），减少系统调用频率
- 信号处理（SIGINT/SIGTERM）优雅关闭：停止采集 → 刷新缓冲 → 调用解包 → 输出 ASCII
- 禁止在数据热路径上使用 `std::mutex`，改用无锁结构或 SPSC ring buffer

### 编码风格（对齐 INS401）
- 类命名：`PascalCase`
- 方法命名：遵循 INS401 已有风格
- 文件组织：头文件/实现分离，驱动核心类、数据解析器、缓冲管理器各自独立
- 错误处理：统一日志框架，不使用 exception 做流程控制
- RAII 管理所有资源（socket、文件句柄、线程）

## 模块边界（示例，请以实际结构为准）

```
项目根目录/
├── ins401_driver/          # 只读参考，做优化，尽量不修改
│   ├── include/
│   ├── src/
│   └── CMakeLists.txt
├── lms4xxx_driver/    # ✅ 优化目标
│   ├── include/
│   ├── src/
│   └── CMakeLists.txt
├── common/                 # ⚠️ 可能需要共享的工具类（需协调修改）
│   ├── ring_buffer.h
│   ├── binary_writer.h
│   ├── signal_handler.h
│   └── logger.h
└── CMakeLists.txt
```

## ⚠️ 重要规则

### 编译规则
- **所有 Agent 禁止自行编译或构建代码**
- 所有代码构建和编译由用户在 Docker 环境中完成
- 用户会将编译结果和错误信息反馈给 Agent
- Agent 只负责：阅读代码、分析架构、编写/修改代码、审查代码

### 协作规则
- 修改优化 `common/` 目录下文件前，必须先通过消息与 Team Lead 协调
- 每个 Teammate 只修改自己负责的文件
- 代码修改前先发消息说明修改意图，获得确认后再动手
- 所有架构决策必须经过团队讨论后由 Team Lead 确认

---

## Agent Team 角色定义

### 🎯 Team Lead（协调者）
- **职责**：任务分解、进度协调、架构决策仲裁、与用户沟通
- **模式**：Delegate Mode（不直接写代码）
- **重点**：确保 SICK 驱动的重构方向与 INS401 风格一致

### 📖 Teammate 1: Architecture Analyst（架构分析师）
- **职责**：深入阅读 INS401 和 SICK LMS4XXX 代码，输出架构对比报告
- **文件范围**：所有文件（只读分析），输出分析文档到 `docs/`
- **关注点**：
  - INS401 的线程模型、缓冲策略、数据流设计
  - SICK 当前代码与 INS401 的差异点
  - 提取可复用的设计模式

### ⚡ Teammate 2: Concurrency Specialist（并发专家）
- **职责**：设计和实现高并发组件（ring buffer、异步 I/O、信号处理）
- **文件范围**：`common/` 目录、`lms4xxx_driver/src/` 中的线程相关代码
- **关注点**：
  - 无锁队列 / SPSC ring buffer 实现
  - 磁盘写入缓冲策略
  - 优雅关闭（graceful shutdown）流程
  - 600Hz 数据流的零丢包保证

### 🔧 Teammate 3: SICK Driver Implementer（驱动实现者）
- **职责**：按照 INS401 风格重写 SICK LMS4XXX 驱动核心逻辑
- **文件范围**：`lms4xxx_driver/` 全部文件
- **关注点**：
  - SICK SOPAS 协议处理
  - 网络通信（TCP 接收数据）
  - 二进制数据写入磁盘
  - 用户终止时的解包流程（二进制 → ASCII）
  - 对齐 INS401 的类结构和命名风格

### 🔍 Teammate 4: Code Reviewer（代码审查员）
- **职责**：审查所有代码变更，检查并发安全性、性能瓶颈、风格一致性
- **模式**：Plan Mode（只读审查，不直接修改代码）
- **文件范围**：所有文件（只读）
- **关注点**：
  - 线程安全：数据竞争、死锁、资源泄漏
  - 性能：热路径上的不必要拷贝或锁竞争
  - 风格：与 INS401 编码规范的一致性，包括注释、命名、文件组织、错误处理方式
  - 健壮性：边界条件、错误处理、资源清理

---

## 工作流程（三阶段）

### Phase 1: 代码阅读与架构分析
1. Architecture Analyst 阅读全部代码，输出架构分析报告
2. 团队讨论 INS401 的设计模式和可复用组件以及潜在的改进点
3. **与用户讨论**：确认优化方向和架构设计

### Phase 2: 设计与实现
1. Concurrency Specialist 设计并发组件（ring buffer、writer、signal handler）
2. 优化现有的INS401 驱动中相关组件，确保其性能和稳定性
3. SICK Driver Implementer 按照确认的架构重写驱动
4. 两者通过消息协调接口设计
5. **用户编译并反馈结果**

### Phase 3: 审查与迭代
1. Code Reviewer 审查所有变更
2. 发现问题反馈给对应 Teammate 修复
3. **用户最终编译验证**

## 验证命令（仅供用户在 Docker 中执行）
```bash
# 用户在 Docker 中编译
cd build && cmake .. && make -j$(nproc)

# 用户运行测试
./bin/AmigaDrivers
```
