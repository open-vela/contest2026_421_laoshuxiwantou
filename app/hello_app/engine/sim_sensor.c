/****************************************************************************
 * engine/sim_sensor.c — 假温度传感器（演示数据源）
 *
 * 队伍 421 / laoshuxiwantou，2026-09-19
 *
 * 由主循环 ~100ms 调一次 sm_sim_sensor_tick()；内部 5 秒节拍驱动
 * sensor1.temp 在 240~340（0.1°C，三角波）往复，经 sm_set_device 写入
 * 以触发规则联动。无内部线程、无阻塞。
 *
 * host 单测（SM_HOST_TEST）用虚拟时钟替代 clock()，可瞬时推进验证
 * 越限触发，见 sm_engine_internal.h 的 sm_sim_test_advance_ms()。
 ****************************************************************************/

#include <stdbool.h>
#include <time.h>

#include "smarthome_types.h"
#include "smarthome_internal.h"
#include "sm_engine_internal.h"

/* 节拍与三角波参数（0.1°C 单位） */
#define SM_SIM_PERIOD_MS  5000L   /* 5 秒一拍 */
#define SM_SIM_STEP       5       /* 每拍步进 5（0.5°C） */
#define SM_SIM_MIN        240     /* 24.0°C */
#define SM_SIM_MAX        340     /* 34.0°C */
#define SM_SIM_DEV_ID     "sensor1"

/* 内部状态（单任务访问，无锁） */
static bool g_enabled = true;     /* 默认使能：上电即开始演示数据 */
static int  g_value = SM_SIM_MIN; /* 当前温度（0.1°C） */
static int  g_dir = SM_SIM_STEP;  /* +step 上升 / -step 下降 */
static long g_last_ms = -1;       /* 上次步进时刻，-1 表示尚未起拍 */

/* 当前时刻（毫秒）。SM_HOST_TEST 下用测试可控虚拟时钟。 */
#if defined(SM_HOST_TEST)
static long g_test_ms = 0;

static long sim_now_ms(void)
{
  return g_test_ms;
}

void sm_sim_test_advance_ms(long ms)
{
  g_test_ms += ms;
}
#else
static long sim_now_ms(void)
{
  clock_t t = clock();

  /* 分离秒/余数避免 32 位平台 clock()*1000 溢出 */
  return (long)(t / CLOCKS_PER_SEC) * 1000L +
         (long)(t % CLOCKS_PER_SEC) * (1000L / CLOCKS_PER_SEC);
}
#endif

void sm_sim_sensor_tick(void)
{
  long now;

  if (!g_enabled)
    {
      return;                       /* 禁用后 tick 直接返回 */
    }

  now = sim_now_ms();
  if (g_last_ms < 0)
    {
      g_last_ms = now;              /* 首拍仅起表，不推数据 */
      return;
    }

  if (now - g_last_ms < SM_SIM_PERIOD_MS)
    {
      return;                       /* 未到 5 秒节拍 */
    }

  /* 固定节拍：即使超时很久，一拍也只推进一步，保证演示轨迹可预期 */
  g_last_ms = now;

  if (g_dir > 0)
    {
      g_value += SM_SIM_STEP;
      if (g_value >= SM_SIM_MAX)
        {
          g_value = SM_SIM_MAX;     /* 到顶折返 */
          g_dir = -SM_SIM_STEP;
        }
    }
  else
    {
      g_value -= SM_SIM_STEP;
      if (g_value <= SM_SIM_MIN)
        {
          g_value = SM_SIM_MIN;     /* 到底折返 */
          g_dir = SM_SIM_STEP;
        }
    }

  /* 经唯一状态入口写入，借以触发规则求值（失败静默忽略） */
  (void)sm_set_device(SM_SIM_DEV_ID, SM_PROP_TEMP, g_value);
}

void sm_sim_sensor_set_enabled(bool en)
{
  g_enabled = en;
}

bool sm_sim_sensor_enabled(void)
{
  return g_enabled;
}
