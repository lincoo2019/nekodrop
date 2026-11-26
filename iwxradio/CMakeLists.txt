# SPDX-License-Identifier: GPL-2.0-only
# The Intel radio daemon and its offline self-test. Builds with MSVC or
# clang-cl (scripts/build-radio.ps1 configures it).
cmake_minimum_required(VERSION 3.20)
project(iwxradio CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(IWX_DEFS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(IWX_OPTS /W3 /EHsc -Wno-address-of-packed-member -Wno-gnu-variable-sized-type-not-at-end
      -Wno-zero-length-array -Wno-unused-function -Wno-microsoft-anon-tag)
else()
  set(IWX_OPTS /W3 /EHsc /wd4005)
endif()

add_executable(winawdl_radio
  src/radio_main.cpp src/iwx.cpp src/iwx_sta.cpp src/iwx_scan.cpp src/fw.cpp src/plat_win.cpp src/kdev.cpp src/dma.cpp src/proxy.cpp
  src/crypto.cpp src/tap.cpp src/wifi_sta.cpp src/wpa3.cpp src/rx_reorder.cpp)
target_compile_definitions(winawdl_radio PRIVATE ${IWX_DEFS})
target_compile_options(winawdl_radio PRIVATE ${IWX_OPTS})
target_link_libraries(winawdl_radio PRIVATE setupapi winmm advapi32 bcrypt)

add_executable(iwx_selftest tests/selftest.cpp src/fw.cpp src/plat_win.cpp src/crypto.cpp src/wpa3.cpp src/rx_reorder.cpp)
target_link_libraries(iwx_selftest PRIVATE bcrypt)
target_compile_definitions(iwx_selftest PRIVATE ${IWX_DEFS})
target_compile_options(iwx_selftest PRIVATE ${IWX_OPTS})
