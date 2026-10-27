#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include "loader/binio.h"
#include "carma/carma_game.h"
#include "loader/inflate.h"
#include "vfs.h"

namespace carma {

std::vector<std::string> CarmaGame::trackDirs() {
    std::vector<std::string> r;
    std::unordered_map<std::string, bool> seen;
    for (const auto& e : vfs::files()) {
        std::string d = vfs::dirOf(e.path);
        if (d.empty() || vfs::lower(vfs::baseOf(vfs::dirOf(d))) != "tracks") continue;
        if (seen.emplace(vfs::lower(d), true).second) r.push_back(d);
    }
    std::sort(r.begin(), r.end(), [](const std::string& a, const std::string& b) { return vfs::lower(a) < vfs::lower(b); });
    return r;
}

void CarmaGame::openDirPaks(const std::string& dir, std::vector<std::unique_ptr<Pak>>& into) {
    std::string pre = vfs::lower(dir) + "/";
    for (size_t i = 0; i < vfs::files().size(); i++) {
        const auto& e = vfs::files()[i];
        if (!endsWith(e.lower, ".dir") || e.lower.compare(0, pre.size(), pre) != 0) continue;
        if (e.lower.find('/', pre.size()) != std::string::npos) continue;
        int pk = vfs::findIn(dir, vfs::stemOf(e.path) + ".pak");
        if (pk < 0) continue;
        auto p = std::make_unique<Pak>();
        if (p->open((int)i, pk, vfs::stemOf(e.path))) into.push_back(std::move(p));
    }
}

bool CarmaGame::readFromPaks(const std::vector<std::unique_ptr<Pak>>& paks, const std::string& name, std::vector<uint8_t>& out) {
    for (const auto& p : paks) {
        int e = p->find(name);
        if (e >= 0 && p->extract(e, out)) return true;
    }
    return false;
}

bool CarmaGame::detect() {
    for (const std::string& d : trackDirs())
        if (!vfs::subdirs(d).empty() || vfs::findIn(d, vfs::baseOf(d) + ".txt") >= 0) return true;
    return false;
}

std::vector<CarmaGame::RaceRow> CarmaGame::readRaceTable() {
    std::vector<RaceRow> rows;
    int i = locate("Races_Strings_UK.txt", "", "");
    if (i < 0) i = locate("Races_Strings_US.txt", "", "");
    std::string t;
    if (i < 0 || !vfs::readText(vfs::files()[i].path, t)) return rows;
    for (const std::string& l : splitLines(t)) {
        auto q = quotedStrings(stripComment(l));
        if (l.compare(0, 4, "RACE") == 0) {
            size_t a = l.find('"');
            size_t b = a == std::string::npos ? a : l.find('"', a + 1);
            if (b != std::string::npos) rows.push_back({l.substr(a + 1, b - a - 1), "", ""});
        } else if (!rows.empty() && !q.empty()) {
            if (l.compare(0, 5, "WORLD") == 0) rows.back().world = q[0];
            else if (l.compare(0, 12, "DISPLAY_NAME") == 0) rows.back().display = q[0];
        }
    }
    return rows;
}

std::vector<LevelInfo> CarmaGame::listLevels() {
    struct Entry { int order; std::string key; LevelInfo info; };
    std::vector<Entry> entries;
    std::vector<RaceRow> table = readRaceTable();
    for (const std::string& d : trackDirs()) {
        std::string nm = vfs::baseOf(d);
        std::vector<uint8_t> data;
        int li = vfs::findIn(d, nm + ".txt");
        bool ok = li >= 0 && vfs::read(li, data);
        if (!ok) {
            std::vector<std::unique_ptr<Pak>> paks;
            openDirPaks(d, paks);
            ok = readFromPaks(paks, nm + ".txt", data);
        }
        if (!ok) continue;
        KeyFile kf;
        kf.parse(std::string((const char*)data.data(), data.size()));
        if (!kf.get("STATIC_MESH_DESCRIPTOR")) continue;
        std::string title = kf.str("NAME");
        if (title.empty()) title = nm;
        std::string lnm = vfs::lower(nm), group;
        int order = 1000;
        for (size_t k = 0; k < table.size(); k++)
            if (vfs::lower(table[k].name) == lnm) {
                order = (int)k;
                group = table[k].world.empty() ? nm : table[k].world;
                if (!table[k].display.empty()) title += " \xe2\x80\x93 " + table[k].display;
                break;
            }
        {
            std::string lt = vfs::lower(title);
            if (order != 1000 && lt.find("multiplayer") != std::string::npos) { order = 4000 + order; group = "Multiplayer & arenas"; }
            else if (order != 1000 && lt.find("free driving") != std::string::npos) { order = 2000 + order; group = "Free driving"; }
        }
        if (order == 1000) {
            bool addon = lnm.compare(0, 5, "1920s") == 0 || lnm.compare(0, 4, "hell") == 0 || lnm.compare(0, 14, "super speedway") == 0;
            bool freeDrive = vfs::lower(title).find("free driving") != std::string::npos;
            if (addon) { group = "Add-on levels"; order = 3000; }
            else if (freeDrive) { group = "Free driving"; order = 2000; }
            else { group = "Multiplayer & arenas"; order = 4000; }
        }
        entries.push_back({order, lnm, {d, title, group}});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.order != b.order ? a.order < b.order : a.key < b.key;
    });
    std::vector<LevelInfo> out;
    for (auto& e : entries) out.push_back(std::move(e.info));
    return out;
}

