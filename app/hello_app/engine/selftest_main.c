/****************************************************************************
 * engine/selftest_main.c — engine/存储/JSON/模拟器 host 单测入口
 *
 * 队伍 421 / laoshuxiwantou，2026-09-19
 *
 * 只在本机用 gcc 编译运行（见 run_selftest.sh），不参与板端构建：
 *   gcc -Wall -Wextra -std=c99 -I.. -DSM_HOST_TEST \
 *       selftest_main.c sm_engine.c sm_storage.c sm_json.c sim_sensor.c \
 *       -o /tmp/sm_selftest
 * sm_beep.c 不参与编译，本文件提供 sm_beep_play() 桩（返回负值）。
 * main 返回 0 表示全部通过，非 0 表示存在失败。
 ****************************************************************************/

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L                 /* setenv / mkdir（-std=c99） */
#endif

#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smarthome_types.h"
#include "smarthome_storage.h"
#include "smarthome_internal.h"
#include "sm_engine_internal.h"

/* ---- sm_beep_play 桩：selftest 不编译 sm_beep.c ---- */

static int g_beep_calls = 0;                    /* 记录 beep 动作触发次数 */

int sm_beep_play(void)
{
  g_beep_calls++;
  return SM_ERR_UNSUPPORTED;                    /* 桩：模拟失败静默跳过 */
}

/* ---- 测试框架 ---- */

static int g_pass = 0;
static int g_fail = 0;
static int g_hits = 0;                          /* 规则命中回调计数 */
static char g_last_rule[SM_RULE_ID_LEN] = "";
static int  g_last_value = 0;

#define CHECK(cond, name)                                       \
  do                                                            \
    {                                                           \
      if (cond)                                                 \
        {                                                       \
          g_pass++;                                             \
          printf("  PASS %s\n", name);                          \
        }                                                       \
      else                                                      \
        {                                                       \
          g_fail++;                                             \
          printf("  FAIL %s (line %d)\n", name, __LINE__);      \
        }                                                       \
    }                                                           \
  while (0)

/* 规则命中事件回调（只读入参） */
static void test_event_cb(const sm_rule_t *rule, const sm_device_t *dev,
                          int value, void *arg)
{
  (void)dev;
  (void)arg;
  g_hits++;
  snprintf(g_last_rule, sizeof(g_last_rule), "%s", rule->id);
  g_last_value = value;
}

/* 切换并准备数据目录 */
static void use_data_dir(const char *dir)
{
  setenv("SM_DATA_DIR", dir, 1);
  mkdir(dir, 0755);                             /* EEXIST 忽略 */
}

/* 写文本文件（构造损坏 JSON / 指定 devices.json 用） */
static void write_file(const char *path, const char *content)
{
  FILE *fp = fopen(path, "w");

  if (fp != NULL)
    {
      fputs(content, fp);
      fclose(fp);
    }
}

/* 规则集逐字段比较（回避结构体 padding 的 memcmp 陷阱） */
static bool rules_equal(const sm_rule_t *a, const sm_rule_t *b, int n)
{
  int i;

  for (i = 0; i < n; i++)
    {
      if (strcmp(a[i].id, b[i].id) != 0 ||
          strcmp(a[i].name, b[i].name) != 0 ||
          a[i].enabled != b[i].enabled ||
          a[i].cond.type != b[i].cond.type ||
          strcmp(a[i].cond.dev, b[i].cond.dev) != 0 ||
          a[i].cond.prop != b[i].cond.prop ||
          a[i].cond.op != b[i].cond.op ||
          a[i].cond.value != b[i].cond.value ||
          a[i].action.type != b[i].action.type ||
          strcmp(a[i].action.dev, b[i].action.dev) != 0 ||
          a[i].action.prop != b[i].action.prop ||
          a[i].action.value != b[i].action.value)
        {
          return false;
        }
    }
  return true;
}

