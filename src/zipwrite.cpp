// See zipwrite.h.
#include "zipwrite.h"
#define NOMINMAX
#include <windows.h>
#include <algorithm>

namespace zipwrite {

uint32_t Crc32(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool init;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

namespace {
struct Bits {
    std::vector<uint8_t>& out;
    uint32_t acc = 0;
    int n = 0;
    void Put(uint32_t v, int count) {  // LSB first
        acc |= v << n;
        n += count;
        while (n >= 8) { out.push_back((uint8_t)acc); acc >>= 8; n -= 8; }
    }
    void PutCode(uint32_t code, int len) {  // Huffman codes go MSB first
        uint32_t r = 0;
        for (int i = 0; i < len; i++) r |= ((code >> i) & 1) << (len - 1 - i);
        Put(r, len);
    }
    void Flush() { if (n) { out.push_back((uint8_t)acc); acc = 0; n = 0; } }
};

void Literal(Bits& b, int sym) {  // fixed literal/length code (RFC 1951 3.2.6)
    if (sym < 144) b.PutCode(0x30 + sym, 8);
    else if (sym < 256) b.PutCode(0x190 + sym - 144, 9);
    else if (sym < 280) b.PutCode(sym - 256, 7);
    else b.PutCode(0xC0 + sym - 280, 8);
}

const int kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const int kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const int kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const int kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void Match(Bits& b, int len, int dist) {
    int i = 28;
    while (kLenBase[i] > len) i--;
    Literal(b, 257 + i);
    if (kLenExtra[i]) b.Put(len - kLenBase[i], kLenExtra[i]);
    int d = 29;
    while (kDistBase[d] > dist) d--;
    b.PutCode(d, 5);
    if (kDistExtra[d]) b.Put(dist - kDistBase[d], kDistExtra[d]);
}
}  // namespace

std::vector<uint8_t> Deflate(const uint8_t* p, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n / 3 + 64);
    Bits b{out};
    b.Put(1, 1);  // BFINAL
    b.Put(1, 2);  // BTYPE 01 = fixed Huffman
    const int kWindow = 32768, kHashBits = 15, kMaxChain = 48;
    std::vector<int> head(1 << kHashBits, -1), prev(kWindow, -1);
    auto hash = [&](size_t i) { return ((p[i] << 10) ^ (p[i + 1] << 5) ^ p[i + 2]) & ((1 << kHashBits) - 1); };
    size_t i = 0;
    while (i < n) {
        int bestLen = 0, bestDist = 0;
        if (i + 2 < n) {
            int h = hash(i);
            int cand = head[h];
            for (int chain = 0; cand >= 0 && chain < kMaxChain; chain++) {
                int dist = (int)(i - cand);
                if (dist > kWindow - 1) break;
                int max = (int)std::min<size_t>(258, n - i), len = 0;
                while (len < max && p[cand + len] == p[i + len]) len++;
                if (len > bestLen) { bestLen = len; bestDist = dist; if (len == max) break; }
                int next = prev[cand % kWindow];
                if (next >= cand) break;
                cand = next;
            }
        }
        size_t step = bestLen >= 3 ? bestLen : 1;
        if (bestLen >= 3) Match(b, bestLen, bestDist);
        else Literal(b, p[i]);
        for (size_t k = 0; k < step; k++, i++) {  // index every position the match covered
            if (i + 2 < n) {
                int h = hash(i);
                prev[i % kWindow] = head[h];
                head[h] = (int)i;
            }
        }
    }
    Literal(b, 256);  // end of block
    b.Flush();
    return out;
}

std::vector<uint8_t> Build(const std::vector<Entry>& entries) {
    std::vector<uint8_t> zip, central;
    auto u16 = [](std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); };
    auto u32 = [&](std::vector<uint8_t>& v, uint32_t x) { u16(v, x & 0xFFFF); u16(v, x >> 16); };
    SYSTEMTIME t;
    GetLocalTime(&t);
    uint32_t dosTime = (t.wHour << 11) | (t.wMinute << 5) | (t.wSecond / 2);
    uint32_t dosDate = ((t.wYear - 1980) << 9) | (t.wMonth << 5) | t.wDay;
    for (auto& e : entries) {
        uint32_t crc = Crc32(e.data.data(), e.data.size());
        std::vector<uint8_t> packed = Deflate(e.data.data(), e.data.size());
        bool stored = packed.size() >= e.data.size();
        const std::vector<uint8_t>& body = stored ? e.data : packed;
        uint32_t offset = (uint32_t)zip.size();
        u32(zip, 0x04034b50); u16(zip, 20); u16(zip, 0x0800); u16(zip, stored ? 0 : 8); u16(zip, dosTime); u16(zip, dosDate);
        u32(zip, crc); u32(zip, (uint32_t)body.size()); u32(zip, (uint32_t)e.data.size());
        u16(zip, (uint32_t)e.name.size()); u16(zip, 0);
        zip.insert(zip.end(), e.name.begin(), e.name.end());
        zip.insert(zip.end(), body.begin(), body.end());
        u32(central, 0x02014b50); u16(central, 20); u16(central, 20); u16(central, 0x0800); u16(central, stored ? 0 : 8);
        u16(central, dosTime); u16(central, dosDate); u32(central, crc); u32(central, (uint32_t)body.size());
        u32(central, (uint32_t)e.data.size()); u16(central, (uint32_t)e.name.size()); u16(central, 0); u16(central, 0);
        u16(central, 0); u16(central, 0); u32(central, 0); u32(central, offset);
        central.insert(central.end(), e.name.begin(), e.name.end());
    }
    uint32_t cdOffset = (uint32_t)zip.size();
    zip.insert(zip.end(), central.begin(), central.end());
    u32(zip, 0x06054b50); u16(zip, 0); u16(zip, 0); u16(zip, (uint32_t)entries.size()); u16(zip, (uint32_t)entries.size());
    u32(zip, (uint32_t)central.size()); u32(zip, cdOffset); u16(zip, 0);
    return zip;
}

}  // namespace zipwrite
