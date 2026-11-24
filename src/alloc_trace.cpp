// Diagnostic: logs every large allocation (VirtualAlloc, HeapAlloc, MapViewOfFile) with the code that
// asked for it, so the big users of the 32-bit address space can be found. Off unless
// [Diagnostics] trace_allocs_mb is set. Import slots of loaded modules and kernel32's export table
// are redirected; nothing is changed in the allocation itself.
#include "common.h"
#include <psapi.h>
#include <intrin.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace memx {

static SIZE_T g_threshold;
static decltype(&VirtualAlloc) g_VirtualAlloc;
static decltype(&HeapAlloc) g_HeapAlloc;
static decltype(&HeapReAlloc) g_HeapReAlloc;
static decltype(&MapViewOfFile) g_MapViewOfFile;
static decltype(&MapViewOfFileEx) g_MapViewOfFileEx;
static __declspec(thread) bool g_in_trace;

void describe_address(void *addr, char *out, size_t n) {
    if (!out || n == 0) return;
    HMODULE mod = nullptr;
    char name[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod) && mod &&
        GetModuleFileNameA(mod, name, MAX_PATH)) {
        const char *base = strrchr(name, '\\');
        snprintf(out, n, "%s+%x", base ? base + 1 : name, (unsigned)((uintptr_t)addr - (uintptr_t)mod));
    } else {
        snprintf(out, n, "%p", addr);
    }
}

// Words on the stack that point just after a call instruction in a loaded module other than this one
// (likely return addresses): callers that a frame-pointer walk misses.
int scan_stack(void **sp, char *out, size_t n, int max) {
    if (!out || n == 0) return 0;
    size_t len = 0;
    int found = 0;
    out[0] = 0;
    for (int i = 0; i < 1024 && found < max && len + 1 < n; ++i) {
        BYTE *p;
        __try { p = (BYTE *)sp[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if ((uintptr_t)p < 0x10007) continue;
        HMODULE m = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p, &m) || !m) continue;
        if (m == (HMODULE)&__ImageBase) continue;
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p - 7, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) continue;
        bool call = false;
        __try {
            call = p[-5] == 0xE8 || (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) || (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) ||
                   (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) || (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (!call) continue;
        char one[96];
        describe_address(p, one, sizeof(one));
        int w = snprintf(out + len, n - len, found ? " %s" : "%s", one);
        if (w < 0 || (size_t)w >= n - len) { len = n - 1; break; }
        len += (size_t)w;
        ++found;
    }
    return found;
}

static void trace(const char *what, SIZE_T size, void *result) {
    if (g_in_trace) return;   // no nesting on this thread (logging allocates)
    DWORD err = GetLastError();
    g_in_trace = true;
    void *frames[12];
    USHORT count = CaptureStackBackTrace(2, 12, frames, nullptr);
    char stack[700] = {0};
    size_t len = 0;
    for (USHORT i = 0; i < count && len + 1 < sizeof(stack); ++i) {
        char one[96];
        describe_address(frames[i], one, sizeof(one));
        int w = snprintf(stack + len, sizeof(stack) - len, i ? " < %s" : "%s", one);
        if (w < 0 || (size_t)w >= sizeof(stack) - len) break;
        len += (size_t)w;
    }
    // Code compiled without frame pointers hides callers from the walk above; also list likely return addresses.
    char scan[700] = {0};
    scan_stack((void **)_AddressOfReturnAddress(), scan, sizeof(scan), 10);
    log("alloc: %s %lu KB -> %p from %s | stack: %s", what, (unsigned long)(size >> 10), result, stack, scan);
    g_in_trace = false;
    SetLastError(err);
}

static LPVOID WINAPI my_VirtualAlloc(LPVOID a, SIZE_T size, DWORD type, DWORD prot) {
    LPVOID r = g_VirtualAlloc(a, size, type, prot);
    if (size >= g_threshold) trace((type & MEM_RESERVE) ? (type & MEM_COMMIT ? "VirtualAlloc reserve+commit" : "VirtualAlloc reserve") : "VirtualAlloc commit", size, r);
    return r;
}
static LPVOID WINAPI my_HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size) {
    LPVOID r = g_HeapAlloc(heap, flags, size);
    if (size >= g_threshold) trace("HeapAlloc", size, r);
    return r;
}
static LPVOID WINAPI my_HeapReAlloc(HANDLE heap, DWORD flags, LPVOID p, SIZE_T size) {
    LPVOID r = g_HeapReAlloc(heap, flags, p, size);
    if (size >= g_threshold) trace("HeapReAlloc", size, r);
    return r;
}
static LPVOID WINAPI my_MapViewOfFile(HANDLE m, DWORD access, DWORD hi, DWORD lo, SIZE_T size) {
    LPVOID r = g_MapViewOfFile(m, access, hi, lo, size);
    if (size >= g_threshold || size == 0) {
        DWORD err = GetLastError();
        MEMORY_BASIC_INFORMATION mbi;
        SIZE_T real = r && VirtualQuery(r, &mbi, sizeof(mbi)) ? mbi.RegionSize : size;
        SetLastError(err);
        if (real >= g_threshold) trace("MapViewOfFile", real, r);
    }
    return r;
}
static LPVOID WINAPI my_MapViewOfFileEx(HANDLE m, DWORD access, DWORD hi, DWORD lo, SIZE_T size, LPVOID at) {
    LPVOID r = g_MapViewOfFileEx(m, access, hi, lo, size, at);
    if (size >= g_threshold || size == 0) {
        DWORD err = GetLastError();
        MEMORY_BASIC_INFORMATION mbi;
        SIZE_T real = r && VirtualQuery(r, &mbi, sizeof(mbi)) ? mbi.RegionSize : size;
        SetLastError(err);
        if (real >= g_threshold) trace("MapViewOfFileEx", real, r);
    }
    return r;
}

