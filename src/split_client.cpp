// Process split, game side ([Split] enabled = 1): the game starts its server as a second process
// (Vampire.exe -memx_server, see split_server.cpp) and plays on it over the engine's own network code,
// so the world (vampire.dll, Python, AI, physics) and the game's rendering, sound and UI each have a
// 4 GB address space.
//
// The game's "map", "load" and "save" commands (what New Game, the load menu and the save menu run) go to
// the server process; the game preloads the target BSP on "memx:loading" and connects once "memx:ready"
// arrives. Level changes happen on the server; when the server reports a new level the game reconnects if
// it is not already on its way. If the server process cannot be started, the commands run locally as before.
#include "common.h"
#include "split_player_interp.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace memx {

// ---- engine.dll 1.2 (at its preferred base, checked before anything is installed) ----
static const uintptr_t kCmdSource = 0x20348960;   // 1 = typed / from the command buffer
static const uintptr_t kClsState = 0x201DBE50;    // 0 none, 1 disconnected, 2 connecting, 3 connected, 4 in game
static const uintptr_t kSvActive = 0x212B0754;    // sv.active: this process runs a server of its own
typedef int(__cdecl *CmdArgc_t)();
typedef const char *(__cdecl *CmdArgv_t)(int);
typedef void(__cdecl *CbufAddText_t)(const char *);
typedef void(__cdecl *VoidFn_t)();
typedef void(__cdecl *ConPrintf_t)(const char *fmt, ...);
static const CmdArgc_t Cmd_Argc = (CmdArgc_t)0x2002E8E0;
static const CmdArgv_t Cmd_Argv = (CmdArgv_t)0x2002E8F0;
static const CbufAddText_t Cbuf_AddText = (CbufAddText_t)0x2002DCC0;
static const VoidFn_t Cbuf_Execute = (VoidFn_t)0x2002DF10;
static const ConPrintf_t Con_Printf = (ConPrintf_t)0x2003F290;

// ConCommand objects keep their callback at +0x1C
struct CommandHook {
    uintptr_t callback_field;
    uintptr_t original;
    VoidFn_t replacement;
    const char *name;
};
static void split_map();
static void split_load();
static void split_save();
static void split_disconnect();
static void split_changelevel();
static void split_reload();
static void split_restart();
static void split_v_setpause();
static void split_v_unpause();
static CommandHook s_commands[] = {
    {0x20B437F8, 0x20090F60, split_map, "map"},
    {0x20B438DC, 0x200991A0, split_load, "load"},
    {0x20B438B4, 0x20098E40, split_save, "save"},
    {0x20B43434, 0x200911C0, split_disconnect, "disconnect"},
    {0x20B433E4, 0x20091060, split_changelevel, "changelevel"},
    {0x20B4340C, 0x20090FB0, split_reload, "reload"},
    {0x20B4345C, 0x20090F80, split_restart, "restart"},
    // Bloodlines' own pause pair (client.dll's interface screens): pauses the world of this process only
    {0x20B432F4, 0x200918A0, split_v_setpause, "v_setpause"},
    {0x20B432CC, 0x20091900, split_v_unpause, "v_unpause"},
};

// ---- the server process ----
// Per-server-generation pipe/thread state so an old dying server's threads can never steal commands
// from or corrupt status for a newly restarted server process.
struct ServerConn {
    LONG gen;
    HANDLE proc;               // SYNCHRONIZE handle owned by server_input_thread
    HANDLE out_read;           // stdout pipe read end owned by server_output_thread
    char pipe_name[128];
    HANDLE send_event;
    CRITICAL_SECTION cs;
    std::vector<std::string> queue;
    volatile LONG ref;
};

static void conn_release(ServerConn *c) {
    if (!c) return;
    if (InterlockedDecrement(&c->ref) == 0) {
        if (c->send_event) CloseHandle(c->send_event);
        DeleteCriticalSection(&c->cs);
        delete c;
    }
}

static HANDLE volatile s_server;
static HANDLE s_job;
static ServerConn *s_conn;              // protected by s_cs
static volatile LONG s_active_gen;      // generation of the current server process
static int s_port;
static volatile LONG s_server_spawns;   // "memx:ready" lines seen (one per level the server starts)
static LONG s_spawns_seen;
static bool s_want_game;                // the player started or loaded a game on the server
static volatile bool s_frame_seen;      // main engine thread has entered Host_RunFrame

static CRITICAL_SECTION s_cs;
static INIT_ONCE s_cs_once = INIT_ONCE_STATIC_INIT;
static void ensure_cs() {
    BOOL pending = FALSE;
    if (InitOnceBeginInitialize(&s_cs_once, 0, &pending, nullptr) && pending) {
        InitializeCriticalSectionAndSpinCount(&s_cs, 4000);
        InitOnceComplete(&s_cs_once, 0, nullptr);
    }
}
static std::atomic<uint32_t> s_pending_work{0}; // bit 0: console, 1: cmds, 2: saveclient, 3: preload
static std::atomic<bool> s_server_dead{false};  // set by server_input_thread when active server exits
// Level commands sent to the server are numbered ("memx:loadid"), so that a late report about an earlier
// one ("memx:loadfailed <number>") can never end the loading screen of the one the player waits for.
static long s_load_id;                          // the last level command's number (main thread)
static bool s_load_expects_level;               // it put the loading screen up (map, load)
static std::atomic<long> s_load_failed_id{0};   // the server says this command did not start a level
static std::vector<std::string> s_to_console;   // server output for the game's console
static std::vector<std::string> s_from_server;  // commands the server's code ran that exist only in this process
static std::string s_save_client_req;           // .HL2 path the server asked this process to write
static long s_save_client_id;                   // sequence ID for s_save_client_req
static std::string s_preload_req;               // maps/<name>.bsp the server just started loading
static std::string s_preloaded_map;             // BSP preloaded into the client's hunk; skip next Host_FreeToLowMark
static bool s_sent_commands;                    // this server process has the list of this process's commands
static int s_sent_names;                        // ... of this many (the engine's list grows at the first level)
static BYTE *s_sent_cmd_head;                   // head of kCommandList at last send_command_names()
static std::unordered_set<std::string> s_sent_cmd_set; // command names already sent to this server
static bool s_ready_for_connect;                // memx:ready arrived; uncap connect frames until cls_state != 2
static bool s_preload_ok;                       // split_free_world hook verified and installed
static volatile bool s_split_fallback;          // permanent fallback to single player after restart limit
static int s_restarts;                          // server restarts since last long healthy period
static DWORD s_healthy_since;                   // GetTickCount() when current healthy server play began
static HMODULE s_interp_client;                 // client.dll already patched for local/NPC interpolation
static HMODULE s_checked_cl;                    // client.dll already checked for s_pLocalPlayer offset
static bool s_offset_ok;

// Guard C_ItemGKeyring::ReceiveMessage (client.dll+0x37F0, vtable slot +0x1E4488) against underflow
// and duplicate adds: vanilla client.dll unconditionally decrements m_Size (+0x97C) on '{' (remove key)
// even when m_Size == 0 or idx >= m_Size; if m_Size becomes -1 while m_pMemory == NULL, the next 'z'
// (add key) skips allocation (-1 + 1 <= 0) and writes 192 bytes to 0xFFFFFF40 (client.dll+0x38B9 crash).
typedef int(__fastcall *KeyringRecv_t)(void *self, void *, int size, void *data);
static KeyringRecv_t s_keyring_recv_orig;
static int __fastcall split_keyring_receive_message(void *self, void *, int size, void *data) {
    if (self && data && size >= 1) {
        const BYTE *buf = (const BYTE *)data;
        int &count = *(int *)((BYTE *)self + 0x97C);
        if (buf[0] == '{') {
            int idx = (size >= 2) ? (int)buf[1] : 0;
            if (count <= 0 || idx >= count) {
                log("split: ignored out-of-range keyring removal (idx=%d, client count=%d)", idx, count);
                return 0;
            }
        } else if (buf[0] == 'z') {
            if (count < 0) {
                log("split: clamped negative keyring count (%d -> 0) before add", count);
                count = 0;
            }
            const char *mem = *(const char *const *)((const BYTE *)self + 0x970);
            int alloc = *(const int *)((const BYTE *)self + 0x974);
            int n = (count < alloc) ? count : alloc;
            if (mem && n > 0 && size >= 2) {
                char key[96];
                size_t avail = (size_t)(size - 1);
                size_t klen = 0;
                while (klen < avail && klen < sizeof(key) - 1 && buf[1 + klen] != '\0') {
                    key[klen] = (char)buf[1 + klen];
                    ++klen;
                }
                key[klen] = '\0';
                for (int i = 0; i < n; ++i) {
                    if (_strnicmp(mem + (size_t)i * 192, key, 96) == 0) return 0;
                }
            }
        }
    }
    return s_keyring_recv_orig ? s_keyring_recv_orig(self, nullptr, size, data) : 0;
}

// The server's main loop sends "memx:alive" once a second (split_server.cpp). A server process that still
// runs but stops sending it is stuck: the world stands still and the game would show "Connection
// Problem" or its loading screen for good. It is ended, and the game goes back to the menu (client_frame).
static volatile DWORD s_alive_tick;             // GetTickCount() of the last "memx:alive" of the current server
static volatile DWORD s_start_tick;             // GetTickCount() when the current server process was launched
static volatile LONG s_alive_seen;              // the current server has sent one (older builds never do)
static volatile LONG s_server_loading;          // a level is loading on the server (no frames for a while)
static int s_stuck_strikes;                     // consecutive watchdog polls over the limit
static const DWORD kStuckInGameMs = 20000;      // no sign of life while playing
static const DWORD kStuckLoadingMs = 90000;     // ... while the server loads a level (one long frame)
static const DWORD kStuckStartupMs = 120000;    // ... before the first "memx:alive" after launch

static bool server_usable() {
    HANDLE srv = s_server;
    return srv && WaitForSingleObject(srv, 0) == WAIT_TIMEOUT;
}

// milliseconds since the running server last showed its main loop going round (or since launch if none yet)
static DWORD server_quiet_ms() {
    if (!server_usable()) return 0;
    DWORD tick = s_alive_seen ? s_alive_tick : s_start_tick;
    if (!tick) return 0;
    DWORD now = GetTickCount();
    return (int)(now - tick) > 0 ? (now - tick) : 0;
}

static DWORD server_stuck_limit_ms() {
    if (!s_alive_seen) return kStuckStartupMs;
    return s_server_loading ? kStuckLoadingMs : kStuckInGameMs;
}

// Ends a server process whose main loop stopped; the next New Game or Load starts a new one.
static void end_stuck_server(DWORD quiet) {
    HANDLE srv = s_server;
    if (!srv) return;
    log("split: the server process has not run its main loop for %lu s; ending it", quiet / 1000);
    Con_Printf("Memory Expansion: the server process stopped responding (%lu s) and was ended. Load a save or start a new game.\n",
               quiet / 1000);
    InterlockedExchange(&s_alive_seen, 0);
    TerminateProcess(srv, 1);
    WaitForSingleObject(srv, 3000);
}

