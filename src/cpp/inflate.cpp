#include "inflate.h"

namespace {

struct Huff {
    uint16_t count[16];
    uint16_t symbol[320];
};

struct State {
    const uint8_t* in;
    size_t inLen, inPos = 0;
    uint32_t bitBuf = 0;
    int bitCnt = 0;
    std::vector<uint8_t>* out;
    bool err = false;
};

int getBits(State& s, int need) {
    uint32_t v = s.bitBuf;
    while (s.bitCnt < need) {
        if (s.inPos >= s.inLen) { s.err = true; return 0; }
        v |= (uint32_t)s.in[s.inPos++] << s.bitCnt;
        s.bitCnt += 8;
    }
    s.bitBuf = v >> need;
    s.bitCnt -= need;
    return (int)(v & ((1u << need) - 1));
}

void buildHuff(Huff& h, const uint8_t* lengths, int n) {
    for (int i = 0; i < 16; i++) h.count[i] = 0;
    for (int i = 0; i < n; i++) h.count[lengths[i]]++;
    uint16_t offs[16];
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h.count[i];
    for (int i = 0; i < n; i++)
        if (lengths[i]) h.symbol[offs[lengths[i]]++] = (uint16_t)i;
}

int decodeSym(State& s, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= getBits(s, 1);
        if (s.err) return -1;
        int cnt = h.count[len];
        if (code - cnt < first) return h.symbol[index + (code - first)];
        index += cnt;
        first += cnt;
        first <<= 1;
        code <<= 1;
    }
    s.err = true;
    return -1;
}

const uint16_t kLenBase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint16_t kLenExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint16_t kDistExtra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(State& s, const Huff& lit, const Huff& dist) {
    std::vector<uint8_t>& o = *s.out;
    for (;;) {
        int sym = decodeSym(s, lit);
        if (s.err) return false;
        if (sym < 256) {
            o.push_back((uint8_t)sym);
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            int len = kLenBase[sym] + getBits(s, kLenExtra[sym]);
            int ds = decodeSym(s, dist);
            if (s.err || ds < 0 || ds >= 30) return false;
            size_t d = kDistBase[ds] + getBits(s, kDistExtra[ds]);
            if (s.err || d > o.size()) return false;
            size_t from = o.size() - d;
            for (int i = 0; i < len; i++) o.push_back(o[from + i]);
        }
    }
}

bool run(State& s) {
    int last;
    do {
        last = getBits(s, 1);
        int type = getBits(s, 2);
        if (s.err) return false;
        if (type == 0) {
            s.bitBuf = 0;
            s.bitCnt = 0;
            if (s.inPos + 4 > s.inLen) return false;
            unsigned len = s.in[s.inPos] | (s.in[s.inPos + 1] << 8);
            s.inPos += 4;
            if (s.inPos + len > s.inLen) return false;
            s.out->insert(s.out->end(), s.in + s.inPos, s.in + s.inPos + len);
            s.inPos += len;
        } else if (type == 1) {
            static Huff lit, dist;
            static bool init = false;
            if (!init) {
                uint8_t l[288];
                int i = 0;
                for (; i < 144; i++) l[i] = 8;
                for (; i < 256; i++) l[i] = 9;
                for (; i < 280; i++) l[i] = 7;
                for (; i < 288; i++) l[i] = 8;
                buildHuff(lit, l, 288);
                for (i = 0; i < 30; i++) l[i] = 5;
                buildHuff(dist, l, 30);
                init = true;
            }
            if (!codes(s, lit, dist)) return false;
        } else if (type == 2) {
            int nlen = getBits(s, 5) + 257, ndist = getBits(s, 5) + 1, ncode = getBits(s, 4) + 4;
            if (s.err || nlen > 286 || ndist > 30) return false;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t lengths[320] = {0};
            for (int i = 0; i < ncode; i++) lengths[order[i]] = (uint8_t)getBits(s, 3);
            Huff lencode;
            buildHuff(lencode, lengths, 19);
            int idx = 0;
            uint8_t ll[320] = {0};
            while (idx < nlen + ndist) {
                int sym = decodeSym(s, lencode);
                if (s.err) return false;
                if (sym < 16) {
                    ll[idx++] = (uint8_t)sym;
                } else {
                    int len = 0, rep;
                    if (sym == 16) {
                        if (idx == 0) return false;
                        len = ll[idx - 1];
                        rep = 3 + getBits(s, 2);
                    } else if (sym == 17) {
                        rep = 3 + getBits(s, 3);
                    } else {
                        rep = 11 + getBits(s, 7);
                    }
                    if (idx + rep > nlen + ndist) return false;
                    while (rep--) ll[idx++] = (uint8_t)len;
                }
            }
            Huff lit, dist;
            buildHuff(lit, ll, nlen);
            buildHuff(dist, ll + nlen, ndist);
            if (!codes(s, lit, dist)) return false;
        } else {
            return false;
        }
    } while (!last);
    return true;
}

}  // namespace

bool inflateRaw(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t sizeHint) {
    State s;
    s.in = src;
    s.inLen = n;
    out.clear();
    if (sizeHint) out.reserve(sizeHint);
    s.out = &out;
    return run(s);
}

bool inflateZlib(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t sizeHint) {
    if (n < 2) return false;
    return inflateRaw(src + 2, n - 2, out, sizeHint);
}
