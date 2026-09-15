cmake_minimum_required(VERSION 3.10)
project(anland C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic")

add_library(socket_utils STATIC common/socket_utils.c)

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

    add_library(display_producer SHARED libdisplay_producer/display_producer.c)
    target_link_libraries(display_producer socket_utils pthread)

    add_library(display_consumer SHARED libdisplay_consumer/display_consumer.c)
    target_link_libraries(display_consumer socket_utils pthread)

    include(CTest)
    if(BUILD_TESTING)
        add_executable(display_consumer_tests tests/display_consumer_tests.c)
        target_link_libraries(display_consumer_tests display_consumer socket_utils pthread)
        add_test(NAME display_consumer_tests COMMAND display_consumer_tests)
        set_tests_properties(display_consumer_tests PROPERTIES TIMEOUT 20)
    endif()

    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2 sdl2)
        pkg_check_modules(GBM gbm)
        pkg_check_modules(LIBDRM libdrm)
        pkg_check_modules(EGL egl)
        pkg_check_modules(GLESV2 glesv2)
    endif()
endif()
