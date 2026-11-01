#pragma once

#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/output/Output.hpp>
#include <chrono>
#include <functional>
#include <set>
#include <string>
#include <vector>

struct anland_de_backend;
struct anland_tablet_sample;

namespace Aquamarine {
    class CAnlandAllocator;
    class CAnlandOutput;
    class CAnlandPointer;
    class CAnlandKeyboard;
    class CAnlandTouch;
    class CAnlandTablet;
    class CAnlandTabletTool;

    // Owns one public backend. All frame and input operations run on AQ's dispatch thread.
    class CAnlandBackend : public IBackendImplementation {
      public:
        explicit CAnlandBackend(Hyprutils::Memory::CSharedPointer<CBackend> backend);
        ~CAnlandBackend() override;
        eBackendType type() override;
        bool start() override;
        std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>> pollFDs() override;
        int drmFD() override;
        int drmRenderNodeFD() override;
        bool dispatchEvents() override;
        uint32_t capabilities() override;
        void onReady() override;
        std::vector<SDRMFormat> getRenderFormats() override;
        std::vector<SDRMFormat> getCursorFormats() override;
        bool createOutput(const std::string& name = "") override;
        Hyprutils::Memory::CSharedPointer<IAllocator> preferredAllocator() override;
        std::vector<Hyprutils::Memory::CSharedPointer<IAllocator>> getAllocators() override;
        Hyprutils::Memory::CWeakPointer<IBackendImplementation> getPrimary() override;
        bool inFallback() const;
        bool (*queueTextKey)(uint32_t, bool, uint32_t) = nullptr;
        void (*onInputReset)() = nullptr;
        void cancelTextQueuedKeys();
        void deliverTextQueuedKey(uint32_t key, bool pressed, uint32_t time);
        size_t renderBufferCount() const;
        bool sendClipboardToConsumer(const std::string& text);
        void updateMouseCapture(bool active);
        bool (*queryMouseCapture)() = nullptr;
        // Called on AQ's dispatch thread. Ignore failures belonging to old images.
        // Transport teardown is deferred to service(), never reentered from rendering.
        bool notifyRenderImportFailure(Hyprutils::Memory::CSharedPointer<IBuffer> buffer);
        void (*onClipboardFromConsumer)(const char*, size_t, void*) = nullptr;
        void* m_clipboardFromConsumerUserdata = nullptr;
        void (*onTextFromConsumer)(const char*, size_t, void*) = nullptr;
        void* m_textFromConsumerUserdata = nullptr;
        Hyprutils::Memory::CWeakPointer<CAnlandBackend> self;

      private:
        friend class CAnlandOutput;
        friend class CAnlandAllocator;
        void service();
        void dispatchScene();
        void onInputReadable();
        void resetInput();
        void tabletFrame(const anland_tablet_sample& previous, const anland_tablet_sample& sample);
        void syncWatchers();
        void loseSession();
        void syncMouseCapture();
        bool effectiveMouseCapture() const;
        Hyprutils::Memory::CWeakPointer<CBackend> m_backend;
        anland_de_backend* m_public = nullptr;
        Hyprutils::Memory::CSharedPointer<CAnlandAllocator> m_allocator;
        Hyprutils::Memory::CSharedPointer<CAnlandOutput> m_output;
        Hyprutils::Memory::CSharedPointer<CAnlandPointer> m_pointer;
        Hyprutils::Memory::CSharedPointer<CAnlandKeyboard> m_keyboard;
        Hyprutils::Memory::CSharedPointer<CAnlandTouch> m_touch;
        Hyprutils::Memory::CSharedPointer<CAnlandTablet> m_tablet;
        std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>> m_watchers;
        Hyprutils::Memory::CSharedPointer<std::function<void()>> m_readyIdle;

        int m_renderFD = -1, m_timerFD = -1;
        bool m_ready = false, m_lost = false, m_capture = false, m_servicing = false;
        bool m_captureKnown = false, m_captureSent = false;
        uint32_t m_width = 0, m_height = 0, m_refresh = 60000;
        uint64_t m_generation = 0;
        std::set<uint32_t> m_keys, m_buttons;
        std::set<int32_t> m_touches;
        uint32_t m_payloadType = 0, m_resourceType = 0, m_resourceCount = 0;
        std::string m_payload;
        std::chrono::steady_clock::time_point m_payloadDeadline;
    };
}
