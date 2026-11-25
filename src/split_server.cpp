// Process split, server side: Vampire.exe started with -memx_server runs the game's server only
// (vampire.dll: world, AI, physics, Python) with no window, no rendering and no local player, through
// the dedicated-server interface engine.dll still contains. The game itself runs as a second process
// that connects to it, so each half has its own 4 GB address space.
//
// How: the launcher asks engine.dll for VENGINE_LAUNCHER_API_VERSION001 and calls its Run(). In server
// mode the engine's CreateInterface export returns our object instead, whose Run() drives
// VENGINE_HLDS_API_VERSION001 (ModInit / RunFrame / ModShutdown) with the launcher's own factory, so
// the Unofficial Patch loader, its DLL fixes and the loader plugins are the same as in the game.
//
// Console commands arrive on a named pipe (\\.\pipe\vtmb-memx-<name>, -memx_pipe <name>, default
// "server"), one command per line, and are run between frames.
#include "common.h"
#include "split_load_watch.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>
#include <mmsystem.h>

namespace memx {

typedef void *(__cdecl *CreateInterfaceFn)(const char *name, int *ret);

// engine.dll 1.2: CDedicatedServerAPI, vtable engine.dll+0x186B3C
struct IDedicatedServerAPI {
    virtual bool ModInit(const char *basedir, const char *cmdline, CreateInterfaceFn factory) = 0;
    virtual void ModShutdown() = 0;
    virtual bool RunFrame() = 0;
    virtual void AddConsoleText(const char *text) = 0;
};

#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS 0x00000008
#endif

static bool s_server = false;
static HANDLE s_stdout;   // duplicated stdout of the server process (null: none)
static volatile LONG s_stdout_broken = 0;   // the game closed its end of stdout
static bool s_server_checked = false;
static CreateInterfaceFn s_engine_ci;      // engine.dll's own CreateInterface
static CreateInterfaceFn s_launcher_factory;
static char s_pipe_name[128] = "\\\\.\\pipe\\vtmb-memx-server";

bool server_mode() {
    if (!s_server_checked) {
        s_server_checked = true;
        const char *cl = GetCommandLineA();
        s_server = strstr(cl, "-memx_server") != nullptr;
        // our own copy of stdout (the game reads status and console lines from it): the original handle
        // does not stay valid in the game's process
        HANDLE so = GetStdHandle(STD_OUTPUT_HANDLE);
        if (s_server && so && so != INVALID_HANDLE_VALUE &&
            !DuplicateHandle(GetCurrentProcess(), so, GetCurrentProcess(), &s_stdout, 0, FALSE, DUPLICATE_SAME_ACCESS))
            s_stdout = nullptr;
        if (const char *p = strstr(cl, "-memx_pipe ")) {
            p += 11;
            while (*p == ' ') ++p;
            bool quoted = *p == '"';
            if (quoted) ++p;
            char name[64] = {};
            int n = 0;
            while (*p && *p != '"' && (quoted || *p != ' ') && n < 63) name[n++] = *p++;
            if (n) snprintf(s_pipe_name, sizeof(s_pipe_name), "\\\\.\\pipe\\vtmb-memx-%s", name);
        }
    }
    return s_server;
}

// ---- commands from the pipe ----
static SRWLOCK s_cmd_lock = SRWLOCK_INIT;
static std::vector<std::string> s_cmds;
static std::atomic<bool> s_has_cmds{false};
static HANDLE s_cmd_event;         // wakes the server loop immediately when a pipe command arrives
static HANDLE s_saveclient_done;   // signalled immediately on pipe_thread when the game finishes writing .HL2
static volatile LONG s_saveclient_seq = 0;
static volatile LONG s_saveclient_ok = 0;

static void enqueue_pipe_line(const std::string &line, bool truncated) {
    if (truncated) log("server: warning: pipe command truncated at %u bytes", (unsigned)line.size());
    if (line == "memx:saveclient_done" || line.rfind("memx:saveclient_done ", 0) == 0) {
        LONG expected = InterlockedCompareExchange(&s_saveclient_seq, 0, 0);
        if (line == "memx:saveclient_done") {
            InterlockedExchange(&s_saveclient_ok, 1);
            if (s_saveclient_done) SetEvent(s_saveclient_done);
        } else {
            long id = 0;
            int ok = 1;
            int parsed = sscanf_s(line.c_str() + 21, "%ld %d", &id, &ok);
            if (parsed >= 1 && id == expected) {
                InterlockedExchange(&s_saveclient_ok, ok ? 1 : 0);
                if (s_saveclient_done) SetEvent(s_saveclient_done);
            } else {
                log("server: ignoring stale saveclient_done (got %ld, expected %ld)", id, (long)expected);
            }
        }
        return;
    }
    // Engine Cbuf_Execute uses a 1024-byte stack buffer; reject oversized console commands cleanly.
    bool is_memx = line.rfind("memx:", 0) == 0;
    if (!is_memx && line.size() > 1000) {
        log("server: warning: rejected oversized command (%u bytes): %.80s", (unsigned)line.size(), line.c_str());
        return;
    }
    AcquireSRWLockExclusive(&s_cmd_lock);
    bool was_empty = s_cmds.empty();
    if (s_cmds.size() < 4096) {
        s_cmds.push_back(line);
    } else {
        log("server: warning: pipe command queue full (4096), dropped: %.80s", line.c_str());
    }
    s_has_cmds.store(true, std::memory_order_release);
    if (was_empty && s_cmd_event) SetEvent(s_cmd_event);
    ReleaseSRWLockExclusive(&s_cmd_lock);
}

static DWORD WINAPI pipe_thread(void *) {
    for (;;) {
        HANDLE pipe = CreateNamedPipeA(s_pipe_name, PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                       1, 0, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            log("server: cannot create pipe %s (error %lu)", s_pipe_name, GetLastError());
            return 1;
        }
        if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(pipe);
            continue;
        }
        std::string line;
        bool truncated = false;
        char buf[1024];
        DWORD got;
        while (ReadFile(pipe, buf, sizeof(buf), &got, nullptr) && got) {
            for (DWORD i = 0; i < got; ++i) {
                char ch = buf[i];
                if (ch == '\0') continue;
                if (ch == '\n' || ch == '\r') {
                    if (!line.empty()) enqueue_pipe_line(line, truncated);
                    line.clear();
                    truncated = false;
                } else if (line.size() < 4096) {
                    line += ch;
                } else {
                    truncated = true;
                }
            }
        }
        if (!line.empty()) enqueue_pipe_line(line, truncated);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

static void install_spew_tap();
static bool status_write(const char *text);

// ---- the main loop's progress ----
// The game process sees the world only through this process's snapshots and stdout. The main loop sends
// "memx:alive" once a second (the game ends a server that stops sending it, split_client.cpp) and notes
// what it is doing, so a stall is logged with where it happened.
static volatile DWORD s_loop_tick;          // GetTickCount() when the main loop last went round
static const char *volatile s_loop_step = "starting";
static DWORD WINAPI loop_watch_thread(void *) {
    DWORD reported = 0;
    DWORD broken_since = 0;
    for (;;) {
        Sleep(1000);
        if (s_stdout && s_stdout_broken) {
            DWORD now = GetTickCount();
            if (!broken_since) broken_since = now ? now : 1;
            else if (now - broken_since >= 5000) {
                log("server: game process stdout pipe closed and server did not exit; terminating orphan server");
                ExitProcess(0);
            }
        }
        DWORD tick = s_loop_tick;
        if (!tick) continue;
        DWORD now = GetTickCount();
        DWORD stalled = (int)(now - tick) > 0 ? (now - tick) : 0;
        if (stalled >= 10000 && stalled / 10000 != reported) {
            reported = stalled / 10000;
            log("server: the main loop has not gone round for %lu s (in: %s)", stalled / 1000, s_loop_step);
        } else if (stalled < 10000 && reported) {
            log("server: the main loop runs again");
            reported = 0;
        }
    }
}

// A message box in this process would never be seen (no window; it opens behind the game) and would
// stop the world for good while it waits for a click. The engine has a few (Host_Changelevel's failed
// save, the sound device's retry box): log the text and answer as a click on the safe button would.
static int message_box_answer(UINT type) {
    switch (type & 0xF) {
    case MB_OKCANCEL: case MB_YESNOCANCEL: case MB_RETRYCANCEL: return IDCANCEL;
    case MB_ABORTRETRYIGNORE: return IDIGNORE;
    case MB_YESNO: return IDNO;
    case 6 /*MB_CANCELTRYCONTINUE*/: return 11 /*IDCONTINUE*/;
    default: return IDOK;
    }
}
static int WINAPI server_message_box_a(HWND, LPCSTR text, LPCSTR caption, UINT type) {
    log("server: message box not shown (this process has no window): \"%s\": %s", caption ? caption : "", text ? text : "");
    return message_box_answer(type);
}
static int WINAPI server_message_box_w(HWND, LPCWSTR text, LPCWSTR caption, UINT type) {
    char t[512] = {}, c[128] = {};
    if (text) WideCharToMultiByte(CP_UTF8, 0, text, -1, t, sizeof(t) - 1, nullptr, nullptr);
    if (caption) WideCharToMultiByte(CP_UTF8, 0, caption, -1, c, sizeof(c) - 1, nullptr, nullptr);
    log("server: message box not shown (this process has no window): \"%s\": %s", c, t);
    return message_box_answer(type);
}
// user32's export (modules loaded later) and the import slots of the modules loaded so far
static int patch_user32_module(BYTE *base, void *old_fn, void *new_fn) {
    int count = 0;
    __try {
        auto dos = (IMAGE_DOS_HEADER *)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        if (dos->e_lfanew < 0 || dos->e_lfanew > 0x100000) return 0;
        auto nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        DWORD img_size = nt->OptionalHeader.SizeOfImage;
        IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress || !dir.Size || dir.VirtualAddress >= img_size) return 0;
        for (auto imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
             (DWORD)((BYTE *)imp - base) + sizeof(*imp) <= img_size && imp->Name; ++imp) {
            if (!imp->FirstThunk || imp->Name >= img_size || imp->FirstThunk >= img_size) continue;
            if (_stricmp((char *)(base + imp->Name), "user32.dll") != 0) continue;
            for (auto th = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
                 (DWORD)((BYTE *)th - base) + sizeof(*th) <= img_size && th->u1.Function; ++th) {
                if ((void *)th->u1.Function != old_fn) continue;
                DWORD prot;
                if (VirtualProtect(&th->u1.Function, 4, PAGE_READWRITE, &prot)) {
                    InterlockedExchange((volatile LONG *)&th->u1.Function, (LONG)(uintptr_t)new_fn);
                    VirtualProtect(&th->u1.Function, 4, prot, &prot);
                    ++count;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return count;
}
static int patch_user32_imports(const char *name, void *old_fn, void *new_fn) {
    int count = 0;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&patch_user32_imports, &self);
    HMODULE mods[512];
    DWORD needed = 0;
    typedef BOOL(WINAPI * EnumMods_t)(HANDLE, HMODULE *, DWORD, LPDWORD);
    auto enum_mods = (EnumMods_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules");
    if (!enum_mods || !enum_mods(GetCurrentProcess(), mods, sizeof(mods), &needed)) return 0;
    for (DWORD m = 0; m < needed / sizeof(HMODULE) && m < 512; ++m) {
        if (mods[m] == self) continue;
        count += patch_user32_module((BYTE *)mods[m], old_fn, new_fn);
    }
    (void)name;
    return count;
}
static void install_message_box_guard() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) user32 = LoadLibraryW(L"user32.dll");
    if (!user32) return;
    struct { const char *name; void *fn; } hooks[] = {{"MessageBoxA", (void *)server_message_box_a}, {"MessageBoxW", (void *)server_message_box_w}};
    int slots = 0;
    for (auto &h : hooks) {
        void *old = nullptr;
        FARPROC cur = GetProcAddress(user32, h.name);
        if (!cur) continue;
        if ((void *)cur == h.fn) continue;
        if (!patch_export(user32, h.name, h.fn, &old)) continue;
        slots += patch_user32_imports(h.name, (void *)cur, h.fn);
    }
    log("server: message boxes are logged instead of shown (%d import slots)", slots);
}
static size_t add_game_cmds(const std::string &line);
static bool client_signing_on(int *seq_out = nullptr);
static bool s_ran_frame = false;
static LARGE_INTEGER s_last_frame_qpc = {};
static LARGE_INTEGER s_qpc_freq = {};

// ---- a client DLL that does nothing ----
// Engine code that runs on the server sometimes tells the client DLL something (the autosave at a level
// start shows its "saving" notice through it). A dedicated server has no client DLL; it gets this one:
// client.dll's CHLClient (VClient008) slot by slot, every method returning 0 and popping the argument
// bytes the real one pops (taken from client.dll 1.2 in IDA).
static __declspec(naked) void cl_ret0() { __asm { xor eax, eax } __asm { ret } }
static __declspec(naked) void cl_ret4() { __asm { xor eax, eax } __asm { ret 4 } }
static __declspec(naked) void cl_ret8() { __asm { xor eax, eax } __asm { ret 8 } }
static __declspec(naked) void cl_ret12() { __asm { xor eax, eax } __asm { ret 12 } }
static __declspec(naked) void cl_ret16() { __asm { xor eax, eax } __asm { ret 16 } }
static __declspec(naked) void cl_ret24() { __asm { xor eax, eax } __asm { ret 24 } }
static const BYTE kClientArgBytes[45] = {12, 0, 4, 0, 0, 0, 0, 8, 4, 4, 0, 16, 4, 0, 0, 8, 0, 0, 4, 12, 24, 16, 4,
                                         8, 0, 4, 4, 8, 4, 0, 8, 4, 4, 4, 24, 24, 4, 4, 4, 4, 8, 4, 4, 12, 0};
// VClient008 slot 43 (+0xAC, 12 arg bytes): CHLClient::SendInfoBarMessage(unsigned int type, int param, const char *str).
// engine.dll calls it directly in Host_Savegame_f (79 "Game Saving...", 54 "Game Saved"),
// Host_Autosave_f (79, 54), and CSaveRestore::ValidSave (55, 66..71 "Cannot save...").
// Forward it to the game process so save/autosave/can't-save HUD notices appear as in single player.
static void __fastcall null_client_infobar(void *, void *, unsigned int type, int param, const char *str) {
    if (!s_stdout || type > 82) return;
    char line[320];
    if (str) {
        char clean[200];
        size_t n = 0;
        __try {
            for (const char *p = str; *p && n < sizeof(clean) - 1; ++p) {
                unsigned char ch = (unsigned char)*p;
                if (ch == '\r' || ch == '\n') continue;
                clean[n++] = (char)ch;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            n = 0;
        }
        clean[n] = '\0';
        if (n > 0) {
            snprintf(line, sizeof(line), "memx:infobar %u %d %s\n", type, param, clean);
            status_write(line);
            return;
        }
    }
    snprintf(line, sizeof(line), "memx:infobar %u %d\n", type, param);
    status_write(line);
}

static void *s_null_client_vt[45];
static void *s_null_client = s_null_client_vt;
static void install_null_client() {
    void **slot = (void **)0x201CC820;   // engine.dll: the client DLL interface
    if (*slot) return;
    for (int i = 0; i < 45; ++i) {
        switch (kClientArgBytes[i]) {
        case 0: s_null_client_vt[i] = (void *)cl_ret0; break;
        case 4: s_null_client_vt[i] = (void *)cl_ret4; break;
        case 8: s_null_client_vt[i] = (void *)cl_ret8; break;
        case 12: s_null_client_vt[i] = (void *)cl_ret12; break;
        case 16: s_null_client_vt[i] = (void *)cl_ret16; break;
        default: s_null_client_vt[i] = (void *)cl_ret24; break;
        }
    }
    s_null_client_vt[43] = (void *)null_client_infobar;
    *slot = &s_null_client;
}

// Pause refcounts (v_pause_count 0x20B43788, setpause_count 0x20B4378C) and v_paused_flag (0x20D63510)
// are never cleared by SV_SpawnServer. Loading a save from the ESC menu or a paused UI screen disconnects
// the client before unpause/v_unpause is forwarded, leaking the server pause refcount across levels.
static bool s_pause_addrs_ok = false;
static void check_server_pause_addrs(HMODULE engine) {
    static const BYTE kVPauseCount[] = {0xA1, 0x88, 0x37, 0xB4, 0x20};
    static const BYTE kVPauseFlag[] = {0xC7, 0x05, 0x10, 0x35, 0xD6, 0x20, 0x01, 0x00, 0x00, 0x00};
    static const BYTE kSetPauseCount[] = {0xA1, 0x8C, 0x37, 0xB4, 0x20};
    s_pause_addrs_ok = in_module_image(engine, 0x918A9, sizeof(kVPauseCount)) &&
                       in_module_image(engine, 0x918BB, sizeof(kVPauseFlag)) &&
                       in_module_image(engine, 0x9199B, sizeof(kSetPauseCount)) &&
                       memcmp((const BYTE *)engine + 0x918A9, kVPauseCount, sizeof(kVPauseCount)) == 0 &&
                       memcmp((const BYTE *)engine + 0x918BB, kVPauseFlag, sizeof(kVPauseFlag)) == 0 &&
                       memcmp((const BYTE *)engine + 0x9199B, kSetPauseCount, sizeof(kSetPauseCount)) == 0;
}
static void reset_server_pause_state() {
    if (!s_pause_addrs_ok) return;
    *(volatile int *)0x20B43788 = 0;   // v_pause_count
    *(volatile int *)0x20B4378C = 0;   // setpause_count
    *(volatile int *)0x20D63510 = 0;   // v_paused_flag
    *(volatile int *)0x212B0758 = 0;   // sv.paused
}

// ---- what the engine still has to do for a level command (split_load_watch.h) ----
static const uintptr_t kCbufSize = 0x20346930;    // bytes waiting in the command buffer
static const uintptr_t kHostState = 0x20D5E0B0;   // CHostState: +0 current state, +4 next state
static bool s_load_watch_ok;   // the two are where the analysed engine.dll has them; without them no failure is reported
static void check_load_watch(HMODULE engine) {
    // Cbuf_Execute: sub esp,400h; push ebx,ebp; mov ebp,[20346930]; xor ebx,ebx; cmp ebp,ebx
    static const BYTE kCbufExecute[] = {0x81, 0xEC, 0x00, 0x04, 0x00, 0x00, 0x53, 0x55, 0x8B, 0x2D, 0x30, 0x69, 0x34, 0x20, 0x33, 0xDB, 0x3B, 0xEB};
    // HostState_GameShutdown: cmp [20D5E0B0],4; jz; mov eax,5; mov [20D5E0B0],eax; mov [20D5E0B4],eax
    static const BYTE kGameShutdown[] = {0x83, 0x3D, 0xB0, 0xE0, 0xD5, 0x20, 0x04, 0x74, 0x10, 0xB8, 0x05, 0x00, 0x00, 0x00,
                                         0xA3, 0xB0, 0xE0, 0xD5, 0x20, 0xA3, 0xB4, 0xE0, 0xD5, 0x20};
    s_load_watch_ok = in_module_image(engine, 0x2DF10, sizeof(kCbufExecute)) && in_module_image(engine, 0x9A670, sizeof(kGameShutdown)) &&
                      memcmp((const BYTE *)engine + 0x2DF10, kCbufExecute, sizeof(kCbufExecute)) == 0 &&
                      memcmp((const BYTE *)engine + 0x9A670, kGameShutdown, sizeof(kGameShutdown)) == 0;
    if (!s_load_watch_ok) log("server: failed level commands are not reported (unknown engine code)");
}

// Client-side level-transition plugins (e.g. CaineWorld) are loaded into the headless server process
// too by the UP loader and hook IVEngineServer::ChangeLevel (VEngineServer014 slot 9, engine+0x186EA0),
// deferring the transition until Present() finishes a fade. Present() never runs in a dedicated server,
// so every door / level transition would be swallowed forever. Restore engine.dll's ChangeLevel slot in
// the server process (BLSE hooks ChangeLevel inline at engine+0x108A50, so BLSE still receives every call).
static void **s_changelevel_slot = nullptr;
static void *s_changelevel_orig = nullptr;
static int s_changelevel_restores = 0;
static char s_changelevel_logged[MAX_PATH] = {};
static void init_changelevel_guard(HMODULE engine) {
    s_changelevel_slot = nullptr;
    s_changelevel_orig = nullptr;
    s_changelevel_restores = 0;
    s_changelevel_logged[0] = 0;
    static const BYTE kChangeLevelHead[] = {0xA1, 0xD0, 0xF1, 0x2A, 0x21, 0x3B, 0x05, 0x20, 0xF5, 0x38, 0x21, 0x74, 0x41};
    if (engine == (HMODULE)0x20000000 && in_module_image(engine, 0x186EA0, 4) &&
        in_module_image(engine, 0x108A50, sizeof(kChangeLevelHead)) &&
        memcmp((const BYTE *)engine + 0x108A50, kChangeLevelHead, sizeof(kChangeLevelHead)) == 0) {
        s_changelevel_slot = (void **)((BYTE *)engine + 0x186EA0);
        s_changelevel_orig = (void *)((BYTE *)engine + 0x108A50);
    } else {
        log("server: IVEngineServer::ChangeLevel head bytes do not match engine.dll 1.2; guard disabled");
    }
}
static bool safe_restore_slot(void **slot, void *orig) {
    DWORD prot = 0;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &prot)) return false;
    bool ok = false;
    __try {
        InterlockedExchangePointer(slot, orig);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    VirtualProtect(slot, sizeof(void *), prot, &prot);
    if (ok) FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
    return ok;
}
static void restore_changelevel_if_hooked() {
    if (!s_changelevel_slot || !s_changelevel_orig || s_changelevel_restores >= 10) return;
    void *cur = *(void *volatile *)s_changelevel_slot;
    if (cur == s_changelevel_orig) return;
    HMODULE mod = nullptr;
    char modname[MAX_PATH] = "unknown";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)cur, &mod) && mod) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(mod, path, MAX_PATH)) {
            const char *base = strrchr(path, '\\');
            snprintf(modname, sizeof(modname), "%s", base ? base + 1 : path);
        }
        if (!_stricmp(modname, "engine.dll") || !_stricmp(modname, "blse.vtm")) return;
    }
    if (safe_restore_slot(s_changelevel_slot, s_changelevel_orig)) {
        ++s_changelevel_restores;
        if (_stricmp(s_changelevel_logged, modname) != 0) {
            snprintf(s_changelevel_logged, sizeof(s_changelevel_logged), "%s", modname);
            log("server: restored IVEngineServer::ChangeLevel (was hooked by %s; client transition hooks cannot run headless)", modname);
        }
        if (s_changelevel_restores == 10)
            log("server: IVEngineServer::ChangeLevel kept being re-hooked by %s; stopping restore retries", modname);
    }
}

