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

/* 触摸设备路径（D12X demo68-nor GT911） */
#define SM_TOUCH_DEVPATH "/dev/input0"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  lv_nuttx_dsc_t info;
  lv_nuttx_result_t result;

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

      idle = lv_timer_handler();

      sm_engine_tick();
      sm_sim_sensor_tick();
      sm_ui_tick();
      sm_net_tick();

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