// a UDP port nothing on this PC has bound (ws2_32 is loaded on demand: the .vtm does not import it)
static bool udp_port_free(int port) {
    HMODULE ws = LoadLibraryW(L"ws2_32.dll");
    if (!ws) return true;
    auto wsa_startup = (int(WINAPI *)(WORD, void *))GetProcAddress(ws, "WSAStartup");
    auto wsa_cleanup = (int(WINAPI *)())GetProcAddress(ws, "WSACleanup");
    auto sock = (UINT_PTR(WINAPI *)(int, int, int))GetProcAddress(ws, "socket");
    auto bind_ = (int(WINAPI *)(UINT_PTR, const void *, int))GetProcAddress(ws, "bind");
    auto close = (int(WINAPI *)(UINT_PTR))GetProcAddress(ws, "closesocket");
    if (!wsa_startup || !sock || !bind_ || !close) {
        FreeLibrary(ws);
        return true;
    }
    BYTE wsadata[512];
    bool wsa_ok = wsa_startup(0x0202, wsadata) == 0;
    bool ok = true;
    if (wsa_ok) {
        UINT_PTR s = sock(2 /*AF_INET*/, 2 /*SOCK_DGRAM*/, 17 /*IPPROTO_UDP*/);
        if (s != (UINT_PTR)~0) {
            struct { short family; unsigned short port; unsigned long addr; char zero[8]; } a =   // sockaddr_in, 127.0.0.1
                {2, (unsigned short)(((port >> 8) & 0xFF) | ((port & 0xFF) << 8)), 0x0100007FUL, {}};
            ok = bind_(s, &a, sizeof(a)) == 0;
            close(s);
        }
        if (wsa_cleanup) wsa_cleanup();
    }
    FreeLibrary(ws);
    return ok;
}

// the game's -game argument, for the server (quote-aware, case-insensitive, standalone token)
static void game_dir_arg(char *out, size_t n) {
    if (!out || !n) return;
    out[0] = 0;
    const char *p = GetCommandLineA();
    if (!p) return;
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        const char *tok_start = p;
        bool in_quotes = false;
        while (*p && (in_quotes || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') in_quotes = !in_quotes;
            ++p;
        }
        size_t tok_len = (size_t)(p - tok_start);
        if (tok_len == 5 && _strnicmp(tok_start, "-game", 5) == 0) {
            while (*p == ' ' || *p == '\t') ++p;
            bool quoted = (*p == '"');
            if (quoted) ++p;
            size_t i = 0;
            while (*p && i + 1 < n && (quoted ? *p != '"' : (*p != ' ' && *p != '\t'))) out[i++] = *p++;
            out[i] = 0;
            return;
        }
    }
}

static void handle_server_line(const ServerConn *conn, const std::string &line, bool allow_protocol) {
    if (line.empty()) return;
    if (allow_protocol && line.rfind("memx:", 0) == 0) {
        if (conn->gen != s_active_gen) return;   // ignore late status from a replaced server
        if (line == "memx:alive") {
            s_alive_tick = GetTickCount();
            InterlockedExchange(&s_alive_seen, 1);
        } else if (line.rfind("memx:loadfailed ", 0) == 0) {
            s_load_failed_id.store(atol(line.c_str() + 16), std::memory_order_release);
            log("split: server: %s", line.c_str());
        } else if (line.rfind("memx:ready", 0) == 0) {
            InterlockedExchange(&s_server_loading, 0);
            InterlockedIncrement(&s_server_spawns);
            log("split: server: %s", line.c_str());
        } else if (line.rfind("memx:loading ", 0) == 0) {
            InterlockedExchange(&s_server_loading, 1);
            EnterCriticalSection(&s_cs);
            s_preload_req = line.substr(13);
            s_pending_work.fetch_or(8u, std::memory_order_release);
            LeaveCriticalSection(&s_cs);
        } else if (line.rfind("memx:saveclient ", 0) == 0) {
            long req_id = 0;
            std::string path;
            const char *p = line.c_str() + 16;
            char *end = nullptr;
            long parsed = strtol(p, &end, 10);
            if (end && end > p && *end == ' ' && end[1] != '\0') {
                req_id = parsed;
                path = end + 1;
            } else {
                path = p;
            }
            EnterCriticalSection(&s_cs);
            s_save_client_id = req_id;
            s_save_client_req = std::move(path);
            s_pending_work.fetch_or(4u, std::memory_order_release);
            LeaveCriticalSection(&s_cs);
        } else if (line.rfind("memx:cmd ", 0) == 0) {
            // a command of this process (client.dll's, an engine client command) that the world's code
            // ran: in single player it ran here
            std::string cmd = line.substr(9);
            if (cmd.size() > 980) {
                log("split: warning: dropped oversized server command (%u bytes)", (unsigned)cmd.size());
                return;
            }
            EnterCriticalSection(&s_cs);
            if (s_from_server.size() < 500) {
                s_from_server.push_back(std::move(cmd));
                s_pending_work.fetch_or(2u, std::memory_order_release);
            } else {
                log("split: warning: client command queue full (500), dropped: %.80s", cmd.c_str());
            }
            LeaveCriticalSection(&s_cs);
        }
        return;
    }
    // Con_Printf passes lines to Sys_Printf (1024-byte _vsnprintf buffer without guaranteed NUL if full):
    // cap forwarded console lines well below 1024 bytes.
    std::string msg = "[server] ";
    if (line.size() > 900) msg.append(line.data(), 900);
    else msg += line;
    msg += '\n';
    EnterCriticalSection(&s_cs);
    if (s_to_console.size() < 500) {
        s_to_console.push_back(std::move(msg));
        s_pending_work.fetch_or(1u, std::memory_order_release);
    }
    LeaveCriticalSection(&s_cs);
}

// The server's stdout: its console (copied to the game's console) and "memx:" status lines.
static DWORD WINAPI server_output_thread(void *arg) {
    auto *conn = (ServerConn *)arg;
    HANDLE pipe = conn->out_read;
    std::string line;
    bool truncated = false;
    char buf[4096];
    DWORD got;
    while (ReadFile(pipe, buf, sizeof(buf), &got, nullptr) && got) {
        for (DWORD i = 0; i < got; ++i) {
            if (buf[i] == '\0') continue;
            if (buf[i] != '\n') {
                if (buf[i] != '\r') {
                    if (line.size() < 2000) line += buf[i];
                    else truncated = true;
                }
                continue;
            }
            handle_server_line(conn, line, !truncated);
            line.clear();
            truncated = false;
        }
    }
    if (!line.empty()) handle_server_line(conn, line, false);
    CloseHandle(pipe);
    log("split: the server process's output has ended");
    conn_release(conn);
    return 0;
}

// Commands go out in order on one pipe connection, reopened if the server closes its end. The thread
// belongs to one server process (its own ServerConn) and exits immediately when that process ends.
static DWORD WINAPI server_input_thread(void *arg) {
    auto *conn = (ServerConn *)arg;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE wait[2] = {conn->proc, conn->send_event};
    std::vector<std::string> lines;
    size_t next = 0;
    while (WaitForSingleObject(conn->proc, 0) == WAIT_TIMEOUT) {
        if (next == lines.size()) {
            lines.clear();
            next = 0;
            EnterCriticalSection(&conn->cs);
            lines.swap(conn->queue);
            LeaveCriticalSection(&conn->cs);
            if (!lines.empty()) continue;
            DWORD w = WaitForMultipleObjects(2, wait, FALSE, INFINITE);
            if (w != WAIT_OBJECT_0 + 1) break;   // server exited or wait failed
            continue;
        }
        if (pipe == INVALID_HANDLE_VALUE) {
            if (!WaitNamedPipeA(conn->pipe_name, 200)) { Sleep(50); continue; }   // the server is still starting
            pipe = CreateFileA(conn->pipe_name, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) { Sleep(50); continue; }
        }
        std::string l = lines[next] + "\n";
        size_t off = 0;
        bool write_ok = true;
        while (off < l.size()) {
            DWORD done = 0;
            if (!WriteFile(pipe, l.data() + off, (DWORD)(l.size() - off), &done, nullptr) || done == 0) {
                write_ok = false;
                break;
            }
            off += done;
        }
        if (!write_ok) {
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
            continue;
        }
        ++next;
    }
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    CloseHandle(conn->proc);
    if (conn->gen == s_active_gen) s_server_dead.store(true, std::memory_order_release);
    log("split: the server process has ended");
    conn_release(conn);
    return 0;
}

static void to_server(const std::string &line) {
    EnterCriticalSection(&s_cs);
    ServerConn *c = s_conn;
    if (c) {
        EnterCriticalSection(&c->cs);
        c->queue.push_back(line);
        LeaveCriticalSection(&c->cs);
        SetEvent(c->send_event);
    }
    LeaveCriticalSection(&s_cs);
}

