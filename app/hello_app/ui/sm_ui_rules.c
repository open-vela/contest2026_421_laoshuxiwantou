/****************************************************************************
 * app/hello_app/ui/sm_ui_rules.c — 规则页
 *
 * 上半区（高 106）：规则列表，滚动容器，每条 = enabled 开关 + 摘要
 *   "r1 high temp light on: sensor1.temp>=300 -> set light1.on=1"
 *   点选条目载入下方编辑器（可 Update / Delete）。
 * 下半区（高 84）：编辑器，纯下拉 + 步进按钮，无任何键盘/自由文本输入：
 *   条件下拉（sensor1.temp / light1.on / light1.brightness / socket1.on）
 *   比较符下拉（>= / <= / ==）
 *   阈值（-/+ 步进：temp 步长 5，其余 1）
 *   动作下拉（set light1.on / set light1.brightness / set light2.on /
 *             set socket1.on / beep）
 *   动作值（-/+ 步进 1；beep 无动作值，步进置灰）
 *   Add（id 自动 rN）/ Update / Delete / Save / Reset + 右侧状态文字
 *
 * 编辑模型：sm_rules_copy() 取出本地数组，Add/Update/Delete 只改本地副本；
 * "Save" 调 sm_rules_replace(完整数组, count, true) 热生效并落盘；
 * "Reset" 调 sm_rules_restore_default() 后重新拷贝刷新（不自动落盘）。
 ****************************************************************************/

#include <stdio.h>
#include <string.h>

#include "sm_ui.h"

/* ---- 布局常量 ----------------------------------------------------------- */

#define PAGE_W     480
#define PAGE_H     192
#define LIST_H     106              /* 上半区列表高度 */
#define EDIT_H     84               /* 下半区编辑器高度 */
#define EDIT_Y     (PAGE_H - EDIT_H)   /* 108 */
#define ROW_H      26               /* 列表条目高 */
#define ROW_STEP   28               /* 条目间距 */
#define WGT_H      26               /* 编辑器控件高 */
#define R1_Y       0                /* 编辑器三行 y */
#define R2_Y       29
#define R3_Y       58

/* ---- 条件/动作下拉选项表（与 smarthome_storage.h schema 枚举对应） ------- */

typedef struct
{
  const char *dev;
  sm_prop_t   prop;
  int         step;   /* 步进：temp 以 0.1°C 为单位步长 5，其余 1 */
  int         min;
  int         max;
} cond_opt_t;

static const cond_opt_t s_cond_opts[] =
{
  { "sensor1", SM_PROP_TEMP,       5,   0, 500 },
  { "light1",  SM_PROP_ON,         1,   0,   1 },
  { "light1",  SM_PROP_BRIGHTNESS, 1,   0, 100 },
  { "socket1", SM_PROP_ON,         1,   0,   1 },
};

#define COND_OPT_CNT  (int)(sizeof(s_cond_opts) / sizeof(s_cond_opts[0]))

typedef struct
{
  bool        is_set;   /* true=SM_ACT_SET，false=SM_ACT_BEEP */
  const char *dev;
  sm_prop_t   prop;
  int         min;
  int         max;
} act_opt_t;

static const act_opt_t s_act_opts[] =
{
  { true,  "light1",  SM_PROP_ON,         0,   1 },
  { true,  "light1",  SM_PROP_BRIGHTNESS, 0, 100 },
  { true,  "light2",  SM_PROP_ON,         0,   1 },
  { true,  "socket1", SM_PROP_ON,         0,   1 },
  { false, "",        SM_PROP_ON,         0,   0 },   /* beep，无动作值 */
};

#define ACT_OPT_CNT  (int)(sizeof(s_act_opts) / sizeof(s_act_opts[0]))

/* ---- 内部数据 ----------------------------------------------------------- */

static sm_rule_t s_rules[SM_MAX_RULES];   /* 本地编辑副本（sm_rules_copy 取出） */
static int       s_rule_count;
static int       s_edit_idx = -1;         /* 当前点选（编辑中）的规则下标 */

