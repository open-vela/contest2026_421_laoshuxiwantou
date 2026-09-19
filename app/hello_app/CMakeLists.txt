# Contest 2026 team 421 smarthome control panel app
if(CONFIG_LVX_USE_DEMO_CONTEST2026_000_HELLO_APP)
  file(GLOB SMARTHOME_ENGINE_SRCS engine/*.c)
  file(GLOB SMARTHOME_UI_SRCS ui/*.c)
  file(GLOB SMARTHOME_SIM_SRCS sim/*.c)
  list(FILTER SMARTHOME_ENGINE_SRCS EXCLUDE REGEX "selftest_main\\.c$")
  nuttx_add_application(
    NAME smarthome
    SRCS smarthome_main.c ${SMARTHOME_ENGINE_SRCS} ${SMARTHOME_UI_SRCS} ${SMARTHOME_SIM_SRCS}
    STACKSIZE 262144
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR})
endif()
