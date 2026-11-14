# Far Cry 2 fast travel mod

A single-player mod for Far Cry 2 (PC, Steam). Press F7 to open a map of the
current territory, click an icon, and travel there using the game's own
teleport.

- Works in both territories, drawn over the game's own paper map.
- Locked safehouses and weapon shops: you arrive outside the door.
- The game is paused while the map is open; travel is refused in a vehicle.
- Enemies ignore you for 5 seconds after a trip (configurable).

## Install

Copy the contents of `release/bin` into the game's `bin` folder, next to
`FarCry2.exe`. See [release/README.txt](release/README.txt) for settings,
limits and removal.

The mod is a proxy `dinput8.dll`. It replaces no game file and contains no
game files or game code; map pictures are read from your own installation at
run time.

## Limits

- Works with one specific build of `Dunia.dll` (PE timestamp `0x49FB4BF6`,
  the Steam version). On any other build the mod switches itself off and says
  so in `FastTravel.log`.
- Single-player only.
- Cutscene detection is not finished: the map can be opened during scripted
  scenes. Don't travel during them.

## Build

Needs Visual Studio Build Tools with the 32-bit C++ toolchain, CMake and
Ninja. Run `build.bat` (adjust the Build Tools path at the top if yours
differs). The output is `build\dinput8.dll`.
