/****************************************************************************
 * sim/sm_net.c — 远程控制传输层（TCP / UART，单任务全非阻塞）
 *
 * 队伍 421 / laoshuxiwantou，轨 C（协议层 + 设备模拟器）
 *
 * 职责（对外接口见 ../smarthome_internal.h）：
 *   sm_net_start_tcp(port)        监听 TCP（板内 127.0.0.1 回环 / Linux
 *                                 模拟器下与 PC 脚本互通）
 *   sm_net_start_uart(devpath,baud) 串口 8N1 raw 非阻塞透传（板端演示）
 *   sm_net_tick()                 主循环 ~50ms 调一次，内部无阻塞推进：
 *     ① TCP 非阻塞 accept（单客户端，新连接顶掉旧连接）
 *     ② 按行收包 → sm_proto_parse → set 调 sm_set_device 按返回值回 ack；
 *        get 回 evt:devices
 *     ③ 每 SM_NET_POLL_TICKS 次 tick（≈500ms，按 50ms 周期折算）轮询设备
 *        表与上次快照差量，变化字段发 evt:state
 *     ④ 规则命中事件经 sm_engine_add_event_cb（引擎第二槽）推入环形缓冲，
 *        本 tick 内转发 evt:rule（回调只置脏拷贝，发送在主流程做）
 *
 * 线程模型：无线程无锁无阻塞——socket O_NONBLOCK，termios raw + 非阻塞读，
 * 一切在 tick 里推进。发送为 best-effort：单槽续传缓冲，发不完的下个 tick
 * 接着发；通道故障只丢消息绝不阻塞——网络挂掉不影响 UI 演示。
 * 失败语义：start 失败返回负值并置 disabled，之后 tick 直接返回。
 *
 * 头文件约束：仅 POSIX socket/termios/unistd/fcntl + 标准 C 头（stdio 等）。
 ****************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "../smarthome_internal.h"
#include "sm_net_internal.h"

/****************************************************************************
 * 常量与内部单例状态
 ****************************************************************************/

#define SM_NET_LINE_MAX    256  /* 收包行缓冲上限（协议消息远小于此） */
#define SM_NET_OUT_MAX     768  /* 发送续传槽（与 devices 消息上限一致） */
#define SM_NET_POLL_TICKS  10   /* 状态差量轮询周期（≈10×50ms = 500ms） */
#define SM_NET_RULE_SLOTS  4    /* evt:rule 环形缓冲深度（链式联动排队） */

typedef enum
{
  SM_NET_MODE_DOWN = 0,     /* 未启动 / 已禁用 */
  SM_NET_MODE_TCP_LISTEN,   /* TCP 监听中（无客户端） */
  SM_NET_MODE_TCP_CONN,     /* TCP 已连接 */
  SM_NET_MODE_UART,         /* 串口透传 */
} sm_net_mode_t;

/* 待转发的规则命中事件（回调里只拷贝，发送留给 tick 主流程） */
typedef struct
{
  char      rule_id[SM_RULE_ID_LEN];
  char      rule_name[SM_NAME_LEN];
  char      dev[SM_DEV_ID_LEN];
  sm_prop_t prop;
  int       value;
} sm_net_rule_evt_t;

/* sm_engine_add_event_cb 注册进度 */
enum
{
  SM_NET_CB_IDLE = 0,       /* 尚未尝试注册 */
  SM_NET_CB_OK,             /* 已注册 */
  SM_NET_CB_DISABLED,       /* 槽位不足（NOMEM），永久跳过 evt:rule */
};

static struct
{
  sm_net_mode_t mode;
  bool disabled;                     /* start 失败 / 传输硬错误后置位 */
  int  listen_fd;                    /* TCP 监听 socket */
  int  data_fd;                      /* TCP 已连接 socket 或串口 fd */

  char line_buf[SM_NET_LINE_MAX];    /* 收包行拼接缓冲 */
  int  line_len;
  bool line_drop;                    /* 行超长，丢弃到下一个 '\n' 为止 */

  char out_buf[SM_NET_OUT_MAX];      /* 未发完的消息尾巴（单槽续传） */
  int  out_len;
  int  out_off;

  uint32_t tick_count;               /* tick 计数（≈50ms/次，用于轮询分频） */