static void forget_sent_settings();
static bool start_server() {
    s_sent_commands = false;
    s_sent_names = 0;
    s_sent_cmd_head = nullptr;
    s_sent_cmd_set.clear();
    s_server_dead.store(false, std::memory_order_release);
    forget_sent_settings();   // a new server process has every default again
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    int base_port = g_settings.split_port;
    if (base_port < 1024 || base_port > 65480) base_port = 27115;
    int chosen_port = -1;
    for (int p = base_port; p < base_port + 50; ++p) {
        if (udp_port_free(p)) { chosen_port = p; break; }
    }
    if (chosen_port < 0) {
        log("split: no free UDP port found in %d..%d; using %d", base_port, base_port + 49, base_port);
        chosen_port = base_port;
    }
    s_port = chosen_port;
    LONG gen = InterlockedIncrement(&s_active_gen);
    DWORD start_now = GetTickCount();
    s_start_tick = start_now ? start_now : 1;
    s_alive_tick = s_start_tick;
    s_stuck_strikes = 0;
    s_healthy_since = 0;
    InterlockedExchange(&s_alive_seen, 0);
    InterlockedExchange(&s_server_loading, 0);
    s_load_failed_id.store(0, std::memory_order_release);
    char pipe_arg[64];
    char pipe_name[128];
    if (gen == 1) snprintf(pipe_arg, sizeof(pipe_arg), "%lu", GetCurrentProcessId());
    else snprintf(pipe_arg, sizeof(pipe_arg), "%lu-%ld", GetCurrentProcessId(), gen);
    snprintf(pipe_name, sizeof(pipe_name), "\\\\.\\pipe\\vtmb-memx-%s", pipe_arg);
    char game[MAX_PATH];
    game_dir_arg(game, sizeof(game));
    wchar_t cmd[2048];
    // -game unquoted: the engine keeps quotes as part of the folder name. +ip 127.0.0.1: the server's
    // socket is reachable from this PC only.
    _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"\"%s\" -memx_server -memx_pipe %S -port %d -maxplayers 1 -novid%s%S +ip 127.0.0.1",
                 exe, pipe_arg, s_port, game[0] ? L" -game " : L"", game);

    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE out_read = nullptr, out_write = nullptr;
    if (!CreatePipe(&out_read, &out_write, &sa, 1 << 16)) return false;
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) {
        CloseHandle(out_read);
        CloseHandle(out_write);
        return false;
    }

    // the server inherits these two handles and nothing else
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<BYTE> attr_buf(attr_size);
    auto attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attr_buf.data();
    HANDLE inherit[2] = {out_write, nul};
    BOOL ok = FALSE;
    DWORD create_err = 0;
    PROCESS_INFORMATION pi = {};
    if (attr_size && InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) {
        if (UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr)) {
            STARTUPINFOEXW si = {};
            si.StartupInfo.cb = sizeof(si);
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            si.StartupInfo.hStdInput = nul;
            si.StartupInfo.hStdOutput = out_write;
            si.StartupInfo.hStdError = out_write;
            si.lpAttributeList = attrs;
            wchar_t dir[MAX_PATH];
            GetCurrentDirectoryW(MAX_PATH, dir);
            ok = CreateProcessW(exe, cmd, nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                nullptr, dir, &si.StartupInfo, &pi);
            if (!ok) create_err = GetLastError();
        } else {
            create_err = GetLastError();
        }
        DeleteProcThreadAttributeList(attrs);
    } else {
        create_err = GetLastError();
    }
    CloseHandle(out_write);
    CloseHandle(nul);
    if (!ok) {
        log("split: could not start the server process (error %lu); the game runs its server itself", create_err);
        CloseHandle(out_read);
        return false;
    }
    // the server ends with the game, however the game ends
    if (s_job) { CloseHandle(s_job); s_job = nullptr; }
    s_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = {};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (s_job) {
        if (!SetInformationJobObject(s_job, JobObjectExtendedLimitInformation, &lim, sizeof(lim)))
            log("split: warning: SetInformationJobObject failed (%lu)", GetLastError());
        else if (!AssignProcessToJobObject(s_job, pi.hProcess))
            log("split: warning: AssignProcessToJobObject failed (%lu); server will exit on pipe close", GetLastError());
    }
    HANDLE own = nullptr;
    HANDLE send_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!send_ev || !DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &own, SYNCHRONIZE, FALSE, 0)) {
        log("split: failed to create server IPC handles (error %lu)", GetLastError());
        if (send_ev) CloseHandle(send_ev);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (s_job) { CloseHandle(s_job); s_job = nullptr; }
        CloseHandle(out_read);
        return false;
    }
    auto *conn = new ServerConn();
    conn->gen = gen;
    conn->proc = own;
    conn->out_read = out_read;
    strncpy_s(conn->pipe_name, pipe_name, _TRUNCATE);
    conn->send_event = send_ev;
    InitializeCriticalSection(&conn->cs);
    conn->ref = 3;   // s_conn + output thread + input thread

    EnterCriticalSection(&s_cs);
    ServerConn *prev = s_conn;
    s_conn = conn;
    LeaveCriticalSection(&s_cs);
    conn_release(prev);

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    InterlockedExchangePointer((volatile PVOID *)&s_server, pi.hProcess);
    log("split: server process %lu started on UDP port %d", pi.dwProcessId, s_port);
    if (HANDLE t = CreateThread(nullptr, 0, server_output_thread, conn, 0, nullptr)) CloseHandle(t);
    else { CloseHandle(out_read); conn_release(conn); }
    if (HANDLE t = CreateThread(nullptr, 0, server_input_thread, conn, 0, nullptr)) CloseHandle(t);
    else {
        CloseHandle(own);
        s_server_dead.store(true, std::memory_order_release);
        conn_release(conn);
    }
    // a one-player game reachable from this PC only, sending snapshots as often as the game asks
    int net_rate = g_settings.split_server_fps > 100 ? g_settings.split_server_fps : 100;
    to_server("sv_lan 1");
    to_server("sv_timeout 86400");
    char rate_cmd[48];
    snprintf(rate_cmd, sizeof(rate_cmd), "sv_maxupdaterate %d", net_rate);
    to_server(rate_cmd);
    to_server("sv_maxrate 0");
    // world frames a second ([Split] server_fps; single player runs them at the game's frame rate)
    char fps[32];
    snprintf(fps, sizeof(fps), "fps_max %d", g_settings.split_server_fps);
    to_server(fps);
    return true;
}

// ---- the commands ----
static std::string args_from(int first) {
    std::string a;
    for (int i = first; i < Cmd_Argc(); ++i) {
        if (i > first) a += ' ';
        const char *v = Cmd_Argv(i);
        if (!v) v = "";
        std::string clean;
        bool need_quote = (*v == '\0');
        for (const char *p = v; *p; ++p) {
            if (*p == '\r' || *p == '\n' || *p == '"') continue;
            if (*p == ' ' || *p == '\t' || *p == ';' || (*p == '/' && p[1] == '/')) need_quote = true;
            clean += *p;
        }
        if (clean.empty()) need_quote = true;
        if (need_quote) a += '"';
        a += clean;
        if (need_quote) a += '"';
    }
    return a;
}

static void call_original(const char *name) {
    for (CommandHook &c : s_commands)
        if (!strcmp(c.name, name)) ((VoidFn_t)c.original)();
}

static const uintptr_t kCommandList = 0x20A3F45C;    // list node: +4 ConCommandBase *, +8 next; base +16 name

// Strip FCVAR_ARCHIVE (0x80) from the network cvars we override for loopback split mode so the engine
// never permanently writes split-only rates into the player's config.cfg.
static void clear_split_archive_flags() {
    static const char *const kNetVars[] = {"cl_updaterate", "cl_cmdrate", "cl_cmdbackup", "cl_interp", "rate", "cl_resend"};
    for (BYTE *n = *(BYTE **)kCommandList; n; n = *(BYTE **)(n + 8)) {
        BYTE *var = *(BYTE **)(n + 4);
        if (!var) continue;
        if (((bool(__fastcall *)(void *, void *))(*(void ***)var)[1])(var, nullptr)) continue;
        const char *name = *(const char **)(var + 16);
        if (!name) continue;
        for (const char *t : kNetVars) {
            if (!_stricmp(name, t)) {
                *(int *)(var + 24) &= ~0x80;
                break;
            }
        }
    }
}

static void connect_to_server() {
    clear_split_archive_flags();
    char cmd[256];
    int net_rate = g_settings.split_server_fps > 100 ? g_settings.split_server_fps : 100;
    // Match client update/cmd rates to server_fps; cl_interp 0.025 (25 ms) gives ~3.6 snapshots at 144 Hz for
    // zero-desync local-player interpolation; cl_cmdbackup 0 trims loopback overhead; cl_resend 0.25 avoids 6s stalls.
    snprintf(cmd, sizeof(cmd),
             "cl_updaterate %d\ncl_cmdrate %d\ncl_cmdbackup 0\ncl_interp 0.025\nrate 30000\ncl_resend 0.25\nconnect 127.0.0.1:%d\n",
             net_rate, net_rate, s_port);
    Cbuf_AddText(cmd);
}

// A server that has ended (crashed) is replaced when the player next starts or loads a game; after three
// failures the game runs its server itself, as without the split.
static bool ensure_server() {
    if (s_split_fallback) return false;
    // a server that runs but has not gone round its main loop for a while gets no more work
    DWORD quiet = server_quiet_ms();
    DWORD limit = s_alive_seen ? (s_server_loading ? kStuckLoadingMs : 10000) : kStuckStartupMs;
    if (quiet > limit) end_stuck_server(quiet);
    if (server_usable()) return true;
    if (s_restarts >= 3) {
        s_split_fallback = true;
        log("split: restart limit (3) reached; falling back to single player for this session");
        return false;
    }
    ++s_restarts;
    log("split: the server process is gone; starting a new one (%d of 3)", s_restarts);
    Con_Printf("Memory Expansion: restarting the server process\n");
    HANDLE old_srv = (HANDLE)InterlockedExchangePointer((volatile PVOID *)&s_server, nullptr);
    if (old_srv) CloseHandle(old_srv);
    if (s_job) { CloseHandle(s_job); s_job = nullptr; }
    EnterCriticalSection(&s_cs);
    ServerConn *old_conn = s_conn;
    s_conn = nullptr;
    s_preload_req.clear();
    s_save_client_req.clear();
    LeaveCriticalSection(&s_cs);
    conn_release(old_conn);
    if (!start_server()) {
        if (s_restarts >= 3) s_split_fallback = true;
        return false;
    }
    return true;
}

// ---- what the two processes must share ----
// In single player the world's code (Python, vampire.dll) runs console commands in the one process the
// game has: the aliases the game's cfg files define (cfg/user.cfg: vm_discipline -> checkDiscipline(),
// the player's idle animations, the Unofficial Patch's patchtype) and the commands of client.dll and of
// the engine's client side (vloadtips, ...). The server process execs none of those cfg files and has no
// client.dll. Before each new game or load it gets this process's aliases, and once the names of this
// process's commands and variables: the server sends a command it does not know but this process does
// back here ("memx:cmd", split_server.cpp), where it runs as in single player.
static const uintptr_t kAliasList = 0x203489DC;      // cmdalias_t *: +0 next, +4 name[32], +36 value

// The player's settings: the server process reads none of the game's cfg files (config.cfg) and sees no
// option the player changes, so the world ran with defaults: no NPC line in dialogue (cl_captions, the
// subtitle option: CDialog::Acquire shows the NPC's line only when it is on), the default difficulty
// (skill), vampire.dll's own options (vdiscipline_allow_renewables, ...). Synced: every variable of
// vampire.dll (the world's code; this process's copy runs nothing) and the engine variables below that
// the world reads. A value goes when it differs from the last one sent (at first: from its default), at
// each new game or load and within half a second of a change. Values the world's code sets itself stay
// as they are on the server.
static const char *const kEngineSettings[] = {"cl_captions", "skill", "sv_cheats", "violence_hblood", "violence_ablood",
                                               "violence_hgibs", "violence_agibs"};
struct SyncedVar {
    BYTE *var;          // ConVar (+4 parent, +16 name, +28 default, +32 value string)
    std::string sent;   // the value the server has from us (its default before the first send)
};
static std::vector<SyncedVar> s_synced;
static bool s_synced_built;
static void forget_sent_settings() {
    for (SyncedVar &s : s_synced) s.sent = *(const char **)(s.var + 28);
}

static bool has_ctl_or_space(const char *s, bool forbid_space) {
    for (; *s; ++s) {
        if (*s == '\r' || *s == '\n' || *s == '"') return true;
        if (forbid_space && (*s == ' ' || *s == '\t' || *s == ';')) return true;
    }
    return false;
}

