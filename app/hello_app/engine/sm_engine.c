/****************************************************************************
 * engine/sm_engine.c — 智能家居联动规则引擎
 *
 * 队伍 421 / laoshuxiwantou，2026-09-19
 *
 * 实现 smarthome_types.h 冻结契约的全部引擎 API：
 *   - 静态设备表 + 静态规则表，无 malloc；指针运行期稳定
 *   - init：默认设备表打底 → devices.json 按 id 合并（可选文件）→
 *     rules.json 优先，缺失/损坏/超限回退默认规则集（不回写）
 *   - sm_set_device：校验设备/属性/类型匹配 → 更新状态 → 规则求值
 *   - 求值：边沿触发（条件 false→true 执行动作并回调），动作引起的
 *     状态变化继续参与求值（链式联动），迭代至稳定，上限 4 轮防环
 *   - 持久化：on/brightness 变更（含 UI/网络/规则联动路径）经
 *     sm_engine_tick 防抖 500ms 落盘 devices.json（temp 除外）；
 *     rules 落盘仅 UI Save（persist=true）与恢复出厂
 *
 * 线程模型：整个应用单任务运行（无锁），UI/网络/假传感器都在主循环
 * 顺序调用本模块；g_inside_evaluate 防止动作/回调路径重入求值。
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "smarthome_types.h"
#include "smarthome_storage.h"
#include "smarthome_internal.h"
#include "sm_engine_internal.h"

/* 链式联动迭代上限（契约），防规则互激死循环 */
#define SM_MAX_EVAL_ROUNDS  4

/****************************************************************************
 * 内部状态（静态分配，指针运行期稳定）
 ****************************************************************************/

static sm_device_t   g_devices[SM_MAX_DEVICES];   /* 设备表 */
static int           g_dev_count;

static sm_rule_t     g_rules[SM_MAX_RULES];       /* 当前生效规则集 */
static int           g_rule_count;

static bool          g_armed[SM_MAX_RULES];       /* 边沿检测：条件上次真值 */

/* 规则命中回调槽位：槽 0 契约接口（UI 触发记录），槽 1 内部接口（sim 轨
 * evt:rule 转发）。主任务单线程模型，无需加锁。 */
#define SM_EVENT_CB_SLOTS 2
static sm_event_cb_t g_event_cbs[SM_EVENT_CB_SLOTS];
static void         *g_event_args[SM_EVENT_CB_SLOTS];

static bool          g_inside_evaluate;           /* 求值重入保护 */
static bool          g_inited;                    /* 是否已完成初始化 */

/* 设备状态防抖落盘：任意 on/brightness 变更（UI/网络/规则联动同源）后
 * 统一延迟落盘 devices.json；temp 不落盘（假传感器 5s 步进，恢复无意义
 * 且会造成无意义擦写）。主循环经 sm_engine_tick 到期统一写。 */
#define SM_DEV_SAVE_DELAY_MS  500
static bool          s_dev_dirty;                 /* 有待落盘的状态变更 */
static long          s_dirty_ms;                  /* 最后变更时刻（ms） */

/****************************************************************************
 * 设备/属性基础工具
 ****************************************************************************/

static sm_device_t *dev_find(const char *id)
{
  int i;

  if (id == NULL)
    {
      return NULL;
    }

  for (i = 0; i < g_dev_count; i++)
    {
      if (strcmp(g_devices[i].id, id) == 0)
        {
          return &g_devices[i];
        }
    }

  return NULL;
}

/* 属性是否属于该设备类型（SENSOR 只收 temp，SOCKET 只收 on 等） */
static bool prop_of_type(sm_dev_type_t type, sm_prop_t prop)
{
  switch (type)
    {
    case SM_DEV_LIGHT:
      return prop == SM_PROP_ON || prop == SM_PROP_BRIGHTNESS;
    case SM_DEV_SOCKET:
      return prop == SM_PROP_ON;
    case SM_DEV_SENSOR:
      return prop == SM_PROP_TEMP;
    default:
      return false;
    }
}

/* 读设备当前属性值（调用方保证 type/prop 已匹配） */
static int dev_read_prop(const sm_device_t *dev, sm_prop_t prop, int *value)
{
  switch (prop)
    {
    case SM_PROP_ON:
      *value = dev->on ? 1 : 0;
      return SM_OK;
    case SM_PROP_BRIGHTNESS:
      *value = dev->brightness;
      return SM_OK;
    case SM_PROP_TEMP:
      *value = dev->temp;
      return SM_OK;
    default:
      return SM_ERR_INVALID;
    }
}

/****************************************************************************
 * 求值与动作执行
 ****************************************************************************/

