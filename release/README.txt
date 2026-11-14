Far Cry 2 fast travel mod (single-player)
=========================================

Install
-------
Copy the "bin" folder from here into the Far Cry 2 game folder, so the three
files land next to FarCry2.exe:

  <game>\bin\dinput8.dll              the mod itself
  <game>\bin\FastTravel.ini           settings (optional; created with defaults if missing)
  <game>\bin\FastTravel.arrivals.txt  saved arrival spots (optional; see below)

No game file is replaced. A clean game has no dinput8.dll in its bin folder.

Use
---
Press F7 in the game to open the map, click an icon to travel, F7 or Esc to
close. The map does not open in menus, and travel is refused in a vehicle.

Settings (FastTravel.ini, read when the game starts)
  Hotkey        F1 to F12, default F7
  GraceSeconds  0 to 15, default 5: seconds enemies ignore you after a trip

Requirements and limits
-----------------------
- The mod only works with one specific build of the game's Dunia.dll (the one
  in the Steam copy it was made on). With any other build it switches itself
  off and says so in FastTravel.log. If another mod replaces Dunia.dll, the
  same applies.
- The map pictures are read from the game's own files (Data_Win32\patch.fat or
  Data_Win32\worlds\worlds.fat). If a picture is not found, icons are drawn on
  a plain dark panel.
- Single-player only.
- Cutscene detection is not finished: the map can be opened during scripted
  scenes. Don't travel during them.

Files the mod writes in <game>\bin
----------------------------------
  FastTravel.log           log of the current session
  FastTravelLogs\          logs of earlier sessions (safe to delete)
  FastTravel.arrivals.txt  outside arrival spots for locked safehouses and
                           weapon shops. Without it, the first trip to such a
                           place shows two loading screens while the spot is
                           worked out; later trips show one.

Remove
------
Delete dinput8.dll and the FastTravel* files and folder from <game>\bin.
