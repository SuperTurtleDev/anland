# Unified Anland display producer and DRM-like userspace device layer.
# This is the single public producer-side library used by WM backends.
set(DISPLAY_PRODUCER_SOURCES
    display_producer.c
    anland_audio.c
    anland_camera.c
    anland_device.c
    anland_buffer_registry.c
    anland_de_backend.c
    anland_present.c
    anland_present_legacy.c
    anland_present_awl.c
    anland_scene.c
    anland_scene_legacy.c
    anland_window_map.c
    ../common/anland_present_ipc.c)

add_library(display_producer SHARED ${DISPLAY_PRODUCER_SOURCES})
target_include_directories(display_producer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(display_producer PRIVATE socket_utils pthread PkgConfig::PIPEWIRE)
