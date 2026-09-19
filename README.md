# 智能家居中控面板（D12X）— 本地规则引擎 + LVGL 触控 UI

队伍：421 / laoshuxiwantou　赛道：AI 硬件产品创新　平台：ArtInChip D12X-Demo68-nor + openvela（dev-ai-contest-2026 分支）

## 一、作品简介

一台基于 openvela（NuttX 内核）+ LVGL 的 4.3 吋 480×272 触控智能家居中控面板，运行在 ArtInChip D12X（RISC-V，PSRAM 4MB）上。面板管理 4 台设备（客厅灯 light1、卧室灯 light2、智能插座 socket1、温度传感器 sensor1），核心是一套完全跑在板上的**本地规则引擎**：条件→动作、边沿触发、支持链式联动，规则以 JSON 持久化在 /data/rules.json，断电重启自动加载、文件损坏自动回退默认规则集。UI 三页面（总览/场景/规则编辑）全部触控操作：总览页实时刷新设备卡片与触发记录，场景页一键回家/离家/睡眠，规则页下拉式编辑、保存即落盘热生效。另提供 PC 设备模拟器（TCP 回环/UART 传输）与板内"手机"回环演示（`smarthome --phone`），在不开联网硬件的前提下演示"面板↔设备"双向同步。规则命中与场景切换有提示音（/data/beep.wav）。

## 二、选题方向

选 **AI 硬件产品创新** 赛道，作品落点是"能真正在家服役"的智能家居中控：

1. **断网可用**：智能家居的中枢价值在"联动永远在场"。本作品规则引擎、设备状态、场景逻辑全部在板端本地执行，不依赖云、不依赖路由器；拔掉一切网络，"温度越限→自动开灯→超温报警"依然成立（答辩主线演示即断电重启后联动自动恢复）。
2. **断电不丢**：规则与设备状态持久化在板载数据分区 /data（littlefs，启动即挂载），原子写（.tmp+rename）防写半截；损坏/拔卡场景回退出厂默认集，保证任何情况下能启动、不卡死。
3. **数据不出户**：温度、开关状态、生活规律等敏感数据全程留在本地，契合家庭场景隐私底线；协议层用"设备模拟器 + TCP 回环/UART"呈现扩展形态，联网硬件为后续增强项（选型与移植结论见 `app/hello_app/docs/defconfig_notes.md`）。
4. **工程可复现**：全部代码在本队伍仓内，公共仓零改动；评委可按第四节从零复现编译烧录运行。

## 三、目录结构

```text
contest2026_421_laoshuxiwantou/
├── app/hello_app/              # 中控应用本体（linkfile 映射 packages/demos/contest2026_421_hello_app）
│   ├── smarthome_types.h       #   冻结契约：物模型/规则/引擎 API（只读，修改须走主集成者）
│   ├── smarthome_storage.h     #   冻结契约：JSON 持久化 schema 与存储 API
│   ├── smarthome_internal.h    #   内部接口：beep/假传感器/网络 tick/板内回环演示
│   ├── smarthome_main.c        #   主程序入口（单任务主循环：LVGL + 引擎/UI/网络/假传感器 tick）
│   ├── engine/                 #   规则引擎轨：求值（边沿触发+链式联动）、JSON 解析、持久化、提示音、假传感器、host 自测
│   ├── ui/                     #   UI 轨：三页面（总览/场景/规则编辑）+ 顶栏 + tab 栏，480x272 全触控
│   ├── sim/                    #   协议层轨：TCP 回环/UART 传输 + 板内"手机"回环演示 + PC 模拟器脚本 + 协议文档
│   └── docs/                   #   defconfig 调研笔记、规则示例 JSON、演示口播稿
├── board/contest_board/        # 板级目录（映射 vendor/openvela/boards/contest2026_421_board）：
│   ├── configs/smarthome/defconfig  #   本作品固件配置（已验证一键出固件，复用 artinchip 板级/芯片层）
│   └── scripts/Make.defs       #   转发到 artinchip demo68-nor 公共 Make.defs 的软链
├── tools/                      # gen_beep.py + beep.wav（演示提示音）；aic_uart_upg.py 为原有烧录辅助脚本
├── quickapp/hello_quickapp/    # 快应用形态样例（本作品未使用）
├── logs/                       # AI Coding 日志（contest-log-collector 归档，JSONL，评分输入之一）
└── README.md                   # 本文件
```

## 四、运行方式

前提：Linux 主机（已验证环境为 Ubuntu 类系统），网络可达 GitHub；RISC-V 工具链由 `vendor/artinchip/tools/env.sh` 安装。

### 4.1 拉取工程

```bash
mkdir openvela && cd openvela
repo init -u https://github.com/open-vela/contest2026_421_laoshuxiwantou \
  -b dev-ai-contest-2026 -m contest2026_421_laoshuxiwantou.xml
repo sync -c -j8
```

### 4.2 编译（工作区根目录执行）

```bash
# 首次先安装 RISC-V 工具链（产物在 vendor/artinchip/toolchain/）
./vendor/artinchip/tools/env.sh

# 本作品固件（推荐，已验证）：队伍仓 smarthome 板级配置
#   基于 artinchip nsh_lvgl 基线：去掉 LVGL demo、使能 smarthome 应用、
#   开启 NET+TCP+LOOPBACK（TCP 回环呈现协议层同一代码路径）
./build.sh ../vendor/openvela/boards/contest2026_421_board/configs/smarthome -j8

# 对照基线（纯平台验证用，不含 smarthome 应用）：
./build.sh vendor/artinchip/boards/d12x/demo68-nor/configs/nsh_lvgl/ -j8
```

