#include "mapscreen.h"
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "game.h"
#include "input.h"
#include "log.h"
#include "overlay.h"

namespace {

const int kMaxIcons = 256;
const float kHoverRadius = 16.0f;   // pixels
const float kArrivalLift = 0.5f;    // metres above the icon's own height
const float kOutsideDistance = 12.0f; // metres from a safehouse icon
const float kOutsideLift = 1.0f;     // metres above the ground there
const DWORD kTeleportCooldownMs = 5000;
const DWORD kGraceTimeoutMs = 45000;
const DWORD kPendingSettleMs = 2000;    // wait after the game is running again
const DWORD kPendingTimeoutMs = 120000;

// The game's own paper-map picture for the first territory, by the hash of
// its file name in the archives, and the world square it covers (north up).
// The square is fitted by eye from the town's position; not yet checked
// against the game's own data.
const unsigned long kMapImageHash = 0xF4B9A27D;
// The second territory (south) is a separate world that uses the same
// coordinate range, so position cannot tell the two apart. The north is
// recognised by a landmark that exists only there: Mike's bar.
// The south picture's square is assumed to be the same; not yet checked.
const unsigned long kMapImageHashSouth = 0xDD71168F;
const float kNorthLandmarkX = 2612.45f, kNorthLandmarkY = 2131.43f;
const float kMapImageMin = 1024.0f, kMapImageMax = 4096.0f;

// Settings, read once from FastTravel.ini next to the game's exe. The file is
// written with defaults if it does not exist.
int g_hotkeyVk = VK_F7;
char g_hotkeyName[16] = "F7";
float g_graceSeconds = 5.0f;   // enemies ignore the player this long after a trip

void LoadSettings() {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    char* slash = strrchr(path, '\\');
    if (!slash || (slash - path) + 24 >= MAX_PATH) return;
    lstrcpyA(slash + 1, "FastTravel.ini");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = fopen(path, "w");
        if (f) {
            fputs("; Far Cry 2 fast travel mod settings. Changes apply the next time the game starts.\n"
                  "[FastTravel]\n"
                  "; Key that opens and closes the map: F1 to F12.\n"
                  "Hotkey=F7\n"
                  "; Seconds enemies ignore you after a trip (the game's own value is 15).\n"
                  "GraceSeconds=5\n", f);
            fclose(f);
        }
    }
    char key[16];
    GetPrivateProfileStringA("FastTravel", "Hotkey", "F7", key, sizeof(key), path);
    int fn = 0;
    if ((key[0] == 'F' || key[0] == 'f') && sscanf(key + 1, "%d", &fn) == 1 && fn >= 1 && fn <= 12) {
        g_hotkeyVk = VK_F1 + fn - 1;
        _snprintf(g_hotkeyName, sizeof(g_hotkeyName), "F%d", fn);
    } else {
        Log("Settings: Hotkey '%s' is not F1 to F12, using F7", key);
    }
    char grace[16];
    GetPrivateProfileStringA("FastTravel", "GraceSeconds", "5", grace, sizeof(grace), path);
    float g = 0.0f;
    if (sscanf(grace, "%f", &g) == 1 && g >= 0.0f && g <= 15.0f)
        g_graceSeconds = g;
    else
        Log("Settings: GraceSeconds '%s' is not between 0 and 15, using 5", grace);
    Log("Settings: hotkey %s, grace period %.1f s", g_hotkeyName, g_graceSeconds);
}

struct TypeStyle {
    const char* label;
    DWORD color;
    bool shown;
};
// Indexed by map element type (see game.h).
const TypeStyle kStyles[kMapElementTypeCount] = {
    {"Objective", 0xFFFFFFFF, false},
    {"Guard post (PGP)", 0xFFB0B0B0, true},
    {"Safehouse", 0xFF4CD964, true},
    {"Cell tower", 0xFF9B8CFF, true},
    {"Weapon shop", 0xFFFF9F2E, true},
    {"Bus station", 0xFF39B8FF, true},
    {"Underground", 0xFFFFFFFF, false},
    {"HQ", 0xFFFF5A5A, true},
    {"Mike's place", 0xFFFFE14D, true},
    {"Other", 0xFFE8E8E8, true},
    {"City", 0xFFFF7AD9, true},
};

