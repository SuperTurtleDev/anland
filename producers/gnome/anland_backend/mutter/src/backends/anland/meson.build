# Anland backend source list — the single place that enumerates the shared
# backend and the shared DRM-like presentation layer.
#
# Every GNOME distribution overlay consumes THIS file (through the overlay
# symlinks), so adding a shared source file is a one-line change here instead of
# an edit to each distribution's mutter.patch. Distribution overlays must only
# carry version/API differences, never a second copy of a backend.
#
# Paths are relative to src/ (this file is pulled in with subdir() from
# src/meson.build) and stay plain STRINGS: src/meson.build iterates
# mutter_sources with string methods (the introspection block calls
# source.endswith('.c')), which a files() FileHolder would not support.
#
# 'libdisplay_producer' and 'common' are symlinks to the repository top level.

mutter_sources += [
  # Backend + DE-side bridges.
  'backends/anland/meta-backend-anland.c',
  'backends/anland/meta-backend-anland.h',
  'backends/anland/meta-anland-audio.c',
  'backends/anland/meta-anland-audio.h',
  'backends/anland/meta-anland-camera.c',
  'backends/anland/meta-anland-camera.h',
  'backends/anland/meta-anland-input.c',
  'backends/anland/meta-anland-input.h',
  'backends/anland/meta-anland-clipboard.c',
  'backends/anland/meta-anland-clipboard.h',

  # The shared presentation layer. Mutter drives the SAME commit/present
  # contract as KWin; only the Cogl/Clutter translation above it is
  # Mutter-specific.
  'backends/anland/libdisplay_producer/anland_device.c',
  'backends/anland/libdisplay_producer/display_producer.c',
  'backends/anland/common/socket_utils.c',
  'backends/anland/libdisplay_producer/anland_scene.c',
  'backends/anland/libdisplay_producer/anland_scene_legacy.c',
  'backends/anland/libdisplay_producer/anland_present.c',
  # One scene/device presentation path; no extra presentation service.
  'backends/anland/libdisplay_producer/anland_buffer_registry.c',
  'backends/anland/libdisplay_producer/anland_window_map.c',
  'backends/anland/libdisplay_producer/anland_de_backend.c',
  # Wire framing stays in common/protocol.h and the producer transport.
]