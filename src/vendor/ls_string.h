#pragma once

// Larian's string, as it is laid out in the Linux build.
//
// Sixteen bytes: up to fifteen characters inline with the length in the last
// byte, or a pointer followed by size and capacity with the top bit of the
// capacity marking the heap form. Not std::string -- that is thirty-two
// bytes here, and assuming it is what made sizeof(Module) disagree with the
// stride actually found in memory.

#include <cstdint>
#include <cstring>
#include <string>

#include "../mem.h"

namespace bg3le {

inline bool read_ls_string(void const* addr, std::string* out) {
    unsigned char raw[16] = {};
    if (!safe_read(addr, raw, sizeof(raw))) return false;

    if ((raw[15] & 0x80) == 0) {
        const std::size_t size = raw[15];
        if (size > 15) return false;
        out->assign((char const*)raw, size);
        return true;
    }

    std::uint64_t buffer = 0;
    std::uint32_t size = 0;
    std::memcpy(&buffer, raw, sizeof(buffer));
    std::memcpy(&size, raw + 8, sizeof(size));
    if (buffer == 0 || size > (1u << 20)) return false;

    out->assign(size, '\0');
    if (size == 0) return true;
    return safe_read((void const*)(std::uintptr_t)buffer, out->data(), size);
}

}  // namespace bg3le
