// How long a client has been active, counted only while its packets arrive (split_server.cpp; no engine or
// Windows calls here, so offline unit tests can drive it.)
//
// A client that loads textures or compiles shaders on its first frame sends nothing for seconds. Plain
// elapsed time would already be over a limit at its first packet after that, before it could answer
// anything; so a gap between two packets counts for at most kMaxGapMs.
#pragma once

namespace memx {

struct ActivityTimer {
    static constexpr unsigned long kMaxGapMs = 100;

    bool started = false;
    int level = 0;                // svs.spawncount the time belongs to
    unsigned long last = 0;       // GetTickCount() of the last packet
    unsigned long active_ms = 0;

    void reset() { *this = ActivityTimer(); }

    // a packet arrived: the active time so far
    unsigned long packet(int spawncount, unsigned long now) {
        if (!started || level != spawncount) {
            started = true;
            level = spawncount;
            active_ms = 0;
        } else {
            unsigned long gap = (now - last) & 0xFFFFFFFFul;
            active_ms += gap < kMaxGapMs ? gap : kMaxGapMs;
        }
        last = now;
        return active_ms;
    }
};

} // namespace memx
