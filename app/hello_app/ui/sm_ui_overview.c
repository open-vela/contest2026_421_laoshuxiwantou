/****************************************************************************
 * app/hello_app/ui/sm_ui_overview.c — 总览页
 *
 * 设备卡片网格（2 列）：
 *   light1 / light2 ：开关 lv_switch + 亮度 lv_slider
 *   socket1         ：开关
 *   sensor1         ：温度标签（xx.x C）
 *
 * 页面底部 3 行触发记录：通过 sm_engine_set_event_cb 注册回调，
 * 追加 "r1 high temp light on -> light1.on=1" 样式，保留最近 3 条。
 *
 * 刷新策略：所有控件操作直接调 sm_set_device()（单任务模型安全）；
 * sm_ui_overview_tick() 约 200ms 轮询设备表，与 s_shown 缓存比对，
 * 只在值变化时才重写控件（避免重复重绘，滑条拖动中不打断）。
 ****************************************************************************/

#include <stdio.h>

#include "sm_ui.h"

/* ---- 布局常量 ----------------------------------------------------------- */

#define PAGE_W       480
#define PAGE_H       192
#define CARD_W       234
#define CARD_H       62
#define CARD_COL_X0  4                    /* 左列 x */
#define CARD_COL_X1  (CARD_W + 8)         /* 右列 x = 242 */
#define CARD_ROW_Y0  2                    /* 第一行 y */
#define CARD_ROW_Y1  (CARD_H + 8)         /* 第二行 y = 70 ... 70+62=132 */
#define LOG_Y        (CARD_ROW_Y1 + CARD_H + 4)   /* 触发记录区 y = 136 */
#define LOG_H        (PAGE_H - LOG_Y - 2)         /* 高 54 */

#define LOG_LINES     3
#define LOG_LINE_LEN  96

/* ---- 内部数据 ----------------------------------------------------------- */

static lv_obj_t *s_sw_light1;      /* light1 开关 */
static lv_obj_t *s_sl_light1;      /* light1 亮度滑条 */
static lv_obj_t *s_sw_light2;
static lv_obj_t *s_sl_light2;
static lv_obj_t *s_sw_socket;      /* socket1 开关 */
static lv_obj_t *s_lbl_sensor;     /* sensor1 温度标签 */

/* 触发记录环形缓冲：s_log_head 是下一条写入位置，也指向最旧一条 */
static char       s_log_lines[LOG_LINES][LOG_LINE_LEN];
static int        s_log_head;
static lv_obj_t  *s_lbl_log;

/* 最近一次刷到控件上的值（差量刷新缓存；-1 表示未知，强制首刷） */
static struct
{
  bool l1_on;
  int  l1_br;
  bool l2_on;
  int  l2_br;
  bool sk_on;
  int  temp;
} s_shown =
{
  false, -1, false, -1, false, -1,
};

/* ---- 内部辅助 ----------------------------------------------------------- */

static const char *prop_name(sm_prop_t prop)
{
  switch (prop)
  {
    case SM_PROP_ON:         return "on";
    case SM_PROP_BRIGHTNESS: return "brightness";
    case SM_PROP_TEMP:       return "temp";
    default:                 return "?";
  }
}

/* 基础卡片：主题默认外观（带边框圆角），去内边距与滚动 */
static lv_obj_t *card_create(lv_obj_t *parent, int x, int y, const char *title)
{
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, CARD_W, CARD_H);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(card, 0, 0);
  lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t *lbl = lv_label_create(card);
  lv_label_set_text(lbl, title);
  lv_obj_set_pos(lbl, 6, 4);

  return card;
}

/* ---- 控件事件回调（直接调引擎 API，单任务安全） ------------------------- */

static void switch_cb(lv_event_t *e)
{
  lv_obj_t   *sw = (lv_obj_t *)lv_event_get_target(e);
  const char *id = (const char *)lv_event_get_user_data(e);
  int         on = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;

  sm_set_device(id, SM_PROP_ON, on);

  /* 引擎链式规则可能继续改动其它设备，立即做一次差量刷新 */
  sm_ui_overview_tick();
}

static void slider_cb(lv_event_t *e)
{
  lv_obj_t   *sl = (lv_obj_t *)lv_event_get_target(e);
  const char *id = (const char *)lv_event_get_user_data(e);

  sm_set_device(id, SM_PROP_BRIGHTNESS, (int)lv_slider_get_value(sl));
  sm_ui_overview_tick();
}

/* ---- 规则命中事件（引擎 → 触发记录） ------------------------------------ */

static void log_rebuild(void)
{
  char   buf[LOG_LINES * LOG_LINE_LEN];
  size_t off = 0;
  int    i;

  buf[0] = '\0';
  /* 从最旧到最新拼 3 行（未写满的空行跳过） */
  for (i = 0; i < LOG_LINES; i++)
  {
    const char *ln = s_log_lines[(s_log_head + i) % LOG_LINES];

    if (ln[0] == '\0')
      continue;

    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "%s%s",
                            (off > 0) ? "\n" : "", ln);
  }

  lv_label_set_text(s_lbl_log, buf);
}

/* 回调运行在主任务上下文，可直接调 LVGL API（契约保证） */
static void rule_event_cb(const sm_rule_t *rule, const sm_device_t *dev,
                          int value, void *arg)
{
  (void)arg;

  if (rule == NULL || dev == NULL || s_lbl_log == NULL)
    return;

  snprintf(s_log_lines[s_log_head], sizeof(s_log_lines[0]),
           "%s %s -> %s.%s=%d",
           rule->id, rule->name, dev->id, prop_name(rule->action.prop), value);
  s_log_head = (s_log_head + 1) % LOG_LINES;

  log_rebuild();
}

