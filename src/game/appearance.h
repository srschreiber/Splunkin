#pragma once
#include <cstdint>
#include <cstdio>

// Player appearance: skin tint, a hand-drawn pixel face, and silly bone scales. Plain
// data so it serializes trivially for networking, and persists to disk so a player's
// look loads as the default next launch. App-level data, kept out of the engine.

namespace dc::game {

inline constexpr int FACE_W = 16, FACE_H = 16, FACE_N = FACE_W * FACE_H;
inline constexpr int PALETTE_N = 8;   // index 0 = blank (no pixel); 1..7 = ink colors

// Fixed pen palette (index 0 is "blank" / erase). Kept small so the picker is simple.
inline void palette_rgb(int idx, float& r, float& g, float& b) {
    static const float P[PALETTE_N][3] = {
        {0.0f, 0.0f, 0.0f},     // 0 blank (unused as ink; treated as erase)
        {0.04f, 0.04f, 0.05f},  // 1 black
        {0.97f, 0.97f, 0.97f},  // 2 white
        {0.90f, 0.20f, 0.20f},  // 3 red
        {0.25f, 0.55f, 1.00f},  // 4 blue
        {0.30f, 0.85f, 0.35f},  // 5 green
        {1.00f, 0.85f, 0.20f},  // 6 yellow
        {0.70f, 0.35f, 0.85f},  // 7 purple
    };
    const int i = (idx < 0 || idx >= PALETTE_N) ? 0 : idx;
    r = P[i][0]; g = P[i][1]; b = P[i][2];
}

// A few skin tones to pick from (also used as swatches in the customizer).
inline constexpr int SKIN_N = 8;
inline void skin_rgb(int idx, float& r, float& g, float& b) {
    static const float S[SKIN_N][3] = {
        {0.95f, 0.80f, 0.66f},  // light
        {0.85f, 0.66f, 0.50f},  // tan
        {0.66f, 0.48f, 0.34f},  // brown
        {0.42f, 0.28f, 0.20f},  // dark
        {0.55f, 0.75f, 0.55f},  // goblin green
        {0.55f, 0.65f, 0.85f},  // frost blue
        {0.85f, 0.55f, 0.65f},  // rose
        {0.75f, 0.75f, 0.80f},  // ashen
    };
    const int i = (idx < 0 || idx >= SKIN_N) ? 0 : idx;
    r = S[i][0]; g = S[i][1]; b = S[i][2];
}

// One player's look. Bone scales let players make goofy proportions.
struct Appearance {
    float   skin[3]   = {0.85f, 0.66f, 0.50f};   // body tint
    uint8_t face[FACE_N] = {0};                  // palette index per cell (row-major; 0 = blank)
    float   bone_head  = 1.0f;                   // silly proportion multipliers
    float   bone_arms  = 1.0f;
    float   bone_hands = 1.0f;
    float   bone_torso = 1.0f;
};

inline constexpr uint32_t APPEARANCE_MAGIC = 0xC0DEFACEu;

inline bool save_appearance(const Appearance& a, const char* path) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const uint32_t magic = APPEARANCE_MAGIC;
    std::fwrite(&magic, 4, 1, f);
    std::fwrite(a.skin, sizeof a.skin, 1, f);
    std::fwrite(a.face, sizeof a.face, 1, f);
    const float bs[4] = { a.bone_head, a.bone_arms, a.bone_hands, a.bone_torso };
    std::fwrite(bs, sizeof bs, 1, f);
    std::fclose(f);
    return true;
}

inline bool load_appearance(Appearance& a, const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    uint32_t magic = 0;
    bool ok = std::fread(&magic, 4, 1, f) == 1 && magic == APPEARANCE_MAGIC;
    if (ok) ok = std::fread(a.skin, sizeof a.skin, 1, f) == 1;
    if (ok) ok = std::fread(a.face, sizeof a.face, 1, f) == 1;
    float bs[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (ok) std::fread(bs, sizeof bs, 1, f);   // bone scales are optional/newer; default to 1
    a.bone_head = bs[0]; a.bone_arms = bs[1]; a.bone_hands = bs[2]; a.bone_torso = bs[3];
    std::fclose(f);
    return ok;
}

} // namespace dc::game
