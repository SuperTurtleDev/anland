// wlroots selection translation only. Wire transport remains in anland_device.
#include "clipboard.hpp"
#ifndef ANLAND_CLIPBOARD_SEAT_TEST
#include "wlserver.hpp"
#include "steamcompmgr.hpp"
#endif
#include "anland_device.h"
#include "wlr_begin.hpp"
#include <wlr/types/wlr_data_device.h>
#include "wlr_end.hpp"
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <mutex>
#include <vector>
#include <algorithm>

struct AnlandClipboard {
    wl_listener request{}, changed{}, seat_destroy{};
    wl_event_source *timer = nullptr;
    bool attached = true, importing = false;
    int read_fd = -1;
    std::string read_text;
    std::chrono::steady_clock::time_point read_deadline;
    struct Write { int fd; std::string text; size_t offset; std::chrono::steady_clock::time_point deadline; };
    std::vector<Write> writes;
    std::mutex mutex;
    std::optional<std::string> pending;
};
struct ClipboardSource {
    wlr_data_source base;
    AnlandClipboard *owner;
    std::string text;
};
static ssize_t safe_write(int fd, const void *buf, size_t size)
{
    // Do not change Gamescope's process-wide SIGPIPE policy.
    sigset_t mask, old, pending;
    sigemptyset(&mask); sigaddset(&mask, SIGPIPE);
    int rc = pthread_sigmask(SIG_BLOCK, &mask, &old);
    if (rc) { errno = rc; return -1; }
    sigpending(&pending);
    bool was_pending = sigismember(&pending, SIGPIPE);
    ssize_t n = write(fd, buf, size);
    int error = errno;
    if (n < 0 && error == EPIPE && !was_pending) {
        timespec zero{};
        while (sigtimedwait(&mask, nullptr, &zero) < 0 && errno == EINTR) {}
    }
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
    errno = error;
    return n;
}
static void queue_text(AnlandClipboard *c, std::string text)
{
    std::lock_guard<std::mutex> guard(c->mutex);
    c->pending = std::move(text);
    nudge_steamcompmgr();
}
static void cancel_read(AnlandClipboard *c)
{
    if (c->read_fd >= 0) close(c->read_fd);
    c->read_fd = -1;
    c->read_text.clear();
}
static int tick(void *data)
{
    auto *c = static_cast<AnlandClipboard *>(data);
    auto now = std::chrono::steady_clock::now();
    if (c->read_fd >= 0) {
        // Bounded work per event-loop dispatch, no blocking seat or renderer.
        char buf[8192];
        for (int budget = 0; budget < 8; ++budget) {
            ssize_t n = read(c->read_fd, buf, sizeof(buf));
            if (n > 0) {
                if (c->read_text.size() + size_t(n) > ANLAND_DEVICE_MAX_PAYLOAD_SIZE) {
                    cancel_read(c); break;
                }
                c->read_text.append(buf, size_t(n));
            } else if (!n) {
                queue_text(c, std::move(c->read_text)); cancel_read(c); break;
            } else if (errno != EAGAIN && errno != EINTR) { cancel_read(c); break; }
            else break;
        }
        if (c->read_fd >= 0 && now >= c->read_deadline) cancel_read(c);
    }
    for (auto it = c->writes.begin(); it != c->writes.end();) {
        bool done = now >= it->deadline;
        if (!done) {
            size_t count = std::min(size_t(65536), it->text.size() - it->offset);
            ssize_t n = safe_write(it->fd, it->text.data() + it->offset, count);
            if (n > 0) it->offset += size_t(n);
            if (n < 0 && errno != EAGAIN && errno != EINTR) done = true;
            if (it->offset == it->text.size()) done = true;
        }
        if (done) { close(it->fd); it = c->writes.erase(it); }
        else ++it;
    }
    if (c->read_fd >= 0 || !c->writes.empty()) wl_event_source_timer_update(c->timer, 10);
    return 0;
}
static void source_send(wlr_data_source *base, const char *, int32_t fd)
{
    auto *s = reinterpret_cast<ClipboardSource *>(base);
    auto *c = s->owner;
    int flags = fcntl(fd, F_GETFL);
    if (!c->attached || c->writes.size() >= 16 || flags < 0 ||
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) { close(fd); return; }
    c->writes.push_back({fd, s->text, 0, std::chrono::steady_clock::now() + std::chrono::seconds(5)});
    wl_event_source_timer_update(c->timer, 1);
}
static void source_destroy(wlr_data_source *base)
{
    delete reinterpret_cast<ClipboardSource *>(base);
}
static const wlr_data_source_impl source_impl = { .send = source_send, .destroy = source_destroy };
static void request(wl_listener *l, void *data)
{
    AnlandClipboard *c = wl_container_of(l, c, request);
    if (!c->attached) return;
    auto *ev = static_cast<wlr_seat_request_set_selection_event *>(data);
    wlr_seat_set_selection(wlserver.wlr.seat, ev->source, ev->serial);
}
static void changed(wl_listener *l, void *)
{
    AnlandClipboard *c = wl_container_of(l, c, changed);
    cancel_read(c);
    { std::lock_guard<std::mutex> guard(c->mutex); c->pending.reset(); }
    if (c->importing) return;
    auto *source = wlserver.wlr.seat->selection_source;
    if (!source) { queue_text(c, {}); return; }
    if (source->impl == &source_impl) return; // no Android -> seat -> Android echo
    const char *mime = nullptr;
    const char *preferred[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain"};
    auto **entries = static_cast<char **>(source->mime_types.data);
    for (const char *candidate : preferred) {
        for (size_t i = 0; i < source->mime_types.size / sizeof(char *); ++i)
            if (!strcmp(entries[i], candidate)) { mime = candidate; break; }
        if (mime) break;
    }
    if (!mime) return;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0) return;
    // Only the compositor read end is nonblocking; do not impose EAGAIN
    // semantics on an application's clipboard writer.
    if (fcntl(fds[0], F_SETFL, O_NONBLOCK) < 0) { close(fds[0]); close(fds[1]); return; }
    c->read_fd = fds[0];
    c->read_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    wlr_data_source_send(source, mime, fds[1]);
    wl_event_source_timer_update(c->timer, 1);
}
static void detach(AnlandClipboard *c)
{
    if (!c->attached) return;
    c->attached = false;
    wl_list_remove(&c->request.link); wl_list_remove(&c->changed.link);
    wl_list_remove(&c->seat_destroy.link);
    if (c->timer) { wl_event_source_remove(c->timer); c->timer = nullptr; }
    cancel_read(c);
    for (auto &w : c->writes) close(w.fd);
    c->writes.clear();
}
static void seat_destroy(wl_listener *l, void *)
{
    AnlandClipboard *c = wl_container_of(l, c, seat_destroy);
    detach(c);
}
AnlandClipboard *anland_clipboard_start()
{
    auto *c = new AnlandClipboard;
    c->timer = wl_event_loop_add_timer(wlserver.event_loop, tick, c);
    if (!c->timer) { delete c; return nullptr; }
    c->request.notify = request; c->changed.notify = changed; c->seat_destroy.notify = seat_destroy;
    wl_signal_add(&wlserver.wlr.seat->events.request_set_selection, &c->request);
    wl_signal_add(&wlserver.wlr.seat->events.set_selection, &c->changed);
    wl_signal_add(&wlserver.wlr.seat->events.destroy, &c->seat_destroy);
    return c;
}
void anland_clipboard_set(AnlandClipboard *c, const char *text, size_t size)
{
    if (!c || !c->attached || size > ANLAND_DEVICE_MAX_PAYLOAD_SIZE) return;
    ClipboardSource *s = nullptr;
    if (size) {
        s = new ClipboardSource{};
        s->owner = c; s->text.assign(text, size);
        wlr_data_source_init(&s->base, &source_impl);
        for (const char *mime : {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain"}) {
            auto **entry = static_cast<char **>(wl_array_add(&s->base.mime_types, sizeof(char *)));
            if (!entry) { wlr_data_source_destroy(&s->base); return; }
            *entry = strdup(mime);
            if (!*entry) { s->base.mime_types.size -= sizeof(char *); wlr_data_source_destroy(&s->base); return; }
        }
    }
    // An import supersedes a stale application export queued on the other thread.
    { std::lock_guard<std::mutex> guard(c->mutex); c->pending.reset(); }
    c->importing = true;
    wlr_seat_set_selection(wlserver.wlr.seat, s ? &s->base : nullptr, wl_display_next_serial(wlserver.display));
    c->importing = false;
}
std::optional<std::string> anland_clipboard_take(AnlandClipboard *c)
{
    if (!c) return {};
    std::lock_guard<std::mutex> guard(c->mutex);
    auto text = std::move(c->pending); c->pending.reset(); return text;
}
void anland_clipboard_stop(AnlandClipboard *c)
{
    if (!c) return;
    if (c->attached) {
        auto *source = wlserver.wlr.seat->selection_source;
        if (source && source->impl == &source_impl) {
            c->importing = true;
            wlr_seat_set_selection(wlserver.wlr.seat, nullptr, wl_display_next_serial(wlserver.display));
        }
        detach(c);
    }
    delete c;
}
void anland_clipboard_export(AnlandClipboard *c, const std::string &text)
{
    if (c && text.size() <= ANLAND_DEVICE_MAX_PAYLOAD_SIZE) queue_text(c, text);
}
