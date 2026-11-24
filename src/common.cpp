#include "common.h"
#include <cstdarg>

namespace memx {

Settings g_settings;
wchar_t g_dir[MAX_PATH];

static CRITICAL_SECTION g_log_lock;
static HANDLE g_log_file = INVALID_HANDLE_VALUE;

static void log_open() {
    InitializeCriticalSection(&g_log_lock);
    wchar_t path[MAX_PATH + 64];
    // the server process (process split) runs from the same folder and keeps its own log
    _snwprintf_s(path, _countof(path), _TRUNCATE, L"%s%s", g_dir, server_mode() ? L"memory_expansion_server.log" : L"memory_expansion.log");
    g_log_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void log(const char *fmt, ...) {
    if (g_log_file == INVALID_HANDLE_VALUE) return;
    char buf[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = sprintf_s(buf, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    buf[n] = '\0';
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    if (m < 0) m = (int)strlen(buf + n);
    n += m;
    if (n > (int)sizeof(buf) - 3) n = (int)sizeof(buf) - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    EnterCriticalSection(&g_log_lock);
    DWORD written;
    WriteFile(g_log_file, buf, n, &written, nullptr);
    LeaveCriticalSection(&g_log_lock);
}

static int ini_int(const wchar_t *ini, const wchar_t *section, const wchar_t *key, int def) {
    return (int)GetPrivateProfileIntW(section, key, def, ini);
}

void load_settings() {
    wchar_t ini[MAX_PATH + 64];
    _snwprintf_s(ini, _countof(ini), _TRUNCATE, L"%smemory_expansion.ini", g_dir);
    Settings &s = g_settings;
    s.d3d9ex = ini_int(ini, L"D3D9", L"d3d9ex", 1) != 0;
    s.offload_textures = s.d3d9ex;
    s.offload_buffers = ini_int(ini, L"D3D9", L"offload_buffers", 1) != 0;
    s.allow_wrapper = ini_int(ini, L"D3D9", L"allow_d3d9_wrapper", 0) != 0;
    s.staging_cache_mb = ini_int(ini, L"D3D9", L"staging_cache_mb", 16);
    if (s.staging_cache_mb < 0) s.staging_cache_mb = 0;
    if (s.staging_cache_mb > 512) s.staging_cache_mb = 512;
    s.stats_seconds = ini_int(ini, L"Diagnostics", L"stats_seconds", 60);
    if (s.stats_seconds < 0) s.stats_seconds = 0;
    if (s.stats_seconds > 86400) s.stats_seconds = 86400;
    s.verbose = ini_int(ini, L"Diagnostics", L"verbose", 0) != 0;
    s.trace_allocs_mb = ini_int(ini, L"Diagnostics", L"trace_allocs_mb", 0);
    if (s.trace_allocs_mb < 0) s.trace_allocs_mb = 0;
    if (s.trace_allocs_mb > 4095) s.trace_allocs_mb = 4095;
    s.console_log = ini_int(ini, L"Diagnostics", L"console_log", 0) != 0;
    s.keep_system_copy = ini_int(ini, L"Diagnostics", L"keep_system_copy", 0) != 0;
    s.hunk_mb = ini_int(ini, L"Engine", L"hunk_mb", 0);
    s.split = ini_int(ini, L"Split", L"enabled", 0) != 0;
    s.split_port = ini_int(ini, L"Split", L"port", 27115);
    if (s.split_port < 1024 || s.split_port > 65480) s.split_port = 27115;
    s.split_server_fps = ini_int(ini, L"Split", L"server_fps", 100);
    if (s.split_server_fps < 30) s.split_server_fps = 30;
    if (s.split_server_fps > 1000) s.split_server_fps = 1000;
    if (!s.d3d9ex) s.offload_textures = s.offload_buffers = false;
}

VaStats va_stats() {
    VaStats r = {};
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0;
    while (VirtualQuery((void *)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uint64_t size = mbi.RegionSize;
        if (mbi.State == MEM_FREE) {
            r.free_total += size;
            if (size > r.largest_free) r.largest_free = size;
        } else {
            r.used += size;
            if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE) r.private_commit += size;
        }
        uintptr_t next = (uintptr_t)mbi.BaseAddress + (uintptr_t)mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
    }
    return r;
}

void *patch_vtable(void **vt, int index, void *fn) {
    if (!vt || index < 0) return nullptr;
    void *old = vt[index];
    if (old == fn) return old;
    DWORD prot;
    if (!VirtualProtect(&vt[index], sizeof(void *), PAGE_READWRITE, &prot)) return nullptr;
    old = InterlockedExchangePointer(&vt[index], fn);
    VirtualProtect(&vt[index], sizeof(void *), prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), &vt[index], sizeof(void *));
    return old;
}

bool in_module_image(HMODULE mod, DWORD rva, DWORD len) {
    if (!mod) return false;
    __try {
        auto dos = (const IMAGE_DOS_HEADER *)mod;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        if (dos->e_lfanew < 0 || dos->e_lfanew > 0x100000) return false;
        auto nt = (const IMAGE_NT_HEADERS *)((const BYTE *)mod + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        DWORD img_size = nt->OptionalHeader.SizeOfImage;
        return rva <= img_size && len <= img_size - rva;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void install_d3d9();   // d3d9_offload.cpp
void install_alloc_trace(int threshold_mb);   // alloc_trace.cpp
void install_engine_patches();   // engine_patches.cpp
void start_stats();    // d3d9_offload.cpp

static void startup(HMODULE self) {
    DWORD len = GetModuleFileNameW(self, g_dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        g_dir[0] = 0;
    } else {
        wchar_t *slash = wcsrchr(g_dir, L'\\');
        if (slash) slash[1] = 0;
        else g_dir[0] = 0;
    }
    log_open();
    load_settings();
    VaStats va = va_stats();
    log("Memory Expansion 1.1 loaded; address space: %llu MB used, %llu MB free (largest block %llu MB)",
        va.used >> 20, va.free_total >> 20, va.largest_free >> 20);
    log("already loaded: engine.dll %p, shaderapidx9.dll %p, tier0.dll %p, launcher.dll %p", GetModuleHandleW(L"engine.dll"),
        GetModuleHandleW(L"shaderapidx9.dll"), GetModuleHandleW(L"tier0.dll"), GetModuleHandleW(L"launcher.dll"));
    MEMORYSTATUSEX ms = {sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    log("system: %llu MB RAM, this process can address %llu MB", ms.ullTotalPhys >> 20, ms.ullTotalVirtual >> 20);
    if (ms.ullTotalVirtual < (3ull << 30))
        log("warning: Vampire.exe is not large-address-aware, so the game has only 2 GB of address space. The "
            "Unofficial Patch's Vampire.exe is; use it (or set the flag) to get 4 GB.");
    log("settings: d3d9ex=%d offload_textures=%d offload_buffers=%d allow_d3d9_wrapper=%d staging_cache_mb=%d",
        g_settings.d3d9ex, g_settings.offload_textures, g_settings.offload_buffers, g_settings.allow_wrapper,
        g_settings.staging_cache_mb);
    install_alloc_trace(g_settings.trace_allocs_mb);
    install_engine_patches();
    install_d3d9();
    start_stats();
}

} // namespace memx

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void *) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        memx::startup(module);
    }
    return TRUE;
}