/* 条件真值判断；TIME 条件 v1 保留恒假，条件引用无效设备/属性视为假 */
static bool cond_eval(const sm_condition_t *cond)
{
  sm_device_t *dev;
  int value;

  if (cond->type != SM_COND_ATTR)
    {
      return false;
    }

  dev = dev_find(cond->dev);
  if (dev == NULL || !prop_of_type(dev->type, cond->prop))
    {
      return false;
    }

  if (dev_read_prop(dev, cond->prop, &value) != SM_OK)
    {
      return false;
    }

  switch (cond->op)
    {
    case SM_OP_GE:
      return value >= cond->value;
    case SM_OP_LE:
      return value <= cond->value;
    case SM_OP_EQ:
      return value == cond->value;
    default:
      return false;
    }
}

/* 当前毫秒时钟（与 sim_sensor 同款溢出安全写法） */
static long now_ms(void)
{
  clock_t t = clock();

  /* 分离秒/余数避免 32 位平台 clock()*1000 溢出 */
  return (long)(t / CLOCKS_PER_SEC) * 1000L +
         (long)(t % CLOCKS_PER_SEC) * (1000L / CLOCKS_PER_SEC);
}

/* 唯一的状态更新路径：校验 + 写状态（不触发求值，避免重入） */
static int apply_set(const char *id, sm_prop_t prop, int value,
                     sm_device_t **out_dev)
{
  sm_device_t *dev = dev_find(id);

  if (dev == NULL || !prop_of_type(dev->type, prop))
    {
      return SM_ERR_INVALID;
    }

  switch (prop)
    {
    case SM_PROP_ON:
      dev->on = (value != 0);
      break;
    case SM_PROP_BRIGHTNESS:
      dev->brightness = value;
      break;
    case SM_PROP_TEMP:
      dev->temp = value;
      break;
    default:
      return SM_ERR_INVALID;
    }

  if (prop != SM_PROP_TEMP)
    {
      s_dev_dirty = true;
      s_dirty_ms = now_ms();
    }

  if (out_dev != NULL)
    {
      *out_dev = dev;
    }

  return SM_OK;
}

/* 回调兜底设备指针：beep 动作 / 非法目标没有直接目标设备，
 * 用条件观察设备（报警来源）代替；再兜底设备表首项，保证 UI 拿到
 * 非空指针（契约要求回调收到设备快照指针）。 */
static sm_device_t *event_dev_fallback(const sm_rule_t *rule,
                                       const char *preferred_id)
{
  sm_device_t *dev = dev_find(preferred_id);

  if (dev == NULL)
    {
      dev = dev_find(rule->cond.dev);
    }

  if (dev == NULL && g_dev_count > 0)
    {
      dev = &g_devices[0];
    }

  return dev;
}

/* 执行单条命中规则的动作：内部更新状态 + printf 日志 + 事件回调。
 * 注意：SET 走 apply_set 直写状态，不调 sm_set_device，避免重入求值。 */
static void action_exec(const sm_rule_t *rule)
{
  sm_device_t *dev = NULL;
  const char *log_dev;
  const char *log_prop;
  int value = rule->action.value;

  if (rule->action.type == SM_ACT_SET)
    {
      if (apply_set(rule->action.dev, rule->action.prop, value, &dev)
          != SM_OK)
        {
          /* 目标设备/属性非法：跳过动作但规则仍算命中（边沿已消费） */
          printf("[SM] rule %s action skipped: bad target %s\n",
                 rule->id, rule->action.dev);
        }
      log_dev = (dev != NULL) ? dev->id : rule->action.dev;
      log_prop = sm_prop_name(rule->action.prop);
    }
  else
    {
      /* SM_ACT_BEEP：/data/beep.wav 缺失或音频未使能时静默跳过 */
      (void)sm_beep_play();
      dev = event_dev_fallback(rule, NULL);
      log_dev = (dev != NULL) ? dev->id : rule->cond.dev;
      log_prop = sm_prop_name(rule->cond.prop);
    }

  /* 契约规定的命中日志格式 */
  printf("[SM] rule hit: %s -> %s.%s=%d\n",
         rule->id, log_dev, log_prop, value);

  {
    int slot;

    for (slot = 0; slot < SM_EVENT_CB_SLOTS; slot++)
      {
        if (g_event_cbs[slot] != NULL)
          {
            g_event_cbs[slot](rule, dev, value, g_event_args[slot]);
          }
      }
  }
}

/* 对全部 enabled 规则做一轮边沿检测求值；动作引起的状态变化在后续
 * 轮次继续生效（链式联动），最多 SM_MAX_EVAL_ROUNDS 轮防环。 */