static lv_obj_t *s_list;                  /* 规则列表滚动容器 */
static lv_obj_t *s_dd_cond;               /* 条件下拉 */
static lv_obj_t *s_dd_op;                 /* 比较符下拉 */
static lv_obj_t *s_dd_act;                /* 动作下拉 */
static lv_obj_t *s_lbl_cond_val;          /* 阈值数值标签 */
static lv_obj_t *s_lbl_act_val;           /* 动作值数值标签 */
static lv_obj_t *s_btn_act_minus;         /* 动作值步进（beep 时置灰） */
static lv_obj_t *s_btn_act_plus;
static lv_obj_t *s_lbl_status;            /* 操作状态文字 */

static int s_cond_val = 300;              /* 编辑器当前阈值 */
static int s_act_val  = 1;                /* 编辑器当前动作值 */

/* 前向声明（row_click_cb 在定义之前引用） */
static void editor_load(int idx);

/* ---- 小工具 ------------------------------------------------------------- */

static const char *prop_str(sm_prop_t p)
{
  switch (p)
  {
    case SM_PROP_ON:         return "on";
    case SM_PROP_BRIGHTNESS: return "brightness";
    case SM_PROP_TEMP:       return "temp";
    default:                 return "?";
  }
}

static const char *op_str(sm_op_t op)
{
  switch (op)
  {
    case SM_OP_GE: return ">=";
    case SM_OP_LE: return "<=";
    case SM_OP_EQ: return "==";
    default:       return ">=";
  }
}

