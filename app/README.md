# Amiga Sensor Console（Web GUI）

基于 NiceGUI 的传感器采集控制台：在**宿主机**上运行（使用仓库根的 uv `.venv`），
通过 `docker exec` 控制容器 `amiga-sensor-dev` 内的 C++ 二进制。

## 首次运行

1. **构建 C++ 侧**（容器内，一次性）：本 GUI 依赖两个新目标——`jai_snapshot`
   与带 `--json` 的 `jai_discover`。`.devcontainer/docker-compose.yml` 新增了
   `cap_add: SYS_NICE`（AmigaDrivers 的文件 caps 需要它），因此需要重建容器：

   ```bash
   docker compose -f .devcontainer/docker-compose.yml up -d --force-recreate
   docker exec -w /workspace amiga-sensor-dev bash Build.bash
   ```

2. **启动 GUI**（宿主机，仓库根目录）：

   ```bash
   uv sync          # 首次或依赖变化后
   uv run amiga-gui # 打开 http://<host>:8619
   ```

   端口/绑定可用环境变量覆盖：`AMIGA_GUI_PORT`、`AMIGA_GUI_HOST`。

## 页面

| 页面 | 功能 |
|---|---|
| `/` 总览 | 各传感器 Enable 开关（写 config-main.yaml）、启动/停止采集、逐传感器状态卡（健康/数据量/写入速率）、磁盘量表、会话信息 |
| `/config` 配置 | 6 个配置文件的原文编辑（注释保留）+ 语法校验 + 原子保存（.bak 备份）；运行中保存 → 下次启动生效 |
| `/logs` 日志 | 统一会话日志实时滚动，级别/模块过滤，跟随开关 |
| `/gox` GoX 工具 | jai_discover 扫描表格；单张拍照预览（曝光/增益滑条 → jai_snapshot → unpack_raw.py 解码 PNG → 直方图/过曝率），连续预览模式 |

## 行为要点

- **启停是进程级的**：任何传感器组合的变更都通过 Enable 开关 + 重启采集生效
  （统一架构中四驱动同生命周期）。停止 = 容器内 `pkill -TERM`，15s 后升级 KILL。
- **关闭 GUI 不会停止采集**；重新打开 GUI 会自动接管正在运行的会话
  （读取最新时间戳目录并回放日志重建状态，此时退出码不可知）。
- **相机独占**：采集进程启用 GoX 运行期间，扫描/拍照按钮被禁用；反之拍照进行中
  也会阻止启动采集。
- 快照产物在 `app/_runtime/snapshot/`（gitignore，自动保留最近 10 次）。
- GUI 无鉴权且默认绑定 0.0.0.0 —— 仅面向采集车局域网使用。
- `Enable Logging: false` 会让健康监控与日志页失效（启动前会弹警告）。
- Output Directory 必须位于容器挂载可见范围（仓库目录内，或
  `/mnt/SharedData/Post_Processing_Data`），否则启动前置检查会拦截。
