// A "game" knows how to find its levels in the virtual file system and turn one into a Scene.
#pragma once
#include <string>
#include <vector>
#include "scene.h"

struct LevelInfo {
    std::string id;     // opaque, passed back to load()
    std::string name;   // display name
    std::string group;  // optional grouping label in the selector
};

struct Game {
    virtual ~Game() {}
    virtual const char* id() const = 0;
    virtual const char* name() const = 0;
    virtual const char* dropHint() const = 0;
    // True when the file system contains this game's data.
    virtual bool detect() = 0;
    virtual std::vector<LevelInfo> listLevels() = 0;
    virtual bool load(const std::string& levelId, Scene& out, std::string& error) = 0;
};

Game* makeRevolt();
Game* makeCarma();
