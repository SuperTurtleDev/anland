cmake_minimum_required(VERSION 3.10)
project(anland C)

set(CMAKE_C_STANDARD 11)
include(CTest)
if(BUILD_TESTING AND NOT ANDROID)
    add_executable(anland_tablet_test libdisplay_producer/tests/tablet_test.c)
    target_include_directories(anland_tablet_test PRIVATE libdisplay_producer common)
    add_test(NAME native_tablet_protocol COMMAND anland_tablet_test)
    add_executable(anland_hdr_policy_test libdisplay_producer/tests/hdr_policy_test.c)
    target_include_directories(anland_hdr_policy_test PRIVATE libdisplay_producer)
    add_test(NAME hdr_consumer_policy COMMAND anland_hdr_policy_test)
    add_executable(anland_hdr_device_control_test libdisplay_producer/tests/hdr_device_control_test.c)
    target_include_directories(anland_hdr_device_control_test PRIVATE libdisplay_producer common)
    target_compile_options(anland_hdr_device_control_test PRIVATE -ffunction-sections -fdata-sections)
    set_property(TARGET anland_hdr_device_control_test APPEND_STRING PROPERTY LINK_FLAGS " -Wl,--gc-sections")
    add_test(NAME hdr_device_control COMMAND anland_hdr_device_control_test)
endif()
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic")
add_library(socket_utils STATIC common/socket_utils.c)
set_target_properties(socket_utils PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(socket_utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/common)

if(ANDROID)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D__ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__")
endif()

option(BUILD_BENCHMARKS "Build Anland IPC and audio benchmarks" ON)
if(BUILD_BENCHMARKS)
    add_executable(benchmark_audio_ipc engineering/benchmarks/benchmark_audio_ipc.c)
    target_link_libraries(benchmark_audio_ipc pthread)

    add_executable(benchmark_shm_audio engineering/benchmarks/benchmark_shm_audio.c)
    target_link_libraries(benchmark_shm_audio pthread)
endif()

if(ANDROID)
    add_library(display_daemon_lib STATIC libdisplay_daemon/display_daemon.c)
    target_link_libraries(display_daemon_lib socket_utils)

    add_executable(display_daemon daemon/daemon.c)
    target_link_libraries(display_daemon display_daemon_lib socket_utils)

    add_library(display_consumer SHARED libdisplay_consumer/display_consumer.c)
    target_link_libraries(display_consumer socket_utils)
else()
    add_library(display_daemon_lib STATIC libdisplay_daemon/display_daemon.c)
    target_link_libraries(display_daemon_lib socket_utils pthread)

    add_executable(display_daemon daemon/daemon.c)
    target_link_libraries(display_daemon display_daemon_lib socket_utils pthread)

    add_subdirectory(libdisplay_producer)

    add_library(display_consumer SHARED libdisplay_consumer/display_consumer.c)
    target_link_libraries(display_consumer socket_utils pthread)

endif()

add_executable(anland_audio_headless consumers/headless_audio/anland_audio_headless.c)
target_link_libraries(anland_audio_headless socket_utils pthread)