  sm_device_t snap[SM_MAX_DEVICES];  /* 差量推送基线快照 */
  int         snap_count;
  bool        snap_valid;

  sm_net_rule_evt_t rule_ring[SM_NET_RULE_SLOTS];
  int rule_head;                     /* 环形缓冲写位置 */
  int rule_tail;                     /* 待发送读位置 */
  int rule_pending;                  /* 环中待发条数 */
  int cb_state;                      /* SM_NET_CB_* */
} g_net =
{
  .mode      = SM_NET_MODE_DOWN,
  .disabled  = true,                 /* 未 start 前视为禁用 */
  .listen_fd = -1,
  .data_fd   = -1,
};

/****************************************************************************
 * 引擎"第二槽"事件回调的 weak 兜底
 *
 * 正式实现由主集成 Agent 放在 engine 侧；这里给一个返回 SM_ERR_NOMEM 的
 * __weak 定义：engine 强符号存在时被自动覆盖；暂缺时 evt:rule 优雅降级，
 * 链接不失败、演示不中断。
 ****************************************************************************/

#if defined(__GNUC__)
__attribute__((weak))
int sm_engine_add_event_cb(sm_event_cb_t cb, void *arg)
{
  (void)cb;
  (void)arg;
  return SM_ERR_NOMEM;
}
#endif

/****************************************************************************
 * 底层收发
 ****************************************************************************/

/* 当前可用对端 fd；TCP 监听中无客户端返回 -1（此时消息直接丢弃） */
static int active_fd(void)
{
  if (g_net.mode == SM_NET_MODE_TCP_CONN || g_net.mode == SM_NET_MODE_UART)
    {
      return g_net.data_fd;
    }

  return -1;
}

/* 非阻塞写一段数据：返回已写字节数；EAGAIN/EINTR 返回 0；硬错误返回 -1。
 * 无对端时"视为已写完"（上层不会续传，消息本来就没有接收者）。 */
static int net_write(const char *buf, int len)
{
  int fd = active_fd();
  ssize_t n;
  int flags = 0;

  if (fd < 0)
    {
      return len;
    }

#ifdef MSG_NOSIGNAL
  flags = MSG_NOSIGNAL;              /* 对端断开时不触发 SIGPIPE（host 调试） */
#endif

  if (g_net.mode == SM_NET_MODE_UART)
    {
      n = write(fd, buf, (size_t)len);
    }
  else
    {
      n = send(fd, buf, (size_t)len, flags);
    }

  if (n >= 0)
    {
      return (int)n;
    }

  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
    {
      return 0;
    }

  return -1;
}

/* 续传上次未发完的消息尾巴；硬错误时丢弃尾巴（TCP 由后续 recv 收尾） */
static void net_flush_pending(void)
{
  while (g_net.out_off < g_net.out_len)
    {
      int n = net_write(g_net.out_buf + g_net.out_off,
                        g_net.out_len - g_net.out_off);

      if (n > 0)
        {
          g_net.out_off += n;
          continue;
        }

      if (n == 0)
        {
          return;                    /* 通道忙：下个 tick 继续 */
        }

      g_net.out_len = 0;             /* 硬错误：丢弃尾巴 */
      g_net.out_off = 0;
      return;
    }

  g_net.out_len = 0;
  g_net.out_off = 0;
}

/* 发送一条完整行消息（构造器输出自带 '\n'）。通道忙时把尾巴存入 out_buf
 * 续传；续传槽被占时丢弃新消息（演示速率下几乎不会发生）。 */
static void net_send_str(const char *s)
{
  int len;
  int n;

  if (s == NULL || active_fd() < 0)
    {
      return;                        /* 无对端：直接丢弃 */
    }

  if (g_net.out_off < g_net.out_len)
    {
      net_flush_pending();
      if (g_net.out_off < g_net.out_len)
        {
          printf("[NET] out busy, message dropped\n");
          return;
        }
    }

  len = (int)strlen(s);
  n   = net_write(s, len);
  if (n < 0)
    {
      return;                        /* 硬错误：交给下次 flush/recv 收尾 */
    }

  if (n < len)
    {
      if (len - n > SM_NET_OUT_MAX)
        {
          return;                    /* 放不下：丢弃（正常不会发生） */
        }

      memcpy(g_net.out_buf, s + n, (size_t)(len - n));
      g_net.out_len = len - n;
      g_net.out_off = 0;
    }
}