/* ---- 卡片构建 ------------------------------------------------------------ */

/* 灯卡片：右上开关 + 底部亮度滑条 */
static void light_card(lv_obj_t *parent, int x, const char *id,
                       lv_obj_t **sw_out, lv_obj_t **sl_out)
{
  lv_obj_t *card = card_create(parent, x, CARD_ROW_Y0, id);

  lv_obj_t *sw = lv_switch_create(card);
  lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, -6, 2);
  lv_obj_add_event_cb(sw, switch_cb, LV_EVENT_VALUE_CHANGED, (void *)id);

  lv_obj_t *sl = lv_slider_create(card);
  lv_obj_set_pos(sl, 6, CARD_H - 22);
  lv_obj_set_width(sl, CARD_W - 12);
  lv_slider_set_range(sl, 0, 100);
  lv_obj_add_event_cb(sl, slider_cb, LV_EVENT_VALUE_CHANGED, (void *)id);

  *sw_out = sw;
  *sl_out = sl;
}

/* 插座卡片：仅右上开关 */
static void socket_card(lv_obj_t *parent, int x, const char *id,
                        lv_obj_t **sw_out)
{
  lv_obj_t *card = card_create(parent, x, CARD_ROW_Y1, id);

  lv_obj_t *sw = lv_switch_create(card);
  lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, -6, 2);
  lv_obj_add_event_cb(sw, switch_cb, LV_EVENT_VALUE_CHANGED, (void *)id);

  *sw_out = sw;
}

/* 传感器卡片：温度标签（右下，与 socket1 同行） */
static void sensor_card(lv_obj_t *parent, int x, int y, const char *id)
{
  lv_obj_t *card = card_create(parent, x, y, id);

  s_lbl_sensor = lv_label_create(card);
  lv_label_set_text(s_lbl_sensor, "--.- C");
  lv_obj_set_pos(s_lbl_sensor, 6, 26);
}

/* ---- 对内共享实现 -------------------------------------------------------- */

void sm_ui_overview_tick(void)
{
  sm_device_t *d;

  if (s_sw_light1 == NULL)
    return;

  if ((d = sm_get_device("light1")) != NULL)
  {
    if ((int)d->on != (int)s_shown.l1_on)
    {
      s_shown.l1_on = d->on;
      if (d->on)
        lv_obj_add_state(s_sw_light1, LV_STATE_CHECKED);
      else
        lv_obj_remove_state(s_sw_light1, LV_STATE_CHECKED);
    }

    if ((int)d->brightness != s_shown.l1_br &&
        !lv_slider_is_dragged(s_sl_light1))
    {
      s_shown.l1_br = (int)d->brightness;
      lv_slider_set_value(s_sl_light1, d->brightness, LV_ANIM_OFF);
    }
  }

  if ((d = sm_get_device("light2")) != NULL)
  {
    if ((int)d->on != (int)s_shown.l2_on)
    {
      s_shown.l2_on = d->on;
      if (d->on)
        lv_obj_add_state(s_sw_light2, LV_STATE_CHECKED);
      else
        lv_obj_remove_state(s_sw_light2, LV_STATE_CHECKED);
    }

    if ((int)d->brightness != s_shown.l2_br &&
        !lv_slider_is_dragged(s_sl_light2))
    {
      s_shown.l2_br = (int)d->brightness;
      lv_slider_set_value(s_sl_light2, d->brightness, LV_ANIM_OFF);
    }
  }

  if ((d = sm_get_device("socket1")) != NULL)
  {
    if ((int)d->on != (int)s_shown.sk_on)
    {
      s_shown.sk_on = d->on;
      if (d->on)
        lv_obj_add_state(s_sw_socket, LV_STATE_CHECKED);
      else
        lv_obj_remove_state(s_sw_socket, LV_STATE_CHECKED);
    }
  }

  if ((d = sm_get_device("sensor1")) != NULL &&
      (int)d->temp != s_shown.temp)
  {
    s_shown.temp = (int)d->temp;
    lv_label_set_text_fmt(s_lbl_sensor, "%d.%d C",
                          d->temp / 10, d->temp % 10);
  }
}

/* ---- 页面创建 ------------------------------------------------------------ */

lv_obj_t *sm_ui_overview_create(lv_obj_t *parent)
{
  lv_obj_t *page = sm_ui_panel(parent);
  lv_obj_set_size(page, PAGE_W, PAGE_H);

  /* 设备卡片网格（2 列） */
  light_card(page, CARD_COL_X0, "light1", &s_sw_light1, &s_sl_light1);
  light_card(page, CARD_COL_X1, "light2", &s_sw_light2, &s_sl_light2);
  socket_card(page, CARD_COL_X0, "socket1", &s_sw_socket);
  sensor_card(page, CARD_COL_X1, CARD_ROW_Y1, "sensor1");

  /* 页面底部触发记录：3 行，超长以省略号截断 */
  s_lbl_log = lv_label_create(page);
  lv_obj_set_pos(s_lbl_log, 6, LOG_Y);
  lv_obj_set_size(s_lbl_log, PAGE_W - 12, LOG_H);
  lv_label_set_long_mode(s_lbl_log, LV_LABEL_LONG_DOT);
  lv_label_set_text(s_lbl_log, "");

  /* 注册规则命中事件回调（引擎回调槽唯一，见报告说明） */
  sm_engine_set_event_cb(rule_event_cb, NULL);

  /* 用设备表当前值首刷一遍控件 */
  sm_ui_overview_tick();

  return page;
}
