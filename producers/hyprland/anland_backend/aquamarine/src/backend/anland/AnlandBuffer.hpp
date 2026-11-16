#pragma once
#include <aquamarine/buffer/Buffer.hpp>
#include "anland_device.h"
#include <cstdint>
namespace Aquamarine {
    class CAnlandBuffer : public IBuffer {
      public:
        CAnlandBuffer(anland_device_fb_t fb, uint64_t generation, uint32_t slot);
        ~CAnlandBuffer() override;
        eBufferCapability caps() override { return BUFFER_CAPABILITY_NONE; }
        eBufferType type() override { return BUFFER_TYPE_DMABUF; }
        void update(const Hyprutils::Math::CRegion&) override { ; }
        bool isSynchronous() override { return false; }
        bool good() override { return m_attrs.fds[0] >= 0; }
        SDMABUFAttrs dmabuf() override { return m_attrs; }
        uint64_t m_generation;
        uint32_t m_slot;
      private:
        SDMABUFAttrs m_attrs;
    };

}
