/****************************************************************************
 * app/hello_app/smarthome_main.c — 智能家居中控面板主入口（主集成 Agent 维护）
 *
 * 队伍 421 / laoshuxiwantou。单任务主循环，tick 顺序驱动各层：
 *   LVGL 事件处理 → 引擎 → 假传感器 → UI 刷新 → 网络收发
 * 显示/触摸初始化沿用 apps/examples/lvgldemo 的 NuttX 接入模式。
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/boardctl.h>

#include <lvgl/lvgl.h>
#include <lvgl/src/drivers/nuttx/lv_nuttx_touchscreen.h>

#include "smarthome_types.h"
#include "smarthome_internal.h"
#include "ui/sm_ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 作为 NSH 内置应用启动时，板级初始化通常已由 NSH 完成；
 * 否则仿 lvgldemo 在此补做 boardctl(BOARDIOC_INIT)。 */
#undef NEED_BOARDINIT
#if defined(CONFIG_BOARDCTL) && !defined(CONFIG_NSH_ARCHINIT)
#  define NEED_BOARDINIT 1
#endif

/* 主循环单次休眠上限（ms）：LVGL 空闲时间再长也不能拖慢
 * 假传感器/网络 tick 的响应节奏 */
#define SM_LOOP_SLEEP_MAX_MS 20

/* 心跳周期（主循环拍数）：~500 拍 ≈ 5-10s，用于确认任务存活 */
#define SM_LOOP_BEAT_PERIOD 500

/* 触摸设备路径（D12X demo68-nor GT911） */
#define SM_TOUCH_DEVPATH "/dev/input0"

static uint32_t g_loop_beats;
static uint32_t g_loop_count;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  lv_nuttx_dsc_t info;
  lv_nuttx_result_t result;
  int i;

  /* 第二实例守卫：面板已在运行时，仅允许 --phone 以独立模式再起一个
   * "手机"回环客户端（纯 socket，不碰 LVGL/引擎），播完一轮即退出。 */
  if (lv_is_initialized())
    {
      if (argc > 1 && strcmp(argv[1], "--phone") == 0)
        {
          printf("[SM] panel already running, phone demo standalone\n");
          sm_phone_demo_start(9000);

          /* 25s 足够一轮：5s 首连 + 6 步 x 2s */
          for (i = 0; i < 500; i++)
            {
              sm_phone_demo_tick();
              usleep(50 * 1000);
            }
        }
      else
        {
          printf("[SM] smarthome panel already running\n");
        }

      return 0;
    }

  (void)argc;
  (void)argv;

#ifdef NEED_BOARDINIT
  boardctl(BOARDIOC_INIT, 0);
#endif

  printf("[SM] smarthome panel starting (contest 2026 team 421)\n");

  /* 引擎先于 UI 初始化：UI 事件回调与首刷都假设设备表已就绪 */
  if (sm_engine_init() != SM_OK)
    {
      printf("[SM] ERROR: engine init failed\n");
      return 1;
    }

  lv_init();

  lv_nuttx_dsc_init(&info);

#ifdef CONFIG_LV_USE_NUTTX_LCD
  info.fb_path = "/dev/lcd0";
#endif

#ifdef CONFIG_LV_USE_NUTTX_TOUCHSCREEN
  info.input_path = SM_TOUCH_DEVPATH;
#endif

  lv_nuttx_init(&info, &result);
  if (result.disp == NULL)
    {
      printf("[SM] ERROR: display/touch init failed\n");
      return 1;
    }

  /* 触摸 indev 兜底重试：rcS 自启时机可能早于 GT911 驱动注册，
   * lv_nuttx_init 打开 /dev/input0 失败会静默返回 NULL（无输入且无日志），
   * 这里显式重试直到设备就绪。 */
  if (result.indev == NULL)
    {
      for (i = 0; i < 10 && result.indev == NULL; i++)
        {
          printf("[SM] touch indev missing, retry %d/10 in 500ms\n", i + 1);
          usleep(500 * 1000);
          result.indev = lv_nuttx_touchscreen_create(SM_TOUCH_DEVPATH);
        }

      if (result.indev == NULL)
        {
          printf("[SM] ERROR: no touchscreen after retries, UI runs without input\n");
        }
      else
        {
          printf("[SM] touchscreen attached on retry\n");
        }
    }

  sm_ui_init();

  /* 远程呈现通道（no-go 替身，失败不影响本地演示）：
   * 优先 TCP 回环（模拟器/板内回环拓扑），退化 UART（板上串口拓扑） */
  if (sm_net_start_tcp(9000) != SM_OK)
    {
      (void)sm_net_start_uart("/dev/ttyS1", 115200);
    }

  /* `smarthome --phone`：板内"手机"回环演示客户端（真 socket 代码路径） */
  if (argc > 1 && strcmp(argv[1], "--phone") == 0)
    {
      sm_phone_demo_start(9000);
    }

  printf("[SM] entering main loop\n");

  while (1)
    {
      uint32_t idle;

      if (g_loop_count < 3)
        {
          printf("[SM] loop %u: call lv_timer_handler\n", g_loop_count + 1);
        }

      idle = lv_timer_handler();

      if (g_loop_count < 3)
        {
          printf("[SM] loop %u: timer_handler done (idle=%u)\n",
                 g_loop_count + 1, idle);
        }

      sm_engine_tick();
      sm_sim_sensor_tick();
      sm_ui_tick();
      sm_net_tick();

      if (g_loop_count < 3)
        {
          printf("[SM] loop %u: ticks done\n", g_loop_count + 1);
        }

      g_loop_count++;
      if (++g_loop_beats >= SM_LOOP_BEAT_PERIOD)
        {
          sm_device_t *sensor = sm_get_device("sensor1");

          g_loop_beats = 0;
          if (sensor != NULL)
            {
              printf("[SM] alive, sensor1 temp=%d.%d C\n",
                     sensor->temp / 10, sensor->temp % 10);
            }
          else
            {
              printf("[SM] alive (no sensor1 in table)\n");
            }
        }

      if (idle > SM_LOOP_SLEEP_MAX_MS)
        {
          idle = SM_LOOP_SLEEP_MAX_MS;
        }
      else if (idle == 0)
        {
          idle = 10;
        }

      usleep(idle * 1000);
    }

  return 0;
}
