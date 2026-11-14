#pragma once
#include <windows.h>

// DirectInput interception. The game reads mouse and keyboard through
// DirectInput, and this DLL is the DirectInput proxy, so it can watch the
// device reads and hide them from the game while the map screen is open.

// Call with the interface the real DirectInput8Create returned.
void InputInstall(void* directInput8);

// While capture is on, mouse movement and clicks go to the mod and the game
// sees no mouse or keyboard input (key and button releases still pass, so
// nothing stays stuck down).
void InputSetCapture(bool on);

// Returns mouse movement and left-button presses accumulated since the last
// call, then clears them.
void InputTakeMouse(int* dx, int* dy, int* leftClicks);
