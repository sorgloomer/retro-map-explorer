#include "./image.h"
#include <cstring>

namespace {
uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
}  // namespace

bool decodeBMP(const uint8_t* d, size_t n, Image& out) {
    if (n < 54 || d[0] != 'B' || d[1] != 'M') return false;
    uint32_t dataOff = rd32(d + 10);
    uint32_t hdr = rd32(d + 14);
    int w, h, bpp;
    uint32_t comp = 0, colors = 0;
    size_t palOff;
    if (hdr == 12) {
        w = rd16(d + 18);
        h = rd16(d + 20);
        bpp = rd16(d + 24);
        palOff = 14 + 12;
    } else {
        w = (int32_t)rd32(d + 18);
        h = (int32_t)rd32(d + 22);
        bpp = rd16(d + 28);
        comp = rd32(d + 30);
        colors = rd32(d + 46);
        palOff = 14 + hdr;
    }
    bool topDown = h < 0;
    if (topDown) h = -h;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || comp != 0) return false;
    int palEntry = hdr == 12 ? 3 : 4;
    uint32_t palCount = bpp <= 8 ? (colors ? colors : (1u << bpp)) : 0;
    size_t rowBytes = (((size_t)w * bpp + 31) / 32) * 4;
    if (dataOff + rowBytes * h > n) return false;
    if (palOff + (size_t)palCount * palEntry > n) return false;
    out.w = w;
    out.h = h;
    out.rgba.assign((size_t)w * h * 4, 255);
    for (int y = 0; y < h; y++) {
        const uint8_t* row = d + dataOff + rowBytes * y;
        uint8_t* dst = &out.rgba[(size_t)(topDown ? y : h - 1 - y) * w * 4];
        for (int x = 0; x < w; x++, dst += 4) {
            if (bpp == 8) {
                const uint8_t* p = d + palOff + row[x] * palEntry;
                dst[0] = p[2]; dst[1] = p[1]; dst[2] = p[0];
            } else if (bpp == 4) {
                int idx = (x & 1) ? (row[x >> 1] & 15) : (row[x >> 1] >> 4);
                const uint8_t* p = d + palOff + idx * palEntry;
                dst[0] = p[2]; dst[1] = p[1]; dst[2] = p[0];
            } else if (bpp == 1) {
                int idx = (row[x >> 3] >> (7 - (x & 7))) & 1;
                const uint8_t* p = d + palOff + idx * palEntry;
                dst[0] = p[2]; dst[1] = p[1]; dst[2] = p[0];
            } else if (bpp == 24) {
                dst[0] = row[x * 3 + 2]; dst[1] = row[x * 3 + 1]; dst[2] = row[x * 3];
            } else if (bpp == 32) {
                dst[0] = row[x * 4 + 2]; dst[1] = row[x * 4 + 1]; dst[2] = row[x * 4];
            } else if (bpp == 16) {
                uint16_t v = rd16(row + x * 2);
                dst[0] = (uint8_t)(((v >> 10) & 31) * 255 / 31);
                dst[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
                dst[2] = (uint8_t)((v & 31) * 255 / 31);
            } else {
                return false;
            }
        }
    }
    return true;
}

bool decodeTGA(const uint8_t* d, size_t n, Image& out) {
    if (n < 18) return false;
    int idLen = d[0], cmType = d[1], type = d[2];
    int cmFirst = rd16(d + 3), cmLen = rd16(d + 5), cmBits = d[7];
    int w = rd16(d + 12), h = rd16(d + 14), bpp = d[16], desc = d[17];
    bool rle = type == 9 || type == 10 || type == 11;
    int base = type & 7;
    if (w <= 0 || h <= 0 || (base != 1 && base != 2 && base != 3)) return false;
    size_t pos = 18 + idLen;
    const uint8_t* pal = nullptr;
    int palBytes = (cmBits + 7) / 8;
    if (cmType == 1) {
        pal = d + pos;
        pos += (size_t)cmLen * palBytes;
    }
    int px = (bpp + 7) / 8;
    if (pos > n) return false;
    std::vector<uint8_t> raw((size_t)w * h * px);
    if (!rle) {
        if (pos + raw.size() > n) return false;
        memcpy(raw.data(), d + pos, raw.size());
    } else {
        size_t o = 0;
        while (o < raw.size() && pos < n) {
            int c = d[pos++];
            int cnt = (c & 127) + 1;
            if (c & 128) {
                if (pos + px > n) return false;
                for (int i = 0; i < cnt && o < raw.size(); i++, o += px) memcpy(&raw[o], d + pos, px);
                pos += px;
            } else {
                size_t bytes = (size_t)cnt * px;
                if (pos + bytes > n) return false;
                bytes = std::min(bytes, raw.size() - o);
                memcpy(&raw[o], d + pos, bytes);
                o += bytes;
                pos += (size_t)cnt * px;
            }
        }
    }
    out.w = w;
    out.h = h;
    out.rgba.assign((size_t)w * h * 4, 255);
    bool topDown = (desc & 0x20) != 0;
    bool rightLeft = (desc & 0x10) != 0;
    for (int y = 0; y < h; y++) {
        uint8_t* dst = &out.rgba[(size_t)(topDown ? y : h - 1 - y) * w * 4];
        for (int xs = 0; xs < w; xs++) {
            int x = rightLeft ? w - 1 - xs : xs;
            const uint8_t* s = &raw[((size_t)y * w + xs) * px];
            uint8_t* p = dst + x * 4;
            if (base == 1) {
                int idx = (px == 1 ? s[0] : rd16(s)) - cmFirst;
                if (!pal || idx < 0 || idx >= cmLen) continue;
                const uint8_t* c = pal + (size_t)idx * palBytes;
                p[0] = c[2]; p[1] = c[1]; p[2] = c[0];
                if (palBytes == 4) p[3] = c[3];
            } else if (base == 3) {
                p[0] = p[1] = p[2] = s[0];
            } else if (px == 4) {
                p[0] = s[2]; p[1] = s[1]; p[2] = s[0]; p[3] = s[3];
            } else if (px == 3) {
                p[0] = s[2]; p[1] = s[1]; p[2] = s[0];
            } else if (px == 2) {
                uint16_t v = rd16(s);
                p[0] = (uint8_t)(((v >> 10) & 31) * 255 / 31);
                p[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
                p[2] = (uint8_t)((v & 31) * 255 / 31);
            } else {
                return false;
            }
        }
    }
    size_t partial = 0;
    for (size_t i = 3; i < out.rgba.size(); i += 4) {
        if (out.rgba[i] < 255) out.hasAlpha = true;
        if (out.rgba[i] > 8 && out.rgba[i] < 247) partial++;
    }
    out.partialAlpha = partial * 50 > out.rgba.size() / 4;  // more than 2% of the pixels
    return true;
}
