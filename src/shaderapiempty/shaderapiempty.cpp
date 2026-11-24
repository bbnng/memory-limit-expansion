// shaderapiempty.dll for the Memory Expansion server process (Vampire.exe -memx_server).
//
// The engine's dedicated-server path initializes the material system with "shaderapiempty", a shader
// API that draws nothing; Bloodlines never shipped one. This one answers the three interfaces the
// material system asks for:
//   ShaderApi027                     every method does nothing and returns 0 (slot table generated from
//                                    shaderapidx9.dll, so each method pops the right argument bytes);
//                                    the few that shaderapidx9 itself forwards to the material system
//                                    are forwarded the same way
//   MaterialSystemHardwareConfig010  shaderapidx9's own, after its Init (which reads the graphics card's
//                                    capabilities but creates no device), so materials pick the same
//                                    shader fallbacks on the server as in the game
//   ShaderShadow009                  does nothing
// No Direct3D device, no textures and no vertex buffers exist in the server process.
#include <windows.h>

typedef void *(__cdecl *CreateInterfaceFn)(const char *name, int *ret);

static void *g_util;   // the material system's IShaderUtil (Init argument 1)
static void *g_real_api, *g_real_hw;
static HMODULE g_real;

static double __cdecl plat_float_time_impl() {
    typedef double(__cdecl * Fn)();
    static Fn tier0 = []() -> Fn {
        HMODULE t0 = GetModuleHandleA("tier0.dll");
        return t0 ? (Fn)GetProcAddress(t0, "Plat_FloatTime") : nullptr;
    }();
    if (tier0) return tier0();
    static LARGE_INTEGER freq, start;
    LARGE_INTEGER now;
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start); }
    QueryPerformanceCounter(&now);
    return freq.QuadPart ? (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart : 0.0;
}
// slot 0 is shaderapidx9's tail jump to tier0's Plat_FloatTime: no arguments, double in st(0)
static __declspec(naked) void plat_float_time() {
    __asm { jmp plat_float_time_impl }
}

static bool load_real() {
    if (g_real) return g_real_api && g_real_hw;
    char path[MAX_PATH + 32];
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&load_real, &self) || !self)
        return false;
    DWORD len = GetModuleFileNameA(self, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    char *slash = strrchr(path, '\\');
    char *dst = slash ? slash + 1 : path;
    strcpy_s(dst, sizeof(path) - (size_t)(dst - path), "shaderapidx9.dll");
    g_real = LoadLibraryA(path);
    if (!g_real) return false;
    auto ci = (CreateInterfaceFn)GetProcAddress(g_real, "CreateInterface");
    if (!ci) return false;
    g_real_api = ci("ShaderApi027", nullptr);
    g_real_hw = ci("MaterialSystemHardwareConfig010", nullptr);
    return g_real_api && g_real_hw;
}

// IShaderAPI::Init(util, ?, adapter, flags, factory), thiscall: called as __fastcall with ecx = this
typedef bool(__fastcall *Init_t)(void *self, void *edx, void *util, int a3, int adapter, int flags, void *factory);
static bool __fastcall api_init(void *, void *, void *util, int a3, int adapter, int flags, void *factory) {
    g_util = util;
    if (!load_real()) return false;
    // shaderapidx9's Init only creates IDirect3D9 and reads the adapter's capabilities
    Init_t real_init = (Init_t)(*(void ***)g_real_api)[59];
    return real_init(g_real_api, nullptr, util, a3, adapter, flags, factory);
}

#include "stubs.inc"

struct Obj { void *const *vt; };
static Obj g_api = {g_api_vt}, g_shadow = {g_shadow_vt}, g_debug = {g_debug_vt};

extern "C" __declspec(dllexport) void *__cdecl CreateInterface(const char *name, int *ret) {
    void *r = nullptr;
    if (name) {
        if (!strcmp(name, "ShaderApi027")) r = &g_api;
        else if (!strcmp(name, "ShaderShadow009")) r = &g_shadow;
        else if (!strcmp(name, "DebugTextureInfo001")) r = &g_debug;
        else if (!strcmp(name, "MaterialSystemHardwareConfig010")) r = load_real() ? g_real_hw : nullptr;
    }
    if (ret) *ret = r ? 0 : 1;
    return r;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void *) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}
