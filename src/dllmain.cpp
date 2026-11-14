// Far Cry 2 fast travel mod - Phase 3 test build: F7 opens the mod's own map
// screen; clicking an icon teleports the player there.
// Built as dinput8.dll and placed in the game's bin folder. It forwards the
// one DirectInput call the game makes to the real system DLL.
#include <windows.h>
#include "log.h"
#include "game.h"
#include "input.h"
#include "mapscreen.h"
#include "overlay.h"

struct IUnknown;
typedef HRESULT(WINAPI* DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, IUnknown*);

static DirectInput8Create_t g_realCreate = nullptr;

static DirectInput8Create_t LoadReal() {
    // Load the real DLL by full system path so we never load ourselves.
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 16) return nullptr;
    lstrcatA(path, "\\dinput8.dll");
    HMODULE real = LoadLibraryA(path);
    if (!real) {
        Log("ERROR: could not load %s (error %lu)", path, GetLastError());
        return nullptr;
    }
    Log("Real DirectInput loaded from %s", path);
    return (DirectInput8Create_t)GetProcAddress(real, "DirectInput8Create");
}

extern "C" HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID iid,
                                                   LPVOID* out, IUnknown* outer) {
    if (!g_realCreate) g_realCreate = LoadReal();
    if (!g_realCreate) return E_FAIL;
    HRESULT hr = g_realCreate(inst, version, iid, out, outer);
    Log("DirectInput8Create forwarded, result 0x%08lX", (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out) InputInstall(*out);
    return hr;
}

// The mod's per-frame work runs on the game's main thread, from inside its
// message pump.
typedef BOOL(WINAPI* PeekMessageA_t)(LPMSG, HWND, UINT, UINT, UINT);
static PeekMessageA_t g_realPeek = nullptr;
static bool g_busy = false;

static bool GameHasFocus() {
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    if (!w) return false;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

static BOOL WINAPI Hook_PeekMessageA(LPMSG msg, HWND wnd, UINT lo, UINT hi, UINT remove) {
    static DWORD loggedThread = 0;
    DWORD tid = GetCurrentThreadId();
    if (loggedThread != tid) {
        loggedThread = tid;
        Log("Message pump running on thread %lu", tid);
    }
    // The main loop's pump (RVA 0x649460) calls with hwnd 0 and PM_REMOVE. A
    // second caller on another thread (RVA 0xA3D790) uses PM_NOREMOVE; never
    // act on that one.
    if (!g_busy && wnd == nullptr && (remove & PM_REMOVE)) {
        g_busy = true;
        if (GameHasFocus() || MapScreenIsOpen()) MapScreenTick();
        g_busy = false;
    }
    PeekMessageA_t real = g_realPeek ? g_realPeek : PeekMessageA;
    return real(msg, wnd, lo, hi, remove);
}

// Installs the hotkey hook, then once a second logs the local player's
// position when it is available and has changed.
static DWORD WINAPI ModThread(LPVOID) {
    if (!GameInit()) return 0;
    OverlayInstall();
    g_realPeek = (PeekMessageA_t)GameHookImport("USER32.dll", "PeekMessageA",
                                                (void*)Hook_PeekMessageA);
    if (g_realPeek)
        Log("Hotkey ready: %s opens the fast travel map", MapScreenHotkeyName());
    else
        Log("ERROR: could not hook the message pump, the map screen is disabled");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        LogInit(self);
        MapScreenInit();
        char exe[MAX_PATH] = "";
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        Log("FC2 fast travel mod: loaded");
        Log("Host process: %s", exe);
        HANDLE t = CreateThread(nullptr, 0, ModThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
