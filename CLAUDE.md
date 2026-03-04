# 传感器驱动针对性优化 - Agent Teams 配置

## 项目背景

本项目已完成 INS401 和 SICK LMS4XXX 传感器驱动的初步开发, 现进入针对性优化阶段
每次优化由用户提出具体改进需求, Agent Team 协作完成从讨论到实施到审阅的完整闭环

## 核心工作流

```
用户提出改进需求
       │
       ▼
┌─────────────────────┐
│  System Architect   │◄──── 与用户反复讨论, 完全理解需求
│  (整体设计架构师)     │      不急于实施, 确认所有细节
└────────┬────────────┘
         │ 输出明确的优化计划
         ▼
┌─────────────────────────────────────────┐
│  C++ Logic Expert    C++ Concurrency    │
│  (逻辑专家)           Expert             │
│                      (多线程专家)         │
│  各自按计划实施对应部分                    │
└─────────────────┬───────────────────────┘
                  │ 实施完毕
                  ▼
         ┌─────────────────┐
         │  Code Reviewer   │──── 审阅代码质量 + 验证是否完成全部计划
         │  (代码审阅专家)   │
         └────────┬────────┘
                  │
          ┌───────┴────────┐
          │                │
     审阅通过          发现问题
          │                │
          ▼                ▼
  System Architect    反馈给对应专家
  全局把控确认         修复后重新审阅
          │
          ▼
  通知用户编译验证
```

## 编码规范

### 注释风格（全员必须遵守）
- 注释简明扼要, 禁止以句号(".")结尾
- 英文注释首字母大写, 其他小写, 不使用缩写
- 示例:

```cpp
// ✅ 正确
int buffer_size = 65536;  // 64KB flush threshold
void stop();              // Stop acquisition and trigger unpacking
std::atomic<bool> running_{false};  // Shared flag for graceful shutdown

// ❌ 错误 - 句号结尾
int buffer_size = 65536;  // 64KB flush threshold.

// ❌ 错误 - 冗余啰嗦
int buffer_size = 65536;  // This is the buffer size used for flushing data to the disk file.
```

- 函数注释用简短单行或双行说明功能和关键参数
- 禁止冗余注释, 代码本身已清晰时不加注释
- 修改任何文件时顺手修正已有注释中的句号

### C++ 编码规范
- C++17 标准
- RAII 管理所有资源
- 热路径禁止 std::mutex, 使用 SPSC ring buffer 或 atomic 操作
- 错误处理统一走日志, 不用 exception 做流程控制
- 命名风格对齐 INS401 已有代码

## ⚠️ 编译规则（最高优先级）

**所有 Agent 严禁执行以下操作：**
- `cmake`, `make`, `g++`, `gcc`, `clang++` 等任何编译命令
- `catkin_make`, `colcon build` 等构建系统命令
- 任何形式的代码编译和链接

**所有编译由用户在 Docker 中完成, 用户会将结果反馈给 Agent**

## 模块边界

```
项目根目录/
├── ins401_driver/              # 可修改
├── sick_lms4xxx_driver/        # 可修改
├── common/                     # 可修改（需 System Architect 协调）
├── docs/                       # 文档输出
└── CMakeLists.txt              # 可修改（需 System Architect 协调）
```

- 修改 `common/` 或根目录 `CMakeLists.txt` 前必须通知 System Architect
- 每个专家只修改 System Architect 分配给自己的文件
- 禁止两个专家同时编辑同一文件

---

## Agent 角色定义

### 🏗️ System Architect（整体设计架构师）
- **模式**: Delegate Mode（不直接写代码）
- **核心职责**:
  1. 与用户反复讨论, 完全理解每次优化需求的所有细节
  2. 不急于推进, 用户明确说"确认"或"开始实施"后才分发任务
  3. 将计划拆解为具体任务, 明确分配文件所有权
  4. Code Reviewer 审阅通过后做全局把控和最终确认
  5. 通知用户进行编译验证
- **讨论原则**:
  - 主动追问不明确的细节
  - 复述需求让用户确认
  - 提出技术方案供用户选择
  - 用户未确认前绝不推进实施

### 🧠 C++ Logic Expert（C++ 逻辑专家）
- **模式**: Default Mode
- **负责范围**: 业务逻辑优化
  - 数据解析, 协议处理, 状态机, 配置管理
  - SICK SOPAS 协议逻辑
  - 解包器(Unpacker)优化
  - 驱动初始化, 连接管理, 错误恢复
- **工作原则**:
  - 严格按 System Architect 计划执行
  - 不确定时先消息问 System Architect
  - 完成后通知 Code Reviewer 审查
  - 禁止编译

### ⚡ C++ Concurrency Expert（C++ 高流量多线程专家）
- **模式**: Default Mode
- **负责范围**: 并发与性能优化
  - Ring buffer, 锁策略, 原子操作, 内存序
  - 线程生命周期, 优雅关闭
  - 磁盘 I/O 性能, 缓冲策略, 批量写入
  - 数据流瓶颈分析
  - 多传感器高帧率 零丢包保证
- **工作原则**:
  - 严格按 System Architect 计划执行
  - 修改 common/ 时先通知 System Architect
  - 完成后通知 Code Reviewer 审查
  - 禁止编译

### 🔍 Code Reviewer（C++ 代码审核审阅专家）
- **模式**: Plan Mode（只读, 不修改代码）
- **审查维度**:
  1. **安全性**: 数据竞争, 死锁, 资源泄漏, 越界, 悬挂指针
  2. **性能**: 热路径锁竞争, 不必要拷贝, 缓存不友好访问
  3. **风格**: 注释无句号, 命名与 INS401 一致, 结构合理
  4. **完成度**: 逐条核对 System Architect 计划, 确认全部实施
- **审查结果**:
  - 通过 → 通知 System Architect 全局确认
  - 不通过 → 逻辑问题发给 Logic Expert, 并发问题发给 Concurrency Expert

---

## 协作协议

### 消息格式
```
[状态] 简要说明
[文件] 涉及的文件列表
```
状态值: PLAN / WIP / DONE / REVIEW / FIX_NEEDED / APPROVED

### 每轮优化完整流程
1. 用户描述改进需求
2. System Architect 与用户讨论（可能多轮对话）
3. 用户确认需求理解正确
4. System Architect 输出优化计划并分配任务和文件所有权
5. Logic Expert / Concurrency Expert 并行实施
6. 实施完成 → Code Reviewer 审查
7. 审查不通过 → 反馈修复 → 回到第 6 步
8. 审查通过 → System Architect 全局确认
9. 用户在 Docker 编译验证
10. 编译有错 → System Architect 分配修复 → 回到第 5 步
11. 编译通过 → 本轮完成, 等待下一个需求