// ---- the launcher interface, replaced ----
struct LauncherAPI {
    // CEngineAPI::Run(instance, basedir, cmdline, factory); a true result asks the launcher to restart
    virtual bool Run(void *instance, char *basedir, char *cmdline, CreateInterfaceFn factory);
};

static void *__cdecl server_factory(const char *name, int *ret) {
    // ModInit only checks that this interface exists; the engine never calls it
    static void *exports_stub[4] = {};
    static void *exports = exports_stub;
    if (!strcmp(name, "VENGINE_DEDICATEDEXPORTS_API_VERSION001")) {
        if (ret) *ret = 0;
        return &exports;
    }
    return s_launcher_factory(name, ret);
}

bool LauncherAPI::Run(void *, char *basedir, char *cmdline, CreateInterfaceFn factory) {
    s_launcher_factory = factory;
    auto hlds = (IDedicatedServerAPI *)s_engine_ci("VENGINE_HLDS_API_VERSION001", nullptr);
    if (!hlds) {
        log("server: engine.dll has no VENGINE_HLDS_API_VERSION001");
        return false;
    }
    log("server: starting the dedicated server (basedir %s)%s", basedir, s_stdout ? "; status to stdout" : "");
    s_saveclient_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hlds->ModInit(basedir, cmdline, server_factory)) {
        log("server: ModInit failed");
        if (s_saveclient_done) { CloseHandle(s_saveclient_done); s_saveclient_done = nullptr; }
        return false;
    }
    // Single player has no network timeout; raise sv_timeout so slow HD-texture loads, window drags,
    // or pauses on the local client never drop the player back to the menu.
    hlds->AddConsoleText("sv_timeout 86400\n");
    if (HMODULE vamp = GetModuleHandleW(L"vampire.dll")) {
        // CRopeKeyframe::ShouldTransmit (vampire.dll+0x19F690) transmits out-of-PVS rope endpoints only on the
        // first snapshot (m_bFirstTransmit at +0x450), then PVS-culls them. Over UDP that initial snapshot can
        // be dropped during map load, leaving ropes without their far endpoint so they hang straight down.
        // Always transmit ropes (static, 0 delta bytes once acknowledged): jnz +11 -> nop nop.
        static const CodePatch kRopeTransmit = {
            0x19F690,
            {0x8A, 0x81, 0x50, 0x04, 0x00, 0x00, 0x84, 0xC0, 0x75, 0x0B, 0xB0, 0x01},
            {0x8A, 0x81, 0x50, 0x04, 0x00, 0x00, 0x84, 0xC0, 0x90, 0x90, 0xB0, 0x01},
            12,
            "server: rope endpoints stay transmitted"};
        apply_patch(vamp, kRopeTransmit);
    }
    install_spew_tap();
    install_null_client();
    restore_changelevel_if_hooked();
    // The Unofficial Patch loader tells its plugins when the engine loads the DLLs a game with a window
    // loads after vampire.dll (StudioRender, GameUI, the sound DLL, client.dll), and BLSE installs its game
    // hooks (game events, level changes, game frames, its console commands) at those calls. A dedicated
    // server loads none of them: give BLSE the call for GameUI, which installs only those hooks.
    if (HMODULE blse = GetModuleHandleW(L"blse.vtm")) {
        if (auto loaded_gameui = (void(__cdecl *)())GetProcAddress(blse, "loaded_gameui")) {
            log("server: BLSE: installing its game hooks");
            loaded_gameui();
        }
    }
    log("server: running; commands on %s", s_pipe_name);
    s_cmd_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    QueryPerformanceFrequency(&s_qpc_freq);
    if (HANDLE t = CreateThread(nullptr, 0, pipe_thread, nullptr, 0, nullptr)) CloseHandle(t);
    s_loop_tick = GetTickCount();
    if (HANDLE t = CreateThread(nullptr, 0, loop_watch_thread, nullptr, 0, nullptr)) CloseHandle(t);
    DWORD last_alive = 0;
    timeBeginPeriod(1);
    // The loop sleeps between world frames until just before the next 1/fps_max deadline (waking
    // immediately if a pipe command arrives). Windows 11 may ignore timeBeginPeriod for a process without
    // a visible window, so disable timer-resolution throttling and use a high-resolution waitable timer.
    {
        struct { ULONG Version, ControlMask, StateMask; } throttling = {1, 0x4, 0};
        auto set_info = (BOOL(WINAPI *)(HANDLE, int, void *, DWORD))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation");
        if (set_info) set_info(GetCurrentProcess(), 4 /*ProcessPowerThrottling*/, &throttling, sizeof(throttling));
    }
    HANDLE tick = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    std::vector<std::string> cmds;
    int reported = -1;
    int paused = 0;
    int last_seq = -1;
    DWORD last_active = 0;
    LoadWatch load_watch;
    long load_id = 0;   // the game's number for the level command that follows ("memx:loadid")
    for (;;) {
        if (s_stdout && s_stdout_broken) {
            log("server: game process stdout pipe closed; shutting down");
            break;
        }
        DWORD now_tick = GetTickCount();
        s_loop_tick = now_tick ? now_tick : 1;
        if (now_tick - last_alive >= 1000) {
            last_alive = now_tick;
            status_write("memx:alive\n");
        }
        if (s_has_cmds.load(std::memory_order_acquire)) {
            s_loop_step = "console commands from the game";
            AcquireSRWLockExclusive(&s_cmd_lock);
            cmds.swap(s_cmds);
            s_has_cmds.store(false, std::memory_order_release);
            ReleaseSRWLockExclusive(&s_cmd_lock);
            size_t gamecmds_total = 0;
            bool got_gamecmds = false;
            for (std::string &c : cmds) {
                if (c.rfind("memx:gamecmds ", 0) == 0) {
                    gamecmds_total = add_game_cmds(c);
                    got_gamecmds = true;
                    continue;
                }
                if (c.rfind("memx:loadid ", 0) == 0) {
                    load_id = atol(c.c_str() + 12);
                    continue;
                }
                // the game's aliases come before every new game or load (split_client.cpp): not logged one by one
                if (c.rfind("alias ", 0) != 0) log("server: > %s", c.c_str());
                const char *trimmed = c.c_str();
                while (*trimmed == ' ' || *trimmed == '\t') ++trimmed;
                if (!_strnicmp(trimmed, "map ", 4)) {
                    // A new game starts from an empty save folder: the level states (*.HL?) a previous game
                    // left there would otherwise be restored into the new one. CSaveRestore::ClearSaveDir
                    // (engine.dll saverestore object at 0x201A44E8, vtable slot 8), as single player does
                    // when it ends a game.
                    void *sr = *(void *volatile *)0x201A44E8;
                    if (sr && (*(void ***)sr)[8] == (void *)0x20098CB0) ((void(__fastcall *)(void *, void *))(*(void ***)sr)[8])(sr, nullptr);
                }
                if (s_load_watch_ok && is_level_command(trimmed)) {
                    load_watch.begin(load_id, *(volatile int *)0x212AF1D0);
                    load_id = 0;
                }
                c += '\n';
                hlds->AddConsoleText(c.c_str());
            }
            if (got_gamecmds && g_settings.verbose)
                log("server: %u names of the game's commands and variables", (unsigned)gamecmds_total);
            cmds.clear();
        }
        // The engine's "world model" (engine.dll 0x20B42AEC) is set by the client when it loads a map;
        // engine code the game calls on the server reads it too (GetLightForPoint, used for how lit the
        // player is). Here it is the server's map (sv.worldmodel, 0x212B0828).
        *(void *volatile *)0x20B42AEC = *(void *volatile *)0x212B0828;
        restore_changelevel_if_hooked();
        tune_engine_sockets();
        s_loop_step = "a world frame (RunFrame)";
        if (!hlds->RunFrame()) break;
        s_loop_step = "after the world frame";
        // a new level is up (svs.spawncount 0x212AF1D0 changes with every SV_SpawnServer; sv.active 0x212B0754):
        // tell the game, which connects to it
        int sv_active = *(volatile int *)0x212B0754;
        int spawn = *(volatile int *)0x212AF1D0;
        if (sv_active && spawn != reported) {
            reported = spawn;
            char line[160];
            snprintf(line, sizeof(line), "memx:ready %d %.120s\n", spawn, (const char *)0x212B07E8);
            status_write(line);
        }
        // the game waits on its loading screen: tell it when the level it asked for will not come
        if (load_watch.pending &&
            load_watch.failed(*(volatile int *)kCbufSize == 0, *(volatile int *)kHostState, *(volatile int *)(kHostState + 4),
                              sv_active != 0, spawn, GetTickCount())) {
            log("server: level command %ld did not start a level (sv_active=%d, spawn=%d); notifying the game", load_watch.id, sv_active, spawn);
            char line[48];
            snprintf(line, sizeof(line), "memx:loadfailed %ld\n", load_watch.id);
            status_write(line);
        }
        // sv.paused (0x212B0758): the game's menu and interface screens pause the world
        int sv_paused = *(volatile int *)0x212B0758;
        if (sv_paused != paused) {
            paused = sv_paused;
            log("server: world %s", paused ? "paused" : "running again");
        }
        // While a client is signing on and fragments are actively flowing (netchan sequence advancing),
        // yield without a timer wait so 160-fragment stop-and-wait finishes in ~20 ms instead of ~600 ms.
        // Once the client pauses > 25 ms (loading BSP/models), fall back to the timer so we idle at ~0% CPU.
        int seq = 0;
        if (client_signing_on(&seq)) {
            DWORD now = GetTickCount();
            if (seq != last_seq) {
                last_seq = seq;
                last_active = now;
            }
            if (now - last_active < 25) {
                SwitchToThread();
                continue;
            }
        } else {
            last_seq = -1;
        }
        s_loop_step = "waiting for the next frame";
        // Compute adaptive sleep until ~0.8 ms before the next world frame, waking immediately on pipe input.
        LONGLONG wait_us = 1000;
        if (!sv_active || sv_paused) {
            wait_us = 8000;
        } else if (s_ran_frame && s_qpc_freq.QuadPart > 0) {
            BYTE *fps_cv = *(BYTE *volatile *)0x20B42884;
            float fmax = fps_cv ? *(float *)(fps_cv + 0x28) : (float)g_settings.split_server_fps;
            if (fmax < 30.0f) fmax = (float)g_settings.split_server_fps;
            if (fmax > 1000.0f) fmax = 1000.0f;
            LONGLONG target_us = (LONGLONG)(1000000.0f / fmax);
            LARGE_INTEGER now_qpc;
            QueryPerformanceCounter(&now_qpc);
            LONGLONG elapsed_us = (now_qpc.QuadPart - s_last_frame_qpc.QuadPart) * 1000000LL / s_qpc_freq.QuadPart;
            LONGLONG rem_us = target_us - elapsed_us;
            if (rem_us > 1500) {
                wait_us = rem_us - 800;
                if (wait_us > 8000) wait_us = 8000;
            } else if (rem_us > 400) {
                wait_us = 250;
            } else {
                SwitchToThread();
                continue;
            }
        }
        LARGE_INTEGER due;
        due.QuadPart = -wait_us * 10;   // 100-ns units
        if (tick && SetWaitableTimer(tick, &due, 0, nullptr, nullptr, FALSE)) {
            HANDLE waits[2] = {tick, s_cmd_event};
            WaitForMultipleObjects(s_cmd_event ? 2 : 1, waits, FALSE, 50);
        } else {
            Sleep((DWORD)(wait_us >= 1000 ? wait_us / 1000 : 1));
        }
    }
    if (tick) CloseHandle(tick);
    HANDLE ev = nullptr;
    AcquireSRWLockExclusive(&s_cmd_lock);
    ev = s_cmd_event;
    s_cmd_event = nullptr;
    ReleaseSRWLockExclusive(&s_cmd_lock);
    if (ev) CloseHandle(ev);
    timeEndPeriod(1);
    log("server: shutting down");
    hlds->ModShutdown();
    return false;
}