/* 设备表逐字段比较 */
static bool devices_equal(const sm_device_t *a, const sm_device_t *b, int n)
{
  int i;

  for (i = 0; i < n; i++)
    {
      if (strcmp(a[i].id, b[i].id) != 0 ||
          a[i].type != b[i].type ||
          a[i].on != b[i].on ||
          a[i].brightness != b[i].brightness ||
          a[i].temp != b[i].temp)
        {
          return false;
        }
    }
  return true;
}

/* 重置引擎与命中统计（各引擎用例开头调用） */
static void engine_reset(const char *dir)
{
  use_data_dir(dir);
  sm_engine_init();
  g_hits = 0;
  g_last_rule[0] = '\0';
  g_last_value = 0;
}

/****************************************************************************
 * T1：JSON rules / devices 序列化 → 反序列化 roundtrip
 ****************************************************************************/

static void t_json_roundtrip(void)
{
  sm_rule_t rules[SM_MAX_RULES];
  sm_rule_t parsed[SM_MAX_RULES];
  sm_device_t devs[SM_MAX_DEVICES];
  sm_device_t dparsed[SM_MAX_DEVICES];
  char buf1[SM_JSON_BUF_SIZE];
  char buf2[SM_JSON_BUF_SIZE];
  int n = 0;
  int n2 = 0;
  int len;

  printf("[T] json roundtrip\n");

  CHECK(sm_storage_default_rules(rules, SM_MAX_RULES, &n) == SM_OK && n == 4,
        "default rules count");
  len = sm_json_rules_to_str(rules, n, buf1, sizeof(buf1));
  CHECK(len > 0, "rules to_str");
  CHECK(sm_json_parse_rules(buf1, parsed, SM_MAX_RULES, &n2) == SM_OK &&
        n2 == n, "rules parse back");
  CHECK(rules_equal(rules, parsed, n), "rules field-equal");
  CHECK(sm_json_rules_to_str(parsed, n2, buf2, sizeof(buf2)) == len &&
        strcmp(buf1, buf2) == 0, "rules str stable");
  CHECK(sm_json_rules_to_str(rules, n, buf2, 16) == SM_ERR_NOMEM,
        "rules to_str nomem");

  CHECK(sm_storage_default_devices(devs, SM_MAX_DEVICES, &n) == SM_OK &&
        n == 4, "default devices count");
  len = sm_json_devices_to_str(devs, n, buf1, sizeof(buf1));
  CHECK(len > 0, "devices to_str");
  CHECK(sm_json_parse_devices(buf1, dparsed, SM_MAX_DEVICES, &n2) == SM_OK &&
        n2 == n, "devices parse back");
  CHECK(devices_equal(devs, dparsed, n), "devices field-equal");
}

/****************************************************************************
 * T2：损坏 JSON 返回 SM_ERR_IO / time 条件返回 SM_ERR_UNSUPPORTED /
 *     超量规则返回 SM_ERR_NOMEM / 未知字段忽略
 ****************************************************************************/