static int clampi(int v, int lo, int hi)
{
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static void status_set(const char *txt)
{
  if (s_lbl_status != NULL)
    lv_label_set_text(s_lbl_status, txt);
}

static void refresh_val_labels(void)
{
  lv_label_set_text_fmt(s_lbl_cond_val, "%d", s_cond_val);
  lv_label_set_text_fmt(s_lbl_act_val, "%d", s_act_val);
}

/* 条件下拉选中项 → (dev, prop) 的下标；不匹配返回 -1 */
static int cond_opt_index(const char *dev, sm_prop_t prop)
{
  int i;

  for (i = 0; i < COND_OPT_CNT; i++)
  {
    if (s_cond_opts[i].prop == prop && strcmp(s_cond_opts[i].dev, dev) == 0)
      return i;
  }

  return -1;
}

/* 动作 → 下拉选项下标；不匹配返回 -1 */
static int act_opt_index(const sm_action_t *a)
{
  int i;

  if (a->type == SM_ACT_BEEP)
    return ACT_OPT_CNT - 1;

  for (i = 0; i < ACT_OPT_CNT; i++)
  {
    if (s_act_opts[i].is_set && s_act_opts[i].prop == a->prop &&
        strcmp(s_act_opts[i].dev, a->dev) == 0)
      return i;
  }

  return -1;
}

/* 编辑器当前下拉选中值（带越界保护） */
static int cond_dd_selected(void)
{
  int i = (int)lv_dropdown_get_selected(s_dd_cond);

  if (i < 0 || i >= COND_OPT_CNT)
    i = 0;
  return i;
}

static int act_dd_selected(void)
{
  int i = (int)lv_dropdown_get_selected(s_dd_act);

  if (i < 0 || i >= ACT_OPT_CNT)
    i = 0;
  return i;
}

/* ---- 编辑器 → 规则字段 --------------------------------------------------- */

static void editor_fill_cond(sm_condition_t *c)
{
  int i = cond_dd_selected();

  memset(c, 0, sizeof(*c));
  c->type  = SM_COND_ATTR;
  snprintf(c->dev, sizeof(c->dev), "%s", s_cond_opts[i].dev);
  c->prop  = s_cond_opts[i].prop;
  c->op    = (sm_op_t)clampi((int)lv_dropdown_get_selected(s_dd_op),
                             (int)SM_OP_GE, (int)SM_OP_EQ);
  c->value = s_cond_val;
  /* hour/minute 保留字段清零（SM_COND_TIME 未使用） */
}

static void editor_fill_action(sm_action_t *a)
{
  int i = act_dd_selected();

  memset(a, 0, sizeof(*a));
  a->type = s_act_opts[i].is_set ? SM_ACT_SET : SM_ACT_BEEP;
  snprintf(a->dev, sizeof(a->dev), "%s", s_act_opts[i].dev);
  a->prop  = s_act_opts[i].prop;
  a->value = s_act_opts[i].is_set ? s_act_val : 0;
}

/* ---- 规则摘要与 id 生成 --------------------------------------------------- */

/* "r1 high temp light on: sensor1.temp>=300 -> set light1.on=1" 样式 */
static void rule_summary(const sm_rule_t *r, char *buf, int len)
{
  if (r->action.type == SM_ACT_BEEP)
  {
    snprintf(buf, len, "%s %s: %s.%s%s%d -> beep",
             r->id, r->name,
             r->cond.dev, prop_str(r->cond.prop), op_str(r->cond.op),
             r->cond.value);
  }
  else
  {
    snprintf(buf, len, "%s %s: %s.%s%s%d -> set %s.%s=%d",
             r->id, r->name,
             r->cond.dev, prop_str(r->cond.prop), op_str(r->cond.op),
             r->cond.value,
             r->action.dev, prop_str(r->action.prop), r->action.value);
  }
}

static bool rule_id_exists(int n)
{
  char tmp[SM_RULE_ID_LEN];
  int  i;

  snprintf(tmp, sizeof(tmp), "r%d", n);
  for (i = 0; i < s_rule_count; i++)
  {
    if (strcmp(s_rules[i].id, tmp) == 0)
      return true;
  }

  return false;
}

/* 生成 "rN"：现有 rN 最大编号 +1；无编号规则时从条数起探并保证唯一 */
static void next_rule_id(char *buf, int len)
{
  int i, n, maxn = 0;

  for (i = 0; i < s_rule_count; i++)
  {
    if (sscanf(s_rules[i].id, "r%d", &n) == 1 && n > maxn)
      maxn = n;
  }

  if (maxn <= 0)
    maxn = s_rule_count;

  do
  {
    maxn++;
  } while (rule_id_exists(maxn));

  snprintf(buf, len, "r%d", maxn);
}

/* ---- 规则列表 ------------------------------------------------------------ */

/* 列表选中高亮：只改状态，不重建列表（避免在事件回调里删除事件目标对象） */
static void highlight_row(int sel)
{
  uint32_t i, n = lv_obj_get_child_count(s_list);

  for (i = 0; i < n; i++)
  {
    lv_obj_t *row = lv_obj_get_child(s_list, i);

    if ((int)i == sel)
      lv_obj_add_state(row, LV_STATE_CHECKED);
    else
      lv_obj_remove_state(row, LV_STATE_CHECKED);
  }
}

/* 点选列表条目：载入编辑器 */
static void row_click_cb(lv_event_t *e)
{
  int idx = (int)(intptr_t)lv_event_get_user_data(e);

  if (idx < 0 || idx >= s_rule_count)
    return;

  s_edit_idx = idx;
  editor_load(idx);
  highlight_row(idx);
}

/* 列表条目上的 enabled 开关：只改本地副本，Save 时统一生效 */
static void row_enable_cb(lv_event_t *e)
{
  lv_obj_t *sw  = (lv_obj_t *)lv_event_get_target(e);
  int       idx = (int)(intptr_t)lv_event_get_user_data(e);

  if (idx >= 0 && idx < s_rule_count)
    s_rules[idx].enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
}

/* 重建规则列表（仅在 Add/Delete/Save/Reset 等非列表事件回调里调用） */
static void list_refresh(void)
{
  int i;

  lv_obj_clean(s_list);

  for (i = 0; i < s_rule_count; i++)
  {
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_set_pos(row, 0, i * ROW_STEP);
    lv_obj_set_size(row, PAGE_W - 8, ROW_H);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)i);

    /* enabled 开关 */
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 44, 22);
    lv_obj_set_pos(sw, 2, 2);
    if (s_rules[i].enabled)
      lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, row_enable_cb, LV_EVENT_VALUE_CHANGED,
                        (void *)(intptr_t)i);

    /* 名称 + 条件/动作摘要（超长省略号截断） */
    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_pos(lbl, 52, 5);
    lv_obj_set_width(lbl, PAGE_W - 8 - 58);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);

    char buf[160];
    rule_summary(&s_rules[i], buf, sizeof(buf));
    lv_label_set_text(lbl, buf);

    if (i == s_edit_idx)
      lv_obj_add_state(row, LV_STATE_CHECKED);
  }
}

