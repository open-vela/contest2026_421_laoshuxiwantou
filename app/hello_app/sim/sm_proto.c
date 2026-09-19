/****************************************************************************
 * sim/sm_proto.c — 面板↔手机 行式 JSON 协议（手写小解析器 + 构造器）
 *
 * 队伍 421 / laoshuxiwantou，轨 C（协议层 + 设备模拟器）
 *
 * 协议（冻结，详见 sim/protocol.md）：每条消息一行 UTF-8 JSON，'\n' 结尾。
 *   手机→面板：{"cmd":"set","dev":"light1","prop":"on","value":1}
 *              {"cmd":"get"}
 *   面板→手机：{"evt":"ack","ok":1} / {"evt":"ack","ok":0,"err":"invalid"}
 *              {"evt":"state","dev":"light1","prop":"on","value":1}
 *              {"evt":"rule","id":"r1","name":"..","dev":"light1",
 *               "prop":"on","value":1}
 *              {"evt":"devices","devices":[{"id":"light1","type":"light",
 *               "on":1,"brightness":60},...]}
 *
 * 不引第三方 JSON 库，也不复用 sm_json_*（那是规则 schema 定制的）：
 * 这里按"找 key 读值"的固定形状提取约百行，鲁棒目标——畸形输入一律
 * 判失败，绝不越界；未知字段忽略；字符串做最小转义/反转义。
 * 构造器输出自带结尾 '\n'（传输层整行直发），写入各自独立静态缓冲。
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sm_net_internal.h"

/****************************************************************************
 * JSON 值定位（只做"找 key 读值"，不做完整 JSON 文法）
 ****************************************************************************/

static const char *skip_ws(const char *p)
{
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
    {
      p++;
    }

  return p;
}

/* 在 json 中定位 "key" 对应值的起点，返回值首字符指针；找不到返回 NULL。
 * key 必须以 "key" 完整带引号出现（避免 "id" 误配 "ident"），其后为
 * ':' + 值；本 key 匹配失败（或值缺失）则跳过该字符串继续向后找。 */
static const char *json_locate(const char *json, const char *key)
{
  size_t klen = strlen(key);
  const char *p = json;

  while ((p = strchr(p, '"')) != NULL)
    {
      p++;                                    /* 越过开引号 */

      if (strncmp(p, key, klen) == 0 && p[klen] == '"')
        {
          const char *v = skip_ws(p + klen + 1);

          if (*v == ':')
            {
              v = skip_ws(v + 1);
              if (*v != '\0')
                {
                  return v;
                }
            }
        }

      /* 跳过当前字符串到收尾引号（处理 \ 转义），避免值内容里的引号
       * 被误当成下一个 key 的开头 */
      while (*p != '\0' && *p != '"')
        {
          if (*p == '\\' && p[1] != '\0')
            {
              p++;
            }

          p++;
        }

      if (*p == '\0')
        {
          break;
        }

      p++;                                    /* 越过收尾引号 */
    }

  return NULL;
}

/* 读字符串值（v 指向开引号）：处理 \" \\ 转义取字面字符，其余转义序列
 * 原样保留反斜杠+字符（协议内不会出现，兜底）；超长截断；未闭合判失败 */
static bool json_get_string(const char *v, char *out, int outlen)
{
  int n = 0;

  if (*v != '"')
    {
      return false;
    }

  v++;

  while (*v != '"')
    {
      char c;

      if (*v == '\0')
        {
          return false;                       /* 字符串未闭合 */
        }

      if (*v == '\\' && v[1] != '\0')
        {
          v++;                                /* 跳过转义符取下一字符 */
        }

      c = *v++;
      if (n < outlen - 1)
        {
          out[n++] = c;
        }
    }

  out[n] = '\0';
  return true;
}

/* 读数值：接受数字（strtol 前缀解析）与 true/false 布尔；字符串数字
 * 不接受（防 "value":"1" 之类的协议误用静默通过） */
static bool json_get_int(const char *v, int *out)
{
  char *end;
  long n;

  if (strncmp(v, "true", 4) == 0)
    {
      *out = 1;
      return true;
    }

  if (strncmp(v, "false", 5) == 0)
    {
      *out = 0;
      return true;
    }

  if (*v == '"')
    {
      return false;
    }

  n = strtol(v, &end, 10);
  if (end == v)
    {
      return false;
    }

  *out = (int)n;
  return true;
}

