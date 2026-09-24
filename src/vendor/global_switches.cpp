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

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
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
    std::vector<BoolField> Strings;
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
        const bool isString = kinds[i] == (std::uint8_t)FieldKind::LSString;
        if (!isBool && !isFloat && !isString) continue;

        std::uint32_t offset = 0;
        if (!bg3le_meta_field(meta, names[i], &offset, &size, &kind, &elemKind,
                              &elemCount)) {
            continue;
        }
        if (isBool) {
            out.Bools.push_back(BoolField{names[i], offset});
        } else if (isFloat) {
            out.Floats.push_back(BoolField{names[i], offset});
        } else if (offset != out.Language) {
            // Language is the anchor; it agrees by construction and would
            // only inflate the score.
            out.Strings.push_back(BoolField{names[i], offset});
        }
    }

    // Enough of each for the test to mean something.
    out.Ok = out.Bools.size() >= 20 && out.Floats.size() >= 10
             && out.Strings.size() >= 3;
    if (!out.Ok) {
        logf("global switches: %zu boolean, %zu float and %zu string members "
             "found; not enough to identify the object safely",
             out.Bools.size(), out.Floats.size(), out.Strings.size());
    }
    return out;
}

// Whether sixteen bytes are one of Larian's strings.
//
// This is the test with real weight, and it took the four-break "solution"
// below to see why. A boolean is one bit: ninety-one of them leave a search
// enough freedom to fit almost anything, and adding shifts to the fit makes
// that worse rather than better. A float is thirty-two bits, of which most
// patterns are NaN or astronomical. A string is a hundred and twenty-eight
// bits with a length that has to agree with its own contents, and
// GlobalSwitches declares seven of them within a few hundred bytes of the
// anchor.
//
// Inline: the top bit of the last byte is clear, that byte is the length,
// and everything from the length to the terminator is zero. On the heap: the
// top bit is set, and there is a readable pointer with a size no larger than
// its capacity and a terminator where the size says.
//
// See vendor/bg3se/CoreLib/Base/LSString.h for the layout itself.
bool looks_like_string(void const* at, bool requireContent = false) {
    unsigned char bytes[16] = {};
    if (!safe_read(at, bytes, sizeof(bytes))) return false;

    if ((bytes[15] & 0x80) == 0) {
        const unsigned length = bytes[15];
        if (length > 15) return false;

        // An empty string is sixteen zero bytes, and a settings object is
        // full of those -- the first version of this counted 77 "strings"
        // in a kilobyte for exactly that reason. Valid, but not evidence.
        if (requireContent && length == 0) return false;
        for (unsigned i = length; i < 15; ++i) {
            if (bytes[i] != 0) return false;
        }
        // These are all ASCII settings -- a language, a path, a URL, a
        // secret -- so a byte outside it says this is not one of them.
        for (unsigned i = 0; i < length; ++i) {
            if (bytes[i] < 0x20 || bytes[i] > 0x7e) return false;
        }
        return true;
    }

    void const* buffer = nullptr;
    std::uint32_t size = 0;
    std::uint32_t capacity = 0;
    std::memcpy(&buffer, bytes, sizeof(buffer));
    std::memcpy(&size, bytes + 8, sizeof(size));
    std::memcpy(&capacity, bytes + 12, sizeof(capacity));
    capacity &= 0x7fffffffu;

    if (buffer == nullptr) return false;
    if (((std::uintptr_t)buffer & 0x7) != 0) return false;
    if (size > capacity || capacity == 0 || capacity > (1u << 24)) return false;

    // The terminator has to be where the size says it is, which is what
    // makes this hard to pass by accident.
    char terminator = 1;
    if (!safe_read((char const*)buffer + size, &terminator, 1)) return false;
    return terminator == '\0';
}

