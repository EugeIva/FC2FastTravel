#include "game.h"
#include "log.h"

namespace {

// PE TimeDateStamp of the supported Dunia.dll build (2009-05-01).
const DWORD kSupportedTimestamp = 0x49FB4BF6;

// RVAs inside Dunia.dll (preferred base 0x10000000).
// Global pointer to the player list used by the engine's "get local player"
// helper at RVA 0x134E30:
//   list   = *(void**)(base + kPlayerListPtr)
//   count  = *(u32*)(list + 8)
//   player = **(void***)(list + 4)            first entry
//   handle = *(void**)(*(u8**)(player + 4) + 8)
//   entity = *(void**)(handle + 0xC)
const DWORD kPlayerListPtr = 0x1594968;

// Entity layout (from the position getter at RVA 0x4D0C10 and the id compare
// at RVA 0x134E93).
const DWORD kEntityId = 0x08;
const DWORD kEntityPos = 0x60;

// Entry into the tail of the engine's "teleport_to_current_objective" handler
// (RVA 0x700F30), just after it has computed the destination. From here the
// handler queues these operations and submits the queue:
//   prepare loading screen, remove entity from list, teleport entity,
//   load world (synchronous), skip frames, prepare renderer, game start,
//   unload loading screen.
const DWORD kTeleportTail = 0x70116D;
// The first instruction there is: cmp dword ptr [base+0xF2D868], 0
const DWORD kTeleportTailGlobal = 0xF2D868;

// Entity manager and the engine's own iteration over all entities, as used by
// the teleport handler's objective search (RVA 0x701033..0x7010D7).
const DWORD kEntityManagerPtr = 0x1594270;   // global pointer
const DWORD kEntityMapOffset = 0x1C;         // container inside the manager
const DWORD kEntityMapEnd = 0x10;            // end node pointer inside the container
const DWORD kIterBegin = 0x7008B0;           // thiscall(iter*)
const DWORD kIterNext = 0x700BD0;            // thiscall(iter*)
const DWORD kEntityFlush = 0x4CFD30;         // thiscall(entity*)
const DWORD kGetComponent = 0x4D5530;        // thiscall(entity*, typeId*)
// CMapElementComponent type descriptor: lazy-init guard, init function, id.
const DWORD kMapElemTypeGuard = 0xF2D878;
const DWORD kMapElemTypeInit = 0x41E60;      // cdecl()
const DWORD kMapElemTypeId = 0xF2D888;
// CMapElementComponent fields (from its property table at RVA 0x4EE60).
const DWORD kMapElemUnknown16C = 0x16C;
const DWORD kMapElemType = 0x170;
const DWORD kMapElemDiscovered = 0x174;

struct EntityIter {
    void* cur;
    void* node;
    void* map;
};

typedef void(__fastcall* IterFn)(EntityIter* self, void* edx);
typedef void(__fastcall* FlushFn)(void* entity, void* edx);
typedef void*(__fastcall* GetComponentFn)(void* entity, void* edx, void* typeId);
typedef void*(__cdecl* TypeInitFn)();

BYTE* g_base = nullptr;

template <typename T>
T Read(const void* p, DWORD offset = 0) {
    return *(const T*)((const BYTE*)p + offset);
}

}  // namespace

// Used from the assembly thunk below, so they need plain external names.
void* g_fc2ftTeleportTail = nullptr;
Vec3 g_fc2ftTeleportDest = {0, 0, 0};

// Rebuilds the handler's stack frame (sub esp,0x138; push ebx,ebp,esi,edi),
// stores the destination where the handler keeps it ([esp+0x18..0x20]) and
// jumps into the handler. The handler's own epilogue restores the registers
// and returns with "ret 4", which removes our one dummy argument.
__declspec(naked) static void __stdcall TeleportThunk(int /*unused*/) {
    __asm {
        sub esp, 0x138
        push ebx
        push ebp
        push esi
        push edi
        mov eax, dword ptr [g_fc2ftTeleportDest]
        mov dword ptr [esp + 0x18], eax
        mov eax, dword ptr [g_fc2ftTeleportDest + 4]
        mov dword ptr [esp + 0x1C], eax
        mov eax, dword ptr [g_fc2ftTeleportDest + 8]
        mov dword ptr [esp + 0x20], eax
        mov dword ptr [esp + 0x24], 0xFFFFFFFF
        jmp dword ptr [g_fc2ftTeleportTail]
    }
}

