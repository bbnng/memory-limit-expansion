// Memory Expansion for Vampire: The Masquerade - Bloodlines: shared helpers.
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>

namespace memx {

// ---- settings (memory_expansion.ini next to the .vtm) ----
struct Settings {
    bool d3d9ex = true;             // create the device as Direct3D 9Ex
    bool offload_textures = true;   // informational: with the 9Ex device, managed textures always go to video memory
    bool offload_buffers = true;    // managed vertex/index buffers -> video memory only
    bool allow_wrapper = false;     // also run when d3d9.dll is not Windows' own (DXVK, ReShade, ...)
    int staging_cache_mb = 16;      // upload buffers kept for reuse
    int stats_seconds = 60;         // how often to log statistics (0 = never)
    bool verbose = false;
    bool console_log = false;       // copy the game's console to game_console.log (always on for the server)
    int trace_allocs_mb = 0;        // log allocations of at least this size with their caller (0 = off)
    int hunk_mb = 0;                // engine hunk cap in MB (0 = the game's 200 MB)
    bool split = false;             // process split: run the game's server in a second process
    int split_port = 27115;         // its UDP port on this PC (the first free one from here)
    int split_server_fps = 100;     // the server process's world frames a second (its fps_max)
    bool keep_system_copy = false;  // test only: also allocate the system-memory copy the managed pool kept (A/B measurements)
};
extern Settings g_settings;
extern wchar_t g_dir[MAX_PATH];     // folder of the .vtm, with trailing backslash

void log(const char *fmt, ...);
void load_settings();

// ---- address-space and memory figures ----
struct VaStats {
    uint64_t used, free_total, largest_free, private_commit;
};
VaStats va_stats();

// ---- vtable patching ----
// Replaces vt[index] with fn and returns the previous entry (or the existing one if already fn).
void *patch_vtable(void **vt, int index, void *fn);

// Points an export of mod at fn (old receives the previous target). Only later GetProcAddress calls see it.
bool patch_export(HMODULE mod, const char *name, void *fn, void **old);

// True when [rva, rva + len) lies within mod's mapped PE image.
bool in_module_image(HMODULE mod, DWORD rva, DWORD len);

// ---- diagnostics (alloc_trace.cpp) ----
void describe_address(void *addr, char *out, size_t n);          // "module.dll+rva" or the raw address
int scan_stack(void **sp, char *out, size_t n, int max);        // likely return addresses on a stack
// Where another thread of this process is. Taking a sample suspends the thread for a moment; describing it
// afterwards takes no loader lock, so it works on a thread that is stuck holding one.
struct ThreadSample {
    void *ip;
    int words;
    void *stack[16384];             // copy of its stack from the top
};
bool sample_thread(HANDLE thread, void *stack_base, ThreadSample &s);
void describe_mapped(void *addr, char *out, size_t n);           // describe_address without the loader lock
int scan_words(void *const *words, int count, char *out, size_t n, int max);   // scan_stack for a copied stack

// ---- process split (split_server.cpp) ----
bool server_mode();                  // this process was started as the game's server (-memx_server)
void on_engine_mapped(HMODULE engine);   // engine.dll was just mapped (before it runs)
void install_split_client(HMODULE engine);   // split_client.cpp: the game process's side
bool redirect_call(HMODULE mod, DWORD rva, DWORD expect_target_rva, void *fn, const char *what);

// Byte-checked code patch (split_server.cpp): true when the bytes at mod+rva are (now) repl; bytes that
// are neither expect nor repl are left alone and logged.
struct CodePatch {
    DWORD rva;
    BYTE expect[16], repl[16];
    int len;
    const char *what;
};
bool apply_patch(HMODULE mod, const CodePatch &p);
void tune_engine_sockets();

} // namespace memx
