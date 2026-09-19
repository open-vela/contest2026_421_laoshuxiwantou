/****************************************************************************
 * sim/sm_net_internal.h — sim 轨私有声明（非契约文件）
 *
 * 队伍 421 / laoshuxiwantou，轨 C（协议层 + 设备模拟器）
 *
 * 仅 sim/ 内部（sm_net.c / sm_proto.c）与 host 自测桩使用，不对外发布。
 * 冻结契约见 ../smarthome_types.h / ../smarthome_storage.h；
 * 对外接口见 ../smarthome_internal.h（sm_net_start_tcp / sm_net_start_uart
 * / sm_net_tick）。
 ****************************************************************************/

#ifndef __SM_NET_INTERNAL_H
#define __SM_NET_INTERNAL_H

#include "../smarthome_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * 规则事件转发回调槽位说明
 *
 * evt:rule 转发使用 ../smarthome_internal.h 中的
 * sm_engine_add_event_cb(cb, arg)（引擎"第二槽"，槽 0 由 UI 轨经
 * sm_engine_set_event_cb 占用；槽位已被占用时返回 SM_ERR_NOMEM）。
 *
 * 兜底：sm_net.c 内另有一个 __weak 同名实现（返回 SM_ERR_NOMEM），
 * engine 侧强符号存在时被自动覆盖——引擎实现暂缺时链接不失败，
 * evt:rule 优雅降级，其余功能不受影响。
 ****************************************************************************/

/* ---- sm_proto.c 提供：协议解析与构造（sm_net.c 调用） ------------------- */

/* 手机→面板命令类型 */
typedef enum
{
  SM_PROTO_CMD_NONE = 0,
  SM_PROTO_CMD_SET,             /* {"cmd":"set","dev":..,"prop":..,"value":..} */
  SM_PROTO_CMD_GET,             /* {"cmd":"get"} */
} sm_proto_cmd_t;

/* 解析出的一条命令 */
typedef struct
{
  sm_proto_cmd_t cmd;
  char           dev[SM_DEV_ID_LEN];  /* SET：目标设备 id */
  bool           has_prop;            /* SET 恒为 true（解析已校验） */
  sm_prop_t      prop;
  int            value;
} sm_proto_msg_t;

/* 行解析：SM_OK / SM_ERR_INVALID（空行/畸形 JSON/未知命令）。
 * 鲁棒性：未知字段忽略；允许前后空白与结尾 '\r'；字符串值做最小反转义；
 * 任何畸形输入都返回失败，绝不越界读。 */
int sm_proto_parse(const char *line, sm_proto_msg_t *msg);

/* 属性字符串 ↔ 枚举（"on" | "brightness" | "temp"，与 smarthome_storage.h
 * 冻结 schema 的枚举映射一致） */
int         sm_proto_prop_from_str(const char *s, sm_prop_t *prop);
const char *sm_proto_prop_to_str(sm_prop_t prop);

/* 设备类型枚举 → schema 字符串（"light" | "socket" | "sensor"） */
const char *sm_proto_type_to_str(sm_dev_type_t type);

/****************************************************************************
 * 构造器：各自写入独立静态缓冲，返回缓冲指针（行消息自带结尾 '\n'）。
 * 注意：同一构造器下次调用会覆盖上次内容——构造后必须立即发送。
 * 返回 NULL 表示缓冲装不下（调用方按"消息丢弃"处理，不影响主流程）。
 ****************************************************************************/

/* {"evt":"ack","ok":1} / {"evt":"ack","ok":0,"err":"invalid"}（err 可 NULL） */
const char *sm_proto_build_ack(bool ok, const char *err);

/* {"evt":"state","dev":"light1","prop":"on","value":1} */
const char *sm_proto_build_state(const char *dev, sm_prop_t prop, int value);

/* {"evt":"rule","id":"r1","name":"..","dev":"light1","prop":"on","value":1} */
const char *sm_proto_build_rule(const char *rule_id, const char *rule_name,
                                const char *dev, sm_prop_t prop, int value);

/* {"evt":"devices","devices":[{"id":"light1","type":"light","on":1,
 *   "brightness":60},...]}，字段按 smarthome_storage.h devices.json schema */
const char *sm_proto_build_devices(const sm_device_t *devs, int count);

#ifdef __cplusplus
}
#endif

#endif /* __SM_NET_INTERNAL_H */
