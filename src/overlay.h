#pragma once
#include <windows.h>

// Direct3D 9 drawing on top of the game. The overlay hooks the device's
// Present call and, while the map screen is open, calls MapScreenRender()
// just before the frame is shown.

// Hooks Dunia.dll's import of Direct3DCreate9. Call once, early.
void OverlayInstall();

// Size of the back buffer seen at the last Present (0 until the first one).
void OverlayGetScreenSize(int* w, int* h);

// Drawing calls, valid only inside MapScreenRender(). Colours are 0xAARRGGBB.
void DrawRect(float x0, float y0, float x1, float y1, DWORD color);
void DrawFrame(float x0, float y0, float x1, float y1, float thickness, DWORD color);
void DrawDiamond(float cx, float cy, float r, DWORD color);
void DrawTextAt(float x, float y, const char* text, DWORD color);
float TextWidth(const char* text);
float TextHeight();

// Draws a texture from the game's archives (by file name hash) into the
// rectangle. The texture is loaded on first use. Returns false if it is not
// available, in which case nothing is drawn.
bool DrawGameTexture(unsigned long nameHash, float x0, float y0, float x1, float y1, DWORD color);