/* 组合：定位 key 并读字符串 */
static bool json_str_field(const char *json, const char *key,
                           char *out, int outlen)
{
  const char *v = json_locate(json, key);

  return v != NULL && json_get_string(v, out, outlen);
}

/* 组合：定位 key 并读数值 */
static bool json_int_field(const char *json, const char *key, int *out)
{
  const char *v = json_locate(json, key);

  return v != NULL && json_get_int(v, out);
}

/****************************************************************************
 * 构造辅助：字符串转义（规则名来自 UI 输入，防御引号/控制字符）
 ****************************************************************************/

/* 把 src 包成带引号的 JSON 字符串写入 dst：转义 " 与 \，0x20 以下控制
 * 字符丢弃；截断安全（空间不足时收尾仍合法） */
static void json_quote(char *dst, int dstlen, const char *src)
{
  int n = 0;

  if (dst == NULL || dstlen < 3)
    {
      return;
    }

  dst[n++] = '"';

  while (*src != '\0' && n < dstlen - 3)
    {
      char c = *src++;

      if (c == '"' || c == '\\')
        {
          dst[n++] = '\\';
          dst[n++] = c;
        }
      else if ((unsigned char)c >= 0x20)
        {
          dst[n++] = c;
        }
    }

  dst[n++] = '"';
  dst[n]   = '\0';
}

/****************************************************************************
 * 枚举映射（与 smarthome_storage.h 冻结 schema 一致）
 ****************************************************************************/

int sm_proto_prop_from_str(const char *s, sm_prop_t *prop)
{
  if (s == NULL || prop == NULL)
    {
      return SM_ERR_INVALID;
    }

  if (strcmp(s, "on") == 0)
    {
      *prop = SM_PROP_ON;
    }
  else if (strcmp(s, "brightness") == 0)
    {
      *prop = SM_PROP_BRIGHTNESS;
    }
  else if (strcmp(s, "temp") == 0)
    {
      *prop = SM_PROP_TEMP;
    }
  else
    {
      return SM_ERR_INVALID;
    }

  return SM_OK;
}

const char *sm_proto_prop_to_str(sm_prop_t prop)
{
  switch (prop)
    {
    case SM_PROP_ON:
      return "on";
    case SM_PROP_BRIGHTNESS:
      return "brightness";
    case SM_PROP_TEMP:
      return "temp";
    default:
      return "unknown";
    }
}

const char *sm_proto_type_to_str(sm_dev_type_t type)
{
  switch (type)
    {
    case SM_DEV_LIGHT:
      return "light";
    case SM_DEV_SOCKET:
      return "socket";
    case SM_DEV_SENSOR:
      return "sensor";
    default:
      return "unknown";
    }
}

/****************************************************************************
 * 行解析
 ****************************************************************************/

int sm_proto_parse(const char *line, sm_proto_msg_t *msg)
{
  char cmd[16];
  char prop[16];

  if (line == NULL || msg == NULL)
    {
      return SM_ERR_INVALID;
    }

  memset(msg, 0, sizeof(*msg));

  /* {"cmd":".."} 为必要骨架；get 只认 cmd，其余字段忽略 */
  if (!json_str_field(line, "cmd", cmd, sizeof(cmd)))
    {
      return SM_ERR_INVALID;
    }

  if (strcmp(cmd, "get") == 0)
    {
      msg->cmd = SM_PROTO_CMD_GET;
      return SM_OK;
    }

  if (strcmp(cmd, "set") != 0)
    {
      return SM_ERR_INVALID;
    }

  /* set 四要素齐全才算合法：cmd/dev/prop/value；缺一或值畸形 → INVALID */
  msg->cmd = SM_PROTO_CMD_SET;

  if (!json_str_field(line, "dev", msg->dev, sizeof(msg->dev)) ||
      msg->dev[0] == '\0')
    {
      return SM_ERR_INVALID;
    }

  if (!json_str_field(line, "prop", prop, sizeof(prop)) ||
      sm_proto_prop_from_str(prop, &msg->prop) != SM_OK)
    {
      return SM_ERR_INVALID;
    }

  msg->has_prop = true;

  if (!json_int_field(line, "value", &msg->value))
    {
      return SM_ERR_INVALID;
    }

  return SM_OK;
}