static LauncherAPI s_launcher_api;

// Server mode only: the first access violations are logged with their callers (the dedicated path is
// code Bloodlines never ran, so this is how its next missing piece shows up).
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *e) {
    static volatile LONG count;
    static __declspec(thread) bool in_veh;
    if (in_veh || e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || InterlockedIncrement(&count) > 4)
        return EXCEPTION_CONTINUE_SEARCH;
    in_veh = true;
    char where[96], stack[700];
    describe_address(e->ExceptionRecord->ExceptionAddress, where, sizeof(where));
    scan_stack((void **)e->ContextRecord->Esp, stack, sizeof(stack), 12);
    log("server: access violation at %s (%s %p) | callers: %s", where, e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
        (void *)e->ExceptionRecord->ExceptionInformation[1], stack);
    // text on the stack near the fault (overflowed buffers show what overflowed them)
    char text[400];
    int n = 0;
    const BYTE *sp = (const BYTE *)e->ContextRecord->Esp;
    for (int i = 0; i < 1024 && n < (int)sizeof(text) - 2; ++i) {
        BYTE b;
        __try { b = sp[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (b >= 32 && b < 127) text[n++] = (char)b;
        else if (n && text[n - 1] != '|') text[n++] = '|';
    }
    text[n] = 0;
    log("server: stack text: %s", text);
    in_veh = false;
    return EXCEPTION_CONTINUE_SEARCH;
}

static void *__cdecl my_engine_ci(const char *name, int *ret) {
    if (name && !strcmp(name, "VENGINE_LAUNCHER_API_VERSION001")) {
        if (ret) *ret = 0;
        return &s_launcher_api;
    }
    return s_engine_ci(name, ret);
}

static bool patch_matches(HMODULE mod, const CodePatch &p) {
    if (p.len <= 0 || p.len > (int)sizeof(p.expect) || !in_module_image(mod, p.rva, (DWORD)p.len)) return false;
    const BYTE *at = (const BYTE *)mod + p.rva;
    return memcmp(at, p.repl, p.len) == 0 || memcmp(at, p.expect, p.len) == 0;
}

bool apply_patch(HMODULE mod, const CodePatch &p) {
    if (p.len <= 0 || p.len > (int)sizeof(p.expect) || !in_module_image(mod, p.rva, (DWORD)p.len)) {
        log("not patched (out of image bounds): %s", p.what);
        return false;
    }
    BYTE *at = (BYTE *)mod + p.rva;
    if (memcmp(at, p.repl, p.len) == 0) return true;
    if (memcmp(at, p.expect, p.len) != 0) {
        log("not patched (unknown bytes): %s", p.what);
        return false;
    }
    DWORD prot;
    if (!VirtualProtect(at, p.len, PAGE_EXECUTE_READWRITE, &prot)) return false;
    memcpy(at, p.repl, p.len);
    VirtualProtect(at, p.len, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), at, p.len);
    return true;
}

// engine.dll 1.2 at its preferred base (the patched operands are absolute addresses)
static const CodePatch kEnginePatches[] = {
    // CDedicatedServerAPI::ModInit calls Shader_InitDedicated (0x200875A0), which never fetches the
    // hardware config the particle manager reads: call the client's Shader_Init (0x200875F0) instead...
    {0xFBA52, {0xE8, 0x49, 0xBB, 0xF8, 0xFF}, {0xE8, 0x99, 0xBB, 0xF8, 0xFF}, 5, "ModInit -> Shader_Init"},
    // ...with "shaderapiempty" (0x201A1CC0) instead of "shaderapidx9" (0x201A1D70)
    {0x8761F, {0x68, 0x70, 0x1D, 0x1A, 0x20}, {0x68, 0xC0, 0x1C, 0x1A, 0x20}, 5, "Shader_Init uses shaderapiempty"},
    // Host_RunFrame passes the frame time to the studio renderer, which a dedicated server never
    // creates: jump over it (0x2008E636 -> 0x2008E675)
    {0x8E636, {0x8B, 0x0D, 0xB0, 0x74, 0x19, 0x20}, {0xEB, 0x3D, 0x90, 0x90, 0x90, 0x90}, 6, "Host_RunFrame without studio render"},
    // the loading-progress update redraws the loading screen (V_RenderView): nothing to draw
    {0x87110, {0x51, 0xA1, 0xBC, 0x2E, 0xB4, 0x20}, {0xC3, 0xA1, 0xBC, 0x2E, 0xB4, 0x20}, 6, "no loading-screen redraw"},
    // Mod_LoadStudioModel: skip loading a model's mesh data into the studio renderer (0x200B8C30 jz -> jmp)
    {0xB8C30, {0x74, 0x08, 0x56, 0x8B, 0xCB}, {0xEB, 0x08, 0x56, 0x8B, 0xCB}, 5, "no studio mesh data"},
    // SV_SpawnServer: NET_Config(maxclients > 1) closes the network sockets of a one-player server;
    // the game process connects over them: NET_Config(1) (0x200F5697: setg al; xor ecx,ecx; mov cl,al; push ecx)
    {0xF5697, {0x0F, 0x9F, 0xC0, 0x33, 0xC9, 0x8A, 0xC8, 0x51}, {0x6A, 0x01, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90}, 8, "one-player server keeps its sockets"},
    // SV_SpawnServer sends map and client.dll checksums 0 for a one-player game (the local client
    // skips the check); a connecting client compares them: compute them (0x200F5C36 jle -> nop)
    {0xF5C36, {0x0F, 0x8E, 0x8C, 0x00, 0x00, 0x00}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}, 6, "map checksum for one-player server"},
    // CEngine::Frame sleeps 20-50 ms per frame while the game window is not the active app; the server
    // has no window (that sleep was the whole ping): never (0x200FC1E7 jnz -> jmp)
    {0xFC1E7, {0x75, 0x1E, 0x8B, 0x7E, 0x08}, {0xEB, 0x1E, 0x8B, 0x7E, 0x08}, 5, "no inactive-window sleep"},
    // Host_WriteConfiguration (0x2008B760; Host_Shutdown calls it): the server shares the game's folder and
    // must never write the player's cfg/config.cfg -> ret
    {0x8B760, {0xA0, 0xC4, 0x2E, 0xB4, 0x20}, {0xC3, 0xC4, 0x2E, 0xB4, 0x20}, 5, "server writes no config.cfg"},
    // CEngineTraceClient::GetWorldCollideable (0x2006A540) dereferences g_pClientEntityList (0x201CC82C),
    // which is NULL on a dedicated server (crashes at 0x6A546 when vampire.dll traces after cutscenes).
    // Redirect to CEngineTraceServer::GetWorldCollideable (0x2006A560): jmp +0x1B.
    {0x6A540, {0x8B, 0x0D, 0x2C, 0xC8, 0x1C, 0x20}, {0xE9, 0x1B, 0x00, 0x00, 0x00, 0x90}, 6, "EngineTraceClient::GetWorldCollideable -> Server"},
    // CEngineTraceClient::SetTraceEntity (0x20069630) also dereferences g_pClientEntityList (0x201CC82C)
    // on static-prop hits. Redirect to CEngineTraceServer::SetTraceEntity (0x200696B0): jmp +0x7B.
    {0x69630, {0x56, 0x8B, 0x74, 0x24, 0x0C}, {0xE9, 0x7B, 0x00, 0x00, 0x00}, 5, "EngineTraceClient::SetTraceEntity -> Server"},
};

