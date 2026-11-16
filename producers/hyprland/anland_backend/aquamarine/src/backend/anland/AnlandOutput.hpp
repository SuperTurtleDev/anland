#pragma once
#include "AnlandAllocator.hpp"
#include <map>

namespace Aquamarine {
    class CAnlandOutput : public IOutput {
      public:
        explicit CAnlandOutput(Hyprutils::Memory::CWeakPointer<CAnlandBackend> backend);
        ~CAnlandOutput() override;
        bool commit() override;
        bool test() override;
        Hyprutils::Memory::CSharedPointer<IBackendImplementation> getBackend() override;
        std::vector<SDRMFormat> getRenderFormats() override;
        bool pendingPageFlip() override { return m_commitID != 0; }
        bool pendingIdleFrame() override { return m_rendering; }
        void scheduleFrame(scheduleFrameReason reason = AQ_SCHEDULE_UNKNOWN) override;
        bool destroy() override;
        void onEvent(const anland_scene_event_t& event);
        void updateMode(bool forceNotification = false);
        bool resetForSession();
        void retryReleases();
        std::vector<int> releaseFDs() const;
        bool hasReleaseFault() const { return m_releaseFault; }
        anland_de_target_t m_renderTarget = {};
        bool m_rendering = false, m_repaint = true;
      private:
        Hyprutils::Memory::CWeakPointer<CAnlandBackend> m_backend;
        uint64_t m_layer = 0, m_commitID = 0;
        uint32_t m_sequence = 0;
        std::map<uint64_t, Hyprutils::Memory::CSharedPointer<CAnlandBuffer>> m_pinned;
        void applyEnabled(bool value);
        bool targetAvailable(const anland_de_target_t& target) const;
        struct SRelease {
            int fd;
            Hyprutils::Memory::CSharedPointer<CAnlandBuffer> buffer;
            std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            bool failed = false;
        };
        // A failed sync object cannot prove completion. Keep its buffer quarantined
        // until output teardown and fail closed; reconnect must not reuse it.
        bool m_releaseFault = false;
        std::vector<SRelease> m_releases;
        Hyprutils::Memory::CSharedPointer<std::function<void()>> m_idle;
    };
}