cmake_minimum_required(VERSION 3.16)
project(anland_niri_bridge C)
# Standalone build for the single niri adapter; common sources are built once
# using the repository's existing producer CMake target, never copied here.
get_filename_component(ANLAND_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_subdirectory("${ANLAND_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/common")
add_library(anland_niri_bridge SHARED anland_niri_bridge.c)
target_include_directories(anland_niri_bridge PRIVATE "${ANLAND_ROOT}/libdisplay_producer")
target_link_libraries(anland_niri_bridge PRIVATE display_producer)
# The bridge and public library are installed together. Avoid embedding a
# machine-specific build-cache path in the runtime package.
set_target_properties(anland_niri_bridge PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN")