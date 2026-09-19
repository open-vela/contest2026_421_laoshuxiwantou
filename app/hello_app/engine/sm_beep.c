/****************************************************************************
 * engine/sm_beep.c — 提示音播放（/data/beep.wav → NuttX audio 框架）
 *
 * 队伍 421 / laoshuxiwantou，2026-09-19
 *
 * 实现内部接口 sm_beep_play()（见 smarthome_internal.h）：
 *   - CONFIG_AUDIO 未使能：整模块空壳，直接返回 SM_ERR_UNSUPPORTED
 *   - 读 /data/beep.wav：校验 44 字节 WAV 头（RIFF/WAVE/fmt /data），
 *     预缓冲整段 PCM（≤64KB）到静态内存
 *   - 走 NuttX audio 框架标准调用序列（参考 apps/system/nxplayer）：
 *     open /dev/audio/xx → AUDIOIOC_GETCAPS 校验 → AUDIOIOC_RESERVE →
 *     AUDIOIOC_CONFIGURE(16k/16bit/mono PCM) → AUDIOIOC_ALLOCBUFFER →
 *     AUDIOIOC_ENQUEUEBUFFER(AUDIO_APB_FINAL) → AUDIOIOC_START
 *   - fire-and-forget：START 后立即返回，不等待播完、不注册消息队列
 *   - 任何一步失败立即返回负值，绝不阻塞/崩溃调用方
 *
 * 会话复用：apb 缓冲首次播放时一次性分配，之后重复使用；若上次播放
 * 可能仍在进行，先 AUDIOIOC_STOP 让驱动回收缓冲再重新填充入队
 * （NuttX 音频驱动在 stop 路径同步 dequeue 挂起缓冲）。
 * 本文件是 engine 轨唯一允许包含 nuttx/ 专有头的文件。
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_AUDIO

#include <sys/ioctl.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/audio/audio.h>

#include "smarthome_types.h"
#include "smarthome_storage.h"
#include "smarthome_internal.h"

#define SM_BEEP_PCM_MAX   (64 * 1024)     /* 预缓冲上限（契约 ≤64KB） */
#define SM_BEEP_WAV_HDR   44              /* 冻结：44 字节 WAV 头 */
#define SM_BEEP_RATE      16000           /* 冻结：16k 采样率 */
#define SM_BEEP_BPS       16              /* 冻结：16bit 位宽 */

/* 单个 apb 缓冲可申请的最大字节数：CONFIG_AUDIO_LARGE_BUFFERS 未开时
 * apb_samp_t 为 16 位（nmaxbytes/numbytes 表达不了 64KB），此时整段
 * 分配按 65535 上限截断（超出部分不播放，提示音场景可接受） */
#define SM_BEEP_APB_MAX \
  (((int)(apb_samp_t)-1 > SM_BEEP_PCM_MAX) ? SM_BEEP_PCM_MAX \
                                           : (int)(apb_samp_t)-1)

/* 播放会话状态（单任务访问，无锁） */
static int                 g_dev_fd = -1;     /* /dev/audio/xx 句柄 */
static struct ap_buffer_s *g_apb = NULL;      /* 复用的音频管道缓冲 */
static bool                g_started = false; /* 上次 START 可能仍在播 */

#ifdef CONFIG_AUDIO_MULTI_SESSION
static void               *g_session = NULL;
#endif

/* PCM 预缓冲（整段一次性读入） */
static uint8_t g_pcm[SM_BEEP_PCM_MAX];

/****************************************************************************
 * WAV 读取
 ****************************************************************************/

static uint16_t le16(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
  return (uint32_t)le16(p) | ((uint32_t)le16(p + 2) << 16);
}

/* 读 /data/beep.wav，跳过 44 字节头，返回 PCM 字节数或负错误码 */
static int beep_load_pcm(uint8_t *buf, int cap)
{
  FILE *fp;
  uint8_t hdr[SM_BEEP_WAV_HDR];
  uint32_t data_len;
  int n;

  fp = fopen(SM_BEEP_PATH, "rb");
  if (fp == NULL)
    {
      return -ENOENT;                   /* 文件缺失：静默跳过 */
    }

  if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr))
    {
      fclose(fp);
      return -EIO;
    }

  /* 冻结格式：44 字节头，RIFF/WAVE + "fmt " + "data" 固定排布 */
  if (memcmp(&hdr[0], "RIFF", 4) != 0 ||
      memcmp(&hdr[8], "WAVE", 4) != 0 ||
      memcmp(&hdr[12], "fmt ", 4) != 0 ||
      memcmp(&hdr[36], "data", 4) != 0)
    {
      fclose(fp);
      return -EINVAL;
    }

  /* data 块长度字段异常（0 / 谎报）时按缓冲上限尽量读取 */
  data_len = le32(&hdr[40]);
  if (data_len == 0 || data_len > (uint32_t)cap)
    {
      data_len = (uint32_t)cap;
    }

  n = (int)fread(buf, 1, (size_t)data_len, fp);
  fclose(fp);

  if (n <= 0)
    {
      return -EIO;
    }

  return n;
}