// Whether every declared string member reads as one, and how many of them
// carry content.
//
// The count matters because an empty string is sixteen zero bytes and passes
// for free: bg3se's property map exposes only four of GlobalSwitches'
// strings, so if three of them are empty on this install the test is worth
// one string, not four.
bool strings_agree(void const* base, Offsets const& at,
                   std::vector<BoolField>* disagreed = nullptr,
                   std::size_t* withContent = nullptr) {
    bool all = true;
    if (withContent != nullptr) *withContent = 0;
    for (BoolField const& field : at.Strings) {
        void const* address = (char const*)base + field.Offset;
        if (!looks_like_string(address)) {
            all = false;
            if (disagreed != nullptr) disagreed->push_back(field);
            continue;
        }
        if (withContent != nullptr && looks_like_string(address, true)) {
            ++*withContent;
        }
    }
    return all;
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

// ---------------------------------------------------------------------------
// Solving for the layout
//
// bg3se's offsets are a Windows reverse-engineering, and the notes in
// reference/GLOBAL-SWITCHES.md record what the search found: on the
// best candidate the disagreements begin at +208 and are the same four
// members run after run. That is the signature of a member before +208 with
// a different size on this build -- everything after it has drifted by a
// constant -- rather than of a search finding noise.
//
// So rather than accept or reject bg3se's offsets whole, this looks for the
// drift. Take the lowest offset that disagrees, try shifting every offset
// from there up by a constant, and keep the shift that agrees best. Repeat
// on what still disagrees, up to a few breaks.
//
// If it converges the answer is a layout, not a guess: a hundred-odd
// independent checks agreeing on one set of offsets. If it does not, that is
// reported and the object is still refused.
// ---------------------------------------------------------------------------

struct Break {
    std::uint32_t Pivot{0};
    int Shift{0};
};

std::uint32_t adjusted(std::uint32_t offset,
                       std::vector<Break> const& breaks) {
    int delta = 0;
    for (Break const& at : breaks) {
        if (offset >= at.Pivot) delta += at.Shift;
    }
    const int moved = (int)offset + delta;
    return moved < 0 ? 0u : (std::uint32_t)moved;
}

bool bool_agrees(void const* base, std::uint32_t offset) {
    std::uint8_t value = 0;
    if (!safe_read((char const*)base + offset, &value, 1)) return false;
    return value <= 1;
}

bool float_agrees(void const* base, std::uint32_t offset) {
    float value = 0.0f;
    if (!safe_read((char const*)base + offset, &value, 4)) return false;
    if (!std::isfinite(value)) return false;
    const float magnitude = value < 0.0f ? -value : value;
    return magnitude == 0.0f || (magnitude >= 1e-6f && magnitude <= 1e6f);
}

// Every declared member that does not read as its own type under these
// breaks, lowest offset first.
std::vector<BoolField> disagreements(void const* base, Offsets const& at,
                                     std::vector<Break> const& breaks) {
    std::vector<BoolField> bad;
    for (BoolField const& field : at.Bools) {
        if (!bool_agrees(base, adjusted(field.Offset, breaks))) {
            bad.push_back(field);
        }
    }
    for (BoolField const& field : at.Floats) {
        if (!float_agrees(base, adjusted(field.Offset, breaks))) {
            bad.push_back(field);
        }
    }
    for (BoolField const& field : at.Strings) {
        if (!looks_like_string((char const*)base
                               + adjusted(field.Offset, breaks))) {
            bad.push_back(field);
        }
    }
    std::sort(bad.begin(), bad.end(),
              [](BoolField const& a, BoolField const& b) {
                  return a.Offset < b.Offset;
              });
    return bad;
}

// How many members from `pivot` up agree under these breaks.
std::size_t agreeing_from(void const* base, Offsets const& at,
                          std::vector<Break> const& breaks,
                          std::uint32_t pivot) {
    std::size_t agreed = 0;
    for (BoolField const& field : at.Bools) {
        if (field.Offset < pivot) continue;
        if (bool_agrees(base, adjusted(field.Offset, breaks))) ++agreed;
    }
    for (BoolField const& field : at.Floats) {
        if (field.Offset < pivot) continue;
        if (float_agrees(base, adjusted(field.Offset, breaks))) ++agreed;
    }
    // A string is worth far more than a boolean, and counting it as one
    // vote among ninety-one is what let the six-break fit win.
    for (BoolField const& field : at.Strings) {
        if (field.Offset < pivot) continue;
        if (looks_like_string((char const*)base
                              + adjusted(field.Offset, breaks))) {
            agreed += 16;
        }
    }
    return agreed;
}

std::size_t members_from(Offsets const& at, std::uint32_t pivot) {
    std::size_t total = 0;
    for (BoolField const& field : at.Bools) {
        if (field.Offset >= pivot) ++total;
    }
    for (BoolField const& field : at.Floats) {
        if (field.Offset >= pivot) ++total;
    }
    for (BoolField const& field : at.Strings) {
        if (field.Offset >= pivot) total += 16;
    }
    return total;
}

// The shifts that make this base read as GlobalSwitches, or an empty result
// if no small set of them does.
//
// Shifts are tried in steps of four: a member whose size differs does so by
// a whole field, and every type in this struct is four- or eight-aligned.
bool solve_layout(void const* base, Offsets const& at,
                  std::vector<Break>* breaks) {
    constexpr int kRange = 128;
    constexpr int kStep = 4;

    // One break, not six.
    //
    // Six was tried and it "solved" -- four breaks of +36, +4, -116 and -84,
    // after which all ninety-one booleans and all ten floats agreed. That is
    // a curve fit, not a struct: each break gives the search sixty-four free
    // values, and a boolean check is one bit. Two negative shifts of eighty
    // bytes and more are not what a member changing size looks like.
    //
    // One break is a claim that can be wrong: a single member before the
    // pivot has a different size on this build, everything after it moved by
    // that much, and nothing else changed.
    constexpr std::size_t kMaxBreaks = 1;

    breaks->clear();
    for (std::size_t round = 0; round < kMaxBreaks; ++round) {
        const auto bad = disagreements(base, at, *breaks);
        if (bad.empty()) return true;

        const std::uint32_t pivot = bad.front().Offset;
        const std::size_t total = members_from(at, pivot);
        std::size_t best = agreeing_from(base, at, *breaks, pivot);
        int bestShift = 0;

        for (int shift = -kRange; shift <= kRange; shift += kStep) {
            if (shift == 0) continue;

            std::vector<Break> trial = *breaks;
            trial.push_back(Break{pivot, shift});
            const std::size_t agreed = agreeing_from(base, at, trial, pivot);
            if (agreed > best) {
                best = agreed;
                bestShift = shift;
            }
        }

        if (bestShift == 0) return false;  // nothing improves it
        breaks->push_back(Break{pivot, bestShift});
        if (best == total) return disagreements(base, at, *breaks).empty();
    }
    return disagreements(base, at, *breaks).empty();
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
    // Every other declared string has to read as one before anything is
    // accepted. Scoring is left to the booleans so that a near miss is still
    // reported -- gating the score on the strings left nothing to diagnose.
    const bool stringsOk = bestAt != nullptr && strings_agree(bestAt, at);

    if (best == at.Bools.size() && best > 0 && stringsOk) {
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

        if (bestAt != nullptr) {
            std::vector<BoolField> badStrings;
            std::size_t withContent = 0;
            const bool ok =
                strings_agree(bestAt, at, &badStrings, &withContent);
            logf("global switches: %zu of %zu declared strings read as one "
                 "(%zu of them non-empty, which is what counts)%s",
                 at.Strings.size() - badStrings.size(), at.Strings.size(),
                 withContent, ok ? "" : " --");

            std::string declared;
            for (BoolField const& field : at.Strings) {
                char piece[24] = {};
                std::snprintf(piece, sizeof(piece), "%s+%d",
                              declared.empty() ? "" : " ",
                              (int)field.Offset - (int)at.Language);
                declared += piece;
            }
            logf("global switches:   bg3se puts them at %s from Language",
                 declared.c_str());
            for (BoolField const& field : badStrings) {
                logf("global switches:   +%-5u %s is not a string here",
                     field.Offset, field.Name != nullptr ? field.Name : "?");
            }
        }

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

            // And whether the drift can be solved for. See solve_layout.
            std::vector<Break> breaks;
            if (solve_layout(bestAt, at, &breaks)) {
                logf("global switches: the layout solves with %zu break%s --",
                     breaks.size(), breaks.size() == 1 ? "" : "s");
                for (Break const& b : breaks) {
                    logf("global switches:   everything from +%u shifts by "
                         "%+d", b.Pivot, b.Shift);
                }
                logf("global switches: with those, all %zu declared booleans "
                     "and %zu floats read as their own type",
                     at.Bools.size(), at.Floats.size());
            } else {
                const auto left = disagreements(bestAt, at, breaks);
                logf("global switches: the layout does not solve -- %zu "
                     "break%s tried, %zu members still disagree",
                     breaks.size(), breaks.size() == 1 ? "" : "s",
                     left.size());
                for (Break const& b : breaks) {
                    logf("global switches:   tried: from +%u shift %+d",
                         b.Pivot, b.Shift);
                }
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