// Movement smoothness (the game and the server talk over 127.0.0.1 UDP, which the engine treats as a
// remote address, not as its in-process loopback). Optional: a failed check leaves the server working,
// only less smooth.
static const CodePatch kSmoothPatches[] = {
    // Netchan_CanPacket holds packets back by the client's "rate" (at most 20000 bytes/s, 9999 with sv_lan)
    // except on the loopback channel: 25 snapshots a second instead of one per server frame. Every channel
    // of this process is the game on this PC: take the loopback path (0x200BC5C0: cmp [eax+4],1; jnz -> nops)
    {0xBC5C0, {0x83, 0x78, 0x04, 0x01, 0x75, 0x18}, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}, 6, "server: snapshots without rate limit"},
    // SV_SpawnServer keeps 8 snapshots per client for delta updates with one player, 64 with more; at one
    // snapshot per frame 8 run out before the game acknowledges one, and every update becomes a full one
    // (0x200F58AB: dec edx; neg edx; sbb edx,edx; and edx,38h; add edx,8 -> mov edx,64; the ring is allocated)
    {0xF58AB, {0x4A, 0xF7, 0xDA, 0x1B, 0xD2, 0x83, 0xE2, 0x38, 0x83, 0xC2, 0x08},
     {0xBA, 0x40, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90}, 11, "server: 64 delta snapshots for one player"},
    // SV_SendClientMessages sets client->send_message = 1 every frame only for NA_LOOPBACK (1), not 127.0.0.1
    // (NA_IP = 3): without it, signon fragments and snapshots wait on the updaterate timer instead of flowing
    // every frame (0x200F3CFA: mov eax,[esi+24h]; mov ebx,1; cmp eax,ebx; jne +0Dh -> nops)
    {0xF3CFA, {0x8B, 0x46, 0x24, 0xBB, 0x01, 0x00, 0x00, 0x00, 0x3B, 0xC3, 0x75, 0x0D},
     {0x8B, 0x46, 0x24, 0xBB, 0x01, 0x00, 0x00, 0x00, 0x3B, 0xC3, 0x90, 0x90}, 12, "server: send signon/snapshots every frame"},
    // NET_SendLong (0x200C08DE): splits packets > 1400 bytes (0x578) into 1390-byte chunks with Sleep(1) per
    // chunk. On 127.0.0.1 loopback, single UDP datagrams up to 64496 bytes (0xFBF0 < recvfrom 0xFBF4) go in
    // one atomic sendto() with zero Sleep(1) stalls and zero split-packet reassembly drops.
    {0xC08DE, {0x81, 0xFB, 0x78, 0x05, 0x00, 0x00},
     {0x81, 0xFB, 0xF0, 0xFB, 0x00, 0x00}, 6, "server: atomic 64KB loopback UDP datagrams"},
};

