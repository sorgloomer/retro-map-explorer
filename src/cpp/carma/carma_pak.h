// Carmageddon .dir / .pak archives: names in a prefix-compressed trie (.dir), zlib payloads with an obfuscated header (.pak).
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace carma {

struct DirEntry {
    std::string name;
    uint32_t off = 0, size = 0;
};

struct Pak {
    std::string base;
    int pakFile = -1;
    std::vector<DirEntry> entries;
    std::unordered_map<std::string, int> byName;  // lower-case name
    std::unordered_map<std::string, int> textures;  // lower-case texture name -> entry of best .tga

    bool open(int dirFile, int pakFileIdx, const std::string& baseName);

    void parseDir(const std::vector<uint8_t>& d);

    // "<name>_<w>x<h>_<bits>.tga"  ->  name (prefer 32 bit variants)
    void indexTextures();

    int find(const std::string& name) const;

    bool extract(int e, std::vector<uint8_t>& out) const;
};

}  // namespace carma
