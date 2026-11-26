# SPDX-License-Identifier: GPL-2.0-only
# User-mode MediaTek MT7922 / MT7921 radio daemon for nekodrop / winawdl.
cmake_minimum_required(VERSION 3.20)
project(mtkradio CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(MTK_DEFS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(MTK_OPTS /W3 /EHsc -Wno-address-of-packed-member -Wno-unused-function)
else()
  set(MTK_OPTS /W3 /EHsc /wd4005 /utf-8)
endif()

set(IWX_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../iwxradio/src)
set(MT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../mtradio/src)

# Shared Wi-Fi station (join, WPA2/WPA3, TAP bridge) from iwxradio, driven via
# StaDevice (iwxradio/src/sta_device.h) by mtk_station.cpp with mtradio's host CCMP.
add_library(mtk_stashared STATIC
  ${IWX_SRC}/wifi_sta.cpp
  ${IWX_SRC}/tap.cpp
  ${IWX_SRC}/crypto.cpp
  ${IWX_SRC}/wpa3.cpp
)
target_include_directories(mtk_stashared PUBLIC ${IWX_SRC})
target_compile_definitions(mtk_stashared PRIVATE ${MTK_DEFS})
target_compile_options(mtk_stashared PRIVATE ${MTK_OPTS})
target_link_libraries(mtk_stashared PUBLIC bcrypt setupapi)

add_executable(nekodrop-radio-mtk
  src/mtk_main.cpp
  src/mtk_dev.cpp
  src/mtk_dma.cpp
  src/mtk_station.cpp
  ${MT_SRC}/ccmp.cpp
  ${IWX_SRC}/proxy.cpp
  ${IWX_SRC}/plat_win.cpp
)
target_include_directories(nekodrop-radio-mtk PRIVATE src ${IWX_SRC} ${MT_SRC} ../driver/regproxy)
target_compile_definitions(nekodrop-radio-mtk PRIVATE ${MTK_DEFS})
target_compile_options(nekodrop-radio-mtk PRIVATE ${MTK_OPTS})
target_link_libraries(nekodrop-radio-mtk PRIVATE mtk_stashared setupapi winmm advapi32 bcrypt)

add_executable(ccmp_test
  ../mtradio/tests/ccmp_test.cpp
  ${MT_SRC}/ccmp.cpp
)
target_include_directories(ccmp_test PRIVATE ${MT_SRC})
target_compile_definitions(ccmp_test PRIVATE ${MTK_DEFS})
target_compile_options(ccmp_test PRIVATE ${MTK_OPTS})
target_link_libraries(ccmp_test PRIVATE bcrypt)
