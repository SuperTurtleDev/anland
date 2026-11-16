# Self-contained snapshot generated from the repository's single public source.
# Public objects live in one PIC archive, never in the WM source glob.
include("${CMAKE_CURRENT_LIST_DIR}/public/libdisplay_producer/producer.cmake")
anland_add_producer_target(anland_producer STATIC)
add_library(Anland::Producer ALIAS anland_producer)
# Upstream Aquamarine uses the plain target_link_libraries signature.
target_link_libraries(aquamarine Anland::Producer)