/* 应答 ack（ok=1 / ok=0 + err 说明） */
static void send_ack(bool ok, const char *err)
{
  net_send_str(sm_proto_build_ack(ok, err));
}

/****************************************************************************
 * 传输生命周期
 ****************************************************************************/

/* 关闭当前传输并复位会话缓冲（mode 置 DOWN；不动 disabled 标志） */
static void transport_close(void)
{
  if (g_net.listen_fd >= 0)
    {
      close(g_net.listen_fd);
      g_net.listen_fd = -1;
    }

  if (g_net.data_fd >= 0)
    {
      close(g_net.data_fd);
      g_net.data_fd = -1;
    }

  g_net.mode       = SM_NET_MODE_DOWN;
  g_net.line_len   = 0;
  g_net.line_drop  = false;
  g_net.out_len    = 0;
  g_net.out_off    = 0;
  g_net.snap_valid = false;          /* 换道后重建差量基线 */
}

/* 传输彻底禁用（start 失败 / 串口硬错误）：之后 tick 直接返回 */
static void transport_disable(void)
{
  transport_close();
  g_net.disabled = true;
}

/* 关闭当前 TCP 连接并复位会话缓冲，回到监听等待下一个客户端 */
static void conn_close(void)
{
  if (g_net.data_fd >= 0)
    {
      close(g_net.data_fd);
      g_net.data_fd = -1;
    }

  g_net.line_len = 0;
  g_net.line_drop = false;
  g_net.out_len  = 0;
  g_net.out_off  = 0;

  if (g_net.mode == SM_NET_MODE_TCP_CONN)
    {
      g_net.mode = SM_NET_MODE_TCP_LISTEN;
    }
}

/****************************************************************************
 * 收包与命令执行
 ****************************************************************************/

/* 执行一行协议命令（已去 \r\n）：
 *   set → sm_set_device（唯一状态变更入口，含规则求值），按返回值回 ack
 *   get → 回 evt:devices（全量设备表快照）
 * 空行/纯空白静默忽略（串口噪声）；畸形行丢弃并回 ack ok:0 */
static void handle_line(const char *line)
{
  sm_proto_msg_t msg;
  sm_device_t *table;
  const char *out;
  const char *p = line;
  int count = 0;
  int ret;

  while (*p == ' ' || *p == '\t')
    {
      p++;
    }

  if (*p == '\0')
    {
      return;                        /* 空行：静默忽略 */
    }

  if (sm_proto_parse(line, &msg) != SM_OK)
    {
      printf("[NET] malformed line dropped\n");
      send_ack(false, "invalid");
      return;
    }

  if (msg.cmd == SM_PROTO_CMD_GET)
    {
      table = sm_device_table(&count);
      out   = (table != NULL && count > 0)
              ? sm_proto_build_devices(table, count) : NULL;

      if (out == NULL)
        {
          send_ack(false, "error");
          return;
        }

      printf("[NET] get -> devices (%d)\n", count);
      net_send_str(out);
      return;
    }

  /* SM_PROTO_CMD_SET：成功与否以引擎返回为准（未知设备/属性不匹配等
   * 统一映射为 err:"invalid"） */
  ret = sm_set_device(msg.dev, msg.prop, msg.value);
  if (ret == SM_OK)
    {
      printf("[NET] set %s.%s=%d\n", msg.dev,
             sm_proto_prop_to_str(msg.prop), msg.value);
      send_ack(true, NULL);
    }
  else
    {
      printf("[NET] set %s.%s=%d rejected (%d)\n", msg.dev,
             sm_proto_prop_to_str(msg.prop), msg.value, ret);
      send_ack(false, "invalid");
    }
}

/* 喂入收到的字节流，按 '\n' 切行；行超长则整行丢弃并回 ack ok:0 */
static void feed_bytes(const char *buf, size_t len)
{
  size_t i;

  for (i = 0; i < len; i++)
    {
      char c = buf[i];

      if (c == '\n')
        {
          if (g_net.line_drop)
            {
              /* 超长行收尾：溢出时已回过 ack，这里只复位继续收下一行 */
              g_net.line_drop = false;
              g_net.line_len  = 0;
            }
          else
            {
              g_net.line_buf[g_net.line_len] = '\0';
              handle_line(g_net.line_buf);
              g_net.line_len = 0;
            }
        }
      else if (c == '\r')
        {
          /* CRLF 兼容：忽略 */
        }
      else if (g_net.line_drop)
        {
          /* 超长行剩余部分继续丢弃 */
        }
      else if (g_net.line_len >= SM_NET_LINE_MAX - 1)
        {
          g_net.line_drop = true;
          g_net.line_len  = 0;
          send_ack(false, "invalid");
        }
      else
        {
          g_net.line_buf[g_net.line_len++] = c;
        }
    }
}

