/****************************************************************************
 * app/smarthome_types.h — 契约文件，子任务只读
 *
 * 智能家居中控面板（D12X）物模型与规则引擎 API
 * 队伍 421 / laoshuxiwantou，2026-09-18 冻结
 *
 * 修改本文件必须由主集成 Agent 执行；子任务发现契约不足时向集成者报告，
 * 不得自行修改。所有引擎/存储/UI/模拟代码只依赖本文件与 smarthome_storage.h。
 *
 * 线程模型（重要）：整个应用单任务运行（LVGL 主循环任务内 tick 驱动），
 * 引擎/存储/模拟设备都没有内部线程与锁；UI 事件回调、网络 tick、
 * 假传感器 tick 全部在主循环里顺序调用。
 ****************************************************************************/

#ifndef __SMARTHOME_TYPES_H
#define __SMARTHOME_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * 容量上限
 ****************************************************************************/

#define SM_DEV_ID_LEN     16    /* 设备 id 字符串长度（含 '\0'） */
#define SM_RULE_ID_LEN    16
#define SM_NAME_LEN       24    /* 规则名长度（含 '\0'） */
#define SM_MAX_DEVICES    8
#define SM_MAX_RULES      16

/****************************************************************************
 * 返回码
 ****************************************************************************/

#define SM_OK              0
#define SM_ERR_INVALID   (-1)   /* 参数非法（未知设备/属性/操作数） */
#define SM_ERR_NOTFOUND  (-2)   /* 文件/设备不存在 */
#define SM_ERR_IO        (-3)   /* 存储读写失败或 JSON 损坏 */
#define SM_ERR_NOMEM     (-4)   /* 容量不足（设备表/规则表/缓冲区满） */
#define SM_ERR_UNSUPPORTED (-5) /* 功能保留未实现（如时间条件） */

/****************************************************************************
 * 设备物模型
 *
 * 默认出厂设备表（storage 默认集与 UI/模拟器、docs 示例 JSON 必须一致）：
 *   light1  LIGHT  客厅灯      on + brightness
 *   light2  LIGHT  卧室灯      on + brightness
 *   socket1 SOCKET 智能插座    on
 *   sensor1 SENSOR 温度传感器  temp（0.1°C 为单位，305 = 30.5°C）
 ****************************************************************************/

typedef enum
{
  SM_DEV_LIGHT = 0,     /* 灯：on / brightness */
  SM_DEV_SOCKET,        /* 插座：on */
  SM_DEV_SENSOR,        /* 传感器：temp */
} sm_dev_type_t;

typedef enum
{
  SM_PROP_ON = 0,       /* 0/1，LIGHT 与 SOCKET */
  SM_PROP_BRIGHTNESS,   /* 0..100，仅 LIGHT */
  SM_PROP_TEMP,         /* 0.1°C，仅 SENSOR */
} sm_prop_t;

typedef struct
{
  char          id[SM_DEV_ID_LEN];
  sm_dev_type_t type;
  bool          on;
  int           brightness;   /* 0-100，仅 LIGHT 有效 */
  int           temp;         /* 0.1°C，仅 SENSOR 有效 */
} sm_device_t;

/****************************************************************************
 * 规则：条件 → 动作（v1 一条规则单条件单动作，UI 用下拉框编辑）
 *
 * 条件类型：
 *   SM_COND_ATTR  设备属性比较：dev/prop 与 value 按 op 比较
 *   SM_COND_TIME  每天 hh:mm 触发（保留，v1 引擎可返回 SM_ERR_UNSUPPORTED）
 *
 * 命中语义：边沿触发。规则条件由假变真时执行一次动作并回调事件；
 * 条件变假后重新武装。动作引起的状态变化会继续参与求值（链式联动），
 * 引擎内部迭代至稳定，最多 4 轮，防环。
 ****************************************************************************/

typedef enum
{
  SM_COND_ATTR = 0,
  SM_COND_TIME,         /* v1 保留：引擎不实现，UI 不展示 */
} sm_cond_type_t;

typedef enum
{
  SM_OP_GE = 0,         /* >= */
  SM_OP_LE,             /* <= */
  SM_OP_EQ,             /* == */
} sm_op_t;

