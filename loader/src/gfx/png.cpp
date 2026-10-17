#include "gfx/png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

namespace gfx {
namespace {

// PNG's CRC-32: the standard reflected polynomial 0xEDB88320, initial and final values complemented.
uint32_t crc32_of(const uint8_t *data, size_t len, uint32_t crc = 0xFFFFFFFFu)
{
    static uint32_t table[256];
    static bool made = false;
    if (!made) {
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[n] = c;
        }
        made = true;
    }
    for (size_t i = 0; i < len; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

// zlib's Adler-32, which is what a deflate stream inside a PNG is wrapped in.
uint32_t adler32_of(const uint8_t *data, size_t len)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void put_be32(std::vector<uint8_t> &v, uint32_t x)
{
    v.push_back((uint8_t)(x >> 24));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}

void chunk(std::vector<uint8_t> &out, const char type[4], const std::vector<uint8_t> &payload)
{
    put_be32(out, (uint32_t)payload.size());
    const size_t crc_start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    const uint32_t crc = crc32_of(out.data() + crc_start, out.size() - crc_start) ^ 0xFFFFFFFFu;
    put_be32(out, crc);
}

} // namespace

bool write_png(const char *path, const void *rgba, uint32_t width, uint32_t height)
{
    if (!path || !rgba || !width || !height) return false;
    const size_t row_bytes = (size_t)width * 4;
    const size_t raw_size = (row_bytes + 1) * height; // one filter byte per row

    // Raw scanlines, each prefixed with filter type 0 (None). Every row is stored verbatim, which is what
    // lets the stored-block deflate below be a straight copy.
    std::vector<uint8_t> raw;
    raw.reserve(raw_size);
    const uint8_t *src = (const uint8_t *)rgba;
    for (uint32_t y = 0; y < height; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), src + (size_t)y * row_bytes, src + (size_t)(y + 1) * row_bytes);
    }

    // zlib stream: header, stored blocks, Adler-32. A stored block holds at most 65535 bytes and its
    // header is byte-aligned, which is the whole reason a hand-rolled writer can be this short.
    std::vector<uint8_t> z;
    z.push_back(0x78); // CM = 8 (deflate), CINFO = 7 (32 KiB window)
    z.push_back(0x01); // FCHECK such that (0x78<<8 | 0x01) % 31 == 0, no dictionary, fastest
    size_t at = 0;
    while (at < raw.size()) {
        const size_t n = raw.size() - at > 65535 ? 65535 : raw.size() - at;
        const bool last = at + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back((uint8_t)(n & 0xFF));
        z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)(~n & 0xFF));
        z.push_back((uint8_t)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
        at += n;
    }
    put_be32(z, adler32_of(raw.data(), raw.size()));

    std::vector<uint8_t> out;
    const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    out.insert(out.end(), signature, signature + 8);

    std::vector<uint8_t> ihdr;
    put_be32(ihdr, width);
    put_be32(ihdr, height);
    ihdr.push_back(8); // bit depth
    ihdr.push_back(6); // colour type: truecolour with alpha
    ihdr.push_back(0); // deflate
    ihdr.push_back(0); // adaptive filtering
    ihdr.push_back(0); // no interlace
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", std::vector<uint8_t>());

    FILE *f = fopen(path, "wb");
    if (!f) return false;
    const size_t n = fwrite(out.data(), 1, out.size(), f);
    const bool ok = (n == out.size()) && fclose(f) == 0;
    return ok;
}

} // namespace gfx
