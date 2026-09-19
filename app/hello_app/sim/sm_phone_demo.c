/****************************************************************************
 * app/hello_app/sim/sm_phone_demo.c — 板内"手机"回环演示客户端（集成者维护）
 *
 * 用途：D12X 无 WiFi/以太网驱动（no-go 决议），局域网直连待硬件打通。
 * 该客户端经 127.0.0.1 TCP 回环连本板 sm_net 服务端，按脚本发送与
 * PC 侧 sim_device_pc.py 完全相同的 {"cmd":"set"} 消息——用真实
 * socket+JSON 代码路径呈现"手机点一下、面板同步"的演示效果。
 *
 * 用法：NSH 下 `smarthome --phone`（面板与"手机"同进程内两个角色，
 * 网络栈 CONFIG_NET+CONFIG_NET_LOOPBACK=y）。单任务非阻塞实现：
 * connect/send 全在 tick 内推进，卡住只丢消息不阻塞主循环。
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "smarthome_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define PHONE_DEMO_PORT       9000
#define PHONE_STEP_MS         2000    /* 消息间隔 */
#define PHONE_REPEAT_MS       60000   /* 一轮播完后到下一轮的间隔 */
#define PHONE_CONNECT_MS      5000    /* 启动到首连的等待 */

/* 脚本消息（与 sim_device_pc.py 的 demo 序列一致） */
static const char *g_phone_script[] =
{
  "{\"cmd\":\"set\",\"dev\":\"light1\",\"prop\":\"on\",\"value\":1}\n",
  "{\"cmd\":\"set\",\"dev\":\"light1\",\"prop\":\"brightness\",\"value\":80}\n",
  "{\"cmd\":\"set\",\"dev\":\"socket1\",\"prop\":\"on\",\"value\":1}\n",
  "{\"cmd\":\"set\",\"dev\":\"light2\",\"prop\":\"on\",\"value\":1}\n",
  "{\"cmd\":\"set\",\"dev\":\"light2\",\"prop\":\"on\",\"value\":0}\n",
  "{\"cmd\":\"set\",\"dev\":\"light1\",\"prop\":\"on\",\"value\":0}\n",
};

#define PHONE_SCRIPT_LEN  (sizeof(g_phone_script) / sizeof(g_phone_script[0]))

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct phone_state_s
{
  bool     enabled;
  int      sockfd;
  uint32_t next_ms;      /* clock() 节拍：下次动作时间 */
  int      step;         /* 当前脚本步（-1 = 待重连） */
  uint32_t last_clock;
};

static struct phone_state_s g_phone;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t phone_clock_ms(void)
{
  return (uint32_t)(clock() / (CLOCKS_PER_SEC / 1000));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void sm_phone_demo_start(uint16_t port)
{
  (void)port;
  memset(&g_phone, 0, sizeof(g_phone));
  g_phone.enabled = true;
  g_phone.sockfd = -1;
  g_phone.step = -1;
  g_phone.next_ms = phone_clock_ms() + PHONE_CONNECT_MS;
  printf("[SM] phone demo client armed (loopback, first run in %d ms)\n",
         PHONE_CONNECT_MS);
}

void sm_phone_demo_tick(void)
{
  struct sockaddr_in addr;
  uint32_t now;
  int ret;

  if (!g_phone.enabled)
    {
      return;
    }

  now = phone_clock_ms();
  if (now < g_phone.next_ms)
    {
      return;
    }

  if (g_phone.step < 0)
    {
      /* 无连接：建一个非阻塞 socket 发起回环连接 */
      if (g_phone.sockfd < 0)
        {
          g_phone.sockfd = socket(AF_INET, SOCK_STREAM, 0);
          if (g_phone.sockfd < 0)
            {
              g_phone.enabled = false;
              printf("[SM] phone demo: no socket, disabled\n");
              return;
            }

          memset(&addr, 0, sizeof(addr));
          addr.sin_family = AF_INET;
          addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
          addr.sin_port = htons(PHONE_DEMO_PORT);

          ret = connect(g_phone.sockfd, (struct sockaddr *)&addr,
                        sizeof(addr));
          if (ret < 0 && errno != EINPROGRESS)
            {
              close(g_phone.sockfd);
              g_phone.sockfd = -1;
              g_phone.next_ms = now + PHONE_CONNECT_MS;
              return;
            }

          g_phone.last_clock = now;
          g_phone.step = 0;
          g_phone.next_ms = now + PHONE_STEP_MS;
          printf("[SM] phone demo: connected to panel\n");
        }

      return;
    }

  /* 有连接：到点发一条（小报文走回环， Practically 不会阻塞） */
  (void)send(g_phone.sockfd, g_phone_script[g_phone.step],
             strlen(g_phone_script[g_phone.step]), MSG_DONTWAIT);
  g_phone.step++;

  if (g_phone.step >= (int)PHONE_SCRIPT_LEN)
    {
      /* 一轮播完：断开重连，隔 PHONE_REPEAT_MS 再来一轮 */
      close(g_phone.sockfd);
      g_phone.sockfd = -1;
      g_phone.step = -1;
      g_phone.next_ms = now + PHONE_REPEAT_MS;
      printf("[SM] phone demo: round done, repeat in %d ms\n",
             PHONE_REPEAT_MS);
    }
  else
    {
      g_phone.next_ms = now + PHONE_STEP_MS;
    }
}
