#include "carma/carma_text.h"
#include <cstring>
#include <sstream>

namespace carma {

std::vector<std::string> splitLines(const std::string& t) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p <= t.size()) {
        size_t e = t.find('\n', p);
        if (e == std::string::npos) e = t.size();
        std::string l = t.substr(p, e - p);
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.pop_back();
        size_t s = 0;
        while (s < l.size() && (l[s] == ' ' || l[s] == '\t')) s++;
        out.push_back(l.substr(s));
        p = e + 1;
    }
    return out;
}

std::vector<std::string> quotedStrings(const std::string& line) {
    std::vector<std::string> r;
    size_t p = 0;
    while (true) {
        size_t a = line.find('"', p);
        if (a == std::string::npos) break;
        size_t b = line.find('"', a + 1);
        if (b == std::string::npos) break;
        r.push_back(line.substr(a + 1, b - a - 1));
        p = b + 1;
    }
    return r;
}

std::string stripComment(const std::string& l) {
    bool inq = false;
    for (size_t i = 0; i + 1 < l.size(); i++) {
        if (l[i] == '"') inq = !inq;
        if (!inq && l[i] == '/' && l[i + 1] == '/') return l.substr(0, i);
    }
    return l;
}

bool endsWith(const std::string& s, const char* suf) {
    size_t n = strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

void KeyFile::parse(const std::string& text) {
    for (const std::string& raw : splitLines(text)) {
        std::string l = stripComment(raw);
        size_t sp = 0;
        while (sp < l.size() && l[sp] != ' ' && l[sp] != '\t') sp++;
        if (sp == 0) continue;
        std::string key = l.substr(0, sp);
        size_t v = sp;
        while (v < l.size() && (l[v] == ' ' || l[v] == '\t')) v++;
        items.push_back({key, l.substr(v)});
    }
}

const std::string* KeyFile::get(const char* key) const {
    for (auto& it : items)
        if (it.first == key) return &it.second;
    return nullptr;
}

std::string KeyFile::str(const char* key) const {
    const std::string* v = get(key);
    if (!v) return "";
    auto q = quotedStrings(*v);
    return q.empty() ? *v : q[0];
}

bool KeyFile::nums(const char* key, float* out, int n) const {
    const std::string* v = get(key);
    if (!v) return false;
    std::istringstream is(*v);
    for (int i = 0; i < n; i++)
        if (!(is >> out[i])) return false;
    return true;
}

}  // namespace carma
