// Reads Larian's LSPK archives.
//
// bg3le needs this for one thing: which mod defines a given stat. That is
// not recorded anywhere in memory -- upstream learns it by hooking the
// engine as each Stats/Generated/*.txt is opened, and no engine function in
// the Linux build carries a symbol to hook. The files themselves do say,
// though, in their paths, so the archives are read directly.
//
// Only version 18 single-part archives are handled, which is every archive
// that holds stats; the multi-part ones are textures.
//
// Header, then a file list at the offset it names: uint32 count, uint32
// compressed size, then an LZ4 block holding count fixed-size entries.

#include "pak.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "log.h"

extern "C" {
#include "lz4.h"
}

namespace bg3le {

namespace {

constexpr std::uint32_t kVersion = 18;
constexpr std::size_t kHeaderSize = 40;

// Guards against a corrupt header turning into a huge allocation.
constexpr std::uint32_t kMaxFiles = 4u << 20;
constexpr std::uint32_t kMaxListBytes = 1u << 28;
constexpr std::uint32_t kMaxFileBytes = 1u << 28;

#pragma pack(push, 1)
struct Entry {
    char Name[256];
    std::uint32_t OffsetLow;
    std::uint16_t OffsetHigh;
    std::uint8_t Part;
    std::uint8_t Flags;
    std::uint32_t SizeOnDisk;
    std::uint32_t UncompressedSize;
};
#pragma pack(pop)

static_assert(sizeof(Entry) == 272, "LSPK v18 file entries are 272 bytes");

// The low nibble of Flags is the compression method; the rest is its level,
// which does not matter for decoding.
constexpr std::uint8_t kMethodMask = 0x0f;
constexpr std::uint8_t kMethodNone = 0;
constexpr std::uint8_t kMethodLZ4 = 2;

std::uint64_t entry_offset(Entry const& e) {
    return (std::uint64_t)e.OffsetLow | ((std::uint64_t)e.OffsetHigh << 32);
}

bool read_at(std::FILE* f, long offset, void* out, std::size_t size) {
    if (std::fseek(f, offset, SEEK_SET) != 0) return false;
    return std::fread(out, 1, size, f) == size;
}

// One file's bytes, decompressed if it was stored that way.
bool read_entry(std::FILE* f, Entry const& e, std::vector<char>* out) {
    if (e.SizeOnDisk == 0 || e.SizeOnDisk > kMaxFileBytes) return false;
    if (e.UncompressedSize > kMaxFileBytes) return false;

    std::vector<char> raw(e.SizeOnDisk);
    if (!read_at(f, (long)entry_offset(e), raw.data(), raw.size())) {
        return false;
    }

    switch (e.Flags & kMethodMask) {
    case kMethodNone:
        // A stored entry leaves UncompressedSize at zero.
        *out = std::move(raw);
        return true;

    case kMethodLZ4: {
        out->resize(e.UncompressedSize);
        const int got = LZ4_decompress_safe(raw.data(), out->data(),
                                            (int)raw.size(),
                                            (int)out->size());
        return got == (int)e.UncompressedSize;
    }

    default:
        // zlib and zstd exist in the format but not in any archive that
        // ships stats; skipped rather than half-handled.
        return false;
    }
}

}  // namespace

bool pak_read(char const* path,
              std::function<bool(char const* name)> const& accept,
              std::function<void(char const* name, char const* data,
                                 std::size_t size)> const& sink) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;

    unsigned char header[kHeaderSize];
    if (std::fread(header, 1, sizeof(header), f) != sizeof(header)
        || std::memcmp(header, "LSPK", 4) != 0) {
        std::fclose(f);
        return false;
    }

    std::uint32_t version = 0;
    std::uint64_t listOffset = 0;
    std::uint16_t parts = 0;
    std::memcpy(&version, header + 4, sizeof(version));
    std::memcpy(&listOffset, header + 8, sizeof(listOffset));
    std::memcpy(&parts, header + 38, sizeof(parts));
    if (version != kVersion || parts != 1) {
        std::fclose(f);
        return false;
    }

    std::uint32_t count = 0;
    std::uint32_t compressed = 0;
    if (!read_at(f, (long)listOffset, &count, sizeof(count))
        || std::fread(&compressed, 1, sizeof(compressed), f)
               != sizeof(compressed)) {
        std::fclose(f);
        return false;
    }
    if (count == 0 || count > kMaxFiles || compressed == 0
        || compressed > kMaxListBytes) {
        std::fclose(f);
        return false;
    }

    std::vector<char> packed(compressed);
    if (std::fread(packed.data(), 1, packed.size(), f) != packed.size()) {
        std::fclose(f);
        return false;
    }

    std::vector<Entry> entries(count);
    const int want = (int)(count * sizeof(Entry));
    if (LZ4_decompress_safe(packed.data(), (char*)entries.data(),
                            (int)packed.size(), want) != want) {
        std::fclose(f);
        return false;
    }
    packed.clear();
    packed.shrink_to_fit();

    std::vector<char> contents;
    for (Entry const& e : entries) {
        // A name that fills the field has no terminator of its own.
        char name[sizeof(e.Name) + 1];
        std::memcpy(name, e.Name, sizeof(e.Name));
        name[sizeof(e.Name)] = '\0';

        if (!accept(name)) continue;
        if (!read_entry(f, e, &contents)) {
            logf("pak: %s: could not read %s", path, name);
            continue;
        }
        sink(name, contents.data(), contents.size());
    }

    std::fclose(f);
    return true;
}

}  // namespace bg3le
