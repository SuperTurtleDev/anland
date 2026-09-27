# awl/CMakeLists.txt — host build of the awl logic layer (AndroidWayland).
#
# Unlike the Android cross build (services/waylandbridge/CMakeLists.txt), this
# builds ONLY the pure-C logic layer so it can be compiled and tested on the
# Debian container without an NDK sysroot / binder / EGL:
#
#   - wayland-server-static : the awl-patched libwayland fork (static)
#   - awllogic               : the protocol state machines (static lib)
#   - awl_host_test          : smoke test binary
#
# The Android adaptation layer (awl_renderer.cpp / waylandbridge.cpp) is NOT
# built here; the cross build (repo) keeps owning it.
#
# Logging: host build uses fprintf(stderr); pass -DAWL_LOG_BIONIC=ON to get
# the android/log.h flavor (cross build only).
cmake_minimum_required(VERSION 3.16)
project(awl C)

set(CMAKE_C_STANDARD 11)

option(AWL_LOG_DEBUG "compile LOGD tracing" OFF)
if(AWL_LOG_DEBUG)
  add_compile_definitions(AWL_LOG_DEBUG)
endif()

set(AWL ${CMAKE_CURRENT_SOURCE_DIR})
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN}/config)

# System libffi (host; the cross build compiles its own under build/)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBFFI REQUIRED IMPORTED_TARGET libffi)

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
message(STATUS "wayland-scanner: ${WAYLAND_SCANNER}")

# ---- built-in (awl-patched) libwayland-server ----------------------------------
set(WAYLAND_SRC ${AWL}/third_party/wayland)

configure_file(${AWL}/third_party/awl_config.h ${GEN}/config.h COPYONLY)
configure_file(${AWL}/third_party/wayland-version.h ${GEN}/wayland-version.h COPYONLY)

# core wayland.xml
add_custom_command(
  OUTPUT ${GEN}/wayland-server-protocol-core.h ${GEN}/wayland-server-protocol.h
         ${GEN}/wayland-protocol-core.c
  COMMAND ${WAYLAND_SCANNER} server-header -c ${WAYLAND_SRC}/protocol/wayland.xml
          ${GEN}/wayland-server-protocol-core.h
  COMMAND ${WAYLAND_SCANNER} server-header ${WAYLAND_SRC}/protocol/wayland.xml
          ${GEN}/wayland-server-protocol.h
  COMMAND ${WAYLAND_SCANNER} private-code -c ${WAYLAND_SRC}/protocol/wayland.xml
          ${GEN}/wayland-protocol-core.c
  DEPENDS ${WAYLAND_SRC}/protocol/wayland.xml)

# ---- awl protocol extensions ---------------------------------------------------
set(AWL_PROTOCOLS
  linux-dmabuf-unstable-v1
  xdg-shell
  viewporter
  fractional-scale-v1
  relative-pointer-unstable-v1
  pointer-constraints-unstable-v1
  idle-inhibit-unstable-v1
  xwayland-shell-v1
  text-input-unstable-v1
  text-input-unstable-v3
  input-method-unstable-v2
  tablet-unstable-v2)

foreach(p ${AWL_PROTOCOLS})
  set(PXML ${CMAKE_CURRENT_SOURCE_DIR}/services/waylandbridge/protocols/${p}.xml)
  add_custom_command(
    OUTPUT ${GEN}/${p}-server-protocol.h ${GEN}/${p}-protocol.c
    COMMAND ${WAYLAND_SCANNER} server-header ${PXML} ${GEN}/${p}-server-protocol.h
    COMMAND ${WAYLAND_SCANNER} private-code  ${PXML} ${GEN}/${p}-protocol.c
    DEPENDS ${PXML})
  list(APPEND AWL_GENERATED_C ${GEN}/${p}-protocol.c)
  list(APPEND AWL_GENERATED_H ${GEN}/${p}-server-protocol.h)
endforeach()

add_library(wayland-server-static STATIC
  ${WAYLAND_SRC}/src/wayland-server.c
  ${WAYLAND_SRC}/src/wayland-shm.c
  ${WAYLAND_SRC}/src/connection.c
  ${WAYLAND_SRC}/src/event-loop.c
  ${WAYLAND_SRC}/src/wayland-os.c
  ${WAYLAND_SRC}/src/wayland-util.c
  ${GEN}/wayland-protocol-core.c)

