############################################################################
# Contest 2026 team 421 - smarthome control panel app
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = smarthome
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = 262144
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_000_HELLO_APP)

# 引擎 / UI 两层源文件（sim/ 由主集成者在轨 C 交付后追加；
# engine/selftest_main.c 仅 host 单测用，不进板端构建）
CSRCS = engine/sm_engine.c \
        engine/sm_storage.c \
        engine/sm_json.c \
        engine/sim_sensor.c \
        engine/sm_beep.c \
        ui/sm_ui.c \
        ui/sm_ui_overview.c \
        ui/sm_ui_scene.c \
        ui/sm_ui_rules.c

MAINSRC = smarthome_main.c

# 子目录源文件按根目录契约头包含（#include "smarthome_types.h"）
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/demos/contest2026_421_hello_app

include $(APPDIR)/Application.mk
