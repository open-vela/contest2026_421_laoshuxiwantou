/****************************************************************************
 * app/smarthome_internal.h — 内部跨模块接口（主集成 Agent 维护）
 *
 * 非冻结契约：子任务只按此声明实现/调用，不得修改本文件。
 * 发现接口不足 → 报告集成者，由集成者统一调整。
 ****************************************************************************/

#ifndef __SMARTHOME_INTERNAL_H
#define __SMARTHOME_INTERNAL_H

#include "smarthome_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- engine/ 轨实现 ---------------------------------------------------- */

/* 引擎事件回调第二槽（槽 0 由 UI 经契约 sm_engine_set_event_cb 占用）。
 * sim/ 轨用它转发 evt:rule；已占用返回 SM_ERR_NOMEM。 */
int sm_engine_add_event_cb(sm_event_cb_t cb, void *arg);

/* 播放提示音 /data/beep.wav（16k/16bit/mono PCM WAV，44 字节头）。
 * 走 NuttX audio 框架写 /dev/audio；CONFIG_AUDIO 未开、设备或文件缺失、
 * 任何失败一律静默返回负值，绝不阻塞/崩溃调用方。 */
int sm_beep_play(void);

/* 假传感器：内部 5 秒节拍驱动 sensor1.temp 在 240~340（0.1°C）三角波
 * 往复，经 sm_set_device 写入以触发规则。由主循环 ~100ms 调一次 tick。 */
void sm_sim_sensor_tick(void);
void sm_sim_sensor_set_enabled(bool en);
bool sm_sim_sensor_enabled(void);

/* ---- sim/ 轨实现 -------------------------------------------------------- */

/* 传输层二选一启动（都失败不致命，演示可继续用板内模拟）：
 * tcp  监听 TCP 服务（板内 127.0.0.1 回环或模拟器下与 PC 脚本互通）
 * uart 以串口为传输（如 /dev/ttyS1，115200），PC 脚本经 USB 转串口接入 */
int sm_net_start_tcp(int port);
int sm_net_start_uart(const char *devpath, int baud);

/* 主循环 ~50ms 调一次：收包解析执行、状态轮询差量发布（evt:state）、
 * 规则命中事件转发（evt:rule）。内部无线程无阻塞。 */
void sm_net_tick(void);

/* ---- sim/ 板内"手机"回环演示（sm_phone_demo.c，集成者维护） ------------ */

/* 主入口识别 `smarthome --phone` 后调用；经 127.0.0.1 回环按脚本发送
 * 与 PC 模拟器相同的 set 指令（真 socket 代码路径）。 */
void sm_phone_demo_start(uint16_t port);

/* 主循环每拍调用；非阻塞推进连接与脚本发送。 */
void sm_phone_demo_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* __SMARTHOME_INTERNAL_H */
