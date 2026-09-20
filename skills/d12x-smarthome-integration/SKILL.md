---
name: d12x-smarthome-integration
description: openvela AI 大赛 D12X 智能家居中控面板的集成与调试手册——从本项目 6 个固件版本迭代提炼的红线规则与排障模式。凡在 D12X-Demo68-nor（ArtInChip BSP）上开发、打包烧录、分析板端串口日志、调试 LVGL 触摸/网络/持久化问题时先读本 skill。Trigger: D12X、demo68、AiBurn、sftool、artinchip 打包、LVGL 触摸无响应、smarthome、NuttX accept 阻塞、littlefs、rcS 自启。
---

# D12X 智能家居中控面板 — 集成与调试手册

队伍 421 / laoshuxiwantou 在 2026 openvela AI 大赛中沉淀的项目级 Skill。
适用对象：ArtInChip D12X-Demo68-nor（RISC-V，PSRAM 4MB，SPI NOR 16MB）+ openvela
dev-ai-contest-2026 + LVGL 9.1 + 本仓 `app/hello_app/smarthome` 应用。

## 一、红线规则（违反必翻车）

1. **NuttX TCP：不配 `CONFIG_NET_TCPBACKLOG=y`，`accept()` 无条件阻塞。**
   O_NONBLOCK 对 listen fd 的 accept 无效（`tcp_accept.c` 源码注明
   "except TCP accept with backlog enabled"），单任务 tick 主循环会静默死在
   第一次连接到达处。现象：界面卡死、心跳消失、无 panic。
2. **rcS 启动脚本会被 C 预处理器处理**（`RCSRCS`/`etc/init.d/rcS`）：脚本内
   不能出现任何 `#` 开头的注释行，否则 cpp 报 "invalid preprocessing
   directive"，romfs 构建失败。中文注释同理禁用。
3. **编辑 vendor 下的 rcS.nsh 后 etc romfs 不自动重建**：make 依赖未跟踪
   vendor 文件，需 `rm -rf arch/risc-v/src/board/etctmp{,.c,.o}` 强制重建；
   验证要看解包后的 `etctmp/etc/init.d/rcS`（`etctmp.c` 是十六进制转储，
   直接 grep 文本无效）。
4. **littlefs name_max 两端必须一致**：`makelittlefs.py` 打包参数 255，
   内核侧 `CONFIG_FS_LITTLEFS_NAME_MAX` 默认 32 → 挂载直接
   `Unsupported name_max (255 > 32)`，/data 挂不上、持久化全失效。
5. **AiBurn 只认整包 `.img`**：裸 `nuttx.bin` 刷入无法启动，必须走
   `vendor/artinchip/pack/pack.sh` 生成 FIT（d12x_os.itb）+ 分区表整包。
   打包产物固定名 `d12x_demo68-nor_v1.0.0.img`，复制版本镜像时只改
   文件名后缀，不动打包脚本。
6. **自启与外设注册的竞态**：rcS `smarthome &` 早于 GT911 input 设备注册，
   `lv_nuttx_touchscreen_create` 需带重试兜底（10×500ms），不能假设
   `/dev/input0` 已存在。
7. **烧录版本纪律**：每次出包必须重命名带版本号（v1.x），防止烧错旧镜像；
   验证前先看 `nuttx.bin` 时间戳与提交号对得上。

## 二、架构模式

- **单任务 tick 模型**：主循环顺序驱动 LVGL → 引擎 → 假传感器 → UI → 网络，
  全应用零线程零锁；一切耗时操作（持久化、网络）必须做成非阻塞状态机。
- **契约先行**：物模型/引擎 API（`smarthome_types.h`）与持久化 schema
  （`smarthome_storage.h`）冻结后并行分发子任务开发，杜绝接口漂移。
- **防抖落盘**：状态变更后延迟 500ms 统一写（滑条拖动/场景批量/联动链
  收敛只落一次盘）；高频无意义量（假传感器温度）不落盘，保护 SPI NOR。
- **原子写 + 回退**：`.tmp` + rename 防断电写半截；启动加载失败一律回退
  出厂默认集且不回写，保证"任何情况能启动、不卡死"。
- **可自证的恢复**：启动/落盘各打一行 `[SM]` 日志（rules loaded N /
  device state restored/saved），断电恢复演示在串口上肉眼可验证。

## 三、排障模式：串口日志二分法

主循环卡死类问题的定位链（本项目实测有效）：
1. 先加**心跳日志**（每 N 拍一行带状态量）→ 判断"死了还是没输入"；
2. 心跳停 → 加**分相位探针**（每拍在 lv_timer_handler 前后、ticks 完成点
   各打一条，只打前 3 拍）→ 锁定卡死发生在哪两个打印之间；
3. 读该区间的**上游源码**（NuttX 内核、LVGL driver）而不是猜——本项目
   即靠此发现 accept 阻塞的 BACKLOG 前置条件（红线 1）。
教训：v1.2 曾按"触摸竞态"假设修复并烧录验证，被日志证伪——**假设必须由
日志检验，不能连改两版**。

## 四、工程账（3MB os / 4MB PSRAM 红线下的预算参考）

| 项 | 实测（v1.6） |
|---|---|
| nuttx.bin | 1,273,120 B（os 分区 3MB 占 42%） |
| PSRAM 静态+堆 | 1,458,464 / 4MB = 34.77% |
| 本应用 text+data+bss | ~36KB flash / ~75KB RAM |
| host 自测 | 引擎+存储+JSON+传感器 85 例全过（gcc，SM_DATA_DIR 重定向） |

编译入口：`./build.sh ../vendor/openvela/boards/contest2026_421_board/configs/smarthome -j8`
（队伍仓 defconfig 经 `CONFIG_ARCH_BOARD_CUSTOM_DIR` 相对路径复用 artinchip
芯片层，公共仓零改动）；打包：`vendor/artinchip/pack/pack.sh`。
