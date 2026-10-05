# Unified Anland display producer and DRM-like userspace device layer.
# This is the single public producer-side library used by WM backends.
include("${CMAKE_CURRENT_LIST_DIR}/presentation.cmake")
set(DISPLAY_PRODUCER_SOURCES
    display_producer.c
    anland_audio.c
    anland_camera.c
    anland_device.c
    anland_buffer_registry.c)

add_library(display_producer SHARED ${DISPLAY_PRODUCER_SOURCES}
    $<TARGET_OBJECTS:anland_presentation>)
target_include_directories(display_producer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_link_libraries(display_producer PRIVATE socket_utils Threads::Threads PkgConfig::PIPEWIRE)