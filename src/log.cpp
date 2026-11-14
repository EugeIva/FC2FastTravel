#include "log.h"
#include <cstdarg>
#include <cstdio>

static char g_logPath[MAX_PATH];
static CRITICAL_SECTION g_lock;

void LogInit(HMODULE self) {
    InitializeCriticalSection(&g_lock);
    GetModuleFileNameA(self, g_logPath, MAX_PATH);
    char* slash = strrchr(g_logPath, '\\');
    if (slash) slash[1] = 0;
    strncat(g_logPath, "FastTravel.log", MAX_PATH - strlen(g_logPath) - 1);
    // Keep the previous session's log: move it to FastTravelLogs\ under a
    // name made from the time it was last written.
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExA(g_logPath, GetFileExInfoStandard, &info) &&
        (info.nFileSizeLow || info.nFileSizeHigh)) {
        char dir[MAX_PATH], old[MAX_PATH];
        strncpy(dir, g_logPath, MAX_PATH);
        dir[MAX_PATH - 1] = 0;
        char* s2 = strrchr(dir, '\\');
        if (s2) s2[1] = 0;
        strncat(dir, "FastTravelLogs", MAX_PATH - strlen(dir) - 1);
        CreateDirectoryA(dir, nullptr);
        FILETIME local;
        SYSTEMTIME t;
        FileTimeToLocalFileTime(&info.ftLastWriteTime, &local);
        FileTimeToSystemTime(&local, &t);
        if (_snprintf(old, sizeof(old), "%s\\FastTravel-%04d%02d%02d-%02d%02d%02d.log", dir,
                      t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond) > 0) {
            old[sizeof(old) - 1] = 0;
            MoveFileA(g_logPath, old);
        }
    }
    // Start a fresh log each launch.
    HANDLE h = CreateFileA(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

void Log(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(msg, sizeof(msg) - 1, fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = 0;

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1100];
    int n = _snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s\r\n",
                      t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    if (n < 0) n = sizeof(line);

    EnterCriticalSection(&g_lock);
    HANDLE h = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(h, line, (DWORD)n, &written, nullptr);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_lock);
}