static void t_json_errors(void)
{
  sm_rule_t rules[SM_MAX_RULES];
  sm_device_t devs[SM_MAX_DEVICES];
  char big[8192];
  int n = 0;
  int k;
  int rc;

  printf("[T] json corrupt input\n");

  CHECK(sm_json_parse_rules("", rules, SM_MAX_RULES, &n) == SM_ERR_IO,
        "empty text -> IO");
  CHECK(sm_json_parse_rules("{", rules, SM_MAX_RULES, &n) == SM_ERR_IO,
        "lone brace -> IO");
  CHECK(sm_json_parse_rules("{\"version\":1,\"rules\":[{\"id\":12}]}",
                            rules, SM_MAX_RULES, &n) == SM_ERR_IO,
        "id type mismatch -> IO");
  CHECK(sm_json_parse_rules(
      "{\"rules\":[{\"id\":\"a\",\"cond\":{\"type\":\"attr\",\"dev\":\"s\","
      "\"prop\":\"temp\",\"op\":\"<\",\"value\":1},\"action\":{\"type\":\"beep\"}}]}",
      rules, SM_MAX_RULES, &n) == SM_ERR_IO,
      "bad op -> IO");
  CHECK(sm_json_parse_rules(
      "{\"rules\":[{\"id\":\"a\",\"cond\":{\"type\":\"attr\",\"dev\":\"s\","
      "\"prop\":\"temp\",\"op\":\">=\",\"value\":1}]}",
      rules, SM_MAX_RULES, &n) == SM_ERR_IO,
      "missing action -> IO");
  CHECK(sm_json_parse_rules(
      "{\"rules\":[{\"id\":\"a\",\"cond\":{\"type\":\"time\",\"hour\":8,"
      "\"minute\":30},\"action\":{\"type\":\"beep\"}}]}",
      rules, SM_MAX_RULES, &n) == SM_ERR_UNSUPPORTED,
      "time cond -> UNSUPPORTED");
  CHECK(sm_json_parse_devices(
      "{\"devices\":[{\"id\":\"x\",\"type\":\"oven\"}]}",
      devs, SM_MAX_DEVICES, &n) == SM_ERR_IO,
      "bad device type -> IO");

  /* 17 条合法规则 → 超出 SM_MAX_RULES → NOMEM */
  strcpy(big, "{\"version\":1,\"rules\":[");
  for (k = 0; k < 17; k++)
    {
      char one[256];

      snprintf(one, sizeof(one),
               "%s{\"id\":\"r%02d\",\"name\":\"n%d\",\"enabled\":true,"
               "\"cond\":{\"type\":\"attr\",\"dev\":\"sensor1\","
               "\"prop\":\"temp\",\"op\":\">=\",\"value\":%d},"
               "\"action\":{\"type\":\"set\",\"dev\":\"light1\","
               "\"prop\":\"on\",\"value\":1}}",
               k > 0 ? "," : "", k, k, 300 + k);
      strcat(big, one);
    }
  strcat(big, "]}");
  rc = sm_json_parse_rules(big, rules, SM_MAX_RULES, &n);
  CHECK(rc == SM_ERR_NOMEM, "17 rules -> NOMEM");

  /* 未知字段（含嵌套对象/数组）忽略 */
  CHECK(sm_json_parse_rules(
      "{\"version\":1,\"foo\":{\"bar\":[1,2,{\"x\":null}]},"
      "\"rules\":[{\"id\":\"z\",\"name\":\"ok\",\"enabled\":true,"
      "\"note\":\"skip\",\"cond\":{\"type\":\"attr\",\"dev\":\"sensor1\","
      "\"prop\":\"temp\",\"op\":\">=\",\"value\":300,\"extra\":[1,{\"y\":2}]},"
      "\"action\":{\"type\":\"beep\"}}]}",
      rules, SM_MAX_RULES, &n) == SM_OK && n == 1 &&
      rules[0].cond.value == 300,
      "unknown fields ignored");
}

/****************************************************************************
 * T3：SM_DATA_DIR 下 save/load rules / devices roundtrip；
 *     缺文件 NOTFOUND、损坏文件 IO
 ****************************************************************************/

static void t_storage_roundtrip(void)
{
  sm_rule_t rules[SM_MAX_RULES];
  sm_rule_t loaded[SM_MAX_RULES];
  sm_device_t devs[SM_MAX_DEVICES];
  sm_device_t dloaded[SM_MAX_DEVICES];
  int n = 0;
  int ln = 0;

  printf("[T] storage save/load roundtrip\n");

  use_data_dir("/tmp/smtest");
  sm_storage_default_rules(rules, SM_MAX_RULES, &n);
  CHECK(sm_storage_save_rules(rules, n) == SM_OK, "save rules");
  CHECK(sm_storage_load_rules(loaded, SM_MAX_RULES, &ln) == SM_OK &&
        ln == n && rules_equal(rules, loaded, n), "load rules equal");

  sm_storage_default_devices(devs, SM_MAX_DEVICES, &n);
  CHECK(sm_storage_save_devices(devs, n) == SM_OK, "save devices");
  CHECK(sm_storage_load_devices(dloaded, SM_MAX_DEVICES, &ln) == SM_OK &&
        ln == n && devices_equal(devs, dloaded, n), "load devices equal");

  use_data_dir("/tmp/smtest/empty");
  CHECK(sm_storage_load_rules(loaded, SM_MAX_RULES, &ln) == SM_ERR_NOTFOUND,
        "missing rules.json -> NOTFOUND");
  CHECK(sm_storage_load_devices(dloaded, SM_MAX_DEVICES, &ln)
        == SM_ERR_NOTFOUND, "missing devices.json -> NOTFOUND");

  use_data_dir("/tmp/smtest/corrupt");
  CHECK(sm_storage_load_rules(loaded, SM_MAX_RULES, &ln) == SM_ERR_IO,
        "corrupt rules.json -> IO");
  CHECK(sm_storage_load_devices(dloaded, SM_MAX_DEVICES, &ln) == SM_ERR_IO,
        "corrupt devices.json -> IO");
}

