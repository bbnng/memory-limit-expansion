// The local player's movement in a split game, as smooth as the game's own frames.
// (split_client.cpp feeds this from the engine; no Windows or engine code here, so it can be tested alone.)
//
// The server moves the player only by the commands the game sends, one per game frame, each with its own number
// of milliseconds. The server's frames and the game's frames are not in step, so a snapshot holds the player after
// no, one or two further commands: in *server* time the player's speed is uneven, and interpolating between
// snapshots by their server times (or by a client clock that chases them) shows exactly that.
// In *command* time the movement is even: after the commands up to number n the player is where those
// milliseconds of movement put them, whenever that happened to run. So:
//   - MoveClock numbers the game's own time and knows up to which moment each command goes;
//   - MoveTimeline stores each snapshot's position at the moment of the last command it contains and shows the
//     player a constant, small delay behind the game's own time.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace memx {

// ---- command clock ----
struct MoveClock {
    static constexpr int kStamps = 256;
    static constexpr double kMaxBehind = 0.100;   // seconds between the game's time and the commands' before starting over

    double wall = 0.0;          // the game's own time
    double sent = 0.0;          // up to where the commands sent so far go
    double drift = 0.0;         // smoothed: server's clock minus this player's command clock on the server (seconds)
    double stamps[kStamps] = {};
    int stamp_seq[kStamps] = {};
    int last_seq = 0;           // the newest command looked at
    bool running = false;

    void reset() { *this = MoveClock(); }

    // One game frame of dt seconds. drift_ms: what the server reported since the last frame (0 = nothing new).
    // The server reports, with the noise of its own frames, how far the player's commands are from its clock; the
    // game answers by making single commands a millisecond longer or shorter. Only the lasting part of that is
    // real (two processes' clocks, a long frame): it goes into the game's time here, at most 2% of it.
    void advance(double dt, int drift_ms, bool paused) {
        if (!(dt > 0.0)) return;
        if (drift_ms != 0 && !paused) {
            double d = drift_ms * 0.001;
            if (d > 0.1) d = 0.1;
            if (d < -0.1) d = -0.1;
            drift += (d - drift) * 0.03;
        }
        double adjust = drift * dt;
        const double most = dt * 0.02;
        if (adjust > most) adjust = most;
        if (adjust < -most) adjust = -most;
        if (paused) adjust = 0.0;
        drift -= adjust;
        wall += dt + adjust;
        if (fabs(wall - sent) > kMaxBehind + dt) wall = sent + dt;   // a frame longer than any command
    }

    // What the next command should be made longer by (-1, 0 or 1 ms) so that the commands keep up with the game's
    // time. CInput::CreateMove turns the frame's float seconds into floor(ms + 0.5).
    int command_extra(double dt) const {
        double base = floor((double)((float)dt * 1000.0f) + 0.5);
        double want = (wall - sent) * 1000.0;
        double extra = want - base;
        if (extra > 0.5) return 1;
        if (extra < -0.5 && base > 1.0) return -1;
        return 0;
    }

    // Command number seq was sent with this many milliseconds (msec < 0: that packet held no command).
    void command(int seq, int msec) {
        if (msec > 0) sent += msec * 0.001;
        stamps[seq & (kStamps - 1)] = sent;
        stamp_seq[seq & (kStamps - 1)] = seq;
        last_seq = seq;
        running = true;
    }

    // The moment the commands up to number seq go to.
    bool stamp(int seq, double &t) const {
        if (!running || seq <= 0 || stamp_seq[seq & (kStamps - 1)] != seq) return false;
        t = stamps[seq & (kStamps - 1)];
        return true;
    }
};

// ---- the player's positions in command time ----
struct MovePose {
    float org[3];
    float ang[3];
    float view[3];   // m_vecViewOffset (the eyes above the origin: crouching)
};

struct MoveTimeline {
    static constexpr int kHistory = 64;
    static constexpr int kLags = 512;
    static constexpr double kLagWindow = 1.5;        // seconds the delay looks back
    static constexpr double kMaxDelay = 0.060;
    static constexpr double kExtrapolate = 0.012;    // seconds past the newest position, one late snapshot
    static constexpr double kBlendTime = 0.100;      // a jump that is no teleport is spread over this time
    static constexpr float kBlendLimit = 64.0f;      // units; more is shown at once

    struct Sample {
        double t;
        MovePose pose;
    };
    Sample history[kHistory];
    int count = 0;                 // oldest first
    struct Lag {
        double t;
        float lag;
    };
    Lag lags[kLags];
    int lag_head = 0, lag_count = 0;
    double delay = 0.0;
    bool delay_known = false;
    // what the last frame showed
    bool shown = false;
    double shown_t = 0.0;
    bool shown_guess = false;      // past the newest position
    MovePose shown_pose;
    bool shown_carried = false;
    // an offset that fades out
    float blend_org[3] = {}, blend_view[3] = {};
    double blend_age = kBlendTime;
    // carried by something (a lift): the player moves without a speed of their own
    bool carried = false;
    int carried_yes = 0, carried_no = 0;

