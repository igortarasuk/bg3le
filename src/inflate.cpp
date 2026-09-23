// DEFLATE decompression.
//
// Larian's archives use three compression methods and mod paks use all of
// them: stored, LZ4, and zlib. Without this one, a zlib-compressed mod is
// invisible -- which is how a stat whose mod could not be identified came
// to be attributed to the base game.
//
// The structure follows Mark Adler's "puff", the reference decoder for
// RFC 1951: canonical Huffman decoded by counting codes per length, which
// needs no tables to build and no allocation at all.

#include "inflate.h"

#include <cstdint>
#include <cstring>

namespace bg3le {

namespace {

constexpr int kMaxBits = 15;
constexpr int kLitCodes = 288;
constexpr int kDistCodes = 30;

struct Bits {
    unsigned char const* In{nullptr};
    std::size_t Size{0};
    std::size_t At{0};
    unsigned long Buffer{0};
    int Count{0};

    // -1 rather than an exception or a longjmp: every caller checks.
    int take(int need) {
        long value = (long)Buffer;
        while (Count < need) {
            if (At >= Size) return -1;
            value |= (long)In[At++] << Count;
            Count += 8;
        }
        Buffer = (unsigned long)(value >> need);
        Count -= need;
        return (int)(value & ((1L << need) - 1));
    }
};

struct Huffman {
    short Count[kMaxBits + 1];
    short Symbol[kLitCodes + kDistCodes + 2];
};

void construct(Huffman* h, short const* lengths, int n) {
    for (int i = 0; i <= kMaxBits; ++i) h->Count[i] = 0;
    for (int i = 0; i < n; ++i) ++h->Count[lengths[i]];

    short offsets[kMaxBits + 2] = {};
    for (int len = 1; len <= kMaxBits; ++len) {
        offsets[len + 1] = (short)(offsets[len] + h->Count[len]);
    }
    for (int i = 0; i < n; ++i) {
        if (lengths[i] != 0) h->Symbol[offsets[lengths[i]]++] = (short)i;
    }
}

int decode(Bits* s, Huffman const& h) {
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len <= kMaxBits; ++len) {
        const int bit = s->take(1);
        if (bit < 0) return -1;
        code |= bit;
        const int count = h.Count[len];
        if (code - count < first) return h.Symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

struct Output {
    char* At{nullptr};
    std::size_t Size{0};
    std::size_t Used{0};

    bool put(int byte) {
        if (Used >= Size) return false;
        At[Used++] = (char)byte;
        return true;
    }
};

bool stored(Bits* s, Output* out) {
    s->Buffer = 0;
    s->Count = 0;
    if (s->At + 4 > s->Size) return false;
    const unsigned len = (unsigned)s->In[s->At]
                         | ((unsigned)s->In[s->At + 1] << 8);
    const unsigned nlen = (unsigned)s->In[s->At + 2]
                          | ((unsigned)s->In[s->At + 3] << 8);
    if ((len ^ 0xffffu) != nlen) return false;
    s->At += 4;
    if (s->At + len > s->Size) return false;
    if (out->Used + len > out->Size) return false;
    std::memcpy(out->At + out->Used, s->In + s->At, len);
    s->At += len;
    out->Used += len;
    return true;
}

bool codes(Bits* s, Output* out, Huffman const& lit, Huffman const& dist) {
    static const short lens[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17,
                                   19, 23, 27, 31, 35, 43, 51, 59, 67, 83,
                                   99, 115, 131, 163, 195, 227, 258};
    static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2,
                                   2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5,
                                   0};
    static const short dists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49,
                                    65, 97, 129, 193, 257, 385, 513, 769,
                                    1025, 1537, 2049, 3073, 4097, 6145,
                                    8193, 12289, 16385, 24577};
    static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5,
                                   6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11,
                                   12, 12, 13, 13};

