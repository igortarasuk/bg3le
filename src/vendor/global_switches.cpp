// ls::GlobalSwitches, found by its own contents.
//
// The engine's settings object: language, UI scale, mouse and controller
// sensitivities, the twelve sound settings. bg3se reaches it through
// gStaticSymbols.ls__GlobalSwitches, a pointer it recovers by pattern-scanning
// a Windows image; there is no symbol for it here and nothing of Larian's is
// hookable by name, so it has to be found by content like everything else.
//
// The anchor is `Language`, and it is a good one. It is one of Larian's
// sixteen-byte strings (see CoreLib/Base/LSString.h) holding a short value, so
// a language of "English" is exactly this in memory:
//
//     45 6e 67 6c 69 73 68 00 00 00 00 00 00 00 00 07
//     E  n  g  l  i  s  h                          len
//
// Fifteen characters inline and the length in the last byte. That is a
// sixteen-byte needle with no wildcards, which is far more specific than
// scanning for the text alone.
//
// Several objects hold such a string, so a hit is a candidate rather than an
// answer -- thirty-one of them in one run, and the first version of this took
// the first whose UIScaling happened to fall in a plausible range. It
// reported an object whose language read "Japanese" on an English install.
//
// So the check is the layout itself, not a guess about values. GlobalSwitches
// declares seventy-odd boolean members, and a bool is 0 or 1; anything else at
// that offset is not that field. Subtract the declared offset of `Language`
// from a hit to get a candidate base, then require *every* declared boolean to
// read 0 or 1. A wrong base has to pass seventy independent one-bit tests,
// which is not something a coincidence does -- and if bg3se's declared offsets
// were wrong for this build, the real object would fail too and nothing would
// be reported, which is the answer that should be given in that case.
//
// The definition is by Norbyte and the bg3se contributors
// (https://github.com/Norbyte/bg3se); locating it is ours.

#include <stdafx.h>

#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>

#include "../component_meta_abi.h"
#include "../log.h"
#include "../mem.h"
#include "cache_lock.h"

extern "C" bool bg3le_scannable_region(char const* line,
                                       unsigned long long* from,
                                       unsigned long long* to);
extern "C" void const* bg3le_meta_class(char const* className);
extern "C" bool bg3le_meta_field(void const* handle, char const* path,
                                 std::uint32_t* offset, std::uint16_t* size,
                                 std::uint8_t* kind, std::uint8_t* elemKind,
                                 std::uint16_t* elemCount);
extern "C" std::size_t bg3le_meta_fields_at(void const* handle,
                                           char const* path,
                                           char const** names,
                                           std::uint8_t* kinds,
                                           std::size_t max);

