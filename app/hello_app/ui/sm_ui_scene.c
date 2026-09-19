/****************************************************************************
 * app/hello_app/ui/sm_ui_scene.c — 场景页
 *
 * 三个大按钮（Go Home / Away / Sleep），点击后逐项调 sm_set_device() 应用
 * 场景，再调 sm_beep_play() 播放提示音（文件缺失时引擎侧静默），最后即时
 * 差量刷新总览页卡片。按钮下小字说明各自动作。
 *
 * 场景定义（与需求一致）：
 *   Go Home: light1 on=1 br=80, light2 on=1 br=50, socket1 on=1
 *   Away   : light1/light2/socket1 全 off
 *   Sleep  : light2 off, socket1 off, light1 on=1 br=10
 ****************************************************************************/

#include "sm_ui.h"

#include "../smarthome_internal.h"    /* sm_beep_play */

/* ---- 内部数据 ----------------------------------------------------------- */

#define PAGE_W   480
#define PAGE_H   192
#define BTN_W    140
#define BTN_H    92
#define BTN_Y    14
#define DESC_Y   (BTN_Y + BTN_H + 8)      /* 114，按钮下小字 */
#define COL_X(i) (10 + (i) * 160)          /* 10 / 170 / 330 */

typedef struct
{
  const char *name;   /* 按钮文案 */
  const char *desc;   /* 按钮下小字说明 */
} scene_desc_t;

static const scene_desc_t s_scenes[3] =
{
  { "Go Home", "light1 on 80%\nlight2 on 50%\nsocket1 on" },
  { "Away",    "light1 off\nlight2 off\nsocket1 off" },
  { "Sleep",   "light1 on 10%\nlight2 off\nsocket1 off" },
};

/* ---- 场景应用 ----------------------------------------------------------- */

static void scene_apply(int idx)
{
  switch (idx)
  {
    case 0:   /* Go Home */
      sm_set_device("light1", SM_PROP_ON, 1);
      sm_set_device("light1", SM_PROP_BRIGHTNESS, 80);
      sm_set_device("light2", SM_PROP_ON, 1);
      sm_set_device("light2", SM_PROP_BRIGHTNESS, 50);
      sm_set_device("socket1", SM_PROP_ON, 1);
      break;

    case 1:   /* Away：全部关闭 */
      sm_set_device("light1", SM_PROP_ON, 0);
      sm_set_device("light2", SM_PROP_ON, 0);
      sm_set_device("socket1", SM_PROP_ON, 0);
      break;

    case 2:   /* Sleep：夜灯模式 */
      sm_set_device("light1", SM_PROP_ON, 1);
      sm_set_device("light1", SM_PROP_BRIGHTNESS, 10);
      sm_set_device("light2", SM_PROP_ON, 0);
      sm_set_device("socket1", SM_PROP_ON, 0);
      break;

    default:
      return;
  }

  /* 场景切换提示音（引擎侧任何失败均静默，不阻塞） */
  sm_beep_play();

  /* 不等下一个 200ms tick，立即刷新总览页卡片 */
  sm_ui_overview_tick();
}

static void scene_btn_cb(lv_event_t *e)
{
  scene_apply((int)(intptr_t)lv_event_get_user_data(e));
}

/* ---- 页面创建 ------------------------------------------------------------ */

lv_obj_t *sm_ui_scene_create(lv_obj_t *parent)
{
  lv_obj_t *page = sm_ui_panel(parent);
  lv_obj_set_size(page, PAGE_W, PAGE_H);

  int i;
  for (i = 0; i < 3; i++)
  {
    /* 场景大按钮 */
    lv_obj_t *btn = lv_button_create(page);
    lv_obj_set_pos(btn, COL_X(i), BTN_Y);
    lv_obj_set_size(btn, BTN_W, BTN_H);
    lv_obj_add_event_cb(btn, scene_btn_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)i);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, s_scenes[i].name);
    lv_obj_center(lbl);

    /* 按钮下小字：动作说明，三行居中 */
    lv_obj_t *desc = lv_label_create(page);
    lv_label_set_text(desc, s_scenes[i].desc);
    lv_obj_set_pos(desc, COL_X(i), DESC_Y);
    lv_obj_set_width(desc, BTN_W);
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);
  }

  return page;
}
