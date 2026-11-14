#include "input.h"
#include "log.h"

// Only the COM vtable layouts of IDirectInput8 and IDirectInputDevice8 are
// used here, so dinput.h and dxguid.lib are not needed.

namespace {

const GUID kSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID kSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

// Vtable slots.
const int kDI_CreateDevice = 3;
const int kDev_GetDeviceState = 9;
const int kDev_GetDeviceData = 10;

// Standard mouse data format (c_dfDIMouse / c_dfDIMouse2).
const DWORD kMouseX = 0, kMouseY = 4, kMouseButton0 = 12, kMouseButtonsEnd = 20;

// DIDEVICEOBJECTDATA for DirectInput 8.
struct ObjectData {
    DWORD dwOfs;
    DWORD dwData;
    DWORD dwTimeStamp;
    DWORD dwSequence;
    UINT_PTR uAppData;
};

typedef HRESULT(__stdcall* CreateDevice_t)(void* self, const GUID& guid, void** out, void* outer);
typedef HRESULT(__stdcall* GetDeviceState_t)(void* self, DWORD cb, void* data);
typedef HRESULT(__stdcall* GetDeviceData_t)(void* self, DWORD cbObj, ObjectData* data, DWORD* inOut,
                                            DWORD flags);

CreateDevice_t g_realCreateDevice = nullptr;
GetDeviceState_t g_realGetDeviceState = nullptr;
GetDeviceData_t g_realGetDeviceData = nullptr;

void* g_mouse = nullptr;
void* g_keyboard = nullptr;

volatile LONG g_capture = 0;
volatile LONG g_dx = 0, g_dy = 0, g_clicks = 0;
bool g_leftWasDown = false;

void* PatchVtable(void* object, int slot, void* hook) {
    void** vtable = *(void***)object;
    if (vtable[slot] == hook) return nullptr;  // already patched
    DWORD old;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
    void* orig = vtable[slot];
    vtable[slot] = hook;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
    return orig;
}

HRESULT __stdcall Hook_GetDeviceState(void* self, DWORD cb, void* data) {
    HRESULT hr = g_realGetDeviceState(self, cb, data);
    if (FAILED(hr) || !data || !g_capture) return hr;
    if (self == g_mouse && cb >= 16) {
        const BYTE* b = (const BYTE*)data;
        InterlockedExchangeAdd(&g_dx, *(const LONG*)(b + kMouseX));
        InterlockedExchangeAdd(&g_dy, *(const LONG*)(b + kMouseY));
        bool left = (b[kMouseButton0] & 0x80) != 0;
        if (left && !g_leftWasDown) InterlockedIncrement(&g_clicks);
        g_leftWasDown = left;
        ZeroMemory(data, cb);
    } else if (self == g_keyboard) {
        ZeroMemory(data, cb);
    }
    return hr;
}

HRESULT __stdcall Hook_GetDeviceData(void* self, DWORD cbObj, ObjectData* data, DWORD* inOut,
                                     DWORD flags) {
    HRESULT hr = g_realGetDeviceData(self, cbObj, data, inOut, flags);
    if (FAILED(hr) || !data || !inOut || !g_capture) return hr;
    if (self != g_mouse && self != g_keyboard) return hr;
    if (cbObj < sizeof(ObjectData) - sizeof(UINT_PTR)) return hr;

    DWORD kept = 0;
    for (DWORD i = 0; i < *inOut; ++i) {
        ObjectData* e = (ObjectData*)((BYTE*)data + i * cbObj);
        bool keep = false;
        if (self == g_mouse) {
            if (e->dwOfs == kMouseX) {
                InterlockedExchangeAdd(&g_dx, (LONG)e->dwData);
            } else if (e->dwOfs == kMouseY) {
                InterlockedExchangeAdd(&g_dy, (LONG)e->dwData);
            } else if (e->dwOfs >= kMouseButton0 && e->dwOfs < kMouseButtonsEnd) {
                bool down = (e->dwData & 0x80) != 0;
                if (e->dwOfs == kMouseButton0) {
                    if (down && !g_leftWasDown) InterlockedIncrement(&g_clicks);
                    g_leftWasDown = down;
                }
                keep = !down;  // let button releases through
            }
        } else {
            keep = (e->dwData & 0x80) == 0;  // let key releases through
        }
        if (keep) {
            if (kept != i) memmove((BYTE*)data + kept * cbObj, e, cbObj);
            ++kept;
        }
    }
    *inOut = kept;
    return hr;
}

HRESULT __stdcall Hook_CreateDevice(void* self, const GUID& guid, void** out, void* outer) {
    HRESULT hr = g_realCreateDevice(self, guid, out, outer);
    if (FAILED(hr) || !out || !*out) return hr;
    const char* kind = nullptr;
    if (IsEqualGUID(guid, kSysMouse)) {
        g_mouse = *out;
        kind = "mouse";
    } else if (IsEqualGUID(guid, kSysKeyboard)) {
        g_keyboard = *out;
        kind = "keyboard";
    }
    if (!kind) {
        Log("DirectInput: other device created (not intercepted)");
        return hr;
    }
    void* a = PatchVtable(*out, kDev_GetDeviceState, (void*)Hook_GetDeviceState);
    if (a) g_realGetDeviceState = (GetDeviceState_t)a;
    void* b = PatchVtable(*out, kDev_GetDeviceData, (void*)Hook_GetDeviceData);
    if (b) g_realGetDeviceData = (GetDeviceData_t)b;
    Log("DirectInput: %s device created at 0x%p, reads %s", kind, *out,
        (g_realGetDeviceState && g_realGetDeviceData) ? "intercepted" : "NOT intercepted");
    return hr;
}

}  // namespace

void InputInstall(void* directInput8) {
    if (!directInput8 || g_realCreateDevice) return;
    void* orig = PatchVtable(directInput8, kDI_CreateDevice, (void*)Hook_CreateDevice);
    if (orig) g_realCreateDevice = (CreateDevice_t)orig;
    Log(orig ? "DirectInput: device creation intercepted"
             : "ERROR: could not intercept DirectInput device creation");
}

void InputSetCapture(bool on) {
    if (on) {
        InterlockedExchange(&g_dx, 0);
        InterlockedExchange(&g_dy, 0);
        InterlockedExchange(&g_clicks, 0);
    }
    InterlockedExchange(&g_capture, on ? 1 : 0);
}

void InputTakeMouse(int* dx, int* dy, int* leftClicks) {
    *dx = (int)InterlockedExchange(&g_dx, 0);
    *dy = (int)InterlockedExchange(&g_dy, 0);
    *leftClicks = (int)InterlockedExchange(&g_clicks, 0);
}
