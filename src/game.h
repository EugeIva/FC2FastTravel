#pragma once
#include <windows.h>

// Access to Far Cry 2 engine state inside Dunia.dll.
//
// All offsets below were found by static analysis of one specific engine
// build (PE timestamp 0x49FB4BF6, SHA-256 FCE1CAB5...1F592D) and are only
// valid for it. GameInit() refuses any other build.

struct Vec3 {
    float x, y, z;
};

struct PlayerState {
    void* entity;        // engine entity object of the local player
    unsigned long long id; // entity id (two dwords at entity+8)
    Vec3 pos;            // world position (entity+0x60)
};

// Returns false (and logs why) if Dunia.dll is missing or is a different build.
bool GameInit();

// Reads the local player's entity and position. Returns false when there is no
// player yet (menus, loading) or the memory could not be read.
bool GameReadPlayer(PlayerState* out);

// Runs the engine's own teleport sequence (loading screen, move, synchronous
// world load) with the given destination. Must be called on the game's main
// thread. Returns false if the engine code does not look as expected or the
// call faulted.
bool GameTeleportPlayer(const Vec3& dest);

// Replaces one entry in Dunia.dll's import table (for example "USER32.dll",
// "PeekMessageA") with `hook`. Returns the original function, or nullptr on
// failure.
void* GameHookImport(const char* dll, const char* function, void* hook);

// Map element types, in the order the engine registers them.
enum {
    kMapObjective = 0,
    kMapPGP = 1,
    kMapSafeHouse = 2,
    kMapCellTower = 3,
    kMapWeaponShop = 4,
    kMapBusStation = 5,
    kMapUnderground = 6,
    kMapHQ = 7,
    kMapMikesPlace = 8,
    kMapOther = 9,
    kMapCity = 10,
    kMapElementTypeCount = 11
};

// One map icon entity.
struct MapElement {
    int type;            // kMap...
    bool discovered;
    unsigned long state; // unnamed field at component+0x16C (0, 1 or 2)
    Vec3 pos;            // world position of the entity
    // Safehouses only (from the entity's CSafeHouseComponent):
    int locked;          // 1 locked, 0 unlocked, -1 not a safehouse
    bool haveLinked[3];  // entities the safehouse refers to at +0x40, +0x44,
    Vec3 linked[3];      // +0x48 (one may be the door), where loaded
};

// Reads every map element into `out` (up to `max`). Returns the number
// written, -1 if the entity manager is not available, -2 if reading faulted.
// Must be called on the game's main thread.
int GameGetMapElements(MapElement* out, int max);

// Terrain (ground) height at a world position, from the engine's own terrain
// data. Returns false if the terrain is not available there. This is the bare
// ground: it knows nothing about buildings, rocks or water.
bool GameTerrainHeight(float x, float y, float* z);

// The engine's own pause flag (the one its menus set). 1 paused, 0 running,
// -1 unknown.
int GameIsPaused();
// Sets or clears that flag. Returns false if it could not be reached.
bool GameSetPaused(bool paused);

// Whether the player is in a vehicle (driver, passenger or mounted gun), from
// the engine's own lookup. 1 yes, 0 no, -1 unknown. Main thread only.
int GamePlayerInVehicle();

// After a teleport the engine makes its enemy squad commanders stand down
// for a fixed time (15 s in this build). This shortens what is left of that
// to at most `graceSeconds`. Returns how many commanders were adjusted, or -1
// if the AI objects could not be reached. Main thread only.
int GameShortenAiStandDown(float graceSeconds);

// The engine's "cinematic UI mode" flag, which the game's scripts switch on
// with SetCinematicUIMode to hide the HUD during scripted scenes.
// 1 on, 0 off, -1 unknown. Main thread only.
int GameInCinematic();

// The engine's own check on the same service object (virtual, 0x10728470),
// believed to be "saving is allowed right now": false while mission scripts
// hold the player. 1 allowed, 0 not, -1 unknown. Main thread only.
int GameSaveAllowed();