static void evaluate(void)
{
  int round;
  int i;

  if (g_inside_evaluate)
    {
      return;                           /* 双保险，正常路径不会发生 */
    }

  g_inside_evaluate = true;

  for (round = 0; round < SM_MAX_EVAL_ROUNDS; round++)
    {
      bool fired = false;

      for (i = 0; i < g_rule_count; i++)
        {
          sm_rule_t *rule = &g_rules[i];
          bool now;

          if (!rule->enabled)
            {
              g_armed[i] = false;       /* 禁用规则不参与，退网状态 */
              continue;
            }

          now = cond_eval(&rule->cond);
          if (now && !g_armed[i])
            {
              g_armed[i] = true;        /* false→true 边沿：触发一次 */
              action_exec(rule);
              fired = true;
            }
          else if (!now)
            {
              g_armed[i] = false;       /* 条件变假：重新武装 */
            }
        }

      if (!fired)
        {
          break;                        /* 本轮无新命中：已稳定 */
        }
    }

  g_inside_evaluate = false;
}

/* 以当前设备状态初始化全部规则的边沿状态（init / 替换规则集后调用；
 * 启动时已为真的条件只武装不触发，避免上电误动作） */
static void rules_rearm(void)
{
  int i;

  for (i = 0; i < g_rule_count; i++)
    {
      g_armed[i] = g_rules[i].enabled ? cond_eval(&g_rules[i].cond) : false;
    }

  for (; i < SM_MAX_RULES; i++)
    {
      g_armed[i] = false;
    }
}

/****************************************************************************
 * 初始化
 ****************************************************************************/

static int ensure_init(void)
{
  if (!g_inited)
    {
      return sm_engine_init();
    }

  return SM_OK;
}

int sm_engine_init(void)
{
  sm_device_t devs[SM_MAX_DEVICES];
  sm_rule_t rules[SM_MAX_RULES];
  int count = 0;
  int ret;
  int i;

  /* 1. 设备表：出厂默认打底（静态默认集不可能失败，防御性返回） */
  count = 0;
  ret = sm_storage_default_devices(devs, SM_MAX_DEVICES, &count);
  if (ret != SM_OK || count <= 0)
    {
      return (ret != SM_OK) ? ret : SM_ERR_IO;
    }

  memcpy(g_devices, devs, (size_t)count * sizeof(sm_device_t));
  g_dev_count = count;

  /* 2. devices.json 可选：按 id 合并覆盖状态；缺失静默，损坏保持默认 */
  ret = sm_storage_load_devices(devs, SM_MAX_DEVICES, &count);
  if (ret == SM_OK)
    {
      for (i = 0; i < count; i++)
        {
          sm_device_t *target = dev_find(devs[i].id);

          if (target != NULL)
            {
              *target = devs[i];
            }
          /* 未知 id 忽略：设备表型号由出厂定义，文件只恢复状态 */
        }

      if (count > 0)
        {
          printf("[SM] device state restored %d from /data/devices.json\n",
                 count);
        }
    }
  else if (ret != SM_ERR_NOTFOUND)
    {
      printf("[SM] devices.json load failed (%d), keep factory defaults\n",
             ret);
    }

  /* 3. 规则表：rules.json 优先；NOTFOUND/IO/NOMEM/UNSUPPORTED 一律
   * 回退默认规则集且不回写 SD，避免坏卡反复擦写 */
  ret = sm_storage_load_rules(rules, SM_MAX_RULES, &count);
  if (ret != SM_OK)
    {
      printf("[SM] rules.json load failed (%d), fallback to default rules\n",
             ret);

      count = 0;
      ret = sm_storage_default_rules(rules, SM_MAX_RULES, &count);
      if (ret != SM_OK)
        {
          return ret;
        }
    }
  else
    {
      printf("[SM] rules loaded %d from /data/rules.json\n", count);
    }

  memcpy(g_rules, rules, (size_t)count * sizeof(sm_rule_t));
  g_rule_count = count;

  /* 事件回调保持（允许 init 前注册）；重置求值状态并武装边沿 */
  g_inside_evaluate = false;
  g_inited = true;
  rules_rearm();

  return SM_OK;
}

void sm_engine_tick(void)
{
  /* 设备状态防抖落盘：距最后一次 on/brightness 变更 500ms 后统一写一次
   * devices.json（滑条拖动/场景批量设置/联动链收敛后只落一次盘）。
   * 失败只打印不重试，不阻塞主循环，下次变更会再次触发。 */
  if (g_inited && s_dev_dirty &&
      (now_ms() - s_dirty_ms) >= SM_DEV_SAVE_DELAY_MS)
    {
      s_dev_dirty = false;

      if (sm_storage_save_devices(g_devices, g_dev_count) != SM_OK)
        {
          printf("[SM] devices.json save failed\n");
        }
      else
        {
          printf("[SM] device state saved\n");
        }
    }
}

