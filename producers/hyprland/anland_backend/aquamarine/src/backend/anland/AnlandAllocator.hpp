#pragma once
#include <aquamarine/backend/AnlandBackend.hpp>
#include <aquamarine/allocator/Swapchain.hpp>
#include <drm_fourcc.h>
#include "AnlandBuffer.hpp"
#include "anland_de_backend.h"

namespace Aquamarine {
    class CAnlandAllocator : public IAllocator {
      public:
        explicit CAnlandAllocator(Hyprutils::Memory::CWeakPointer<CAnlandBackend> owner) : m_owner(owner) { ; }
        bool import(const anland_de_target_t& target);
        Hyprutils::Memory::CSharedPointer<IBuffer> acquire(const SAllocatorBufferParams&, Hyprutils::Memory::CSharedPointer<CSwapchain>) override;
        Hyprutils::Memory::CSharedPointer<IBuffer> nextBuffer(int* age) override;
        Hyprutils::Memory::CSharedPointer<CBackend> getBackend() override;
        int drmFD() override { return -1; }
        eAllocatorType type() override { return AQ_ALLOCATOR_TYPE_GBM; }
        bool supportsBufferAge() override { return false; }
        uint64_t generation = 0;
        std::vector<Hyprutils::Memory::CSharedPointer<CAnlandBuffer>> buffers;
      private:
        Hyprutils::Memory::CWeakPointer<CAnlandBackend> m_owner;
        size_t m_cursor = 0;
    };
}