产物为 `nuttx/nuttx.bin`（约 1.27MB，os 分区 3MB 占用约 42%；psram 静态占用约 34.6%）。配置差异说明见 `app/hello_app/docs/defconfig_notes.md`；规则文件样例见 `app/hello_app/docs/rules_example.json`。

### 4.3 烧录（由硬件同学在 Windows 侧执行）

烧录工具与参数以硬件同学现场说明为准（可参考仓库根 `tools/aic_uart_upg.py`）；镜像取工作区根目录 `nuttx/nuttx.bin`。【占位：烧录命令与实测截图待硬件同学补充】

### 4.4 运行与演示

1. 串口进入 NSH（日志串口）。
2. `/data`（littlefs 数据分区）由启动脚本自动挂载，规则 JSON 直接可用。
   如需提示音：把 `tools/beep.wav`（16k/16bit/单声道 PCM WAV）拷到 SD 卡
   根目录后 `mount -t fatfs /dev/mmcsd1 /sdcard && cp /sdcard/beep.wav /data/`
   （无该文件时提示音静默跳过，不影响其它功能）。【占位：SD 挂载点以实机为准】
3. 运行：NSH 下执行 `smarthome`（加 `--phone` 同时启动板内"手机"回环演示）。
4. 页面操作：
   - **总览页**：light1/light2 开关+亮度滑条、socket1 开关、sensor1 温度卡；
     底部滚动显示最近 3 条规则触发记录。顶栏实时温度。
   - **场景页**：Go Home（全屋亮起 80%/50% + 插座开）、Away（全关）、
     Sleep（客厅夜灯 10%），点击即应用并有提示音。
   - **规则页**：点选规则 → 下拉改条件/比较符/阈值/动作 → Save 落盘
     /data/rules.json 并热生效；Reset 恢复出厂 4 条（高温开灯/低温关灯/
     高亮度开插座/超温报警）。
5. 内置假传感器每 5 秒步进 0.5°C，在 24.0→34.0°C 三角往复：升到 30.0°C
   自动开客厅灯、32.0°C 报警音，降回自动关灯——全程无需人工干预。
6. "远程控制"演示（联网硬件 no-go 的替身，同一代码路径）：
   - **板内回环（实机推荐）**：`smarthome --phone` 启动后，板内"手机"
     客户端经 127.0.0.1 真实 TCP socket 向面板服务端发送与 PC 脚本相同的
     JSON 指令（开灯→调亮度→开插座…，每 60 秒一轮），UI 卡片同步变化、
     串口打印收发日志。
   - **PC 模拟器（模拟器/局域网拓扑）**：
     ```bash
     python3 app/hello_app/sim/sim_device_pc.py --host 127.0.0.1 --port 9000
     # REPL：light1 on 1 / light1 brightness 80 / get / demo
     # 串口拓扑：--serial /dev/ttyUSB1 --baud 115200（需 pip install pyserial）
     ```
   - 已知限制：D12X 无 WiFi/以太网驱动，局域网直连待硬件打通；协议细节见
     `app/hello_app/sim/protocol.md`。
7. `/data/rules.json` 格式与示例：见 `app/hello_app/docs/rules_example.json`
   （schema 冻结于 `app/hello_app/smarthome_storage.h`，未知字段忽略，
   损坏自动回退默认规则集）。

### 4.5 公共仓改动记录

本作品全部在队伍仓内完成，未修改 nuttx/apps/vendor 等公共生产仓，无公共仓 PR。`board/contest_board/configs/smarthome/` 通过 `CONFIG_ARCH_BOARD_CUSTOM_DIR` 相对路径复用 artinchip 板级/芯片层，无需改动公共仓文件。

## 五、AI Coding 使用说明

本项目由 AI 深度参与完成（人负责接线/烧录、看串口日志、跑编译；代码 100% AI 编写）：

1. **契约先行**：主集成 Agent 先冻结两份头文件契约
   （smarthome_types.h 物模型/引擎 API、smarthome_storage.h 持久化 schema），
   所有后续代码只依赖契约，杜绝并行开发期的接口漂移。
2. **四子任务并行**：在契约冻结后，由四个子 Agent 并行开发——
   引擎轨（engine/：规则求值+JSON+持久化+beep+假传感器+host 自测 85 用例）、
   UI 轨（ui/：LVGL 9.1 三页面）、模拟器轨（sim/：TCP 回环/UART 协议层+PC 脚本）、
   文书轨（docs/：本 README、defconfig 笔记、示例规则、口播稿）。
3. **串行集成**：主集成 Agent 逐轨合入、独占编译修编；引擎事件回调扩双槽
   （UI 触发记录 + 网络 evt:rule 转发共用）；网络栈按需启用（NET/TCP/LOOPBACK）
   并用 codesize 核对 os 分区 3MB 与 4MB psram 预算。
4. **单线程 tick 模型**：整个应用单任务运行，LVGL 主循环内以 tick 驱动
   UI 刷新（~200ms）、网络收发（~50ms）、假传感器（~100ms），引擎/存储/
   模拟设备零线程零锁，从架构上规避并发缺陷（契约文件 smarthome_types.h
   头注明确该线程模型）。
5. **日志**：各阶段 AI 会话日志按《AI Coding 日志归集与提交手册》用
   contest-log-collector 归档至 `logs/mrou20000221/<日期>/`（JSONL）。

演示视频：【占位：录制后补充链接】

---

## 附：官方文档索引（原模板保留）

- [《大赛总览》](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/contest_overview.md)：赛道、流程、评分、资源
- [《参赛代码提交指南》](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/code_submission_guide.md)：仓库获取、提交流程、时间与权限（**以此为准**）
- [《AI Coding 日志归集与提交手册》](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_coding_log_guide.md)：AI 对话日志导出与提交
- [AI 硬件赛道教程导航](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/contest_2026/ai_hardware/ai_hardware_guide_index.md)
