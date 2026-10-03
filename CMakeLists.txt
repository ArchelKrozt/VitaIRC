cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(VitaIRC C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "VitaIRC")
set(VITA_TITLEID  "VIRC00001")
set(VITA_VERSION  "01.00")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall -Wno-format-truncation -Wno-stringop-truncation")

option(VITAIRC_DEMO "Build the screenshot/demo mode (development only)" OFF)
option(VITAIRC_DEMO_EN "Demo mode with English UI and sample messages" OFF)
if(VITAIRC_DEMO)
  add_definitions(-DVITAIRC_DEMO)
endif()
if(VITAIRC_DEMO_EN)
  add_definitions(-DVITAIRC_DEMO -DVITAIRC_DEMO_EN)
endif()

add_executable(${PROJECT_NAME}
  src/main.c
  src/irc.c
  src/conn.c
  src/net.c
  src/config.c
  src/util.c
  src/minijson.c
  src/history.c
  src/i18n.c
  src/camera.c
  src/sound.c
)

target_link_libraries(${PROJECT_NAME}
  vita2d
  curl
  mbedtls
  mbedx509
  mbedcrypto
  png
  jpeg
  freetype
  bz2
  zstd
  z
  pthread
  m
  SceLibKernel_stub
  SceDisplay_stub
  SceGxm_stub
  SceCtrl_stub
  SceTouch_stub
  ScePgf_stub
  ScePvf_stub
  SceCommonDialog_stub
  SceAppUtil_stub
  SceAppMgr_stub
  SceCamera_stub
  SceAudio_stub
  SceSysmodule_stub
  SceNet_stub
  SceNetCtl_stub
  ScePower_stub
  SceRtc_stub
  SceIofilemgr_stub
  SceKernelThreadMgr_stub
  SceProcessmgr_stub
  SceSysmem_stub
  SceKernelModulemgr_stub
)

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  FILE res/cacert.pem cacert.pem
)
