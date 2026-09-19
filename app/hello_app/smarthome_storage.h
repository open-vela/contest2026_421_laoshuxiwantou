/****************************************************************************
 * app/smarthome_storage.h — 契约文件，子任务只读
 *
 * SD 卡/数据分区 JSON 持久化接口 + 固定 schema 的最小 JSON 解析器
 * 队伍 421 / laoshuxiwantou，2026-09-18 冻结
 *
 * 修改本文件必须由主集成 Agent 执行。实现放 engine/ 轨（sm_storage.c、
 * sm_json.c），仅依赖标准 C 文件 I/O（fopen/fread/fwrite），host 单测与
 * 板端共用同一实现。不依赖 cJSON（apps/netutils 配置耦合，规避）。
 *
 * 存储介质：/data 必须为可写文件系统（fatfs SD 卡或 littlefs 数据分区）。
 * 打开失败一律返回 SM_ERR_NOTFOUND，由调用方回退默认规则集——保证拔卡、
 * 挂载失败、首次上电都不会卡住启动。
 ****************************************************************************/

#ifndef __SMARTHOME_STORAGE_H
#define __SMARTHOME_STORAGE_H

#include "smarthome_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * 存储路径
 ****************************************************************************/

#define SM_RULES_PATH     "/data/rules.json"
#define SM_DEVICES_PATH   "/data/devices.json"
#define SM_BEEP_PATH      "/data/beep.wav"

/* 序列化缓冲区大小（16 条规则 JSON 上限，save 用静态缓冲实现） */
#define SM_JSON_BUF_SIZE  4096

/****************************************************************************
 * JSON schema（冻结。docs/ 示例集与模拟器脚本必须与此一致）
 *
 * /data/rules.json：
 * {
 *   "version": 1,
 *   "rules": [
 *     {
 *       "id": "r1", "name": "high temp light on", "enabled": true,
 *       "cond":  {"type": "attr", "dev": "sensor1",
 *                 "prop": "temp", "op": ">=", "value": 300},
 *       "action": {"type": "set", "dev": "light1",
 *                  "prop": "on", "value": 1}
 *     },
 *     {
 *       "id": "r4", "name": "high temp beep", "enabled": true,
 *       "cond":  {"type": "attr", "dev": "sensor1",
 *                 "prop": "temp", "op": ">=", "value": 320},
 *       "action": {"type": "beep"}
 *     }
 *   ]
 * }
 *
 * /data/devices.json（设备状态持久化，可选文件，缺失用出厂默认）：
 * {
 *   "version": 1,
 *   "devices": [
 *     {"id": "light1",  "type": "light",  "on": 1, "brightness": 60},
 *     {"id": "socket1", "type": "socket", "on": 0},
 *     {"id": "sensor1", "type": "sensor", "temp": 260}
 *   ]
 * }
 *
 * 枚举字符串映射（双向）：
 *   设备类型  light | socket | sensor
 *   属性      on | brightness | temp
 *   条件类型  attr（time 为保留字，解析到即报 SM_ERR_UNSUPPORTED）
 *   比较符    >= | <= | ==
 *   动作类型  set | beep
 * 未知字段忽略；解析中途类型不符 → SM_ERR_IO（调用方回退默认集）。
 ****************************************************************************/

/****************************************************************************
 * 持久化 API（engine/ 轨实现）
 *
 * load_*  返回 SM_OK / SM_ERR_NOTFOUND（文件不存在）/ SM_ERR_IO（JSON
 *         损坏、字段非法）/ SM_ERR_NOMEM（超过 max）。
 * save_*  原子写：先写 .tmp 再 rename，避免断电写半截；返回 SM_OK 或
 *         SM_ERR_IO。目录 /data 不存在时返回 SM_ERR_IO（挂载由系统负责）。
 * default_*  出厂默认集（设备表与 smarthome_types.h 注释一致；默认规则
 *         4 条：高温开灯 / 低温关灯 / 高亮度开插座 / 超温报警鸣音）。
 ****************************************************************************/

int sm_storage_load_rules(sm_rule_t *out, int max, int *count);
int sm_storage_save_rules(const sm_rule_t *rules, int count);
int sm_storage_load_devices(sm_device_t *out, int max, int *count);
int sm_storage_save_devices(const sm_device_t *devs, int count);
int sm_storage_default_rules(sm_rule_t *out, int max, int *count);
int sm_storage_default_devices(sm_device_t *out, int max, int *count);

/****************************************************************************
 * 最小 JSON 解析器 API（engine/sm_json.c 实现，sim/ 网络协议同样复用）
 *
 * 只服务上述固定 schema 与模拟器消息（见 sim 轨协议），不做通用 JSON。
 * 解析失败返回负错误码；to_str 返回写入字节数（不含 '\0'），缓冲不足
 * 返回 SM_ERR_NOMEM。
 ****************************************************************************/

int sm_json_parse_rules(const char *text, sm_rule_t *out, int max, int *count);
int sm_json_rules_to_str(const sm_rule_t *rules, int count,
                         char *buf, int buflen);
int sm_json_parse_devices(const char *text, sm_device_t *out, int max,
                          int *count);
int sm_json_devices_to_str(const sm_device_t *devs, int count,
                           char *buf, int buflen);

#ifdef __cplusplus
}
#endif

#endif /* __SMARTHOME_STORAGE_H */