void tune_engine_sockets() {
    static UINT_PTR s_tuned[2] = {};
    volatile UINT_PTR *socks = (volatile UINT_PTR *)0x2116ADF8;   // engine.dll net_sockets[NS_CLIENT, NS_SERVER]
    if (socks[0] == s_tuned[0] && socks[1] == s_tuned[1]) return;
    HMODULE ws = GetModuleHandleW(L"ws2_32.dll");
    if (!ws) return;
    auto setopt = (int(WINAPI *)(UINT_PTR, int, int, const char *, int))GetProcAddress(ws, "setsockopt");
    if (!setopt) return;
    int buf = 1 << 20;   // 1 MB SO_RCVBUF / SO_SNDBUF (default 64 KB overflows at 144 Hz -> Dropped packets)
    for (int i = 0; i < 2; ++i) {
        UINT_PTR s = socks[i];
        if (s && s != (UINT_PTR)~0 && s != s_tuned[i]) {
            setopt(s, 0xFFFF /*SOL_SOCKET*/, 0x1002 /*SO_RCVBUF*/, (const char *)&buf, sizeof(buf));
            setopt(s, 0xFFFF /*SOL_SOCKET*/, 0x1001 /*SO_SNDBUF*/, (const char *)&buf, sizeof(buf));
            s_tuned[i] = s;
            if (g_settings.verbose) log("%s: UDP socket %d buffers set to 1 MB", server_mode() ? "server" : "split", i);
        } else if (!s) {
            s_tuned[i] = 0;
        }
    }
}

// Single-player behaviour the server's console needs (optional: a failed check only logs).
static const CodePatch kPlayerPatches[] = {
    // Cmd_ExecuteString runs a game DLL command from the console with the player as its "command client"
    // only when a local client exists (cls.state != 0), else with none. The world's own code runs player
    // commands that way (Python's ccmd: disciplines, animations, items): the player, as in single player
    // (0x2002EA8C: test eax,eax; jz -> test eax,eax; nops)
    {0x2EA8C, {0x85, 0xC0, 0x0F, 0x84, 0xB8, 0x00, 0x00, 0x00}, {0x85, 0xC0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90}, 8, "server: console commands as the player"},
    // pause, setpause, unpause, v_setpause do nothing unless the local client has a level (cl.levelname,
    // 0x20315BB0), which a dedicated server never has; the game's menu pauses the world with them
    // (mov al,[0x20315BB0]; test al,al -> or al,1)
    {0x917D5, {0x84, 0xC0, 0x0F, 0x84}, {0x0C, 0x01, 0x0F, 0x84}, 4, "server: pause"},
    {0x91955, {0x84, 0xC0, 0x74, 0x79}, {0x0C, 0x01, 0x74, 0x79}, 4, "server: setpause"},
    {0x919E5, {0x84, 0xC0, 0x0F, 0x84}, {0x0C, 0x01, 0x0F, 0x84}, 4, "server: unpause"},
    {0x918A5, {0x84, 0xC0, 0x74, 0x4B}, {0x0C, 0x01, 0x74, 0x4B}, 4, "server: v_setpause"},
};

// ---- commands of the game process ----
// In single player the world's code runs client.dll's commands (vloadtips, ...) in its own process. Here
// they reach Cmd_ExecuteString's last resort, the console's Python (0x200DAB20), as unknown text. The
// game sends the names of its commands and variables ("memx:gamecmds", split_client.cpp); a line that
// starts with one of them goes back to the game ("memx:cmd" on stdout), where it runs.
static std::unordered_set<std::string> s_game_cmds;   // lower case (under s_cmd_lock)
static size_t add_game_cmds(const std::string &line) {
    size_t i = 13;
    AcquireSRWLockExclusive(&s_cmd_lock);
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') ++i;
        size_t j = line.find(' ', i);
        if (j == std::string::npos) j = line.size();
        if (j > i) {
            std::string n = line.substr(i, j - i);
            for (char &ch : n) ch = (char)tolower((unsigned char)ch);
            s_game_cmds.insert(std::move(n));
        }
        i = j;
    }
    size_t n = s_game_cmds.size();
    ReleaseSRWLockExclusive(&s_cmd_lock);
    return n;
}
typedef int(__cdecl *PyConsole_t)(const char *);
static int __cdecl server_unknown_text(const char *text) {
    if (!text) return ((PyConsole_t)0x200DAB20)(text);
    std::string first;
    for (const char *p = text; *p && *p != ' ' && *p != '\t' && *p != '"' && *p != ';' && *p != '\r' && *p != '\n' && first.size() < 64; ++p)
        first += (char)tolower((unsigned char)*p);
    AcquireSRWLockShared(&s_cmd_lock);
    bool game = !first.empty() && s_game_cmds.find(first) != s_game_cmds.end();
    ReleaseSRWLockShared(&s_cmd_lock);
    if (game && s_stdout) {
        // Never allow embedded CR/LF to inject a second memx: protocol line onto stdout, and stay
        // under engine Cbuf_Execute's 1024-byte stack buffer.
        std::string clean;
        for (const char *p = text; *p && *p != '\r' && *p != '\n' && clean.size() < 980; ++p) clean += *p;
        if (!clean.empty()) {
            std::string line = "memx:cmd " + clean + "\n";
            status_write(line.c_str());
        }
        return 0;
    }
    if (g_settings.verbose) log("server: to Python: %.120s", text);
    return ((PyConsole_t)0x200DAB20)(text);
}

// ---- console output ----
// Con_Printf passes a line to Sys_Printf only under a cvar condition, and a dedicated server's
// Sys_Printf prints with printf (engine.dll's static CRT, to a stdout a GUI process does not have).
// Both are redirected here: every console line goes to server_console.log / game_console.log next to
// the .vtm and, on the server, to stdout when the process was given one (the game reads it).
//
// No thread ever waits on the game here: text for stdout goes into a queue that one writer thread sends
// (a pipe whose reader is busy or gone blocks WriteFile, and the world's main thread must never wait on
// that). The locks below are held only to copy text, never across a call that can block or print.
static SRWLOCK s_out_lock = SRWLOCK_INIT;     // s_out_queue, s_file_queue, s_stdout_at_bol, s_out_dropped
static SRWLOCK s_file_lock = SRWLOCK_INIT;    // the console file
static HANDLE s_console_file = INVALID_HANDLE_VALUE;
static bool s_console_tried = false;
static std::string s_out_queue;               // text for stdout, in order
static std::string s_file_queue;              // server console text batched for server_console.log
static bool s_stdout_at_bol = true;           // the queue ends at a line start: memx: lines begin on a fresh line
static bool s_out_dropped = false;            // console text was left out (the game did not read for a while)
static HANDLE s_out_event;                    // the writer thread has text to send
static const size_t kMaxConsoleQueue = 1 << 20;   // console text waits at most this much; status lines always go
static __declspec(thread) bool s_in_tap;

static void write_console_file(const char *data, size_t n) {
    if (!n) return;
    DWORD done;
    AcquireSRWLockExclusive(&s_file_lock);
    if (!s_console_tried) {
        s_console_tried = true;
        wchar_t path[MAX_PATH + 64];
        _snwprintf_s(path, _countof(path), _TRUNCATE, L"%s%s", g_dir, server_mode() ? L"server_console.log" : L"game_console.log");
        s_console_file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, 0, nullptr);
    }
    if (s_console_file != INVALID_HANDLE_VALUE) WriteFile(s_console_file, data, (DWORD)n, &done, nullptr);
    ReleaseSRWLockExclusive(&s_file_lock);
}

static DWORD WINAPI stdout_thread(void *) {
    std::string out, flog;
    for (;;) {
        WaitForSingleObject(s_out_event, 1000);
        AcquireSRWLockExclusive(&s_out_lock);
        out.swap(s_out_queue);
        flog.swap(s_file_queue);
        ReleaseSRWLockExclusive(&s_out_lock);
        if (!flog.empty()) {
            write_console_file(flog.data(), flog.size());
            flog.clear();
        }
        size_t off = 0;
        while (off < out.size() && s_stdout && !s_stdout_broken) {
            DWORD done = 0;
            if (!WriteFile(s_stdout, out.data() + off, (DWORD)(out.size() - off), &done, nullptr) || !done) {
                InterlockedExchange(&s_stdout_broken, 1);
                break;
            }
            off += done;
        }
        out.clear();
    }
}

static void ensure_out_thread() {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending = FALSE;
    if (InitOnceBeginInitialize(&once, 0, &pending, nullptr) && pending) {
        s_out_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE t = s_out_event ? CreateThread(nullptr, 0, stdout_thread, nullptr, 0, nullptr) : nullptr;
        if (t) CloseHandle(t);
        else {
            if (s_out_event) { CloseHandle(s_out_event); s_out_event = nullptr; }
            InterlockedExchange(&s_stdout_broken, 1);
        }
        InitOnceComplete(&once, 0, nullptr);
    }
}

