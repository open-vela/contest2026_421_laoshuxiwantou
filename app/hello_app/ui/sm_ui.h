/****************************************************************************
 * app/hello_app/ui/sm_ui.h — 智能家居面板 LVGL UI（三页面触控界面）
 *
 * 队伍 421 / laoshuxiwantou，轨 B（LVGL UI）
 * 目标屏 480x272 RGB565；全部文案英文（板子无中文字体）。
 *
 * 用法（显示初始化与主循环由主集成者负责，UI 不做 lv_init/lv_display）：
 *   sm_engine_init();              // 先初始化引擎
 *   sm_ui_init();                  // 在 lv_screen_active() 上构建整棵界面树
 *   while (1) {                    // 主循环
 *     lv_timer_handler();
 *     sm_ui_tick();                // 约 200ms 一次
 *     usleep(...);
 *   }
 *
 * 本头文件同时承载 ui/ 各 .c 之间的私有共享声明（不设独立私有头）。
 ****************************************************************************/

#ifndef __SM_UI_H
#define __SM_UI_H

#include <stdbool.h>
#include <stdint.h>

#include <lvgl/lvgl.h>

#include "../smarthome_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 公共入口（主程序调用） -------------------------------------------- */

/* 在当前活动屏上构建整棵界面树：顶部栏 + 三页面内容区 + 底部 tab 栏 */
void sm_ui_init(void);

/* 主循环约 200ms 调一次：轮询设备表，用缓存值比对避免重复重绘 */
void sm_ui_tick(void);

/* ---- ui/ 内部共享接口（仅各页面源文件使用） ----------------------------- */

/* 页面索引（顺序与底部 tab 按钮一致） */
enum
{
  SM_UI_PAGE_OVERVIEW = 0,   /* 总览页 */
  SM_UI_PAGE_SCENE,          /* 场景页 */
  SM_UI_PAGE_RULES,          /* 规则页 */
  SM_UI_PAGE_COUNT,
};

/* sm_ui.c 提供 */

/* 平铺容器：无内边距/无边框/无圆角/不滚动，用于精确布局 */
lv_obj_t *sm_ui_panel(lv_obj_t *parent);

/* 切换页面：显示第 idx 页、隐藏其余页，并同步底部 tab 按钮高亮（CHECKED） */
void sm_ui_show_page(int idx);

/* 各页面实现（sm_ui_init 依次调用，返回页面容器交由 sm_ui.c 保管切换） */

lv_obj_t *sm_ui_overview_create(lv_obj_t *parent);
/* 总览页卡片轮询刷新（~200ms；场景页切换场景后也调它做即时差量刷新） */
void      sm_ui_overview_tick(void);

lv_obj_t *sm_ui_scene_create(lv_obj_t *parent);

lv_obj_t *sm_ui_rules_create(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif

#endif /* __SM_UI_H */
