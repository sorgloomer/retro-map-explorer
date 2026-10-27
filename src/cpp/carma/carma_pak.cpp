#include "carma/carma_pak.h"
#include <algorithm>
#include "carma/carma_text.h"
#include "loader/inflate.h"
#include "vfs.h"

namespace carma {

bool Pak::open(int dirFile, int pakFileIdx, const std::string& baseName) {
    base = baseName;
    pakFile = pakFileIdx;
    std::vector<uint8_t> d;
    if (!vfs::read(dirFile, d)) return false;
    parseDir(d);
    for (size_t i = 0; i < entries.size(); i++) byName[vfs::lower(entries[i].name)] = (int)i;
    indexTextures();
    return true;
}

void Pak::parseDir(const std::vector<uint8_t>& d) {
    char fn[512];
    bool rb[512] = {false};
    int i = 0, sbase = 0;
    size_t p = 0;
    while (p + 1 < d.size()) {
        int code;
        do {
            if (p + 1 >= d.size() || i >= 500) return;
            fn[i] = (char)d[p];
            code = d[p + 1];
            p += 2;
            if (code == 136) sbase = i;
            if (code == 192) rb[i] = true;
            i++;
        } while (!(code & 8));
        if (p + 8 > d.size()) return;
        DirEntry e;
        e.name.assign(fn, i);
        memcpy(&e.off, &d[p], 4);
        memcpy(&e.size, &d[p + 4], 4);
        p += 8;
        entries.push_back(std::move(e));
        if (code == 72) {
        } else if (code == 136) {
            i = sbase;
        } else if (code == 200) {
            if (i > 0) rb[i - 1] = true;
        } else {
            int j = -1;
            for (int k = 511; k >= 0; k--)
                if (rb[k]) { j = k; break; }
            if (j >= 0) rb[j] = false;
            i = j < 0 ? 0 : j;
        }
    }
}

void Pak::indexTextures() {
    std::unordered_map<std::string, int> bits;
    for (size_t i = 0; i < entries.size(); i++) {
        std::string n = vfs::lower(entries[i].name);
        if (!endsWith(n, ".tga")) continue;
        n.resize(n.size() - 4);
        size_t u1 = n.find_last_of('_');
        if (u1 == std::string::npos) continue;
        int b = atoi(n.c_str() + u1 + 1);
        n.resize(u1);
        size_t u2 = n.find_last_of('_');
        if (u2 == std::string::npos) continue;
        bool plainNumber = n.find_first_not_of("0123456789", u2 + 1) == std::string::npos;
        n.resize(u2);
        if (plainNumber) {  // "<name>_<w>_<h>_<bits>" spelling: the width comes before the height
            size_t u3 = n.find_last_of('_');
            if (u3 != std::string::npos && n.find_first_not_of("0123456789", u3 + 1) == std::string::npos) n.resize(u3);
        }
        auto it = bits.find(n);
        if (it == bits.end() || b > it->second) {
            bits[n] = b;
            textures[n] = (int)i;
        }
    }
}

int Pak::find(const std::string& name) const {
    auto it = byName.find(vfs::lower(name));
    return it == byName.end() ? -1 : it->second;
}

bool Pak::extract(int e, std::vector<uint8_t>& out) const {
    const DirEntry& de = entries[e];
    std::vector<uint8_t> raw;
    if (!vfs::readRange(pakFile, de.off, de.size, raw)) return false;
    if (raw.size() >= 8 && (raw[0] ^ raw[1]) == 0x7a && (raw[2] ^ raw[3]) == 0x0e) {
        uint32_t exp = (raw[4] ^ raw[7]) | ((raw[5] ^ raw[7]) << 8) | ((raw[6] ^ raw[7]) << 16);
        return inflateZlib(raw.data() + 8, raw.size() - 8, out, exp);
    }
    out = std::move(raw);
    return true;
}

}  // namespace carma
