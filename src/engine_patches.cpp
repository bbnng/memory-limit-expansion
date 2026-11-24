// engine.dll patches, applied the moment engine.dll is mapped (before it initializes).
//
// Hunk size: Sys_InitMemory (engine.dll 1.2, RVA 0xFA470) sizes the engine's hunk, the pool that holds
// the map and, in its free middle, the sound and model cache, as min(RAM / 2, 200 MB). A full hunk
// ends the game with "Hunk_Alloc: failed"; a small free middle makes the cache throw out sounds and
// models. [Engine] hunk_mb raises the 200 MB cap. -heapsize on the command line still wins.
#include "common.h"
#include <winternl.h>

namespace memx {

static const DWORD kCapRva = 0xFA4C2;
// cmp eax,0C800000h / jle +0A / mov [20B42BC8],0C800000h   (the absolute address assumes base 20000000h)
static const BYTE kCapCode[] = {0x3D, 0x00, 0x00, 0x80, 0x0C, 0x7E, 0x0A, 0xC7, 0x05, 0xC8, 0x2B, 0xB4, 0x20, 0x00, 0x00, 0x80, 0x0C};

// Called for every load of engine.dll (an engine restart maps it again); the bytes decide.
static void patch_engine(HMODULE engine) {
    int mb = g_settings.hunk_mb;
    if (mb <= 0 || !engine) return;
    if (mb < 200) mb = 200;
    if (mb > 1024) mb = 1024;
    if (engine != (HMODULE)0x20000000 || !in_module_image(engine, kCapRva, sizeof(kCapCode))) {
        log("engine: hunk size not changed: engine.dll is not the analysed 1.2 build (or loaded elsewhere)");
        return;
    }
    BYTE *code = (BYTE *)engine + kCapRva;
    DWORD cap = (DWORD)mb << 20;
    if (code[0] == 0x3D && memcmp(code + 1, &cap, 4) == 0 && memcmp(code + 5, kCapCode + 5, 8) == 0 &&
        memcmp(code + 13, &cap, 4) == 0)
        return;   // already done for this mapping
    if (memcmp(code, kCapCode, sizeof(kCapCode)) != 0) {
        log("engine: hunk size not changed: engine.dll is not the analysed 1.2 build (or loaded elsewhere)");
        return;
    }
    DWORD prot;
    if (!VirtualProtect(code, sizeof(kCapCode), PAGE_EXECUTE_READWRITE, &prot)) {
        log("engine: hunk size not changed: VirtualProtect failed %lu", GetLastError());
        return;
    }
    memcpy(code + 1, &cap, 4);    // cmp eax, cap
    memcpy(code + 13, &cap, 4);   // mov [hunk size], cap
    VirtualProtect(code, sizeof(kCapCode), prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), code, sizeof(kCapCode));
    log("engine: hunk cap raised from 200 MB to %d MB (used when the PC has at least twice that much RAM)", mb);
}

static void engine_mapped(HMODULE engine) {
    patch_engine(engine);
    on_engine_mapped(engine);
}

// ---- DLL load notifications (ntdll, Vista and later) ----
struct LDR_DLL_NOTIFICATION_DATA_ {
    ULONG Flags;
    const UNICODE_STRING *FullDllName;
    const UNICODE_STRING *BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
};
typedef VOID(CALLBACK *LdrNotify_t)(ULONG reason, const LDR_DLL_NOTIFICATION_DATA_ *data, PVOID ctx);
typedef NTSTATUS(NTAPI *LdrRegister_t)(ULONG flags, LdrNotify_t fn, PVOID ctx, PVOID *cookie);

static VOID CALLBACK on_dll(ULONG reason, const LDR_DLL_NOTIFICATION_DATA_ *data, PVOID) {
    if (reason != 1 || !data || !data->BaseDllName || !data->BaseDllName->Buffer) return;   // 1 = loaded
    const UNICODE_STRING *n = data->BaseDllName;
    if (n->Length == 10 * sizeof(wchar_t) && _wcsnicmp(n->Buffer, L"engine.dll", 10) == 0) engine_mapped((HMODULE)data->DllBase);
}

// The Unofficial Patch's loader maps engine.dll with LoadLibraryExA(DONT_RESOLVE_DLL_REFERENCES), which
// sends no DLL notification, so the LoadLibrary functions are watched too.
static decltype(&LoadLibraryExA) g_LoadLibraryExA;
static decltype(&LoadLibraryExW) g_LoadLibraryExW;
static decltype(&LoadLibraryA) g_LoadLibraryA;
static decltype(&LoadLibraryW) g_LoadLibraryW;

