// The line under the copyright on the main menu, saying the extender
// loaded.
//
// bg3se appends to the game's own version string: it reads the translated
// string for handle h5b6e4138g2cf0g4d67gb825gee416cf8c54f -- the
// copyright line -- adds its own line, and writes it back through
// TranslatedStringRepository::UpdateTranslatedString.
//
// bg3le reads its localisation from the archives rather than from that
// repository, so it has nothing to write back through. What it has
// instead is a very exact anchor: the string is known, and so is its
// length. An LSStringView is a pointer and a size, so a view whose size is
// that length and whose bytes are that text is the repository's entry for
// this handle and nothing else. There is no fingerprint here and no
// guessing -- the check is the whole string.

#include <stdafx.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../log.h"
#include "../mem.h"

extern "C" bool bg3le_scannable_region(char const* line,
                                       unsigned long long* from,
                                       unsigned long long* to);

namespace bg3le {

extern "C" char const* bg3le_loca_get(char const* handle);

namespace {

// The main menu's copyright line.
constexpr char const* kVersionHandle =
    "h5b6e4138g2cf0g4d67gb825gee416cf8c54f";

// The views that were patched, so they can be re-checked cheaply. bg3se
// rewrites the string as the client leaves GameState::LoadModule, before
// the menu is built; bg3le has no game-state hook, so instead it keeps the
// write in place -- if the engine rebuilds the pool the check notices and
// writes it again.
std::vector<std::uint64_t> patchedViews;

template <class T>
bool read_as(void const* addr, T* out) {
    return safe_read(addr, out, sizeof(T));
}

}  // namespace

// Appends bg3le's own line to the menu's version text. Returns how many
// views were updated; the repository keeps several pools, and the menu
// reads whichever one its language resolves to, so every view holding this
// exact string is updated rather than guessing which.
extern "C" std::size_t bg3le_version_text_install() {
    // Off unless asked for, because it crashed the game on exit.
    //
    // The write points an engine-owned LSStringView at a buffer this
    // library allocated. The repository frees its strings on shutdown, so
    // it then frees a pointer it never allocated -- which is exactly what
    // happened. It also never worked: the write lands and reads back
    // correctly, and the menu still shows the original line, so the menu
    // is not reading through the view that was patched.
    //
    // Doing this properly means what bg3se does: find
    // ls::TranslatedStringRepository and call its own
    // UpdateTranslatedString, so the string is allocated and owned by the
    // engine and whatever the UI caches is invalidated with it. Until
    // then this stays off, because a cosmetic line on the menu is not
    // worth a crash on close.
    if (std::getenv("BG3LE_MENU_TEXT") == nullptr) return 0;

    char const* original = bg3le_loca_get(kVersionHandle);
    if (original == nullptr || original[0] == '\0') return 0;

    const std::size_t length = std::strlen(original);
    if (length < 16) return 0;

    // Already ours, from an earlier attempt.
    if (std::strstr(original, "bg3le") != nullptr) return 0;

    // Leaked on purpose: the engine will hold this pointer for as long as
    // the process lives, so it must not be freed.
    static std::string* replacement = nullptr;
    if (replacement == nullptr) {
        replacement = new std::string(
            std::string(original) + "\r\nbg3le loaded, Script Extender v32 "
            "API, built on " __DATE__ " " __TIME__ ".");
    }

    // Already patched: verify rather than scan. This runs every warming
    // round, and a full scan each time would be absurd.
    if (!patchedViews.empty()) {
        std::size_t intact = 0;
        std::vector<char> back(replacement->size(), '\0');
        for (std::uint64_t view : patchedViews) {
            std::uint64_t data = 0;
            std::uint64_t size = 0;
            auto const* at = (char*)(std::uintptr_t)view;
            if (read_as(at, &data) && read_as(at + 8, &size)
                && size == replacement->size()
                && safe_read((void const*)(std::uintptr_t)data, back.data(),
                             back.size())
                && std::memcmp(back.data(), replacement->data(),
                               back.size()) == 0) {
                ++intact;
                continue;
            }

            // The engine put its own string back; put ours in again.
            char const* value = replacement->c_str();
            const std::uint64_t newSize = replacement->size();
            if (safe_write((void*)at, &value, sizeof(value))
                && safe_write((void*)(at + 8), &newSize, sizeof(newSize))) {
                ++intact;
                logf("version text: reapplied after the engine reset it");
            }
        }
        if (intact > 0) return intact;

        // None of them survived, so the pool itself was replaced: scan
        // again below.
        patchedViews.clear();
    }

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return 0;

    constexpr std::size_t kChunk = 1u << 20;
    constexpr std::size_t kView = 16;
    static std::vector<unsigned char> block;
    block.resize(kChunk + kView);

    std::vector<char> text(length);
    char line[512];
    std::size_t patched = 0;

    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (!bg3le_scannable_region(line, &from, &to)) continue;

        for (unsigned long long base = from; base < to; base += kChunk) {
            std::size_t want = (std::size_t)(to - base);
            if (want > kChunk + kView) want = kChunk + kView;
            const std::size_t got =
                safe_read_some((void const*)base, block.data(), want);
            if (got < kView) continue;
            scan_yield();

            for (std::size_t off = 0; off + kView <= got; off += 8) {
                // Size first: it is the cheap half, and it rejects
                // everything but a view of exactly this length.
                std::uint64_t size = 0;
                std::memcpy(&size, block.data() + off + 8, sizeof(size));
                if (size != length) continue;

                std::uint64_t data = 0;
                std::memcpy(&data, block.data() + off, sizeof(data));
                if (data < 0x10000 || data > 0x800000000000ull) continue;

                if (!safe_read((void const*)(std::uintptr_t)data,
                               text.data(), length)) {
                    continue;
                }
                if (std::memcmp(text.data(), original, length) != 0) continue;

                // The whole string matched, so this is the entry. Point it
                // at ours.
                auto const* at = (char*)(std::uintptr_t)(base + off);
                char const* value = replacement->c_str();
                const std::uint64_t newSize = replacement->size();
                if (!safe_write((void*)at, &value, sizeof(value))) continue;
                if (!safe_write((void*)(at + 8), &newSize, sizeof(newSize))) {
                    continue;
                }

                // Read it back rather than trusting the write: this is the
                // one place bg3le modifies the game's memory, and a write
                // that silently did nothing would leave the menu looking
                // exactly as it does when the extender failed to load.
                std::uint64_t checkData = 0;
                std::uint64_t checkSize = 0;
                std::vector<char> back(replacement->size() + 1, '\0');
                if (!read_as(at, &checkData) || !read_as(at + 8, &checkSize)
                    || checkSize != newSize
                    || !safe_read((void const*)(std::uintptr_t)checkData,
                                  back.data(), replacement->size())
                    || std::memcmp(back.data(), replacement->data(),
                                   replacement->size()) != 0) {
                    logf("version text: the write did not take at %p", at);
                    continue;
                }

                if (patched == 0) {
                    logf("version text: now reads \"%s\"", back.data());
                }
                patchedViews.push_back(base + off);
                ++patched;
            }
        }
    }
    std::fclose(maps);

    if (patched > 0) {
        logf("version text: added bg3le's line to the menu version string "
             "(%zu views)", patched);
    } else {
        static bool said = false;
        if (!said) {
            said = true;
            logf("version text: the menu version string was not found in "
                 "memory; the menu will not say bg3le loaded");
        }
    }
    return patched;
}

}  // namespace bg3le
