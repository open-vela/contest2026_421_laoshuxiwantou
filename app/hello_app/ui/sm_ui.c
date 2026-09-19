/****************************************************************************
 * app/hello_app/ui/sm_ui.c — 布局总架构
 *
 * 480x272 屏幕分区：
 *   顶部标题栏  (0,0)   480x32 ：标题 + 右侧实时温度（sensor1，"26.5 C" 样式）
 *   中部内容区  (0,32)  480x192：三个页面容器叠放，LV_OBJ_FLAG_HIDDEN 切换
 *   底部 tab 栏 (0,224) 480x48 ：Overview / Scene / Rules，当前页按钮 CHECKED
 *
 * 线程模型：整个应用单任务（LVGL 主循环内 tick 驱动），此处所有回调直接调
 * 引擎 API（sm_set_device 等），无锁需求。
 ****************************************************************************/

#include "sm_ui.h"

/* ---- 屏幕分区尺寸 ------------------------------------------------------- */

#define SM_UI_SCREEN_W   480
#define SM_UI_SCREEN_H   272
#define SM_UI_TOP_H      32
#define SM_UI_TAB_H      48
#define SM_UI_TAB_W      (SM_UI_SCREEN_W / SM_UI_PAGE_COUNT)   /* 160 */

/* ---- 内部数据 ----------------------------------------------------------- */

static lv_obj_t *s_pages[SM_UI_PAGE_COUNT];   /* 三个页面容器 */
static lv_obj_t *s_tabs[SM_UI_PAGE_COUNT];    /* 底部三个 tab 按钮 */

static lv_obj_t *s_temp_label;                /* 顶栏实时温度标签 */
static int       s_top_temp = -1;             /* 上次刷到顶栏的 temp（-1=未知） */

/* ---- 内部辅助 ----------------------------------------------------------- */

/* 顶部标题栏：标题居左，实时温度居右 */
static void topbar_create(lv_obj_t *parent)
{
  lv_obj_t *bar = sm_ui_panel(parent);
  lv_obj_set_pos(bar, 0, 0);
  lv_obj_set_size(bar, SM_UI_SCREEN_W, SM_UI_TOP_H);

  lv_obj_t *title = lv_label_create(bar);
  lv_label_set_text(title, "Smart Home Panel");
  lv_obj_set_pos(title, 8, 8);

  s_temp_label = lv_label_create(bar);
  lv_label_set_text(s_temp_label, "--.- C");
  lv_obj_align(s_temp_label, LV_ALIGN_RIGHT_MID, -8, 0);
}

/* tab 按钮点击 → 切页 */
static void tab_cb(lv_event_t *e)
{
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  sm_ui_show_page(idx);
}

/* 底部 tab 栏：三个等宽按钮，当前页按钮置 CHECKED 高亮 */
static void tabbar_create(lv_obj_t *parent)
{
  static const char *tab_names[SM_UI_PAGE_COUNT] =
  {
    "Overview", "Scene", "Rules",
  };

  lv_obj_t *bar = sm_ui_panel(parent);
  lv_obj_set_pos(bar, 0, SM_UI_SCREEN_H - SM_UI_TAB_H);
  lv_obj_set_size(bar, SM_UI_SCREEN_W, SM_UI_TAB_H);

  int i;
  for (i = 0; i < SM_UI_PAGE_COUNT; i++)
  {
    lv_obj_t *btn = lv_button_create(bar);
    lv_obj_set_pos(btn, i * SM_UI_TAB_W, 0);
    lv_obj_set_size(btn, SM_UI_TAB_W, SM_UI_TAB_H);
    lv_obj_add_event_cb(btn, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, tab_names[i]);
    lv_obj_center(lbl);

    s_tabs[i] = btn;
  }
}

/* ---- 对内共享实现 ------------------------------------------------------- */

lv_obj_t *sm_ui_panel(lv_obj_t *parent)
{
  lv_obj_t *p = lv_obj_create(parent);
  lv_obj_set_style_pad_all(p, 0, 0);
  lv_obj_set_style_border_width(p, 0, 0);
  lv_obj_set_style_radius(p, 0, 0);
  lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_OFF);
  return p;
}

void sm_ui_show_page(int idx)
{
  int i;

  if (idx < 0 || idx >= SM_UI_PAGE_COUNT)
    return;

  for (i = 0; i < SM_UI_PAGE_COUNT; i++)
  {
    /* 页面显隐切换 */
    if (i == idx)
      lv_obj_remove_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);

    /* tab 按钮高亮同步 */
    if (s_tabs[i] == NULL)
      continue;

    if (i == idx)
      lv_obj_add_state(s_tabs[i], LV_STATE_CHECKED);
    else
      lv_obj_remove_state(s_tabs[i], LV_STATE_CHECKED);
  }
}

/* ---- 公共入口 ----------------------------------------------------------- */

void sm_ui_init(void)
{
  lv_obj_t *scr = lv_screen_active();
  lv_obj_t *content;

  /* 顶部标题栏 */
  topbar_create(scr);

  /* 中部内容区：三个页面容器由各页面模块创建后登记到 s_pages[] */
  content = sm_ui_panel(scr);
  lv_obj_set_pos(content, 0, SM_UI_TOP_H);
  lv_obj_set_size(content, SM_UI_SCREEN_W,
                  SM_UI_SCREEN_H - SM_UI_TOP_H - SM_UI_TAB_H);   /* 192 */

  s_pages[SM_UI_PAGE_OVERVIEW] = sm_ui_overview_create(content);
  s_pages[SM_UI_PAGE_SCENE]    = sm_ui_scene_create(content);
  s_pages[SM_UI_PAGE_RULES]    = sm_ui_rules_create(content);

  /* 底部 tab 栏 */
  tabbar_create(scr);

  /* 默认显示总览页 */
  sm_ui_show_page(SM_UI_PAGE_OVERVIEW);
}

void sm_ui_tick(void)
{
  /* 顶栏温度：sensor1.temp（0.1°C 单位），值变化才重绘 */
  sm_device_t *d = sm_get_device("sensor1");
  if (s_temp_label != NULL && d != NULL && (int)d->temp != s_top_temp)
  {
    s_top_temp = (int)d->temp;
    lv_label_set_text_fmt(s_temp_label, "%d.%d C",
                          s_top_temp / 10, s_top_temp % 10);
  }

  /* 总览页设备卡片差量刷新 */
  sm_ui_overview_tick();
}
