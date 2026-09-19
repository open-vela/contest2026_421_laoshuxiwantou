# 智能家居面板 ↔ 手机 远程控制协议（v1，冻结）

队伍 421 / laoshuxiwantou，轨 C（协议层 + 设备模拟器），2026-09-19

## 1. 概述与设计目标

面板（`hello_app`，黄山派 SF32LB52 / openvela 模拟器）与"手机 App"之间用
**行式 JSON** 通信：每条消息一行 UTF-8 文本，以 `'\n'` 结尾，单行 ≤ 256 字节。
TCP 与 UART 两种承载走**同一套帧格式与同一份板端代码路径**
（`sim/sm_net.c` 传输 + `sim/sm_proto.c` 解析构造），PC 侧配套模拟器为
`sim/sim_device_pc.py`。

设计取舍：

- 不引第三方 JSON 库，也不复用 `sm_json_*`（那是规则 schema 定制的）；
  协议消息用"找 key 读值"的手写小解析器，畸形行一律丢弃并回 `ack ok:0`。
- 无重传、无会话管理，best-effort 发送——网络故障只丢消息，绝不阻塞
  UI 主循环（演示第一优先级）。
- 单客户端：新连接顶掉旧连接。

## 2. 帧格式

```
+----------+--------- 每行一条消息 ---------+------+
| UTF-8 JSON ... } | '\n' (0x0A) |（'\r' 容忍忽略）|
+---------------------------------------------------+
```

- 方向 1（手机 → 面板）：`cmd` 消息
- 方向 2（面板 → 手机）：`evt` 消息

## 3. 消息定义（冻结）

### 3.1 设置设备（手机 → 面板）

```json
{"cmd":"set","dev":"light1","prop":"on","value":1}
```

| 字段    | 说明 |
|---------|------|
| `dev`   | 设备 id，与设备表一致（`light1`/`light2`/`socket1`/`sensor1`） |
| `prop`  | `on`（0/1） \| `brightness`（0..100，仅灯） \| `temp`（0.1°C 单位，仅传感器，305 = 30.5°C） |
| `value` | 整数；取值合法性由引擎校验 |

未知设备 / 属性与类型不匹配 → `ack ok:0`。未知字段忽略。

### 3.2 应答（面板 → 手机）

```json
{"evt":"ack","ok":1}
{"evt":"ack","ok":0,"err":"invalid"}
```

`err` 说明：`invalid` 请求非法（畸形 JSON / 未知命令 / 未知设备 /
属性不匹配 / 行超长）；`error` 面板内部错误；`nomem` 缓冲不足。

### 3.3 查询设备列表（手机 → 面板）与回复

```json
{"cmd":"get"}
{"evt":"devices","devices":[{"id":"light1","type":"light","on":1,"brightness":60},
                            {"id":"light2","type":"light","on":0,"brightness":10},
                            {"id":"socket1","type":"socket","on":0},
                            {"id":"sensor1","type":"sensor","temp":265}]}
```

字段与 `/data/devices.json` schema 一致：`light` 带 `on`+`brightness`，
`socket` 带 `on`，`sensor` 带 `temp`（0.1°C）。

### 3.4 状态差量推送（面板 → 手机）

面板侧设备状态变化时（UI 操作、网络 set、假传感器、规则联动），每
≈500ms 轮询一次设备表并与基线快照比对，**每个变化字段推一条**。
基线在新 TCP 客户端接入时立即建立（差量 = 相对"该客户端接入时刻"的
变化，接入前后的变更不丢）；全量状态可随时 `get` 获取：

```json
{"evt":"state","dev":"light1","prop":"on","value":1}
```

### 3.5 规则命中推送（面板 → 手机）

```json
{"evt":"rule","id":"r1","name":"high temp light on","dev":"light1","prop":"on","value":1}
```

`dev`/`prop`/`value` 描述规则动作的执行结果（beep 动作时 `dev` 为条件
观察设备）。依赖引擎"第二槽"回调 `sm_engine_add_event_cb()`（槽 0 被
UI 的 `sm_engine_set_event_cb` 占用）；槽位不足（`SM_ERR_NOMEM`）时本
功能自动降级跳过，其余功能不受影响。

## 4. 会话与鲁棒性规则

- 面板为服务端（TCP）/ DCE（UART），单会话；TCP 新连接顶掉旧连接。
- TCP 监听 `INADDR_ANY` + `SO_REUSEADDR`，socket 全程 `O_NONBLOCK`；
  UART 8N1 raw，`O_NONBLOCK` + `VMIN=0/VTIME=0`。
- 接收按 `'\n'` 分帧：空行忽略；畸形行丢弃并回 `ack ok:0`；
  超长行（>255B）整行丢弃并回 `ack ok:0`。
