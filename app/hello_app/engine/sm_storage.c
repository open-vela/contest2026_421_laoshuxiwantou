/****************************************************************************
 * engine/sm_storage.c — /data JSON 持久化（rules.json / devices.json）
 *
 * 队伍 421 / laoshuxiwantou，2026-09-19
 *
 * 仅依赖标准 C 文件 I/O（fopen/fread/fwrite/rename），host 单测与板端
 * 共用同一实现。save 原子写：先写 .tmp 再 rename，避免断电写半截。
 * 打开失败区分 SM_ERR_NOTFOUND（文件不存在）与 SM_ERR_IO（其他失败），
 * 由调用方（sm_engine_init）回退默认集，保证拔卡/首启不卡启动。
 *
 * host 单测支持环境变量 SM_DATA_DIR 重定向数据目录（仅 SM_HOST_TEST
 * 生效）；board 构建宏路径 /data/... 原样使用。
 ****************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "smarthome_types.h"
#include "smarthome_storage.h"

#ifdef SM_HOST_TEST
#include <stdlib.h>                     /* getenv（仅 host 单测） */
#endif

/* 出厂默认规则条数（高温开灯 / 低温关灯 / 高亮度开插座 / 超温报警鸣音） */
#define SM_DEF_RULE_COUNT   4
/* 出厂默认设备条数（light1/light2/socket1/sensor1，见 smarthome_types.h） */
#define SM_DEF_DEV_COUNT    4

/* 路径缓冲：目录 + 文件名 + 扩展余量 */
#define SM_PATH_MAX         96

/* 共享静态 JSON 缓冲（契约 SM_JSON_BUF_SIZE；单任务无并发） */
static char s_json_buf[SM_JSON_BUF_SIZE];

/****************************************************************************
 * 出厂默认集（与 smarthome_types.h 注释、storage.h schema 示例一致）
 ****************************************************************************/

static const sm_rule_t s_default_rules[SM_DEF_RULE_COUNT] =
{
  {   /* 高温开灯 */
    .id      = "r1",
    .name    = "high temp light on",
    .enabled = true,
    .cond    =
      {
        .type  = SM_COND_ATTR,
        .dev   = "sensor1",
        .prop  = SM_PROP_TEMP,
        .op    = SM_OP_GE,
        .value = 300,
      },
    .action  =
      {
        .type  = SM_ACT_SET,
        .dev   = "light1",
        .prop  = SM_PROP_ON,
        .value = 1,
      },
  },
  {   /* 低温关灯 */
    .id      = "r2",
    .name    = "low temp light off",
    .enabled = true,
    .cond    =
      {
        .type  = SM_COND_ATTR,
        .dev   = "sensor1",
        .prop  = SM_PROP_TEMP,
        .op    = SM_OP_LE,
        .value = 250,
      },
    .action  =
      {
        .type  = SM_ACT_SET,
        .dev   = "light1",
        .prop  = SM_PROP_ON,
        .value = 0,
      },
  },
  {   /* 高亮度开插座 */
    .id      = "r3",
    .name    = "bright socket on",
    .enabled = true,
    .cond    =
      {
        .type  = SM_COND_ATTR,
        .dev   = "light1",
        .prop  = SM_PROP_BRIGHTNESS,
        .op    = SM_OP_GE,
        .value = 80,
      },
    .action  =
      {
        .type  = SM_ACT_SET,
        .dev   = "socket1",
        .prop  = SM_PROP_ON,
        .value = 1,
      },
  },
  {   /* 超温报警鸣音（beep 动作无目标设备，value 无效恒 0） */
    .id      = "r4",
    .name    = "high temp beep",
    .enabled = true,
    .cond    =
      {
        .type  = SM_COND_ATTR,
        .dev   = "sensor1",
        .prop  = SM_PROP_TEMP,
        .op    = SM_OP_GE,
        .value = 320,
      },
    .action  =
      {
        .type = SM_ACT_BEEP,
      },
  },
};

/* 出厂设备状态：light1 亮度 60 与 schema 示例一致，其余取演示初始值 */
static const sm_device_t s_default_devices[SM_DEF_DEV_COUNT] =
{
  { .id = "light1",  .type = SM_DEV_LIGHT,  .on = false, .brightness = 60,  .temp = 0   },
  { .id = "light2",  .type = SM_DEV_LIGHT,  .on = false, .brightness = 50,  .temp = 0   },
  { .id = "socket1", .type = SM_DEV_SOCKET, .on = false, .brightness = 0,   .temp = 0   },
  { .id = "sensor1", .type = SM_DEV_SENSOR, .on = false, .brightness = 0,   .temp = 260 },
};

/****************************************************************************
 * 内部工具
 ****************************************************************************/

/* 拼数据文件完整路径。file 为 SM_RULES_PATH 等宏（形如 "/data/xxx"）；
 * host 单测经 SM_DATA_DIR 重定向（替换前缀 "/data"），board 原样返回。 */
static int sm_path(const char *file, char *out, int outlen)
{
  int n;

#if defined(SM_HOST_TEST)
  const char *dir = getenv("SM_DATA_DIR");

  if (dir == NULL || dir[0] == '\0')
    {
      dir = "/data";                    /* 未设置时与板端行为一致 */
    }

  n = snprintf(out, outlen, "%s%s", dir, file + 5);   /* 跳过 "/data" */
#else
  (void)file;
  n = snprintf(out, outlen, "%s", file);
#endif

  if (n < 0 || n >= outlen)
    {
      return SM_ERR_IO;
    }

  return SM_OK;
}