/* ---- 编辑器事件 ----------------------------------------------------------- */

static void editor_load(int idx)
{
  const sm_rule_t *r  = &s_rules[idx];
  int              ci = cond_opt_index(r->cond.dev, r->cond.prop);
  int              ai = act_opt_index(&r->action);
  uint32_t         op_sel;

  /* 规则中出现下拉表达不了的条件/动作（如自定义 JSON 载入的设备）时
   * 回落到第一项；只要不按 Update，原规则不会被改写 */
  if (ci < 0) ci = 0;
  if (ai < 0) ai = 0;

  lv_dropdown_set_selected(s_dd_cond, (uint32_t)ci);
  s_cond_val = clampi(r->cond.value, s_cond_opts[ci].min, s_cond_opts[ci].max);

  op_sel = (r->cond.op >= SM_OP_GE && r->cond.op <= SM_OP_EQ)
           ? (uint32_t)r->cond.op : 0u;
  lv_dropdown_set_selected(s_dd_op, op_sel);

  lv_dropdown_set_selected(s_dd_act, (uint32_t)ai);
  s_act_val = clampi(r->action.value, s_act_opts[ai].min, s_act_opts[ai].max);

  /* beep 动作无动作值，步进按钮置灰 */
  uint32_t dis = s_act_opts[ai].is_set ? 0u : 1u;
  if (dis)
  {
    lv_obj_add_state(s_btn_act_minus, LV_STATE_DISABLED);
    lv_obj_add_state(s_btn_act_plus, LV_STATE_DISABLED);
  }
  else
  {
    lv_obj_remove_state(s_btn_act_minus, LV_STATE_DISABLED);
    lv_obj_remove_state(s_btn_act_plus, LV_STATE_DISABLED);
  }

  refresh_val_labels();
}

/* 条件下拉变化：阈值收进新量程（步长随选项在步进回调里实时取） */
static void cond_dd_cb(lv_event_t *e)
{
  (void)e;
  int i = cond_dd_selected();

  s_cond_val = clampi(s_cond_val, s_cond_opts[i].min, s_cond_opts[i].max);
  refresh_val_labels();
}

/* 动作下拉变化：动作值收进新量程；beep 置灰步进 */
static void act_dd_cb(lv_event_t *e)
{
  (void)e;
  int i = act_dd_selected();

  if (s_act_opts[i].is_set)
  {
    lv_obj_remove_state(s_btn_act_minus, LV_STATE_DISABLED);
    lv_obj_remove_state(s_btn_act_plus, LV_STATE_DISABLED);
    s_act_val = clampi(s_act_val, s_act_opts[i].min, s_act_opts[i].max);
  }
  else
  {
    lv_obj_add_state(s_btn_act_minus, LV_STATE_DISABLED);
    lv_obj_add_state(s_btn_act_plus, LV_STATE_DISABLED);
  }

  refresh_val_labels();
}

/* 步进按钮：user_data 0=阈值- 1=阈值+ 2=动作值- 3=动作值+ */
static void step_cb(lv_event_t *e)
{
  int which = (int)(intptr_t)lv_event_get_user_data(e);
  int ci    = cond_dd_selected();
  int ai    = act_dd_selected();

  if (which < 2)
  {
    int step = s_cond_opts[ci].step;

    s_cond_val = clampi(s_cond_val + ((which == 1) ? step : -step),
                        s_cond_opts[ci].min, s_cond_opts[ci].max);
  }
  else
  {
    if (!s_act_opts[ai].is_set)
      return;   /* beep 无动作值（按钮已置灰，双保险） */

    s_act_val = clampi(s_act_val + ((which == 3) ? 1 : -1),
                       s_act_opts[ai].min, s_act_opts[ai].max);
  }

  refresh_val_labels();
}

