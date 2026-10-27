// The powerup string table (PowerupStrings_*.txt): every powerup in id order, grouped by "// apo", "// car", "// peds"...
// The id -> kind and id -> icon rules are hard-coded in the game executable; the functions here follow what the game visibly does.
#pragma once
#include <string>
#include <vector>

namespace carma {

struct PupInfo { std::string group, name; };

struct PupTable {
    std::vector<PupInfo> items;

    void parse(const std::string& text);
    // 0 = generic, 1 = special ability, 2 = pedestrian powerup, 3 = permanent upgrade, 4 = credits, 5 = time bonus
    int classify(int id, const std::string& label) const;
    // Icon hierarchy for a powerup id: permanent upgrades have one icon per kind (armour / power / offensive / everything),
    // cash, pedestrian, time, random and the mission item pieces have their own, everything else is the wrench.
    std::string iconFor(int id) const;
};

}  // namespace carma