/* 非阻塞收包泵：读到 EAGAIN 为止；TCP 对端关闭/硬错误则断开回监听，
 * 串口硬错误则禁用传输（演示不卡死，重启传输需重新 start） */
static void recv_pump(bool is_uart)
{
  char chunk[64];

  for (;;)
    {
      ssize_t n;

      if (is_uart)
        {
          n = read(g_net.data_fd, chunk, sizeof(chunk));
        }
      else
        {
          n = recv(g_net.data_fd, chunk, sizeof(chunk), 0);
        }

      if (n > 0)
        {
          feed_bytes(chunk, (size_t)n);
          continue;
        }

      if (n == 0)
        {
          if (is_uart)
            {
              break;                   /* 串口暂无数据（部分驱动返回 0） */
            }

          printf("[NET] client disconnected\n");
          conn_close();                /* TCP 对端关闭 → 回监听 */
          break;
        }

      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        {
          break;                       /* 正常：暂无数据 */
        }

      if (is_uart)
        {
          printf("[NET] uart read error (%d), transport disabled\n", errno);
          transport_disable();
        }
      else
        {
          printf("[NET] client read error (%d)\n", errno);
          conn_close();
        }

      break;
    }
}

/* 非阻塞 accept：LISTEN 时建连；CONN 时新连接顶掉旧连接（单客户端） */
static void snapshot_build(void);          /* 前置声明：接入时建差量基线 */

static void tcp_try_accept(void)
{
  int cfd = accept(g_net.listen_fd, NULL, NULL);

  if (cfd < 0)
    {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
          printf("[NET] accept failed (%d)\n", errno);
        }

      return;
    }

  if (fcntl(cfd, F_SETFL, O_NONBLOCK) < 0)
    {
      close(cfd);
      return;
    }

  if (g_net.data_fd >= 0)
    {
      printf("[NET] new client replaces old one\n");
      conn_close();                    /* 复位会话缓冲并回 LISTEN */
    }

  g_net.data_fd = cfd;
  g_net.mode    = SM_NET_MODE_TCP_CONN;
  snapshot_build();                  /* 基线=接入时刻状态，差量不丢变更 */
  printf("[NET] client connected\n");
}

/****************************************************************************
 * 状态差量推送（evt:state）
 ****************************************************************************/

/* 建立/重建差量基线快照（只记录不推送） */
static void snapshot_build(void)
{
  int count = 0;
  sm_device_t *table = sm_device_table(&count);

  if (table == NULL || count <= 0 || count > SM_MAX_DEVICES)
    {
      return;
    }

  memcpy(g_net.snap, table, (size_t)count * sizeof(sm_device_t));
  g_net.snap_count = count;
  g_net.snap_valid = true;
}

/* 与基线快照逐字段比较，变化字段各发一条 evt:state（差量）。
 * 基线在新客户端接入时立即建立（见 tcp_try_accept）：差量语义为
 * "相对该客户端接入时刻"的变化，接入前后瞬间的变更不丢；
 * 全量列表由手机侧主动 get 获取。 */
static void poll_state_diff(void)
{
  int count = 0;
  sm_device_t *table = sm_device_table(&count);
  int i;

  if (table == NULL || count <= 0 || count > SM_MAX_DEVICES)
    {
      return;
    }

  if (!g_net.snap_valid || g_net.snap_count != count)
    {
      snapshot_build();              /* UART 模式等无接入时机的场景 */
      return;
    }

  for (i = 0; i < count; i++)
    {
      sm_device_t *d = &table[i];
      sm_device_t *s = &g_net.snap[i];

      if (strcmp(s->id, d->id) != 0)
        {
          *s = *d;                     /* 表序异常（罕见）：重建该槽基线 */
          continue;
        }

      if (s->on != d->on)
        {
          net_send_str(sm_proto_build_state(d->id, SM_PROP_ON,
                                            d->on ? 1 : 0));
          s->on = d->on;
        }

      if (s->brightness != d->brightness)
        {
          net_send_str(sm_proto_build_state(d->id, SM_PROP_BRIGHTNESS,
                                            d->brightness));
          s->brightness = d->brightness;
        }

      if (s->temp != d->temp)
        {
          net_send_str(sm_proto_build_state(d->id, SM_PROP_TEMP, d->temp));
          s->temp = d->temp;
        }
    }
}

