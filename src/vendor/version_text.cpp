// The line under the copyright on the main menu, saying bg3le loaded.
//
// bg3se appends to the game's own version string: it reads the translated
// string for handle h5b6e4138g2cf0g4d67gb825gee416cf8c54f -- the menu's
// copyright line -- adds its own line and writes it back through
// TranslatedStringRepository::UpdateTranslatedString.
//
// bg3le has no symbol for that repository or that method, so it edits the
// string where the repository keeps it. Two places hold it and both are
// patched, because which one the menu reads is not something to guess at:
//
//   - the LSStringView values of a pool's Texts map: a pointer and an
//     eight-byte length;
//   - the STDString objects a pool owns: Larian's sixteen-byte string,
//     which in heap form is a pointer, a four-byte length and a four-byte
//     capacity whose top bit marks it as heap-allocated.
//
// Both are identified by content, not by shape: the text has to be the
// whole 69-byte copyright line, byte for byte. Nothing else matches that.
//
// The replacement is allocated with the game's own allocator. An earlier
// version used ::new, and the repository freeing its strings on shutdown
// then freed a pointer it had never allocated -- which crashed the game on
// close. Anything handed to the engine has to come from the engine's heap.
//
// It reapplies rather than patching once, and a watcher thread catches the
// string within a fraction of a second of it appearing.
//
// None of which is enough. The interface resolves this string into its own
// copy -- rendered text is UTF-16 there -- and after that the source is
// just data nobody reads. There is exactly one copy of the UTF-8 string in
// the address space, it is patched correctly, and the menu shows the
// original.
//
// bg3se does not edit the source. It calls
// TranslatedStringRepository::UpdateTranslatedString, which goes through
// the engine and takes whatever the interface caches with it. Getting
// there needs that method's address, and no engine function in this build
// carries a symbol -- so it needs code pattern-matching against the
// image, the same capability the manager lookups want. Until that exists
// this stays behind BG3LE_MENU_TEXT=1.

#include <stdafx.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "../log.h"
#include "../mem.h"

extern "C" bool bg3le_scannable_region(char const* line,
                                       unsigned long long* from,
                                       unsigned long long* to);

