#include "minigame01/params.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace minigame01 {

namespace {

struct Entry {
    const char* name;
    float Params::*field;
};

const Entry kEntries[] = {
#define X(name, def) {#name, &Params::name},
    MINIGAME_PARAMS(X)
#undef X
};

}  // namespace

Params& tuning() {
    static Params p;
    return p;
}

std::string paramsToJson(const Params& p) {
    std::string out = "{\n";
    const size_t n = sizeof kEntries / sizeof kEntries[0];
    for (size_t i = 0; i < n; i++) {
        // the shortest text that reads back as the same float (0.05, not 0.0500000007)
        const float value = p.*kEntries[i].field;
        char number[32];
        for (int digits = 6; digits <= 9; digits++) {
            snprintf(number, sizeof number, "%.*g", digits, (double)value);
            if ((float)std::strtod(number, nullptr) == value) break;
        }
        char line[160];
        snprintf(line, sizeof line, "  \"%s\": %s%s\n", kEntries[i].name, number, i + 1 < n ? "," : "");
        out += line;
    }
    out += "}\n";
    return out;
}

bool paramsFromJson(const std::string& text, Params& out, std::string& error) {
    Params result;  // the defaults: what the text leaves out
    size_t p = 0;
    int line = 1;
    auto fail = [&](const std::string& why) {
        error = "line " + std::to_string(line) + ": " + why;
        return false;
    };
    auto skip = [&]() {
        while (p < text.size() && std::isspace((unsigned char)text[p])) {
            if (text[p] == '\n') line++;
            p++;
        }
    };
    skip();
    if (p >= text.size() || text[p] != '{') return fail("expected '{'");
    p++;
    bool first = true;
    while (true) {
        skip();
        if (p >= text.size()) return fail("the object is not closed, expected '}'");
        if (text[p] == '}') { p++; break; }
        if (!first) {
            if (text[p] != ',') return fail("expected ',' or '}'");
            p++;
            skip();
            if (p < text.size() && text[p] == '}') return fail("a ',' before '}' is not allowed");
        }
        first = false;
        if (p >= text.size() || text[p] != '"') return fail("expected a \"name\"");
        size_t end = text.find('"', p + 1);
        if (end == std::string::npos) return fail("the name is not closed");
        const std::string name = text.substr(p + 1, end - p - 1);
        p = end + 1;
        skip();
        if (p >= text.size() || text[p] != ':') return fail("expected ':' after \"" + name + "\"");
        p++;
        skip();
        const char* start = text.c_str() + p;
        char* stop = nullptr;
        const double value = std::strtod(start, &stop);
        if (stop == start) return fail("expected a number for \"" + name + "\"");
        p += (size_t)(stop - start);
        bool known = false;
        for (const Entry& e : kEntries)
            if (name == e.name) { result.*e.field = (float)value; known = true; break; }
        if (!known) return fail("unknown parameter \"" + name + "\"");
    }
    skip();
    if (p < text.size()) return fail("unexpected text after the closing '}'");
    out = result;
    return true;
}

}  // namespace minigame01
