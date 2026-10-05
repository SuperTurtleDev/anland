# One backend and one public producer foundation, shared by all version overlays.
# common/ and libdisplay_producer/ are links to the repository's canonical sources.
anland_inc = include_directories('.', 'common', 'libdisplay_producer')
anland_public = static_library('gamescope-anland-public',
  'common/socket_utils.c',
  'libdisplay_producer/display_producer.c',
  'libdisplay_producer/anland_device.c',
  'libdisplay_producer/anland_scene.c',
  'libdisplay_producer/anland_scene_legacy.c',
  'libdisplay_producer/anland_present.c',
  # One scene/device presentation path; no extra presentation service.
  'libdisplay_producer/anland_buffer_registry.c',
  'libdisplay_producer/anland_window_map.c',
  'libdisplay_producer/anland_de_backend.c',
  'libdisplay_producer/anland_audio.c',
  'libdisplay_producer/anland_camera.c',
  include_directories: anland_inc,
  dependencies: [dependency('libpipewire-0.3'), dependency('threads')],
  c_args: ['-D_GNU_SOURCE'],
  install: false)
anland_dep = declare_dependency(link_with: anland_public,
  include_directories: anland_inc,
  dependencies: [dependency('libpipewire-0.3'), dependency('threads')])
src += files('AnlandBackend.cpp', 'clipboard.cpp', 'session.c', 'input.c')