# libawl native helper: libawlspawn.so (fork+exec with an inherited fd).
# Plumbing ONLY — no binder, no wayland (the spawned clients stay pure and
# portable). The wayland fd handoff needs a real fork: Java's Runtime.exec
# cannot pass open fds to children.
cmake_minimum_required(VERSION 3.22.1)
project(awlspawn C)

add_library(awlspawn SHARED awlspawn_jni.c)
target_link_libraries(awlspawn PRIVATE log)
