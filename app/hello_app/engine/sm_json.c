/****************************************************************************
 * engine/sm_json.c — 固定 schema 最小 JSON 解析 / 序列化
 *
 * 队伍 421 / laoshuxiwantou，2026-09-19
 *
 * 只服务 smarthome_storage.h 冻结的 rules.json / devices.json 两种形状，
 * 手写逐字符扫描，无动态内存分配，任何结构异常一律返回 SM_ERR_IO 而
 * 不是崩溃。未知字段跳过忽略；"time" 条件解析到即报 SM_ERR_UNSUPPORTED。
 * 数字只接受整数（冻结 schema 不含浮点）；字符串支持常见转义。
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "smarthome_types.h"
#include "smarthome_storage.h"
#include "sm_engine_internal.h"

/* 未知字段值跳过时的递归深度上限，防恶意深嵌套耗尽栈 */
#define SM_JSD_MAX_DEPTH  8

/* 未知字段 key / 字符串值跳过时的临时缓冲 */
#define SM_JSD_KEY_CAP    64
#define SM_JSD_STR_CAP    128

/****************************************************************************
 * 扫描器
 ****************************************************************************/

struct sm_jscan
{
  const char *p;    /* 当前扫描位置（文本内只读前进） */
};

/* 跳过空白（空格 / 制表 / 回车 / 换行） */
static void js_ws(struct sm_jscan *j)
{
  while (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n')
    {
      j->p++;
    }
}

/* 期待指定字节，失败返回 SM_ERR_IO */
static int js_expect(struct sm_jscan *j, char c)
{
  js_ws(j);
  if (*j->p != c)
    {
      return SM_ERR_IO;
    }

  j->p++;
  return SM_OK;
}

/* 读一个字符串字面量到 out（cap 含 '\0'），返回长度；超长按损坏处理 */
static int js_string(struct sm_jscan *j, char *out, int cap)
{
  int len = 0;

  js_ws(j);
  if (*j->p != '"')
    {
      return SM_ERR_IO;
    }

  j->p++;
  for (; ; )
    {
      unsigned char c = (unsigned char)*j->p;

      if (c == '\0')
        {
          return SM_ERR_IO;             /* 字符串未闭合 */
        }

      if (c == '"')
        {
          j->p++;
          break;
        }

      if (c == '\\')
        {
          j->p++;
          switch (*j->p)
            {
            case '"':
            case '\\':
            case '/':
              c = (unsigned char)*j->p;
              j->p++;
              break;
            case 'n':
              c = '\n';
              j->p++;
              break;
            case 't':
              c = '\t';
              j->p++;
              break;
            case 'r':
              c = '\r';
              j->p++;
              break;
            case 'b':
              c = '\b';
              j->p++;
              break;
            case 'f':
              c = '\f';
              j->p++;
              break;
            case 'u':
              {
                int i;
                /* \uXXXX：跳过 4 个十六进制位，按 '?' 占位（schema 不用） */
                j->p++;
                for (i = 0; i < 4; i++)
                  {
                    if (!isxdigit((unsigned char)j->p[i]))
                      {
                        return SM_ERR_IO;
                      }
                  }
                j->p += 4;
                c = '?';
                break;
              }
            default:
              return SM_ERR_IO;         /* 非法转义 */
            }
        }

      if (len + 1 >= cap)
        {
          return SM_ERR_IO;             /* 超出目标缓冲，按损坏处理 */
        }

      out[len++] = (char)c;
      j->p++;
    }

  out[len] = '\0';
  return len;
}

/* 读一个整数（可选负号），不支持小数 / 指数（schema 只用整数） */
static int js_int(struct sm_jscan *j, int *out)
{
  long v = 0;
  int  neg = 0;

  js_ws(j);
  if (*j->p == '-')
    {
      neg = 1;
      j->p++;
    }

  if (*j->p < '0' || *j->p > '9')
    {
      return SM_ERR_IO;
    }

  while (*j->p >= '0' && *j->p <= '9')
    {
      v = v * 10 + (*j->p - '0');
      if (v > 0x7fffffffL)
        {
          v = 0x7fffffffL;              /* 溢出钳位，避免回绕 */
        }
      j->p++;
    }

  if (*j->p == '.' || *j->p == 'e' || *j->p == 'E')
    {
      return SM_ERR_IO;                 /* 冻结 schema 不含浮点数 */
    }

  *out = (int)(neg ? -v : v);
  return SM_OK;
}

/* 读 true / false / 1 / 0（schema 用 true，宽松兼容数字布尔） */
static int js_bool(struct sm_jscan *j, bool *out)
{
  js_ws(j);
  if (strncmp(j->p, "true", 4) == 0)
    {
      j->p += 4;
      *out = true;
      return SM_OK;
    }

  if (strncmp(j->p, "false", 5) == 0)
    {
      j->p += 5;
      *out = false;
      return SM_OK;
    }

  if (*j->p == '1')
    {
      j->p++;
      *out = true;
      return SM_OK;
    }

  if (*j->p == '0')
    {
      j->p++;
      *out = false;
      return SM_OK;
    }

  return SM_ERR_IO;
}

/* 跳过一个数值 token（未知字段用，宽松消费数字字符） */
static int js_skip_number(struct sm_jscan *j)
{
  js_ws(j);
  if (*j->p != '-' && (*j->p < '0' || *j->p > '9'))
    {
      return SM_ERR_IO;
    }

  while (*j->p == '-' || *j->p == '+' || *j->p == '.' ||
         *j->p == 'e' || *j->p == 'E' ||
         (*j->p >= '0' && *j->p <= '9'))
    {
      j->p++;
    }

  return SM_OK;
}

static int js_skip_value(struct sm_jscan *j, int depth);

/* 跳过一个对象（进入时 '{' 已消费） */
static int js_skip_object(struct sm_jscan *j, int depth)
{
  js_ws(j);
  if (*j->p == '}')
    {
      j->p++;
      return SM_OK;
    }

  for (; ; )
    {
      char key[SM_JSD_KEY_CAP];
      int  ret;

      ret = js_string(j, key, sizeof(key));
      if (ret < 0)
        {
          return ret;
        }

      ret = js_expect(j, ':');
      if (ret != SM_OK)
        {
          return ret;
        }

      ret = js_skip_value(j, depth + 1);
      if (ret != SM_OK)
        {
          return ret;
        }

      js_ws(j);
      if (*j->p == ',')
        {
          j->p++;
          continue;
        }

      if (*j->p == '}')
        {
          j->p++;
          return SM_OK;
        }

      return SM_ERR_IO;
    }
}

/* 跳过一个数组（进入时 '[' 已消费） */
static int js_skip_array(struct sm_jscan *j, int depth)
{
  js_ws(j);
  if (*j->p == ']')
    {
      j->p++;
      return SM_OK;
    }

  for (; ; )
    {
      int ret = js_skip_value(j, depth + 1);
      if (ret != SM_OK)
        {
          return ret;
        }

      js_ws(j);
      if (*j->p == ',')
        {
          j->p++;
          continue;
        }

      if (*j->p == ']')
        {
          j->p++;
          return SM_OK;
        }

      return SM_ERR_IO;
    }
}

/* 跳过任意 JSON 值（未知字段忽略用） */
static int js_skip_value(struct sm_jscan *j, int depth)
{
  char tmp[SM_JSD_STR_CAP];
  int  ret;

  if (depth > SM_JSD_MAX_DEPTH)
    {
      return SM_ERR_IO;
    }

  js_ws(j);
  switch (*j->p)
    {
    case '"':
      ret = js_string(j, tmp, sizeof(tmp));
      return ret < 0 ? ret : SM_OK;
    case '{':
      j->p++;
      return js_skip_object(j, depth);
    case '[':
      j->p++;
      return js_skip_array(j, depth);
    case 't':
      if (strncmp(j->p, "true", 4) != 0)
        {
          return SM_ERR_IO;
        }
      j->p += 4;
      return SM_OK;
    case 'f':
      if (strncmp(j->p, "false", 5) != 0)
        {
          return SM_ERR_IO;
        }
      j->p += 5;
      return SM_OK;
    case 'n':
      if (strncmp(j->p, "null", 4) != 0)
        {
          return SM_ERR_IO;
        }
      j->p += 4;
      return SM_OK;
    default:
      return js_skip_number(j);
    }
}

/****************************************************************************
 * 枚举字符串映射（与 smarthome_storage.h 冻结 schema 一致）
 ****************************************************************************/

static int sm_prop_from_str(const char *s, sm_prop_t *out)
{
  if (strcmp(s, "on") == 0)
    {
      *out = SM_PROP_ON;
      return SM_OK;
    }

  if (strcmp(s, "brightness") == 0)
    {
      *out = SM_PROP_BRIGHTNESS;
      return SM_OK;
    }

  if (strcmp(s, "temp") == 0)
    {
      *out = SM_PROP_TEMP;
      return SM_OK;
    }

  return SM_ERR_IO;
}

static int sm_devtype_from_str(const char *s, sm_dev_type_t *out)
{
  if (strcmp(s, "light") == 0)
    {
      *out = SM_DEV_LIGHT;
      return SM_OK;
    }

  if (strcmp(s, "socket") == 0)
    {
      *out = SM_DEV_SOCKET;
      return SM_OK;
    }

  if (strcmp(s, "sensor") == 0)
    {
      *out = SM_DEV_SENSOR;
      return SM_OK;
    }

  return SM_ERR_IO;
}

static const char *sm_op_to_str(sm_op_t op)
{
  switch (op)
    {
    case SM_OP_GE:
      return ">=";
    case SM_OP_LE:
      return "<=";
    default:
      return "==";
    }
}

static int sm_op_from_str(const char *s, sm_op_t *out)
{
  if (strcmp(s, ">=") == 0)
    {
      *out = SM_OP_GE;
      return SM_OK;
    }

  if (strcmp(s, "<=") == 0)
    {
      *out = SM_OP_LE;
      return SM_OK;
    }

  if (strcmp(s, "==") == 0)
    {
      *out = SM_OP_EQ;
      return SM_OK;
    }

  return SM_ERR_IO;
}

/****************************************************************************
 * 规则对象解析
 ****************************************************************************/

/* cond 对象（进入时 '{' 未消费）；type=="time" 立即报 SM_ERR_UNSUPPORTED */
static int sm_json_parse_cond(struct sm_jscan *j, sm_condition_t *c)
{
  char key[SM_JSD_KEY_CAP];
  char strbuf[SM_JSD_STR_CAP];
  bool have_type = false;
  bool have_dev = false;
  bool have_prop = false;
  bool have_op = false;
  bool have_value = false;
  int  ret;

  memset(c, 0, sizeof(*c));

  ret = js_expect(j, '{');
  if (ret != SM_OK)
    {
      return ret;
    }

  js_ws(j);
  if (*j->p != '}')
    {
      for (; ; )
        {
          ret = js_string(j, key, sizeof(key));
          if (ret < 0)
            {
              return ret;
            }

          ret = js_expect(j, ':');
          if (ret != SM_OK)
            {
              return ret;
            }

          if (strcmp(key, "type") == 0)
            {
              ret = js_string(j, strbuf, sizeof(strbuf));
              if (ret < 0)
                {
                  return ret;
                }

              if (strcmp(strbuf, "attr") == 0)
                {
                  c->type = SM_COND_ATTR;
                  have_type = true;
                }
              else if (strcmp(strbuf, "time") == 0)
                {
                  /* 契约：time 为保留字，解析到即报 */
                  return SM_ERR_UNSUPPORTED;
                }
              else
                {
                  return SM_ERR_IO;
                }
            }
          else if (strcmp(key, "dev") == 0)
            {
              ret = js_string(j, c->dev, SM_DEV_ID_LEN);
              if (ret < 0)
                {
                  return ret;
                }
              have_dev = true;
            }
          else if (strcmp(key, "prop") == 0)
            {
              ret = js_string(j, strbuf, sizeof(strbuf));
              if (ret < 0)
                {
                  return ret;
                }
              ret = sm_prop_from_str(strbuf, &c->prop);
              if (ret != SM_OK)
                {
                  return ret;
                }
              have_prop = true;
            }
          else if (strcmp(key, "op") == 0)
            {
              ret = js_string(j, strbuf, sizeof(strbuf));
              if (ret < 0)
                {
                  return ret;
                }
              ret = sm_op_from_str(strbuf, &c->op);
              if (ret != SM_OK)
                {
                  return ret;
                }
              have_op = true;
            }
          else if (strcmp(key, "value") == 0)
            {
              ret = js_int(j, &c->value);
              if (ret != SM_OK)
                {
                  return ret;
                }
              have_value = true;
            }
          else if (strcmp(key, "hour") == 0 || strcmp(key, "minute") == 0)
            {
              /* TIME 保留字段，attr 条件下忽略 */
              int dummy;
              ret = js_int(j, &dummy);
              if (ret != SM_OK)
                {
                  return ret;
                }
            }
          else
            {
              ret = js_skip_value(j, 0);
              if (ret != SM_OK)
                {
                  return ret;
                }
            }

          js_ws(j);
          if (*j->p == ',')
            {
              j->p++;
              continue;
            }

          if (*j->p == '}')
            {
              j->p++;
              break;
            }

          return SM_ERR_IO;
        }
    }
  else
    {
      j->p++;
    }

  if (!have_type || !have_dev || !have_prop || !have_op || !have_value)
    {
      return SM_ERR_IO;                 /* attr 条件字段不全 */
    }

  if (c->dev[0] == '\0')
    {
      return SM_ERR_IO;
    }

  return SM_OK;
}

/* action 对象（进入时 '{' 未消费） */
static int sm_json_parse_action(struct sm_jscan *j, sm_action_t *a)
{
  char key[SM_JSD_KEY_CAP];
  char strbuf[SM_JSD_STR_CAP];
  bool have_type = false;
  bool have_dev = false;
  bool have_prop = false;
  int  ret;

  memset(a, 0, sizeof(*a));

  ret = js_expect(j, '{');
  if (ret != SM_OK)
    {
      return ret;
    }

  js_ws(j);
  if (*j->p != '}')
    {
      for (; ; )
        {
          ret = js_string(j, key, sizeof(key));
          if (ret < 0)
            {
              return ret;
            }

          ret = js_expect(j, ':');
          if (ret != SM_OK)
            {
              return ret;
            }

          if (strcmp(key, "type") == 0)
            {
              ret = js_string(j, strbuf, sizeof(strbuf));
              if (ret < 0)
                {
                  return ret;
                }

              if (strcmp(strbuf, "set") == 0)
                {
                  a->type = SM_ACT_SET;
                  have_type = true;
                }
              else if (strcmp(strbuf, "beep") == 0)
                {
                  a->type = SM_ACT_BEEP;
                  have_type = true;
                }
              else
                {
                  return SM_ERR_IO;
                }
            }
          else if (strcmp(key, "dev") == 0)
            {
              ret = js_string(j, a->dev, SM_DEV_ID_LEN);
              if (ret < 0)
                {
                  return ret;
                }
              have_dev = true;
            }
          else if (strcmp(key, "prop") == 0)
            {
              ret = js_string(j, strbuf, sizeof(strbuf));
              if (ret < 0)
                {
                  return ret;
                }
              ret = sm_prop_from_str(strbuf, &a->prop);
              if (ret != SM_OK)
                {
                  return ret;
                }
              have_prop = true;
            }
          else if (strcmp(key, "value") == 0)
            {
              ret = js_int(j, &a->value);
              if (ret != SM_OK)
                {
                  return ret;
                }
            }
          else
            {
              ret = js_skip_value(j, 0);
              if (ret != SM_OK)
                {
                  return ret;
                }
            }

          js_ws(j);
          if (*j->p == ',')
            {
              j->p++;
              continue;
            }

          if (*j->p == '}')
            {
              j->p++;
              break;
            }

          return SM_ERR_IO;
        }
    }
  else
    {
      j->p++;
    }

  if (!have_type)
    {
      return SM_ERR_IO;
    }

  if (a->type == SM_ACT_SET && (!have_dev || !have_prop || a->dev[0] == '\0'))
    {
      return SM_ERR_IO;                 /* set 动作目标不全 */
    }

  return SM_OK;
}

/* 单个规则对象（进入时 '{' 未消费） */
static int sm_json_parse_rule_obj(struct sm_jscan *j, sm_rule_t *r)
{
  char key[SM_JSD_KEY_CAP];
  bool have_cond = false;
  bool have_action = false;
  int  ret;

  memset(r, 0, sizeof(*r));
  r->enabled = false;                   /* 缺省关闭：未显式声明不自动生效 */

  ret = js_expect(j, '{');
  if (ret != SM_OK)
    {
      return ret;
    }

  js_ws(j);
  if (*j->p != '}')
    {
      for (; ; )
        {
          ret = js_string(j, key, sizeof(key));
          if (ret < 0)
            {
              return ret;
            }

          ret = js_expect(j, ':');
          if (ret != SM_OK)
            {
              return ret;
            }

          if (strcmp(key, "id") == 0)
            {
              ret = js_string(j, r->id, SM_RULE_ID_LEN);
              if (ret < 0)
                {
                  return ret;
                }
            }
          else if (strcmp(key, "name") == 0)
            {
              ret = js_string(j, r->name, SM_NAME_LEN);
              if (ret < 0)
                {
                  return ret;
                }
            }
          else if (strcmp(key, "enabled") == 0)
            {
              ret = js_bool(j, &r->enabled);
              if (ret != SM_OK)
                {
                  return ret;
                }
            }
          else if (strcmp(key, "cond") == 0)
            {
              ret = sm_json_parse_cond(j, &r->cond);
              if (ret != SM_OK)
                {
                  return ret;
                }
              have_cond = true;
            }
          else if (strcmp(key, "action") == 0)
            {
              ret = sm_json_parse_action(j, &r->action);
              if (ret != SM_OK)
                {
                  return ret;
                }
              have_action = true;
            }
          else
            {
              ret = js_skip_value(j, 0);
              if (ret != SM_OK)
                {
                  return ret;
                }
            }

          js_ws(j);
          if (*j->p == ',')
            {
              j->p++;
              continue;
            }

          if (*j->p == '}')
            {
              j->p++;
              break;
            }

          return SM_ERR_IO;
        }
    }
  else
    {
      j->p++;
    }

  if (!have_cond || !have_action)
    {
      return SM_ERR_IO;                 /* 规则缺 cond / action */
    }

  if (r->id[0] == '\0')
    {
      return SM_ERR_IO;                 /* id 必填 */
    }

  return SM_OK;
}

/* rules 数组（进入时 '[' 未消费） */
static int sm_json_parse_rules_array(struct sm_jscan *j, sm_rule_t *out,
                                     int max, int *n)
{
  int ret;

  ret = js_expect(j, '[');
  if (ret != SM_OK)
    {
      return ret;
    }

  js_ws(j);
  if (*j->p == ']')
    {
      j->p++;
      return SM_OK;
    }

  for (; ; )
    {
      if (*n >= max)
        {
          return SM_ERR_NOMEM;          /* 超过容量上限 */
        }

      ret = sm_json_parse_rule_obj(j, &out[*n]);
      if (ret != SM_OK)
        {
          return ret;
        }

      (*n)++;

      js_ws(j);
      if (*j->p == ',')
        {
          j->p++;
          continue;
        }

      if (*j->p == ']')
        {
          j->p++;
          return SM_OK;
        }

      return SM_ERR_IO;
    }
}

/****************************************************************************
 * 设备对象解析
 ****************************************************************************/

static int sm_json_parse_device_obj(struct sm_jscan *j, sm_device_t *d)
{
  char key[SM_JSD_KEY_CAP];
  char strbuf[SM_JSD_STR_CAP];
  bool have_type = false;
  int  ret;

  memset(d, 0, sizeof(*d));

  ret = js_expect(j, '{');
  if (ret != SM_OK)
    {
      return ret;
    }

  js_ws(j);
  if (*j->p != '}')
    {
      for (; ; )
        {
          ret = js_string(j, key, sizeof(key));
          if (ret < 0)
            {
              return ret;
            }

          ret = js_expect(j, ':');
          if (ret != SM_OK)
            {
              return ret;
            }

          if (strcmp(key, "id") == 0)
            {
              ret = js_string(j, d->id, SM_DEV_ID_LEN);
              if (ret < 0)
                {
                  return ret;
                }
            }
          else if (strcmp(key, "type") == 0)
            {
              ret = js_string(j, strbuf, sizeof(strbuf));
              if (ret < 0)
                {
                  return ret;
                }
              ret = sm_devtype_from_str(strbuf, &d->type);
              if (ret != SM_OK)
                {
                  return ret;
                }
              have_type = true;
            }
          else if (strcmp(key, "on") == 0)
            {
              ret = js_bool(j, &d->on);
              if (ret != SM_OK)
                {
                  return ret;
                }
            }
          else if (strcmp(key, "brightness") == 0)
            {
              ret = js_int(j, &d->brightness);
              if (ret != SM_OK)
                {
                  return ret;
                }
            }
          else if (strcmp(key, "temp") == 0)
            {
              ret = js_int(j, &d->temp);
              if (ret != SM_OK)
                {
                  return ret;
                }
            }
          else
            {
              ret = js_skip_value(j, 0);
              if (ret != SM_OK)
                {
                  return ret;
                }
            }

          js_ws(j);
          if (*j->p == ',')
            {
              j->p++;
              continue;
            }

          if (*j->p == '}')
            {
              j->p++;
              break;
            }

          return SM_ERR_IO;
        }
    }
  else
    {
      j->p++;
    }

  if (!have_type || d->id[0] == '\0')
    {
      return SM_ERR_IO;                 /* type / id 必填 */
    }

  return SM_OK;
}

static int sm_json_parse_devices_array(struct sm_jscan *j, sm_device_t *out,
                                       int max, int *n)
{
  int ret;

  ret = js_expect(j, '[');
  if (ret != SM_OK)
    {
      return ret;
    }

  js_ws(j);
  if (*j->p == ']')
    {
      j->p++;
      return SM_OK;
    }

  for (; ; )
    {
      if (*n >= max)
        {
          return SM_ERR_NOMEM;
        }

      ret = sm_json_parse_device_obj(j, &out[*n]);
      if (ret != SM_OK)
        {
          return ret;
        }

      (*n)++;

      js_ws(j);
      if (*j->p == ',')
        {
          j->p++;
          continue;
        }

      if (*j->p == ']')
        {
          j->p++;
          return SM_OK;
        }

      return SM_ERR_IO;
    }
}

/* 顶层对象公共骨架：遍历 key:value，回调式分派已知名，其余跳过 */
static int sm_json_parse_top(struct sm_jscan *j,
                             int (*on_key)(struct sm_jscan *j,
                                           const char *key, void *arg),
                             void *arg)
{
  char key[SM_JSD_KEY_CAP];
  int  ret;

  ret = js_expect(j, '{');
  if (ret != SM_OK)
    {
      return ret;
    }

  js_ws(j);
  if (*j->p == '}')
    {
      j->p++;
    }
  else
    {
      for (; ; )
        {
          ret = js_string(j, key, sizeof(key));
          if (ret < 0)
            {
              return ret;
            }

          ret = js_expect(j, ':');
          if (ret != SM_OK)
            {
              return ret;
            }

          ret = on_key(j, key, arg);
          if (ret != SM_OK)
            {
              return ret;
            }

          js_ws(j);
          if (*j->p == ',')
            {
              j->p++;
              continue;
            }

          if (*j->p == '}')
            {
              j->p++;
              break;
            }

          return SM_ERR_IO;
        }
    }

  /* 顶层结束后只允许空白 */
  js_ws(j);
  if (*j->p != '\0')
    {
      return SM_ERR_IO;
    }

  return SM_OK;
}

/* ---- rules 顶层 key 分派 ---- */

struct sm_rules_ctx
{
  sm_rule_t *out;
  int        max;
  int        n;
};

static int sm_rules_on_key(struct sm_jscan *j, const char *key, void *arg)
{
  struct sm_rules_ctx *ctx = (struct sm_rules_ctx *)arg;
  int ret;

  if (strcmp(key, "rules") == 0)
    {
      return sm_json_parse_rules_array(j, ctx->out, ctx->max, &ctx->n);
    }

  if (strcmp(key, "version") == 0)
    {
      int version;
      ret = js_int(j, &version);
      return ret;
    }

  return js_skip_value(j, 0);
}

/* ---- devices 顶层 key 分派 ---- */

struct sm_devices_ctx
{
  sm_device_t *out;
  int          max;
  int          n;
};

static int sm_devices_on_key(struct sm_jscan *j, const char *key, void *arg)
{
  struct sm_devices_ctx *ctx = (struct sm_devices_ctx *)arg;

  if (strcmp(key, "devices") == 0)
    {
      return sm_json_parse_devices_array(j, ctx->out, ctx->max, &ctx->n);
    }

  if (strcmp(key, "version") == 0)
    {
      int version;
      return js_int(j, &version);
    }

  return js_skip_value(j, 0);
}

/****************************************************************************
 * 对外 API：解析
 ****************************************************************************/

int sm_json_parse_rules(const char *text, sm_rule_t *out, int max, int *count)
{
  struct sm_jscan j;
  struct sm_rules_ctx ctx;
  int ret;

  if (text == NULL || out == NULL || count == NULL || max < 0)
    {
      return SM_ERR_INVALID;
    }

  j.p = text;
  ctx.out = out;
  ctx.max = max;
  ctx.n = 0;

  ret = sm_json_parse_top(&j, sm_rules_on_key, &ctx);
  if (ret != SM_OK)
    {
      return ret;
    }

  *count = ctx.n;
  return SM_OK;
}

int sm_json_parse_devices(const char *text, sm_device_t *out, int max,
                          int *count)
{
  struct sm_jscan j;
  struct sm_devices_ctx ctx;
  int ret;

  if (text == NULL || out == NULL || count == NULL || max < 0)
    {
      return SM_ERR_INVALID;
    }

  j.p = text;
  ctx.out = out;
  ctx.max = max;
  ctx.n = 0;

  ret = sm_json_parse_top(&j, sm_devices_on_key, &ctx);
  if (ret != SM_OK)
    {
      return ret;
    }

  *count = ctx.n;
  return SM_OK;
}

/****************************************************************************
 * 序列化（小缓冲追加器，缓冲不足统一返回 SM_ERR_NOMEM）
 ****************************************************************************/

struct sm_sbuf
{
  char *buf;
  int   cap;      /* 缓冲总容量（含 '\0' 位） */
  int   len;      /* 已写字节数（不含 '\0'） */
  int   err;      /* 非 0 表示已溢出，后续写入为空操作 */
};

static void sb_putc(struct sm_sbuf *b, char c)
{
  if (b->err != 0)
    {
      return;
    }

  if (b->len + 1 > b->cap - 1)
    {
      b->err = SM_ERR_NOMEM;
      return;
    }

  b->buf[b->len++] = c;
}

static void sb_puts(struct sm_sbuf *b, const char *s)
{
  while (*s != '\0')
    {
      sb_putc(b, *s++);
    }
}

static void sb_putint(struct sm_sbuf *b, int v)
{
  char tmp[16];

  snprintf(tmp, sizeof(tmp), "%d", v);
  sb_puts(b, tmp);
}

/* JSON 字符串字面量（带转义；非 ASCII 原样透传 UTF-8 字节） */
static void sb_putjstr(struct sm_sbuf *b, const char *s)
{
  sb_putc(b, '"');
  for (; *s != '\0'; s++)
    {
      unsigned char c = (unsigned char)*s;

      if (c == '"' || c == '\\')
        {
          sb_putc(b, '\\');
          sb_putc(b, (char)c);
        }
      else if (c < 0x20)
        {
          char t[8];

          snprintf(t, sizeof(t), "\\u%04x", c);
          sb_puts(b, t);
        }
      else
        {
          sb_putc(b, (char)c);
        }
    }
  sb_putc(b, '"');
}

int sm_json_rules_to_str(const sm_rule_t *rules, int count,
                         char *buf, int buflen)
{
  struct sm_sbuf b;
  int i;

  if (rules == NULL || buf == NULL || buflen <= 0 || count < 0)
    {
      return SM_ERR_INVALID;
    }

  b.buf = buf;
  b.cap = buflen;
  b.len = 0;
  b.err = 0;

  sb_puts(&b, "{\"version\":1,\"rules\":[");
  for (i = 0; i < count; i++)
    {
      const sm_rule_t *r = &rules[i];

      if (i > 0)
        {
          sb_putc(&b, ',');
        }

      sb_puts(&b, "{\"id\":");
      sb_putjstr(&b, r->id);
      sb_puts(&b, ",\"name\":");
      sb_putjstr(&b, r->name);
      sb_puts(&b, ",\"enabled\":");
      sb_puts(&b, r->enabled ? "true" : "false");
      sb_puts(&b, ",\"cond\":{\"type\":\"");
      sb_puts(&b, r->cond.type == SM_COND_ATTR ? "attr" : "time");
      if (r->cond.type == SM_COND_ATTR)
        {
          sb_puts(&b, "\",\"dev\":");
          sb_putjstr(&b, r->cond.dev);
          sb_puts(&b, ",\"prop\":");
          sb_putjstr(&b, sm_prop_name(r->cond.prop));
          sb_puts(&b, ",\"op\":\"");
          sb_puts(&b, sm_op_to_str(r->cond.op));
          sb_putc(&b, '"');
          sb_puts(&b, ",\"value\":");
          sb_putint(&b, r->cond.value);
        }
      else
        {
          /* TIME 保留：引擎不落地，这里仅保证序列化不丢字段 */
          sb_puts(&b, "\",\"hour\":");
          sb_putint(&b, r->cond.hour);
          sb_puts(&b, ",\"minute\":");
          sb_putint(&b, r->cond.minute);
        }
      sb_puts(&b, "},\"action\":{\"type\":\"");
      sb_puts(&b, r->action.type == SM_ACT_SET ? "set" : "beep");
      if (r->action.type == SM_ACT_SET)
        {
          sb_puts(&b, "\",\"dev\":");
          sb_putjstr(&b, r->action.dev);
          sb_puts(&b, ",\"prop\":");
          sb_putjstr(&b, sm_prop_name(r->action.prop));
          sb_puts(&b, ",\"value\":");
          sb_putint(&b, r->action.value);
        }
      else
        {
          sb_putc(&b, '"');             /* beep 无字段：补齐 type 字符串引号 */
        }
      sb_puts(&b, "}}");
    }
  sb_puts(&b, "]}");

  if (b.err != 0)
    {
      return b.err;
    }

  buf[b.len] = '\0';
  return b.len;
}

int sm_json_devices_to_str(const sm_device_t *devs, int count,
                           char *buf, int buflen)
{
  struct sm_sbuf b;
  int i;

  if (devs == NULL || buf == NULL || buflen <= 0 || count < 0)
    {
      return SM_ERR_INVALID;
    }

  b.buf = buf;
  b.cap = buflen;
  b.len = 0;
  b.err = 0;

  sb_puts(&b, "{\"version\":1,\"devices\":[");
  for (i = 0; i < count; i++)
    {
      const sm_device_t *d = &devs[i];

      if (i > 0)
        {
          sb_putc(&b, ',');
        }

      sb_puts(&b, "{\"id\":");
      sb_putjstr(&b, d->id);
      sb_puts(&b, ",\"type\":\"");
      switch (d->type)
        {
        case SM_DEV_LIGHT:
          sb_puts(&b, "light");
          break;
        case SM_DEV_SOCKET:
          sb_puts(&b, "socket");
          break;
        default:
          sb_puts(&b, "sensor");
          break;
        }
      sb_puts(&b, "\",\"on\":");
      sb_putint(&b, d->on ? 1 : 0);
      if (d->type == SM_DEV_LIGHT)
        {
          sb_puts(&b, ",\"brightness\":");
          sb_putint(&b, d->brightness);
        }
      if (d->type == SM_DEV_SENSOR)
        {
          sb_puts(&b, ",\"temp\":");
          sb_putint(&b, d->temp);
        }
      sb_putc(&b, '}');
    }
  sb_puts(&b, "]}");

  if (b.err != 0)
    {
      return b.err;
    }

  buf[b.len] = '\0';
  return b.len;
}