namespace bg3le {

namespace {

// The languages the game ships, longest first so a prefix cannot shadow a
// longer name. Only the ones that fit inline are usable as a needle, which is
// all of them: the longest is "ChineseTraditional" at eighteen, and that one
// is excluded below rather than guessed at.
char const* const kLanguages[] = {
    "English", "French", "German", "Italian", "Polish", "Russian",
    "Spanish", "Turkish", "Ukrainian", "Japanese", "Korean", "Chinese",
    "Portuguese",
};

// One inline sixteen-byte string holding this text, as bytes.
void needle_for(char const* text, unsigned char out[16], bool* usable) {
    const std::size_t length = std::strlen(text);
    *usable = length > 0 && length <= 15;
    if (!*usable) return;

    std::memset(out, 0, 16);
    std::memcpy(out, text, length);
    out[15] = (unsigned char)length;
}

struct BoolField {
    char const* Name{nullptr};
    std::uint32_t Offset{0};
};

struct Offsets {
    bool Ok{false};
    std::uint32_t Language{0};
    std::vector<BoolField> Bools;
    std::vector<BoolField> Floats;
};

// Where `Language` sits and where every boolean does, from bg3le's own field
// table for the class.
Offsets declared() {
    Offsets out;
    void const* meta = bg3le_meta_class("GlobalSwitches");
    if (meta == nullptr) return out;

    std::uint16_t size = 0;
    std::uint8_t kind = 0;
    std::uint8_t elemKind = 0;
    std::uint16_t elemCount = 0;
    if (!bg3le_meta_field(meta, "Language", &out.Language, &size, &kind,
                          &elemKind, &elemCount)) {
        return out;
    }

    constexpr std::size_t kMax = 512;
    char const* names[kMax] = {};
    std::uint8_t kinds[kMax] = {};
    const std::size_t count =
        bg3le_meta_fields_at(meta, nullptr, names, kinds, kMax);

    for (std::size_t i = 0; i < count; ++i) {
        const bool isBool = kinds[i] == (std::uint8_t)FieldKind::Bool;
        const bool isFloat = kinds[i] == (std::uint8_t)FieldKind::Float;
        if (!isBool && !isFloat) continue;

        std::uint32_t offset = 0;
        if (!bg3le_meta_field(meta, names[i], &offset, &size, &kind, &elemKind,
                              &elemCount)) {
            continue;
        }
        if (isBool) {
            out.Bools.push_back(BoolField{names[i], offset});
        } else {
            out.Floats.push_back(BoolField{names[i], offset});
        }
    }

    // Enough of each for the test to mean something.
    out.Ok = out.Bools.size() >= 20 && out.Floats.size() >= 10;
    if (!out.Ok) {
        logf("global switches: %zu boolean and %zu float members found; not "
             "enough to identify the object safely", out.Bools.size(),
             out.Floats.size());
    }
    return out;
}

// Whether every declared float reads as a settings value.
//
// This is the test that actually discriminates, and the reason is that a
// boolean is one bit: an object full of small bytes passes eighty of ninety
// one boolean checks by luck, and one did -- 82 of 91 at a base whose
// UIScaling read 1060 and whose MouseSensitivity read -1158458304, which is a
// float's bit pattern seen as an integer.
//
// A float is thirty-two bits with almost all of them meaningless. A setting is
// a small finite number; random bytes are overwhelmingly NaN, infinite,
// denormal or astronomically large. Twenty-odd of those in a row is a real
// test where ninety one one-bit tests are not.
bool floats_agree(void const* base, Offsets const& at) {
    for (BoolField const& field : at.Floats) {
        float value = 0.0f;
        if (!safe_read((char const*)base + field.Offset, &value, 4)) {
            return false;
        }
        if (!std::isfinite(value)) return false;
        const float magnitude = value < 0.0f ? -value : value;
        if (magnitude != 0.0f && (magnitude < 1e-6f || magnitude > 1e6f)) {
            return false;
        }
    }
    return true;
}

// How many of the declared booleans read 0 or 1 at this base.
//
// A count rather than a yes or no, because the count is the diagnosis. If the
// best candidate in the process passes nearly all of them, the object is there
// and one or two offsets are wrong; if the best passes about half, nothing at
// any of those addresses is this struct -- half is what random bytes give,
// since a byte is 0 or 1 about one time in 128 but a settings object is full
// of small values.
std::size_t booleans_agreeing(void const* base, Offsets const& at,
                              std::vector<BoolField>* disagreed = nullptr) {
    std::size_t agreed = 0;
    for (BoolField const& field : at.Bools) {
        std::uint8_t value = 0;
        if (!safe_read((char const*)base + field.Offset, &value, 1)) break;
        if (value > 1) {
            if (disagreed != nullptr) disagreed->push_back(field);
            continue;
        }
        ++agreed;
    }
    return agreed;
}

void* g_switches = nullptr;
bool g_searched = false;

void* search() {
    const Offsets at = declared();
    if (!at.Ok) {
        logf("global switches: no field table for GlobalSwitches; not "
             "searching");
        return nullptr;
    }

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return nullptr;

    constexpr std::size_t kChunk = 1u << 20;
    constexpr std::size_t kOverlap = 16;
    std::vector<unsigned char> block(kChunk + kOverlap);

    std::size_t hits = 0;
    void* found = nullptr;
    std::size_t best = 0;
    void const* bestAt = nullptr;
    char const* bestLanguage = nullptr;

    char line[512];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (!bg3le_scannable_region(line, &from, &to)) continue;

        for (unsigned long long base = from; base < to; base += kChunk) {
            const std::size_t want =
                (std::size_t)std::min<unsigned long long>(kChunk + kOverlap,
                                                          to - base);
            std::size_t got = safe_read_some((void const*)(std::uintptr_t)base,
                                             block.data(), want);
            if (got < 16) continue;

            for (char const* language : kLanguages) {
                unsigned char needle[16] = {};
                bool usable = false;
                needle_for(language, needle, &usable);
                if (!usable) continue;

                for (std::size_t i = 0; i + 16 <= got; i += 8) {
                    if (std::memcmp(block.data() + i, needle, 16) != 0) {
                        continue;
                    }
                    ++hits;

                    // The needle is the Language member, so the object starts
                    // that far back.
                    const unsigned long long candidate =
                        base + i - at.Language;
                    if (candidate > base + i) continue;  // wrapped
                    auto* object = (void*)(std::uintptr_t)candidate;
                    if (!floats_agree(object, at)) continue;

                    const std::size_t agreed = booleans_agreeing(object, at);
                    if (agreed > best) {
                        best = agreed;
                        bestAt = object;
                        bestLanguage = language;
                    }
                }
            }
        }
    }
    std::fclose(maps);

