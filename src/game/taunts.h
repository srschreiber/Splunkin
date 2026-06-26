#pragma once
#include <cstdint>
#include <string>

// Enemy taunts: procedurally GENERATED insults built from templates + big word banks, so the
// player rarely hears the same line twice (permutations run into the tens of thousands).
// Output is kept shell-safe (no quotes / `$` / backticks / backslashes) so each line can be
// piped straight to the platform text-to-speech command; commas + `!` / `?` are fine because
// the command runs via /bin/sh (non-interactive), not an interactive shell.

namespace dc::game {

namespace detail {

// A tiny LCG step over a seed reference -> next value. Caller threads one seed through a line.
inline uint32_t taunt_next(uint32_t& s) { s = s * 1664525u + 1013904223u; return s >> 8; }

// pick one entry from a string array, advancing the seed.
template <int N>
inline const char* taunt_pick(uint32_t& s, const char* const (&arr)[N]) {
    return arr[taunt_next(s) % N];
}

// --- Word banks (all lowercase, shell-safe) ------------------------------------------------

// Imperative verbs that take a grimy OBJECT ("eat shit", "choke on dust").
inline const char* const VERB[] = {
    "eat", "suck", "lick", "choke on", "kiss", "swallow", "gargle", "smell", "taste",
    "worship", "bury yourself in", "drown in", "wallow in", "go chew on", "beg for", "gnaw on",
};
// Grimy objects for the verbs above.
inline const char* const OBJECT[] = {
    "shit", "dirt", "mud", "dust", "ash", "garbage", "slop", "filth", "gravel", "my blade",
    "my boot", "your teeth", "sewage", "rot", "the floor", "horse dung", "swamp water",
};
// Epithets, addressed straight at the player.
inline const char* const INSULT[] = {
    "asshole", "worm", "maggot", "coward", "fool", "peasant", "weakling", "idiot", "dog",
    "rat", "swine", "wretch", "halfwit", "milkdrinker", "bootlicker", "mouthbreather", "clown",
    "buffoon", "imbecile", "cretin", "louse", "toad", "gremlin", "knave", "dimwit", "nincompoop",
    "dunce", "pissant", "scoundrel", "ninny", "lummox", "oaf", "frank stallone", "retard",
};
// Adjectives.
inline const char* const ADJ[] = {
    "ugly", "pathetic", "stinking", "sniveling", "gutless", "brainless", "worthless", "sorry",
    "rancid", "feeble", "putrid", "sloppy", "clumsy", "miserable", "wretched", "festering",
    "drooling", "spineless", "useless", "greasy", "moldy", "crusty", "soggy", "lumpy", "reeking",
};
// Lowly creatures ("your mother is a X", "you fight like a X").
inline const char* const CREATURE[] = {
    "hamster", "goblin", "troll", "toad", "slug", "goat", "donkey", "weasel", "pig",
    "cockroach", "sewer rat", "gutter rat", "three legged mule", "wet dog", "dead fish",
    "bag of onions", "sack of turnips", "diseased goat", "bloated toad", "limp noodle",
    "headless chicken", "drowned cat", "one eyed newt",
};
// Things your face resembles.
inline const char* const THING[] = {
    "a smashed turnip", "roadkill", "a wet sock", "a boiled boot", "spoiled milk",
    "a chewed up shoe", "the wrong end of a horse", "a melted candle", "old porridge",
    "a busted drum", "a moldy potato", "a slapped backside", "a dropped pie",
};
// Transitive threat verbs ("i will VERB you").
inline const char* const THREAT[] = {
    "bury", "gut", "crush", "end", "flatten", "skewer", "pummel", "squash", "thrash",
    "stomp", "wallop", "clobber", "pulverize", "mangle",
};

} // namespace detail

// Generate one randomized AMBIENT taunt, threading `seed`. Returns a fresh std::string.
inline std::string taunt_generate(uint32_t& seed) {
    using namespace detail;
    const int form = taunt_next(seed) % 19;
    const std::string V = taunt_pick(seed, VERB),  O = taunt_pick(seed, OBJECT);
    const std::string V2 = taunt_pick(seed, VERB), O2 = taunt_pick(seed, OBJECT);
    const std::string I = taunt_pick(seed, INSULT), A = taunt_pick(seed, ADJ);
    const std::string A2 = taunt_pick(seed, ADJ),  C = taunt_pick(seed, CREATURE);
    const std::string T = taunt_pick(seed, THING), H = taunt_pick(seed, THREAT);
    switch (form) {
        case 17: return V + " " + O + " and " + V2 + " " + O2 + "!";        // compound: "eat shit and choke on dirt!"
        case 18: return V + " " + O + ", " + I + ", and " + V2 + " " + O2 + "!";
        case 0:  return V + " " + O + ", " + I + "!";
        case 1:  return "your mother is a " + C + "!";
        case 2:  return "you " + A + " " + I + "!";
        case 3:  return "you fight like a " + C + "!";
        case 4:  return "i will " + H + " you, " + I + "!";
        case 5:  return V + " " + O + "!";
        case 6:  return A + " " + I + "!";
        case 7:  return "go " + V + " " + O + ", " + I + "!";
        case 8:  return "your face looks like " + T + "!";
        case 9:  return "nobody likes you, " + I + "!";
        case 10: return "die, " + I + "!";
        case 11: return "is that all you got, " + I + "?";
        case 12: return "you smell like " + T + "!";
        case 13: return "back to the gutter, " + I + "!";
        case 14: return "you " + A + ", " + A2 + " " + I + "!";
        case 15: return "your mother fights like a " + C + "!";
        default: return "you " + A + " sack of " + O + "!";
    }
}

// Generate one randomized REACTIVE gloat (said by the enemy that just hurt you). Punchier.
inline std::string reactive_generate(uint32_t& seed) {
    using namespace detail;
    const int form = taunt_next(seed) % 9;
    const std::string V = taunt_pick(seed, VERB),  O = taunt_pick(seed, OBJECT);
    const std::string I = taunt_pick(seed, INSULT), A = taunt_pick(seed, ADJ);
    switch (form) {
        case 0:  return "got you, " + I + "!";
        case 1:  return "stay down, " + I + "!";
        case 2:  return V + " " + O + "!";
        case 3:  return "feel that, " + I + "?";
        case 4:  return "too slow, " + I + "!";
        case 5:  return "you " + A + " " + I + "!";
        case 6:  return "bleed, " + I + "!";
        case 7:  return "sit down, " + I + "!";
        default: return "lights out, " + I + "!";
    }
}

// --- Back-compat shims (older call sites index a canned bank). These now just feed the index
// into the procedural generator so everything is dynamic. -----------------------------------
inline std::string taunt_line(int i)    { uint32_t s = 0x9E3779B9u ^ static_cast<uint32_t>(i) * 2654435761u; return taunt_generate(s); }
inline std::string reactive_line(int i) { uint32_t s = 0x85EBCA6Bu ^ static_cast<uint32_t>(i) * 2246822519u; return reactive_generate(s); }
inline const char* taunt_text(int)    { return ""; }   // deprecated (use taunt_generate)
inline const char* reactive_text(int) { return ""; }   // deprecated (use reactive_generate)
inline int taunt_count()    { return 1; }
inline int reactive_count() { return 1; }

} // namespace dc::game
