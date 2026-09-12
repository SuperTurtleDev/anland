cmake_minimum_required(VERSION 3.22.1)
project(waylandbridge C CXX)

# This CMakeLists lives at awl/services/waylandbridge/ — the awl root is two levels up
get_filename_component(AWL ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Hot-path DEBUG tracing (LOGD — per-attach/commit/frame/buffer protocol logs
# and render-thread buffer imports, see awl_log.h) is compiled out unless this
# is ON. Release builds (`make native`) leave it OFF; `make native-debug`
# enables it. LOGI/LOGE are always kept.
option(AWL_LOG_DEBUG "Compile LOGD hot-path tracing into the binaries" OFF)
if(AWL_LOG_DEBUG)
  add_compile_definitions(AWL_LOG_DEBUG)
endif()

# ---------------- Configuration ----------------
if(NOT AWL_OUT)
  set(AWL_OUT ${CMAKE_BINARY_DIR}/out)
endif()
set(AWL_GENERATED ${CMAKE_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${AWL_GENERATED})

# Host wayland-scanner (protocol codegen; version must match wayland-src, 1.24)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
message(STATUS "wayland-scanner: ${WAYLAND_SCANNER}")

# libwayland source: in-repo fork (git@github.com:SuperTurtleDev/libwayland.git, branch awl)
set(WAYLAND_SRC ${AWL}/third_party/wayland CACHE PATH "libwayland source")

# ---------------- libwayland-server (static, cross-compiled) ----------------
# The wayland sources include "../config.h" relative to src/; resolve it to
# gen/config.h (with -I gen/config, "../config.h" => gen/config.h)
file(MAKE_DIRECTORY ${AWL_GENERATED}/config)
configure_file(${AWL}/third_party/awl_config.h
               ${AWL_GENERATED}/config.h COPYONLY)
# Hand-written version header + bionic ucred shim
configure_file(${AWL}/third_party/wayland-version.h
               ${AWL_GENERATED}/wayland-version.h COPYONLY)

# libffi (Android cross-compile output, make libffi → build/libffi-android-out)
set(LIBFFI_DIR ${AWL}/build/libffi-android-out)

# Core protocol codegen
add_custom_command(
  OUTPUT ${AWL_GENERATED}/wayland-server-protocol-core.h
  COMMAND ${WAYLAND_SCANNER} server-header -c ${WAYLAND_SRC}/protocol/wayland.xml
          ${AWL_GENERATED}/wayland-server-protocol-core.h
  DEPENDS ${WAYLAND_SRC}/protocol/wayland.xml)
add_custom_command(
  OUTPUT ${AWL_GENERATED}/wayland-server-protocol.h
  COMMAND ${WAYLAND_SCANNER} server-header ${WAYLAND_SRC}/protocol/wayland.xml
          ${AWL_GENERATED}/wayland-server-protocol.h
  DEPENDS ${WAYLAND_SRC}/protocol/wayland.xml)
add_custom_command(
  OUTPUT ${AWL_GENERATED}/wayland-protocol-core.c
  COMMAND ${WAYLAND_SCANNER} private-code -c ${WAYLAND_SRC}/protocol/wayland.xml
          ${AWL_GENERATED}/wayland-protocol-core.c
  DEPENDS ${WAYLAND_SRC}/protocol/wayland.xml)

# linux-dmabuf v1 protocol codegen
set(DMABUF_XML ${CMAKE_CURRENT_SOURCE_DIR}/protocols/linux-dmabuf-unstable-v1.xml)
add_custom_command(
  OUTPUT ${AWL_GENERATED}/linux-dmabuf-unstable-v1-server-protocol.h
  COMMAND ${WAYLAND_SCANNER} server-header ${DMABUF_XML}
          ${AWL_GENERATED}/linux-dmabuf-unstable-v1-server-protocol.h
  DEPENDS ${DMABUF_XML})
add_custom_command(
  OUTPUT ${AWL_GENERATED}/linux-dmabuf-unstable-v1-protocol.c
  COMMAND ${WAYLAND_SCANNER} public-code ${DMABUF_XML}
          ${AWL_GENERATED}/linux-dmabuf-unstable-v1-protocol.c
  DEPENDS ${DMABUF_XML})

# xdg-shell protocol codegen (window management)
set(XDG_XML ${CMAKE_CURRENT_SOURCE_DIR}/protocols/xdg-shell.xml)
add_custom_command(
  OUTPUT ${AWL_GENERATED}/xdg-shell-server-protocol.h
  COMMAND ${WAYLAND_SCANNER} server-header ${XDG_XML}
          ${AWL_GENERATED}/xdg-shell-server-protocol.h
  DEPENDS ${XDG_XML})
add_custom_command(
  OUTPUT ${AWL_GENERATED}/xdg-shell-protocol.c
  COMMAND ${WAYLAND_SCANNER} private-code ${XDG_XML}
          ${AWL_GENERATED}/xdg-shell-protocol.c
  DEPENDS ${XDG_XML})

# viewporter + fractional-scale v1 protocol codegen (#31 arbitrary-ratio zoom, awl_viewport.c in this dir)
set(VP_XML ${CMAKE_CURRENT_SOURCE_DIR}/protocols/viewporter.xml)
add_custom_command(
  OUTPUT ${AWL_GENERATED}/viewporter-server-protocol.h
  COMMAND ${WAYLAND_SCANNER} server-header ${VP_XML}
          ${AWL_GENERATED}/viewporter-server-protocol.h
  DEPENDS ${VP_XML})
add_custom_command(
  OUTPUT ${AWL_GENERATED}/viewporter-protocol.c
  COMMAND ${WAYLAND_SCANNER} private-code ${VP_XML}
          ${AWL_GENERATED}/viewporter-protocol.c
  DEPENDS ${VP_XML})
set(FS_XML ${CMAKE_CURRENT_SOURCE_DIR}/protocols/fractional-scale-v1.xml)
add_custom_command(
  OUTPUT ${AWL_GENERATED}/fractional-scale-v1-server-protocol.h
  COMMAND ${WAYLAND_SCANNER} server-header ${FS_XML}
          ${AWL_GENERATED}/fractional-scale-v1-server-protocol.h
  DEPENDS ${FS_XML})
add_custom_command(
  OUTPUT ${AWL_GENERATED}/fractional-scale-v1-protocol.c
  COMMAND ${WAYLAND_SCANNER} private-code ${FS_XML}
          ${AWL_GENERATED}/fractional-scale-v1-protocol.c
  DEPENDS ${FS_XML})

# relative-pointer v1 protocol codegen (relative motion reporting: PTR_REL →
# relative_motion; the Xwayland warp emulator converts it into X relative
# MotionNotify)
set(RP_XML ${CMAKE_CURRENT_SOURCE_DIR}/protocols/relative-pointer-unstable-v1.xml)
add_custom_command(
  OUTPUT ${AWL_GENERATED}/relative-pointer-unstable-v1-server-protocol.h
  COMMAND ${WAYLAND_SCANNER} server-header ${RP_XML}
          ${AWL_GENERATED}/relative-pointer-unstable-v1-server-protocol.h
  DEPENDS ${RP_XML})
add_custom_command(
  OUTPUT ${AWL_GENERATED}/relative-pointer-unstable-v1-protocol.c
  COMMAND ${WAYLAND_SCANNER} private-code ${RP_XML}
          ${AWL_GENERATED}/relative-pointer-unstable-v1-protocol.c
  DEPENDS ${RP_XML})

# pointer-constraints v1 protocol codegen (lock/confine; awl_input.c in this
# dir — activation tells the Activity to requestPointerCapture via C_CAPTURE)
set(PC_XML ${CMAKE_CURRENT_SOURCE_DIR}/protocols/pointer-constraints-unstable-v1.xml)
add_custom_command(
  OUTPUT ${AWL_GENERATED}/pointer-constraints-unstable-v1-server-protocol.h
  COMMAND ${WAYLAND_SCANNER} server-header ${PC_XML}
          ${AWL_GENERATED}/pointer-constraints-unstable-v1-server-protocol.h
  DEPENDS ${PC_XML})
add_custom_command(
  OUTPUT ${AWL_GENERATED}/pointer-constraints-unstable-v1-protocol.c
  COMMAND ${WAYLAND_SCANNER} private-code ${PC_XML}
          ${AWL_GENERATED}/pointer-constraints-unstable-v1-protocol.c
  DEPENDS ${PC_XML})

# idle-inhibit v1 protocol codegen (idle inhibitor: keep the screen on while
# the inhibiting surface is visible; awl_idle.c in this dir — activation tells
# the Activity to set FLAG_KEEP_SCREEN_ON via C_KEEPON)
set(II_XML ${CMAKE_CURRENT_SOURCE_DIR}/protocols/idle-inhibit-unstable-v1.xml)
add_custom_command(
  OUTPUT ${AWL_GENERATED}/idle-inhibit-unstable-v1-server-protocol.h
  COMMAND ${WAYLAND_SCANNER} server-header ${II_XML}
          ${AWL_GENERATED}/idle-inhibit-unstable-v1-server-protocol.h
  DEPENDS ${II_XML})
add_custom_command(
  OUTPUT ${AWL_GENERATED}/idle-inhibit-unstable-v1-protocol.c
  COMMAND ${WAYLAND_SCANNER} private-code ${II_XML}
          ${AWL_GENERATED}/idle-inhibit-unstable-v1-protocol.c
  DEPENDS ${II_XML})

# xdg-toplevel-icon v1 protocol codegen (per-window Recents icons,
# awl_icon.c in this dir — applied icon → C_ICON, the Activity pulls the
# pixels over AWL_T_ICON and puts them in the task description)
set(XTI_XML ${CMAKE_CURRENT_SOURCE_DIR}/protocols/xdg-toplevel-icon-v1.xml)
add_custom_command(
  OUTPUT ${AWL_GENERATED}/xdg-toplevel-icon-v1-server-protocol.h
  COMMAND ${WAYLAND_SCANNER} server-header ${XTI_XML}
          ${AWL_GENERATED}/xdg-toplevel-icon-v1-server-protocol.h
  DEPENDS ${XTI_XML})
add_custom_command(
  OUTPUT ${AWL_GENERATED}/xdg-toplevel-icon-v1-protocol.c
  COMMAND ${WAYLAND_SCANNER} private-code ${XTI_XML}
          ${AWL_GENERATED}/xdg-toplevel-icon-v1-protocol.c
  DEPENDS ${XTI_XML})

# xwayland-shell v1 protocol codegen (Xwayland rootless window association,
# awl_xwayland.c in this dir)
set(XWLS_XML ${CMAKE_CURRENT_SOURCE_DIR}/protocols/xwayland-shell-v1.xml)
add_custom_command(
  OUTPUT ${AWL_GENERATED}/xwayland-shell-v1-server-protocol.h
  COMMAND ${WAYLAND_SCANNER} server-header ${XWLS_XML}
          ${AWL_GENERATED}/xwayland-shell-v1-server-protocol.h
  DEPENDS ${XWLS_XML})
add_custom_command(
  OUTPUT ${AWL_GENERATED}/xwayland-shell-v1-protocol.c
  COMMAND ${WAYLAND_SCANNER} private-code ${XWLS_XML}
          ${AWL_GENERATED}/xwayland-shell-v1-protocol.c
  DEPENDS ${XWLS_XML})

# text-input v1/v3 protocol codegen (Android IME bridging, see awl_ime.c in this dir)
set(TI1_XML ${CMAKE_CURRENT_SOURCE_DIR}/protocols/text-input-unstable-v1.xml)
add_custom_command(
  OUTPUT ${AWL_GENERATED}/text-input-unstable-v1-server-protocol.h
  COMMAND ${WAYLAND_SCANNER} server-header ${TI1_XML}
          ${AWL_GENERATED}/text-input-unstable-v1-server-protocol.h
  DEPENDS ${TI1_XML})
add_custom_command(
  OUTPUT ${AWL_GENERATED}/text-input-unstable-v1-protocol.c
  COMMAND ${WAYLAND_SCANNER} private-code ${TI1_XML}
          ${AWL_GENERATED}/text-input-unstable-v1-protocol.c
  DEPENDS ${TI1_XML})
set(TI3_XML ${CMAKE_CURRENT_SOURCE_DIR}/protocols/text-input-unstable-v3.xml)
add_custom_command(
  OUTPUT ${AWL_GENERATED}/text-input-unstable-v3-server-protocol.h
  COMMAND ${WAYLAND_SCANNER} server-header ${TI3_XML}
          ${AWL_GENERATED}/text-input-unstable-v3-server-protocol.h
  DEPENDS ${TI3_XML})
add_custom_command(
  OUTPUT ${AWL_GENERATED}/text-input-unstable-v3-protocol.c
  COMMAND ${WAYLAND_SCANNER} private-code ${TI3_XML}
          ${AWL_GENERATED}/text-input-unstable-v3-protocol.c
  DEPENDS ${TI3_XML})

add_library(wayland-server-static STATIC
  ${WAYLAND_SRC}/src/wayland-server.c
  ${WAYLAND_SRC}/src/wayland-shm.c
  ${WAYLAND_SRC}/src/connection.c
  ${WAYLAND_SRC}/src/event-loop.c
  ${WAYLAND_SRC}/src/wayland-os.c
  ${WAYLAND_SRC}/src/wayland-util.c
  ${AWL_GENERATED}/wayland-protocol-core.c
  ${AWL_GENERATED}/wayland-server-protocol.h
  ${AWL_GENERATED}/wayland-server-protocol-core.h
)
target_include_directories(wayland-server-static PUBLIC
  ${WAYLAND_SRC}/src
  ${AWL_GENERATED}
  ${AWL_GENERATED}/config          # config.h (referenced via the relative path ../config.h)
  ${AWL}/third_party
  ${AWL}/third_party/include   # sys/ucred.h shim
  ${LIBFFI_DIR}/include)
target_link_libraries(wayland-server-static PUBLIC ${LIBFFI_DIR}/lib/libffi.a)
target_compile_options(wayland-server-static PRIVATE -Wno-unused-parameter -DHAVE_CONFIG_H=0)
# bionic compatibility: memfd_create/accept4 available, no sys/ucred.h
target_compile_definitions(wayland-server-static PRIVATE
  HAVE_MEMFD_CREATE=1 HAVE_ACCEPT4=1 HAVE_SYS_UCRED_H=0 HAVE_XUCRED_CR_PID=0
  HAVE_BROKEN_MSG_CMSG_CLOEXEC=0)

# ---------------- libwayland-client (static, for the shm test app) ----------------
# Client-flavor codegen: core client header + xdg-shell client header (the
# private-code .c files are interface-agnostic and shared with the server).
add_custom_command(
  OUTPUT ${AWL_GENERATED}/wayland-client-protocol.h
  COMMAND ${WAYLAND_SCANNER} client-header ${WAYLAND_SRC}/protocol/wayland.xml
          ${AWL_GENERATED}/wayland-client-protocol.h
  DEPENDS ${WAYLAND_SRC}/protocol/wayland.xml)
add_custom_command(
  OUTPUT ${AWL_GENERATED}/xdg-shell-client-protocol.h
  COMMAND ${WAYLAND_SCANNER} client-header ${XDG_XML}
          ${AWL_GENERATED}/xdg-shell-client-protocol.h
  DEPENDS ${XDG_XML})

add_library(wayland-client-static STATIC
  ${WAYLAND_SRC}/src/wayland-client.c
  ${WAYLAND_SRC}/src/connection.c
  ${WAYLAND_SRC}/src/wayland-os.c
  ${WAYLAND_SRC}/src/wayland-util.c
  ${AWL_GENERATED}/wayland-protocol-core.c
  ${AWL_GENERATED}/wayland-client-protocol.h
)
target_include_directories(wayland-client-static PUBLIC
  ${WAYLAND_SRC}/src
  ${AWL_GENERATED}
  ${AWL_GENERATED}/config
  ${AWL}/third_party/include
  ${LIBFFI_DIR}/include)
target_link_libraries(wayland-client-static PUBLIC ${LIBFFI_DIR}/lib/libffi.a)
target_compile_options(wayland-client-static PRIVATE -Wno-unused-parameter -DHAVE_CONFIG_H=0)
target_compile_definitions(wayland-client-static PRIVATE
  HAVE_MEMFD_CREATE=1 HAVE_ACCEPT4=1 HAVE_SYS_UCRED_H=0 HAVE_XUCRED_CR_PID=0
  HAVE_BROKEN_MSG_CMSG_CLOEXEC=0)

# ---------------- SHM test app (third-party wayland client, #36) ----
# Executable named like a shared library (libawlshm.so) so it packs into the
# test APK's jniLibs and gets extracted to nativeLibraryDir (the only place
# an app may exec on Android 10+); runtime: extractNativeLibs=true.
# PURE wayland client: the connection fd arrives via the fork-inherited
# WAYLAND_SOCKET env var — no binder here (a future glibc/proot build keeps
# this file unchanged).
add_executable(awlshm awl_shm_test.c
  ${AWL_GENERATED}/xdg-shell-protocol.c
  ${AWL_GENERATED}/xdg-shell-client-protocol.h)
target_include_directories(awlshm PRIVATE ${AWL_GENERATED})
target_link_libraries(awlshm PRIVATE wayland-client-static log)
set_target_properties(awlshm PROPERTIES
  OUTPUT_NAME awlshm
  PREFIX "lib"
  SUFFIX ".so"
  RUNTIME_OUTPUT_DIRECTORY ${AWL_OUT}
  COMPILE_OPTIONS "-Wno-unused-parameter")

# ---------------- Single waylandbridge binary (root daemon, module install) ----
# One executable target: logic layer (protocol state machines) + adaptation
# layer (GPU renderer, binder host) compiled straight in — no adapt/logic
# static-lib split, the artifact is one ELF (no .so / LD_LIBRARY_PATH /
# exe-lib mismatch issues). Only third-party libwayland stays a static lib
# (own bionic-compat flags below).
add_executable(waylandbridge
  # logic layer
  awl_server.c
  awl_surface.c
  awl_xdg.c
  awl_dmabuf.c
  awl_input.c
  awl_ime.c
  awl_subsurface.c
  awl_data_device.c
  awl_viewport.c
  awl_xwayland.c
  awl_idle.c
  awl_icon.c
  awl_sched.c
  ${AWL_GENERATED}/xwayland-shell-v1-protocol.c
  ${AWL_GENERATED}/xwayland-shell-v1-server-protocol.h
  ${AWL_GENERATED}/relative-pointer-unstable-v1-protocol.c
  ${AWL_GENERATED}/relative-pointer-unstable-v1-server-protocol.h
  ${AWL_GENERATED}/pointer-constraints-unstable-v1-protocol.c
  ${AWL_GENERATED}/pointer-constraints-unstable-v1-server-protocol.h
  ${AWL_GENERATED}/idle-inhibit-unstable-v1-protocol.c
  ${AWL_GENERATED}/idle-inhibit-unstable-v1-server-protocol.h
  ${AWL_GENERATED}/xdg-toplevel-icon-v1-protocol.c
  ${AWL_GENERATED}/xdg-toplevel-icon-v1-server-protocol.h
  ${AWL_GENERATED}/viewporter-protocol.c
  ${AWL_GENERATED}/viewporter-server-protocol.h
  ${AWL_GENERATED}/fractional-scale-v1-protocol.c
  ${AWL_GENERATED}/fractional-scale-v1-server-protocol.h
  ${AWL_GENERATED}/linux-dmabuf-unstable-v1-protocol.c
  ${AWL_GENERATED}/linux-dmabuf-unstable-v1-server-protocol.h
  ${AWL_GENERATED}/xdg-shell-protocol.c
  ${AWL_GENERATED}/xdg-shell-server-protocol.h
  ${AWL_GENERATED}/text-input-unstable-v1-protocol.c
  ${AWL_GENERATED}/text-input-unstable-v1-server-protocol.h
  ${AWL_GENERATED}/text-input-unstable-v3-protocol.c
  ${AWL_GENERATED}/text-input-unstable-v3-server-protocol.h
  # adaptation layer
  awl_renderer.cpp
  waylandbridge.cpp
)
target_include_directories(waylandbridge PRIVATE
  ${AWL}/include ${AWL_GENERATED}
  ${WAYLAND_SRC}/src
  ${AWL_GENERATED}/config
  ${AWL}/third_party          # keymap_evdev.h (embedded xkb keymap)
  ${AWL}/third_party/include  # sys/ucred.h shim
  ${LIBFFI_DIR}/include)
target_link_libraries(waylandbridge
  PRIVATE wayland-server-static binder_ndk android log EGL GLESv3 nativewindow)
set_target_properties(waylandbridge PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${AWL_OUT}
  COMPILE_OPTIONS "-Wno-unused-parameter")   # protocol handlers have unused params