struct State {
    bool open;
    int count;
    MapElement icons[kMaxIcons];
    float minX, maxX, minY, maxY;   // world bounds shown
    bool useImage;                  // bounds are the map picture's square
    bool south;                     // second territory: use its picture
    bool inVehicle;                 // player was in a vehicle when the map opened
    float cursorX, cursorY;         // pixels
    bool havePlayer;
    Vec3 player;
    int hover;                      // icon index or -1
    char status[96];
};

CRITICAL_SECTION g_lock;
State g_state;
volatile LONG g_open = 0;
DWORD g_lastTeleportTick = 0;

struct Layout {
    float x0, y0, size;   // square map area in pixels
};

Layout MakeLayout(int w, int h) {
    Layout l;
    float margin = 70.0f;
    l.size = (float)h - 2 * margin;
    if (l.size > (float)w - 2 * margin) l.size = (float)w - 2 * margin;
    if (l.size < 100.0f) l.size = 100.0f;
    l.x0 = ((float)w - l.size) * 0.5f;
    l.y0 = ((float)h - l.size) * 0.5f;
    return l;
}

// World to screen. World y is drawn upwards (assumed north; not yet checked
// against the game's own map).
void ToScreen(const State& s, const Layout& l, float wx, float wy, float* sx, float* sy) {
    float spanX = s.maxX - s.minX, spanY = s.maxY - s.minY;
    float span = spanX > spanY ? spanX : spanY;
    if (span < 1.0f) span = 1.0f;
    float scale = l.size / span;
    float offX = (l.size - spanX * scale) * 0.5f, offY = (l.size - spanY * scale) * 0.5f;
    *sx = l.x0 + offX + (wx - s.minX) * scale;
    *sy = l.y0 + l.size - offY - (wy - s.minY) * scale;
}

// A locked safehouse cannot be left from the inside (its door is switched off
// until the safehouse is unlocked), and its icon may sit on the roof. The
// ground height is only loaded once the player is there. So the first trip to
// such a place has two steps: travel to the icon, then, once the game is
// running again, travel a second time to open ground a few metres away. The
// outside spot is remembered in a file, so later trips go there directly.
struct Pending {
    bool active;
    Vec3 icon;
    bool safehouse;    // the icon is a safehouse (the other case is a weapon shop)
    Vec3 dest;         // where the first step put the player
    DWORD startTick;
    DWORD readyTick;   // when the terrain first became available, or 0
    DWORD runningTick; // when the player was first seen to move, or 0
};
Pending g_pending = {};

struct Arrival {
    Vec3 icon, dest;
};
const int kMaxArrivals = 128;
Arrival g_arrivals[kMaxArrivals];
int g_arrivalCount = 0;
char g_arrivalsPath[MAX_PATH] = "";

void LoadArrivals() {
    DWORD n = GetModuleFileNameA(nullptr, g_arrivalsPath, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        g_arrivalsPath[0] = 0;
        return;
    }
    char* slash = strrchr(g_arrivalsPath, '\\');
    if (!slash || (slash - g_arrivalsPath) + 32 >= MAX_PATH) {
        g_arrivalsPath[0] = 0;
        return;
    }
    lstrcpyA(slash + 1, "FastTravel.arrivals.txt");
    FILE* f = fopen(g_arrivalsPath, "r");
    if (!f) return;
    char line[200];
    while (g_arrivalCount < kMaxArrivals && fgets(line, sizeof(line), f)) {
        Arrival a;
        if (sscanf(line, "%f %f %f %f %f %f", &a.icon.x, &a.icon.y, &a.icon.z, &a.dest.x,
                   &a.dest.y, &a.dest.z) == 6)
            g_arrivals[g_arrivalCount++] = a;
    }
    fclose(f);
}

const Arrival* FindArrival(const Vec3& icon) {
    for (int i = 0; i < g_arrivalCount; ++i)
        if (fabsf(g_arrivals[i].icon.x - icon.x) < 0.5f &&
            fabsf(g_arrivals[i].icon.y - icon.y) < 0.5f)
            return &g_arrivals[i];
    return nullptr;
}