/****************************************************************************
 * T4：默认规则集回退（缺失 / 损坏 rules.json）；devices.json 合并
 ****************************************************************************/

static void t_engine_fallback(void)
{
  sm_rule_t rules[SM_MAX_RULES];
  sm_rule_t def[SM_MAX_RULES];
  int n = 0;
  int dn = 0;

  printf("[T] engine default fallback\n");

  use_data_dir("/tmp/smtest/empty");
  CHECK(sm_engine_init() == SM_OK, "init on empty data dir");
  CHECK(sm_rules_copy(rules, SM_MAX_RULES, &n) == SM_OK && n == 4,
        "fallback rules count 4");
  CHECK(sm_storage_default_rules(def, SM_MAX_RULES, &dn) == SM_OK &&
        rules_equal(rules, def, n), "rules == default set");
  CHECK(sm_get_device("light1") != NULL && sm_get_device("light2") != NULL &&
        sm_get_device("socket1") != NULL && sm_get_device("sensor1") != NULL,
        "default devices present");

  use_data_dir("/tmp/smtest/corrupt");
  CHECK(sm_engine_init() == SM_OK, "init on corrupt data dir");
  CHECK(sm_rules_count() == 4, "corrupt fallback rules count 4");
  CHECK(sm_get_device("light1") != NULL, "devices kept on corrupt json");

  /* 合法 devices.json：按 id 合并，未列出的设备保持出厂值 */
  use_data_dir("/tmp/smtest");
  write_file("/tmp/smtest/devices.json",
             "{\"version\":1,\"devices\":["
             "{\"id\":\"light1\",\"type\":\"light\",\"on\":1,\"brightness\":99}]}");
  CHECK(sm_engine_init() == SM_OK, "init with devices.json");
  CHECK(sm_get_device("light1")->on == true &&
        sm_get_device("light1")->brightness == 99, "devices.json merged");
  CHECK(sm_get_device("sensor1")->temp == 260,
        "unlisted device keeps default");
}

/****************************************************************************
 * T5：sm_set_device 校验（类型/属性/设备）
 ****************************************************************************/

static void t_engine_set_validate(void)
{
  printf("[T] set_device validation\n");

  engine_reset("/tmp/smtest/validate");

  CHECK(sm_set_device("sensor1", SM_PROP_ON, 1) == SM_ERR_INVALID,
        "sensor on rejected");
  CHECK(sm_set_device("light1", SM_PROP_TEMP, 1) == SM_ERR_INVALID,
        "light temp rejected");
  CHECK(sm_set_device("socket1", SM_PROP_BRIGHTNESS, 50) == SM_ERR_INVALID,
        "socket brightness rejected");
  CHECK(sm_set_device("nope", SM_PROP_ON, 1) == SM_ERR_INVALID,
        "unknown device rejected");
  CHECK(sm_set_device("light1", SM_PROP_ON, 1) == SM_OK,
        "light1 on accepted");
  CHECK(sm_get_device("light1")->on == true, "light1 state updated");
}

/****************************************************************************
 * T6：默认规则命中 + 边沿不重复触发 + r4 beep
 *     初始 sensor1.temp=260，r1(>=300) / r2(<=250) / r4(>=320) 均未武装
 ****************************************************************************/