// Appends text to the stdout queue (and optionally server_console.log batch); a status line (memx:...)
// always starts a line and is never dropped.
static bool queue_stdout(const char *text, size_t n, bool status, bool also_file = false) {
    if (!text || !n) return false;
    bool want_stdout = s_stdout && !s_stdout_broken;
    if (!want_stdout && !also_file) return false;
    ensure_out_thread();
    if (!s_out_event) return false;
    bool queued = true;
    bool wake = false;
    AcquireSRWLockExclusive(&s_out_lock);
    bool was_empty = s_out_queue.empty() && s_file_queue.empty();
    if (also_file && s_file_queue.size() + n <= kMaxConsoleQueue) {
        s_file_queue.append(text, n);
        wake = was_empty;
    }
    if (want_stdout) {
        if (status) {
            if (!s_stdout_at_bol) s_out_queue += '\n';
            s_out_queue.append(text, n);
            s_stdout_at_bol = text[n - 1] == '\n';
            wake = was_empty;
        } else if (s_out_queue.size() + n <= kMaxConsoleQueue) {
            if (s_out_dropped) {
                if (!s_stdout_at_bol) s_out_queue += '\n';
                s_out_queue += "(Memory Expansion: some server console text was left out)\n";
                s_out_dropped = false;
            }
            s_out_queue.append(text, n);
            s_stdout_at_bol = text[n - 1] == '\n';
            wake = was_empty;
        } else {
            s_out_dropped = true;
            queued = false;
        }
    } else if (status) {
        queued = false;
    }
    ReleaseSRWLockExclusive(&s_out_lock);
    if (wake) SetEvent(s_out_event);
    return queued;
}

static void console_write(const char *text) {
    if (!text || !*text) return;
    size_t n = strlen(text);
    if (server_mode()) {
        queue_stdout(text, n, false, true);
        return;
    }
    write_console_file(text, n);
}
// stdout only (the game reads it): status lines. Always starts at column 0 so partial console text
// without a trailing newline can never swallow a memx:* line.
static bool status_write(const char *text) {
    if (!text || !*text) return false;
    return queue_stdout(text, strlen(text), true);
}
static int __cdecl server_print(const char *text) {   // Sys_Printf's printf
    if (!text) return 0;
    if (!s_in_tap) console_write(text);
    return (int)strlen(text);
}
// Sys_Error (engine.dll 0x200FA2B0) in a dedicated server prints its message with printf("%s\n", text)
// (0x200FA31A), which reaches no console, and then crashes on the missing game window (0x200FA336) before
// its message box: without this the reason a server process ended is lost.
static int __cdecl server_fatal_print(const char *, const char *text) {
    if (!text) return 0;
    log("server: engine error: %s", text);
    console_write("Engine error: ");
    console_write(text);
    console_write("\n");
    return (int)strlen(text);
}
typedef void(__cdecl *SysPrintf_t)(const char *fmt, ...);
static SysPrintf_t s_sys_printf;   // engine.dll 0x200FA380
static void __cdecl console_tap(const char *fmt, const char *text) {   // Con_Printf's Sys_Printf("%s", text)
    bool prev = s_in_tap;
    if (text && !prev) console_write(text);
    s_in_tap = true;
    s_sys_printf(fmt, text);
    s_in_tap = prev;
}

// ---- when the world runs ----
// SV_Frame runs the game only when, for a one-player server, the local client is in game
// (cls.state == 4); a dedicated server (cls.state == 0) would run it at once, before any player
// exists, and the game's scripts and AI expect one. The check calls this instead: run when the local
// client is in game, or on the dedicated server when a connected client has sent "begin".
static int __cdecl world_should_run() {
    int cls_state = *(volatile int *)0x201DBE50;
    if (cls_state == 4) return 1;
    if (cls_state != 0) return 0;
    int n = *(volatile int *)0x212AF1C8;          // svs.maxclients
    BYTE *clients = *(BYTE *volatile *)0x212AF1D8; // svs.clients (client_t is 205008 bytes)
    for (int i = 0; clients && i < n && i < 64; ++i)
        if (*(volatile int *)(clients + i * 205008 + 12)) return 1;   // client_t::active
    return 0;
}

// world_should_run for code that keeps values in ecx/edx across the test
static __declspec(naked) int world_should_run_saved() {
    __asm {
        push ecx
        push edx
        call world_should_run
        pop edx
        pop ecx
        ret
    }
}

static CodePatch make_gate_patch(HMODULE engine, DWORD rva, DWORD run_rva, DWORD skip_rva, BYTE skip_disp, const char *what) {
    CodePatch p = {rva, {0xA1, 0x50, 0xBE, 0x1D, 0x20, 0x83, 0xF8, 0x04, 0x74, 0x04, 0x85, 0xC0, 0x75, skip_disp},
                   {0xE8, 0, 0, 0, 0, 0x85, 0xC0, 0x75, (BYTE)(run_rva - (rva + 9)), 0xEB, (BYTE)(skip_rva - (rva + 11)), 0x90, 0x90, 0x90},
                   14, what};
    DWORD rel = (DWORD)((uintptr_t)world_should_run_saved - ((uintptr_t)engine + rva + 5));
    memcpy(p.repl + 1, &rel, 4);
    return p;
}

// True while the dedicated server has a client in signon (connected or spawned, not yet active).
// Used to bypass fps_max throttling and timer waits while signon fragments are actively flowing.
static bool client_signing_on(int *seq_out) {
    if (!*(volatile int *)0x212B0754) return false;   // sv.active
    int n = *(volatile int *)0x212AF1C8;              // svs.maxclients
    BYTE *clients = *(BYTE *volatile *)0x212AF1D8;    // svs.clients (client_t is 205008 bytes)
    int seq = 0;
    bool signing = false;
    for (int i = 0; clients && i < n && i < 64; ++i) {
        BYTE *c = clients + i * 205008;
        int active = *(volatile int *)(c + 12);
        int spawned = *(volatile int *)(c + 16);
        int connected = *(volatile int *)(c + 20);
        if ((connected || spawned) && !active) {
            signing = true;
            // netchan starts at +0x20: incoming_acknowledged (+0x34 -> +0x54), last_reliable_sequence (+0x48 -> +0x68)
            seq += *(volatile int *)(c + 0x54) + *(volatile int *)(c + 0x68);
        }
    }
    if (seq_out) *seq_out = seq;
    return signing;
}

// ---- defer player usercmds until the client has completed its first full entity snapshot ----
// SV_ProcessUsercmds (engine.dll+0xF9990) executes CBasePlayer::ProcessUsercmds -> PostThink as soon as
// host_client->active || host_client->spawned. Over UDP, the client sends clc_move during signon2 (when
// client->spawned == 1, client->active == 0) before the first svc_packetentities snapshot creates client
// entities. On save loads and level transitions, CBasePlayer::OnRestore sets a 10-PostThink countdown to
// resend CItemGKeyring's keys via reliable svc_entitymessage ('z'); if those 10 usercmds run before the
// client finishes its initial full update (including any retry forced at engine+0xF2C19..F2C4B when the
// first parse takes > 1 server frame), CL_Parse_EntityMessage drops every key (GetClientNetworkable == NULL)
// or the retry full update destroys/recreates C_ItemGKeyring empty, crashing client.dll+0x38b9 on the next
// key removal + pickup. Keep ignore=1 in SV_ProcessUsercmds until the client is active AND its initial full
// update is acknowledged in the exact way SV_ReadPackets checks it, with a 5 s safety fallback.
static int s_snapshot_acked_spawn = -1;
static int s_active_seen_spawn = -1;
static DWORD s_active_seen_tick = 0;

static void *__cdecl split_clc_delta(void *cl) {
    void *r = ((void *(__cdecl *)(void *))0x200F9900)(cl);
    if (cl && *(volatile int *)((BYTE *)cl + 12)) {
        int delta = *(volatile int *)((BYTE *)cl + 0x1FE04);
        int ack = *(volatile int *)((BYTE *)cl + 0x54);
        int pending = *(volatile int *)((BYTE *)cl + 0x1FE10);
        BYTE resend = *(volatile BYTE *)((BYTE *)cl + 0x1FE14);
        if (delta > 0 && ((pending == -1 && !resend) || (ack == pending && delta == (pending & 0xFFF)))) {
            s_snapshot_acked_spawn = *(volatile int *)0x212AF1D0;
        }
    }
    return r;
}

static int __cdecl split_usercmds_ready() {
    BYTE *cl = *(BYTE *volatile *)0x20B42B74;   // host_client
    if (!cl || !*(volatile int *)(cl + 12)) {   // !client->active
        s_snapshot_acked_spawn = -1;
        s_active_seen_spawn = -1;
        return 0;
    }
    int spawn = *(volatile int *)0x212AF1D0;    // svs.spawncount
    if (s_snapshot_acked_spawn == spawn) return 1;
    DWORD now = GetTickCount();
    if (s_active_seen_spawn != spawn) {
        s_active_seen_spawn = spawn;
        s_active_seen_tick = now ? now : 1;
    } else if (now - s_active_seen_tick >= 5000) {
        log("server: clc_delta timeout after 5 s (spawn %d); allowing usercmds", spawn);
        s_snapshot_acked_spawn = spawn;
        return 1;
    }
    return 0;
}

// ---- reliable dialogue voice and string-table ordering ----
// Two engine.dll 1.2 bugs hit dialogue voices and sounds over 127.0.0.1 UDP:
// 1. SV_SendClientDatagrams (0x200F3F25) skips SV_WriteClientdataToMessage (and thus SV_UpdateStringTables)
//    while a full entity update is awaiting ACK (client+0x1FE10 != -1, e.g. right after connect or after
//    "Update is too old"), yet still appends and clears client->datagram (which holds svc_sound). A newly
//    precached sound then arrives on the client before its soundprecache string-table entry.
// 2. NPC dialogue speech (vampire.dll 0x102C0520, CHAN_VOICE = 5) uses an unreliable CPASAttenuationFilter
//    (unlike DialogCtrl subtitles, which call MakeReliable()). Any dropped or overflowed datagram loses the
//    voice and lip sync while subtitles and animations continue.
struct BfWrite {
    BYTE *data;      // +0x00
    int data_bytes;  // +0x04
    int data_bits;   // +0x08
    int cur_bit;     // +0x0C
    BYTE overflow;   // +0x10
    BYTE assert_ovf; // +0x11
    BYTE pad[2];     // +0x12
    const char *dbg; // +0x14
};
struct IRecipientFilter {
    virtual bool IsInitMessage() const = 0;
    virtual bool IsReliable() const = 0;
    virtual int GetRecipientCount() const = 0;
    virtual int GetRecipientIndex(int slot) const = 0;
};
struct ReliableVoiceFilter : IRecipientFilter {
    const IRecipientFilter *inner;
    explicit ReliableVoiceFilter(const IRecipientFilter *f) : inner(f) {}
    bool IsInitMessage() const override { return inner->IsInitMessage(); }
    bool IsReliable() const override { return true; }
    int GetRecipientCount() const override { return inner->GetRecipientCount(); }
    int GetRecipientIndex(int slot) const override { return inner->GetRecipientIndex(slot); }
};
typedef void(__cdecl *UpdateStringTables_t)(void *client, BfWrite *msg);
typedef void(__fastcall *BfStartWriting_t)(BfWrite *self, void *, void *data, int nBytes, int iStartBit, int nBits);
typedef int(__fastcall *BfWriteBits_t)(BfWrite *self, void *, const void *data, int nBits);
typedef int(__cdecl *SvStartSound_t)(IRecipientFilter *filter, void *edict, int channel, const char *sample,
                                     float volume, int soundlevel, int flags, int pitch, float *origin, int speaker);