static const DWORD kDatafileFlags = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE;

static bool maybe_engine_name_a(LPCSTR n) {
    if (!n) return false;
    const char *base = n;
    for (const char *p = n; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    return _stricmp(base, "engine.dll") == 0 || _stricmp(base, "engine") == 0;
}
static bool maybe_engine_name_w(LPCWSTR n) {
    if (!n) return false;
    const wchar_t *base = n;
    for (const wchar_t *p = n; *p; ++p)
        if (*p == L'\\' || *p == L'/') base = p + 1;
    return _wcsicmp(base, L"engine.dll") == 0 || _wcsicmp(base, L"engine") == 0;
}
static void check_loaded(HMODULE m) {
    if (!m || ((uintptr_t)m & 3) != 0) return;
    DWORD err = GetLastError();
    wchar_t name[MAX_PATH];
    DWORD len = GetModuleFileNameW(m, name, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        const wchar_t *base = wcsrchr(name, L'\\');
        if (!_wcsicmp(base ? base + 1 : name, L"engine.dll")) engine_mapped(m);
    }
    SetLastError(err);
}
static HMODULE WINAPI my_LoadLibraryExA(LPCSTR n, HANDLE f, DWORD fl) {
    HMODULE m = g_LoadLibraryExA(n, f, fl);
    if (m && !(fl & kDatafileFlags) && maybe_engine_name_a(n)) check_loaded(m);
    return m;
}
static HMODULE WINAPI my_LoadLibraryExW(LPCWSTR n, HANDLE f, DWORD fl) {
    HMODULE m = g_LoadLibraryExW(n, f, fl);
    if (m && !(fl & kDatafileFlags) && maybe_engine_name_w(n)) check_loaded(m);
    return m;
}
static HMODULE WINAPI my_LoadLibraryA(LPCSTR n) {
    HMODULE m = g_LoadLibraryA(n);
    if (m && maybe_engine_name_a(n)) check_loaded(m);
    return m;
}
static HMODULE WINAPI my_LoadLibraryW(LPCWSTR n) {
    HMODULE m = g_LoadLibraryW(n);
    if (m && maybe_engine_name_w(n)) check_loaded(m);
    return m;
}

int redirect_imports(const char *const *names, void **originals, void *const *replacements, int n);   // alloc_trace.cpp

void install_engine_patches() {
    if (g_settings.hunk_mb <= 0 && !server_mode() && !g_settings.console_log && !g_settings.split) return;
    if (HMODULE engine = GetModuleHandleW(L"engine.dll")) {
        log("engine: engine.dll was loaded before Memory Expansion; the hunk may already be allocated");
        engine_mapped(engine);
    }
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    g_LoadLibraryExA = (decltype(g_LoadLibraryExA))GetProcAddress(k32, "LoadLibraryExA");
    g_LoadLibraryExW = (decltype(g_LoadLibraryExW))GetProcAddress(k32, "LoadLibraryExW");
    g_LoadLibraryA = (decltype(g_LoadLibraryA))GetProcAddress(k32, "LoadLibraryA");
    g_LoadLibraryW = (decltype(g_LoadLibraryW))GetProcAddress(k32, "LoadLibraryW");
    int slots = 0;
    if (g_LoadLibraryExA && g_LoadLibraryExW && g_LoadLibraryA && g_LoadLibraryW) {
        const char *names[] = {"LoadLibraryExA", "LoadLibraryExW", "LoadLibraryA", "LoadLibraryW"};
        void *originals[] = {(void *)g_LoadLibraryExA, (void *)g_LoadLibraryExW, (void *)g_LoadLibraryA, (void *)g_LoadLibraryW};
        void *repl[] = {(void *)my_LoadLibraryExA, (void *)my_LoadLibraryExW, (void *)my_LoadLibraryA, (void *)my_LoadLibraryW};
        slots = redirect_imports(names, originals, repl, 4);
    }
    auto reg = (LdrRegister_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    PVOID cookie = nullptr;
    if (reg) reg(0, on_dll, nullptr, &cookie);
    if (g_settings.verbose) log("engine: watching for engine.dll (%d import slots)", slots);
}

} // namespace memx