static void t_engine_rules_edge(void)
{
  printf("[T] rule edge triggering\n");

  engine_reset("/tmp/smtest/edge");

  CHECK(sm_set_device("sensor1", SM_PROP_TEMP, 300) == SM_OK, "set temp 300");
  CHECK(g_hits == 1 && strcmp(g_last_rule, "r1") == 0, "r1 fires on edge");
  CHECK(sm_get_device("light1")->on == true, "light1 turned on by r1");

  CHECK(sm_set_device("sensor1", SM_PROP_TEMP, 310) == SM_OK, "set temp 310");
  CHECK(g_hits == 1, "no refire while cond stays true");

  CHECK(sm_set_device("sensor1", SM_PROP_TEMP, 280) == SM_OK, "set temp 280");
  CHECK(g_hits == 1, "no fire on falling edge");

  CHECK(sm_set_device("sensor1", SM_PROP_TEMP, 300) == SM_OK, "set temp 300");
  CHECK(g_hits == 2, "refire after re-arm");

  CHECK(sm_set_device("sensor1", SM_PROP_TEMP, 320) == SM_OK, "set temp 320");
  CHECK(g_hits == 3 && strcmp(g_last_rule, "r4") == 0,
        "r4 fires at 320 (r1 stays armed)");
  CHECK(g_beep_calls == 1, "beep attempted once");
  CHECK(g_last_value == 0, "beep hit value 0");
}

/****************************************************************************
 * T7：链式联动：自定义规则 c1(temp>=300→light1.brightness=100) 触发
 *     内置 r3(brightness>=80→socket1 on=1)，一次 set 收敛到 2 次命中
 ****************************************************************************/

static void t_engine_chain(void)
{
  sm_rule_t chain[2];

  printf("[T] chained rules\n");

  engine_reset("/tmp/smtest/chain");

  memset(chain, 0, sizeof(chain));
  strcpy(chain[0].id, "c1");
  strcpy(chain[0].name, "chain src");
  chain[0].enabled = true;
  chain[0].cond.type = SM_COND_ATTR;
  strcpy(chain[0].cond.dev, "sensor1");
  chain[0].cond.prop = SM_PROP_TEMP;
  chain[0].cond.op = SM_OP_GE;
  chain[0].cond.value = 300;
  chain[0].action.type = SM_ACT_SET;
  strcpy(chain[0].action.dev, "light1");
  chain[0].action.prop = SM_PROP_BRIGHTNESS;
  chain[0].action.value = 100;

  strcpy(chain[1].id, "r3");
  strcpy(chain[1].name, "bright socket on");
  chain[1].enabled = true;
  chain[1].cond.type = SM_COND_ATTR;
  strcpy(chain[1].cond.dev, "light1");
  chain[1].cond.prop = SM_PROP_BRIGHTNESS;
  chain[1].cond.op = SM_OP_GE;
  chain[1].cond.value = 80;
  chain[1].action.type = SM_ACT_SET;
  strcpy(chain[1].action.dev, "socket1");
  chain[1].action.prop = SM_PROP_ON;
  chain[1].action.value = 1;

  CHECK(sm_rules_replace(chain, 2, false) == SM_OK, "replace chain rules");
  CHECK(sm_set_device("sensor1", SM_PROP_TEMP, 305) == SM_OK, "set temp 305");
  CHECK(g_hits == 2, "chain fired twice in one set");
  CHECK(sm_get_device("light1")->brightness == 100, "c1 applied");
  CHECK(sm_get_device("socket1")->on == true, "socket1 on by r3 chain");
  CHECK(strcmp(g_last_rule, "r3") == 0, "last hit is chained r3");
}

/****************************************************************************
 * T8：互激规则在 4 轮上限内收敛，不挂死
 ****************************************************************************/

