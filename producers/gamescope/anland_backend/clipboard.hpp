#pragma once
#include <optional>
#include <string>
#include <cstddef>
struct AnlandClipboard;
// start/set/stop are called with wlserver lock held; take is thread-safe.
AnlandClipboard *anland_clipboard_start();
void anland_clipboard_set(AnlandClipboard *, const char *, size_t);
void anland_clipboard_stop(AnlandClipboard *);
std::optional<std::string> anland_clipboard_take(AnlandClipboard *);

// Queue XWM selection text for transport; no seat access, thread-safe.
void anland_clipboard_export(AnlandClipboard *, const std::string &);