void SaveArrival(const Vec3& icon, const Vec3& dest) {
    if (FindArrival(icon) || g_arrivalCount >= kMaxArrivals) return;
    Arrival a = {icon, dest};
    g_arrivals[g_arrivalCount++] = a;
    if (!g_arrivalsPath[0]) return;
    FILE* f = fopen(g_arrivalsPath, "a");
    if (!f) return;
    fprintf(f, "%.2f %.2f %.2f %.2f %.2f %.2f\n", icon.x, icon.y, icon.z, dest.x, dest.y, dest.z);
    fclose(f);
}

// After each trip, once the game is running again, the enemies' stand-down
// is cut to g_graceSeconds (the engine's own value is 15 s).
struct Grace {
    bool active;
    bool arrived;   // the player has been seen at the arrival point
    Vec3 dest;
    DWORD startTick;
};
Grace g_grace = {};

void StartGrace(const Vec3& dest) {
    g_grace.active = true;
    g_grace.arrived = false;
    g_grace.dest = dest;
    g_grace.startTick = GetTickCount();
}

void GraceTick() {
    if (!g_grace.active) return;
    DWORD waited = GetTickCount() - g_grace.startTick;
    PlayerState p;
    if (!GameReadPlayer(&p)) {
        if (waited > kGraceTimeoutMs) g_grace.active = false;
        return;
    }
    // Running again: the player has moved off the exact arrival point (they
    // drop the last half metre). Fall back to a timeout in case they do not.
    // The engine moves the player a moment after the trip is requested, so
    // first wait until they are at the arrival point at all.
    bool away = fabsf(p.pos.x - g_grace.dest.x) > 0.02f ||
                fabsf(p.pos.y - g_grace.dest.y) > 0.02f ||
                fabsf(p.pos.z - g_grace.dest.z) > 0.02f;
    if (!g_grace.arrived) {
        if (!away) g_grace.arrived = true;
        if (waited < kGraceTimeoutMs) return;
    }
    bool moved = g_grace.arrived && away;
    if (!moved && waited < kGraceTimeoutMs) return;
    g_grace.active = false;
    int n = GameShortenAiStandDown(g_graceSeconds);
    if (n < 0)
        Log("AI: could not reach the squad commanders; the game's own grace period applies");
    else
        Log("AI: grace period cut to %.0f s for %d squad commander(s)%s", g_graceSeconds, n,
            moved ? "" : " (after timeout)");
}

// Picks a spot kOutsideDistance from the icon: of 16 directions, the one
// whose ground height is closest to the ground height under the icon.
bool OutsidePoint(const Vec3& icon, Vec3* dest) {
    float here = 0.0f;
    if (!GameTerrainHeight(icon.x, icon.y, &here)) return false;
    Log("Arrival: ground under the icon %.2f (icon z=%.2f)", here, icon.z);
    bool found = false;
    float bestDiff = 0.0f;
    for (int i = 0; i < 16; ++i) {
        float ang = (float)i * (6.2831853f / 16.0f);
        float x = icon.x + kOutsideDistance * cosf(ang);
        float y = icon.y + kOutsideDistance * sinf(ang);
        float h = 0.0f;
        if (!GameTerrainHeight(x, y, &h)) continue;
        float diff = fabsf(h - here);
        Log("Arrival: candidate %2d x=%.2f y=%.2f ground=%.2f", i, x, y, h);
        if (!found || diff < bestDiff) {
            found = true;
            bestDiff = diff;
            dest->x = x;
            dest->y = y;
            dest->z = h + kOutsideLift;
        }
    }
    return found;
}

void LogSafehouseAt(const Vec3& icon) {
    static MapElement all[1024];
    int n = GameGetMapElements(all, 1024);
    for (int i = 0; i < n; ++i) {
        const MapElement& e = all[i];
        if (e.type != kMapSafeHouse) continue;
        if (fabsf(e.pos.x - icon.x) > 0.5f || fabsf(e.pos.y - icon.y) > 0.5f) continue;
        Log("Safehouse: %s", e.locked == 1 ? "locked" : e.locked == 0 ? "unlocked" : "no data");
        for (int k = 0; k < 3; ++k) {
            if (e.haveLinked[k])
                Log("Safehouse: linked entity %d at x=%.2f y=%.2f z=%.2f (%.1f m from the icon)",
                    k, e.linked[k].x, e.linked[k].y, e.linked[k].z,
                    sqrtf((e.linked[k].x - icon.x) * (e.linked[k].x - icon.x) +
                          (e.linked[k].y - icon.y) * (e.linked[k].y - icon.y)));
            else
                Log("Safehouse: linked entity %d not loaded", k);
        }
        return;
    }
    Log("Safehouse: not found in the map element list");
}