namespace bg3le {

extern "C" bool bg3le_game_allocator_ready();

namespace {

// Larian's string marks the heap form with the top bit of its capacity.
constexpr std::uint32_t kHeapFlag = 0x80000000u;

// One replacement per distinct original, kept so the same text is not
// allocated twice and so a reapply finds the same pointer.
std::vector<std::pair<std::string, char const*>> replacements;

// The part of the line that identifies it, and the lengths worth reading.
constexpr char const* kMarker = "Larian Studios and Wizards";
constexpr std::size_t kMinText = 24;
constexpr std::size_t kMaxText = 512;

template <class T>
bool read_as(void const* addr, T* out) {
    return safe_read(addr, out, sizeof(T));
}

// The engine's heap, so the engine can free this legitimately.
char const* build_replacement(char const* original) {
    for (auto const& entry : replacements) {
        if (entry.first == original) return entry.second;
    }
    if (!bg3le_game_allocator_ready()) return nullptr;

    const std::string text =
        std::string(original)
        + "\r\nbg3le loaded, Script Extender v32 API, built on "
        __DATE__ " " __TIME__ ".";

    auto* buffer = (char*)bg3se::GameAllocRaw(text.size() + 1);
    if (buffer == nullptr) return nullptr;
    std::memcpy(buffer, text.c_str(), text.size() + 1);

    replacements.emplace_back(original, buffer);
    return buffer;
}

// Counts UTF-16 copies of the line and shows what surrounds them, so the
// interface's own string can be found rather than guessed at.
}  // namespace

// Patches every copy of the menu's version string. Returns how many it
// changed this time round; zero once they all already say bg3le.
extern "C" std::size_t bg3le_version_text_install() {
    // No localisation index needed. It used to wait for one -- 232,878
    // entries parsed out of the archives, seven seconds -- and those seven
    // seconds are what lost the race: the menu's interface resolves this
    // string into its own copy, and once it has, editing the source
    // changes nothing on screen. The marker identifies the string by
    // itself, so this can run within a second of load.
    if (!bg3le_game_allocator_ready()) return 0;

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return 0;

    constexpr std::size_t kChunk = 1u << 20;
    constexpr std::size_t kSlack = 16;
    static std::vector<unsigned char> block;
    block.resize(kChunk + kSlack);

    std::vector<char> text;
    char line[512];
    std::size_t views = 0;
    std::size_t strings = 0;

    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (!bg3le_scannable_region(line, &from, &to)) continue;

        for (unsigned long long base = from; base < to; base += kChunk) {
            std::size_t want = (std::size_t)(to - base);
            if (want > kChunk + kSlack) want = kChunk + kSlack;
            const std::size_t got =
                safe_read_some((void const*)base, block.data(), want);
            if (got < kSlack) continue;
            scan_yield();

            for (std::size_t off = 0; off + kSlack <= got; off += 8) {
                std::uint64_t data = 0;
                std::memcpy(&data, block.data() + off, sizeof(data));
                if (data < 0x10000 || data > 0x800000000000ull) continue;

                // An LSStringView's length is eight bytes; a Larian
                // string's is four, with a capacity after it. Both start
                // with the pointer, so one read of the text settles
                // whether this is the string at all, and the length field
                // says which of the two it is.
                std::uint64_t wide = 0;
                std::uint32_t narrow = 0;
                std::uint32_t capacity = 0;
                std::memcpy(&wide, block.data() + off + 8, sizeof(wide));
                std::memcpy(&narrow, block.data() + off + 8, sizeof(narrow));
                std::memcpy(&capacity, block.data() + off + 12,
                            sizeof(capacity));

                // Any plausible length, not just the one this handle's
                // text happens to have: the menu may read a different
                // handle whose text differs from it invisibly -- a
                // non-breaking space, a trailing character -- and an exact
                // 69-byte compare would miss it while the screen looks
                // identical.
                const bool asView = wide >= kMinText && wide <= kMaxText;
                const bool asString = narrow >= kMinText
                                      && narrow <= kMaxText
                                      && (capacity & kHeapFlag) != 0;
                if (!asView && !asString) continue;

                const std::size_t span = asView ? (std::size_t)wide
                                                : (std::size_t)narrow;
                text.assign(span + 1, '\0');
                if (!safe_read((void const*)(std::uintptr_t)data,
                               text.data(), span)) {
                    continue;
                }
                // The distinctive part of the line, so a variant still
                // matches.
                if (std::strstr(text.data(), kMarker) == nullptr) continue;
                // Already ours.
                if (std::strstr(text.data(), "bg3le") != nullptr) continue;

                logf("version text: found a %zu-byte copy: \"%s\"", span,
                     text.data());

                char const* replacement = build_replacement(text.data());
                if (replacement == nullptr) continue;
                const std::size_t replacementSize = std::strlen(replacement);

                // The whole line matched. Point it at ours.
                auto* at = (char*)(std::uintptr_t)(base + off);
                if (!safe_write(at, &replacement, sizeof(replacement))) {
                    continue;
                }

                if (asView) {
                    const std::uint64_t size = replacementSize;
                    if (safe_write(at + 8, &size, sizeof(size))) ++views;
                } else {
                    const std::uint32_t size = (std::uint32_t)replacementSize;
                    const std::uint32_t cap = size | kHeapFlag;
                    if (safe_write(at + 8, &size, sizeof(size))
                        && safe_write(at + 12, &cap, sizeof(cap))) {
                        ++strings;
                    }
                }
            }
        }
    }
    std::fclose(maps);

    if (views + strings > 0) {
        logf("version text: patched %zu views and %zu strings", views,
             strings);
    }
    return views + strings;
}

}  // namespace bg3le
