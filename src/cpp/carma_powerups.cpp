#include "carma_powerups.h"
#include "carma_text.h"
#include "vfs.h"

namespace carma {

void PupTable::parse(const std::string& text) {
    items.clear();
    const std::string& t = text;
    std::string group;
    for (const std::string& l : splitLines(t)) {
        if (l.empty()) continue;
        if (l.compare(0, 2, "//") == 0) {
            std::string w = l.substr(2);
            while (!w.empty() && w[0] == ' ') w.erase(0, 1);
            bool word = !w.empty();
            for (char c : w) word &= (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            if (word) group = vfs::lower(w);
            continue;
        }
        if (l[0] != '"') continue;
        auto q = quotedStrings(l);
        items.push_back({group, q.empty() ? "" : q[0]});
    }
}

int PupTable::classify(int id, const std::string& label) const {
    std::string g, nm = vfs::lower(label);
    if (id >= 0 && id < (int)items.size()) { g = items[id].group; nm = vfs::lower(items[id].name); }
    auto has = [&](const char* w) { return nm.find(w) != std::string::npos; };
    if (g == "money" || has("credits") || has("loads of cash")) return 4;
    if ((g == "apo" && id >= 1 && id <= 20) || has("extra armour") || has("extra power") || has("extra offensive") ||
        has("extra everything") || has("extra slots") || has("bonus slots") || has("bonus armour") || has("bonus power") ||
        has("bonus offensive") || has("max armour") || has("max power") || has("max offensive") || has("max everything"))
        return 3;
    if (id == 88 || g == "peds" || has("pedestrian") || has("peds ")) return 2;
    if (g == "time" || has("time bonus") || has("bonus time") || has("extra time")) return 5;
    static const char* special[] = {"afterburner", "hot rod", "invulnerab", "underwater", "wall climb", "solid granite", "kangaroo",
                                    "mega-turbo", "turbo nutter", "nuclear bomb", "slaughter mortar", "hovercraft"};
    for (const char* w : special)
        if (has(w)) return 1;
    return 0;
}

std::string PupTable::iconFor(int id) const {
    std::string g;
    if (id >= 0 && id < (int)items.size()) g = items[id].group;
    if (id == 100) return "BombPiececube1";
    if (id == 101) return "Oil_DrumDrum_null";
    if (id == 102) return "fuseFuse_NULL";
    if (id == 103) return "EnginePartobj3";
    if (id == 104) return "DingablesMoneyBagPowerup";
    if (id == 105) return "DingablesArtilleryShellPow";
    if (g == "apo" && id >= 1 && id <= 20) {
        static const char* kinds[] = {"newIconsHELMET", "newIconsENGINE", "newIconsFIST", "newIconsAPOall"};
        return kinds[(id - 1) % 4];  // armour, power, offensive, everything (and their double / max / slot variants)
    }
    if (id == 88 || g == "peds") return "newIconsPEDSIGN";
    if (g == "money") return "newIconsWADOCASH";
    if (g == "time") return "newIconsTIME";
    if (g == "random") return "newIconsRANDOM";
    return "newIconsSPANNER";
}

}  // namespace carma
