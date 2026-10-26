// A "game" knows how to find its levels in the virtual file system and turn one into a Scene.
#pragma once
#include <string>
#include <vector>
#include "scene.h"
#include "minigame01/car_model.h"

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
    // Driving minigame: true when the game's files contain a car the minigame can use.
    virtual bool canDrive() { return false; }
    // True for the game whose car the minigame prefers in every game's levels (when its files are present).
    virtual bool featuredCar() const { return false; }
    // Size of a metre in this game's level units (the minigame scales a car from another game to fit).
    virtual float unitsPerMetre() const { return 1.0f; }
    // Adds the car's meshes and textures to `scene` (the scene that `load` produced) and describes the car. May read files.
    virtual bool loadCarModel(Scene& scene, minigame01::CarModel& out) { (void)scene; (void)out; return false; }
};

Game* makeRevolt();
Game* makeCarma();
