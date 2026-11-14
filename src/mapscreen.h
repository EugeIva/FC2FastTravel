#pragma once

// The mod's own map screen: a full-screen panel with the game's map icons,
// a mouse cursor, and click-to-travel.

// Call once at startup.
void MapScreenInit();

// Call every frame on the game's main thread. Handles the hotkey, the cursor,
// hovering and clicking, and starts the teleport.
void MapScreenTick();

// True while the map screen is showing. Safe from any thread.
bool MapScreenIsOpen();

// Draws the screen. Called by the overlay on the render thread.
void MapScreenRender(int screenW, int screenH);

// Name of the key that opens the map (from FastTravel.ini), for messages.
const char* MapScreenHotkeyName();
