cmake_minimum_required(VERSION 3.20)
project(mtradio CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)

add_library(mtcore STATIC src/mt7902.cpp src/mt7902_radio.cpp src/mt7902_sta.cpp src/proxy.cpp src/dma.cpp)
target_include_directories(mtcore PUBLIC src)
target_compile_options(mtcore PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(mtcore PUBLIC setupapi cfgmgr32)

# The Wi-Fi station beside AWDL: the Intel radio's join, WPA2/WPA3 and TAP
# bridge (iwxradio/src, built with that project's warning level), driven
# through StaDevice by mt_station.cpp with host-side CCMP.
set(IWX_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../iwxradio/src)
add_library(stashared STATIC ${IWX_SRC}/wifi_sta.cpp ${IWX_SRC}/tap.cpp ${IWX_SRC}/crypto.cpp ${IWX_SRC}/wpa3.cpp)
target_include_directories(stashared PUBLIC ${IWX_SRC})
target_compile_definitions(stashared PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(stashared PRIVATE /W3 /EHsc /wd4005 /utf-8)
target_link_libraries(stashared PUBLIC bcrypt setupapi)

add_library(mtsta STATIC src/mt_station.cpp src/ccmp.cpp)
target_compile_options(mtsta PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(mtsta PUBLIC mtcore stashared bcrypt)

add_executable(mtprobe src/probe_main.cpp)
target_compile_options(mtprobe PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(mtprobe PRIVATE mtcore winmm)

add_executable(nekodrop-radio-mt src/radio_mt_main.cpp)
target_compile_options(nekodrop-radio-mt PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(nekodrop-radio-mt PRIVATE mtsta winmm advapi32)

add_executable(ccmp_test tests/ccmp_test.cpp src/ccmp.cpp)
target_include_directories(ccmp_test PRIVATE src)
target_compile_options(ccmp_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(ccmp_test PRIVATE bcrypt)
