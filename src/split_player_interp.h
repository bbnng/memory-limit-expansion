// When may C_BasePlayer::Interpolate (client.dll+0xA6B10) move a player to its interpolated position?
// (split_client.cpp; also driven by tests/player_interp_test.cpp against the real client.dll code.)
//
// The game never interpolates the local player, so this path only ever saw remote players:
//
//     Vector org; QAngle ang;                       // not initialised
//     GetInterpolatedPosition(t, &org, &ang);       // client.dll+0xA6970
//     SetLocalAngles(ang); SetLocalOrigin(org);
//
// GetInterpolatedPosition leaves both outputs alone when GetInterpolationInfo (client.dll+0xA1EF0) finds no
// samples to interpolate between: fewer than two in the position history (a player that was just created has
// one, from its first update), or a first sample without a time.
#pragma once
#include <cmath>
#include <cstring>

namespace memx {

// C_BaseEntity: position history, a CUtlVector of {float time; Vector origin; QAngle angles} (28 bytes each)
constexpr int kPosHistoryData = 0x130;
constexpr int kPosHistoryCount = 0x13C;

inline bool player_interp_ready(const unsigned char *player) {
    if (!player) return false;
    int count;
    const unsigned char *entries;
    std::memcpy(&count, player + kPosHistoryCount, sizeof(count));
    if (count < 2) return false;
    std::memcpy(&entries, player + kPosHistoryData, sizeof(entries));
    if (!entries) return false;
    float first_time;
    std::memcpy(&first_time, entries, sizeof(first_time));
    return std::isfinite(first_time) && first_time != 0.0f;
}

// The code the rules above were read from: GetInterpolationInfo's history test (mov edi, [eax+13Ch]; cmp edi, 2)
// and entry loop (mov edx, [eax+130h]; add edx, ebp; fld [edx]), and GetInterpolatedPosition's early-out
// (test edx, edx; jz: no "from" sample).
inline bool player_history_code_ok_at(const unsigned char *base) {
    static const unsigned char kCount[] = {0x8B, 0xB8, 0x3C, 0x01, 0x00, 0x00, 0x83, 0xFF, 0x02};
    static const unsigned char kEntries[] = {0x8B, 0x90, 0x30, 0x01, 0x00, 0x00, 0x03, 0xD5, 0xD9, 0x02};
    static const unsigned char kNoSample[] = {0x85, 0xD2, 0x0F, 0x84, 0x1E, 0x01, 0x00, 0x00};
    return std::memcmp(base + 0xA1F0D, kCount, sizeof(kCount)) == 0 &&
           std::memcmp(base + 0xA1F3D, kEntries, sizeof(kEntries)) == 0 &&
           std::memcmp(base + 0xA698B, kNoSample, sizeof(kNoSample)) == 0;
}

} // namespace memx