    // Every declared boolean, now that the floats have done the
    // discriminating. Nine in ten was tried and was not enough -- it accepted
    // a base whose UIScaling read 1060.
    //
    // Every candidate is scored and the best one wins rather than the first to
    // pass, so a near miss cannot beat the real object.
    if (best == at.Bools.size() && best > 0) {
        found = const_cast<void*>(bestAt);
        logf("global switches: at %p, language \"%s\" -- %zu of %zu declared "
             "booleans read 0 or 1 (%zu language strings seen)",
             found, bestLanguage != nullptr ? bestLanguage : "?", best,
             at.Bools.size(), hits);

        // Named, because a member that does not read as a bool here is a
        // member not to trust, and saying which beats saying none.
        std::vector<BoolField> bad;
        booleans_agreeing(found, at, &bad);
        for (BoolField const& field : bad) {
            logf("global switches:   +%-5u %s does not read as a boolean on "
                 "this build", field.Offset,
                 field.Name != nullptr ? field.Name : "?");
        }
    }

    if (found == nullptr) {
        logf("global switches: not found -- %zu language strings seen, and the "
             "best base (%p, \"%s\") had %zu of %zu declared booleans reading "
             "0 or 1",
             hits, bestAt, bestLanguage != nullptr ? bestLanguage : "?", best,
             at.Bools.size());

        // Which ones disagreed, and where. A run of failures above one
        // offset says a member before it has a different size on this build
        // and everything after has drifted; scattered failures say the struct
        // is not this one at all.
        if (bestAt != nullptr) {
            std::vector<BoolField> bad;
            booleans_agreeing(bestAt, at, &bad);

            std::uint32_t lowest = 0xffffffffu;
            for (BoolField const& field : bad) {
                if (field.Offset < lowest) lowest = field.Offset;
            }
            logf("global switches: %zu disagreed, the lowest at +%u", bad.size(),
                 bad.size() != 0 ? lowest : 0u);
            for (std::size_t i = 0; i < bad.size() && i < 16; ++i) {
                logf("global switches:   +%-5u %s", bad[i].Offset,
                     bad[i].Name != nullptr ? bad[i].Name : "?");
            }
        }
    }
    return found;
}

}  // namespace

}  // namespace bg3le

// Ext.Utils.GetGlobalSwitches needs an address and a class name; the field
// machinery does the rest.
extern "C" void* bg3le_global_switches() {
    const bg3le::CacheLock lock(bg3le::resource_cache_lock());
    if (!bg3le::g_searched) {
        bg3le::g_searched = true;
        bg3le::g_switches = bg3le::search();
    }
    return bg3le::g_switches;
}