typedef struct
{
  sm_cond_type_t type;
  char           dev[SM_DEV_ID_LEN];  /* SM_COND_ATTR：被观察设备 id */
  sm_prop_t      prop;
  sm_op_t        op;
  int            value;               /* 阈值（temp 用 0.1°C；on 用 0/1） */
  int            hour;                /* SM_COND_TIME 保留字段 */
  int            minute;
} sm_condition_t;

typedef enum
{
  SM_ACT_SET = 0,       /* 设置目标设备属性 */
  SM_ACT_BEEP,          /* 播放提示音（/data/beep.wav，缺失则静默跳过） */
} sm_act_type_t;

typedef struct
{
  sm_act_type_t type;
  char          dev[SM_DEV_ID_LEN];   /* SM_ACT_SET：目标设备 id */
  sm_prop_t     prop;
  int           value;
} sm_action_t;

typedef struct
{
  char           id[SM_RULE_ID_LEN];
  char           name[SM_NAME_LEN];
  bool           enabled;
  sm_condition_t cond;
  sm_action_t    action;
} sm_rule_t;

/****************************************************************************
 * 规则命中事件回调（引擎 → UI/主程序）
 *
 * 引擎在规则命中并执行动作后调用 cb(rule, dev, value, arg)：
 *   rule  命中的规则（引擎内部存储，回调内只读，不得保存指针）
 *   dev   动作作用后的目标设备快照指针（内部存储，回调内只读）
 *   value 动作写入的属性值
 * 回调运行在主任务上下文，可直接调 LVGL API。
 ****************************************************************************/

typedef void (*sm_event_cb_t)(const sm_rule_t *rule,
                              const sm_device_t *dev,
                              int value, void *arg);

/****************************************************************************
 * 引擎 API（engine/ 轨实现，UI/sim 只调用）
 ****************************************************************************/

/* 初始化：装载默认设备表；加载 /data/rules.json（损坏/缺失回退默认规则集，
 * 但不会自动回写 SD，避免坏卡反复擦写）。返回 SM_OK 或负错误码。 */
int sm_engine_init(void);

/* 主循环周期调用（建议 100ms）。v1 内部仅做周期性事务（预留），
 * 规则求值由 sm_set_device / sm_sim_sensor_tick 触发。 */
void sm_engine_tick(void);

/* 注册规则命中事件回调（可在 init 前调用；传 NULL 注销）。 */
int sm_engine_set_event_cb(sm_event_cb_t cb, void *arg);

/* 唯一的状态变更入口：UI 开关/滑条、网络指令、假传感器都必须走这里。
 * 更新内存状态 → 触发规则求值（含链式联动）→ 返回 SM_OK。
 * 未知设备/属性与设备类型不匹配（如给 SENSOR 设 on）返回 SM_ERR_INVALID。 */
int sm_set_device(const char *id, sm_prop_t prop, int value);

/* 设备表访问：sm_get_device 按 id 查找（不存在返回 NULL，返回内部存储只读
 * 语义，UI 刷新时直接读字段）；sm_device_table 返回表基址并经 count 输出
 * 数量。指针在运行期稳定，可直接长期持有。 */
sm_device_t *sm_get_device(const char *id);
sm_device_t *sm_device_table(int *count);

/* 规则集访问与编辑（UI 规则页用）：
 * sm_rules_copy    把当前规则集拷贝到 out（容量 max），经 count 输出数量
 * sm_rules_count   当前规则数
 * sm_rules_replace 用传入规则集整体替换内存规则并立即热生效（重新求值时
 *                  以新规则为准）；persist=true 同时写 SD 卡 /data/rules.json
 * sm_rules_restore_default 恢复默认规则集到内存（不落盘，配合 replace 保存） */
int sm_rules_copy(sm_rule_t *out, int max, int *count);
int sm_rules_count(void);
int sm_rules_replace(const sm_rule_t *rules, int count, bool persist);
int sm_rules_restore_default(void);

#ifdef __cplusplus
}
#endif

#endif /* __SMARTHOME_TYPES_H */
