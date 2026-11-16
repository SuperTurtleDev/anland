# Shared recipe for public producer implementations. No WM-specific source list.
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/presentation.cmake")
set(ANLAND_PRODUCER_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(anland_add_producer_target target kind)
    if(TARGET ${target})
        message(FATAL_ERROR "Duplicate Anland producer target: ${target}")
    endif()
    if(NOT kind STREQUAL "STATIC" AND NOT kind STREQUAL "SHARED")
        message(FATAL_ERROR "Anland producer requires STATIC or SHARED linkage")
    endif()
    find_package(PkgConfig REQUIRED)
    find_package(Threads REQUIRED)
    pkg_check_modules(ANLAND_PRODUCER_PW REQUIRED IMPORTED_TARGET libpipewire-0.3)
    add_library(${target} ${kind}
        ${ANLAND_PRODUCER_SOURCE_DIR}/display_producer.c
        ${ANLAND_PRODUCER_SOURCE_DIR}/anland_audio.c
        ${ANLAND_PRODUCER_SOURCE_DIR}/anland_camera.c
        ${ANLAND_PRODUCER_SOURCE_DIR}/anland_device.c
        ${ANLAND_PRODUCER_SOURCE_DIR}/anland_buffer_registry.c
        ${ANLAND_PRODUCER_SOURCE_DIR}/../common/socket_utils.c
        $<TARGET_OBJECTS:anland_presentation>)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON
        C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${ANLAND_PRODUCER_SOURCE_DIR}>
        $<BUILD_INTERFACE:${ANLAND_PRODUCER_SOURCE_DIR}/../common>
        $<INSTALL_INTERFACE:include/display-producer>)
    target_link_libraries(${target} PRIVATE Threads::Threads PkgConfig::ANLAND_PRODUCER_PW)
endfunction()