/* ---- Add / Update / Delete / Save / Reset --------------------------------- */

static void add_cb(lv_event_t *e)
{
  (void)e;

  if (s_rule_count >= SM_MAX_RULES)
  {
    status_set("Rule list full");
    return;
  }

  sm_rule_t *r = &s_rules[s_rule_count];

  memset(r, 0, sizeof(*r));
  next_rule_id(r->id, sizeof(r->id));
  snprintf(r->name, sizeof(r->name), "rule %d", s_rule_count + 1);
  r->enabled = true;
  editor_fill_cond(&r->cond);
  editor_fill_action(&r->action);

  s_edit_idx = s_rule_count;
  s_rule_count++;

  list_refresh();
  status_set("Added, press Save");
}

static void update_cb(lv_event_t *e)
{
  (void)e;

  if (s_edit_idx < 0 || s_edit_idx >= s_rule_count)
  {
    status_set("Select a rule first");
    return;
  }

  editor_fill_cond(&s_rules[s_edit_idx].cond);
  editor_fill_action(&s_rules[s_edit_idx].action);

  list_refresh();
  status_set("Updated, press Save");
}

static void delete_cb(lv_event_t *e)
{
  (void)e;

  if (s_edit_idx < 0 || s_edit_idx >= s_rule_count)
  {
    status_set("Select a rule first");
    return;
  }

  memmove(&s_rules[s_edit_idx], &s_rules[s_edit_idx + 1],
          (size_t)(s_rule_count - s_edit_idx - 1) * sizeof(sm_rule_t));
  s_rule_count--;
  s_edit_idx = -1;

  list_refresh();
  status_set("Deleted, press Save");
}

static void save_cb(lv_event_t *e)
{
  (void)e;

  int rc = sm_rules_replace(s_rules, s_rule_count, true);

  if (rc == SM_OK)
  {
    status_set("Saved & hot-applied");
  }
  else
  {
    char buf[32];
    snprintf(buf, sizeof(buf), "Save failed (%d)", rc);
    status_set(buf);
  }
}

static void reset_cb(lv_event_t *e)
{
  (void)e;

  if (sm_rules_restore_default() != SM_OK)
  {
    status_set("Reset failed");
    return;
  }

  /* 恢复默认只进内存（不落盘），重新拷贝到本地副本并刷新 */
  s_rule_count = 0;
  sm_rules_copy(s_rules, SM_MAX_RULES, &s_rule_count);
  s_edit_idx = -1;

  list_refresh();
  status_set("Defaults, not saved");
}

/* ---- 页面构建 ------------------------------------------------------------- */

static lv_obj_t *editor_button(lv_obj_t *parent, int x, int y, int w,
                               const char *txt, lv_event_cb_t cb, int ud)
{
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_set_pos(btn, x, y);
  lv_obj_set_size(btn, w, WGT_H);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)ud);

  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, txt);
  lv_obj_center(lbl);

  return btn;
}

static lv_obj_t *editor_dropdown(lv_obj_t *parent, int x, int y, int w,
                                 const char *opts, lv_event_cb_t cb)
{
  lv_obj_t *dd = lv_dropdown_create(parent);
  lv_obj_set_pos(dd, x, y);
  lv_obj_set_size(dd, w, WGT_H);
  lv_dropdown_set_options(dd, opts);
  if (cb != NULL)
    lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, NULL);
  return dd;
}

/* 数值标签（-/+ 步进中间的只读显示） */
static lv_obj_t *editor_val_label(lv_obj_t *parent, int x, int y)
{
  lv_obj_t *lbl = lv_label_create(parent);
  lv_obj_set_pos(lbl, x, y);
  lv_obj_set_width(lbl, 52);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(lbl, "0");
  return lbl;
}