// Second step of a first trip to a locked safehouse. Called every frame.
void PendingTick() {
    if (!g_pending.active) return;
    DWORD now = GetTickCount();
    if (now - g_pending.startTick > kPendingTimeoutMs) {
        g_pending.active = false;
        Log("Arrival: gave up waiting for the area to load; you are at the icon position");
        return;
    }
    PlayerState p;
    if (!GameReadPlayer(&p)) return;
    float dx = p.pos.x - g_pending.icon.x, dy = p.pos.y - g_pending.icon.y;
    if (dx * dx + dy * dy > 30.0f * 30.0f) return;   // not there yet
    float h = 0.0f;
    if (!GameTerrainHeight(g_pending.icon.x, g_pending.icon.y, &h)) return;
    if (!g_pending.readyTick) g_pending.readyTick = now ? now : 1;

    // The game is running again once the player is no longer exactly where
    // the first step put them (they fall the last half metre). Never act
    // during the loading screen.
    if (!g_pending.runningTick) {
        bool moved = fabsf(p.pos.x - g_pending.dest.x) > 0.02f ||
                     fabsf(p.pos.y - g_pending.dest.y) > 0.02f ||
                     fabsf(p.pos.z - g_pending.dest.z) > 0.02f;
        if (moved) {
            g_pending.runningTick = now ? now : 1;
            Log("Arrival: game running again at x=%.2f y=%.2f z=%.2f", p.pos.x, p.pos.y, p.pos.z);
        }
        return;
    }
    if (now - g_pending.runningTick < kPendingSettleMs) return;

    g_pending.active = false;
    if (g_pending.safehouse) LogSafehouseAt(g_pending.icon);
    Vec3 dest;
    if (!OutsidePoint(g_pending.icon, &dest)) {
        Log("Arrival: no ground data around the icon; you are at the icon position");
        return;
    }
    Log("Arrival: outside spot x=%.2f y=%.2f z=%.2f, saved for next time", dest.x, dest.y, dest.z);
    SaveArrival(g_pending.icon, dest);
    if (GameTeleportPlayer(dest)) StartGrace(dest);
    g_lastTeleportTick = GetTickCount();
}

bool g_pausedByMap = false;

void Close() {
    if (g_pausedByMap) {
        GameSetPaused(false);
        g_pausedByMap = false;
    }
    InputSetCapture(false);
    EnterCriticalSection(&g_lock);
    g_state.open = false;
    LeaveCriticalSection(&g_lock);
    InterlockedExchange(&g_open, 0);
}