void CarmaGame::indexBases() {
    if (!byBase.empty() && byBaseFiles == vfs::files().size()) return;  // rebuilt when files were added
    byBase.clear();
    byBaseFiles = vfs::files().size();
    for (size_t i = 0; i < vfs::files().size(); i++) byBase[vfs::lower(vfs::baseOf(vfs::files()[i].path))].push_back((int)i);
}

int CarmaGame::locate(const std::string& name, const std::string& first, const std::string& second) {
    indexBases();
    auto it = byBase.find(vfs::lower(name));
    if (it == byBase.end()) return -1;
    std::string f1 = vfs::lower(first) + "/", f2 = vfs::lower(second) + "/";
    int best = -1, bestScore = -1;
    for (int i : it->second) {
        const std::string& lp = vfs::files()[i].lower;
        int score = lp.compare(0, f1.size(), f1) == 0 ? 3 : (!second.empty() && lp.compare(0, f2.size(), f2) == 0) ? 2 : 1;
        if (score > bestScore) { best = i; bestScore = score; }
    }
    return best;
}

Pak* CarmaGame::adopt(std::unique_ptr<Pak> p) {
    paks.push_back(std::move(p));
    return paks.back().get();
}

bool CarmaGame::readNamed(const std::string& name, std::vector<uint8_t>& out) {
    int i = locate(name, trackDir, baseDir);
    if (i >= 0) {
        const std::string& lp = vfs::files()[i].lower;
        std::string t = vfs::lower(trackDir) + "/", b = vfs::lower(baseDir) + "/";
        bool near = lp.compare(0, t.size(), t) == 0 || lp.compare(0, b.size(), b) == 0;
        if (near && vfs::read(i, out)) return true;
    }
    for (Pak* p : trackPaks) {
        int e = p->find(name);
        if (e >= 0 && p->extract(e, out)) return true;
    }
    return i >= 0 && vfs::read(i, out);
}

bool CarmaGame::readNamedText(const std::string& name, std::string& out) {
    std::vector<uint8_t> d;
    if (!readNamed(name, d)) return false;
    out.assign((const char*)d.data(), d.size());
    return true;
}

Pak* CarmaGame::pakForHie(const std::string& base) {
    std::string hie = base + ".hie";
    for (Pak* p : trackPaks)
        if (p->find(hie) >= 0) return p;
    for (auto& p : paks)
        if (vfs::lower(p->base) == vfs::lower(base)) return p.get();
    int d = locate(base + ".dir", trackDir, baseDir);
    int k = locate(base + ".pak", trackDir, baseDir);
    if (d < 0 || k < 0) return nullptr;
    auto p = std::make_unique<Pak>();
    if (!p->open(d, k, base)) return nullptr;
    return adopt(std::move(p));
}

Pak* CarmaGame::carmaPak() {
    if (carma) return carma;
    int d = locate("CARMA.dir", "", ""), k = locate("CARMA.pak", "", "");
    if (d < 0 || k < 0) return nullptr;
    auto p = std::make_unique<Pak>();
    if (!p->open(d, k, "CARMA")) return nullptr;
    return carma = adopt(std::move(p));
}

