cmake_minimum_required(VERSION 3.10)
project(anland C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic")
add_library(socket_utils STATIC common/socket_utils.c)
set_target_properties(socket_utils PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(socket_utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/common)

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

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
    add_subdirectory(libdisplay_producer)

    add_library(display_consumer SHARED libdisplay_consumer/display_consumer.c)
    target_link_libraries(display_consumer socket_utils pthread)

endif()