// When has a "map", "load", "changelevel", "reload" or "restart" command failed? (split_server.cpp; no
// engine or Windows calls here, so offline unit tests can drive it.)
//
// The engine never starts a level inside the command. The text waits in the command buffer until a host
// frame passes Host_FilterTime (RunFrame returns without one while fps_max holds it back); the command then
// only writes the next state of CHostState (engine.dll 0x20D5E0B0: +0 current state, +4 next state); at the
// end of that frame the state machine goes to "shut the game down", and the level itself is loaded at the
// start of a later RunFrame. So a number of RunFrame calls without a new level says nothing. The command
// is over only when the command buffer is empty and both host states are back at "run", and it has failed
// only if no level runs that was not there before.
#pragma once

namespace memx {

struct LoadWatch {
    static constexpr int kHostRun = 4;                 // CHostState: HS_RUN, nothing left to do
    // The engine must stay at rest this long without a new level: a plugin that hooks the level commands
    // (to fade the sound or the picture out first) may hold one back for a moment and queue it again.
    static constexpr unsigned long kRestMs = 1000;

    bool pending = false;
    long id = 0;               // the game's number for the command ("memx:loadid"; 0: sent without one)
    int spawn_before = 0;      // svs.spawncount when the command was queued
    bool resting = false;
    unsigned long rest_since = 0;

    void begin(long command_id, int spawncount) {
        pending = true;
        id = command_id;
        spawn_before = spawncount;
        resting = false;
    }

    // After each RunFrame. True once for a command that ended without a level.
    bool failed(bool cbuf_empty, int host_state, int host_next_state, bool sv_active, int spawncount, unsigned long now_ms) {
        if (!pending) return false;
        if (!cbuf_empty || host_state != kHostRun || host_next_state != kHostRun) {
            resting = false;   // the command, or the level load it asked for, is still to come
            return false;
        }
        if (sv_active && spawncount != spawn_before) {
            pending = false;   // the level is up ("memx:ready")
            return false;
        }
        if (!resting) {
            resting = true;
            rest_since = now_ms;
            return false;
        }
        if (now_ms - rest_since < kRestMs) return false;
        pending = false;
        return true;
    }
};

// a console line that asks the engine for a level
inline bool is_level_command(const char *line) {
    static const char *const kVerbs[] = {"map", "load", "changelevel", "changelevel2", "reload", "restart"};
    while (*line == ' ' || *line == '\t') ++line;
    for (const char *verb : kVerbs) {
        const char *a = line, *b = verb;
        while (*b && (*a | 0x20) == *b) { ++a; ++b; }
        if (!*b && (*a == 0 || *a == ' ' || *a == '\t')) return true;
    }
    return false;
}

} // namespace memx