bool CarmaGame::readRaceBlock(const std::string& track, KeyFile& out) {
    Pak* p = carmaPak();
    std::vector<uint8_t> d;
    int e = p ? p->find("races.txt") : -1;
    if (e < 0 || !p->extract(e, d)) return false;
    auto trim = [](std::string v) {
        size_t a = v.find_first_not_of(" \t\r"), b = v.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
    };
    std::string block;
    bool in = false, match = false;
    for (const std::string& raw : splitLines(std::string((const char*)d.data(), d.size()))) {
        std::string l = trim(stripComment(raw));
        if (l == "RACE") { in = true; match = false; block.clear(); continue; }
        if (!in) continue;
        if (l == "END") {
            if (match) { out.parse(block); return true; }
            in = false;
            continue;
        }
        block += l + "\n";
        if (l.compare(0, 5, "TRACK") == 0) {
            auto q = quotedStrings(l);
            match = !q.empty() && vfs::lower(q[0]) == vfs::lower(track);
        }
    }
    return false;
}

bool CarmaGame::readRacePup(const KeyFile& race, std::string& text) {
    std::string file = race.str("POWERUPS");
    if (file.empty()) return false;
    std::string save = race.str("POWERUPS_SAVENAME");
    for (char& c : save) if (c == '\\') c = '/';
    size_t e = save.find_last_of('/');
    size_t b = e == std::string::npos || e == 0 ? std::string::npos : save.find_last_of('/', e - 1);
    std::string folder = b == std::string::npos ? "" : save.substr(b + 1, e - b - 1);
    std::string tracks = vfs::dirOf(trackDir);
    std::vector<uint8_t> d;
    if (!folder.empty())
        for (const std::string& sd : vfs::subdirs(tracks)) {
            if (vfs::lower(sd) != vfs::lower(folder)) continue;
            std::string dir = tracks + "/" + sd;
            int li = vfs::findIn(dir, file);
            if (li >= 0 && vfs::read(li, d)) { text.assign((const char*)d.data(), d.size()); return true; }
            std::vector<std::unique_ptr<Pak>> tmp;
            openDirPaks(dir, tmp);
            if (readFromPaks(tmp, file, d)) { text.assign((const char*)d.data(), d.size()); return true; }
        }
    return readNamedText(file, text);  // the level's own archives
}

bool CarmaGame::findPup(const std::string& levelName, std::string& text) {
    std::string ln = vfs::lower(levelName), suffix;
    size_t us = ln.find('_');
    if (us != std::string::npos) suffix = ln.substr(us + 1);
    auto score = [&](const std::string& file) {
        std::string stem = vfs::lower(file);
        if (endsWith(stem, ".pup")) stem.resize(stem.size() - 4);
        size_t sl = stem.find_last_of('/');
        if (sl != std::string::npos) stem = stem.substr(sl + 1);
        if (stem == ln) return 3;
        if (!suffix.empty() && stem.find(suffix) != std::string::npos) return 2;
        // Missions, free driving and arenas often ship a "<Track>_Race1.pup" that serves them all; races need their own.
        if (suffix.compare(0, 4, "race") != 0) return 1;
        return 0;
    };
    int best = 0;
    Pak* bestPak = nullptr;
    std::string bestName;
    int bestLoose = -1;
    for (Pak* p : trackPaks)
        for (const DirEntry& e : p->entries)
            if (endsWith(vfs::lower(e.name), ".pup")) {
                int sc = score(e.name);
                if (sc > best) { best = sc; bestPak = p; bestName = e.name; bestLoose = -1; }
            }
    std::string t = vfs::lower(trackDir) + "/", b = vfs::lower(baseDir) + "/";
    for (size_t i = 0; i < vfs::files().size(); i++) {
        const auto& f = vfs::files()[i];
        if (!endsWith(f.lower, ".pup") || (f.lower.compare(0, t.size(), t) != 0 && f.lower.compare(0, b.size(), b) != 0)) continue;
        int sc = score(f.path);
        if (sc > best) { best = sc; bestPak = nullptr; bestLoose = (int)i; }
    }
    std::vector<uint8_t> d;
    if (bestLoose >= 0) { if (!vfs::read(bestLoose, d)) return false; }
    else if (bestPak) { int e = bestPak->find(bestName); if (e < 0 || !bestPak->extract(e, d)) return false; }
    else return findSiblingPup(suffix.compare(0, 4, "race") == 0, text);
    text.assign((const char*)d.data(), d.size());
    return true;
}