    void reset() { *this = MoveTimeline(); }

    // a teleport: nothing before it counts (the delay stays)
    void restart() {
        count = 0;
        shown = false;
        shown_guess = false;
        blend_age = kBlendTime;
        carried = false;
        carried_yes = carried_no = 0;
    }

    // A snapshot's position, after the commands up to moment t. True when it starts a new path (teleport).
    bool add(double t, const MovePose &pose, const float vel[3], bool teleport) {
        if (count > 0) {
            Sample &newest = history[count - 1];
            const double dt = t - newest.t;
            const float dist = distance(pose.org, newest.pose.org);
            if (teleport || dt < -0.0005 || dist > 48.0f + reach_speed(vel) * (float)(dt > 0.0 ? dt : 0.0) || !finite(pose)) {
                restart();
                if (!finite(pose)) return true;
                push(t, pose);
                return true;
            }
            update_carried(dist, vel);
            if (dt < 0.0005) {   // no further command: the world moved the player
                newest.pose = pose;
                return false;
            }
            // movement that starts after the world stood still (a pause) does not stretch back over that time
            if (dt > 0.1 && dist < 8.0f) {
                Sample hold = newest;
                hold.t = t - 0.02;
                push(hold.t, hold.pose);
            }
            push(t, pose);
            return false;
        }
        if (!finite(pose)) return true;
        push(t, pose);
        return true;
    }

    // What to show at the game's time now, dt after the last frame. False: not yet known (use the game's own way).
    bool show(double now, double dt, bool paused, MovePose &out) {
        if (count == 0) return false;
        const double lag = now - history[count - 1].t;
        if (!paused) note_lag(now, lag);
        double target;
        if (!wanted_delay(now, target)) {
            if (!delay_known) return false;
        } else if (!delay_known) {
            delay = target;
            delay_known = true;
        } else if (target > delay) {
            delay = target - delay > dt * 0.05 ? delay + dt * 0.05 : target;
        } else {
            delay = delay - target > dt * 0.02 ? delay - dt * 0.02 : target;
        }
        double rt = now - delay;
        if (shown && rt < shown_t) rt = shown_t - rt > 0.25 ? rt : shown_t;

        bool guess = false;
        MovePose pose;
        if (carried) pose = history[count - 1].pose;   // in step with what carries the player, drawn where it is
        else pose = at(rt, guess);

        if (blend_age < kBlendTime) blend_age += dt;
        if (shown && !shown_carried && !carried && shown_guess) {
            // the last frame went past what was known: continue from there, not from where it should have been
            bool g2 = false;
            MovePose should = at(shown_t, g2);
            if (!g2) start_blend(should);
        } else if (shown && shown_carried != carried) {
            start_blend(pose);
        }
        float left = blend_left();
        if (left > 0.0f) {
            for (int i = 0; i < 3; ++i) {
                pose.org[i] += blend_org[i] * left;
                pose.view[i] += blend_view[i] * left;
            }
        }
        shown = true;
        shown_t = rt;
        shown_guess = guess;
        shown_carried = carried;
        shown_pose = pose;
        out = pose;
        return true;
    }

    // animation time from the newest snapshot back to what is shown (0 or less)
    double shown_behind() const {
        if (!shown || carried || count == 0) return 0.0;
        double d = shown_t - history[count - 1].t;
        if (d > 0.0) d = 0.0;
        if (d < -0.1) d = -0.1;
        return d;
    }

    static float distance(const float a[3], const float b[3]) {
        float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
        return sqrtf(x * x + y * y + z * z);
    }

    // How fast the player can have gone between two snapshots (units a second) before it counts as a teleport:
    // walking and falling stay under 2500; noclip goes as fast as sv_noclipspeed (2000 by default, any value the
    // player types), so the player's own speed counts too, with room for acceleration.
    static float reach_speed(const float vel[3]) {
        const float speed = vel ? sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]) : 0.0f;
        const float fast = std::isfinite(speed) ? 1.5f * speed : 0.0f;
        return fast > 2500.0f ? (fast < 30000.0f ? fast : 30000.0f) : 2500.0f;
    }