/****************************************************************************
 * audio 设备
 ****************************************************************************/

/* 扫描 /dev/audio 目录，返回首个可用的 audio 设备句柄（nxplayer 同款
 * 探测方式：open 后用 AUDIOIOC_GETCAPS 查询校验） */
static int beep_open_device(void)
{
  struct dirent *ent;
  struct audio_caps_s caps;
  char path[32];
  DIR *dirp;
  int fd;

  dirp = opendir("/dev/audio");
  if (dirp == NULL)
    {
      return -ENOENT;                   /* CONFIG_AUDIO 未注册设备节点 */
    }

  while ((ent = readdir(dirp)) != NULL)
    {
      if (ent->d_name[0] == '.')
        {
          continue;
        }

      snprintf(path, sizeof(path), "/dev/audio/%s", ent->d_name);
      fd = open(path, O_RDWR);
      if (fd < 0)
        {
          continue;
        }

      memset(&caps, 0, sizeof(caps));
      caps.ac_len = sizeof(caps);
      caps.ac_type = AUDIO_TYPE_QUERY;
      caps.ac_subtype = AUDIO_TYPE_QUERY;
      if (ioctl(fd, AUDIOIOC_GETCAPS, (unsigned long)&caps) == caps.ac_len)
        {
          closedir(dirp);
          return fd;
        }

      close(fd);
    }

  closedir(dirp);
  return -ENODEV;
}

/* 会话初始化：开设备 → RESERVE → CONFIGURE → ALLOCBUFFER。
 * 成功返回 SM_OK；失败已做现场清理并返回负值。 */
static int beep_session_init(int pcm_size)
{
  struct audio_caps_desc_s cap_desc;
  struct audio_buf_desc_s buf_desc;
  struct ap_buffer_info_s buf_info;
  int ret;

  g_dev_fd = beep_open_device();
  if (g_dev_fd < 0)
    {
      return g_dev_fd;
    }

#ifdef CONFIG_AUDIO_MULTI_SESSION
  ret = ioctl(g_dev_fd, AUDIOIOC_RESERVE, (unsigned long)&g_session);
#else
  ret = ioctl(g_dev_fd, AUDIOIOC_RESERVE, 0);
#endif
  if (ret < 0)
    {
      ret = -errno;
      close(g_dev_fd);
      g_dev_fd = -1;
      return ret;
    }

  /* 配置输出格式：16k / 16bit / mono，本机已剥离 WAV 头，提交裸 PCM */
  memset(&cap_desc, 0, sizeof(cap_desc));
#ifdef CONFIG_AUDIO_MULTI_SESSION
  cap_desc.session = g_session;
#endif
  cap_desc.caps.ac_len            = sizeof(struct audio_caps_s);
  cap_desc.caps.ac_type           = AUDIO_TYPE_OUTPUT;
  cap_desc.caps.ac_subtype        = AUDIO_FMT_PCM;
  cap_desc.caps.ac_channels       = 1;
  cap_desc.caps.ac_chmap          = 0;
  cap_desc.caps.ac_controls.hw[0] = SM_BEEP_RATE;
  cap_desc.caps.ac_controls.b[2]  = SM_BEEP_BPS;
  cap_desc.caps.ac_controls.b[3]  = 0;

  if (ioctl(g_dev_fd, AUDIOIOC_CONFIGURE, (unsigned long)&cap_desc) < 0)
    {
      ret = -errno;
      close(g_dev_fd);
      g_dev_fd = -1;
      return ret;
    }

  /* 优先按整段 PCM 上限分配单个缓冲；失败退回驱动建议大小（播放
   * 截断，提示音场景可接受） */
  memset(&buf_desc, 0, sizeof(buf_desc));
#ifdef CONFIG_AUDIO_MULTI_SESSION
  buf_desc.session = g_session;
#endif
  buf_desc.numbytes  = (apb_samp_t)SM_BEEP_APB_MAX;
  buf_desc.u.pbuffer = &g_apb;

  if (ioctl(g_dev_fd, AUDIOIOC_ALLOCBUFFER, (unsigned long)&buf_desc)
      != (int)sizeof(buf_desc))
    {
      memset(&buf_info, 0, sizeof(buf_info));
#if defined(CONFIG_AUDIO_LARGE_BUFFERS)
      if (ioctl(g_dev_fd, AUDIOIOC_GETBUFFERINFO, (unsigned long)&buf_info)
          != OK || buf_info.buffer_size == 0 ||
          (int)buf_info.buffer_size > SM_BEEP_APB_MAX)
#else
      /* 16 位 apb_samp_t：长度上限天然 ≤ 65535 = APB_MAX，无需比对 */
      if (ioctl(g_dev_fd, AUDIOIOC_GETBUFFERINFO, (unsigned long)&buf_info)
          != OK || buf_info.buffer_size == 0)
#endif
        {
          buf_info.buffer_size = 8192;
        }

      memset(&buf_desc, 0, sizeof(buf_desc));
#ifdef CONFIG_AUDIO_MULTI_SESSION
      buf_desc.session = g_session;
#endif
      buf_desc.numbytes  = buf_info.buffer_size;
      buf_desc.u.pbuffer = &g_apb;

      if (ioctl(g_dev_fd, AUDIOIOC_ALLOCBUFFER, (unsigned long)&buf_desc)
          != (int)sizeof(buf_desc))
        {
          ret = -ENOMEM;
          close(g_dev_fd);
          g_dev_fd = -1;
          return ret;
        }

      if (pcm_size > (int)buf_info.buffer_size)
        {
          pcm_size = (int)buf_info.buffer_size;
        }
    }

  if (pcm_size > SM_BEEP_APB_MAX)
    {
      pcm_size = SM_BEEP_APB_MAX;       /* 16 位 apb_samp_t 下截断 */
    }

  return pcm_size;                      /* >0：实际可播放的 PCM 字节数 */
}