static void t_engine_loop_cap(void)
{
  sm_rule_t loop[4];

  printf("[T] loop cap\n");

  engine_reset("/tmp/smtest/loop");

  /* L1: light1.on==1 -> socket1.on=1; L2: socket1.on==1 -> light1.on=0;
   * L3: light1.on==0 -> socket1.on=0; L4: socket1.on==0 -> light1.on=1 */
  memset(loop, 0, sizeof(loop));
  strcpy(loop[0].id, "L1");
  loop[0].enabled = true;
  loop[0].cond.type = SM_COND_ATTR;
  strcpy(loop[0].cond.dev, "light1");
  loop[0].cond.prop = SM_PROP_ON;
  loop[0].cond.op = SM_OP_EQ;
  loop[0].cond.value = 1;
  loop[0].action.type = SM_ACT_SET;
  strcpy(loop[0].action.dev, "socket1");
  loop[0].action.prop = SM_PROP_ON;
  loop[0].action.value = 1;

  strcpy(loop[1].id, "L2");
  loop[1].enabled = true;
  loop[1].cond.type = SM_COND_ATTR;
  strcpy(loop[1].cond.dev, "socket1");
  loop[1].cond.prop = SM_PROP_ON;
  loop[1].cond.op = SM_OP_EQ;
  loop[1].cond.value = 1;
  loop[1].action.type = SM_ACT_SET;
  strcpy(loop[1].action.dev, "light1");
  loop[1].action.prop = SM_PROP_ON;
  loop[1].action.value = 0;

  strcpy(loop[2].id, "L3");
  loop[2].enabled = true;
  loop[2].cond.type = SM_COND_ATTR;
  strcpy(loop[2].cond.dev, "light1");
  loop[2].cond.prop = SM_PROP_ON;
  loop[2].cond.op = SM_OP_EQ;
  loop[2].cond.value = 0;
  loop[2].action.type = SM_ACT_SET;
  strcpy(loop[2].action.dev, "socket1");
  loop[2].action.prop = SM_PROP_ON;
  loop[2].action.value = 0;

  strcpy(loop[3].id, "L4");
  loop[3].enabled = true;
  loop[3].cond.type = SM_COND_ATTR;
  strcpy(loop[3].cond.dev, "socket1");
  loop[3].cond.prop = SM_PROP_ON;
  loop[3].cond.op = SM_OP_EQ;
  loop[3].cond.value = 0;
  loop[3].action.type = SM_ACT_SET;
  strcpy(loop[3].action.dev, "light1");
  loop[3].action.prop = SM_PROP_ON;
  loop[3].action.value = 1;

  CHECK(sm_rules_replace(loop, 4, false) == SM_OK, "replace loop rules");
  sm_set_device("light1", SM_PROP_ON, 1);
  CHECK(g_hits > 0 && g_hits <= 16, "oscillation bounded (returned)");
  CHECK(sm_get_device("light1") != NULL && sm_get_device("socket1") != NULL,
        "device table intact after loop");
}

/****************************************************************************
 * T9：规则编辑 API（copy/replace persist/restore_default/TIME 拒绝）
 ****************************************************************************/

static void t_engine_edit_apis(void)
{
  sm_rule_t all[SM_MAX_RULES];
  sm_rule_t loaded[SM_MAX_RULES];
  sm_rule_t timerule;
  int n = 0;
  int ln = 0;

  printf("[T] rule edit apis\n");

  /* 清掉上一轮 selftest 可能留下的持久化文件，保证用例可重复 */
  use_data_dir("/tmp/smtest/edit");
  remove("/tmp/smtest/edit/rules.json");
  remove("/tmp/smtest/edit/devices.json");

  engine_reset("/tmp/smtest/edit");

  CHECK(sm_rules_count() == 4, "count 4 after init");
  CHECK(sm_rules_copy(all, 2, &n) == SM_ERR_NOMEM, "copy max 2 -> NOMEM");
  CHECK(sm_rules_copy(all, SM_MAX_RULES, &n) == SM_OK && n == 4,
        "copy full set");

  /* 修改首条规则名并持久化 */
  strcpy(all[0].name, "renamed by test");
  CHECK(sm_rules_replace(all, n, true) == SM_OK, "replace with persist");
  CHECK(sm_storage_load_rules(loaded, SM_MAX_RULES, &ln) == SM_OK &&
        ln == 4 && strcmp(loaded[0].name, "renamed by test") == 0,
        "persisted rules reloaded");

  CHECK(sm_rules_restore_default() == SM_OK, "restore default");
  CHECK(sm_rules_count() == 4, "restore count 4");
  sm_rules_copy(all, SM_MAX_RULES, &n);
  sm_storage_default_rules(loaded, SM_MAX_RULES, &ln);
  CHECK(rules_equal(all, loaded, n), "restored == defaults");

  /* TIME 条件整批拒绝且原规则集不变 */
  memset(&timerule, 0, sizeof(timerule));
  strcpy(timerule.id, "t9");
  timerule.enabled = true;
  timerule.cond.type = SM_COND_TIME;
  timerule.cond.hour = 8;
  timerule.cond.minute = 30;
  timerule.action.type = SM_ACT_BEEP;
  CHECK(sm_rules_replace(&timerule, 1, false) == SM_ERR_UNSUPPORTED,
        "time rule rejected");
  CHECK(sm_rules_count() == 4, "rules unchanged after reject");
}