void Open() {
    static MapElement all[1024];
    int n = GameGetMapElements(all, 1024);
    if (n < 0) {
        Log("Map screen: could not read the map icons (code %d)", n);
        return;
    }
    PlayerState p;
    bool havePlayer = GameReadPlayer(&p);
    if (!havePlayer) {
        Log("Map screen: not opened, no player");
        return;
    }
    if (g_pending.active) {
        Log("Map screen: not opened, a trip is still in progress");
        return;
    }
    if (GameIsPaused() == 1) {
        Log("Map screen: not opened, the game is paused or in a menu");
        return;
    }
    // Not used to refuse yet: in a test the flag was 1 during normal play.
    // Logged so a later test shows what it does during a scripted scene.
    Log("Map screen: gameplay UI flag is %d, save-allowed check is %d", GameInCinematic(),
        GameSaveAllowed());
    int vehicle = GamePlayerInVehicle();
    int w = 0, h = 0;
    OverlayGetScreenSize(&w, &h);
    if (w <= 0 || h <= 0) {
        Log("Map screen: not opened, Direct3D drawing is not hooked");
        return;
    }

    EnterCriticalSection(&g_lock);
    State& s = g_state;
    s.count = 0;
    s.minX = s.maxX = p.pos.x;
    s.minY = s.maxY = p.pos.y;
    bool north = false;
    float allMinX = 1e9f, allMaxX = -1e9f, allMinY = 1e9f, allMaxY = -1e9f;
    for (int i = 0; i < n; ++i) {
        const MapElement& e = all[i];
        if (fabsf(e.pos.x - kNorthLandmarkX) < 5.0f && fabsf(e.pos.y - kNorthLandmarkY) < 5.0f)
            north = true;
        if (e.pos.x < allMinX) allMinX = e.pos.x;
        if (e.pos.x > allMaxX) allMaxX = e.pos.x;
        if (e.pos.y < allMinY) allMinY = e.pos.y;
        if (e.pos.y > allMaxY) allMaxY = e.pos.y;
    }
    s.south = !north;
    Log("Map screen: %s territory; player x=%.1f y=%.1f; all elements span x %.0f..%.0f, y %.0f..%.0f",
        north ? "north" : "south", p.pos.x, p.pos.y, allMinX, allMaxX, allMinY, allMaxY);
    for (int i = 0; i < n && s.count < kMaxIcons; ++i) {
        const MapElement& e = all[i];
        if (e.type < 0 || e.type >= kMapElementTypeCount) continue;
        if (!kStyles[e.type].shown || !e.discovered) continue;
        s.icons[s.count++] = e;
        if (e.pos.x < s.minX) s.minX = e.pos.x;
        if (e.pos.x > s.maxX) s.maxX = e.pos.x;
        if (e.pos.y < s.minY) s.minY = e.pos.y;
        if (e.pos.y > s.maxY) s.maxY = e.pos.y;
    }
    s.useImage = p.pos.x >= kMapImageMin && p.pos.x <= kMapImageMax &&
                 p.pos.y >= kMapImageMin && p.pos.y <= kMapImageMax;
    if (s.useImage) {
        s.minX = s.minY = kMapImageMin;
        s.maxX = s.maxY = kMapImageMax;
    } else {
        float padX = (s.maxX - s.minX) * 0.04f + 20.0f, padY = (s.maxY - s.minY) * 0.04f + 20.0f;
        s.minX -= padX;
        s.maxX += padX;
        s.minY -= padY;
        s.maxY += padY;
    }
    s.cursorX = (float)w * 0.5f;
    s.cursorY = (float)h * 0.5f;
    s.havePlayer = true;
    s.player = p.pos;
    s.hover = -1;
    s.status[0] = 0;
    s.inVehicle = vehicle == 1;
    if (s.inVehicle)
        lstrcpynA(s.status, "You are in a vehicle. Get out to travel.", sizeof(s.status));
    s.open = true;
    int shown = s.count;
    LeaveCriticalSection(&g_lock);

    InputSetCapture(true);
    InterlockedExchange(&g_open, 1);
    g_pausedByMap = GameSetPaused(true);
    Log("Map screen: opened, %d icons shown of %d map elements; game %s; vehicle check: %s",
        shown, n, g_pausedByMap ? "paused" : "NOT paused",
        vehicle == 1 ? "in a vehicle" : vehicle == 0 ? "on foot" : "unknown");
}

}  // namespace

void MapScreenInit() {
    InitializeCriticalSection(&g_lock);
    ZeroMemory(&g_state, sizeof(g_state));
    g_state.hover = -1;
    LoadArrivals();
    LoadSettings();
}

const char* MapScreenHotkeyName() { return g_hotkeyName; }

bool MapScreenIsOpen() { return g_open != 0; }