struct Redirect { const char *name; void *fn; void **orig; };

static int patch_module_imports(HMODULE mod, Redirect *list, int n) {
    int count = 0;
    __try {
        BYTE *base = (BYTE *)mod;
        auto dos = (IMAGE_DOS_HEADER *)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        if (dos->e_lfanew < 0 || dos->e_lfanew > 0x100000) return 0;
        auto nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        DWORD img_size = nt->OptionalHeader.SizeOfImage;
        IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress || dir.VirtualAddress >= img_size) return 0;
        for (auto imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
             (DWORD)((BYTE *)imp - base) + sizeof(*imp) <= img_size && imp->Name; ++imp) {
            if (!imp->FirstThunk || imp->FirstThunk >= img_size) continue;
            for (auto th = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
                 (DWORD)((BYTE *)th - base) + sizeof(*th) <= img_size && th->u1.Function; ++th) {
                for (int i = 0; i < n; ++i) {
                    if (!*list[i].orig || (void *)th->u1.Function != *list[i].orig) continue;
                    DWORD prot;
                    if (VirtualProtect(&th->u1.Function, 4, PAGE_READWRITE, &prot)) {
                        InterlockedExchange((volatile LONG *)&th->u1.Function, (LONG)(uintptr_t)list[i].fn);
                        VirtualProtect(&th->u1.Function, 4, prot, &prot);
                        ++count;
                    }
                    break;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return count;
}

static void patch_export_rva(HMODULE mod, const char *name, void *fn) {
    if (!mod || !name || !fn) return;
    __try {
        BYTE *base = (BYTE *)mod;
        auto dos = (IMAGE_DOS_HEADER *)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        if (dos->e_lfanew < 0 || dos->e_lfanew > 0x100000) return;
        auto nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;
        DWORD img_size = nt->OptionalHeader.SizeOfImage;
        IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dir.VirtualAddress || !dir.Size || dir.VirtualAddress >= img_size) return;
        auto exp = (IMAGE_EXPORT_DIRECTORY *)(base + dir.VirtualAddress);
        if (exp->AddressOfFunctions >= img_size || exp->AddressOfNames >= img_size || exp->AddressOfNameOrdinals >= img_size) return;
        DWORD *funcs = (DWORD *)(base + exp->AddressOfFunctions);
        DWORD *names = (DWORD *)(base + exp->AddressOfNames);
        WORD *ords = (WORD *)(base + exp->AddressOfNameOrdinals);
        for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
            if (names[i] >= img_size || ords[i] >= exp->NumberOfFunctions) continue;
            if (strcmp((char *)(base + names[i]), name) != 0) continue;
            DWORD rva = (DWORD)((uintptr_t)fn - (uintptr_t)base);
            if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) return;
            DWORD prot;
            if (VirtualProtect(&funcs[ords[i]], 4, PAGE_READWRITE, &prot)) {
                InterlockedExchange((volatile LONG *)&funcs[ords[i]], (LONG)rva);
                VirtualProtect(&funcs[ords[i]], 4, prot, &prot);
            }
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static BOOL enum_process_modules(HMODULE *mods, DWORD cb, DWORD *needed) {
    typedef BOOL(WINAPI *EnumMods_t)(HANDLE, HMODULE *, DWORD, LPDWORD);
    static EnumMods_t fn = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll"))
            fn = (EnumMods_t)GetProcAddress(k32, "K32EnumProcessModules");
        if (!fn) {
            if (HMODULE ps = LoadLibraryW(L"psapi.dll"))
                fn = (EnumMods_t)GetProcAddress(ps, "EnumProcessModules");
        }
    }
    return fn ? fn(GetCurrentProcess(), mods, cb, needed) : FALSE;
}

// Redirects functions in every loaded module's import table (except the system's own allocator and
// loader DLLs) and in kernel32's and kernelbase's export tables, so modules loaded later get them too.
int redirect_imports(const char *const *names, void **originals, void *const *replacements, int n) {
    Redirect list[32];
    void *kb_orig[16] = {};
    if (n > 16) n = 16;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll"), kb = GetModuleHandleW(L"kernelbase.dll");
    int count = 0;
    for (int i = 0; i < n; ++i) {
        list[count++] = {names[i], replacements[i], &originals[i]};
        if (kb) {
            kb_orig[i] = (void *)GetProcAddress(kb, names[i]);
            if (kb_orig[i] && kb_orig[i] != originals[i]) list[count++] = {names[i], replacements[i], &kb_orig[i]};
        }
    }
    HMODULE mods[512];
    DWORD needed = 0;
    int patched = 0;
    if (enum_process_modules(mods, sizeof(mods), &needed))
        for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 512; ++i) {
            if (mods[i] == (HMODULE)&__ImageBase) continue;
            wchar_t name[MAX_PATH];
            if (!GetModuleFileNameW(mods[i], name, MAX_PATH)) continue;
            const wchar_t *base = wcsrchr(name, L'\\');
            base = base ? base + 1 : name;
            if (!_wcsicmp(base, L"ntdll.dll") || !_wcsicmp(base, L"kernel32.dll") || !_wcsicmp(base, L"kernelbase.dll")) continue;
            patched += patch_module_imports(mods[i], list, count);
        }
    for (int i = 0; i < n; ++i) {
        patch_export_rva(k32, names[i], replacements[i]);
        if (kb) patch_export_rva(kb, names[i], replacements[i]);
    }
    return patched;
}

void install_alloc_trace(int threshold_mb) {
    if (threshold_mb <= 0) return;
    if (threshold_mb > 4095) threshold_mb = 4095;
    g_threshold = (SIZE_T)threshold_mb << 20;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll"), kb = GetModuleHandleW(L"kernelbase.dll");
    g_VirtualAlloc = (decltype(g_VirtualAlloc))GetProcAddress(k32, "VirtualAlloc");
    g_HeapAlloc = (decltype(g_HeapAlloc))GetProcAddress(k32, "HeapAlloc");
    g_HeapReAlloc = (decltype(g_HeapReAlloc))GetProcAddress(k32, "HeapReAlloc");
    g_MapViewOfFile = (decltype(g_MapViewOfFile))GetProcAddress(k32, "MapViewOfFile");
    g_MapViewOfFileEx = (decltype(g_MapViewOfFileEx))GetProcAddress(k32, "MapViewOfFileEx");
    Redirect list[] = {
        {"VirtualAlloc", (void *)my_VirtualAlloc, (void **)&g_VirtualAlloc},
        {"HeapAlloc", (void *)my_HeapAlloc, (void **)&g_HeapAlloc},
        {"HeapReAlloc", (void *)my_HeapReAlloc, (void **)&g_HeapReAlloc},
        {"MapViewOfFile", (void *)my_MapViewOfFile, (void **)&g_MapViewOfFile},
        {"MapViewOfFileEx", (void *)my_MapViewOfFileEx, (void **)&g_MapViewOfFileEx},
    };
    const int n = sizeof(list) / sizeof(list[0]);
    void *kb_orig[n] = {};
    Redirect list_kb[n];
    int n_kb = 0;
    if (kb) {
        for (int i = 0; i < n; ++i) {
            kb_orig[i] = (void *)GetProcAddress(kb, list[i].name);
            if (kb_orig[i] && kb_orig[i] != *list[i].orig) list_kb[n_kb++] = {list[i].name, list[i].fn, &kb_orig[i]};
        }
    }
    // Imports bound to ntdll directly (HeapAlloc is forwarded there) keep their ntdll address; patch those too.
    void *ntdll_heapalloc = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlAllocateHeap");
    void *ntdll_heaprealloc = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlReAllocateHeap");
    Redirect list2[] = {
        {"RtlAllocateHeap", (void *)my_HeapAlloc, &ntdll_heapalloc},
        {"RtlReAllocateHeap", (void *)my_HeapReAlloc, &ntdll_heaprealloc},
    };
    HMODULE mods[512];
    DWORD needed = 0;
    int patched = 0;
    if (enum_process_modules(mods, sizeof(mods), &needed))
        for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 512; ++i) {
            if (mods[i] == (HMODULE)&__ImageBase) continue;
            wchar_t name[MAX_PATH];
            if (!GetModuleFileNameW(mods[i], name, MAX_PATH)) continue;
            const wchar_t *base = wcsrchr(name, L'\\');
            base = base ? base + 1 : name;
            // Everything except the allocators themselves (the system msvcrt.dll the engine uses,
            // d3d9.dll and the graphics driver included).
            if (!_wcsicmp(base, L"ntdll.dll") || !_wcsicmp(base, L"kernel32.dll") || !_wcsicmp(base, L"kernelbase.dll")) continue;
            patched += patch_module_imports(mods[i], list, n);
            if (n_kb) patched += patch_module_imports(mods[i], list_kb, n_kb);
            patched += patch_module_imports(mods[i], list2, 2);
        }
    // Modules loaded later resolve through kernel32's export table, or kernelbase's for the API sets.
    for (int i = 0; i < n; ++i) {
        patch_export_rva(k32, list[i].name, list[i].fn);
        if (kb) patch_export_rva(kb, list[i].name, list[i].fn);
    }
    log("alloc trace: allocations of %d MB or more are logged (%d import slots redirected)", threshold_mb, patched);
}

} // namespace memx