static void build_editor(lv_obj_t *page)
{
  lv_obj_t *edit = sm_ui_panel(page);
  lv_obj_set_pos(edit, 0, EDIT_Y);
  lv_obj_set_size(edit, PAGE_W, EDIT_H);

  /* 第一行：If [条件下拉] [比较符] [-] [阈值] [+] */
  lv_obj_t *lif = lv_label_create(edit);
  lv_label_set_text(lif, "If");
  lv_obj_set_pos(lif, 2, R1_Y + 5);

  s_dd_cond = editor_dropdown(edit, 28, R1_Y, 148,
                  "sensor1.temp\nlight1.on\nlight1.brightness\nsocket1.on",
                  cond_dd_cb);
  s_dd_op   = editor_dropdown(edit, 180, R1_Y, 56, ">=\n<=\n==", NULL);
  (void)editor_button(edit, 240, R1_Y, 26, "-", step_cb, 0);
  s_lbl_cond_val = editor_val_label(edit, 270, R1_Y + 5);
  (void)editor_button(edit, 326, R1_Y, 26, "+", step_cb, 1);

  /* 第二行：Do [动作下拉] [-] [动作值] [+] */
  lv_obj_t *ldo = lv_label_create(edit);
  lv_label_set_text(ldo, "Do");
  lv_obj_set_pos(ldo, 2, R2_Y + 5);

  s_dd_act = editor_dropdown(edit, 28, R2_Y, 148,
                 "set light1.on\nset light1.brightness\nset light2.on\n"
                 "set socket1.on\nbeep",
                 act_dd_cb);
  s_btn_act_minus = editor_button(edit, 240, R2_Y, 26, "-", step_cb, 2);
  s_lbl_act_val   = editor_val_label(edit, 270, R2_Y + 5);
  s_btn_act_plus  = editor_button(edit, 326, R2_Y, 26, "+", step_cb, 3);

  /* 第三行：Add / Update / Delete / Save / Reset + 状态文字 */
  (void)editor_button(edit, 2, R3_Y, 54, "Add", add_cb, 0);
  (void)editor_button(edit, 60, R3_Y, 72, "Update", update_cb, 0);
  (void)editor_button(edit, 136, R3_Y, 64, "Delete", delete_cb, 0);
  (void)editor_button(edit, 204, R3_Y, 56, "Save", save_cb, 0);
  (void)editor_button(edit, 264, R3_Y, 60, "Reset", reset_cb, 0);

  s_lbl_status = lv_label_create(edit);
  lv_obj_set_pos(s_lbl_status, 330, R3_Y + 5);
  lv_obj_set_width(s_lbl_status, 148);
  lv_label_set_long_mode(s_lbl_status, LV_LABEL_LONG_DOT);
  lv_label_set_text(s_lbl_status, "");
}

static void build_list(lv_obj_t *page)
{
  /* 滚动列表容器（纵向滚动，条目多时可滚） */
  s_list = lv_obj_create(page);
  lv_obj_set_pos(s_list, 0, 0);
  lv_obj_set_size(s_list, PAGE_W, LIST_H);
  lv_obj_set_style_pad_all(s_list, 2, 0);
  lv_obj_set_style_border_width(s_list, 0, 0);
  lv_obj_set_style_radius(s_list, 0, 0);
  lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
}

lv_obj_t *sm_ui_rules_create(lv_obj_t *parent)
{
  lv_obj_t *page = sm_ui_panel(parent);
  lv_obj_set_size(page, PAGE_W, PAGE_H);

  /* 从引擎拷贝当前规则集作为编辑起点 */
  s_rule_count = 0;
  (void)sm_rules_copy(s_rules, SM_MAX_RULES, &s_rule_count);
  s_edit_idx = -1;

  build_list(page);
  build_editor(page);
  list_refresh();
  status_set("Edit rules, then Save");

  return page;
}