    for (;;) {
        const int symbol = decode(s, lit);
        if (symbol < 0) return false;
        if (symbol < 256) {
            if (!out->put(symbol)) return false;
            continue;
        }
        if (symbol == 256) return true;

        const int index = symbol - 257;
        if (index >= 29) return false;
        const int extra = s->take(lext[index]);
        if (extra < 0) return false;
        const std::size_t length = (std::size_t)lens[index] + extra;

        const int dsym = decode(s, dist);
        if (dsym < 0 || dsym >= 30) return false;
        const int dextra = s->take(dext[dsym]);
        if (dextra < 0) return false;
        const std::size_t distance = (std::size_t)dists[dsym] + dextra;
        if (distance > out->Used) return false;
        if (out->Used + length > out->Size) return false;

        // Byte by byte, because the run may overlap itself.
        for (std::size_t i = 0; i < length; ++i) {
            out->At[out->Used] = out->At[out->Used - distance];
            ++out->Used;
        }
    }
}

bool fixed_block(Bits* s, Output* out) {
    static Huffman lit;
    static Huffman dist;
    static bool built = false;
    if (!built) {
        short lengths[kLitCodes];
        int i = 0;
        for (; i < 144; ++i) lengths[i] = 8;
        for (; i < 256; ++i) lengths[i] = 9;
        for (; i < 280; ++i) lengths[i] = 7;
        for (; i < kLitCodes; ++i) lengths[i] = 8;
        construct(&lit, lengths, kLitCodes);

        short distances[kDistCodes];
        for (i = 0; i < kDistCodes; ++i) distances[i] = 5;
        construct(&dist, distances, kDistCodes);
        built = true;
    }
    return codes(s, out, lit, dist);
}

bool dynamic_block(Bits* s, Output* out) {
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4,
                                    12, 3, 13, 2, 14, 1, 15};

    const int nlen = s->take(5);
    const int ndist = s->take(5);
    const int ncode = s->take(4);
    if (nlen < 0 || ndist < 0 || ncode < 0) return false;

    const int literals = nlen + 257;
    const int distances = ndist + 1;
    const int counts = ncode + 4;
    if (literals > kLitCodes || distances > kDistCodes + 2) return false;

    short lengths[kLitCodes + kDistCodes + 2] = {};
    for (int i = 0; i < counts; ++i) {
        const int bits = s->take(3);
        if (bits < 0) return false;
        lengths[order[i]] = (short)bits;
    }
    for (int i = counts; i < 19; ++i) lengths[order[i]] = 0;

    Huffman code{};
    construct(&code, lengths, 19);

    int index = 0;
    while (index < literals + distances) {
        const int symbol = decode(s, code);
        if (symbol < 0) return false;

        if (symbol < 16) {
            lengths[index++] = (short)symbol;
            continue;
        }

        short value = 0;
        int repeat = 0;
        if (symbol == 16) {
            if (index == 0) return false;
            value = lengths[index - 1];
            const int extra = s->take(2);
            if (extra < 0) return false;
            repeat = 3 + extra;
        } else if (symbol == 17) {
            const int extra = s->take(3);
            if (extra < 0) return false;
            repeat = 3 + extra;
        } else {
            const int extra = s->take(7);
            if (extra < 0) return false;
            repeat = 11 + extra;
        }
        if (index + repeat > literals + distances) return false;
        while (repeat-- > 0) lengths[index++] = value;
    }
    if (lengths[256] == 0) return false;

    Huffman lit{};
    Huffman dist{};
    construct(&lit, lengths, literals);
    construct(&dist, lengths + literals, distances);
    return codes(s, out, lit, dist);
}

}  // namespace

bool inflate(char const* in, std::size_t size, char* out,
             std::size_t outSize) {
    if (in == nullptr || out == nullptr || size < 2) return false;

    auto const* bytes = (unsigned char const*)in;
    std::size_t at = 0;
    // A zlib wrapper: deflate method in the low nibble and the two-byte
    // header a multiple of 31. Raw deflate streams fail both tests.
    if ((bytes[0] & 0x0f) == 8
        && ((((unsigned)bytes[0] << 8) | bytes[1]) % 31) == 0) {
        at = 2;
    }

    Bits s{bytes + at, size - at};
    Output o{out, outSize};

    for (;;) {
        const int last = s.take(1);
        const int type = s.take(2);
        if (last < 0 || type < 0) return false;

        bool ok = false;
        switch (type) {
        case 0: ok = stored(&s, &o); break;
        case 1: ok = fixed_block(&s, &o); break;
        case 2: ok = dynamic_block(&s, &o); break;
        default: ok = false; break;
        }
        if (!ok) return false;
        if (last != 0) break;
    }
    return o.Used == outSize;
}

}  // namespace bg3le
