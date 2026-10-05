# Production presentation foundation. Both display_producer and the isolated
# contract tests link this target, so tests exercise the same compiled objects.
# Device functions are supplied by anland_device.c in production and by the
# isolated fake device in tests; no test implementation enters the library.
if(TARGET anland_presentation)
    return()
endif()

find_package(Threads REQUIRED)
add_library(anland_presentation OBJECT
    ${CMAKE_CURRENT_LIST_DIR}/anland_present.c
    ${CMAKE_CURRENT_LIST_DIR}/anland_scene.c
    ${CMAKE_CURRENT_LIST_DIR}/anland_scene_legacy.c
    ${CMAKE_CURRENT_LIST_DIR}/anland_de_backend.c
    ${CMAKE_CURRENT_LIST_DIR}/anland_window_map.c)
set_target_properties(anland_presentation PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)
target_include_directories(anland_presentation PUBLIC ${CMAKE_CURRENT_LIST_DIR})
# Consumers link Threads::Threads explicitly (compatible with CMake 3.10).
target_compile_options(anland_presentation PRIVATE -Wall -Wextra)