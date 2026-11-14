#pragma once
#include <windows.h>

// Appends one timestamped line to FastTravel.log next to the mod DLL.
void LogInit(HMODULE self);
void Log(const char* fmt, ...);