- 发送 best-effort：短消息单次写出，未写完部分存单槽续传缓冲下个
  tick（≈50ms）续发；通道持续故障时丢消息但不阻塞。
- 传输启动失败（端口占用 / 串口打开失败）返回负值并置 disabled，
  `sm_net_tick()` 之后直接返回——演示不因网络挂掉而卡死。
- 数值接受 `true`/`false` 布尔别名（映射 1/0）；字符串数字不接受。

## 5. 演示拓扑

### 5.1 TCP 模式（openvela Linux 模拟器，回环）

```
+------------------ Linux 开发机（openvela 模拟器） ------------------+
|                                                                     |
|  +---------------------------+                 +-----------------+  |
|  | openvela 模拟器进程        |   127.0.0.1     | sim_device_pc.py|  |
|  |  hello_app 面板            | <---TCP:9000--->| （模拟手机 App） |  |
|  |   sm_net_start_tcp(9000)   |    行式 JSON     |  --host --port  |  |
|  |   sm_net_tick() @ ~50ms    |                 |  REPL / demo    |  |
|  +---------------------------+                 +-----------------+  |
+---------------------------------------------------------------------+
```

```bash
# 终端 1：面板（模拟器内 sm_net_start_tcp(9000) 由主程序调用）
./emulator.sh cmake_out/contest421
# 终端 2：PC 侧手机模拟器
python3 sim/sim_device_pc.py --host 127.0.0.1 --port 9000
```

### 5.2 UART 模式（黄山派 SF32LB52 板端）

```
+------------------------------+                      +--------------------------+
| 黄山派 SF32LB52（面板）       |   UART 115200 8N1    | PC（模拟手机 App）        |
|  hello_app                   | <------------------> | sim_device_pc.py         |
|   sm_net_start_uart(         |   /dev/ttyS1     |  --serial /dev/ttyUSB1   |
|     "/dev/ttyS1", 115200)    |   USB 转串口线        |  --baud 115200           |
|   sm_net_tick() @ ~50ms      |                      |  REPL / demo             |
+------------------------------+                      +--------------------------+
```

```bash
# 板端：主程序调用 sm_net_start_uart("/dev/ttyS1", 115200) 后正常演示
python3 sim/sim_device_pc.py --serial /dev/ttyUSB1 --baud 115200
```

## 6. 板端代码路径（sim/sm_net.c 状态机）

```
            start_tcp()/start_uart() 成功
DOWN ────────────────────────────────► TCP_LISTEN ──accept──► TCP_CONN
 ▲                                       ▲  ▲                   │
 │ start 失败 / 串口硬错误 → disabled     │  └──── 新连接顶掉旧 ──┤
 └───────────────────────────────────────┴────────── 对端关闭/错误 ─┘
 UART 模式：DOWN → UART，硬错误 → DOWN(disabled)

tick 主流程（~50ms）：注册规则回调 → 续传尾巴 → accept → 收包执行
（set→ack / get→devices）→ evt:rule 转发 → 每 10 tick 差量 evt:state
```

## 7. 已知限制（答辩口径）

- **D12X 无 WiFi/以太网驱动，局域网直连待硬件打通**；当前经回环 TCP /
  UART / 模拟器呈现**同一代码路径**（传输与协议实现与真机联网形态
  完全一致，仅承载不同）。
- 单客户端；无认证/加密；无消息重传与序号（演示场景报文少、频率低，
  丢帧表现为一次推送缺失，下次差量轮询自然补齐状态）。
- `evt:state` 为 ≈500ms 差量轮询，非变更即时推送；`evt:rule` 环形缓冲
  深 4 条，瞬时命中超过 4 条时丢最旧。
- UART 承载依赖物理连线与 USB 转串口稳定；串口硬错误后需重新调用
  start 才能恢复。
- 协议解析为固定形状提取（非完整 JSON 文法）：满足本协议所有冻结消息
  的收发，对协议外复杂 JSON 不做承诺。

## 8. 自测记录（2026-09-19，Linux host）

- `gcc` 编译 `sm_net.c + sm_proto.c`（-Wall -Wextra，0 告警）+ 引擎桩。
- TCP 端到端：`sim_device_pc.py` 连接桩面板，验证 set→`ack ok:1`、
  未知设备→`ack ok:0 err:invalid`、畸形行→`ack ok:0`、get→`evt:devices`、
  状态变化→`evt:state` 差量推送、桩规则触发→`evt:rule`。
- UART 端到端：Python `pty` 伪终端对承载，`--serial` 模式同样通过。
- 新连接顶掉旧连接、超长行丢弃、CRLF 兼容均已覆盖。