/****************************************************************************
 * 规则命中事件转发（evt:rule）
 ****************************************************************************/

/* 引擎第二槽回调（运行在引擎求值路径内）：只拷贝进环形缓冲置脏，
 * 发送留给 sm_net_tick 主流程，避免回调重入引擎/发送路径。
 * 环满丢最旧（演示关心最新命中）。 */
static void rule_event_cb(const sm_rule_t *rule, const sm_device_t *dev,
                          int value, void *arg)
{
  sm_net_rule_evt_t *evt;

  (void)arg;

  if (rule == NULL || g_net.cb_state != SM_NET_CB_OK)
    {
      return;
    }

  evt = &g_net.rule_ring[g_net.rule_head];
  g_net.rule_head = (g_net.rule_head + 1) % SM_NET_RULE_SLOTS;

  if (g_net.rule_pending >= SM_NET_RULE_SLOTS)
    {
      g_net.rule_tail = (g_net.rule_tail + 1) % SM_NET_RULE_SLOTS;
    }
  else
    {
      g_net.rule_pending++;
    }

  snprintf(evt->rule_id, sizeof(evt->rule_id), "%s", rule->id);
  snprintf(evt->rule_name, sizeof(evt->rule_name), "%s", rule->name);

  /* beep 动作没有直接目标设备：引擎兜底传观察设备快照，这里再兜底一次 */
  if (dev != NULL)
    {
      snprintf(evt->dev, sizeof(evt->dev), "%s", dev->id);
    }
  else if (rule->action.type == SM_ACT_SET)
    {
      snprintf(evt->dev, sizeof(evt->dev), "%s", rule->action.dev);
    }
  else
    {
      snprintf(evt->dev, sizeof(evt->dev), "%s", rule->cond.dev);
    }

  evt->prop  = (rule->action.type == SM_ACT_SET) ? rule->action.prop
                                                 : rule->cond.prop;
  evt->value = value;
}

/* 把环形缓冲中的待发事件逐条转发为 evt:rule */
static void rule_drain(void)
{
  while (g_net.rule_pending > 0)
    {
      sm_net_rule_evt_t *evt = &g_net.rule_ring[g_net.rule_tail];
      const char *out = sm_proto_build_rule(evt->rule_id, evt->rule_name,
                                            evt->dev, evt->prop, evt->value);

      if (out == NULL)
        {
          return;                      /* 构造失败：保留剩余下轮再试 */
        }

      net_send_str(out);
      g_net.rule_tail = (g_net.rule_tail + 1) % SM_NET_RULE_SLOTS;
      g_net.rule_pending--;
    }
}

/* 惰性注册引擎第二槽事件回调：首 tick 尝试；SM_ERR_NOMEM 永久放弃
 * （evt:rule 降级不致命）；其他错误视为引擎未就绪，下个 tick 重试 */
static void rule_register(void)
{
  int ret;

  if (g_net.cb_state != SM_NET_CB_IDLE)
    {
      return;
    }

  ret = sm_engine_add_event_cb(rule_event_cb, NULL);

  if (ret == SM_OK)
    {
      g_net.cb_state = SM_NET_CB_OK;
      printf("[NET] rule event slot registered\n");
    }
  else if (ret == SM_ERR_NOMEM)
    {
      g_net.cb_state = SM_NET_CB_DISABLED;
      printf("[NET] rule event slot unavailable, evt:rule disabled\n");
    }
}

/****************************************************************************
 * 传输启动（对外接口）
 ****************************************************************************/

static speed_t baud_to_speed(int baud)
{
  switch (baud)
    {
    case 9600:    return B9600;
    case 19200:   return B19200;
    case 38400:   return B38400;
    case 57600:   return B57600;
    case 230400:  return B230400;
    case 460800:  return B460800;
    case 921600:  return B921600;
    case 1000000: return B1000000;
    case 115200:
    default:      return B115200;    /* 未识别波特率回退 115200 */
    }
}

