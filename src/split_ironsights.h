// The Unofficial Patch's iron sights plugin (vtmb-ironsights.vtm) in a split game.
//
// The plugin writes its code into unused space of the two game DLLs in memory:
//   vampire.dll+0xAAC000: the console commands ironsights, +ironsights, -ironsights, which set the "sights up"
//       flag (+0xAAC000), and the code that multiplies the player's weapon spread by +0xAAC400. The flag at
//       +0xAAC004 makes the next player update send the new spread to the crosshair.
//   client.dll+0x6D0000: the view. Each frame it reads the "sights up" flag out of *its own process's*
//       vampire.dll, moves the weapon model, changes the field of view, and writes the spread multiplier back
//       there (+0xAAC400, +0xAAC004).
// Both halves expect one process. In a split game:
//   - the commands belong to vampire.dll (FCVAR_GAMEDLL), so the game process's Cmd_ExecuteString sent them to
//     the server process, and the flag the view reads never changed: aiming did nothing;
//   - the multiplier the view works out stayed in the game process: the world never shot more accurately.
// So the game process runs the three commands itself and tells the server process the multiplier.
#pragma once
#include "common.h"
#include <cstring>

namespace memx {

constexpr DWORD kIronSightsToggle = 0xAAC200;   // the three console commands' callbacks (void, cdecl, no arguments)
constexpr DWORD kIronSightsOn = 0xAAC212;
constexpr DWORD kIronSightsOff = 0xAAC227;
constexpr DWORD kIronSightsResend = 0xAAC004;   // the next player update sends the spread to the crosshair
constexpr DWORD kIronSightsSpread = 0xAAC400;   // float, 1.0 without sights
constexpr DWORD kIronSightsNoSpreadChange = 0x3F800000;   // 1.0f

// True when this vampire.dll carries the plugin code described above.
inline bool ironsights_code_ok(HMODULE vampire) {
    if (!vampire || !in_module_image(vampire, 0xAAC000, 0x1000) || !in_module_image(vampire, 0x176048, 0x25)) return false;
    const BYTE *b = (const BYTE *)vampire;
    auto refers_to = [b](DWORD at, DWORD target) {
        DWORD v;
        memcpy(&v, b + at, 4);
        return v == (DWORD)(uintptr_t)(b + target);
    };
    static const BYTE kUpdateA[] = {0xE8, 0x16, 0x62, 0x93, 0x00, 0x90};   // call +0xAAC263
    static const BYTE kUpdateB[] = {0xE8, 0x21, 0x62, 0x93, 0x00, 0x90};   // call +0xAAC28D
    static const char kNames[] = "ironsights\0+ironsights\0-ironsights";
    __try {
        return memcmp(b + 0x176048, kUpdateA, sizeof(kUpdateA)) == 0 && memcmp(b + 0x176067, kUpdateB, sizeof(kUpdateB)) == 0 &&
               memcmp(b + 0xAAC800, kNames, sizeof(kNames)) == 0 &&
               // xor [AAC000],1 / mov [AAC000],1 / mov [AAC000],0, each followed by mov [AAC004],1; ret
               b[0xAAC200] == 0x83 && b[0xAAC201] == 0x35 && refers_to(0xAAC202, 0xAAC000) && b[0xAAC206] == 1 &&
               b[0xAAC207] == 0xC7 && refers_to(0xAAC209, 0xAAC004) && b[0xAAC211] == 0xC3 &&
               b[0xAAC212] == 0xC7 && refers_to(0xAAC214, 0xAAC000) && b[0xAAC218] == 1 &&
               b[0xAAC21C] == 0xC7 && refers_to(0xAAC21E, 0xAAC004) && b[0xAAC226] == 0xC3 &&
               b[0xAAC227] == 0xC7 && refers_to(0xAAC229, 0xAAC000) && b[0xAAC22D] == 0 &&
               b[0xAAC231] == 0xC7 && refers_to(0xAAC233, 0xAAC004) && b[0xAAC23B] == 0xC3 &&
               // fmul [AAC400] (the spread of a shot and of the crosshair)
               b[0xAAC25C] == 0xD8 && b[0xAAC25D] == 0x0D && refers_to(0xAAC25E, 0xAAC400) &&
               b[0xAAC29C] == 0xD8 && b[0xAAC29D] == 0x0D && refers_to(0xAAC29E, 0xAAC400);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The same, remembered for one mapping of vampire.dll (the plugin patches it before its code first runs).
inline BYTE *ironsights_vampire() {
    static HMODULE s_checked;
    static bool s_ok;
    HMODULE v = GetModuleHandleW(L"vampire.dll");
    if (v != s_checked) {
        s_checked = v;
        s_ok = ironsights_code_ok(v);
        if (s_ok) log("%s: iron sights plugin found in vampire.dll", server_mode() ? "server" : "split");
    }
    return s_ok ? (BYTE *)v : nullptr;
}

} // namespace memx
