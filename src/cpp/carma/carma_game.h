// Carmageddon TDR 2000 (Torus Games, 2000).
//   ASSETS/Tracks/<track>/<track>.txt            level descriptor (text)
//   ASSETS/Tracks/<track>/**/<name>.dir + .pak   archives: names in a prefix-compressed trie (.dir),
//                                                 payloads zlib-compressed and header-obfuscated (.pak)
//   ASSETS/CARMA.pak                             game-wide data: races.txt, strings, ...
//   <name>.hie    text scene graph: matrices / textures / meshes / paths
//   <name>.mshs   binary meshes (position, RGBA colour, uv, triangle list)
//   <name>_NxN_32.tga / _8.tga   textures
//   .lins         spline point lists, .pup powerups, _VOL.scol checkpoint volumes
// Native coordinates are Y-up; no conversion except an optional X mirror (kMirrorX).
//
// Sources: carma_files.cpp (file lookup, races.txt, level list), carma_geometry.cpp (hierarchies -> meshes),
// carma_movers.cpp (path followers, animated props), carma_level.cpp (the steps of load()), carma.cpp (load()).
#pragma once
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "carma/carma_formats.h"
#include "carma/carma_pak.h"
#include "carma/carma_paths.h"
#include "carma/carma_powerups.h"
#include "carma/carma_text.h"
#include "game.h"

namespace carma {

// Category ids of the marker layers of one level.
struct Cats {
    int start, pup, special, pedPup, time, upgrade, credit, missionItem, mission, check, race, startLine, ai, drone, ped, respawn;
};

// A powerup placed in the level (id < 0: a mission objective marker without an in-game mesh).
struct Placed { int id; int cat; Vec3 pos; };

struct State {
    Mat4 m = Mat4::identity();
    int tex = -1;
    int attr = -1;
    int mat = -1;
};

enum { MIR_WORLD, MIR_NONE, MIR_X, MIR_Z };  // how emitHie mirrors the geometry (see kMirrorX)

class CarmaGame : public Game {
public:
    const char* id() const override { return "carma"; }
    const char* name() const override { return "Carmageddon TDR 2000"; }
    const char* dropHint() const override { return "Carmageddon TDR 2000 folder (the one containing ASSETS/)"; }

    // Directories directly below a folder called "Tracks".
    std::vector<std::string> trackDirs();

    // Opens every <x>.dir/<x>.pak pair that sits directly in `dir`.
    void openDirPaks(const std::string& dir, std::vector<std::unique_ptr<Pak>>& into);

    static bool readFromPaks(const std::vector<std::unique_ptr<Pak>>& paks, const std::string& name, std::vector<uint8_t>& out);

    bool detect() override;

    // Races_Strings_*.txt lists every race / mission of the campaign in the order the game presents them.
    struct RaceRow { std::string name, world, display; };
    std::vector<RaceRow> readRaceTable();

    std::vector<LevelInfo> listLevels() override;

    // ---- file resolution
    std::unordered_map<std::string, std::vector<int>> byBase;
    size_t byBaseFiles = 0;  // number of files the index was built from
    void indexBases();
    // Finds loose file `name`, preferring files under `first`, then `second`.
    int locate(const std::string& name, const std::string& first, const std::string& second);

    // ---- per-load state
    Scene* sc = nullptr;
    BatchSet* coronaDst = nullptr;  // when set, meshes under a "corona" matrix go here, centred on that matrix (billboard halos)
    Vec3 coronaCenter;
    bool coronaFound = false;
    Mat4 lastRootMat = Mat4::identity();  // first matrix of the hierarchy emitted last
    BatchSet* dst = nullptr;  // when set, emitHie writes into this (local space) instead of the level
    std::string trackDir, baseDir;
    std::vector<std::unique_ptr<Pak>> paks;     // everything opened so far (stable pointers)
    std::vector<Pak*> trackPaks;                // archives directly inside the track / base folder
    std::unordered_map<std::string, int> texCache;
    // TEXTURE_ANIM_DESCRIPTOR: texture name (lower case) -> the frames of its animation script
    struct TexScript { std::vector<std::string> frames; float fps = 30; };
    std::unordered_map<std::string, TexScript> texScripts;
    int missingTex = 0;
    std::vector<std::string> missingTexNames;
    std::vector<std::string> missing;

    Pak* adopt(std::unique_ptr<Pak> p);

    // Reads a descriptor-like file: loose copy near the track, else inside the track archives, else anywhere.
    bool readNamed(const std::string& name, std::vector<uint8_t>& out);
    bool readNamedText(const std::string& name, std::string& out);

    // The archive that holds `<base>.hie`: one of the track archives, or a stand-alone <base>.dir/.pak.
    Pak* pakForHie(const std::string& base);

    int textureFor(const std::string& name, Pak* preferred);


    // Walks the scene graph; `leaf` is called for every mesh (type 3) and spline (type 6) node.
    template <class F>
    void walk(const Hie& h, F&& leaf) {
        if (h.nodes.empty()) return;
        struct Item { int node; State st; };
        std::vector<Item> stack;
        stack.push_back({0, State()});
        std::vector<char> seen(h.nodes.size(), 0);
        while (!stack.empty()) {
            Item it = stack.back();
            stack.pop_back();
            if (it.node < 0 || it.node >= (int)h.nodes.size() || seen[it.node]) continue;
            seen[it.node] = 1;
            const Hie::Node& n = h.nodes[it.node];
            State cur = it.st;
            switch (n.type) {
                case 1:
                    if (n.index >= 0 && n.index < (int)h.mats.size()) {
                        cur.m = Mat4::mul(it.st.m, h.mats[n.index].m);
                        cur.attr = n.index;
                    }
                    break;
                case 2: cur.tex = n.index; break;
                case 5: cur.mat = n.index; break;
                case 3:
                case 6: leaf(n, cur); break;
                default: break;
            }
            if (n.sibling >= 0) stack.push_back({n.sibling, it.st});
            if (n.child >= 0) stack.push_back({n.child, cur});
        }
    }