int sm_net_start_tcp(int port)
{
  struct sockaddr_in addr;
  int fd;
  int one = 1;

  transport_close();                   /* tcp/uart 单活跃：顶掉旧传输 */
  g_net.disabled = false;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      printf("[NET] tcp socket failed (%d)\n", errno);
      transport_disable();
      return -1;
    }

  /* SO_REUSEADDR：演示中反复重启面板免 TIME_WAIT 卡端口 */
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port        = htons((uint16_t)port);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      listen(fd, 1) < 0 ||
      fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
    {
      printf("[NET] tcp bind/listen on port %d failed (%d)\n", port, errno);
      close(fd);
      transport_disable();
      return -1;
    }

  g_net.listen_fd = fd;
  g_net.data_fd   = -1;
  g_net.mode      = SM_NET_MODE_TCP_LISTEN;

  printf("[NET] tcp listening on port %d\n", port);
  return SM_OK;
}

int sm_net_start_uart(const char *devpath, int baud)
{
  struct termios tio;
  speed_t speed;
  int fd;

  if (devpath == NULL || devpath[0] == '\0')
    {
      return -1;
    }

  transport_close();                   /* tcp/uart 单活跃：顶掉旧传输 */
  g_net.disabled = false;

  fd = open(devpath, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0)
    {
      printf("[NET] uart open %s failed (%d)\n", devpath, errno);
      transport_disable();
      return -1;
    }

  memset(&tio, 0, sizeof(tio));
  if (tcgetattr(fd, &tio) < 0)
    {
      printf("[NET] uart tcgetattr %s failed (%d)\n", devpath, errno);
      close(fd);
      transport_disable();
      return -1;
    }

  /* 8N1 raw：关回显/规范模式/信号字符/流控/输出翻译；VMIN=0 VTIME=0，
   * 配合 O_NONBLOCK 保证读永不阻塞 */
  tio.c_iflag &= ~(unsigned)(IGNBRK | BRKINT | PARMRK | ISTRIP |
                             INLCR | IGNCR | ICRNL | IXON);
  tio.c_oflag &= ~(unsigned)OPOST;
  tio.c_lflag &= ~(unsigned)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
  tio.c_cflag &= ~(unsigned)(CSIZE | PARENB | CSTOPB);
  tio.c_cflag |= (unsigned)CS8;
  tio.c_cc[VMIN]  = 0;
  tio.c_cc[VTIME] = 0;

  speed = baud_to_speed(baud);
  cfsetispeed(&tio, speed);
  cfsetospeed(&tio, speed);

  if (tcsetattr(fd, TCSANOW, &tio) < 0)
    {
      printf("[NET] uart tcsetattr %s failed (%d)\n", devpath, errno);
      close(fd);
      transport_disable();
      return -1;
    }

  tcflush(fd, TCIOFLUSH);              /* 丢掉上电残留字节 */

  g_net.data_fd   = fd;
  g_net.listen_fd = -1;
  g_net.mode      = SM_NET_MODE_UART;

  printf("[NET] uart %s @ %d (8N1, nonblocking)\n", devpath, baud);
  return SM_OK;
}

void sm_net_tick(void)
{
  if (g_net.disabled || g_net.mode == SM_NET_MODE_DOWN)
    {
      return;                          /* 未启动/已禁用：演示不受影响 */
    }

  g_net.tick_count++;

  rule_register();                     /* 惰性注册规则事件第二槽 */
  net_flush_pending();                 /* 续传上次未发完的消息 */

  if (g_net.mode == SM_NET_MODE_TCP_LISTEN ||
      g_net.mode == SM_NET_MODE_TCP_CONN)
    {
      tcp_try_accept();                /* 建连 / 新连接顶掉旧的 */
    }

  if (g_net.mode == SM_NET_MODE_TCP_CONN)
    {
      recv_pump(false);
    }
  else if (g_net.mode == SM_NET_MODE_UART)
    {
      recv_pump(true);
    }

  rule_drain();                        /* evt:rule 转发（先于差量轮询） */

  if ((g_net.tick_count % SM_NET_POLL_TICKS) == 0)
    {
      poll_state_diff();               /* ≈500ms 一次差量推送 */
    }
}
