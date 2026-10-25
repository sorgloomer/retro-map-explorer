#include "vfs.h"
#include <emscripten.h>
#include <algorithm>
#include <cstdlib>
#include <set>
#include <unordered_map>

// JS side: window.RME.vfs = { list(): string, read(path, off, len): Promise<ArrayBuffer> }
// list() returns "path\tsize\n" lines.
EM_JS(char*, js_vfs_list, (), {
    const s = window.RME.vfs.list();
    const len = lengthBytesUTF8(s) + 1;
    const p = _malloc(len);
    stringToUTF8(s, p, len);
    return p;
});

EM_ASYNC_JS(int, js_vfs_read, (const char* path, double off, double len, uint8_t* dst), {
    try {
        const buf = await window.RME.vfs.read(UTF8ToString(path), off, len);
        const view = new Uint8Array(buf);
        HEAPU8.set(view.subarray(0, len), dst);
        return view.length;
    } catch (e) {
        console.error('vfs read failed', e);
        return -1;
    }
});

namespace vfs {

static std::vector<Entry> g_files;
static std::unordered_map<std::string, int> g_index;

std::string lower(const std::string& s) {
    std::string r = s;
    for (char& c : r)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return r;
}

void refresh() {
    g_files.clear();
    g_index.clear();
    char* raw = js_vfs_list();
    std::string all(raw);
    free(raw);
    size_t pos = 0;
    while (pos < all.size()) {
        size_t nl = all.find('\n', pos);
        if (nl == std::string::npos) nl = all.size();
        std::string line = all.substr(pos, nl - pos);
        pos = nl + 1;
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        Entry e;
        e.path = line.substr(0, tab);
        e.lower = lower(e.path);
        e.size = strtoull(line.c_str() + tab + 1, nullptr, 10);
        g_index[e.lower] = (int)g_files.size();
        g_files.push_back(std::move(e));
    }
}

const std::vector<Entry>& files() { return g_files; }

int find(const std::string& path) {
    auto it = g_index.find(lower(path));
    return it == g_index.end() ? -1 : it->second;
}

int findIn(const std::string& dir, const std::string& name) { return find(dir.empty() ? name : dir + "/" + name); }

std::vector<std::string> subdirs(const std::string& dir) {
    std::string pre = dir.empty() ? "" : lower(dir) + "/";
    std::set<std::string> seen;
    std::vector<std::string> out;
    for (const Entry& e : g_files) {
        if (e.lower.compare(0, pre.size(), pre) != 0) continue;
        size_t slash = e.path.find('/', pre.size());
        if (slash == std::string::npos) continue;
        std::string name = e.path.substr(pre.size(), slash - pre.size());
        if (seen.insert(lower(name)).second) out.push_back(name);
    }
    return out;
}

std::vector<int> withPrefix(const std::string& prefixLower) {
    std::vector<int> r;
    for (size_t i = 0; i < g_files.size(); i++)
        if (g_files[i].lower.compare(0, prefixLower.size(), prefixLower) == 0) r.push_back((int)i);
    return r;
}

bool readRange(int idx, uint64_t off, uint64_t len, std::vector<uint8_t>& out) {
    if (idx < 0 || idx >= (int)g_files.size()) return false;
    const Entry& e = g_files[idx];
    if (off > e.size) return false;
    if (off + len > e.size) len = e.size - off;
    out.assign(len, 0);
    if (len == 0) return true;
    int got = js_vfs_read(e.path.c_str(), (double)off, (double)len, out.data());
    return got >= 0;
}

bool read(int idx, std::vector<uint8_t>& out) {
    if (idx < 0 || idx >= (int)g_files.size()) return false;
    return readRange(idx, 0, g_files[idx].size, out);
}

bool readText(const std::string& path, std::string& out) {
    int idx = find(path);
    std::vector<uint8_t> b;
    if (idx < 0 || !read(idx, b)) return false;
    out.assign((const char*)b.data(), b.size());
    return true;
}

std::string dirOf(const std::string& p) {
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? "" : p.substr(0, s);
}
std::string baseOf(const std::string& p) {
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}
std::string stemOf(const std::string& p) {
    std::string b = baseOf(p);
    size_t d = b.find_last_of('.');
    return d == std::string::npos ? b : b.substr(0, d);
}
std::string join(const std::string& a, const std::string& b) { return a.empty() ? b : a + "/" + b; }

}  // namespace vfs
