#include "Bridge.hpp"
#include "../Compositor.hpp"
#include "../managers/SeatManager.hpp"
#include "../protocols/types/DataDevice.hpp"
#include <aquamarine/backend/AnlandBackend.hpp>
#include <wayland-server-core.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>
#include <memory>

#include "BridgeInternal.hpp"

static WP<Aquamarine::CAnlandBackend> backend;
static bool incoming = false;
// SeatManager and data offers borrow their sources; keep the consumer selection alive.
static SP<IDataSource> consumerSelection;
static uint64_t selectionSerial = 0;
static constexpr size_t MAX_TEXT = 1024 * 1024;

struct Transfer {
    int fd = -1;
    wl_event_source* io = nullptr;
    wl_event_source* timer = nullptr;
    std::string text;
    size_t offset = 0;
    bool writing = false;
    uint64_t serial = 0;
    SP<IDataSource> source;
    ~Transfer() { if (io) wl_event_source_remove(io); if (timer) wl_event_source_remove(timer); if (fd >= 0) close(fd); }
};
static std::vector<std::shared_ptr<Transfer>> transfers;
static void finish(Transfer* raw, bool deliver) {
    auto it = std::ranges::find_if(transfers, [raw](const auto& item) { return item.get() == raw; });
    if (it == transfers.end()) return;
    auto transfer = *it;
    transfers.erase(it);
    if (deliver && !transfer->writing && transfer->serial == selectionSerial) {
        if (auto owner = backend.lock()) owner->sendClipboardToConsumer(transfer->text);
    }
}
static int expired(void* data) { finish(static_cast<Transfer*>(data), false); return 0; }
static int transferReady(int, uint32_t mask, void* data) {
    auto raw = static_cast<Transfer*>(data);
    auto it = std::ranges::find_if(transfers, [raw](const auto& item) { return item.get() == raw; });
    if (it == transfers.end()) return 0;
    auto transfer = *it; // keep callback storage alive even if finish erases it
    if (mask & WL_EVENT_ERROR) { finish(raw, false); return 0; }
    if (transfer->writing) {
        // Block only this thread's SIGPIPE during writes, without changing process handlers.
        sigset_t set, old; sigemptyset(&set); sigaddset(&set, SIGPIPE); pthread_sigmask(SIG_BLOCK, &set, &old);
        bool failed = false;
        for (unsigned budget=0; budget<16 && transfer->offset<transfer->text.size(); ++budget) {
            const auto n = write(transfer->fd, transfer->text.data()+transfer->offset, transfer->text.size()-transfer->offset);
            if (n > 0) { transfer->offset += n; continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            failed = true; break;
        }
        if (!sigismember(&old, SIGPIPE)) { const timespec zero{}; while (sigtimedwait(&set, nullptr, &zero) >= 0) {} }
        pthread_sigmask(SIG_SETMASK, &old, nullptr);
        if (failed || transfer->offset == transfer->text.size() || (mask & WL_EVENT_HANGUP)) finish(raw, false);
    } else {
        char bytes[8192];
        for (unsigned budget=0; budget<16; ++budget) {
            const auto n = read(transfer->fd, bytes, sizeof(bytes));
            if (n > 0) {
                if (transfer->text.size()+n > MAX_TEXT) { finish(raw, false); break; }
                transfer->text.append(bytes,n); continue;
            }
            if (n == 0) { finish(raw, true); break; }
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) finish(raw, false);
            break;
        }
    }
    return 0;
}
static bool watch(std::shared_ptr<Transfer> transfer) {
    auto loop = wl_display_get_event_loop(g_pCompositor->m_wlDisplay);
    transfer->timer = wl_event_loop_add_timer(loop, expired, transfer.get());
    transfer->io = wl_event_loop_add_fd(loop, transfer->fd, transfer->writing ? WL_EVENT_WRITABLE : WL_EVENT_READABLE, transferReady, transfer.get());
    if (!transfer->timer || !transfer->io || wl_event_source_timer_update(transfer->timer, 2000) != 0) return false;
    transfers.push_back(transfer);
    return true;
}
class CAnlandClipboardSource : public IDataSource {
  public:
    explicit CAnlandClipboardSource(std::string text) : m_text(std::move(text)) { ; }
    std::vector<std::string> mimes() override { return {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING"}; }
    void send(const std::string&, Hyprutils::OS::CFileDescriptor fd) override {
        if (transfers.size() >= 16) return;
        auto transfer = std::make_shared<Transfer>(); transfer->fd = fd.take(); transfer->text = m_text; transfer->writing = true;
        const int flags = fcntl(transfer->fd, F_GETFL);
        if (flags < 0 || fcntl(transfer->fd, F_SETFL, flags | O_NONBLOCK) < 0) return;
        watch(transfer);
    }
    void accepted(const std::string&) override { ; }
    void cancelled() override { ; }
    bool hasDnd() override { return false; }
    bool dndDone() override { return false; }
    void sendDndFinished() override { ; }
    bool used() override { return false; }
    void markUsed() override { ; }
    void error(uint32_t, const std::string&) override { ; }
    eDataSourceType type() override { return DATA_SOURCE_TYPE_WAYLAND; }
    uint32_t actions() override { return 0; }
    void sendDndDropPerformed() override { ; }
    void sendDndAction(wl_data_device_manager_dnd_action) override { ; }
  private:
    std::string m_text;
};
void Anland::selectionChanged(SP<IDataSource> source) {
    // Called after SeatManager has cancelled/replaced the previous source.
    if (source && dynamic_cast<CAnlandClipboardSource*>(source.get()))
        consumerSelection = source;
    else
        consumerSelection.reset();
    ++selectionSerial;
    std::erase_if(transfers, [](const auto& transfer) { return !transfer->writing; });
    auto owner = backend.lock();
    if (!owner || incoming || owner->inFallback() || (source && dynamic_cast<CAnlandClipboardSource*>(source.get()))) return;
    if (!source) { owner->sendClipboardToConsumer(""); return; }
    std::string mime;
    for (const auto& candidate : source->mimes()) {
        if (candidate == "text/plain;charset=utf-8") { mime=candidate; break; }
        if (candidate == "text/plain") mime=candidate;
    }
    if (mime.empty() || transfers.size() >= 16) return;
    int pipefd[2];
    // Clients may use blocking write loops. Only our read end is nonblocking.
    if (pipe2(pipefd, O_CLOEXEC) != 0) return;
    const int flags = fcntl(pipefd[0], F_GETFL);
    if (flags < 0 || fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        close(pipefd[0]); close(pipefd[1]); return;
    }
    auto transfer = std::make_shared<Transfer>(); transfer->fd=pipefd[0]; transfer->serial=selectionSerial; transfer->source=source;
    if (!watch(transfer)) { close(pipefd[1]); return; }
    source->send(mime, Hyprutils::OS::CFileDescriptor{pipefd[1]});
    wl_display_flush_clients(g_pCompositor->m_wlDisplay);
}
void Anland::setClipboardBackend(SP<Aquamarine::CAnlandBackend> owner) { backend = owner; }
void Anland::clipboardFromConsumer(const char* bytes, size_t count, void*) {
    if (!g_pSeatManager || count > MAX_TEXT || (count && !bytes)) return;
    incoming = true;
    if (count) g_pSeatManager->setCurrentSelection(makeShared<CAnlandClipboardSource>(std::string(bytes,count)));
    else g_pSeatManager->setCurrentSelection(nullptr);
    incoming = false;
}

void Anland::shutdownClipboard() {
    // Retire event sources while the Wayland loop and protocols are still alive.
    incoming = true;
    if (consumerSelection && g_pSeatManager && g_pSeatManager->m_selection.currentSelection == consumerSelection)
        g_pSeatManager->setCurrentSelection(nullptr);
    transfers.clear();
    consumerSelection.reset();
    backend.reset();
    incoming = false;
}