bool CarmaGame::findSiblingPup(bool isRace, std::string& text) {
    if (isRace) return false;
    std::string parent = vfs::dirOf(baseDir), want = vfs::lower(vfs::baseOf(baseDir)) + "_race1", sib;
    for (const std::string& sd : vfs::subdirs(parent))
        if (vfs::lower(sd) == want) sib = parent + "/" + sd;
    if (sib.empty()) return false;
    std::string pre = vfs::lower(sib) + "/";
    std::vector<uint8_t> d;
    for (size_t i = 0; i < vfs::files().size(); i++) {  // loose .pup
        const auto& f = vfs::files()[i];
        if (endsWith(f.lower, ".pup") && f.lower.compare(0, pre.size(), pre) == 0 && vfs::read((int)i, d)) {
            text.assign((const char*)d.data(), d.size());
            return true;
        }
    }
    std::vector<std::unique_ptr<Pak>> tmp;  // .pup inside the sibling's archives
    openDirPaks(sib, tmp);
    for (auto& p : tmp)
        for (size_t e = 0; e < p->entries.size(); e++)
            if (endsWith(vfs::lower(p->entries[e].name), ".pup") && p->extract((int)e, d)) {
                text.assign((const char*)d.data(), d.size());
                return true;
            }
    return false;
}

std::string CarmaGame::findAiPathHie() {
    for (Pak* p : trackPaks)
        for (const DirEntry& e : p->entries) {
            std::string n = e.name;
            if (!endsWith(vfs::lower(n), ".lins")) continue;
            std::string stem = n.substr(0, n.size() - 5);
            if (p->find(stem + ".hie") >= 0) return stem + ".hie";
        }
    return "";
}

std::string CarmaGame::findDronePathHie() {
    std::string t = vfs::lower(trackDir) + "/", b = vfs::lower(baseDir) + "/";
    for (const auto& f : vfs::files()) {
        if (!endsWith(f.lower, ".dir") || (f.lower.compare(0, t.size(), t) != 0 && f.lower.compare(0, b.size(), b) != 0)) continue;
        std::string stem = vfs::stemOf(f.path), ls = vfs::lower(stem);
        if (ls.find("drone") != std::string::npos && ls.find("path") != std::string::npos) return stem + ".hie";
    }
    return "";
}
void CarmaGame::loadPupTable() {
    std::string t;
    pupTable = PupTable();
    if (readNamedText("PowerupStrings_UK.txt", t) || readNamedText("PowerupStrings_US.txt", t)) pupTable.parse(t);
}

// TEXTURE_ANIM_DESCRIPTOR: a count, then one `"script.anm" "texture"` line per animated texture. A script is a loop of
// `tex <frame>` lines at `set framerate <fps>`, ended by `jmp start`.
void CarmaGame::readTexAnims(const KeyFile& kf) {
    std::string desc, dt;
    desc = kf.str("TEXTURE_ANIM_DESCRIPTOR");
    if (desc.empty() || !readNamedText(desc, dt)) return;
    for (const std::string& l : splitLines(dt)) {
        auto q = quotedStrings(stripComment(l));
        std::string scriptText;
        if (q.size() < 2 || !readNamedText(q[0], scriptText)) continue;
        TexScript ts;
        for (const std::string& raw : splitLines(scriptText)) {
            std::istringstream is(stripComment(raw));
            std::string cmd, a, b;
            is >> cmd >> a >> b;
            if (cmd == "set" && a == "framerate") ts.fps = (float)atof(b.c_str());
            else if (cmd == "tex" && !a.empty()) ts.frames.push_back(a);
            else if (cmd == "jmp") break;
        }
        if (!ts.frames.empty() && ts.fps > 0) texScripts[vfs::lower(q[1])] = std::move(ts);
    }
}

}  // namespace carma
