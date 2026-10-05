# Keep one transport/presentation backend; only KWin's API boundary differs.
if(PROJECT_VERSION VERSION_GREATER_EQUAL "6.6")
    target_compile_definitions(kwin PUBLIC ANLAND_KWIN_66=1)
endif()
# 6.7 changes EGL ownership, renderer initialization and output modelines.
# Keep the 6.6/older backend API paths unchanged.
if(PROJECT_VERSION VERSION_GREATER_EQUAL "6.7")
    target_compile_definitions(kwin PUBLIC ANLAND_KWIN_67=1)
endif()
target_sources(kwin PRIVATE
    anland_backend.cpp
    anland_egl_backend.cpp
    anland_input.cpp
    anland_logging.cpp
    anland_output.cpp
    # KWin-independent PipeWire engines, shared via the top-level
    # libdisplay_producer/ (reusable by other producers; no KWin API deps)
    libdisplay_producer/anland_audio.c
    libdisplay_producer/anland_camera.c
    # Unified DRM-like device layer. These are symlinks to the repo-top-level
    # libdisplay_producer/ and common/. Compiling the transport
    # sources directly into the kwin target keeps a real distro build
    # self-contained: there is no top-level `anland_device` CMake target
    # inside the unpacked KWin source tree that the overlay lands in.
    libdisplay_producer/anland_device.c
    # Window-level commit contract + the compatibility backend that presents it
    # over the legacy fullscreen transport. The backend presents frames through
    # these instead of driving the device frame path directly, so the DE side
    # matches what a future window-level backend will expose.
    libdisplay_producer/anland_scene.c
    libdisplay_producer/anland_scene_legacy.c
    libdisplay_producer/anland_present.c
    libdisplay_producer/anland_buffer_registry.c
    libdisplay_producer/anland_window_map.c
    libdisplay_producer/anland_de_backend.c
    libdisplay_producer/display_producer.c
    common/socket_utils.c
)
target_include_directories(kwin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libdisplay_producer
    ${CMAKE_CURRENT_SOURCE_DIR}/common
)
# PkgConfig::PipeWire is already resolved at the top level for the screencast plugin.
target_link_libraries(kwin PRIVATE Libdrm::Libdrm PkgConfig::PipeWire)