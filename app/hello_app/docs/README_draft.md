# README 作品说明（草稿，提交前替换队伍仓根 README.md）

> 草稿状态说明（集成者替换前删除本段）：本文为轨 D 产出的五节作品说明草稿。
> 仍属占位、需主集成 Agent 提交前补齐的内容统一用「【占位】」标出：
> 演示视频链接、AI Coding 日志目录中的实际 GitHub 登录名、公共仓 PR 记录、
> PC 模拟器脚本的确切命令行（sim/ 轨交付后按实改写）。

# 智能家居中控面板（D12X）— 本地规则引擎 + LVGL 触控 UI

队伍：421 / laoshuxiwantou　赛道：AI 硬件产品创新　平台：ArtInChip D12X-Demo68-nor + openvela（dev-ai-contest-2026 分支）

## 一、作品简介

一台基于 openvela（NuttX 内核）+ LVGL 的 4.3 吋 480×272 触控智能家居中控面板，运行在 ArtInChip D12X（RISC-V，PSRAM 4MB）上。面板管理 4 台设备（客厅灯 light1、卧室灯 light2、智能插座 socket1、温度传感器 sensor1），核心是一套完全跑在板上的**本地规则引擎**：条件→动作、边沿触发、支持链式联动，规则以 JSON 持久化在 /data/rules.json，断电重启自动加载、文件损坏自动回退默认规则集。UI 三页面（总览/场景/规则编辑）全部触控操作：总览页实时刷新设备卡片与触发记录，场景页一键回家/离家/睡眠，规则页下拉式编辑、保存即落盘热生效。另提供 PC 设备模拟器（TCP 回环/UART 传输），可在不开联网硬件的前提下演示"面板↔设备"双向同步。规则命中与场景切换有提示音（/data/beep.wav）。

（约 290 字）

## 二、选题方向

选 **AI 硬件产品创新** 赛道，作品落点是" genuinely 能在家服役 "的智能家居中控：

1. **断网可用**：智能家居的中枢价值在"联动永远在场"。本作品规则引擎、设备状态、场景逻辑全部在板端本地执行，不依赖云、不依赖路由器；拔掉一切网络，"温度越限→自动开灯→超温报警"依然成立（答辩主线演示即断电重启后联动自动恢复）。
2. **断电不丢**：规则与设备状态持久化在板载数据分区 /data（littlefs），原子写（.tmp+rename）防写半截；损坏/拔卡场景回退出厂默认集，保证任何情况下能启动、不卡死。
3. **数据不出户**：温度、开关状态、生活规律等敏感数据全程留在本地，契合家庭场景隐私底线；协议层用"设备模拟器 + TCP 回环/UART"呈现扩展形态，联网硬件为后续增强项（选型与移植结论见仓库日志与 docs/defconfig_notes.md）。
4. **工程可复现**：全部代码在本队伍仓内，公共仓零改动；评委可按第四节从零复现编译烧录运行。

## 三、目录结构

```text
contest2026_421_laoshuxiwantou/
├── app/hello_app/              # 中控应用本体（linkfile 映射 packages/demos/contest2026_421_hello_app）
│   ├── smarthome_types.h       #   冻结契约：物模型/规则/引擎 API（只读，修改须走主集成者）
│   ├── smarthome_storage.h     #   冻结契约：JSON 持久化 schema 与存储 API
│   ├── smarthome_internal.h    #   内部接口：beep/假传感器/网络 tick
│   ├── smarthome_main.c        #   主程序入口（主循环：LVGL tick + 引擎/UI/网络/假传感器 tick）
│   ├── engine/                 #   规则引擎轨：求值（边沿触发+链式联动）、JSON 解析、SD 持久化、提示音、假传感器、host 自测
│   ├── ui/                     #   UI 轨：三页面（总览/场景/规则编辑）+ 顶栏 + tab 栏，480x272 全触控
│   ├── sim/                    #   协议层轨：TCP 回环/UART 传输 + 状态差量发布/指令执行（PC 模拟器配套）
│   └── docs/                   #   本作品文档：defconfig 调研笔记、规则示例 JSON、演示口播稿
├── board/contest_board/        # 板级适配目录（映射 vendor/openvela/boards/contest2026_421_board）；
│                               #   当前为占位骨架，实际编译复用 artinchip demo68-nor 官方板级层（见 docs/defconfig_notes.md 第四节）
├── quickapp/hello_quickapp/    # 快应用形态样例（本作品未使用）
├── logs/                       # AI Coding 日志（contest-log-collector 归档，JSONL，评分输入之一）
└── README.md                   # 本文件
```

## 四、运行方式

前提：Linux 主机（已验证环境为 Ubuntu 类系统），网络可达 GitHub。

### 4.1 拉取工程

```bash
mkdir openvela && cd openvela
repo init -u https://github.com/open-vela/contest2026_421_laoshuxiwantou \
  -b dev-ai-contest-2026 -m contest2026_421_laoshuxiwantou.xml
repo sync -c -j8
```

### 4.2 编译（工作区根目录执行）

首次先装 RISC-V 工具链：

```bash
./vendor/artinchip/tools/env.sh
```

编译（板级配置二选一）：

```bash
# 候选 A（当前可复现基线，已验证）：artinchip 官方 nsh_lvgl 配置
./build.sh vendor/artinchip/boards/d12x/demo68-nor/configs/nsh_lvgl/ -j8

# 候选 B：队伍仓 contest_board 的 smarthome 板级配置
# （板级移植中——board/contest_board 当前为占位骨架，需 ARCH_CHIP 层对接；
#   以候选 A 的 artinchip nsh_lvgl 为当前可复现基线）
./build.sh vendor/openvela/boards/contest2026_421_board/configs/smarthome/ -j8
```

产物为 `nuttx.bin`。配置差异（关 LVGL demo、开网络回环等）见
`app/hello_app/docs/defconfig_notes.md`；规则文件样例见
`app/hello_app/docs/rules_example.json`。

### 4.3 烧录（由硬件同学在 Windows 侧执行，工具以其现场说明为准）

【占位：烧录工具与参数由硬件同学按实际工具填写；镜像取工作区根目录 nuttx.bin。】

### 4.4 运行与演示

1. 串口进入 NSH（日志串口，波特率 115200）。
2. 挂载确认：`/data` 由启动脚本自动挂载（littlefs 数据分区）；如需替换提示音，
   把 16k/16bit/单声道 PCM WAV（44 字节头）放到 SD 卡根目录后
   `mount -t fatfs /dev/mmcsd1 /sdcard && cp /sdcard/beep.wav /data/`
   （无该文件时提示音静默跳过，不影响其它功能）。【占位：挂载点/命令以实机为准】
3. 运行：NSH 下执行 `smarthome`。
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
6. PC 设备模拟器：板端支持 TCP 回环（127.0.0.1 服务）与 UART 两种传输；
   PC 侧 Python 脚本与板端走同一份 JSON 消息（设备状态同步/控制指令）。
   【占位：模拟器脚本名与命令行以 sim/ 轨最终交付为准】
7. `/data/rules.json` 格式与示例：见 `app/hello_app/docs/rules_example.json`
   （schema 冻结于 `app/hello_app/smarthome_storage.h`，未知字段忽略，
   损坏自动回退默认规则集）。

### 4.5 公共仓改动记录

本作品全部在队伍仓内完成，未修改 nuttx/apps/vendor 等公共生产仓。
【占位：如后续有公共仓 PR（修复/上游化），在此列出 PR 链接与状态。】

## 五、AI Coding 使用说明

本项目由 AI 深度参与完成（人负责接线/烧录、看串口日志、跑编译；代码 100% AI 编写）：

1. **契约先行**：主集成 Agent 先冻结两份头文件契约
   （smarthome_types.h 物模型/引擎 API、smarthome_storage.h 持久化 schema），
   所有后续代码只依赖契约，杜绝并行开发期的接口漂移。
2. **四子任务并行**：在契约冻结后，由四个子 Agent 并行开发——
   引擎轨（engine/：规则求值+JSON+持久化+beep+假传感器+host 自测）、
   UI 轨（ui/：LVGL 三页面）、模拟器轨（sim/：TCP 回环/UART 协议层）、
   文书轨（docs/：本 README、defconfig 笔记、示例规则、口播稿）。
3. **串行集成**：主集成 Agent 逐轨合入、编译修编、跑 codesize/memdump
   核对 3MB os 分区与 4MB RAM 预算。
4. **单线程 tick 模型**：整个应用单任务运行，LVGL 主循环内以 tick 驱动
   UI 刷新（~200ms）、网络收发（~50ms）、假传感器（~100ms），引擎/存储/
   模拟设备零线程零锁，从架构上规避并发缺陷（契约文件 smarthome_types.h
   头注明确该线程模型）。
5. **日志**：各阶段 AI 会话日志按《AI Coding 日志归集与提交手册》用
   contest-log-collector 归档至 `logs/<github-login>/<日期>/`（JSONL）。
   【占位：实际登录名目录与当日日志清单提交前确认】