static void sync_settings() {
    if (!s_synced_built) {
        s_synced_built = true;
        HMODULE vampire = GetModuleHandleW(L"vampire.dll");
        uintptr_t lo = (uintptr_t)vampire, hi = lo;
        if (vampire && in_module_image(vampire, 0, 1)) {
            auto nt = (IMAGE_NT_HEADERS *)((BYTE *)vampire + ((IMAGE_DOS_HEADER *)vampire)->e_lfanew);
            hi = lo + nt->OptionalHeader.SizeOfImage;
        }
        std::unordered_set<BYTE *> seen_vars;
        for (BYTE *n = *(BYTE **)kCommandList; n; n = *(BYTE **)(n + 8)) {
            BYTE *var = *(BYTE **)(n + 4);
            if (!var) continue;
            // IsCommand (vtable slot 1)
            if (((bool(__fastcall *)(void *, void *))(*(void ***)var)[1])(var, nullptr)) continue;
            const char *name = *(const char **)(var + 16), *def = *(const char **)(var + 28);
            if (!name || !def || has_ctl_or_space(name, true)) continue;
            // replicated variables (0x2000, sv_skyname, ...) come from the server: never sent back
            if (*(int *)(var + 24) & 0x2000) continue;
            bool engine_setting = false;
            for (const char *e : kEngineSettings) engine_setting |= !_stricmp(name, e);
            if ((engine_setting || ((uintptr_t)n >= lo && (uintptr_t)n < hi)) && seen_vars.insert(var).second)
                s_synced.push_back({var, def});
        }
        log("split: %u settings follow the game to the server process", (unsigned)s_synced.size());
    }
    for (SyncedVar &s : s_synced) {
        const char *value = *(const char **)(s.var + 32);
        if (!value || strcmp(s.sent.c_str(), value) == 0 || has_ctl_or_space(value, false) || strlen(value) > 200) continue;
        const char *name = *(const char **)(s.var + 16);
        s.sent = value;
        if (g_settings.verbose) log("split: setting %s \"%s\"", name, value);
        to_server(std::string(name) + " \"" + value + "\"");
    }
}

static int send_command_names();
static void send_game_state() {
    int aliases = 0, skipped = 0;
    for (BYTE *a = *(BYTE **)kAliasList; a; a = *(BYTE **)a) {
        const char *name = (const char *)(a + 4);
        const char *value = *(const char **)(a + 36);
        if (!memchr(name, 0, 32) || !value) continue;
        std::string v(value);
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
        if (v.size() > 900 || has_ctl_or_space(v.c_str(), false) || has_ctl_or_space(name, true)) {
            ++skipped;
            continue;
        }
        to_server(std::string("alias ") + name + " \"" + v + "\"");
        ++aliases;
    }
    int names = send_command_names();
    log("split: %d aliases to the server process (%d with quotes or too long left out)%s", aliases, skipped,
        names ? (", and the names of " + std::to_string(names) + " commands and variables").c_str() : "");
    sync_settings();
}

// The names of this process's commands and variables, incrementally whenever new ones appear (client.dll
// creates some when the first level loads, e.g. cl_detailfade). Returns the count of newly sent names.
static int send_command_names() {
    BYTE *head = *(BYTE **)kCommandList;
    int count = 0;
    for (BYTE *n = head; n; n = *(BYTE **)(n + 8)) ++count;
    if (s_sent_commands && head == s_sent_cmd_head && count == s_sent_names) return 0;
    s_sent_commands = true;
    s_sent_cmd_head = head;
    s_sent_names = count;
    int names = 0;
    {
        std::string line = "memx:gamecmds";
        for (BYTE *n = head; n; n = *(BYTE **)(n + 8)) {
            BYTE *base = *(BYTE **)(n + 4);
            const char *name = base ? *(const char **)(base + 16) : nullptr;
            if (!name || !*name || strlen(name) > 64 || has_ctl_or_space(name, true)) continue;
            if (!s_sent_cmd_set.emplace(name).second) continue;
            line += ' ';
            line += name;
            ++names;
            if (line.size() > 900) {
                to_server(line);
                line = "memx:gamecmds";
            }
        }
        if (line.size() > 13) to_server(line);
    }
    return names;
}

// ---- the loading screen ----
// In single player the "map" command names the level for the loading screen (SetLoadingMapName,
// 0x200871B0) and Host_NewGame, which "map" and "load" end in, puts the loading screen up
// (SCR_BeginLoadingPlaque(true), 0x20086A80) before the level loads. A split game only connects, and the
// engine begins the loading screen for a connection only while already connected (a level change):
// without these two calls the screen stays black while the level loads. Byte-checked at install.
static const BYTE kSetLoadingMapBytes[] = {0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0, 0x74, 0x16, 0x80, 0x38, 0x00, 0x74, 0x11, 0x6A, 0x50, 0x50};
static const BYTE kBeginPlaqueBytes[] = {0x51, 0x53, 0x6A, 0x01, 0xE8, 0x37, 0x96, 0x09, 0x00, 0x8A, 0x44, 0x24, 0x10, 0x33, 0xDB, 0x83};
// the "map" command's level test: mov ecx,[0x201AE3B8]; mov eax,[ecx]; lea edx,[esp+8]; push edx; call [eax+28h]
static const BYTE kMapValidSite[] = {0x8B, 0x0D, 0xB8, 0xE3, 0x1A, 0x20, 0x8B, 0x01, 0x8D, 0x54, 0x24, 0x08, 0x52, 0xFF, 0x50, 0x28};
// CL_Disconnect (0x200267B0): mov eax,[0x201DBE50]; sub esp,14h; test eax,eax
static const BYTE kClDisconnectBytes[] = {0xA1, 0x50, 0xBE, 0x1D, 0x20, 0x83, 0xEC, 0x14, 0x85, 0xC0};
static bool s_loading_screen;   // the three above are the known code
static bool s_cl_disconnect_ok;

static bool map_exists(const char *name) {
    void *obj = *(void *volatile *)0x201AE3B8;
    if (!obj) return true;
    auto fn = (int(__fastcall *)(void *, void *, const char *))(*(void ***)obj)[10];
    return fn(obj, nullptr, name) != 0;
}

// a save the server can load: <game folder>\SAVE\<name>.sav (the -game folder, or Vampire)
static bool save_exists(const char *name) {
    if (!name || !*name) return false;
    char root[MAX_PATH], game[MAX_PATH], path[MAX_PATH];
    if (!GetModuleFileNameA(nullptr, root, MAX_PATH)) return false;   // the game folder: Vampire.exe's
    if (char *slash = strrchr(root, '\\')) *slash = 0;
    game_dir_arg(game, sizeof(game));
    const char *ext = strchr(name, '.') ? "" : ".sav";
    for (const char *dir : {game[0] ? (const char *)game : "Vampire", "Vampire"}) {
        int n = snprintf(path, sizeof(path), "%s\\%s\\SAVE\\%s%s", root, dir, name, ext);
        if (n > 0 && n < (int)sizeof(path)) {
            DWORD attr = GetFileAttributesA(path);
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) return true;
        }
    }
    return false;
}

static void show_loading_screen(const char *map) {
    if (!s_loading_screen) return;
    if (map) ((void(__cdecl *)(const char *))0x200871B0)(map);
    ((void(__cdecl *)(int))0x20086A80)(1);
}