/* 原子写：写 path+".tmp" 成功后 rename 覆盖正式文件 */
static int sm_save_file(const char *path, const char *json, int len)
{
  char tmp[SM_PATH_MAX + 8];
  FILE *fp;

  snprintf(tmp, sizeof(tmp), "%s.tmp", path);

  fp = fopen(tmp, "wb");
  if (fp == NULL)
    {
      return SM_ERR_IO;                 /* 目录不存在/介质故障 */
    }

  if (fwrite(json, 1, (size_t)len, fp) != (size_t)len)
    {
      fclose(fp);
      return SM_ERR_IO;
    }

  if (fclose(fp) != 0)
    {
      return SM_ERR_IO;
    }

  if (rename(tmp, path) != 0)
    {
      remove(tmp);                      /* 尽力清理半截临时文件 */
      return SM_ERR_IO;
    }

  return SM_OK;
}

/* 读整个文件到静态缓冲并交由 parse 回调解析；缓冲不足按损坏处理 */
static int sm_load_file(const char *path,
                        int (*parse)(const char *text, void *out, int max,
                                     int *count),
                        void *out, int max, int *count)
{
  FILE *fp;
  size_t n;
  int c;

  fp = fopen(path, "rb");
  if (fp == NULL)
    {
      /* 契约：文件不存在返回 NOTFOUND，由调用方回退默认集 */
      return (errno == ENOENT) ? SM_ERR_NOTFOUND : SM_ERR_IO;
    }

  n = fread(s_json_buf, 1, sizeof(s_json_buf) - 1, fp);
  if (ferror(fp))
    {
      fclose(fp);
      return SM_ERR_IO;
    }

  /* 文件超出缓冲：再读一个字节确认确实到 EOF，否则按损坏处理 */
  c = fgetc(fp);
  if (c != EOF)
    {
      fclose(fp);
      return SM_ERR_IO;
    }

  fclose(fp);
  s_json_buf[n] = '\0';

  return parse(s_json_buf, out, max, count);
}

/* parse 回调适配：rules */
static int sm_parse_rules_adapter(const char *text, void *out, int max,
                                  int *count)
{
  return sm_json_parse_rules(text, (sm_rule_t *)out, max, count);
}

/* parse 回调适配：devices */
static int sm_parse_devices_adapter(const char *text, void *out, int max,
                                    int *count)
{
  return sm_json_parse_devices(text, (sm_device_t *)out, max, count);
}

/****************************************************************************
 * 对外 API：持久化
 ****************************************************************************/

int sm_storage_load_rules(sm_rule_t *out, int max, int *count)
{
  char path[SM_PATH_MAX];
  int ret;

  if (out == NULL || count == NULL || max < 0)
    {
      return SM_ERR_INVALID;
    }

  ret = sm_path(SM_RULES_PATH, path, sizeof(path));
  if (ret != SM_OK)
    {
      return ret;
    }

  return sm_load_file(path, sm_parse_rules_adapter, out, max, count);
}

int sm_storage_save_rules(const sm_rule_t *rules, int count)
{
  char path[SM_PATH_MAX];
  int len;
  int ret;

  if (rules == NULL || count < 0 || count > SM_MAX_RULES)
    {
      return SM_ERR_INVALID;
    }

  len = sm_json_rules_to_str(rules, count, s_json_buf, sizeof(s_json_buf));
  if (len < 0)
    {
      return len;
    }

  ret = sm_path(SM_RULES_PATH, path, sizeof(path));
  if (ret != SM_OK)
    {
      return ret;
    }

  return sm_save_file(path, s_json_buf, len);
}

int sm_storage_load_devices(sm_device_t *out, int max, int *count)
{
  char path[SM_PATH_MAX];
  int ret;

  if (out == NULL || count == NULL || max < 0)
    {
      return SM_ERR_INVALID;
    }

  ret = sm_path(SM_DEVICES_PATH, path, sizeof(path));
  if (ret != SM_OK)
    {
      return ret;
    }

  return sm_load_file(path, sm_parse_devices_adapter, out, max, count);
}

int sm_storage_save_devices(const sm_device_t *devs, int count)
{
  char path[SM_PATH_MAX];
  int len;
  int ret;

  if (devs == NULL || count < 0 || count > SM_MAX_DEVICES)
    {
      return SM_ERR_INVALID;
    }

  len = sm_json_devices_to_str(devs, count, s_json_buf, sizeof(s_json_buf));
  if (len < 0)
    {
      return len;
    }

  ret = sm_path(SM_DEVICES_PATH, path, sizeof(path));
  if (ret != SM_OK)
    {
      return ret;
    }

  return sm_save_file(path, s_json_buf, len);
}

int sm_storage_default_rules(sm_rule_t *out, int max, int *count)
{
  if (out == NULL || count == NULL)
    {
      return SM_ERR_INVALID;
    }

  if (max < SM_DEF_RULE_COUNT)
    {
      return SM_ERR_NOMEM;
    }

  memcpy(out, s_default_rules, sizeof(s_default_rules));
  *count = SM_DEF_RULE_COUNT;
  return SM_OK;
}

int sm_storage_default_devices(sm_device_t *out, int max, int *count)
{
  if (out == NULL || count == NULL)
    {
      return SM_ERR_INVALID;
    }

  if (max < SM_DEF_DEV_COUNT)
    {
      return SM_ERR_NOMEM;
    }

  memcpy(out, s_default_devices, sizeof(s_default_devices));
  *count = SM_DEF_DEV_COUNT;
  return SM_OK;
}
