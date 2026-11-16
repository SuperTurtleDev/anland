#include "AnlandBuffer.hpp"
#include <unistd.h>
using namespace Aquamarine;
CAnlandBuffer::CAnlandBuffer(anland_device_fb_t fb, uint64_t generation, uint32_t slot) : m_generation(generation), m_slot(slot) {
    size = {double(fb.width), double(fb.height)};
    m_attrs.success = true;
    m_attrs.size = size;
    m_attrs.format = fb.format;
    m_attrs.modifier = fb.modifier;
    m_attrs.fds[0] = fb.fd; // adopt caller-owned CLOEXEC duplicate
    m_attrs.strides[0] = fb.stride;
    m_attrs.offsets[0] = fb.offset;
}
CAnlandBuffer::~CAnlandBuffer() {
    if (m_attrs.fds[0] >= 0) close(m_attrs.fds[0]);
    events.destroy.emit();
}
