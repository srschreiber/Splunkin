#pragma once

// Enemy taunts: a small bank of (deliberately crude) insults sampled at random when an
// enemy attacks a nearby player. Kept shell-safe (no quotes/`$`/backticks) so they can be
// piped straight to the platform text-to-speech command. An LLM was considered but a
// cross-platform embedded model isn't worth the weight — canned lines are funnier anyway.

namespace dc::game {

inline const char* taunt_text(int i) {
    static const char* T[] = {
        "fuck you",
        "you suck",
        "eat shit",
        "is that all you got",
        "you hit like a baby",
        "come here and die",
        "pathetic",
        "you smell terrible",
        "your mother was a hamster",
        "git gud",
        "weakling",
        "i will eat your bones",
        "you fight like a cow",
        "cry about it",
        "skill issue",
        "dumbass",
        "you call that a swing",
        "i have seen scarier rats",
        "go home",
        "you are already dead",
        "bonk",
        "stay down",
        "loser",
        "nice try idiot",
    };
    const int n = static_cast<int>(sizeof(T) / sizeof(T[0]));
    return T[((i % n) + n) % n];
}

inline int taunt_count() {
    // Must match the array above.
    return 24;
}

} // namespace dc::game
