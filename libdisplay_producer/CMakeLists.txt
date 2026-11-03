# Unified Anland display producer and DRM-like userspace device layer.
# This is the single public producer-side library used by WM backends.
include("${CMAKE_CURRENT_LIST_DIR}/producer.cmake")
anland_add_producer_target(display_producer SHARED)
# Installed development interface for Meson consumers (Mango, etc.).
# No new wire protocol or duplicate implementation is introduced here.
include(GNUInstallDirs)
install(TARGETS display_producer LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES display_producer.h anland_audio.h anland_camera.h
    anland_device.h anland_tablet.h anland_present.h anland_scene.h anland_scene_legacy.h
    anland_de_backend.h anland_window_map.h anland_buffer_registry.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/protocol.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/display-producer)
configure_file(display-producer.pc.in display-producer.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/display-producer.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