// SV_SendClientDatagrams's call to bf_write::WriteBits(client->datagram) at 0x200F3FA3:
// if msg is still empty (cur_bit == 0), SV_WriteClientdataToMessage was skipped for this packet while a full
// update is in flight; flush pending string-table updates into msg first so svc_sound indices exist on client.
static int __fastcall split_append_datagram(BfWrite *msg, void *, const void *data, int bits) {
    if (bits > 0 && msg && msg->cur_bit == 0) {
        int n = *(volatile int *)0x212AF1C8;
        BYTE *clients = *(BYTE *volatile *)0x212AF1D8;
        for (int i = 0; clients && i < n && i < 64; ++i) {
            BYTE *c = clients + i * 205008;
            if (*(const void **)(c + 0x1FE20) == data) {
                ((UpdateStringTables_t)0x200C5620)(c, msg);
                break;
            }
        }
    }
    return ((BfWriteBits_t)0x20005190)(msg, nullptr, data, bits);
}

// CEngineSoundServer::EmitSoundInternal's call to SV_StartSound at 0x200019E1:
// send CHAN_VOICE (channel == 5, NPC dialogue & voice) over the reliable channel, preceded by any pending
// string-table updates (tested in a scratch buffer first so reliable netchan.message can never overflow).
static int __cdecl split_start_sound(IRecipientFilter *filter, void *edict, int channel, const char *sample,
                                     float volume, int soundlevel, int flags, int pitch, float *origin, int speaker) {
    if (channel == 5 && filter && !filter->IsReliable()) {
        int maxclients = *(volatile int *)0x212AF1C8;
        BYTE *clients = *(BYTE *volatile *)0x212AF1D8;
        int count = filter->GetRecipientCount();
        bool can_reliable = clients && count > 0;
        BYTE scratch[4096];
        // maxplayers is 1 in split mode; handle the single recipient safely without heap allocation.
        if (can_reliable && count <= 1) {
            int slot = filter->GetRecipientIndex(0);
            if (slot >= 1 && slot <= maxclients) {
                BYTE *c = clients + (slot - 1) * 205008;
                if (*(volatile int *)(c + 12)) {
                    BfWrite *rel = (BfWrite *)(c + 0x74);
                    BfWrite tmp = {};
                    ((BfStartWriting_t)0x20004CC0)(&tmp, nullptr, scratch, sizeof(scratch), 0, -1);
                    ((UpdateStringTables_t)0x200C5620)(c, &tmp);
                    int free_bits = rel->data_bits - rel->cur_bit;
                    if (!rel->overflow && rel->data_bits > 0 && !tmp.overflow && tmp.cur_bit + 1024 < free_bits) {
                        if (tmp.cur_bit > 0) ((BfWriteBits_t)0x20005190)(rel, nullptr, scratch, tmp.cur_bit);
                        ReliableVoiceFilter reliable(filter);
                        return ((SvStartSound_t)0x200EF730)(&reliable, edict, channel, sample, volume, soundlevel, flags, pitch, origin, speaker);
                    }
                }
            }
        }
    }
    return ((SvStartSound_t)0x200EF730)(filter, edict, channel, sample, volume, soundlevel, flags, pitch, origin, speaker);
}

// SV_SpawnServer's call to 0x2003DAD0 (at 0x200F5A1F), right after sv.name (0x212B0768) is set and
// seconds before sound_precache/ServerActivate finish: tell the game which BSP is loading so it can
// preload maps/<name>.bsp in parallel while the server loads the world.
static int __cdecl server_spawn_notify(int maxclients) {
    reset_server_pause_state();
    const char *name = (const char *)0x212B0768;
    size_t len = strnlen(name, 64);
    if (s_stdout && len > 0 && len < 64 && !strchr(name, ' ') && !strchr(name, '\n') && !strchr(name, '\r')) {
        char line[160];
        snprintf(line, sizeof(line), "memx:loading maps/%s.bsp\n", name);
        status_write(line);
    }
    return ((int(__cdecl *)(int))0x2003DAD0)(maxclients);
}

// SV_SendClientMessages's call to Netchan_CanPacket (0x200F3E15 -> 0x200BC5A0):
// CL_UPDATE_BACKUP is 64 snapshots (~0.44 s at 144 Hz). When the client hits a load or shader hitch
// right after a level change, blasting 144 snapshots/s overflows the 64-slot delta ring and triggers
// "WARNING: CL_FlushEntityPacket, Update is too old" (red HUD text + entity flush). Once 32 snapshots
// are unacknowledged for an active client, throttle to 1 keepalive per 100 ms until the client ACKs.
static int __cdecl server_can_packet(BYTE *netchan) {
    if (netchan) {
        BYTE *client = netchan - 0x20;
        if (*(volatile int *)(client + 12)) {   // client_t::active
            int out_seq = *(volatile int *)(netchan + 0x40);   // outgoing_sequence
            int in_ack = *(volatile int *)(netchan + 0x34);    // incoming_acknowledged
            if (out_seq - in_ack >= 32) {
                static DWORD s_last_keepalive;
                DWORD now = GetTickCount();
                if (now - s_last_keepalive < 100) return 0;
                s_last_keepalive = now;
            }
        }
    }
    return ((int(__cdecl *)(BYTE *))0x200BC5A0)(netchan);
}

// Host_RunFrame's call to Host_FilterTime (0x2008E499 -> 0x2008BA30): while a client is signing on,
// temporarily uncap fps_max (0.0 skips the limiter in Host_FilterTime) so fragments do not wait 10 ms each.
static int __cdecl server_filter_time(float dt) {
    typedef int(__cdecl *FilterTime_t)(float);
    BYTE *fps = client_signing_on() ? *(BYTE *volatile *)0x20B42884 : nullptr;
    float saved = 0.0f;
    if (fps) {
        saved = *(float *)(fps + 0x28);
        *(float *)(fps + 0x28) = 0.0f;
    }
    int r = ((FilterTime_t)0x2008BA30)(dt);
    if (fps) *(float *)(fps + 0x28) = saved;
    if (r) {
        s_ran_frame = true;
        QueryPerformanceCounter(&s_last_frame_qpc);
    }
    return r;
}

// CVEngineServer (vtable engine.dll+0x186E7C) method 3 asks the client DLL whether its world entity
// exists (vampire.dll calls it before measuring how lit the player is). A dedicated server has no client
// DLL: the world exists for the player once one is in game.
static bool __fastcall engine_client_world_ready(void *) { return world_should_run() != 0; }

// CSaveRestore (vtable engine.dll+0x174B3C): a save (and a level change) writes client state (<map>.HL2:
// static map decals that infodecal removed on first visit, client entities) through the client DLL. Ask
// the game process to write the real .HL2 file and wait briefly for its "memx:saveclient_done" ack on the
// pipe thread. RestoreClientState stays a no-op here; the server sends svc_restore and the game runs it.
static int __fastcall save_client_state_split(void *, void *, const char *fileName) {
    if (!s_stdout || !s_saveclient_done || !fileName || !*fileName || !world_should_run()) return 1;
    if (strchr(fileName, '\n') || strchr(fileName, '\r') || strlen(fileName) > 400) return 1;
    LONG id = InterlockedIncrement(&s_saveclient_seq);
    InterlockedExchange(&s_saveclient_ok, 0);
    ResetEvent(s_saveclient_done);
    char line[512];
    snprintf(line, sizeof(line), "memx:saveclient %ld %s\n", (long)id, fileName);
    if (!status_write(line)) return 1;
    const char *step = s_loop_step;
    s_loop_step = "a save: waiting for the game's client state";
    DWORD w = WaitForSingleObject(s_saveclient_done, 2500);
    s_loop_step = step;
    bool ok = (w == WAIT_OBJECT_0 && InterlockedCompareExchange(&s_saveclient_ok, 0, 0) != 0);
    if (!ok || g_settings.verbose)
        log("server: client state %s (id=%ld %s)",
            w != WAIT_OBJECT_0 ? "save timed out" : (ok ? "saved" : "not written by client"), (long)id, fileName);
    return 1;
}
static int __fastcall restore_client_state_none(void *, void *, int, int) { return 1; }     // slot 17

// call rel32 at engine+rva, from the expected target to fn
bool redirect_call(HMODULE mod, DWORD rva, DWORD expect_target_rva, void *fn, const char *what) {
    if (!in_module_image(mod, rva, 5)) {
        log("not patched (out of image bounds): %s", what);
        return false;
    }
    BYTE *at = (BYTE *)mod + rva;
    DWORD want = (DWORD)((uintptr_t)fn - (uintptr_t)(at + 5));
    DWORD orig = (DWORD)((uintptr_t)mod + expect_target_rva - (uintptr_t)(at + 5));
    if (at[0] == 0xE8 && memcmp(at + 1, &want, 4) == 0) return true;
    if (at[0] != 0xE8 || memcmp(at + 1, &orig, 4) != 0) {
        log("not patched (unknown bytes): %s", what);
        return false;
    }
    DWORD prot;
    if (!VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &prot)) return false;
    memcpy(at + 1, &want, 4);
    VirtualProtect(at, 5, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    return true;
}

// Warning()/Msg()/DevMsg() go through tier0's spew function, not Con_Printf: chain onto the engine's.
typedef int(__cdecl *SpewFn)(int type, const char *msg);
static SpewFn s_engine_spew;
static int __cdecl spew_tap(int type, const char *msg) {
    bool prev = s_in_tap;
    if (msg && !prev) console_write(msg);
    s_in_tap = true;   // the engine's spew function may print through Con_Printf
    int r = s_engine_spew ? s_engine_spew(type, msg) : 0;
    s_in_tap = prev;
    return r;
}
static void install_spew_tap() {
    HMODULE tier0 = GetModuleHandleW(L"tier0.dll");
    if (!tier0) return;
    auto get = (SpewFn(__cdecl *)())GetProcAddress(tier0, "GetSpewOutputFunc");
    auto set = (void(__cdecl *)(SpewFn))GetProcAddress(tier0, "SpewOutputFunc");
    if (!get || !set || get() == spew_tap) return;
    s_engine_spew = get();
    set(spew_tap);
}