    bool loadHie(const std::string& base, Pak*& pk, Hie& hie);


    // mir: MIR_WORLD for level geometry; MIR_X / MIR_Z mirror model-space meshes (icons / path followers, whose forward is +X);
    // MIR_NONE leaves the mesh raw. com receives the height of the centre of mass from the .dcol (movable objects).
    void emitHie(const std::string& base, int& meshCount, bool water = false, int mir = MIR_WORLD, float* com = nullptr);

    // Paths: .lins + hie placed with parent matrices.
    void emitPaths(const std::string& hieBase, int catAI, int catRace, int catStart, int catDrone, bool drone);

    // Polyline of the first spline of a path hierarchy, in world space.
    bool loadPathPoints(const std::string& base, std::vector<Vec3>& pts);



    // ---- races.txt (ASSETS/CARMA.pak): one RACE block per race / mission. It names the powerup file (POWERUPS, owned by the level
    // in POWERUPS_SAVENAME: missions often borrow the file of a race), the AI and drone path hierarchies, the mission script...
    Pak* carma = nullptr;
    Pak* carmaPak();

    // The RACE block whose TRACK is `track`, parsed as key / value lines. False for levels that races.txt does not list.
    bool readRaceBlock(const std::string& track, KeyFile& out);

    // The powerup file of a race block. POWERUPS_SAVENAME ("assets\tracks\<level>\<file>") says which level's folder holds it.
    bool readRacePup(const KeyFile& race, std::string& text);

    // ---- discovery by naming convention, for levels that races.txt does not list (free driving, add-ons)
    // Powerup file for this level: "<level>.pup" or one whose name carries the level's suffix ("race1", "mission2").
    bool findPup(const std::string& levelName, std::string& text);

    // Non-race levels (missions, free driving) use the powerups of the base track, which ship with its Race 1.
    bool findSiblingPup(bool isRace, std::string& text);

    // AI path hierarchy ("<name>.hie" + "<name>.lins") inside one of the track's own archives.
    std::string findAiPathHie();

    // Drone / traffic path archive ("...Drone...Path....dir") anywhere in the track or base folder.
    std::string findDronePathHie();



    // PATH_FOLLOWERS: meshes that travel along splines (helicopters, energy rings, boats...).
    //   @name / "mesh.hie" / "path.hie" / "anim" / speed sway swayMag animTime / startTime / frames / frameNulls
    void loadFollowers(const std::string& fname, Scene& out, int cat, int pathCat);

    // Animated props: lifts, generator wheels and the Police State behemoths. Each descriptor names a hierarchy and a
    // looping list of movements (duration, translation, rotation axis, rotation in degrees).
    void loadAnimProps(const std::string& fname, Scene& out, int cat, int pathCat);

    // ---- powerup metadata (PowerupStrings_*.txt)
    PupTable pupTable;
    void loadPupTable();

    // ---- path follower options
    bool pathAlignment(const std::string& pathBase, float m[16]);
    int followerSmoothIndex(const std::string& hieBase);

    // ---- level assembly: load() runs these in order
    bool load(const std::string& levelId, Scene& out, std::string& err) override;
    // driving minigame (minigame01/eagle_loader.cpp)
    bool canDrive() override;
    bool featuredCar() const override { return true; }  // the Eagle drives in Re-Volt levels too
    bool loadCarModel(Scene& scene, minigame01::CarModel& out) override;
    void beginLoad(const std::string& levelId, Scene& out);  // resets the per-load state, finds the base track
    void readTexAnims(const KeyFile& kf);  // TEXTURE_ANIM_DESCRIPTOR and the .anm scripts it lists
    void openArchives();                                       // track, base track and shared sub folders
    bool readGeometry(const KeyFile& kf, int& meshCount, std::string& err);  // STATIC_MESH_DESCRIPTOR + WATER_MESH
    Cats addCategories(Scene& out);
    void addStart(const KeyFile& kf, Scene& out, const Cats& c);
    void addPowerups(const std::string& name, const KeyFile& race, bool listed, Scene& out, const Cats& c);
    void readPupFile(const std::string& pt, Scene& out, const Cats& c, std::vector<Placed>& placed);
    void readMissionScripts(const std::string& name, const KeyFile& race, bool listed, Scene& out, const Cats& c, std::vector<Placed>& placed);
    void addPowerupMeshes(Scene& out, const std::vector<Placed>& placed);  // the spinning in-game icons
    void addCheckpoints(const KeyFile& kf, Scene& out, const Cats& c);
    void addRoutes(const KeyFile& race, bool listed, const Cats& c);       // opponent and drone paths
    void addMovers(const KeyFile& kf, Scene& out);                          // path followers, animated props
    void addPeds(const KeyFile& kf, Scene& out, const Cats& c);
    void addRespawns(const KeyFile& kf, const std::string& name, Scene& out, const Cats& c);
    void addLighting(const KeyFile& kf, Scene& out);
    void addProps(const KeyFile& kf, Scene& out);                           // breakables and movable objects
    std::string summary(const Scene& out, int meshCount) const;
};

}  // namespace carma