/****************************************************************************
 * 构造器（各自独立静态缓冲，输出自带 '\n'）
 ****************************************************************************/

const char *sm_proto_build_ack(bool ok, const char *err)
{
  static char buf[96];

  if (ok)
    {
      snprintf(buf, sizeof(buf), "{\"evt\":\"ack\",\"ok\":1}\n");
    }
  else
    {
      if (err == NULL || *err == '\0')
        {
          err = "invalid";
        }

      snprintf(buf, sizeof(buf),
               "{\"evt\":\"ack\",\"ok\":0,\"err\":\"%s\"}\n", err);
    }

  return buf;
}

const char *sm_proto_build_state(const char *dev, sm_prop_t prop, int value)
{
  static char buf[128];
  char devq[SM_DEV_ID_LEN * 2 + 2];

  if (dev == NULL)
    {
      dev = "";
    }

  json_quote(devq, sizeof(devq), dev);
  snprintf(buf, sizeof(buf),
           "{\"evt\":\"state\",\"dev\":%s,\"prop\":\"%s\",\"value\":%d}\n",
           devq, sm_proto_prop_to_str(prop), value);

  return buf;
}

const char *sm_proto_build_rule(const char *rule_id, const char *rule_name,
                                const char *dev, sm_prop_t prop, int value)
{
  static char buf[224];
  char idq[SM_RULE_ID_LEN * 2 + 2];
  char nameq[SM_NAME_LEN * 2 + 2];
  char devq[SM_DEV_ID_LEN * 2 + 2];

  json_quote(idq, sizeof(idq), rule_id != NULL ? rule_id : "");
  json_quote(nameq, sizeof(nameq), rule_name != NULL ? rule_name : "");
  json_quote(devq, sizeof(devq), dev != NULL ? dev : "");

  snprintf(buf, sizeof(buf),
           "{\"evt\":\"rule\",\"id\":%s,\"name\":%s,\"dev\":%s,"
           "\"prop\":\"%s\",\"value\":%d}\n",
           idq, nameq, devq, sm_proto_prop_to_str(prop), value);

  return buf;
}

const char *sm_proto_build_devices(const sm_device_t *devs, int count)
{
  static char buf[768];               /* 8 设备 × ~75B + 头尾，余量充足 */
  int n;
  int i;

  if (devs == NULL || count < 0)
    {
      return NULL;
    }

  n = snprintf(buf, sizeof(buf), "{\"evt\":\"devices\",\"devices\":[");
  if (n < 0 || n >= (int)sizeof(buf))
    {
      return NULL;
    }

  for (i = 0; i < count; i++)
    {
      const sm_device_t *d = &devs[i];
      char idq[SM_DEV_ID_LEN * 2 + 2];
      char body[96];
      int  m;

      json_quote(idq, sizeof(idq), d->id);

      /* 字段与 /data/devices.json schema 一致：light 带 on+brightness，
       * socket 带 on，sensor 带 temp（temp 单位 0.1°C） */
      switch (d->type)
        {
        case SM_DEV_LIGHT:
          m = snprintf(body, sizeof(body),
                       "{\"id\":%s,\"type\":\"light\",\"on\":%d,"
                       "\"brightness\":%d}",
                       idq, d->on ? 1 : 0, d->brightness);
          break;

        case SM_DEV_SOCKET:
          m = snprintf(body, sizeof(body),
                       "{\"id\":%s,\"type\":\"socket\",\"on\":%d}",
                       idq, d->on ? 1 : 0);
          break;

        case SM_DEV_SENSOR:
          m = snprintf(body, sizeof(body),
                       "{\"id\":%s,\"type\":\"sensor\",\"temp\":%d}",
                       idq, d->temp);
          break;

        default:
          m = snprintf(body, sizeof(body), "{\"id\":%s}", idq);
          break;
        }

      if (m < 0 || m >= (int)sizeof(body))
        {
          return NULL;
        }

      if (n + (i > 0 ? 1 : 0) + m >= (int)sizeof(buf) - 3)
        {
          return NULL;                  /* 装不下整条：整体放弃 */
        }

      if (i > 0)
        {
          buf[n++] = ',';
        }

      memcpy(buf + n, body, (size_t)m);
      n += m;
    }

  buf[n++] = ']';
  buf[n++] = '}';
  buf[n++] = '\n';
  buf[n]   = '\0';

  return buf;
}