void MapScreenTick() {
    static bool f7Was = false, escWas = false;
    bool f7 = (GetAsyncKeyState(g_hotkeyVk) & 0x8000) != 0;
    bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    bool f7Hit = f7 && !f7Was, escHit = esc && !escWas;
    f7Was = f7;
    escWas = esc;

    PendingTick();
    GraceTick();
    // Log changes of the cinematic flag, so a test shows when the game sets it.
    static DWORD lastCheck = 0;
    static int lastCinematic = -2;
    DWORD nowTick = GetTickCount();
    if (nowTick - lastCheck >= 500) {
        lastCheck = nowTick;
        int c = GameInCinematic();
        if (c != lastCinematic) {
            Log("Gameplay UI flag changed to %d (-1 = unknown)", c);
            lastCinematic = c;
        }
        static int lastSave = -2;
        int sv = GameSaveAllowed();
        if (sv != lastSave) {
            Log("Save-allowed check changed to %d (-1 = unknown)", sv);
            lastSave = sv;
        }
    }

    if (!MapScreenIsOpen()) {
        if (f7Hit) Open();
        return;
    }
    if (f7Hit || escHit) {
        Close();
        Log("Map screen: closed");
        return;
    }

    int dx, dy, clicks;
    InputTakeMouse(&dx, &dy, &clicks);
    int w = 0, h = 0;
    OverlayGetScreenSize(&w, &h);
    PlayerState p;
    bool havePlayer = GameReadPlayer(&p);
    if (!havePlayer) {
        Close();
        Log("Map screen: closed, player no longer available");
        return;
    }

    bool travel = false;
    MapElement target = {};
    EnterCriticalSection(&g_lock);
    State& s = g_state;
    s.player = p.pos;
    if ((dx || dy) && !s.inVehicle) s.status[0] = 0;
    s.cursorX += (float)dx;
    s.cursorY += (float)dy;
    if (s.cursorX < 0) s.cursorX = 0;
    if (s.cursorY < 0) s.cursorY = 0;
    if (s.cursorX > (float)(w - 1)) s.cursorX = (float)(w - 1);
    if (s.cursorY > (float)(h - 1)) s.cursorY = (float)(h - 1);

    Layout l = MakeLayout(w, h);
    s.hover = -1;
    float best = kHoverRadius * kHoverRadius;
    for (int i = 0; i < s.count; ++i) {
        float sx, sy;
        ToScreen(s, l, s.icons[i].pos.x, s.icons[i].pos.y, &sx, &sy);
        float ddx = sx - s.cursorX, ddy = sy - s.cursorY;
        float d2 = ddx * ddx + ddy * ddy;
        if (d2 < best) {
            best = d2;
            s.hover = i;
        }
    }
    if (clicks > 0 && s.hover >= 0) {
        if (s.inVehicle) {
            lstrcpynA(s.status, "You are in a vehicle. Get out to travel.", sizeof(s.status));
        } else if (GetTickCount() - g_lastTeleportTick < kTeleportCooldownMs) {
            lstrcpynA(s.status, "Wait a few seconds between trips", sizeof(s.status));
        } else {
            travel = true;
            target = s.icons[s.hover];
        }
    }
    LeaveCriticalSection(&g_lock);

    if (travel) {
        Close();
        Vec3 dest = target.pos;
        dest.z += kArrivalLift;
        // Places whose icon is indoors: locked safehouses and weapon shops.
        bool lockedHouse = (target.type == kMapSafeHouse && target.locked == 1) ||
                           target.type == kMapWeaponShop;
        const Arrival* known = lockedHouse ? FindArrival(target.pos) : nullptr;
        if (known) dest = known->dest;
        Log("Map screen: travel to %s at x=%.2f y=%.2f z=%.2f", kStyles[target.type].label,
            target.pos.x, target.pos.y, target.pos.z);
        bool ok = GameTeleportPlayer(dest);
        if (ok) StartGrace(dest);
        g_lastTeleportTick = GetTickCount();
        g_pending.active = ok && lockedHouse && !known;
        g_pending.icon = target.pos;
        g_pending.safehouse = target.type == kMapSafeHouse;
        g_pending.dest = dest;
        g_pending.startTick = g_lastTeleportTick;
        g_pending.readyTick = 0;
        g_pending.runningTick = 0;
        if (known) Log("Arrival: indoor icon, using the saved outside spot");
        if (g_pending.active) Log("Arrival: indoor icon, a second trip to an outside spot follows once loaded");
    }
}

