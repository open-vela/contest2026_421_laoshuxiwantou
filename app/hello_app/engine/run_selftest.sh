#!/bin/sh
# engine/run_selftest.sh — host 单测脚本（唯一允许的编译）
#
# 用本机 gcc 编译 engine 单测（不含 sm_beep.c，selftest_main.c 提供
# sm_beep_play 桩），然后运行并打印 PASS/FAIL 摘要；测试整体失败时
# 以非 0 退出。
#
# 用法：sh engine/run_selftest.sh

set -e
cd "$(dirname "$0")"

mkdir -p /tmp/smtest

gcc -Wall -Wextra -std=c99 -I.. -DSM_HOST_TEST \
    selftest_main.c sm_engine.c sm_storage.c sm_json.c sim_sensor.c \
    -o /tmp/sm_selftest

exec /tmp/sm_selftest
