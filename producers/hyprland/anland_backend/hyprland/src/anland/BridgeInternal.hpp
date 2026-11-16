#pragma once
#include <aquamarine/backend/AnlandBackend.hpp>
namespace Anland {
    void setClipboardBackend(Hyprutils::Memory::CSharedPointer<Aquamarine::CAnlandBackend> owner);
    void clipboardFromConsumer(const char* bytes, size_t count, void*);
    void textFromConsumer(const char* bytes, size_t count, void*);
}

namespace Anland {
    void setPointerCaptureBackend(Hyprutils::Memory::CSharedPointer<Aquamarine::CAnlandBackend> backend);
    bool pointerCaptureRequested();
}

namespace Anland {
    void setTextBackend(Hyprutils::Memory::CSharedPointer<Aquamarine::CAnlandBackend>);
    void resetTextInput();
    void cancelTextInput();
    bool queueKeyFromConsumer(uint32_t key, bool pressed, uint32_t time);
}