/****************************************************************************
 * T10：假传感器 5 秒节拍三角波推进 + 越限触发（虚拟时钟推进）
 *      轨迹：起表 → 245(r2 命中, 245≤250) → 300(r1) → 320(r4) →
 *      340 折返 → 295(r1 退网) → 250(r2 下降沿再命中) → 240 折返
 ****************************************************************************/

static void t_sim_sensor(void)
{
  int i;

  printf("[T] sim sensor\n");

  engine_reset("/tmp/smtest/sim");
  CHECK(sm_sim_sensor_enabled() == true, "sim default enabled");

  for (i = 0; i < 21; i++)
    {
      sm_sim_test_advance_ms(5000);
      sm_sim_sensor_tick();
    }

  CHECK(sm_get_device("sensor1")->temp == 340, "sim reached max 340");
  CHECK(g_hits == 3, "ascent hits: r2@245 r1@300 r4@320");

  for (i = 0; i < 20; i++)
    {
      sm_sim_test_advance_ms(5000);
      sm_sim_sensor_tick();
    }

  CHECK(sm_get_device("sensor1")->temp == 240, "sim returned to min 240");
  CHECK(g_hits == 4, "descent: r2 fires again at 250 (rising armed at 250)");
  CHECK(sm_get_device("light1")->on == false, "light1 off by r2 descent");

  /* 禁用后 tick 直接返回，温度冻结 */
  sm_sim_sensor_set_enabled(false);
  CHECK(sm_sim_sensor_enabled() == false, "disable sim");
  g_hits = 0;
  for (i = 0; i < 3; i++)
    {
      sm_sim_test_advance_ms(5000);
      sm_sim_sensor_tick();
    }
  CHECK(sm_get_device("sensor1")->temp == 240, "temp frozen when disabled");
  CHECK(g_hits == 0, "no hits when disabled");

  /* 重新使能：凑满 5 秒间隔后恢复一拍一步（240→245，上升段） */
  sm_sim_sensor_set_enabled(true);
  sm_sim_test_advance_ms(5000);
  sm_sim_sensor_tick();
  CHECK(sm_get_device("sensor1")->temp == 245, "resume stepping to 245");
}

/****************************************************************************
 * main
 ****************************************************************************/

int main(void)
{
  printf("==== smarthome engine selftest ====\n");

  /* 预置损坏文件 */
  use_data_dir("/tmp/smtest/corrupt");
  write_file("/tmp/smtest/corrupt/rules.json", "{\"version\":1,\"rules\":[");
  write_file("/tmp/smtest/corrupt/devices.json", "{\"devices\":");

  sm_engine_set_event_cb(test_event_cb, NULL);

  t_json_roundtrip();
  t_json_errors();
  t_storage_roundtrip();
  t_engine_fallback();
  t_engine_set_validate();
  t_engine_rules_edge();
  t_engine_chain();
  t_engine_loop_cap();
  t_engine_edit_apis();
  t_sim_sensor();

  printf("==== selftest summary: PASS=%d FAIL=%d ====\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