/****************************************************************************
 * 对外 API
 ****************************************************************************/

int sm_beep_play(void)
{
  struct audio_buf_desc_s buf_desc;
  int pcm_size;
  int ret;

  /* 1. 读 WAV → 预缓冲整段 PCM，任何失败立即返回 */
  pcm_size = beep_load_pcm(g_pcm, sizeof(g_pcm));
  if (pcm_size < 0)
    {
      return pcm_size;
    }

  /* 2. 首次调用建立会话；已建立则检查缓冲容量是否够本段 PCM */
  if (g_dev_fd < 0)
    {
      ret = beep_session_init(pcm_size);
      if (ret < 0)
        {
          return ret;
        }

      if (ret > 0 && ret < pcm_size)
        {
          pcm_size = ret;               /* 退回驱动建议大小时截断 */
        }
    }
  else if (g_apb != NULL && pcm_size > (int)g_apb->nmaxbytes)
    {
      pcm_size = (int)g_apb->nmaxbytes; /* 缓冲不够则截断 */
    }

  /* 3. 上次播放可能仍在进行：先 STOP 让驱动同步回收缓冲再复用 */
  if (g_started)
    {
#ifdef CONFIG_AUDIO_MULTI_SESSION
      (void)ioctl(g_dev_fd, AUDIOIOC_STOP, (unsigned long)g_session);
#else
      (void)ioctl(g_dev_fd, AUDIOIOC_STOP, 0);
#endif
      g_started = false;
    }

  /* 4. 填缓冲 → 一次性整段入队 → START 后立即返回（不等待播完） */
  if (g_apb == NULL)
    {
      return -ENODEV;
    }

  memcpy(g_apb->samp, g_pcm, (size_t)pcm_size);
  g_apb->nbytes   = (apb_samp_t)pcm_size;
  g_apb->curbyte  = 0;
  g_apb->nsamples = 0;
  g_apb->flags    = AUDIO_APB_FINAL;    /* 单缓冲即流末尾 */

  memset(&buf_desc, 0, sizeof(buf_desc));
#ifdef CONFIG_AUDIO_MULTI_SESSION
  buf_desc.session = g_session;
#endif
  buf_desc.numbytes  = g_apb->nbytes;
  buf_desc.u.buffer  = g_apb;

  if (ioctl(g_dev_fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&buf_desc) < 0)
    {
      return -errno;
    }

#ifdef CONFIG_AUDIO_MULTI_SESSION
  if (ioctl(g_dev_fd, AUDIOIOC_START, (unsigned long)g_session) < 0)
#else
  if (ioctl(g_dev_fd, AUDIOIOC_START, 0) < 0)
#endif
    {
      return -errno;
    }

  g_started = true;
  return SM_OK;
}

#else /* !CONFIG_AUDIO */

#include "smarthome_types.h"
#include "smarthome_internal.h"

int sm_beep_play(void)
{
  return SM_ERR_UNSUPPORTED;            /* 音频未使能：静默跳过 */
}

#endif /* CONFIG_AUDIO */