int sm_engine_set_event_cb(sm_event_cb_t cb, void *arg)
{
  g_event_cbs[0] = cb;
  g_event_args[0] = arg;
  return SM_OK;
}

int sm_engine_add_event_cb(sm_event_cb_t cb, void *arg)
{
  if (g_event_cbs[1] != NULL)
    {
      return SM_ERR_NOMEM;
    }

  g_event_cbs[1] = cb;
  g_event_args[1] = arg;
  return SM_OK;
}

/****************************************************************************
 * 状态变更入口
 ****************************************************************************/

int sm_set_device(const char *id, sm_prop_t prop, int value)
{
  sm_device_t *dev;
  int ret;

  if (id == NULL)
    {
      return SM_ERR_INVALID;
    }

  if (ensure_init() != SM_OK)
    {
      return SM_ERR_INVALID;
    }

  /* 校验 + 更新内存状态（未知设备 / 属性与类型不匹配 → INVALID） */
  ret = apply_set(id, prop, value, &dev);
  if (ret != SM_OK)
    {
      return ret;
    }

  /* 求值期间（动作/回调路径）不再嵌套求值，外层循环会继续收敛 */
  if (!g_inside_evaluate)
    {
      evaluate();
    }

  return SM_OK;
}

/****************************************************************************
 * 设备表 / 规则集访问
 ****************************************************************************/

sm_device_t *sm_get_device(const char *id)
{
  if (ensure_init() != SM_OK)
    {
      return NULL;
    }

  return dev_find(id);
}

sm_device_t *sm_device_table(int *count)
{
  if (ensure_init() != SM_OK)
    {
      if (count != NULL)
        {
          *count = 0;
        }
      return NULL;
    }

  if (count != NULL)
    {
      *count = g_dev_count;
    }

  return g_devices;
}

int sm_rules_copy(sm_rule_t *out, int max, int *count)
{
  if (out == NULL || count == NULL || max < 0)
    {
      return SM_ERR_INVALID;
    }

  if (ensure_init() != SM_OK)
    {
      return SM_ERR_IO;
    }

  if (g_rule_count > max)
    {
      return SM_ERR_NOMEM;
    }

  memcpy(out, g_rules, (size_t)g_rule_count * sizeof(sm_rule_t));
  *count = g_rule_count;
  return SM_OK;
}

int sm_rules_count(void)
{
  if (ensure_init() != SM_OK)
    {
      return 0;
    }

  return g_rule_count;
}

int sm_rules_replace(const sm_rule_t *rules, int count, bool persist)
{
  int i;
  int ret;

  if (rules == NULL || count < 0)
    {
      return SM_ERR_INVALID;
    }

  if (count > SM_MAX_RULES)
    {
      return SM_ERR_NOMEM;
    }

  if (ensure_init() != SM_OK)
    {
      return SM_ERR_IO;
    }

  for (i = 0; i < count; i++)
    {
      if (rules[i].cond.type == SM_COND_TIME)
        {
          /* v1 不支持时间条件：整批拒绝（UI 不展示该类型） */
          return SM_ERR_UNSUPPORTED;
        }

      if (rules[i].id[0] == '\0')
        {
          return SM_ERR_INVALID;
        }
    }

  memcpy(g_rules, rules, (size_t)count * sizeof(sm_rule_t));
  g_rule_count = count;

  /* 新规则集按当前状态武装（已为真的条件不立即触发），随后求值即以
   * 新规则为准（热生效） */
  rules_rearm();

  if (persist)
    {
      ret = sm_storage_save_rules(g_rules, g_rule_count);
      if (ret != SM_OK)
        {
          /* 内存规则已生效，落盘失败如实上报供 UI 提示 */
          printf("[SM] rules save failed (%d), memory rules active\n", ret);
          return ret;
        }
    }

  return SM_OK;
}

int sm_rules_restore_default(void)
{
  int count = 0;
  int ret;

  if (ensure_init() != SM_OK)
    {
      return SM_ERR_IO;
    }

  ret = sm_storage_default_rules(g_rules, SM_MAX_RULES, &count);
  if (ret != SM_OK)
    {
      return ret;
    }

  g_rule_count = count;
  rules_rearm();

  /* 恢复出厂同步落盘覆盖旧规则文件（Reset 后断电重启仍为出厂集）；
   * 失败（如 /data 未挂载）不影响内存已生效，仅打印 */
  if (sm_storage_save_rules(g_rules, g_rule_count) != SM_OK)
    {
      printf("[SM] factory rules save failed, memory-only\n");
    }

  return SM_OK;
}
