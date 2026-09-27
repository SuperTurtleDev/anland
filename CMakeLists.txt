cmake_minimum_required(VERSION 3.10)
project(anland C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic")

# This source tree does not ship the separate test-tree loopback tests.
# enable_testing() is set below only for the opt-in AWL host tests.

add_library(socket_utils STATIC common/socket_utils.c)
add_library(anland_present_ipc STATIC common/anland_present_ipc.c)
target_include_directories(anland_present_ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/common)

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

    # Contract/loopback tests are kept in the separate local test tree.

    # AWL has extra host dependencies (libffi and libwayland-client). Keep it
    # opt-in so the default producer build remains lightweight and portable.
    option(ANLAND_BUILD_AWL_TESTS "Build AWL host protocol tests" OFF)
    if(ANLAND_BUILD_AWL_TESTS)
        enable_testing()
        add_subdirectory(awl)
    endif()
endif()