static bool CallTeleportGuarded() {
    __try {
        TeleportThunk(0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GameInit() {
    g_base = (BYTE*)GetModuleHandleA("Dunia.dll");
    if (!g_base) {
        Log("ERROR: Dunia.dll is not loaded");
        return false;
    }
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)g_base;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(g_base + dos->e_lfanew);
    DWORD stamp = nt->FileHeader.TimeDateStamp;
    if (stamp != kSupportedTimestamp) {
        Log("ERROR: unsupported Dunia.dll build (timestamp 0x%08lX, expected 0x%08lX). Mod disabled.",
            stamp, kSupportedTimestamp);
        g_base = nullptr;
        return false;
    }
    Log("Dunia.dll build recognised, base 0x%p", g_base);
    return true;
}

bool GameReadPlayer(PlayerState* out) {
    if (!g_base) return false;
    // This may run on the mod's own thread while the game thread is changing
    // these structures, so every read is guarded.
    __try {
        const BYTE* list = Read<const BYTE*>(g_base, kPlayerListPtr);
        if (!list) return false;
        if (Read<DWORD>(list, 8) == 0) return false;
        const BYTE* const* first = Read<const BYTE* const*>(list, 4);
        if (!first) return false;
        const BYTE* player = *first;
        if (!player) return false;
        const BYTE* inner = Read<const BYTE*>(player, 4);
        if (!inner) return false;
        const BYTE* handle = Read<const BYTE*>(inner, 8);
        if (!handle) return false;
        const BYTE* entity = Read<const BYTE*>(handle, 0xC);
        if (!entity) return false;

        out->entity = (void*)entity;
        out->id = Read<unsigned long long>(entity, kEntityId);
        out->pos = Read<Vec3>(entity, kEntityPos);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GameTeleportPlayer(const Vec3& dest) {
    if (!g_base) return false;
    const BYTE* tail = g_base + kTeleportTail;
    if (tail[0] != 0x83 || tail[1] != 0x3D ||
        Read<DWORD>(tail, 2) != (DWORD)(g_base + kTeleportTailGlobal)) {
        Log("ERROR: teleport code not found at the expected address. Teleport refused.");
        return false;
    }
    PlayerState p;
    if (!GameReadPlayer(&p)) {
        Log("Teleport refused: no player");
        return false;
    }
    g_fc2ftTeleportDest = dest;
    g_fc2ftTeleportTail = (void*)tail;
    Log("Teleport: from x=%.2f y=%.2f z=%.2f to x=%.2f y=%.2f z=%.2f (thread %lu)", p.pos.x,
        p.pos.y, p.pos.z, dest.x, dest.y, dest.z, GetCurrentThreadId());
    if (!CallTeleportGuarded()) {
        Log("ERROR: the engine teleport call faulted");
        return false;
    }
    Log("Teleport: engine call returned");
    return true;
}

// The engine clock object and its "paused" byte. The frame update reads this
// byte to choose a reduced set of update steps; the game's menus set and
// clear it with plain byte writes.
const DWORD kClockPtr = 0x1555A60;
const DWORD kClockPaused = 0x48;

int GameIsPaused() {
    if (!g_base) return -1;
    __try {
        const BYTE* clock = Read<const BYTE*>(g_base, kClockPtr);
        if (!clock) return -1;
        return Read<BYTE>(clock, kClockPaused) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool GameSetPaused(bool paused) {
    if (!g_base) return false;
    __try {
        BYTE* clock = Read<BYTE*>(g_base, kClockPtr);
        if (!clock) return false;
        clock[kClockPaused] = paused ? 1 : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Pawn component type (id, lazy-init guard, init) and the engine function
// that returns a pawn's current vehicle component, or null (cdecl).
const DWORD kPawnTypeGuard = 0xF2DA88;
const DWORD kPawnTypeInit = 0x40AB0;
const DWORD kPawnTypeId = 0xF2DA9C;
const DWORD kPawnCurrentVehicle = 0xE6160;
typedef void*(__cdecl* PawnCurrentVehicleFn)(void* pawn);
typedef void(__fastcall* EntityFlushFn)(void* entity, void* edx);
typedef void*(__fastcall* EntityGetComponentFn)(void* entity, void* edx, void* typeId);
typedef void*(__cdecl* ComponentTypeInitFn)();

int GamePlayerInVehicle() {
    if (!g_base) return -1;
    PlayerState p;
    if (!GameReadPlayer(&p)) return -1;
    __try {
        // Expected entry of the lookup: sub esp,8 / mov ecx,[esp+0xC]
        if (Read<DWORD>(g_base, kPawnCurrentVehicle) != 0x8B08EC83) return -1;
        ((EntityFlushFn)(g_base + kEntityFlush))(p.entity, nullptr);
        if (Read<DWORD>(g_base, kPawnTypeGuard) == 0)
            ((ComponentTypeInitFn)(g_base + kPawnTypeInit))();
        void* pawn = ((EntityGetComponentFn)(g_base + kGetComponent))(p.entity, nullptr,
                                                                      g_base + kPawnTypeId);
        if (!pawn) return -1;
        return ((PawnCurrentVehicleFn)(g_base + kPawnCurrentVehicle))(pawn) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Squad commanders (CDispatcherSquadLieutenant), reached the same way the
// engine's own teleport step (0x109EBA30) reaches them:
//   AI manager pointer -> +0xC -> checked cast -> four group slots at +0xB0
//   -> controller at +0x490 -> must be a squad lieutenant
//   -> stand-down timestamp (float, game time) at +0x2F0.
const DWORD kAiManagerPtr = 0x15567FC;
const DWORD kAiCast = 0x8F8830;              // cdecl(object*) -> object* or null
const DWORD kAiGroups = 0xB0;
const DWORD kAiGroupController = 0x490;
const DWORD kLieutenantTypeGuard = 0x15A31F4;
const DWORD kLieutenantTypeInit = 0x8F7E40;
const DWORD kLieutenantTypeKey = 0x15A3210;
const DWORD kLieutenantStandDownTime = 0x2F0;
const DWORD kStandDownSeconds = 0xE3AA68;    // float constant, 15.0
const DWORD kClockTime = 0x30;               // double, game time in seconds
typedef void*(__cdecl* AiCastFn)(void* object);
typedef void*(__fastcall* TypeInfoFn)(void* self, void* edx);

int GameShortenAiStandDown(float graceSeconds) {
    if (!g_base) return -1;
    __try {
        // Expected entry of the cast: push esi / mov esi,[esp+8]
        if (Read<DWORD>(g_base, kAiCast) != 0x24748B56) return -1;
        float standDown = Read<float>(g_base, kStandDownSeconds);
        if (!(standDown > 1.0f && standDown < 600.0f)) return -1;
        const BYTE* clock = Read<const BYTE*>(g_base, kClockPtr);
        BYTE* mgr = Read<BYTE*>(g_base, kAiManagerPtr);
        if (!clock || !mgr) return -1;
        void* raw = Read<void*>(mgr, 0xC);
        BYTE* ai = raw ? (BYTE*)((AiCastFn)(g_base + kAiCast))(raw) : nullptr;
        if (!ai) return -1;
        if (Read<DWORD>(g_base, kLieutenantTypeGuard) == 0)
            ((ComponentTypeInitFn)(g_base + kLieutenantTypeInit))();
        DWORD key = Read<DWORD>(g_base, kLieutenantTypeKey);
        float now = (float)Read<double>(clock, kClockTime);
        float cut = standDown - graceSeconds;   // how far back to move the timestamp
        if (cut <= 0.0f) return 0;

        int count = 0;
        for (int i = 0; i < 4; ++i) {
            BYTE* group = Read<BYTE*>(ai, kAiGroups + 4 * i);
            if (!group) continue;
            BYTE* ctrl = Read<BYTE*>(group, kAiGroupController);
            if (!ctrl) continue;
            void** vtable = Read<void**>(ctrl);
            const BYTE* info = (const BYTE*)((TypeInfoFn)vtable[1])(ctrl, nullptr);
            if (!info || Read<DWORD>(info, 4) < 6 || Read<DWORD>(info, 0x1C) != key) continue;
            float ts = Read<float>(ctrl, kLieutenantStandDownTime);
            float age = now - ts;
            Log("AI: squad commander %d stood down %.1f s ago (game time %.1f)", i, age, now);
            if (ts != 0.0f && age >= 0.0f && age < cut) {
                *(float*)(ctrl + kLieutenantStandDownTime) = now - cut;
                ++count;
            }
        }
        return count;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Cinematic UI mode, read the way the script function SetCinematicUIMode
// (0x107178F0 -> 0x107176C0) writes it:
//   service list pointer -> lookup(type key, 1) -> byte at +0x158.
const DWORD kServiceListPtr = 0x1594164;
const DWORD kServiceLookup = 0x4CE460;        // thiscall(list, key*, int) -> object or null
const DWORD kGameFilesTypeGuard = 0x1599170;
const DWORD kGameFilesTypeInit = 0x64AC70;
const DWORD kGameFilesTypeKey = 0x1599184;
const DWORD kCinematicUiMode = 0x158;
typedef void*(__fastcall* ServiceLookupFn)(void* self, void* edx, void* key, int flag);

int GameInCinematic() {
    if (!g_base) return -1;
    __try {
        // Expected entries: cmp byte [ecx+0x9C],0 / cmp dword [guard],0
        if (Read<DWORD>(g_base, kServiceLookup) != 0x009CB980) return -1;
        if (Read<DWORD>(g_base, kGameFilesTypeInit) != 0x91703D83) return -1;
        void* list = Read<void*>(g_base, kServiceListPtr);
        if (!list) return -1;
        if (Read<DWORD>(g_base, kGameFilesTypeGuard) == 0)
            ((ComponentTypeInitFn)(g_base + kGameFilesTypeInit))();
        const BYTE* svc = (const BYTE*)((ServiceLookupFn)(g_base + kServiceLookup))(
            list, nullptr, g_base + kGameFilesTypeKey, 1);
        if (!svc) return -1;
        return Read<BYTE>(svc, kCinematicUiMode) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

const DWORD kSaveAllowed = 0x728470;   // thiscall(service) -> bool
typedef char(__fastcall* SaveAllowedFn)(void* self, void* edx);

int GameSaveAllowed() {
    if (!g_base) return -1;
    __try {
        if (Read<DWORD>(g_base, kServiceLookup) != 0x009CB980) return -1;
        if (Read<DWORD>(g_base, kGameFilesTypeInit) != 0x91703D83) return -1;
        // Expected entry: push ecx / mov eax,[...]
        if (Read<DWORD>(g_base, kSaveAllowed) != 0x4968A151) return -1;
        void* list = Read<void*>(g_base, kServiceListPtr);
        if (!list) return -1;
        if (Read<DWORD>(g_base, kGameFilesTypeGuard) == 0)
            ((ComponentTypeInitFn)(g_base + kGameFilesTypeInit))();
        void* svc = ((ServiceLookupFn)(g_base + kServiceLookup))(list, nullptr,
                                                                 g_base + kGameFilesTypeKey, 1);
        if (!svc) return -1;
        return ((SaveAllowedFn)(g_base + kSaveAllowed))(svc, nullptr) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void* GameHookImport(const char* dll, const char* function, void* hook) {
    if (!g_base) return nullptr;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)g_base;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(g_base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    const IMAGE_IMPORT_DESCRIPTOR* imp =
        (const IMAGE_IMPORT_DESCRIPTOR*)(g_base + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        if (lstrcmpiA((const char*)(g_base + imp->Name), dll) != 0) continue;
        if (!imp->OriginalFirstThunk) continue;
        const IMAGE_THUNK_DATA* names =
            (const IMAGE_THUNK_DATA*)(g_base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* slots = (IMAGE_THUNK_DATA*)(g_base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            const IMAGE_IMPORT_BY_NAME* n =
                (const IMAGE_IMPORT_BY_NAME*)(g_base + names->u1.AddressOfData);
            if (lstrcmpA((const char*)n->Name, function) != 0) continue;
            DWORD old;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old))
                return nullptr;
            void* orig = (void*)slots->u1.Function;
            slots->u1.Function = (DWORD)hook;
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            return orig;
        }
    }
    return nullptr;
}

// Terrain manager global, its height lookup (thiscall: x, y -> float), and
// the constant that lookup returns where it has no terrain sector.
const DWORD kTerrainManagerPtr = 0x1556F8C;
const DWORD kTerrainHeightAt = 0x328C60;
const DWORD kTerrainNoData = 0xD86A24;
typedef float(__fastcall* TerrainHeightFn)(void* self, void* edx, float x, float y);

bool GameTerrainHeight(float x, float y, float* z) {
    if (!g_base) return false;
    __try {
        void* mgr = Read<void*>(g_base, kTerrainManagerPtr);
        if (!mgr) return false;
        // Check the entry bytes (cvttss2si eax,[esp+8]) before calling.
        if (Read<DWORD>(g_base, kTerrainHeightAt) != 0x442C0FF3) return false;
        float h = ((TerrainHeightFn)(g_base + kTerrainHeightAt))(mgr, nullptr, x, y);
        if (h == Read<float>(g_base, kTerrainNoData)) return false;
        *z = h;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// CSafeHouseComponent: type id, its lazy-init guard and init function, the
// "locked" byte, and three entity handles it holds.
const DWORD kSafeHouseTypeGuard = 0x1598AAC;
const DWORD kSafeHouseTypeInit = 0x649F40;
const DWORD kSafeHouseTypeId = 0x1598ABC;
const DWORD kSafeHouseLocked = 0x11;
const DWORD kSafeHouseLinked[3] = {0x40, 0x44, 0x48};

int GameGetMapElements(MapElement* out, int max) {
    if (!g_base) return -1;
    __try {
        BYTE* mgr = Read<BYTE*>(g_base, kEntityManagerPtr);
        if (!mgr) return -1;
        BYTE* map = mgr + kEntityMapOffset;
        void* end = Read<void*>(map, kEntityMapEnd);
        if (!end) return -1;

        IterFn iterBegin = (IterFn)(g_base + kIterBegin);
        IterFn iterNext = (IterFn)(g_base + kIterNext);
        FlushFn flush = (FlushFn)(g_base + kEntityFlush);
        GetComponentFn getComponent = (GetComponentFn)(g_base + kGetComponent);
        if (Read<DWORD>(g_base, kMapElemTypeGuard) == 0)
            ((TypeInitFn)(g_base + kMapElemTypeInit))();
        if (Read<DWORD>(g_base, kSafeHouseTypeGuard) == 0)
            ((TypeInitFn)(g_base + kSafeHouseTypeInit))();

        EntityIter it;
        it.cur = Read<void*>(end);
        it.node = nullptr;
        it.map = map;
        iterBegin(&it, nullptr);

        int visited = 0, found = 0;
        while (it.cur != end && it.node && visited < 200000 && found < max) {
            ++visited;
            BYTE* handle = Read<BYTE*>(it.node, 0x10);
            BYTE* entity = handle ? Read<BYTE*>(handle, 0xC) : nullptr;
            if (entity) {
                flush(entity, nullptr);
                BYTE* comp = (BYTE*)getComponent(entity, nullptr, g_base + kMapElemTypeId);
                if (comp) {
                    MapElement& e = out[found++];
                    e.type = (int)Read<DWORD>(comp, kMapElemType);
                    e.discovered = Read<BYTE>(comp, kMapElemDiscovered) != 0;
                    e.state = Read<DWORD>(comp, kMapElemUnknown16C);
                    e.pos = Read<Vec3>(entity, kEntityPos);
                    e.locked = -1;
                    for (int k = 0; k < 3; ++k) e.haveLinked[k] = false;
                    BYTE* sh = e.type == kMapSafeHouse
                                   ? (BYTE*)getComponent(entity, nullptr, g_base + kSafeHouseTypeId)
                                   : nullptr;
                    if (sh) {
                        e.locked = Read<BYTE>(sh, kSafeHouseLocked) ? 1 : 0;
                        for (int k = 0; k < 3; ++k) {
                            BYTE* h = Read<BYTE*>(sh, kSafeHouseLinked[k]);
                            BYTE* other = h ? Read<BYTE*>(h, 0xC) : nullptr;
                            if (other) {
                                e.haveLinked[k] = true;
                                e.linked[k] = Read<Vec3>(other, kEntityPos);
                            }
                        }
                    }
                }
            }
            iterNext(&it, nullptr);
        }
        return found;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}