private:
    static bool finite(const MovePose &p) {
        for (int i = 0; i < 3; ++i)
            if (!std::isfinite(p.org[i]) || !std::isfinite(p.ang[i]) || !std::isfinite(p.view[i])) return false;
        return true;
    }

    void push(double t, const MovePose &pose) {
        if (count == kHistory) {
            memmove(history, history + 1, sizeof(Sample) * (kHistory - 1));
            --count;
        }
        history[count].t = t;
        history[count].pose = pose;
        ++count;
    }

    void update_carried(float moved, const float vel[3]) {
        const float speed = sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
        if (speed < 1.0f && moved > 0.02f) {
            carried_no = 0;
            if (++carried_yes >= 3) carried = true;
        } else {
            carried_yes = 0;
            if (speed >= 1.0f || ++carried_no >= 5) {
                carried = false;
                carried_no = 0;
            }
        }
    }

    // the delay that leaves the newest position behind what is shown in all but the rarest frames
    void note_lag(double now, double lag) {
        if (lag < 0.0) lag = 0.0;
        if (lag > 0.5) lag = 0.5;
        lags[lag_head].t = now;
        lags[lag_head].lag = (float)lag;
        lag_head = (lag_head + 1) & (kLags - 1);
        if (lag_count < kLags) ++lag_count;
    }
    bool wanted_delay(double now, double &target) const {
        float top[9] = {};   // the largest, descending
        int n = 0;
        for (int i = 1; i <= lag_count; ++i) {
            const Lag &l = lags[(lag_head - i) & (kLags - 1)];
            if (now - l.t > kLagWindow || now < l.t - 0.001) break;
            ++n;
            if (l.lag > top[8]) {
                int j = 8;
                while (j > 0 && top[j - 1] < l.lag) {
                    top[j] = top[j - 1];
                    --j;
                }
                top[j] = l.lag;
            }
        }
        if (n < 8) return false;
        int skip = n / 64;   // 1.5%: a single late frame is no reason to show everything later
        if (skip < 2) skip = 2;
        if (skip > 8) skip = 8;
        if (n < 24) skip = 1;
        target = top[skip] + 0.0005;
        if (target > kMaxDelay) target = kMaxDelay;
        return true;
    }

    MovePose at(double t, bool &guess) const {
        guess = false;
        const Sample &newest = history[count - 1];
        if (t >= newest.t) {
            MovePose p = newest.pose;
            double past = t - newest.t;
            if (past <= 1e-9 || count < 2) return p;
            guess = true;
            // going on at the same speed for the few milliseconds a snapshot is late
            const Sample &prev = base_for_speed();
            const double span = newest.t - prev.t;
            if (span > 0.0005 && span < 0.1) {
                if (past > kExtrapolate) past = kExtrapolate;
                const float f = (float)(past / span);
                for (int i = 0; i < 3; ++i) {
                    p.org[i] += (newest.pose.org[i] - prev.pose.org[i]) * f;
                    p.view[i] += (newest.pose.view[i] - prev.pose.view[i]) * f;
                }
            }
            return p;
        }
        int i = count - 1;
        while (i > 0 && history[i - 1].t > t) --i;
        if (i == 0) return history[0].pose;
        const Sample &a = history[i - 1], &b = history[i];
        const double span = b.t - a.t;
        const float f = span > 1e-9 ? (float)((t - a.t) / span) : 1.0f;
        MovePose p;
        for (int k = 0; k < 3; ++k) {
            p.org[k] = a.pose.org[k] + (b.pose.org[k] - a.pose.org[k]) * f;
            p.view[k] = a.pose.view[k] + (b.pose.view[k] - a.pose.view[k]) * f;
            float d = fmodf(b.pose.ang[k] - a.pose.ang[k], 360.0f);
            if (d > 180.0f) d -= 360.0f;
            if (d < -180.0f) d += 360.0f;
            p.ang[k] = a.pose.ang[k] + d * f;
        }
        return p;
    }
    const Sample &base_for_speed() const {
        int i = count - 2;
        while (i > 0 && history[count - 1].t - history[i].t < 0.006) --i;
        return history[i];
    }

    float blend_left() const {
        if (blend_age >= kBlendTime) return 0.0f;
        const float x = (float)(blend_age / kBlendTime);
        return 1.0f - x * x * (3.0f - 2.0f * x);
    }
    void start_blend(const MovePose &should) {
        float org[3], view[3];
        for (int i = 0; i < 3; ++i) {
            org[i] = shown_pose.org[i] - should.org[i];
            view[i] = shown_pose.view[i] - should.view[i];
        }
        const float zero[3] = {};
        const float off = distance(org, zero);
        if (off > kBlendLimit) {
            blend_age = kBlendTime;
            return;
        }
        // (an offset that is still fading is part of what was shown)
        memcpy(blend_org, org, sizeof(org));
        memcpy(blend_view, view, sizeof(view));
        blend_age = 0.0;
    }
};

// ---- the client clock (cl.time) ----
// CL_AdjustClock moves cl.time by up to 7.5 ms in every frame towards the server time of the snapshot that came
// last, wherever in that frame it came. cl.time is what everything else that moves on the screen is shown by: it
// must follow the server's clock, not the packets' arrival.
struct ClockFilter {
    double error = 0.0;    // smoothed: cl.time minus server time
    void reset() { error = 0.0; }
    // sample: cl.time minus the newest snapshot's time (0 = no new snapshot). Returns what to take off cl.time.
    double correction(double sample, double dt) {
        if (sample != 0.0) {
            if (fabs(sample) > 0.025 && fabs(sample - error) > 0.025) {   // a hitch, a level's first frames
                error = 0.0;
                return sample * 0.5;
            }
            error += (sample - error) * 0.06;
        }
        double part = dt * 4.0;
        if (part > 0.2) part = 0.2;
        if (part < 0.0) part = 0.0;
        const double c = error * part;
        error -= c;
        return c;
    }
};

} // namespace memx
