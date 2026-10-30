#include "minigame01/params.h"
#include <cctype>
#include <cstdlib>
#include <functional>
#include <vector>

namespace minigame01 {

namespace {

struct GlobalEntry { const char* name; float Params::*field; };
struct CarEntry { const char* name; float CarSettings::*field; };

const GlobalEntry kGlobals[] = {
#define X(name) {#name, &Params::name},
    MINIGAME_GLOBALS(X)
#undef X
};

const CarEntry kCar[] = {
#define X(name) {#name, &CarSettings::name},
    MINIGAME_CAR(X)
#undef X
};

// A reader of the JSON text: objects of numbers (and an object of objects at the top).
struct Reader {
    const std::string& text;
    size_t p = 0;
    int line = 1;
    std::string error;

    explicit Reader(const std::string& t) : text(t) {}

    bool fail(const std::string& why) {
        error = "line " + std::to_string(line) + ": " + why;
        return false;
    }
    void skip() {
        while (p < text.size() && std::isspace((unsigned char)text[p])) {
            if (text[p] == '\n') line++;
            p++;
        }
    }
    // '{' members '}' : for each member the name is read, then `member(name)` reads the value after the colon
    bool object(const std::function<bool(const std::string&)>& member) {
        skip();
        if (p >= text.size() || text[p] != '{') return fail("expected '{'");
        p++;
        bool first = true;
        while (true) {
            skip();
            if (p >= text.size()) return fail("the object is not closed, expected '}'");
            if (text[p] == '}') { p++; return true; }
            if (!first) {
                if (text[p] != ',') return fail("expected ',' or '}'");
                p++;
                skip();
            }
            first = false;
            if (p >= text.size() || text[p] != '"') return fail("expected a \"name\"");
            const size_t end = text.find('"', p + 1);
            if (end == std::string::npos) return fail("the name is not closed");
            const std::string name = text.substr(p + 1, end - p - 1);
            p = end + 1;
            skip();
            if (p >= text.size() || text[p] != ':') return fail("expected ':' after \"" + name + "\"");
            p++;
            skip();
            if (!member(name)) return false;
        }
    }
    bool number(const std::string& name, double& value) {
        const char* start = text.c_str() + p;
        char* stop = nullptr;
        value = std::strtod(start, &stop);
        if (stop == start) return fail("expected a number for \"" + name + "\"");
        p += (size_t)(stop - start);
        return true;
    }
};

}  // namespace

Params& tuning() {
    static Params p;
    return p;
}

CarSettings& carSettings() {
    static CarSettings c;
    return c;
}

bool settingsFromJson(const std::string& text, std::string& error) {
    Params globals;
    CarSettings car;
    std::vector<bool> haveGlobal(sizeof kGlobals / sizeof kGlobals[0], false), haveCar(sizeof kCar / sizeof kCar[0], false);
    Reader r(text);
    const bool ok = r.object([&](const std::string& section) {
        if (section == "globals") {
            return r.object([&](const std::string& name) {
                double v;
                if (!r.number(name, v)) return false;
                for (size_t i = 0; i < haveGlobal.size(); i++)
                    if (name == kGlobals[i].name) { globals.*kGlobals[i].field = (float)v; haveGlobal[i] = true; return true; }
                return r.fail("unknown value \"" + name + "\" in globals");
            });
        }
        if (section == "car") {
            return r.object([&](const std::string& name) {
                double v;
                if (!r.number(name, v)) return false;
                for (size_t i = 0; i < haveCar.size(); i++)
                    if (name == kCar[i].name) { car.*kCar[i].field = (float)v; haveCar[i] = true; return true; }
                return r.fail("unknown value \"" + name + "\" in car");
            });
        }
        return r.fail("unknown section \"" + section + "\"");
    });
    if (!ok) { error = r.error; return false; }
    for (size_t i = 0; i < haveGlobal.size(); i++)
        if (!haveGlobal[i]) { error = std::string("the value \"") + kGlobals[i].name + "\" is missing in globals"; return false; }
    for (size_t i = 0; i < haveCar.size(); i++)
        if (!haveCar[i]) { error = std::string("the value \"") + kCar[i].name + "\" is missing in car"; return false; }
    tuning() = globals;
    carSettings() = car;
    return true;
}

}  // namespace minigame01