void MapScreenRender(int w, int h) {
    static State s;  // render thread's copy
    EnterCriticalSection(&g_lock);
    s = g_state;
    LeaveCriticalSection(&g_lock);
    if (!s.open) return;

    Layout l = MakeLayout(w, h);
    DrawRect(0, 0, (float)w, (float)h, 0xB0000000);
    DrawRect(l.x0, l.y0, l.x0 + l.size, l.y0 + l.size, 0xE0182018);
    if (s.useImage)
        DrawGameTexture(s.south ? kMapImageHashSouth : kMapImageHash, l.x0, l.y0, l.x0 + l.size, l.y0 + l.size, 0xFFFFFFFF);
    DrawFrame(l.x0, l.y0, l.x0 + l.size, l.y0 + l.size, 2.0f, 0xFF8A8A70);

    DrawTextAt(l.x0, l.y0 - TextHeight() - 8.0f, "FAST TRAVEL", 0xFFFFFFFF);
    char hint[80];
    _snprintf(hint, sizeof(hint), "Click an icon to travel.  %s or Esc closes.", g_hotkeyName);
    hint[sizeof(hint) - 1] = 0;
    DrawTextAt(l.x0 + l.size - TextWidth(hint), l.y0 - TextHeight() - 8.0f, hint, 0xFFC8C8C8);

    // Icons, hovered one last so it is on top.
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < s.count; ++i) {
            bool hot = (i == s.hover);
            if (hot != (pass == 1)) continue;
            float sx, sy;
            ToScreen(s, l, s.icons[i].pos.x, s.icons[i].pos.y, &sx, &sy);
            float r = hot ? 10.0f : 6.0f;
            DrawRect(sx - r - 2, sy - r - 2, sx + r + 2, sy + r + 2, hot ? 0xFFFFFFFF : 0xFF000000);
            DrawRect(sx - r, sy - r, sx + r, sy + r, kStyles[s.icons[i].type].color);
        }
    }

    // Player.
    if (s.havePlayer) {
        float px, py;
        ToScreen(s, l, s.player.x, s.player.y, &px, &py);
        DrawDiamond(px, py, 12.0f, 0xFF000000);
        DrawDiamond(px, py, 9.0f, 0xFFFFFFFF);
        DrawTextAt(px + 14.0f, py - TextHeight() * 0.5f, "You", 0xFFFFFFFF);
    }

    // Legend, left of the map if there is room, otherwise inside it.
    float lx = l.x0 > 260.0f ? l.x0 - 250.0f : l.x0 + 12.0f;
    float ly = l.y0 + 12.0f;
    for (int t = 0; t < kMapElementTypeCount; ++t) {
        if (!kStyles[t].shown) continue;
        int n = 0;
        for (int i = 0; i < s.count; ++i)
            if (s.icons[i].type == t) ++n;
        if (!n) continue;
        DrawRect(lx - 2, ly + 5, lx + 14, ly + 21, 0xFF000000);
        DrawRect(lx, ly + 7, lx + 12, ly + 19, kStyles[t].color);
        char line[64];
        _snprintf(line, sizeof(line), "%s (%d)", kStyles[t].label, n);
        line[sizeof(line) - 1] = 0;
        DrawTextAt(lx + 22.0f, ly, line, 0xFFE0E0E0);
        ly += TextHeight();
    }

    // Bottom line: hovered icon or status.
    char line[160];
    line[0] = 0;
    if (s.status[0]) {
        lstrcpynA(line, s.status, sizeof(line));
    } else if (s.hover >= 0) {
        const MapElement& e = s.icons[s.hover];
        float ddx = e.pos.x - s.player.x, ddy = e.pos.y - s.player.y;
        float dist = sqrtf(ddx * ddx + ddy * ddy);
        _snprintf(line, sizeof(line),
                  "%s%s   x=%.0f y=%.0f   about %.0f m away   state %lu   (click to travel)",
                  kStyles[e.type].label,
                  e.locked == 1 ? " (locked)" : e.locked == 0 ? " (unlocked)" : "", e.pos.x,
                  e.pos.y, dist, e.state);
        line[sizeof(line) - 1] = 0;
    }
    if (line[0]) DrawTextAt(l.x0, l.y0 + l.size + 8.0f, line, 0xFFFFFFFF);

    // Cursor: a small arrow made of rectangles.
    float cx = s.cursorX, cy = s.cursorY;
    for (int i = 0; i < 14; ++i) {
        float wdt = (float)(i < 10 ? i + 1 : 14 - i) + 1.0f;
        DrawRect(cx - 1, cy + i - 1, cx + wdt + 1, cy + i + 2, 0xFF000000);
    }
    for (int i = 0; i < 14; ++i) {
        float wdt = (float)(i < 10 ? i + 1 : 14 - i);
        DrawRect(cx, cy + i, cx + wdt, cy + i + 1, 0xFFFFFFFF);
    }
}
