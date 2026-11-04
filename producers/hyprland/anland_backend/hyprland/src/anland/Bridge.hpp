#pragma once
#include <aquamarine/backend/Backend.hpp>
#include <sys/types.h>
#include "../helpers/memory/Memory.hpp"
class IDataSource;
class IKeyboard;
struct wl_client;
namespace Anland {
    void beforeKeyboardEvent();
    void resetTextInput();
    SP<IKeyboard> keyboardForClient(SP<IKeyboard> keyboard, wl_client* client);
    void bind(SP<Aquamarine::CBackend> backend);
    void syncPointerCapture();
    void reportActiveClient(pid_t pid);
    size_t swapchainLength(SP<Aquamarine::IOutput> output, size_t nativeLength);
    uint32_t renderTargetFormat(SP<Aquamarine::IOutput> output);
    void selectionChanged(SP<IDataSource> source);
    void shutdownClipboard();
    void renderImportFailed(SP<Aquamarine::IOutput> output, SP<Aquamarine::IBuffer> buffer);
}