// ---- the dialogue window ----
// client.dll's dialogue window opens and closes only by the world's "DialogCtrl" user message (type 8,
// CDialog::SetDialogActive, vampire.dll 0x100E0F90). Loading a save during a dialogue: in single player
// the world's shutdown closes it; a split game leaves the server's level before that message comes, so
// the window stayed open over the newly loaded level (a black screen with the dialogue box). This
// process closes it itself, through client.dll's own DialogCtrl handler (client.dll+0x58110:
// cdecl (name, size, data) -> CHudDialog::DialogControl), with the message the world would send.
static void close_dialog_window() {
    HMODULE client = GetModuleHandleW(L"client.dll");
    if (!client) return;
    const BYTE *fn = (const BYTE *)client + 0x58110;
    static const BYTE kHead[] = {0x8B, 0x44, 0x24, 0x0C, 0x8B, 0x4C, 0x24, 0x08, 0x8B, 0x54, 0x24, 0x04, 0x50, 0x51, 0x52, 0xB9};
    static const BYTE kTail[] = {0xE8, 0x37, 0x0D, 0x00, 0x00, 0xC3};
    DWORD hud = (DWORD)((uintptr_t)client + 0x3EEC94);   // gHUD (client.dll 0x103EEC94 at its preferred base)
    __try {
        if (memcmp(fn, kHead, sizeof(kHead)) || memcmp(fn + 16, &hud, 4) || memcmp(fn + 20, kTail, sizeof(kTail))) {
            log("split: dialogue window not closed (unknown client.dll code)");
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    BYTE msg[2] = {8, 0};   // DialogCtrl: set active, false
    ((int(__cdecl *)(const char *, int, void *))fn)("DialogCtrl", sizeof(msg), msg);
}

// a level command for the server; "expects_level": the loading screen is up and waits for the level
static void level_command_to_server(const std::string &line, bool expects_level) {
    s_load_expects_level = expects_level || (s_load_expects_level && s_want_game && *(volatile int *)kClsState < 4);
    to_server("memx:loadid " + std::to_string(++s_load_id));
    to_server(line);
}

static void start_on_server(const char *verb) {
    if (*(int *)kCmdSource != 1 || Cmd_Argc() < 2 || !ensure_server()) return call_original(verb);
    char map[128] = {};
    if (!strcmp(verb, "map")) {
        strncpy_s(map, Cmd_Argv(1), _TRUNCATE);
        size_t n = strlen(map);
        if (n > 4 && !_stricmp(map + n - 4, ".bsp")) map[n - 4] = 0;
        // an unknown level: the game's own command says so and starts nothing
        if (s_loading_screen && !map_exists(map)) return call_original(verb);
    }
    std::string line = std::string(verb) + " " + args_from(1);
    log("split: %s", line.c_str());
    Con_Printf("Memory Expansion: the server process runs \"%s\"\n", line.c_str());
    bool starts = map[0] || save_exists(Cmd_Argv(1));
    if (starts) {
        s_want_game = true;
        InterlockedExchange(&s_server_loading, 1);
        s_preloaded_map.clear();
        s_spawns_seen = s_server_spawns;
        EnterCriticalSection(&s_cs);
        s_preload_req.clear();
        s_from_server.clear();
        s_pending_work.fetch_and(~10u, std::memory_order_release);
        LeaveCriticalSection(&s_cs);
        // the engine has no time limit on the loading screen: only for a level or save that exists (a missing
        // one never connects). Connect once "memx:ready" arrives so the client does not stall on cl_resend
        // while the server is still loading the level.
        show_loading_screen(map[0] ? map : nullptr);
    }
    // From a game in progress, this process leaves its level now, under the loading screen, as single
    // player does. Otherwise the server's "shutting down" for the old level ended the game here first
    // (Host_EndGame: the main menu showed instead of the loading screen), and a dialogue window stayed open.
    int cls_state = *(volatile int *)kClsState;
    if (starts && cls_state >= 2 && s_cl_disconnect_ok && !*(volatile int *)kSvActive) {
        close_dialog_window();
        ((VoidFn_t)0x200267B0)();         // CL_Disconnect
        log("split: left the level here before the server loads");
    }
    send_game_state();
    level_command_to_server(line, starts);
}
static void split_map() { start_on_server("map"); }
static void split_load() { start_on_server("load"); }
static void split_save() {
    if (*(int *)kCmdSource != 1 || !server_usable() || !s_want_game) return call_original("save");
    std::string line = "save " + args_from(1);
    log("split: %s", line.c_str());
    to_server(line);
}
// typed level commands of a game in progress: the level is the server's
static void forward_in_game(const char *verb) {
    if (*(int *)kCmdSource != 1 || !server_usable() || !s_want_game) return call_original(verb);
    std::string line = verb;
    if (Cmd_Argc() > 1) line += " " + args_from(1);
    log("split: %s", line.c_str());
    level_command_to_server(line, false);
}
static void split_changelevel() { forward_in_game("changelevel"); }
static void split_reload() { forward_in_game("reload"); }
static void split_restart() { forward_in_game("restart"); }
static void split_disconnect() {
    if (!*(volatile int *)kSvActive) close_dialog_window();
    s_want_game = false;
    s_ready_for_connect = false;
    s_preloaded_map.clear();
    EnterCriticalSection(&s_cs);
    s_preload_req.clear();
    s_from_server.clear();
    s_pending_work.fetch_and(~10u, std::memory_order_release);
    LeaveCriticalSection(&s_cs);
    call_original("disconnect");
}
// v_setpause / v_unpause (client.dll's interface screens) pause the sound here and the world of this
// process; the world is the server's: it pauses too (its "setpause" and "unpause", which the menu uses,
// the engine forwards by itself as commands from the player)
static void split_v_pause(const char *verb) {
    call_original(verb);
    if (s_want_game && server_usable() && *(volatile int *)kClsState >= 3) {
        log("split: %s", verb);
        to_server(verb);
    }
}
static void split_v_setpause() { split_v_pause("v_setpause"); }
static void split_v_unpause() { split_v_pause("v_unpause"); }

// Console text that is no command goes to the game's Python (Cmd_ExecuteString -> 0x200DAB20). In single
// player that Python is the one running the world; in a split game it is the server's.
typedef int(__cdecl *PyConsole_t)(const char *);
static int __cdecl split_python(const char *text) {
    if (text && s_want_game && server_usable() && *(volatile int *)kClsState >= 3) {
        std::string clean;
        for (const char *p = text; *p && *p != '\r' && *p != '\n' && clean.size() < 980; ++p) clean += *p;
        // Never allow console text to spoof internal memx:* pipe protocol lines.
        if (!clean.empty() && _strnicmp(clean.c_str(), "memx:", 5) != 0) {
            log("split: python: %s", clean.c_str());
            to_server(clean);
            return 0;
        }
    }
    return ((PyConsole_t)0x200DAB20)(text);
}

// ---- static props across a level change ----
// The engine's static prop manager (0x2129EDC0) reads a level's static props once (LevelInit, vtable slot
// 2, 0x200E8CE0, guarded by a flag) and forgets them only in LevelShutdown (slot 5, 0x200E9030), which
// only the server's SV_SpawnServer calls (0x20111EA0). A game process playing on another process's
// server never called it: at the next level (a level change, or loading another save) it kept the
// previous level's props and added them to the new level's leaves (client.dll's leaf system), which
// crashed (client.dll+0xD260E) or drew the wrong props. So when this process clears its level
// (CL_ClearState, 0x20026570: the client's LevelShutdown follows) without a server of its own, the props
// go first, while the client's leaf system still holds them, the order single player has.
static void reset_npc_interp();
static const uintptr_t kStaticProps = 0x2129EDC0;
static void __cdecl split_clear_state() {
    reset_npc_interp();
    void **vt = *(void ***)kStaticProps;
    if (*(volatile int *)kClsState != 0 && !*(volatile int *)kSvActive && s_server && vt == (void **)0x2017549C &&
        vt[2] == (void *)0x200E8CE0 && vt[5] == (void *)0x200E9030 && *(volatile BYTE *)(kStaticProps + 92)) {
        if (g_settings.verbose) log("split: static props of the previous level released");
        ((void(__fastcall *)(void *, void *))vt[5])((void *)kStaticProps, nullptr);
    }
    ((VoidFn_t)0x20026570)();
}

// CL_ClearState's call to Host_FreeToLowMark (0x20026673 -> 0x2008F3D0): in single player sv.active != 0
// makes it return at once without unloading the newly loaded BSP. When we just preloaded the target BSP
// in parallel with the server, skip this one flush so CL_RegisterResources hits the already-loaded BSP.
static void __cdecl split_free_world(int clear_client) {
    if (!clear_client && !s_preloaded_map.empty() && !*(volatile int *)kSvActive) {
        if (g_settings.verbose) log("split: keeping preloaded %s across CL_ClearState", s_preloaded_map.c_str());
        s_preloaded_map.clear();
        return;
    }
    ((void(__cdecl *)(int))0x2008F3D0)(clear_client);
}

// ---- map decals restored from .HL2 ----
// CSaveRestore::RestoreDecal (engine.dll 0x20096850) calls sv.PrecacheDecal (0x200968D9 -> 0x200F8870)
// when a restored decal is not yet in the client decal dictionary. Without a local server (sv.active == 0)
// sv.PrecacheDecal returns -1 and R_DecalShoot drops the decal. Allocate a client dictionary slot >= 512 so
// it cannot collide with server decalprecache indices (0..511); Draw_DecalSetName grows the vector to fit.
static int __fastcall split_restore_precache_decal(void *sv, void *, const char *name, int preload) {
    if (*(volatile int *)kSvActive) {
        typedef int(__fastcall *PrecacheDecal_t)(void *, void *, const char *, int);
        return ((PrecacheDecal_t)0x200F8870)(sv, nullptr, name, preload);
    }
    int count = *(volatile int *)0x20A419A4;
    if (g_settings.verbose && count <= 512) log("split: restoring .HL2 map decals (dictionary count %d)", count);
    return count < 512 ? 512 : count;
}

// ---- guard cl.GetSound against out-of-range sound table indices ----
// CNetworkStringTable::GetString has no bounds check: if a sound packet arrives over UDP before the
// reliable string-table update adds its index (e.g. right after a level load), GetString reads out of
// bounds and crashes in S_PrecacheSound (engine.dll+0x119200). Skip the sound if not yet in table.
static void *__fastcall split_cl_get_sound(void *cl, void *, int soundnum) {
    if (!*(volatile int *)kSvActive && soundnum > 0) {
        void *st = *(void *volatile *)0x201A805C;   // g_pStringTableClient
        int tbl = *(volatile int *)((BYTE *)cl + 0x10EFE0);
        if (tbl >= 0 && st && *(void ***)st == (void **)0x20175098 && (*(void ***)st)[2] == (void *)0x200C41F0) {
            int n = ((int(__fastcall *)(void *, void *, int))(*(void ***)st)[2])(st, nullptr, tbl);
            if (soundnum >= n) {
                static int logged;
                if (g_settings.verbose || logged < 20) {
                    ++logged;
                    log("split: sound index %d not yet in client table (%d); skipped", soundnum, n);
                }
                return nullptr;
            }
        }
    }
    return ((void *(__fastcall *)(void *, void *, int))0x20028DE0)(cl, nullptr, soundnum);
}

// ---- fast signon frames ----
// Host_RunFrame's call to Host_FilterTime (0x2008E499 -> 0x2008BA30): temporarily uncap fps_max (0.0 skips
// the frame limiter) while receiving signon fragments so 160-fragment stop-and-wait does not sleep 15 ms per
// fragment. Kept off during normal connecting wait so the client does not burn CPU while the server loads.
static int __cdecl client_filter_time(float dt) {
    typedef int(__cdecl *FilterTime_t)(float);
    int cls_state = *(volatile int *)kClsState;
    if (cls_state != 2) s_ready_for_connect = false;
    bool fast = s_want_game && !*(volatile int *)kSvActive &&
                (cls_state == 3 || (cls_state == 2 && s_ready_for_connect));
    BYTE *fps = fast ? *(BYTE *volatile *)0x20B42884 : nullptr;
    float saved = 0.0f;
    if (fps) {
        saved = *(float *)(fps + 0x28);
        *(float *)(fps + 0x28) = 0.0f;
    }
    int r = ((FilterTime_t)0x2008BA30)(dt);
    if (fps) *(float *)(fps + 0x28) = saved;
    return r;
}

// C_BaseAnimating::Interpolate (client.dll+0x94F00) handles MOVETYPE_STEP (walking NPCs) with an old
// GoldSrc m_flAnimTime extrapolator (client.dll+0x8F960) that computes f = clamp(1 + (cl.time - anim)/dt, 0, 2)
// and lerps origin = (1-f)*m_vecOldOrigin + f*m_vecNetworkOrigin. Over UDP that has two failure modes:
//   1. Extrapolation overshoot (f > 1.0) pushes NPCs past their real position and snaps them back ("yo-yo").
//   2. Relying on raw m_vecOldOrigin / m_flAnimTime / cl.time still lets NPCs briefly "teleport" 1-2 steps
//      ahead whenever:
//        - m_flAnimTime has a rapid double update (dt = 0.01..0.04 s, finishing a step in 1 frame),
//        - CAI_BaseNPCTroika::NPCThink moves m_vecOrigin without advancing m_flAnimTime (separate timers),
//        - a brief frame hitch or packet burst overwrites m_vecOldOrigin before the prior step finished,
//        - cl.time snaps (cl_clockreset) or drifts ahead of m_flAnimTime, or dt > 0.40 s triggers a snap.
// Replace the call at client+0x94F62 with a continuous render-space interpolator keyed per entity:
//   - Driven by a monotonic, pause-aware, hitch-clamped frame clock (immune to cl.time jumps & 10 ms quantization).
//   - Detects every m_vecNetworkOrigin / m_angNetworkAngles change directly (even without m_flAnimTime changes).
//   - Always starts each new segment from the previous frame's exact rendered position (zero forward teleport),
//     carrying over remaining time and pacing multi-step bursts at the NPC's smoothed walk/run speed.
//   - Snaps cleanly on first sight, EF_NOINTERP (0x800), PVS re-entry (>0.35 s unseen), or true teleports (>96 u).
struct NpcInterpSlot {
    const BYTE *ent;
    float last_net_org[3];
    float last_net_ang[3];
    float start_org[3];
    float start_ang[3];
    float cur_org[3];
    float cur_ang[3];
    double start_time;
    double end_time;
    double last_seen;
    float ema_speed;
};
static constexpr size_t kNpcInterpSlots = 512;
static NpcInterpSlot s_npc_interp[kNpcInterpSlots];
static double s_interp_clock = 0.0;
static double s_interp_last_dt = 0.0;

static void reset_npc_interp() {
    memset(s_npc_interp, 0, sizeof(s_npc_interp));
    s_interp_last_dt = 0.0;
}

static float wrap_angle_diff(float from, float to) {
    float d = fmodf(to - from, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    return d;
}

static NpcInterpSlot *find_npc_interp_slot(const BYTE *ent, double now) {
    size_t h = ((uintptr_t)ent >> 4) & (kNpcInterpSlots - 1);
    NpcInterpSlot *empty = nullptr;
    NpcInterpSlot *oldest = &s_npc_interp[h];
    for (size_t i = 0; i < 8; ++i) {
        NpcInterpSlot *s = &s_npc_interp[(h + i) & (kNpcInterpSlots - 1)];
        if (s->ent == ent) return s;
        if (!s->ent && !empty) empty = s;
        if (s->last_seen < oldest->last_seen) oldest = s;
    }
    if (empty) return empty;
    // Evict stale slot (or oldest in probe window).
    (void)now;
    oldest->ent = nullptr;
    return oldest;
}

static float __fastcall split_npc_interp_fraction(BYTE *ent) {
    if (!ent) return 1.0f;
    float *old_org = (float *)(ent + 0x54);
    float *old_ang = (float *)(ent + 0x60);
    const float *net_org = (const float *)(ent + 0x2B4);
    const float *net_ang = (const float *)(ent + 0x2C0);
    float old_t = *(const float *)(ent + 0x6C);
    float cur_t = *(const float *)(ent + 0x70);
    uint32_t effects = *(const uint32_t *)(ent + 0x74);
    double now = s_interp_clock;

    NpcInterpSlot *slot = find_npc_interp_slot(ent, now);
    bool snap = (slot->ent != ent) || ((effects & 0x800u) != 0) ||
                (now < slot->last_seen) || ((now - slot->last_seen) > 0.35);

    if (!snap) {
        // Advance cur_org / cur_ang to exact interpolated position at `now`.
        bool was_at_end = (slot->last_seen >= slot->end_time - 1e-5);
        float prev_f = 1.0f;
        if (slot->end_time > slot->start_time) {
            prev_f = (float)((now - slot->start_time) / (slot->end_time - slot->start_time));
            if (prev_f < 0.0f) prev_f = 0.0f;
            if (prev_f > 1.0f) prev_f = 1.0f;
        }
        for (int i = 0; i < 3; ++i) {
            slot->cur_org[i] = slot->start_org[i] + (slot->last_net_org[i] - slot->start_org[i]) * prev_f;
            slot->cur_ang[i] = slot->start_ang[i] + wrap_angle_diff(slot->start_ang[i], slot->last_net_ang[i]) * prev_f;
        }

        float cdx = net_org[0] - slot->cur_org[0];
        float cdy = net_org[1] - slot->cur_org[1];
        float cdz = net_org[2] - slot->cur_org[2];
        float total_dist_sq = cdx * cdx + cdy * cdy + cdz * cdz;
        if (total_dist_sq > 96.0f * 96.0f) {
            snap = true;
        } else {
            float ndx = net_org[0] - slot->last_net_org[0];
            float ndy = net_org[1] - slot->last_net_org[1];
            float ndz = net_org[2] - slot->last_net_org[2];
            float net_step_sq = ndx * ndx + ndy * ndy + ndz * ndz;
            bool org_changed = net_step_sq > 1e-4f;
            bool ang_changed = fabsf(wrap_angle_diff(slot->last_net_ang[0], net_ang[0])) > 0.05f ||
                               fabsf(wrap_angle_diff(slot->last_net_ang[1], net_ang[1])) > 0.05f ||
                               fabsf(wrap_angle_diff(slot->last_net_ang[2], net_ang[2])) > 0.05f;
            if (org_changed || ang_changed) {
                float anim_dt = cur_t - old_t;
                float step_dt = (anim_dt >= 0.06f && anim_dt <= 0.25f) ? anim_dt : 0.10f;
                float remaining = (slot->end_time > now) ? (float)(slot->end_time - now) : 0.0f;
                if (remaining > 0.12f) remaining = 0.12f;
                float dur = step_dt + remaining;
                if (org_changed) {
                    float total_dist = sqrtf(total_dist_sq);
                    float net_step = sqrtf(net_step_sq);
                    float inst_speed = net_step / step_dt;
                    if (slot->ema_speed > 15.0f && total_dist > slot->ema_speed * dur * 1.35f) {
                        float paced = total_dist / slot->ema_speed;
                        if (paced > 0.25f) paced = 0.25f;
                        if (paced > dur) dur = paced;
                    }
                    if (net_step > 1.0f && net_step < 32.0f) {
                        slot->ema_speed = (slot->ema_speed > 1.0f)
                            ? (slot->ema_speed * 0.75f + inst_speed * 0.25f)
                            : inst_speed;
                    }
                }
                memcpy(slot->start_org, slot->cur_org, sizeof(slot->start_org));
                memcpy(slot->start_ang, slot->cur_ang, sizeof(slot->start_ang));
                memcpy(slot->last_net_org, net_org, sizeof(slot->last_net_org));
                memcpy(slot->last_net_ang, net_ang, sizeof(slot->last_net_ang));
                double lead = (was_at_end && s_interp_last_dt > 0.0 && s_interp_last_dt < 0.025) ? s_interp_last_dt : 0.0;
                slot->start_time = now - lead;
                slot->end_time = slot->start_time + (double)dur;
            }
        }
    }

    if (snap) {
        slot->ent = ent;
        memcpy(slot->last_net_org, net_org, sizeof(slot->last_net_org));
        memcpy(slot->last_net_ang, net_ang, sizeof(slot->last_net_ang));
        memcpy(slot->start_org, net_org, sizeof(slot->start_org));
        memcpy(slot->start_ang, net_ang, sizeof(slot->start_ang));
        memcpy(slot->cur_org, net_org, sizeof(slot->cur_org));
        memcpy(slot->cur_ang, net_ang, sizeof(slot->cur_ang));
        slot->start_time = now;
        slot->end_time = now;
        slot->last_seen = now;
        slot->ema_speed = 0.0f;
        memcpy(old_org, net_org, 12);
        memcpy(old_ang, net_ang, 12);
        return 1.0f;
    }

    slot->last_seen = now;
    float f = 1.0f;
    if (slot->end_time > slot->start_time) {
        f = (float)((now - slot->start_time) / (slot->end_time - slot->start_time));
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
    }
    memcpy(old_org, slot->start_org, 12);
    memcpy(old_ang, slot->start_ang, 12);
    return f;
}

// C_BasePlayer::Interpolate (client.dll+0xA6B10), in place of its IsLocalPlayer() call: interpolate the local
// player too, but only once it has the two position samples that takes (split_player_interp.h).
static int __fastcall split_skip_player_interp(const BYTE *player) {
    return player_interp_ready(player) ? 0 : 1;
}

// True once client.dll has created the local player (C_BasePlayer::s_pLocalPlayer at client.dll+0x4A0D50,
// verified against FUN_10173350+0x35: mov eax,[s_pLocalPlayer]; test eax,eax; jz +0x14).
// Server-forwarded client commands (e.g. "createplayer" on sp_genesisdevice_1 tick 1) arrive over the pipe
// before the first UDP entity snapshot creates the local player; deferring them until the local player
// exists prevents Character Creation from skipping its panel and leaving a black screen + HUD.
static bool local_player_ready() {
    HMODULE cl = GetModuleHandleW(L"client.dll");
    if (!cl) return false;
    if (cl != s_checked_cl) {
        s_checked_cl = cl;
        s_offset_ok = false;
        const BYTE *p = (const BYTE *)cl + 0x173385;
        DWORD ptr_va = (DWORD)((uintptr_t)cl + 0x4A0D50);
        __try {
            s_offset_ok = (p[0] == 0xA1 && memcmp(p + 1, &ptr_va, 4) == 0 &&
                           p[5] == 0x85 && p[6] == 0xC0 && p[7] == 0x74 && p[8] == 0x14);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            s_offset_ok = false;
        }
    }
    if (!s_offset_ok) return true;
    return *(void *volatile *)((BYTE *)cl + 0x4A0D50) != nullptr;
}

// ---- once per engine frame (menu and in game), on the main thread (Host_RunFrame's Cbuf_Execute) ----
static void __cdecl client_frame() {
    s_frame_seen = true;
    // Advance monotonic, pause-aware, hitch-clamped render clock for continuous NPC interpolation.
    if (!*(volatile int *)0x20314874) {
        double dt = *(volatile double *)0x20B42978;
        if (dt > 0.0) {
            if (dt > 0.05) dt = 0.05;
            s_interp_clock += dt;
            s_interp_last_dt = dt;
        } else {
            s_interp_last_dt = 0.0;
        }
    } else {
        s_interp_last_dt = 0.0;
    }
    tune_engine_sockets();
    if (s_server) {
        uint32_t work = s_pending_work.load(std::memory_order_acquire);
        bool can_run_cmds = (*(volatile int *)kClsState == 4) && local_player_ready();
        uint32_t drain_mask = 5u | (can_run_cmds ? 2u : 0u);
        if (work & drain_mask) {
            std::vector<std::string> lines, cmds;
            std::string save_client;
            long save_id = 0;
            EnterCriticalSection(&s_cs);
            lines.swap(s_to_console);
            if (can_run_cmds) cmds.swap(s_from_server);
            save_client.swap(s_save_client_req);
            save_id = s_save_client_id;
            s_pending_work.fetch_and(~drain_mask, std::memory_order_release);
            LeaveCriticalSection(&s_cs);
            if (!save_client.empty()) {
                int saved_ok = 0;
                void *sr = *(void *volatile *)0x201A44E8;
                if (*(volatile int *)kClsState >= 3 && sr && *(void ***)sr == (void **)0x20174B3C &&
                    (*(void ***)sr)[14] == (void *)0x20096D10) {
                    if (g_settings.verbose) log("split: writing client state %s (id=%ld)", save_client.c_str(), save_id);
                    saved_ok = ((int(__fastcall *)(void *, void *, const char *))(*(void ***)sr)[14])(sr, nullptr, save_client.c_str()) ? 1 : 0;
                }
                char ack[64];
                if (save_id > 0) snprintf(ack, sizeof(ack), "memx:saveclient_done %ld %d", save_id, saved_ok);
                else snprintf(ack, sizeof(ack), "memx:saveclient_done");
                to_server(ack);
            }
            for (const std::string &l : lines) Con_Printf("%s", l.c_str());
            for (const std::string &c : cmds) {
                if (g_settings.verbose) log("split: from the server: %s", c.c_str());
                Cbuf_AddText((c + "\n").c_str());
            }
        }
        long failed_id = s_load_failed_id.exchange(0, std::memory_order_acq_rel);
        // Periodic watchdog & settings sync (avoids WaitForSingleObject syscalls every frame).
        if (s_want_game) {
            DWORD now = GetTickCount();
            static DWORD last_watch;
            bool dead = s_server_dead.load(std::memory_order_acquire);
            if (!dead && failed_id != 0 && failed_id == s_load_id) {
                InterlockedExchange(&s_server_loading, 0);
                Con_Printf("Memory Expansion: the server could not load that map or save.\n");
                if (s_load_expects_level && *(volatile int *)kClsState < 4) {
                    log("split: the server could not start the level; back to the menu");
                    s_want_game = false;
                    s_ready_for_connect = false;
                    s_preloaded_map.clear();
                    EnterCriticalSection(&s_cs);
                    s_preload_req.clear();
                    s_from_server.clear();
                    s_pending_work.fetch_and(~10u, std::memory_order_release);
                    LeaveCriticalSection(&s_cs);
                    // "disconnect" takes the loading screen down and shows the menu. The console stays as it
                    // is, as when a load fails in single player ("toggleconsole" opened it over the menu, or
                    // closed it on the player who typed the command there).
                    Cbuf_AddText("disconnect\n");
                }
            }
            if (!dead && s_want_game && now - last_watch >= 250) {
                DWORD frame_gap = last_watch ? (now - last_watch) : 0;
                last_watch = now;
                if (frame_gap > 3000) {
                    // System sleep/hibernate, window drag, or client-side hitch: do not blame the server.
                    DWORD reset_t = now ? now : 1;
                    s_alive_tick = reset_t;
                    s_start_tick = reset_t;
                    s_stuck_strikes = 0;
                } else {
                    DWORD quiet = server_quiet_ms();
                    DWORD limit = server_stuck_limit_ms();
                    if (quiet > limit) {
                        if (++s_stuck_strikes >= 4) {
                            end_stuck_server(quiet);
                            s_stuck_strikes = 0;
                            dead = true;
                        }
                    } else {
                        s_stuck_strikes = 0;
                        if (!server_usable()) {
                            dead = true;
                        } else if (s_alive_seen && s_restarts > 0) {
                            if (!s_healthy_since) s_healthy_since = now ? now : 1;
                            else if (now - s_healthy_since >= 300000) {
                                s_restarts = 0;
                                s_healthy_since = now ? now : 1;
                                log("split: server healthy for 5 min; restart counter reset");
                            }
                        }
                    }
                }
            }
            // If the server died mid-load, mid-save, or mid-game, drop cleanly back to the menu so a modal
            // loading screen cannot soft-lock forever; the next New Game or Load restarts the server.
            if (dead) {
                log("split: server process ended while a game was active; returning to menu");
                Con_Printf("Memory Expansion: the server process stopped; load a save or start a new game to restart it\n");
                close_dialog_window();
                s_want_game = false;
                s_ready_for_connect = false;
                s_preloaded_map.clear();
                EnterCriticalSection(&s_cs);
                s_preload_req.clear();
                s_save_client_req.clear();
                s_from_server.clear();
                s_pending_work.fetch_and(~14u, std::memory_order_release);
                LeaveCriticalSection(&s_cs);
                // back to the menu, with the console open so the message above is seen
                Cbuf_AddText("disconnect\ntoggleconsole\n");
            } else if (s_want_game) {
                // the options the player changes while playing (subtitles, ...)
                static DWORD last_sync, last_cmds_check;
                if (s_synced_built && !s_server_loading && *(volatile int *)kClsState >= 3 && now - last_sync > 500) {
                    last_sync = now;
                    sync_settings();
                    if (now - last_cmds_check > 2000 || *(BYTE **)kCommandList != s_sent_cmd_head) {
                        last_cmds_check = now;
                        if (int names = send_command_names()) log("split: %d new command/variable name(s) to the server process", names);
                    }
                }
            }
        }
        LONG spawns = s_server_spawns;
        if (s_want_game && spawns != s_spawns_seen) {
            // the server has a new level: connect immediately (or wake a pending cl_resend right now)
            s_spawns_seen = spawns;
            EnterCriticalSection(&s_cs);
            s_preload_req.clear();
            s_pending_work.fetch_and(~8u, std::memory_order_release);
            LeaveCriticalSection(&s_cs);
            s_ready_for_connect = true;
            int cls_state = *(volatile int *)kClsState;
            if (cls_state == 2) {
                *(double *)0x2020B650 = 0.0;   // cls.connect_time: CL_CheckForResend fires on this frame
                *(int *)0x2033D86C = 0;        // cls.connect_retry
            } else if (cls_state != 3) {
                connect_to_server();
            }
        } else if (s_want_game && s_preload_ok && (work & 8u) && !*(volatile int *)kSvActive) {
            int cls_state = *(volatile int *)kClsState;
            if (cls_state == 1 || cls_state == 2 || (cls_state == 4 && s_cl_disconnect_ok)) {
                std::string preload;
                EnterCriticalSection(&s_cs);
                preload.swap(s_preload_req);
                s_pending_work.fetch_and(~8u, std::memory_order_release);
                LeaveCriticalSection(&s_cs);
                if (!preload.empty() && preload != s_preloaded_map) {
                    if (cls_state == 4) {
                        // A level change just started on the server (after SaveClientState finished):
                        // show the loading screen and disconnect cleanly so we can preload in parallel.
                        close_dialog_window();
                        EnterCriticalSection(&s_cs);
                        s_from_server.clear();
                        s_pending_work.fetch_and(~2u, std::memory_order_release);
                        LeaveCriticalSection(&s_cs);
                        std::string short_map = preload;
                        if (short_map.rfind("maps/", 0) == 0) short_map = short_map.substr(5);
                        if (short_map.size() > 4 && !_stricmp(short_map.c_str() + short_map.size() - 4, ".bsp"))
                            short_map.resize(short_map.size() - 4);
                        show_loading_screen(short_map.c_str());
                        s_preloaded_map.clear();   // ensure CL_Disconnect flushes the old level's hunk
                        ((VoidFn_t)0x200267B0)();  // CL_Disconnect -> split_clear_state -> Host_FreeToLowMark
                    }
                    void *ml = *(void *volatile *)0x201A6774;   // g_pModelLoader
                    if (ml && *(void ***)ml == (void **)0x20174F74 && (*(void ***)ml)[5] == (void *)0x200B8930) {
                        DWORD t0 = GetTickCount();
                        if (cls_state != 4) {
                            // Release any old level's static props and hunk before preloading the incoming BSP.
                            void **sp_vt = *(void ***)kStaticProps;
                            if (sp_vt == (void **)0x2017549C && sp_vt[5] == (void *)0x200E9030 && *(volatile BYTE *)(kStaticProps + 92))
                                ((void(__fastcall *)(void *, void *))sp_vt[5])((void *)kStaticProps, nullptr);
                            ((void(__cdecl *)(int))0x2008F3D0)(0);   // Host_FreeToLowMark(false)
                        }
                        void *m = ((void *(__fastcall *)(void *, void *, const char *, int))(*(void ***)ml)[5])(ml, nullptr, preload.c_str(), 4);
                        if (m) {
                            s_preloaded_map = preload;
                            log("split: preloaded %s in %lu ms (parallel with server)", preload.c_str(), GetTickCount() - t0);
                        }
                    }
                }
            }
        }
    }
    // Enable zero-desync local-player interpolation in split mode (once per loaded client.dll).
    // Vanilla VTMB runs cl_predict 0 because client.dll's CGameMovement lacks Troika's VTMB extensions
    // (causing +/-0.15..0.43 unit per-tick prediction errors and visible A/D strafing jitter with cl_predict 1).
    // Replacing the IsLocalPlayer() early-out in C_BasePlayer::Interpolate (call at client+0xA6B16) with
    // cl_interp 0.025 smoothly interpolates authoritative 144 Hz server snapshots at full render FPS
    // with zero prediction snaps and zero cl_predictweapons side effects (mouse look stays 0 ms direct).
    // The early-out must stay for a player without position samples (just created: a level's first frames,
    // any full update). Versions up to 1.1.1 removed it altogether: the player was then put at whatever was
    // on the stack, weapons that follow it took that over, and two of them at NaN positions filled the
    // client's spatial partition ("CUtlLinkedList overflow!" at the end of a level change).
    if (s_server && !*(volatile int *)kSvActive) {
        HMODULE cl = GetModuleHandleW(L"client.dll");
        if (cl && cl != s_interp_client) {
            s_interp_client = cl;
            // call IsLocalPlayer; test al, al; jnz +5Bh: the call alone is redirected
            CodePatch local_interp = {
                0xA6B16,
                {0xE8, 0xB5, 0x2E, 0x00, 0x00, 0x84, 0xC0, 0x75, 0x5B},
                {0xE8, 0, 0, 0, 0, 0x84, 0xC0, 0x75, 0x5B},
                9,
                "split: smooth local-player interpolation"};
            DWORD local_rel = (DWORD)((uintptr_t)&split_skip_player_interp - ((uintptr_t)cl + 0xA6B16 + 5));
            memcpy(local_interp.repl + 1, &local_rel, 4);
            if (in_module_image(cl, 0xA6970, 0x200) && in_module_image(cl, 0xA1EF0, 0x100) &&
                player_history_code_ok_at((const unsigned char *)cl))
                apply_patch(cl, local_interp);
            else log("not patched (unknown bytes): split: smooth local-player interpolation");
            // C_BaseAnimating::Interpolate (MOVETYPE_STEP NPCs): replace call [edx+0x1E8] at client+0x94F62
            // (6 bytes: FF 92 E8 01 00 00) with call split_npc_interp_fraction; nop.
            CodePatch npc_interp = {
                0x94F62,
                {0xFF, 0x92, 0xE8, 0x01, 0x00, 0x00},
                {0xE8, 0, 0, 0, 0, 0x90},
                6,
                "split: zero-extrapolation NPC interpolation"};
            DWORD rel = (DWORD)((uintptr_t)&split_npc_interp_fraction - ((uintptr_t)cl + 0x94F62 + 5));
            memcpy(npc_interp.repl + 1, &rel, 4);
            apply_patch(cl, npc_interp);
            // C_ItemGKeyring::ReceiveMessage bounds guard (vtable slot client+0x1E4488 -> client+0x37F0).
            static const BYTE kKeyringHead[] = {0x8B, 0x44, 0x24, 0x04, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00, 0x56, 0x8B, 0xF1, 0x8B, 0x8C, 0x24};
            static const BYTE kKeyringRem[] = {0x3C, 0x7B, 0x75, 0x40, 0xE8, 0xC1, 0x0D, 0x11, 0x00, 0x8B, 0x8E, 0x7C, 0x09, 0x00, 0x00, 0x2B};
            void *expected_fn = (void *)((BYTE *)cl + 0x37F0);
            void **slot = (void **)((BYTE *)cl + 0x1E4488);
            if (in_module_image(cl, 0x37F0, sizeof(kKeyringHead)) &&
                in_module_image(cl, 0x38C6, sizeof(kKeyringRem)) &&
                in_module_image(cl, 0x1E4488, sizeof(void *)) &&
                memcmp((BYTE *)cl + 0x37F0, kKeyringHead, sizeof(kKeyringHead)) == 0 &&
                memcmp((BYTE *)cl + 0x38C6, kKeyringRem, sizeof(kKeyringRem)) == 0 &&
                (*slot == expected_fn || *slot == (void *)split_keyring_receive_message)) {
                s_keyring_recv_orig = (KeyringRecv_t)expected_fn;
                patch_vtable(slot, 0, (void *)split_keyring_receive_message);
                if (g_settings.verbose) log("split: keyring ReceiveMessage guard active");
            } else {
                log("not patched (unknown bytes): split: keyring ReceiveMessage guard");
            }
        }
    }
    Cbuf_Execute();
}

static volatile bool s_installed;   // this process plays on a server process (the commands above are redirected)

static bool read_ptr_safe(uintptr_t addr, void **out) {
    __try {
        *out = *(void *volatile *)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ConCommands are constructed while engine.dll initializes (its DllMain, which the Unofficial Patch loader
// runs later): wait for that, start the server so it loads while the menu is up, then redirect them.
static DWORD WINAPI setup_thread(void *) {
    int after_frame = 0;
    for (int i = 0; i < 12000; ++i) {
        if (GetModuleHandleW(L"engine.dll") != (HMODULE)0x20000000) {
            s_installed = false;
            return 0;
        }
        bool constructed = true;
        for (CommandHook &c : s_commands) {
            void *cur = nullptr;
            if (!read_ptr_safe(c.callback_field, &cur) || cur != (void *)c.original) constructed = false;
        }
        if (constructed) break;
        if (s_frame_seen && ++after_frame > 500) break;   // engine already running for 5 s: won't construct later
        Sleep(10);
    }
    // Essential commands (map, load) must match before launching the server process.
    void *cb0 = nullptr, *cb1 = nullptr;
    if (!read_ptr_safe(s_commands[0].callback_field, &cb0) || cb0 != (void *)s_commands[0].original ||
        !read_ptr_safe(s_commands[1].callback_field, &cb1) || cb1 != (void *)s_commands[1].original) {
        log("split: console commands do not match engine.dll 1.2; the game runs its server itself");
        s_installed = false;
        return 0;
    }
    if (!server_usable() && !start_server()) {
        s_installed = false;
        return 0;
    }
    int n = 0;
    for (CommandHook &c : s_commands) {
        void **field = (void **)c.callback_field;
        void *cur = nullptr;
        if (!read_ptr_safe(c.callback_field, &cur) || (cur != (void *)c.original && cur != (void *)c.replacement)) {
            log("split: \"%s\" not redirected (unknown engine)", c.name);
            continue;
        }
        if (cur == (void *)c.replacement) { ++n; continue; }
        DWORD prot;
        if (VirtualProtect(field, 4, PAGE_READWRITE, &prot)) {
            InterlockedExchange((volatile LONG *)field, (LONG)(uintptr_t)c.replacement);   // the main thread may read it now
            VirtualProtect(field, 4, prot, &prot);
            ++n;
        }
    }
    log("split: %d console commands go to the server process", n);
    return 0;
}

void install_split_client(HMODULE engine) {
    if (engine != (HMODULE)0x20000000) {
        log("split: engine.dll is not at its usual address; the game runs its server itself");
        return;
    }
    // Verify the required per-frame hook target BEFORE modifying any bytes in engine.dll.
    if (!in_module_image(engine, 0x8E555, 5)) {
        log("not patched (unknown bytes): split: per-frame hook");
        return;
    }
    {
        const BYTE *at = (const BYTE *)engine + 0x8E555;
        DWORD orig = (DWORD)((uintptr_t)engine + 0x2DF10 - (uintptr_t)(at + 5));
        DWORD want = (DWORD)((uintptr_t)client_frame - (uintptr_t)(at + 5));
        if (at[0] == 0xE8 && memcmp(at + 1, &want, 4) == 0 && s_installed) return;   // already installed on this mapping
        if (at[0] != 0xE8 || (memcmp(at + 1, &orig, 4) != 0 && memcmp(at + 1, &want, 4) != 0)) {
            log("not patched (unknown bytes): split: per-frame hook");
            return;
        }
    }
    ensure_cs();
    // Reset per-mapping cached pointers in case engine.dll was unloaded and remapped (engine restart).
    s_synced.clear();
    s_synced_built = false;
    s_sent_commands = false;
    s_sent_names = 0;
    s_sent_cmd_head = nullptr;
    s_sent_cmd_set.clear();
    s_interp_client = nullptr;
    s_checked_cl = nullptr;
    s_offset_ok = false;
    s_frame_seen = false;
    // Host_RunFrame's call to Cbuf_Execute (0x2008E555) -> client_frame
    if (!redirect_call(engine, 0x8E555, 0x2DF10, (void *)client_frame, "split: per-frame hook")) return;

    // CL_ParseServerInfo keeps 8 received snapshots for delta updates in a one-player game, 64 with more players
    // (cl.frames 0x203148A8 has room for 64): the server sends one every frame and deltas from up to 64 back
    // (split_server.cpp). 0x2002B392: dec eax; neg eax; sbb eax,eax; and eax,38h; add eax,8 -> mov eax,64.
    // Optional: without it the game still plays, with full updates.
    static const CodePatch kClientFrames = {0x2B392, {0x48, 0xF7, 0xD8, 0x1B, 0xC0, 0x83, 0xE0, 0x38, 0x83, 0xC0, 0x08},
                                            {0xB8, 0x40, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90}, 11, "split: 64 delta snapshots"};
    apply_patch(engine, kClientFrames);
    // Netchan_CanPacket: take the loopback path on the client too so signon acks/commands are not rate-limited
    static const CodePatch kClientNoRateLimit = {0xBC5C0, {0x83, 0x78, 0x04, 0x01, 0x75, 0x18},
                                                 {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}, 6, "split: client without rate limit"};
    apply_patch(engine, kClientNoRateLimit);
    // NET_SendLong (0x200C08DE): atomic 64KB loopback UDP datagrams (matches server, avoids Sleep(1) splits)
    static const CodePatch kClientAtomicUdp = {0xC08DE, {0x81, 0xFB, 0x78, 0x05, 0x00, 0x00},
                                               {0x81, 0xFB, 0xF0, 0xFB, 0x00, 0x00}, 6, "split: atomic 64KB loopback UDP datagrams"};
    apply_patch(engine, kClientAtomicUdp);
    // CL_ParseTime / svc_time (0x2002CE38): skip the maxclients == 1 hard-snap of cl.time = cl.mtime[0]
    // so UDP arrival jitter uses smooth clock-drift correction instead of jumping cl.time backward.
    static const CodePatch kSmoothClock = {0x2CE38, {0x83, 0x3D, 0xDC, 0x5B, 0x31, 0x20, 0x01, 0x75, 0x16},
                                           {0x83, 0x3D, 0xDC, 0x5B, 0x31, 0x20, 0x01, 0xEB, 0x16}, 9, "split: smooth client clock drift"};
    apply_patch(engine, kSmoothClock);
    s_loading_screen = in_module_image(engine, 0x871B0, sizeof(kSetLoadingMapBytes)) &&
                       in_module_image(engine, 0x86A80, sizeof(kBeginPlaqueBytes)) &&
                       in_module_image(engine, 0x90EC9, sizeof(kMapValidSite)) &&
                       !memcmp((void *)0x200871B0, kSetLoadingMapBytes, sizeof(kSetLoadingMapBytes)) &&
                       !memcmp((void *)0x20086A80, kBeginPlaqueBytes, sizeof(kBeginPlaqueBytes)) &&
                       !memcmp((void *)0x20090EC9, kMapValidSite, sizeof(kMapValidSite));
    s_cl_disconnect_ok = in_module_image(engine, 0x267B0, sizeof(kClDisconnectBytes)) &&
                         !memcmp((void *)0x200267B0, kClDisconnectBytes, sizeof(kClDisconnectBytes));
    if (!s_loading_screen) log("split: loading screen not shown (unknown engine code)");
    static const BYTE kFpsMaxRead[] = {0x8B, 0x0D, 0x84, 0x28, 0xB4, 0x20, 0x8B, 0x51, 0x28};
    if (in_module_image(engine, 0x8BA5F, sizeof(kFpsMaxRead)) &&
        memcmp((BYTE *)engine + 0x8BA5F, kFpsMaxRead, sizeof(kFpsMaxRead)) == 0)
        redirect_call(engine, 0x8E499, 0x8BA30, (void *)client_filter_time, "split: fast signon frames");
    static const BYTE kDecalCountRead[] = {0x8B, 0x35, 0xA4, 0x19, 0xA4, 0x20};
    if (in_module_image(engine, 0x4A7B9, sizeof(kDecalCountRead)) &&
        memcmp((BYTE *)engine + 0x4A7B9, kDecalCountRead, sizeof(kDecalCountRead)) == 0)
        redirect_call(engine, 0x968D9, 0xF8870, (void *)split_restore_precache_decal, "split: restore map decals");
    redirect_call(engine, 0x2EC13, 0xDAB20, (void *)split_python, "split: console Python to the server");
    // CL_ClearState's three callers (CL_Disconnect, CL_ParseServerInfo, Host_Disconnect's helper)
    redirect_call(engine, 0x26879, 0x26570, (void *)split_clear_state, "split: static props released (disconnect)");
    redirect_call(engine, 0x2B263, 0x26570, (void *)split_clear_state, "split: static props released (new level)");
    redirect_call(engine, 0x8F3E2, 0x26570, (void *)split_clear_state, "split: static props released (end of game)");
    s_preload_ok = redirect_call(engine, 0x26673, 0x8F3D0, (void *)split_free_world, "split: keep preloaded BSP");
    redirect_call(engine, 0x2A184, 0x28DE0, (void *)split_cl_get_sound, "split: bounds-check client sound table");
    s_installed = true;
    if (HANDLE t = CreateThread(nullptr, 0, setup_thread, nullptr, 0, nullptr)) CloseHandle(t);
}

} // namespace memx

// For BLSE's split link (BLSE src/split_link.cpp), which lets the BLSE copies of the two processes talk:
// 1 = this is the game's server process (-memx_server), 2 = this is the game process and plays on a server
// process (whether that one runs yet or not), 0 = no split (single player).
extern "C" __declspec(dllexport) int __cdecl memx_split_role() {
    if (memx::server_mode()) return 1;
    return (memx::s_installed && !memx::s_split_fallback) ? 2 : 0;
}
