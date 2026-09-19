/****************************************************************************
 * engine/sm_engine_internal.h — engine 轨私有声明（非契约文件）
 *
 * 仅 engine/ 内部各 .c 与 host 自测（SM_HOST_TEST）使用，不对外发布。
 * 契约见 smarthome_types.h / smarthome_storage.h / smarthome_internal.h。
 ****************************************************************************/

#ifndef __SM_ENGINE_INTERNAL_H
#define __SM_ENGINE_INTERNAL_H

#include "smarthome_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * 属性枚举 → schema 字符串
 * JSON 序列化（sm_json.c）与规则命中日志（sm_engine.c）共用，
 * 字符串必须与 smarthome_storage.h 冻结的枚举映射一致。
 ****************************************************************************/

static inline const char *sm_prop_name(sm_prop_t prop)
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

#ifdef SM_HOST_TEST
/* host 单测钩子：手动推进 sim_sensor 内部虚拟时钟，用于快速验证
 * 三角波推进与越限触发。board 构建（未定义 SM_HOST_TEST）不存在。 */
void sm_sim_test_advance_ms(long ms);
#endif

#ifdef __cplusplus
}
#endif

#endif /* __SM_ENGINE_INTERNAL_H */
