// Carmageddon TDR 2000: the load() sequence. The format notes are in carma_game.h, the steps in carma_level.cpp.
#include "carma_game.h"
#include "vfs.h"

using namespace carma;

bool CarmaGame::load(const std::string& levelId, Scene& out, std::string& err) {
    beginLoad(levelId, out);
    openArchives();

    std::string name = vfs::baseOf(trackDir);
    std::string text;
    if (!readNamedText(name + ".txt", text)) { err = "cannot read track descriptor"; return false; }
    KeyFile kf;
    kf.parse(text);
    // Every level descriptor is complete on its own: keys it leaves out or comments out are simply off (no inheritance).
    out.title = kf.str("NAME");
    if (out.title.empty()) out.title = name;
    float col[3];
    if (kf.nums("FOG_TRACK_COLOUR", col, 3)) out.background = {col[0] * 0.9f, col[1] * 0.9f, col[2] * 0.9f};

    KeyFile race;  // the level's block of races.txt (empty for levels it does not list)
    const bool listed = readRaceBlock(name, race);  // listed levels are described completely: no guessing for what they omit

    readTexAnims(kf);

    int meshCount = 0;
    if (!readGeometry(kf, meshCount, err)) return false;

    Cats c = addCategories(out);
    addStart(kf, out, c);
    addPowerups(name, race, listed, out, c);
    addCheckpoints(kf, out, c);
    addRoutes(race, listed, c);
    addMovers(kf, out);
    addPeds(kf, out, c);
    addRespawns(kf, name, out, c);
    addLighting(kf, out);
    addProps(kf, out);
    out.info = summary(out, meshCount);
    return true;
}

Game* makeCarma() { return new CarmaGame(); }