target_include_directories(wayland-server-static PUBLIC
  ${WAYLAND_SRC}/src
  ${GEN}
  ${GEN}/config
  ${AWL}/third_party
  ${AWL}/third_party/include)
target_link_libraries(wayland-server-static PUBLIC PkgConfig::LIBFFI)
target_compile_options(wayland-server-static PRIVATE
  -Wno-unused-parameter -DHAVE_CONFIG_H=0
  -DHAVE_MEMFD_CREATE=1 -DHAVE_ACCEPT4=1 -DHAVE_SYS_UCRED_H=0 -DHAVE_XUCRED_CR_PID=0
  -DHAVE_BROKEN_MSG_CMSG_CLOEXEC=0)

# ---- awl logic layer -----------------------------------------------------------
add_library(awllogic STATIC
  services/waylandbridge/awl_server.c
  services/waylandbridge/awl_surface.c
  services/waylandbridge/awl_xdg.c
  services/waylandbridge/awl_dmabuf.c
  services/waylandbridge/awl_input.c
  services/waylandbridge/awl_ime.c
  services/waylandbridge/awl_input_method_v2.c
  services/waylandbridge/awl_subsurface.c
  services/waylandbridge/awl_data_device.c
  services/waylandbridge/awl_viewport.c
  services/waylandbridge/awl_xwayland.c
  services/waylandbridge/awl_idle.c
  services/waylandbridge/awl_sched.c
  services/waylandbridge/awl_tablet.c
  ${AWL_GENERATED_C})
target_include_directories(awllogic PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/services/waylandbridge
  ${GEN}
  ${WAYLAND_SRC}/src
  ${AWL}/third_party
  ${AWL}/third_party/include)
target_link_libraries(awllogic PUBLIC wayland-server-static pthread m)
target_compile_options(awllogic PRIVATE -Wno-unused-parameter)

# ---- host smoke test -------------------------------------------------------------
# Client-side tablet protocol header (the smoke client is a real Wayland client)
add_custom_command(
  OUTPUT ${GEN}/tablet-unstable-v2-client-protocol.h
  COMMAND ${WAYLAND_SCANNER} client-header
          ${CMAKE_CURRENT_SOURCE_DIR}/services/waylandbridge/protocols/tablet-unstable-v2.xml
          ${GEN}/tablet-unstable-v2-client-protocol.h
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/services/waylandbridge/protocols/tablet-unstable-v2.xml)
add_custom_command(
  OUTPUT ${GEN}/input-method-unstable-v2-client-protocol.h
  COMMAND ${WAYLAND_SCANNER} client-header
          ${CMAKE_CURRENT_SOURCE_DIR}/services/waylandbridge/protocols/input-method-unstable-v2.xml
          ${GEN}/input-method-unstable-v2-client-protocol.h
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/services/waylandbridge/protocols/input-method-unstable-v2.xml)
add_custom_target(awl_client_headers DEPENDS
  ${GEN}/tablet-unstable-v2-client-protocol.h
  ${GEN}/input-method-unstable-v2-client-protocol.h)

pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
enable_testing()

add_executable(awl_host_test tests/awl_host_test.c)
add_dependencies(awl_host_test awl_client_headers)
target_include_directories(awl_host_test PRIVATE ${GEN})
target_link_libraries(awl_host_test PRIVATE awllogic PkgConfig::WAYLAND_CLIENT)
add_test(NAME awl_host COMMAND awl_host_test)
set_tests_properties(awl_host PROPERTIES TIMEOUT 30)

add_executable(awl_ime_v2_test tests/awl_ime_v2_test.c)
add_dependencies(awl_ime_v2_test awl_client_headers)
target_include_directories(awl_ime_v2_test PRIVATE ${GEN})
target_link_libraries(awl_ime_v2_test PRIVATE awllogic PkgConfig::WAYLAND_CLIENT)
add_test(NAME awl_ime_v2 COMMAND awl_ime_v2_test)
set_tests_properties(awl_ime_v2 PROPERTIES TIMEOUT 30)