// engine.dll 1.2: Con_Printf's "jnz skip Sys_Printf" after its cvar test (0x2003F2D7) -> nop nop
static const CodePatch kConsoleAlways = {0x3F2D7, {0x75, 0x12}, {0x90, 0x90}, 2, "Con_Printf always to Sys_Printf"};

static void install_console_tap(HMODULE engine) {
    if (engine != (HMODULE)0x20000000) return;
    s_sys_printf = (SysPrintf_t)((BYTE *)engine + 0xFA380);
    // the call is checked first so that a failure leaves Con_Printf as it was
    if (redirect_call(engine, 0x3F2E3, 0xFA380, (void *)console_tap, "Con_Printf console copy")) apply_patch(engine, kConsoleAlways);
}

void on_engine_mapped(HMODULE engine) {
    if (server_mode() || g_settings.console_log) install_console_tap(engine);
    if (!server_mode()) {
        if (g_settings.split) install_split_client(engine);
        return;
    }
    if (engine != (HMODULE)0x20000000) {
        log("server: fatal: engine.dll is not at its usual address; exiting server process");
        ExitProcess(1);
    }
    FARPROC cur = GetProcAddress(engine, "CreateInterface");
    if (cur == (FARPROC)my_engine_ci) return;   // this mapping is done
    if (!cur) {
        log("server: fatal: engine.dll has no CreateInterface export; exiting server process");
        ExitProcess(1);
    }

    // Prepare required dynamic patches and verify every required patch/vtable site BEFORE modifying any bytes,
    // so an unknown engine.dll build fails cleanly without leaving engine.dll half-patched or launching a
    // second windowed game process.
    CodePatch gates[3] = {
        make_gate_patch(engine, 0xF638B, 0xF6399, 0xF63A0, 0x07, "SV_Frame waits for a player"),
        make_gate_patch(engine, 0xF51D6, 0xF51E4, 0xF51E8, 0x04, "SV_ActivateServer waits for a player"),
        make_gate_patch(engine, 0xEDE2B, 0xEDE39, 0xEDE3F, 0x06, "server-running test waits for a player"),
    };
    // "save" refuses unless the local client is in game (cmp cls.state,4; jnz refuse at 0x200958DE)
    // -> call world_should_run_saved; test eax,eax; jz refuse
    CodePatch save_patch = {0x958DE, {0x83, 0x3D, 0x50, 0xBE, 0x1D, 0x20, 0x04, 0x75, 0x66},
                            {0xE8, 0, 0, 0, 0, 0x85, 0xC0, 0x74, 0x66}, 9, "save while the player is in game"};
    {
        DWORD rel = (DWORD)((uintptr_t)world_should_run_saved - ((uintptr_t)engine + save_patch.rva + 5));
        memcpy(save_patch.repl + 1, &rel, 4);
    }
    for (const CodePatch &p : kEnginePatches)
        if (!patch_matches(engine, p)) { log("server: fatal: not patched (unknown bytes): %s; exiting server process", p.what); ExitProcess(1); }
    for (const CodePatch &g : gates)
        if (!patch_matches(engine, g)) { log("server: fatal: not patched (unknown bytes): %s; exiting server process", g.what); ExitProcess(1); }
    if (!patch_matches(engine, save_patch)) { log("server: fatal: not patched (unknown bytes): %s; exiting server process", save_patch.what); ExitProcess(1); }
    void **vt_eng = (void **)0x20186E7C;
    if (!in_module_image(engine, 0x186E7C, 16) ||
        (vt_eng[3] != (void *)0x201089F0 && vt_eng[3] != (void *)engine_client_world_ready)) {
        log("server: fatal: not patched (unknown vtable): engine client-world test; exiting server process");
        ExitProcess(1);
    }
    void **vt_sr = (void **)0x20174B3C;
    bool sr_ok = in_module_image(engine, 0x174B3C, 72) &&
                 (vt_sr[14] == (void *)0x20096D10 || vt_sr[14] == (void *)save_client_state_split) &&
                 (vt_sr[17] == (void *)0x20096930 || vt_sr[17] == (void *)restore_client_state_none);
    if (!sr_ok) {
        log("server: fatal: not patched (unknown vtable): save client state; exiting server process");
        ExitProcess(1);
    }

    // Hook CreateInterface first: if the export table cannot be patched, exit cleanly instead of opening a game window.
    void *old = nullptr;
    if (!patch_export(engine, "CreateInterface", (void *)my_engine_ci, &old)) {
        log("server: fatal: could not hook engine.dll CreateInterface; exiting server process");
        ExitProcess(1);
    }
    s_engine_ci = (CreateInterfaceFn)old;

    for (const CodePatch &p : kEnginePatches) apply_patch(engine, p);
    for (const CodePatch &g : gates) apply_patch(engine, g);
    apply_patch(engine, save_patch);
    patch_vtable(vt_eng, 3, (void *)engine_client_world_ready);
    patch_vtable(vt_sr, 14, (void *)save_client_state_split);
    patch_vtable(vt_sr, 17, (void *)restore_client_state_none);

    // Optional smoothness / gameplay / fast-load patches (a failed check only logs).
    for (const CodePatch &p : kSmoothPatches) apply_patch(engine, p);
    for (const CodePatch &p : kPlayerPatches) apply_patch(engine, p);
    // Cmd_ExecuteString's call of the console's Python (0x2002EC13): commands of the game process go there
    redirect_call(engine, 0x2EC13, 0xDAB20, (void *)server_unknown_text, "server: game commands back to the game");
    // Host_RunFrame's call to Host_FilterTime (0x2008E499): verify fps_max read at 0x8BA5F before hooking
    static const BYTE kFpsMaxRead[] = {0x8B, 0x0D, 0x84, 0x28, 0xB4, 0x20, 0x8B, 0x51, 0x28};
    if (in_module_image(engine, 0x8BA5F, sizeof(kFpsMaxRead)) &&
        memcmp((BYTE *)engine + 0x8BA5F, kFpsMaxRead, sizeof(kFpsMaxRead)) == 0)
        redirect_call(engine, 0x8E499, 0x8BA30, (void *)server_filter_time, "server: fast signon frames");
    // SV_SpawnServer (0x200F5A1F): notify the game as soon as sv.name is known so it can preload the BSP
    redirect_call(engine, 0xF5A1F, 0x3DAD0, (void *)server_spawn_notify, "server: notify map preload");
    // SV_SendClientMessages (0x200F3E15): throttle snapshots when >= 32 are unacked (prevents CL_FlushEntityPacket)
    redirect_call(engine, 0xF3E15, 0xBC5A0, (void *)server_can_packet, "server: throttle unacked snapshots");
    // Sound & string-table ordering: ensure SV_UpdateStringTables precedes datagram sounds during full updates,
    // and deliver CHAN_VOICE (dialogue) reliably after any pending string-table updates.
    static const BYTE kUpdateStrTablesBytes[] = {0x51, 0xA1, 0x80, 0xB6, 0x16, 0x21, 0x57, 0x33, 0xFF};
    static const BYTE kBfStartWritingBytes[] = {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x54, 0x24, 0x10};
    if (in_module_image(engine, 0xC5620, sizeof(kUpdateStrTablesBytes)) &&
        in_module_image(engine, 0x4CC0, sizeof(kBfStartWritingBytes)) &&
        memcmp((BYTE *)engine + 0xC5620, kUpdateStrTablesBytes, sizeof(kUpdateStrTablesBytes)) == 0 &&
        memcmp((BYTE *)engine + 0x4CC0, kBfStartWritingBytes, sizeof(kBfStartWritingBytes)) == 0) {
        redirect_call(engine, 0xF3FA3, 0x5190, (void *)split_append_datagram, "server: string tables before datagram sounds");
        redirect_call(engine, 0x19E1, 0xEF730, (void *)split_start_sound, "server: reliable dialogue voice");
    }
    // Defer SV_ProcessUsercmds (engine+0xF9AA0) until the client is active and has completed its initial
    // full entity update via clc_delta (slot 0x201AC874), so post-restore PostThink entity messages
    // (e.g. CItemGKeyring::ResendKeysToClient) do not fire before client.dll finishes its first snapshot.
    {
        static const BYTE kUsercmdTail[] = {0x8B, 0x48, 0x10, 0x85, 0xC9, 0x74, 0x07};
        CodePatch usercmd_gate = {
            0xF9AA0,
            {0xA1, 0x74, 0x2B, 0xB4, 0x20, 0x8B, 0x48, 0x0C, 0x85, 0xC9, 0x75, 0x07},
            {0xE8, 0, 0, 0, 0, 0x85, 0xC0, 0x74, 0x11, 0xEB, 0x08, 0x90},
            12,
            "server: wait for first client snapshot before usercmds"};
        DWORD rel = (DWORD)((uintptr_t)&split_usercmds_ready - ((uintptr_t)engine + usercmd_gate.rva + 5));
        memcpy(usercmd_gate.repl + 1, &rel, 4);
        void **clc_delta_slot = (void **)0x201AC874;
        if (in_module_image(engine, 0xF9AA0, 19) &&
            in_module_image(engine, 0x1AC874, 4) &&
            memcmp((BYTE *)engine + 0xF9AAC, kUsercmdTail, sizeof(kUsercmdTail)) == 0 &&
            patch_matches(engine, usercmd_gate) &&
            (*clc_delta_slot == (void *)0x200F9900 || *clc_delta_slot == (void *)split_clc_delta)) {
            s_snapshot_acked_spawn = -1;
            s_active_seen_spawn = -1;
            patch_vtable(clc_delta_slot, 0, (void *)split_clc_delta);
            if (*clc_delta_slot == (void *)split_clc_delta) {
                apply_patch(engine, usercmd_gate);
            } else {
                log("not patched (could not hook clc_delta): %s", usercmd_gate.what);
            }
        } else {
            log("not patched (unknown bytes): %s", usercmd_gate.what);
        }
    }
    // Sys_Printf: call _printf (engine+0x164458) -> server_print
    redirect_call(engine, 0xFA3E3, 0x164458, (void *)server_print, "Sys_Printf output");
    // Sys_Error: call _printf (0x200FA31A) -> server_fatal_print
    redirect_call(engine, 0xFA31A, 0x164458, (void *)server_fatal_print, "Sys_Error message");

    check_load_watch(engine);
    check_server_pause_addrs(engine);
    init_changelevel_guard(engine);
    install_message_box_guard();
    static bool veh;
    if (!veh) veh = AddVectoredExceptionHandler(1, on_exception) != nullptr;
    log("server: engine.dll will run as a dedicated server");
}

} // namespace memx
