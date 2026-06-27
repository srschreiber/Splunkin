#include "engine/platform/window.h"
#include "engine/input/input.h"
#include "engine/renderer/renderer.h"
#include "engine/renderer/mesh.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/model.h"
#include "engine/renderer/animator.h"
#include "engine/entity/player.h"
#include "engine/entity/entity.h"
#include "engine/entity/enemy.h"
#include "engine/entity/spawner.h"
#include "engine/world/map.h"
#include "engine/world/map_mesh.h"
#include "engine/world/torch.h"
#include "engine/world/pathfind.h"
#include "engine/fx/particles.h"
#include "engine/net/net.h"
#include "engine/net/protocol.h"
#include "game/upgrades.h"
#include "game/seven_seg.h"
#include "game/appearance.h"
#include "game/taunts.h"
#include "game/base.h"
#include <thread>
#include <atomic>
#include <string>
#include <cstdlib>

#include <SDL3/SDL.h>
#include <cglm/cglm.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <utility>

// Number of TTS voices currently speaking (caps how many taunts talk over each other).
static std::atomic<int> g_tts_active{0};

// Speak `text` aloud via text-to-speech, off-thread so it never blocks the frame. Capped +
// best-effort: if no engine is available it just stays silent.
//
// PIPER neural TTS (https://github.com/OHF-Voice/piper1-gpl) is used when configured — set
// DUNGEON_PIPER_MODEL to a downloaded voice .onnx (and optionally DUNGEON_PIPER_BIN if `piper`
// isn't on PATH). It synthesizes a wav and plays it (afplay/aplay/ffplay). Otherwise we fall
// back to the OS voice (say / espeak / SAPI).
static std::atomic<unsigned> g_tts_seq{0};
static void speak_async(const std::string& text) {
    if (g_tts_active.load() >= 2) return;            // at most two voices at once
    g_tts_active.fetch_add(1);
    const unsigned seq = g_tts_seq.fetch_add(1);
    std::thread([text, seq]() {
        // Strip shell-significant characters so the text can't break out of the command.
        std::string safe; safe.reserve(text.size());
        for (char c : text) if (c != '"' && c != '\\' && c != '`' && c != '$' && c != '\n' && c != '\r') safe += c;

        std::string cmd;
        // Voice model: $DUNGEON_PIPER_MODEL, else a model dropped at assets/piper/voice.onnx.
        std::string model;
        if (const char* pm = std::getenv("DUNGEON_PIPER_MODEL"); pm && *pm) model = pm;
        else { std::ifstream f("assets/piper/voice.onnx"); if (f.good()) model = "assets/piper/voice.onnx"; }
        if (!model.empty()) {
            // Resolve the piper binary: $DUNGEON_PIPER_BIN, else ~/.local/bin/piper (pipx/pip --user
            // install location), else bare "piper" off PATH.
            std::string bin = "piper";
            if (const char* pb = std::getenv("DUNGEON_PIPER_BIN"); pb && *pb) bin = pb;
            else if (const char* home = std::getenv("HOME"); home) {
                std::string local = std::string(home) + "/.local/bin/piper";
                std::ifstream pf(local); if (pf.good()) bin = local;
            }
#if defined(__APPLE__)
            // macOS has no simple raw-PCM player, so synth to a unique wav then afplay it.
            const std::string wav = "/tmp/dc_tts_" + std::to_string(seq) + ".wav";
            cmd = "printf '%s' \"" + safe + "\" | " + bin + " --model \"" + model + "\" --output_file " + wav
                + " >/dev/null 2>&1 && afplay " + wav + " >/dev/null 2>&1; rm -f " + wav;
#elif defined(__linux__)
            // Stream raw PCM straight into a player (piper voices are 16-bit mono @ 22050).
            cmd = "printf '%s' \"" + safe + "\" | " + bin + " --model \"" + model + "\" --output-raw 2>/dev/null | "
                  "(aplay -q -r 22050 -f S16_LE -t raw - 2>/dev/null || ffplay -nodisp -autoexit -f s16le -ar 22050 -i - >/dev/null 2>&1)";
#elif defined(_WIN32)
            const std::string wav = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "\\dc_tts_" + std::to_string(seq) + ".wav";
            cmd = "echo " + safe + " | \"" + bin + "\" --model \"" + model + "\" --output_file \"" + wav + "\" >NUL 2>&1 && "
                  "powershell -NoProfile -c \"(New-Object Media.SoundPlayer '" + wav + "').PlaySync()\" & del \"" + wav + "\"";
#endif
        }
        if (cmd.empty()) {   // no Piper model configured -> OS voice
#if defined(__APPLE__)
            cmd = "say -v Alex -r 210 \"" + safe + "\"";   // Alex = classic male voice, a touch faster
#elif defined(__linux__)
            cmd = "espeak -v en+m3 -s 175 \"" + safe + "\" >/dev/null 2>&1 || spd-say \"" + safe + "\" >/dev/null 2>&1";
#elif defined(_WIN32)
            cmd = "powershell -NoProfile -Command \"Add-Type -AssemblyName System.Speech;"
                  "$s=New-Object System.Speech.Synthesis.SpeechSynthesizer;"
                  "try{$s.SelectVoiceByHints('Male')}catch{};$s.Speak('" + safe + "')\"";
#endif
        }
        if (!cmd.empty()) std::system(cmd.c_str());
        g_tts_active.fetch_sub(1);
    }).detach();
}

static std::string read_file(const char* path) {
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

// Upgrades + their catalog (Upgrade, IconShape, UpgradeDef, upgrade_def, apply_upgrade,
// elem_mask) live in game/upgrades.h, included above.

inline constexpr uint32_t NO_LOCK = 0xFFFFFFFFu;   // chest.locked_by sentinel: nobody's in the menu

// A placed chest holding 4 items. A player opens its menu (one player at a time) and
// buys ONE remaining item for `cost`; others buy the rest until it's depleted, then the
// price disappears and the lid stays open. `contents` is seeded deterministically so
// every peer agrees; only `taken`/`opened` (and the host's lock) change at runtime.
struct Chest {
    int   col = 0, row = 0;
    float open_t = 0.0f;    // time into the open clip (0 = closed)
    bool  opened = false;   // lid open (set on first open; stays open once depleted)
    int   cost   = 10;      // coins per item
    Upgrade contents[4] = {};   // the 4 items (deterministic from the seed)
    bool    taken[4] = {};      // which slots have been purchased
    uint32_t locked_by = NO_LOCK;  // player id with the menu open (host-authoritative)
    float    lock_time = 0.0f;     // host: auto-release countdown (safety)
    int remaining() const { int n = 0; for (bool t : taken) if (!t) ++n; return n; }
};

// A dropped coin: sits on the floor (settling), then magnets to the player and
// is collected. `age` gates the settle delay so it's always visible briefly.
struct Coin {
    vec3  pos;
    float value = 1.0f;
    float age   = 0.0f;
};

// A dropped XP orb: a blue glowing mote left where an enemy died. Like a coin it
// settles briefly, then magnets to the nearest player and is absorbed for `value` XP.
struct XPOrb {
    vec3  pos;
    float value = 0.0f;   // XP granted on pickup (scaled by enemy difficulty)
    float age   = 0.0f;
    float bob   = 0.0f;   // phase offset so a cluster shimmers out of sync
};

// An updraft pad: stepping onto it launches you upward (traversal). Position only;
// placed deterministically so every peer agrees (no replication).
struct Updraft { float x = 0.0f, z = 0.0f; };

// A drone vending spot: a gunner drone lying on the ground with a price floating above.
// Pay the cost (E) to gain a gunner minion; one-time (consumed on purchase).
struct DroneVendor { float x = 0.0f, z = 0.0f; bool bought = false; };



int main(int argc, char** argv) {
    bool smoke = false;
    const char* map_path = "assets/maps/lane.txt";
    // Networking: --host [port] listens; --connect <ip> [port] joins. Default = solo.
    dc::net::Role net_role = dc::net::Role::Standalone;
    const char* connect_ip = "127.0.0.1";
    uint16_t net_port = 1234;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) smoke = true;
        else if (std::strcmp(argv[i], "--host") == 0) {
            net_role = dc::net::Role::Host;
            if (i + 1 < argc && argv[i + 1][0] != '-') net_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--connect") == 0) {
            net_role = dc::net::Role::Client;
            if (i + 1 < argc && argv[i + 1][0] != '-') connect_ip = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') net_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (argv[i][0] != '-') map_path = argv[i];
    }

    std::string text = read_file(map_path);
    if (text.empty()) { std::fprintf(stderr, "could not read map: %s\n", map_path); return 1; }
    auto map = dc::world::parse_map(text);
    if (!map) { std::fprintf(stderr, "could not parse map: %s\n", map_path); return 1; }

    dc::renderer::ModelData model_data;
    if (!dc::renderer::read_model("assets/models/player.glb", model_data)) {
        std::fprintf(stderr, "could not load model: assets/models/player.glb\n");
        return 1;
    }

    dc::renderer::ModelData chest_data;
    if (!dc::renderer::read_model("assets/models/chest.glb", chest_data)) {
        std::fprintf(stderr, "could not load model: assets/models/chest.glb\n");
        return 1;
    }

    dc::renderer::ModelData helmet_data;
    if (!dc::renderer::read_model("assets/models/helmet.glb", helmet_data)) {
        std::fprintf(stderr, "could not load model: assets/models/helmet.glb\n");
        return 1;
    }

    dc::renderer::ModelData shield_data;
    if (!dc::renderer::read_model("assets/models/shield.glb", shield_data)) {
        std::fprintf(stderr, "could not load model: assets/models/shield.glb\n");
        return 1;
    }

    dc::renderer::ModelData torch_data;
    if (!dc::renderer::read_model("assets/models/torch.glb", torch_data)) {
        std::fprintf(stderr, "could not load model: assets/models/torch.glb\n");
        return 1;
    }

    dc::renderer::ModelData sword_data;
    if (!dc::renderer::read_model("assets/models/sword.glb", sword_data)) {
        std::fprintf(stderr, "could not load model: assets/models/sword.glb\n");
        return 1;
    }
    // Flying eye model (optional): the new tentacled eye.glb is preferred; fall back to the
    // old eye_enemy.glb, then to the red cube if neither is present.
    dc::renderer::ModelData eye_data;
    bool eye_loaded = dc::renderer::read_model("assets/models/eye.glb", eye_data);
    if (!eye_loaded) eye_loaded = dc::renderer::read_model("assets/models/eye_enemy.glb", eye_data);
    if (!eye_loaded) std::fprintf(stderr, "note: no eye model found; flyers use the cube\n");
    // Skeleton enemy model (optional; built by blender/make_skeleton.py). If absent, no
    // skeletons spawn (so they never fall back to looking like a plain melee enemy).
    dc::renderer::ModelData bat_data;
    const bool bat_loaded = dc::renderer::read_model("assets/models/bat.glb", bat_data);
    if (!bat_loaded) std::fprintf(stderr, "note: assets/models/bat.glb not found; run blender/make_bat.py to enable bats\n");
    dc::renderer::ModelData gnome_data;   // re-skins the flamethrower enemy
    const bool gnome_loaded = dc::renderer::read_model("assets/models/gnome.glb", gnome_data);
    dc::renderer::ModelData mage_data;    // re-skins the ranged enemy
    const bool mage_loaded = dc::renderer::read_model("assets/models/mage.glb", mage_data);
    dc::renderer::ModelData turret_data;  // the defensive turret base/housing (barrel stays procedural)
    const bool turret_loaded = dc::renderer::read_model("assets/models/turret.glb", turret_data);
    dc::renderer::ModelData drone_data;   // gunner-minion quadcopter
    const bool drone_loaded = dc::renderer::read_model("assets/models/drone.glb", drone_data);
    dc::renderer::ModelData glyph_data;   // base core glyph stone
    const bool glyph_loaded = dc::renderer::read_model("assets/models/glyphstone.glb", glyph_data);
    dc::renderer::ModelData troll_data;   // big melee bruiser
    const bool troll_loaded = dc::renderer::read_model("assets/models/troll.glb", troll_data);
    dc::renderer::ModelData demon_data;   // big fireball-lobbing demon
    const bool demon_loaded = dc::renderer::read_model("assets/models/demon.glb", demon_data);
    // Full per-class player models with gear baked in (helm/hat + weapon). These replace
    // the generic player.glb for rendering a player of that class.
    dc::renderer::ModelData knight_class_data, wizard_class_data;
    const bool knight_class_loaded = dc::renderer::read_model("assets/models/knight_class.glb", knight_class_data);
    const bool wizard_class_loaded = dc::renderer::read_model("assets/models/wizard_class.glb", wizard_class_data);
    dc::renderer::ModelData scavenger_data;   // friendly coin-collecting mob (dapper gentleman)
    const bool scavenger_loaded = dc::renderer::read_model("assets/models/scavenger.glb", scavenger_data);
    if (!scavenger_loaded) std::fprintf(stderr, "note: assets/models/scavenger.glb not found; run blender/make_scavenger.py\n");

    dc::renderer::ModelData insulter_data;    // bald, red-goatee heckler with an attack-weakening aura
    const bool insulter_loaded = dc::renderer::read_model("assets/models/insulter.glb", insulter_data);
    if (!insulter_loaded) std::fprintf(stderr, "note: assets/models/insulter.glb not found; run blender/make_insulter.py\n");

    dc::renderer::ModelData mounted_knight_data;   // Tree-Sentinel cavalier: barded warhorse + lance/shield rider (origin at hind hooves)
    const bool mounted_knight_loaded = dc::renderer::read_model("assets/models/mounted_knight.glb", mounted_knight_data);
    if (!mounted_knight_loaded) std::fprintf(stderr, "note: assets/models/mounted_knight.glb not found; run blender/make_mounted_knight.py\n");

    dc::renderer::ModelData boat_data;   // wooden rowboat hull + rowing oars (bow +Y, origin at the waterline)
    const bool boat_loaded = dc::renderer::read_model("assets/models/boat.glb", boat_data);
    if (!boat_loaded) std::fprintf(stderr, "note: assets/models/boat.glb not found; run blender/make_boat.py\n");

    dc::renderer::ModelData barracks_data;   // timber barracks hut (1.9-tile footprint, door +Y, static)
    const bool barracks_loaded = dc::renderer::read_model("assets/models/barracks.glb", barracks_data);
    if (!barracks_loaded) std::fprintf(stderr, "note: assets/models/barracks.glb not found; run blender/make_barracks.py\n");

    dc::renderer::ModelData mortar_data;   // heavy siege mortar (barrel +Y, muzzle tip ~(0,1.95,-0.68), static)
    const bool mortar_loaded = dc::renderer::read_model("assets/models/mortar.glb", mortar_data);
    if (!mortar_loaded) std::fprintf(stderr, "note: assets/models/mortar.glb not found; run blender/make_mortar.py\n");

    dc::renderer::ModelData skeleton_data;
    const bool skeleton_loaded = dc::renderer::read_model("assets/models/skeleton.glb", skeleton_data);
    if (!skeleton_loaded) std::fprintf(stderr, "note: assets/models/skeleton.glb not found; run blender/make_skeleton.py to enable skeletons\n");
    else std::fprintf(stderr, "[skeleton] loaded: %zu nodes, %zu mesh parts; bones body=%d head=%d armL=%d; walk=%d punch=%d\n",
                      skeleton_data.nodes.size(), skeleton_data.parts.size(), skeleton_data.body_node, skeleton_data.head_node,
                      skeleton_data.arm_l_node, skeleton_data.walk.valid() ? 1 : 0, skeleton_data.punch.valid() ? 1 : 0);

    dc::platform::Window window;
    if (!window.init("dungeoncrawl")) return 1;

    dc::renderer::Renderer renderer;
    if (!renderer.init()) { window.shutdown(); return 1; }

    // Procedural terrain: gentle hills under the open floor (seed -> identical on
    // host + clients). Walls/gameplay come from the tile map; this only shapes height.
    dc::world::Terrain terrain;
    // Second map-gen pass: scatter sheer-cliff plateaus (deterministic) before meshing.
    // The lane map is kept FLAT for now (0 plateaus) — leave the call so later maps can use it.
    terrain.place_plateaus(0, map->width * dc::world::TILE, map->height * dc::world::TILE,
                           (map->spawn_col + 0.5f) * dc::world::TILE, (map->spawn_row + 0.5f) * dc::world::TILE);
    // Flatten the rolling hills/mounds too so the lane reads clean (terrain code kept intact).
    terrain.hill_amp = 0.0f; terrain.mound_amp = 0.0f; terrain.base_amp = 0.0f;
    // EROSION: carve the river channel into the ground so banks slope down into the water.
    // Span/centerline must match river_depth (riverZ = height*0.30, x0=16*TILE+8, x1=(width-16)*TILE-8).
    terrain.set_river(map->height * 0.30f * dc::world::TILE, 4.5f, 1.6f,
                      16.0f * dc::world::TILE + 8.0f, (map->width - 16.0f) * dc::world::TILE - 8.0f,
                      3.2f, 10.0f, /*carve*/1.2f, /*bank*/4.0f);   // subtle erosion (deep carve broke the build raycast)
    const vec3 terrain_color = { 0.32f, 0.40f, 0.26f };   // mossy green-brown

    // Per-tile ground height (sampled at tile centers), computed once — terrain is static.
    // Feeds the directional flow field so enemies path to ramps instead of up cliffs.
    std::vector<float> tile_heights(static_cast<std::size_t>(map->width) * map->height);
    for (int r = 0; r < map->height; ++r)
        for (int c = 0; c < map->width; ++c)
            tile_heights[static_cast<std::size_t>(r) * map->width + c] =
                terrain.height((c + 0.5f) * dc::world::TILE, (r + 0.5f) * dc::world::TILE);

    dc::renderer::Mesh mesh;
    mesh.upload(dc::world::build_map_mesh(*map, terrain));   // walls (textured)
    dc::renderer::Mesh terrain_mesh;
    terrain_mesh.upload(dc::world::build_terrain_mesh(*map, terrain));   // floor (solid color)
    dc::renderer::Mesh flyer_mesh;   // rebuilt each frame from hovering flying-enemy cubes

    dc::renderer::Model player_model;
    player_model.upload(model_data);

    dc::renderer::Model chest_model;
    chest_model.upload(chest_data);

    dc::renderer::Model helmet_model;
    helmet_model.upload(helmet_data);

    dc::renderer::Model shield_model;
    shield_model.upload(shield_data);

    dc::renderer::Model sword_model;
    sword_model.upload(sword_data);

    dc::renderer::Model eye_model;
    if (eye_loaded) eye_model.upload(eye_data);
    dc::renderer::Model skeleton_model;
    if (skeleton_loaded) skeleton_model.upload(skeleton_data);   // upload after the GL context exists
    dc::renderer::Model scavenger_model;
    if (scavenger_loaded) scavenger_model.upload(scavenger_data);
    dc::renderer::Model insulter_model;
    if (insulter_loaded) insulter_model.upload(insulter_data);
    dc::renderer::Model mounted_knight_model;
    if (mounted_knight_loaded) mounted_knight_model.upload(mounted_knight_data);
    dc::renderer::Model boat_model;
    if (boat_loaded) boat_model.upload(boat_data);
    dc::renderer::Model barracks_model;
    if (barracks_loaded) barracks_model.upload(barracks_data);
    dc::renderer::Model mortar_model;
    if (mortar_loaded) mortar_model.upload(mortar_data);
    dc::renderer::Model bat_model;
    if (bat_loaded) bat_model.upload(bat_data);
    dc::renderer::Model gnome_model;
    if (gnome_loaded) gnome_model.upload(gnome_data);
    dc::renderer::Model mage_model;
    if (mage_loaded) mage_model.upload(mage_data);
    dc::renderer::Model turret_model;
    if (turret_loaded) turret_model.upload(turret_data);
    dc::renderer::Model drone_model;  if (drone_loaded) drone_model.upload(drone_data);
    dc::renderer::Model glyph_model;  if (glyph_loaded) glyph_model.upload(glyph_data);
    dc::renderer::Model troll_model;  if (troll_loaded) troll_model.upload(troll_data);
    dc::renderer::Model demon_model;  if (demon_loaded) demon_model.upload(demon_data);
    dc::renderer::Model knight_class_model; if (knight_class_loaded) knight_class_model.upload(knight_class_data);
    dc::renderer::Model wizard_class_model; if (wizard_class_loaded) wizard_class_model.upload(wizard_class_data);
    // Pick the player model/data for a given class (fall back to the generic player rig).
    auto class_md = [&](int wclass) -> dc::renderer::ModelData& {
        if (wclass == 1 && wizard_class_loaded) return wizard_class_data;
        if (wclass == 0 && knight_class_loaded) return knight_class_data;
        return model_data;
    };
    auto class_mdl = [&](int wclass) -> dc::renderer::Model& {
        if (wclass == 1 && wizard_class_loaded) return wizard_class_model;
        if (wclass == 0 && knight_class_loaded) return knight_class_model;
        return player_model;
    };
    auto class_custom = [&](int wclass) { return (wclass == 1 && wizard_class_loaded) || (wclass == 0 && knight_class_loaded); };

    dc::renderer::Model torch_model;
    torch_model.upload(torch_data);

    // Equipment is attached to a player bone (helmet -> head, shield -> hand).
    // Each item's mesh-node-local TRS is its constant offset relative to that
    // bone; each frame we draw it at: player_placement * boneWorld * offset.
    auto helmet_offset = dc::renderer::mesh_offsets(helmet_data);
    auto shield_offset = dc::renderer::mesh_offsets(shield_data);
    auto sword_offset  = dc::renderer::mesh_offsets(sword_data);

    // Spawn a chest entity per 'C' tile in the map.
    std::vector<Chest> chests;
    for (const auto& cs : map->chests) chests.push_back({ cs.col, cs.row });
    // Predetermine each chest's 4 items at startup, seeded from the map so EVERY peer
    // computes the same contents (no need to replicate them — only taken/opened change).
    {
        uint32_t seed = 0x51ED7u + static_cast<uint32_t>(map->width * 73856093 + map->height * 19349663);
        auto nextu = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
        for (auto& ch : chests) {
            // Pick 4 DISTINCT core upgrades (Fisher-Yates prefix on the pool) so a chest
            // never shows the same card twice — forces variety in what's on offer.
            int pool[CHEST_UPGRADE_COUNT];
            for (int i = 0; i < CHEST_UPGRADE_COUNT; ++i) pool[i] = i;
            for (int k = 0; k < 4; ++k) {
                const int j = k + static_cast<int>(nextu() % static_cast<uint32_t>(CHEST_UPGRADE_COUNT - k));
                const int tmp = pool[k]; pool[k] = pool[j]; pool[j] = tmp;
                ch.contents[k] = static_cast<Upgrade>(pool[k]);
            }
        }
    }
    int menu_chest = -1;   // index of the chest whose purchase menu is open locally (-1 = none)
    bool levelup_open = false;          // level-up upgrade-pick overlay is showing
    Upgrade levelup_cards[4] = {};      // the (up to 4) eligible cards drawn for this pick
    int  levelup_card_count = 0;        // how many cards are actually offered
    vec3 player_prev = {0,0,0};   // local player's last-frame position (for shot-leading velocity)

    // Updraft launch pads (slipstreams): mostly placed ON TOP of and AROUND the plateaus
    // (deterministic from the map seed, so all peers agree). On a plateau they launch you
    // higher; at the base, combined with the wind shove they help fling you up onto it.
    std::vector<Updraft> updrafts;
    {
        uint32_t s = 0x0DDBA11u + static_cast<uint32_t>(map->width * 2654435761u);
        auto nf = [&](float lo, float hi) {
            s = s * 1664525u + 1013904223u;
            return lo + (hi - lo) * ((s >> 8) * (1.0f / 16777216.0f));
        };
        const float worldW = map->width * dc::world::TILE, worldH = map->height * dc::world::TILE;
        auto clampw = [](float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); };
        for (const auto& p : terrain.plateaus) {
            // One on top (kept off the rim so you don't immediately drift off the edge).
            updrafts.push_back({ nf(p.x0 + 3.0f, p.x1 - 3.0f), nf(p.z0 + 3.0f, p.z1 - 3.0f) });
            // One just outside a random edge, at the cliff base.
            const float pad = 4.0f;
            switch (static_cast<int>(nf(0.0f, 4.0f))) {
                case 0:  updrafts.push_back({ clampw(p.x1 + pad, 2.0f, worldW - 2.0f), nf(p.z0, p.z1) }); break;
                case 1:  updrafts.push_back({ clampw(p.x0 - pad, 2.0f, worldW - 2.0f), nf(p.z0, p.z1) }); break;
                case 2:  updrafts.push_back({ nf(p.x0, p.x1), clampw(p.z1 + pad, 2.0f, worldH - 2.0f) }); break;
                default: updrafts.push_back({ nf(p.x0, p.x1), clampw(p.z0 - pad, 2.0f, worldH - 2.0f) }); break;
            }
        }
        // A couple of extra pads out in the open for variety.
        for (int i = 0; i < 2; ++i)
            updrafts.push_back({ nf(8.0f, worldW - 8.0f), nf(8.0f, worldH - 8.0f) });
    }

    // Drone vendors: a few ground drones you can buy (deterministic placement, like pads).
    std::vector<DroneVendor> drone_vendors;
    {
        std::vector<int> open;
        for (int r = 0; r < map->height; ++r)
            for (int c = 0; c < map->width; ++c)
                if (map->at(c, r) == dc::world::Cell::Open && !(c == map->spawn_col && r == map->spawn_row))
                    open.push_back(r * map->width + c);
        uint32_t s = 0xD9047E5u + static_cast<uint32_t>(map->height * 40503u);
        const int want = 8;
        for (int i = 0; i < want && !open.empty(); ++i) {
            s = s * 1664525u + 1013904223u;
            const std::size_t pick = (s >> 8) % open.size();
            const int cell = open[pick]; open[pick] = open.back(); open.pop_back();
            drone_vendors.push_back({ (cell % map->width + 0.5f) * dc::world::TILE, (cell / map->width + 0.5f) * dc::world::TILE, false });
        }
    }
    const int DRONE_COST = 10;

    // Spawn a torch per wall-torch tile: precompute its placement + flame point,
    // and give each its own particle emitter.
    struct Torch {
        mat4 placement;
        vec3 flame_pos;
        dc::fx::ParticleSystem ps;
    };
    // The torch is a static model — pose it once at rest for drawing.
    std::vector<dc::renderer::Mat4> torch_part_world;
    dc::renderer::pose_model(torch_data, {}, 0.0f, torch_part_world);

    // Flame anchor: the centroid of the torch's emissive parts, in model-local
    // space (so light + particles sit at the actual flame, whatever its scale).
    vec3 flame_anchor = { 0.0f, 0.0f, 0.0f };
    int  flame_parts = 0;
    for (std::size_t i = 0; i < torch_data.parts.size(); ++i) {
        const auto& p = torch_data.parts[i];
        if (p.emissive[0] + p.emissive[1] + p.emissive[2] <= 0.0f) continue;  // flame parts only
        vec3 c = { 0.0f, 0.0f, 0.0f };
        const std::size_t nv = p.vertices.size() / 8;
        for (std::size_t v = 0; v < nv; ++v) {
            c[0] += p.vertices[v * 8 + 0]; c[1] += p.vertices[v * 8 + 1]; c[2] += p.vertices[v * 8 + 2];
        }
        if (nv) { c[0] /= nv; c[1] /= nv; c[2] /= nv; }
        vec3 cw; glm_mat4_mulv3(torch_part_world[i].m, c, 1.0f, cw);   // part-local -> model-local
        flame_anchor[0] += cw[0]; flame_anchor[1] += cw[1]; flame_anchor[2] += cw[2];
        ++flame_parts;
    }
    if (flame_parts) { flame_anchor[0] /= flame_parts; flame_anchor[1] /= flame_parts; flame_anchor[2] /= flame_parts; }
    // Lowest point of the torch model (model-local Y) so free-standing torches can sit their
    // FOOT on the ground instead of floating at the (wall-mount) origin.
    float torch_min_y = 1e9f;
    for (std::size_t i = 0; i < torch_data.parts.size(); ++i) {
        const auto& p = torch_data.parts[i];
        const std::size_t nv = p.vertices.size() / 8;
        for (std::size_t v = 0; v < nv; ++v) {
            vec3 lp = { p.vertices[v*8+0], p.vertices[v*8+1], p.vertices[v*8+2] }, wp;
            glm_mat4_mulv3(torch_part_world[i].m, lp, 1.0f, wp);
            if (wp[1] < torch_min_y) torch_min_y = wp[1];
        }
    }
    if (torch_min_y > 1e8f) torch_min_y = 0.0f;

    std::vector<Torch> torches;
    for (const auto& ts : map->torches) {
        Torch t;
        dc::world::torch_placement(ts.col, ts.row, ts.dir, t.placement);
        glm_mat4_mulv3(t.placement, flame_anchor, 1.0f, t.flame_pos);   // model-local -> world
        torches.push_back(std::move(t));
    }

    // Stone pillars: free-standing columns scattered on open floor, each with a torch on
    // top — so the (otherwise dim) arena gets pools of light away from the walls. The
    // pillar mesh is static (built once); each pillar adds a torch entry (light + flame).
    dc::renderer::Mesh pillar_mesh;
    const vec3 pillar_color = { 0.42f, 0.40f, 0.38f };   // cool stone grey
    {
        std::vector<float> pv;
        auto face = [&](float ax,float ay,float az, float bx,float by,float bz,
                        float cx,float cy,float cz, float dx,float dy,float dz, float nx,float ny,float nz) {
            auto V=[&](float x,float y,float z){ pv.insert(pv.end(), {x,y,z,nx,ny,nz,0.f,0.f,0.f}); };
            V(ax,ay,az);V(bx,by,bz);V(cx,cy,cz); V(ax,ay,az);V(cx,cy,cz);V(dx,dy,dz);
        };
        (void)face;   // pillar columns removed — torches now stand free on the ground
        uint32_t s = 0x5707E11u + static_cast<uint32_t>(map->width * 2246822519u);
        auto nf = [&](float lo, float hi) { s = s * 1664525u + 1013904223u; return lo + (hi - lo) * ((s >> 8) * (1.0f / 16777216.0f)); };
        const float worldW = map->width * dc::world::TILE, worldH = map->height * dc::world::TILE;
        const float PILLAR_HW = 0.7f, PILLAR_H = 3.4f;
        const float spawnX = (map->spawn_col + 0.5f) * dc::world::TILE;
        const float spawnZ = (map->spawn_row + 0.5f) * dc::world::TILE;
        for (int i = 0; i < 16; ++i) {
            const float px = nf(6.0f, worldW - 6.0f), pz = nf(6.0f, worldH - 6.0f);
            // Keep pillars clear of the spawn point.
            if (std::fabs(px - spawnX) < 8.0f && std::fabs(pz - spawnZ) < 8.0f) continue;
            const float base = terrain.height(px, pz);
            // A tall free-standing torch (no stone pillar): stand the torch model on the
            // ground and scale it up so it reads as a long torch, light pooling from its top.
            (void)PILLAR_H; (void)PILLAR_HW;   // (pillar columns removed)
            Torch t;
            glm_mat4_identity(t.placement);
            const float tsy = 2.6f;
            vec3 tp = { px, base - torch_min_y * tsy, pz }; glm_translate(t.placement, tp);  // foot on the ground
            vec3 ts = { 1.3f, tsy, 1.3f }; glm_scale(t.placement, ts);
            glm_mat4_mulv3(t.placement, flame_anchor, 1.0f, t.flame_pos);
            torches.push_back(std::move(t));
        }
        pillar_mesh.upload(pv);
    }

    // Grass tufts: little green blades scattered on open floor for ground decoration.
    // Static, deterministic (seeded from the map), built once and drawn flat-green.
    dc::renderer::Mesh grass_mesh;
    const vec3 grass_color = { 0.30f, 0.55f, 0.22f };
    {
        std::vector<float> gv;
        uint32_t gs = 0x9E3779B9u ^ static_cast<uint32_t>(map->width * 2654435761u + map->height);
        auto gf = [&]() { gs = gs * 1664525u + 1013904223u; return (gs >> 8) * (1.0f / 16777216.0f); };
        // One upright blade, built CROSSED (two perpendicular quads) so it has volume from
        // every angle — reads like a 3D tuft of hair/grass, not a flat card that vanishes edge-on.
        auto blade = [&](float x, float z, float y, float w, float h, float lx, float lz) {
            auto V = [&](float vx, float vy, float vz) { gv.insert(gv.end(), { vx, vy, vz, 0.0f, 1.0f, 0.0f, 0.f, 0.f, 0.f }); };
            // Quad A: spans X. Quad B: spans Z (perpendicular). Both taper to a leaning tip.
            const float tx = x + lx, tz = z + lz, ty = y + h;
            V(x - w, y, z); V(x + w, y, z); V(tx, ty, tz);          // X-plane blade
            V(x + w, y, z); V(x - w, y, z); V(tx, ty, tz);          // back face (double-sided)
            V(x, y, z - w); V(x, y, z + w); V(tx, ty, tz);          // Z-plane blade
            V(x, y, z + w); V(x, y, z - w); V(tx, ty, tz);          // back face
        };
        // No grass grows in the river. Recompute the same channel shape locally (the core
        // positions are deterministic from the map: bases sit 16 tiles in at the lane center).
        const float r_laneZ = map->height * 0.30f * dc::world::TILE;   // matches river_depth's biased center
        const float r_x0 = 16.0f * dc::world::TILE + 8.0f, r_x1 = (map->width - 16.0f) * dc::world::TILE - 8.0f;
        auto grass_in_river = [&](float wx, float wz) {
            if (wx < r_x0 || wx > r_x1) return false;
            const float t = (wx - r_x0) / (r_x1 - r_x0);
            const float center = r_laneZ + std::sin(t * 6.2831853f * 1.6f) * 4.5f;
            const float bump = std::exp(-((t - 0.5f) * (t - 0.5f)) / (2.0f * 0.018f));
            return std::fabs(wz - center) < (3.2f + bump * 10.0f);
        };
        // No grass inside either base's footprint (so blades don't poke through the blue base
        // perimeter marker). Clear the LARGEST the base can grow to; both cores sit at the lane
        // center, 16 tiles in from each end.
        const float baseZ = map->height * 0.5f * dc::world::TILE;
        const float p_core_x = 16.0f * dc::world::TILE, e_core_x = (map->width - 16.0f) * dc::world::TILE;
        const float base_clear2 = dc::game::BASE_AREA_MAX * dc::game::BASE_AREA_MAX;
        const int TUFTS = 1400;
        for (int i = 0; i < TUFTS; ++i) {
            const float x = gf() * (map->width * dc::world::TILE);
            const float z = gf() * (map->height * dc::world::TILE);
            const int c = static_cast<int>(x / dc::world::TILE), r = static_cast<int>(z / dc::world::TILE);
            if (map->at(c, r) != dc::world::Cell::Open) continue;
            if (grass_in_river(x, z)) continue;   // no grass in the water
            { const float dx = x - p_core_x, dz = z - baseZ; if (dx*dx + dz*dz < base_clear2) continue; }   // player base
            { const float dx = x - e_core_x, dz = z - baseZ; if (dx*dx + dz*dz < base_clear2) continue; }   // enemy base
            const float y = terrain.height(x, z);
            const int n = 3 + static_cast<int>(gf() * 3);   // a few blades per tuft
            for (int b = 0; b < n; ++b) {
                const float bx = x + (gf() - 0.5f) * 0.35f, bz = z + (gf() - 0.5f) * 0.35f;
                const float h = 0.22f + gf() * 0.22f, lean = (gf() - 0.5f) * 0.18f, lean2 = (gf() - 0.5f) * 0.18f;
                blade(bx, bz, y, 0.03f, h, lean, lean2);
            }
        }
        grass_mesh.upload(gv);
    }

    std::vector<float> particle_verts;   // rebuilt each frame for draw_particles

    // Dynamic entities (enemies for now). Spawn one per 'X' tile in the map.
    // Clients don't own enemies (host-authoritative; they're not replicated yet).
    dc::entity::EntityList entities;
    if (net_role != dc::net::Role::Client)
        for (const auto& es : map->enemies)
            entities.spawn_enemy((es.col + 0.5f) * dc::world::TILE, (es.row + 0.5f) * dc::world::TILE);
    std::vector<dc::renderer::Mat4> enemy_part_world;   // scratch, reused per enemy

    // One demo spawner at the map center, trickling enemies onto open floor.
    std::vector<dc::entity::Spawner> spawners;
    {
        dc::entity::Spawner sp;
        // Enemies pour out of the ENEMY BASE at the far (right) end of the lane and march on us.
        sp.pos[0] = (map->width - 16.0f) * dc::world::TILE;
        sp.pos[2] = map->height * 0.5f * dc::world::TILE;
        sp.radius = 7.0f * dc::world::TILE;                 // cluster around the enemy base mouth
        sp.rate = 0.4f; sp.max_alive = 6;                   // fewer, but stronger (see ENEMY_MAX_HEALTH)
        sp.ranged_fraction = 0.30f;   // ~30% ground shooters
        sp.flying_fraction = 0.15f;   // ~15% hovering shooters
        sp.flame_fraction  = 0.07f;   // ~7% rare flamethrower bruisers
        sp.skeleton_fraction = skeleton_loaded ? 0.5f : 0.0f;   // ~half of spawns are skeletons (if the model exists)
        sp.bat_fraction = bat_loaded ? 0.18f : 0.0f;            // some flyers are bats (if the model exists)
        sp.troll_fraction = troll_loaded ? 0.05f : 0.0f;        // rare big troll
        sp.demon_fraction = demon_loaded ? 0.05f : 0.0f;        // rare big demon
        sp.elite_fraction  = dc::entity::ELITE_CHANCE;   // ~6% roll into a rare golden elite
        spawners.push_back(sp);
    }

    std::vector<Coin> coins;                 // dropped on kills, magnet to the player
    std::vector<XPOrb> xp_orbs;              // blue XP motes dropped on kills, magnet to the local player
    int   pending_levelups = 0;              // queued level-up choices (XP can grant several at once)
    uint32_t levelup_rng = 0x1EEDBEEFu;      // draws the eligible upgrade cards per level-up
    std::vector<float> frame_deaths;         // enemy death positions this frame (xyz triples)
    std::vector<float> frame_death_xp;       // XP per death this frame (parallel to frame_deaths/3)
    std::vector<float> frame_death_gold;     // gold per death this frame (parallel to frame_deaths/3)
    std::vector<float> frame_booms;          // demon fireball explosion positions this frame (xyz triples)
    std::vector<dc::entity::HitNumber> frame_hits;   // damage events this frame (host-computed)
    // Floating damage numbers: rise + fade over their life. Spawned from frame_hits
    // (host/standalone) or the snapshot (client). Rendered as billboarded 7-seg digits.
    struct FloatNum { vec3 pos; float amount; float age; bool crit; };
    std::vector<FloatNum> dmg_numbers;
    constexpr float DMG_LIFE = 1.0f, DMG_RISE = 1.4f;   // seconds visible, world units/s upward
    // Elemental sword sparks (fire rises, ice drifts down, earth scatters): a small
    // simulated pool spawned from each player's blade by their equipped element mask.
    struct Spark { vec3 pos, vel, color; float age = 0.0f, life = 0.0f,
                   grav = 0.0f, size_mul = 1.0f, alpha_mul = 1.0f; };
    std::vector<Spark> sparks;
    uint32_t spark_rng = 0xC0FFEEu;
    // Death disintegration: burst an enemy body into a cloud of tan sand motes that pop
    // outward + up, fall, and fade. Shared by the host (off frame_deaths) and clients (off
    // the replicated death list in the snapshot), so everyone sees the same crumble.
    auto burst_sand = [&](float x, float y, float z) {
        auto frand = [&]() { spark_rng = spark_rng * 1664525u + 1013904223u; return (spark_rng >> 8) * (1.0f / 16777216.0f); };
        const int GRAINS = 60;
        for (int g = 0; g < GRAINS && sparks.size() < 2000; ++g) {
            Spark s;
            s.pos[0] = x + (frand() - 0.5f) * 0.7f;
            s.pos[1] = y + frand() * 1.6f;                 // spread through the body volume
            s.pos[2] = z + (frand() - 0.5f) * 0.7f;
            const float ang = frand() * 6.2831853f, spd = 0.6f + frand() * 1.8f;
            s.vel[0] = std::cos(ang) * spd;
            s.vel[1] = 0.6f + frand() * 1.6f;              // a little pop up before gravity wins
            s.vel[2] = std::sin(ang) * spd;
            const float t = 0.55f + frand() * 0.35f;       // sandy tan, slight variation
            s.color[0] = t + 0.18f; s.color[1] = t; s.color[2] = t * 0.7f;
            s.age = 0.0f; s.life = 0.8f + frand() * 0.6f;
            s.grav = 5.5f; s.size_mul = 1.5f; s.alpha_mul = 2.4f;
            sparks.push_back(s);
        }
    };
    // Fiery explosion burst (demon fireball detonation): a fast outward pop of orange/red
    // embers + a bright flash. Shared by host (off frame_booms) and clients (off the snapshot).
    auto burst_fire = [&](float x, float y, float z) {
        auto frand = [&]() { spark_rng = spark_rng * 1664525u + 1013904223u; return (spark_rng >> 8) * (1.0f / 16777216.0f); };
        const int N = 90;
        for (int g = 0; g < N && sparks.size() < 2200; ++g) {
            Spark s;
            s.pos[0] = x + (frand()-0.5f)*0.5f; s.pos[1] = y + 0.4f + (frand()-0.5f)*0.5f; s.pos[2] = z + (frand()-0.5f)*0.5f;
            const float ang = frand()*6.2831853f, pit = frand()*1.2f, spd = 3.0f + frand()*7.0f;
            s.vel[0] = std::cos(ang)*std::cos(pit)*spd; s.vel[1] = std::sin(pit)*spd + 1.5f; s.vel[2] = std::sin(ang)*std::cos(pit)*spd;
            const float h = frand();
            s.color[0] = 1.0f; s.color[1] = 0.35f + h*0.45f; s.color[2] = 0.08f + h*0.12f;   // orange->yellow
            s.age = 0.0f; s.life = 0.45f + frand()*0.45f;
            s.grav = 4.0f; s.size_mul = 2.4f; s.alpha_mul = 3.0f;
            sparks.push_back(s);
        }
    };
    // Wizard dodge mist: a puff of soft blue-violet motes that drift up + outward as the
    // wizard dissolves into mist (and again as it re-forms). `intensity` scales the count.
    auto burst_mist = [&](float x, float y, float z, int count) {
        auto frand = [&]() { spark_rng = spark_rng * 1664525u + 1013904223u; return (spark_rng >> 8) * (1.0f / 16777216.0f); };
        for (int g = 0; g < count && sparks.size() < 2200; ++g) {
            Spark s;
            s.pos[0] = x + (frand()-0.5f)*0.5f; s.pos[1] = y + frand()*1.7f; s.pos[2] = z + (frand()-0.5f)*0.5f;
            const float ang = frand()*6.2831853f, spd = 0.4f + frand()*1.6f;
            s.vel[0] = std::cos(ang)*spd; s.vel[1] = 0.4f + frand()*1.0f; s.vel[2] = std::sin(ang)*spd;
            const float h = frand();
            s.color[0] = 0.45f + h*0.2f; s.color[1] = 0.6f + h*0.2f; s.color[2] = 1.0f;   // blue-violet
            s.age = 0.0f; s.life = 0.5f + frand()*0.5f;
            s.grav = -0.8f; s.size_mul = 2.0f; s.alpha_mul = 0.4f;   // float gently upward (faint, see-through mist)
            sparks.push_back(s);
        }
    };
    // Ground slam dust: a wide ring of brown-grey debris kicked up across the whole affected
    // disk (e.g. the troll's slam radius). Filled uniformly over the disk so the big AoE reads.
    auto burst_dust = [&](float x, float z, float radius, int count) {
        auto frand = [&]() { spark_rng = spark_rng * 1664525u + 1013904223u; return (spark_rng >> 8) * (1.0f / 16777216.0f); };
        for (int g = 0; g < count && sparks.size() < 2200; ++g) {
            Spark s;
            const float ang = frand() * 6.2831853f;
            const float rr  = radius * std::sqrt(frand());        // uniform over the disk
            s.pos[0] = x + std::cos(ang) * rr;
            s.pos[2] = z + std::sin(ang) * rr;
            s.pos[1] = terrain.height(s.pos[0], s.pos[2]) + 0.1f + frand() * 0.3f;
            const float out = 1.5f + (rr / radius) * 3.5f + frand() * 1.5f;   // outer ring flings further
            s.vel[0] = std::cos(ang) * out; s.vel[2] = std::sin(ang) * out;
            s.vel[1] = 2.0f + frand() * 4.0f;                     // kicked upward
            const float h = 0.28f + frand() * 0.22f;              // earthy brown-grey
            s.color[0] = h * 1.15f; s.color[1] = h; s.color[2] = h * 0.78f;
            s.age = 0.0f; s.life = 0.45f + frand() * 0.55f;
            s.grav = 7.0f; s.size_mul = 2.8f; s.alpha_mul = 0.9f;
            sparks.push_back(s);
        }
    };
    constexpr int START_GOLD = 150;  // both sides open with this (was 100 — a little more buffer)
    int   currency = START_GOLD;
    double team_passive_accum = 0.0;         // gentle passive gold trickle (enemy AI doesn't measure it)
    double host_damage = 0.0;                // host/standalone: this player's total damage dealt (scoreboard)
    float  my_damage = 0.0f;                 // client: our own total, read back from the snapshot
    bool   scoreboard = false;               // hold Tab: damage leaderboard + your items
    bool   tab_prev = false;                 // edge-detect Tab
    float run_time = 0.0f;                    // seconds survived this run (top-of-screen timer)
    float respawn_timer = -1.0f;              // >=0: local player is downed, counting down to revive
    float death_flash = 0.0f;                // red "you died" overlay timer
    float victory_flash = 0.0f;              // gold "VICTORY" overlay timer (enemy base destroyed)

    // Day/night cycle. By day you roam, gather, and the base is safe; at night the horde
    // swells, enemies march on the base, and the solar turrets wake up to defend it.
    // Host-authoritative `tod` (seconds into the cycle) is replicated so all peers agree.
    const float DAY_LEN = 75.0f, NIGHT_LEN = 55.0f, CYCLE_LEN = DAY_LEN + NIGHT_LEN;
    float tod = 0.0f;                         // seconds into the current day/night cycle
    int   day_num = 1;                        // which day we're on (HUD)
    auto is_night = [&]() { return tod >= DAY_LEN; };
    // Map the cycle to a 24h clock: tod=0 (dawn) -> 6:00 AM, wrapping over the full cycle.
    auto clock_str = [&](char* out, std::size_t n) {
        float hf = 6.0f + (tod / CYCLE_LEN) * 24.0f;        // hours since midnight
        if (hf >= 24.0f) hf -= 24.0f;
        int hh = static_cast<int>(hf), mm = static_cast<int>((hf - hh) * 60.0f);
        const char* ap = hh < 12 ? "AM" : "PM";
        int h12 = hh % 12; if (h12 == 0) h12 = 12;
        std::snprintf(out, n, "Day %d   %d:%02d %s", day_num, h12, mm, ap);
    };
    // Smooth 0..1 daylight (1 = noon, ~0 = deep night) with dawn/dusk ramps, for ambient.
    auto daylight01 = [&]() {
        if (tod < DAY_LEN) {                  // day: ramp up at dawn, down at dusk
            const float u = tod / DAY_LEN;    // 0..1 across the day
            return std::min(1.0f, std::min(u, 1.0f - u) * 6.0f);
        }
        const float u = (tod - DAY_LEN) / NIGHT_LEN;          // 0..1 across the night
        return 0.08f + 0.10f * std::min(u, 1.0f - u) * 2.0f;  // stays dark, faint twilight at the edges
    };

    // Two bases face off down the lane: OURS (blue) at the left end, the ENEMY's (red) at the
    // right end. Enemies spawn from the enemy core and march on ours; destroying the enemy core
    // wins the run, losing ours ends it. Both sit on the lane's center row.
    const float CORE_MAX_HEALTH = 2000.0f;
    const float laneZ = map->height * 0.5f * dc::world::TILE;             // center of the lane
    float core_health = CORE_MAX_HEALTH;
    vec3 core_pos = { 16.0f * dc::world::TILE, 0.0f, laneZ };             // player base, left end
    core_pos[1] = terrain.height(core_pos[0], core_pos[2]);
    const uint32_t CORE_ID = 0xC0FFEE01u;   // pseudo-player id for enemy targeting
    // Enemy base (red) at the right end — the objective the lane push is aimed at.
    float enemy_core_health = CORE_MAX_HEALTH;
    vec3 enemy_core_pos = { (map->width - 16.0f) * dc::world::TILE, 0.0f, laneZ };
    enemy_core_pos[1] = terrain.height(enemy_core_pos[0], enemy_core_pos[2]);
    // Core column mesh (static): a tall cylinder, drawn bright so it reads as a glowing pylon.
    const float CORE_H = 5.5f, CORE_RAD = 1.1f;
    dc::renderer::Mesh core_mesh;
    {
        std::vector<float> cv;
        const int SIDES = 16;
        const float y0 = core_pos[1] - 0.5f, y1 = core_pos[1] + CORE_H;
        auto V = [&](float x, float y, float z, float nx, float ny, float nz) { cv.insert(cv.end(), {x,y,z,nx,ny,nz,0,0,0}); };
        for (int k = 0; k < SIDES; ++k) {
            const float a0 = 6.2831853f*k/SIDES, a1 = 6.2831853f*(k+1)/SIDES;
            const float c0 = std::cos(a0), s0 = std::sin(a0), c1 = std::cos(a1), s1 = std::sin(a1);
            const float x0 = core_pos[0]+c0*CORE_RAD, z0 = core_pos[2]+s0*CORE_RAD;
            const float x1 = core_pos[0]+c1*CORE_RAD, z1 = core_pos[2]+s1*CORE_RAD;
            V(x0,y0,z0,c0,0,s0); V(x1,y0,z1,c1,0,s1); V(x1,y1,z1,c1,0,s1);   // side quad
            V(x0,y0,z0,c0,0,s0); V(x1,y1,z1,c1,0,s1); V(x0,y1,z0,c0,0,s0);
            V(core_pos[0],y1,core_pos[2],0,1,0); V(x0,y1,z0,0,1,0); V(x1,y1,z1,0,1,0);  // top cap fan
        }
        core_mesh.upload(cv);
    }
    // Networking transport (enet), declared up here because the base-building helpers below
    // read net.role. Standalone = no socket; host listens; client joins (started further down).
    dc::net::Net net;
    // Energy shield dome over the base: absorbs enemy damage before the core does, flashes
    // red when hit, and has its own (upgradable) health.
    // The player-built base does NOT carry across runs — it starts fresh every run. Seed a
    // ring of default turrets around the perimeter so a new base still has some defense; the
    // player then places more barricades/landmines/turrets during the run.
    dc::game::BaseSave base;
    auto seed_base = [&]() {
        base.pieces.clear();
        base.build_radius = dc::game::BASE_AREA_START;
        // You START WITH NO TURRETS now — buy them from the menu. Only seed a little army.
        // Two starter GRUNT barracks (toward the lane) so an army musters from the first second.
        for (int i = 0; i < 2; ++i) {
            const float bx = core_pos[0] + 4.0f, bz = core_pos[2] + (i == 0 ? 2.5f : -2.5f);
            dc::game::BasePiece bp;
            bp.col = static_cast<int16_t>(bx / dc::world::TILE);
            bp.row = static_cast<int16_t>(bz / dc::world::TILE);
            bp.piece = static_cast<uint8_t>(dc::game::BuildPiece::Barracks);
            bp.rot = 0;   // Grunt
            base.pieces.push_back(bp);
        }
    };
    seed_base();
    // The protective dome / safe zone grows with the buildable area (they're the same radius).
    float shield_radius = base.build_radius;
    const float SHIELD_RECHARGE = 90.0f;   // hp/sec regained while charging (daytime)
    float shield_max = 800.0f;             // upgradable capacity
    float shield_health = shield_max;
    float shield_flash = 0.0f;             // red pulse timer on absorb
    float shield_prev = shield_max;        // to detect drops (client-side flash)
    // --- Base building state ---------------------------------------------------------------
    // The host owns `base.pieces`; clients receive the layout in the snapshot into `net_pieces`
    // and mirror `shield_radius`. `live_pieces()` returns whichever is authoritative locally.
    std::vector<dc::game::BasePiece> net_pieces;     // client: pieces from the snapshot
    // Per-piece RUNTIME state (parallel to the pieces): barricade HP, or landmine armed flag
    // (1 = armed, 0 = spent). Host owns `piece_hp`; clients mirror it into `net_piece_hp`.
    std::vector<float> piece_hp;
    std::vector<float> net_piece_hp;
    // Friendly lane mobs (barracks output). Host owns `allies`; clients render `net_allies`.
    struct Ally { vec3 pos; float yaw = 0.0f, health = 0.0f, max_hp = 1.0f, attack_cd = 0.0f, speed_mul = 1.0f, size_mul = 1.0f, def_mult = 1.0f; uint8_t kind = 0, up = 0; float atk = 0.0f, slam = -1.0f; bool moving = false; };  // up = barracks upgrade level; def_mult = damage-taken scale; moving = advanced this frame (idle vs walk anim)
    std::vector<Ally> allies;
    std::vector<dc::net::AllyState> net_allies;
    const uint32_t ALLY_ID_BASE = 0xA11E0000u;   // pseudo-ids so they slot into the combat target list
    uint32_t ally_rng = 0xA11E5Eedu;
    std::unordered_map<uint32_t, std::pair<float,float>> enemy_prev;   // host: last enemy xz, for water drag
    // Naval units: enemy BOATS that patrol the river firing cannonballs (kind 0). Host owns
    // them; clients render `net_boats`. (Submarines reuse this with kind 1/2, added later.)
    struct Boat { vec3 pos; float yaw = 0.0f, health = 0.0f, fire_cd = 0.0f, surf = 0.0f; uint8_t kind = 0, team = 0, role = 0; uint32_t id = 0; int rider = -1; };  // role: 0 warship, 1 minelayer, 2 minesweeper. rider: -1 none, 0 host, >=1 client
    std::vector<Boat> boats;
    std::vector<dc::net::BoatState> net_boats;   // host serializes BOTH boats (kind 0) and subs (kind 1/2) here
    uint32_t next_boat_id = 1;
    float boat_spawn_cd = 6.0f;
    // SUBMARINES (both teams). kind 1 = submerged periscope, 2 = surfaced (firing); team 0 = enemy,
    // 1 = friendly. Reuse the Boat struct; `fire_cd` doubles as the surface-timer when >0.
    std::vector<Boat> subs;
    // Enemy SEA-MINES bobbing in the river — the AI's answer to our submarines. A friendly sub or
    // warship that drifts within blast range detonates one (big damage). Host owns; clients render.
    struct NavalMine { vec3 pos; float arm = 0.0f; uint32_t id = 0; uint8_t team = 0; };   // team = who laid it (hits the OTHER team)
    std::vector<NavalMine> naval_mines;
    std::vector<dc::net::MineState> net_mines;
    uint32_t next_mine_id = 1;
    // --- ENEMY ECONOMY + ADVISOR AI (host-authoritative brain). The enemy earns gold a little
    // FASTER than we farm it, then a panel of "advisors" each score (0..10) how badly it needs a
    // given item; the next savings target is sampled by those weights, and bought when affordable.
    double enemy_gold      = START_GOLD;  // the enemy's bank; starts equal to ours, every unit is paid for
    float  enemy_rate      = 2.0f;     // gold/sec, sized just above our farm rate
    float  our_gold_rate   = 1.0f;     // smoothed gold/sec WE drop on the ground (what the enemy scales off)
    double gold_drop_accum = 0.0;      // gold dropped during the current 1s measurement window
    float  gold_drop_timer = 1.0f;
    float  enemy_speed_mult = 1.0f;    // CAVALRY purchases speed up the enemy ground horde
    int    enemy_troop_cap  = 14;      // concurrent enemy lane units the barracks will keep alive
    float  enemy_build_radius = 9.0f;  // the enemy's buildable base radius (around its core); EXPAND grows it
    // The enemy fields troops by BUILDING BARRACKS on actual TILES in its base (persistent spawners),
    // just like the player. When it runs out of tiles, the EXPAND advisor spikes to buy more base area.
    struct EnemyBarracks { uint8_t kind = 0, up = 0; float cd = 0.0f; float x = 0.0f, z = 0.0f; };   // kind = EnemyKind, up = upgrade level
    std::vector<EnemyBarracks> ebarracks;
    int    ai_target        = -1;      // item the enemy is saving toward (-1 = none)
    float  ai_decide_cd     = 0.0f;    // cadence between AI decisions
    float  ai_resample_cd   = 0.0f;    // re-pick the savings target at least every half-day
    float  ai_unmet[16]     = {};      // per-item "overdue" timers: a long-unmet critical need ramps toward a (rare) 10
    uint32_t ai_rng         = 0x51A7E5u;
    std::string ai_status   = "AI: warming up";   // debug HUD line (Shift+` hitbox mode shows it)
    // --- Session telemetry: the host writes a timeline to ./session.log (snapshots every few
    // seconds + AI purchases + day/win/loss) so a playthrough can be reviewed afterward.
    std::FILE* glog = std::fopen("session.log", "w");
    float log_timer = 0.0f;
    auto LOGLINE = [&](const char* s){ if (glog) { std::fprintf(glog, "[t=%6.1f d=%d] %s\n", run_time, day_num, s); std::fflush(glog); } };
    if (glog) std::fprintf(glog, "# dungeoncrawl session log. SNAP every 5s; BUY = enemy AI purchase.\n"
                                 "# our$ = our shared gold; ourRate = gold/s we DROP on the ground; eGold/eRate = enemy bank/income.\n"
                                 "# eBoat/eSub/eMine/eTurret/eSpd = enemy forces; pBoat/pSub/pMount = ours the AI sees; core/eCore = base HP.\n");
    // The enemy starts with ONE skeleton barracks on a tile near its core (mirrors our starter barracks).
    ebarracks.push_back({ static_cast<uint8_t>(skeleton_loaded ? dc::entity::EnemyKind::Skeleton : dc::entity::EnemyKind::Melee), 0, 0.0f,
                          enemy_core_pos[0] - 3.0f*dc::world::TILE, enemy_core_pos[2] });
    // Slime puddles left by slime enemies (host owns; clients render `net_slime_patches`). They
    // slow the player + friendly mobs that wade through them (enemies/slimes are immune).
    struct SlimePatch { vec3 pos; float radius = 1.0f, life = 0.0f, max_life = 1.0f; };
    std::vector<SlimePatch> slime_patches;
    std::vector<dc::net::SlimePatchState> net_slime_patches;
    struct SlimeTrack { float cd, x, z; };
    std::unordered_map<uint32_t, SlimeTrack> slime_track;   // host: per-slime drop timer + last pos
    const float SLIME_SLOW = 0.5f;
    const float BOAT_MAX_HP = 700.0f, BOAT_SPEED = 1.8f, BOAT_RANGE = 38.0f, BOAT_FIRE_CD = 2.6f;   // long range is the point
    const float BOARD_RANGE = 6.0f;   // press F this close to a friendly boat to climb aboard
    const float FRIENDLY_BOAT_HP = 7000.0f;   // your warships are VERY tanky — they wade through a swarm
    bool building_mode = false;                      // B toggles the build/place mode
    int  build_sel = 0;                              // selected piece kind (0..Count-1)
    int  build_rot = 0;                              // current placement rotation (0..3)
    int  build_tier = 0;                             // for Barracks: which mob TYPE to build (0..MOB_TYPE_COUNT-1)
    int  shipyard_type = 0;                          // for Shipyard: 0 warship, 1 minelayer, 2 minesweeper
    bool bld_t_prev = false;                          // edge-detect the tier-cycle key
    // Barracks mob types must be UNLOCKED before they can be built. Bit i = type i unlocked;
    // Grunt (0) is free from the start. Host-authoritative; clients mirror it from the snapshot.
    uint32_t barracks_unlocked = 1u;                 // bit 0 (Grunt) unlocked
    // Rally point: when set, your fighter mobs march to + HOLD this spot instead of pushing all
    // the way to the enemy core (they still fight enemies in range). Host-owned, replicated.
    bool rally_active = false; vec3 rally_pos = { 0.0f, 0.0f, 0.0f };
    bool rally_c_prev = false, rally_x_prev = false;
    // Command map (M): per-mob-TYPE hold positions along the lane. type_hold_x[t] >= 0 means that
    // type holds at that world-X (a vertical "front line" to hold); < 0 means AUTO (march on the
    // enemy base). Host-owned, replicated. Drag a pin off the minimap to set it back to auto.
    bool cmd_map = false;                             // the command minimap is open
    float type_hold_x[dc::game::MOB_TYPE_COUNT];
    for (int i = 0; i < dc::game::MOB_TYPE_COUNT; ++i) type_hold_x[i] = -1.0f;
    int cmd_drag = -1;                                // which type's pin is being dragged (-1 = none)
    bool cmd_m_prev = false, cmd_lmb_prev = false;
    float cmd_drag_mx = 0.0f, cmd_drag_my = 0.0f;     // live drag cursor (NDC) for the pin preview
    bool spawn_menu = false;                          // the base "Muster" menu (E near the core)
    bool spawn_digit_prev[10] = {};
    bool spawn_lmb_prev = false;                      // edge-detect clicks on muster-menu rows
    bool upgrade_menu = false;                         // barracks UPGRADE menu (E near a barracks)
    int  upg_col = 0, upg_row = 0;                     // the barracks tile being upgraded
    bool upg_digit_prev[5] = {};
    bool base_dirty = false;                         // host: base layout changed this frame
    auto piece_full_hp = [](int piece) -> float {
        if (piece == static_cast<int>(dc::game::BuildPiece::Barricade)) return dc::game::BARRICADE_MAX_HP;
        if (piece == static_cast<int>(dc::game::BuildPiece::Barracks))  return 1.0f;  // spawn timer (per-type interval set on spawn)
        if (piece == static_cast<int>(dc::game::BuildPiece::SubPen))     return 3.0f;  // first sub launches ~3s after building
        if (piece == static_cast<int>(dc::game::BuildPiece::Shipyard))   return 3.0f;  // first warship launches ~3s after building
        return 1.0f;   // landmine: armed; turret: unused
    };
    // Edge-detect + ghost-target state (persisted across frames; read by the renderer/HUD).
    bool b_prev = false, bld_r_prev = false, bld_f_prev = false, bld_lmb_prev = false, bld_rmb_prev = false;
    bool board_prev = false;                         // F-edge: board / dismount a friendly boat
    bool digit_prev[6] = {};
    bool barup_prev[4] = {};                          // edge-detect the barracks-upgrade keys (6/7/8/9)
    int  build_col = 0, build_row = 0;               // tile the crosshair is aiming at
    bool build_has_target = false, build_valid = false;
    auto live_pieces = [&]() -> const std::vector<dc::game::BasePiece>& {
        return (net.role == dc::net::Role::Client) ? net_pieces : base.pieces;
    };
    auto live_hp = [&]() -> const std::vector<float>& {
        return (net.role == dc::net::Role::Client) ? net_piece_hp : piece_hp;
    };
    // Initial runtime HP for the seeded base.
    piece_hp.resize(base.pieces.size());
    for (std::size_t i = 0; i < base.pieces.size(); ++i) piece_hp[i] = piece_full_hp(base.pieces[i].piece);
    const float NO_BUILD_INNER = CORE_RAD + 2.0f;    // keep the glyph clear: no building on/around it
    // The RIVER: a winding channel of water cutting down the lane from our base to the enemy's,
    // a few blocks wide, opening into a wide basin in the center for ship warfare. Returns the
    // signed distance INTO the water (>0 = in water, magnitude ~ how deep toward center).
    // River centerline is biased toward one side (~30% across) so the MIDDLE of the field stays
    // land for ground troops; it still winds + opens into a basin in the lane's center.
    const float riverZ = map->height * 0.30f * dc::world::TILE;
    auto river_depth = [&](float wx, float wz) -> float {
        const float x0 = core_pos[0] + 8.0f, x1 = enemy_core_pos[0] - 8.0f;   // clear of both cores
        if (wx < x0 || wx > x1) return -1.0f;
        const float t = (wx - x0) / (x1 - x0);                                // 0..1 along the lane
        const float wind = std::sin(t * 6.2831853f * 1.6f) * 4.5f;            // gentle winding
        const float center = riverZ + wind;
        const float bump = std::exp(-((t - 0.5f) * (t - 0.5f)) / (2.0f * 0.018f));  // center basin
        const float half = 3.2f + bump * 10.0f;                               // ~3 blocks wide; opens up mid-map
        return half - std::fabs(wz - center);                                 // >0 inside the river
    };
    auto in_water = [&](float wx, float wz) -> bool { return river_depth(wx, wz) > 0.0f; };
    // Standing in a slime puddle? (host reads slime_patches, clients net_slime_patches.)
    auto in_slime = [&](float wx, float wz) -> bool {
        if (net.role == dc::net::Role::Client) {
            for (const auto& s : net_slime_patches) { const float dx=s.x-wx, dz=s.z-wz; if (dx*dx+dz*dz < s.radius*s.radius) return true; }
        } else {
            for (const auto& s : slime_patches) { const float dx=s.pos[0]-wx, dz=s.pos[2]-wz; if (dx*dx+dz*dz < s.radius*s.radius) return true; }
        }
        return false;
    };
    // The river channel centerline Z at a given X (boats steer to follow it).
    auto channel_center = [&](float wx) -> float {
        const float x0 = core_pos[0] + 8.0f, x1 = enemy_core_pos[0] - 8.0f;
        const float t = (wx - x0) / (x1 - x0);
        return riverZ + std::sin(t * 6.2831853f * 1.6f) * 4.5f;
    };
    const float river_x0 = core_pos[0] + 8.0f;
    dc::renderer::Mesh river_mesh;   // built once (lazily, after GL is up) from river_depth
    dc::renderer::Mesh boat_mesh;    // naval hulls, rebuilt each frame (they bob + turn)
    // (Simplified: water no longer penalizes pathfinding — mobs walk through the river at full
    // speed. The river is purely a visual feature + the boats' domain.)
    // A tile is buildable if its center is inside the dome AND outside the glyph's no-build ring.
    auto tile_buildable = [&](int col, int row) -> bool {
        const float wx = (col + 0.5f) * dc::world::TILE, wz = (row + 0.5f) * dc::world::TILE;
        const float dx = wx - core_pos[0], dz = wz - core_pos[2];
        const float d = std::sqrt(dx * dx + dz * dz);
        if (river_depth(wx, wz) > -0.5f) return false;   // can't build in (or right at) the river
        return d <= shield_radius && d >= NO_BUILD_INNER;
    };
    auto piece_index_at = [&](int col, int row) -> int {
        for (std::size_t i = 0; i < base.pieces.size(); ++i)
            if (base.pieces[i].col == col && base.pieces[i].row == row) return static_cast<int>(i);
        return -1;
    };
    // Host-authoritative edits (used by the local host player AND by client request messages).
    // `wallet` is the requesting player's coin purse; cost is deducted there.
    auto host_place = [&](int col, int row, int piece, int rot, int& wallet) -> bool {
        if (!tile_buildable(col, row) || piece_index_at(col, row) >= 0) return false;
        // Barracks cost the selected mob TYPE's place_cost (the type is stored in `rot`) and
        // require that type to be UNLOCKED first.
        const bool barracks = (piece == static_cast<int>(dc::game::BuildPiece::Barracks));
        if (barracks && !(barracks_unlocked & (1u << rot))) return false;
        if (barracks) {   // capacity: only so many barracks per base (grows with area expansions)
            int nbar = 0; for (const auto& q : base.pieces) if (q.piece == static_cast<uint8_t>(dc::game::BuildPiece::Barracks)) ++nbar;
            if (nbar >= dc::game::barracks_capacity(base.build_radius)) return false;
        }
        const int cost = barracks ? dc::game::mob_type(rot).place_cost
                       : dc::game::piece_cost(static_cast<dc::game::BuildPiece>(piece));
        if (wallet < cost) return false;
        wallet -= cost;
        base.pieces.push_back({ static_cast<int16_t>(col), static_cast<int16_t>(row),
                                static_cast<uint8_t>(piece), static_cast<uint8_t>(rot), {0,0,0,0} });
        piece_hp.push_back(piece_full_hp(piece));
        base_dirty = true; return true;
    };
    auto host_remove = [&](int col, int row, int& wallet) -> bool {
        const int idx = piece_index_at(col, row);
        if (idx < 0) return false;
        // A barracks refunds 75% of its mob type's PLACE cost; other pieces give the usual half back.
        if (base.pieces[idx].piece == static_cast<uint8_t>(dc::game::BuildPiece::Barracks))
            wallet += (dc::game::mob_type(base.pieces[idx].rot).place_cost * 3) / 4;
        else
            wallet += dc::game::piece_cost(static_cast<dc::game::BuildPiece>(base.pieces[idx].piece)) / 2;
        base.pieces[idx] = base.pieces.back(); base.pieces.pop_back();
        if (static_cast<std::size_t>(idx) < piece_hp.size()) { piece_hp[idx] = piece_hp.back(); piece_hp.pop_back(); }
        base_dirty = true; return true;
    };
    auto host_buy_area = [&](int& wallet) -> bool {
        if (base.build_radius >= dc::game::BASE_AREA_MAX) return false;
        const int cost = dc::game::base_area_cost(base.build_radius);
        if (wallet < cost) return false;
        wallet -= cost;
        base.build_radius += dc::game::BASE_AREA_STEP;
        shield_radius = base.build_radius;
        base_dirty = true; return true;
    };
    auto host_unlock = [&](int tier, int&) -> bool {
        if (tier < 0 || tier >= dc::game::MOB_TYPE_COUNT) return false;
        if (barracks_unlocked & (1u << tier)) return false;            // already unlocked
        barracks_unlocked |= (1u << tier); return true;               // unlocking is FREE now (place cost still applies)
    };
    // Upgrade ONE barracks' troops in a stat (0=HP,1=DEF,2=SPEED,3=RATE). Cost scales with the level.
    auto host_upgrade_barracks = [&](int col, int row, int stat, int& wallet) -> bool {
        if (stat < 0 || stat >= dc::game::BARRACKS_UP_STATS) return false;
        const int idx = piece_index_at(col, row);
        if (idx < 0 || base.pieces[idx].piece != static_cast<uint8_t>(dc::game::BuildPiece::Barracks)) return false;
        if (base.pieces[idx].up[stat] >= dc::game::BARRACKS_UP_MAX) return false;
        const int cost = dc::game::barracks_upgrade_cost(base.pieces[idx].up[stat]);
        if (wallet < cost) return false;
        wallet -= cost; base.pieces[idx].up[stat]++; base_dirty = true; return true;
    };

    // Solar turrets are now PLACED build pieces (a working gun you put where you want); their
    // world positions are derived from every Turret piece in the layout. Host does the damage;
    // aim + tracers are computed per-peer from the (replicated) enemies, like the gunner minions.
    const float TURRET_RANGE = 18.0f, TURRET_DAMAGE = 16.0f, TURRET_FIRE_INTERVAL = 0.55f;
    struct TPos { float x, y, z; };
    std::vector<TPos>  turret_pos;
    std::vector<TPos>  turret_aim;        // last-known aim dir (3D, unit); frozen when no target
    std::vector<float> turret_cd;         // host per-turret fire timers
    std::vector<float> turret_flash;      // per-turret tracer-fire timer
    uint64_t turret_built_sig = ~0ull;    // signature of the turret layout the arrays were built for
    auto turret_sig = [&](const std::vector<dc::game::BasePiece>& ps) {
        uint64_t s = 1469598103934665603ull; uint32_t n = 0;
        for (const auto& p : ps) if (p.piece == static_cast<uint8_t>(dc::game::BuildPiece::Turret)) {
            s = (s ^ static_cast<uint32_t>(p.col * 73856093 ^ p.row * 19349663)) * 1099511628211ull; ++n;
        }
        return s ^ (static_cast<uint64_t>(n) << 1);
    };
    // Rebuild turret world positions from the current pieces, but only when the layout actually
    // changed (so per-turret cooldown/aim/flash state survives between frames).
    auto rebuild_turrets = [&]() {
        const auto& ps = live_pieces();
        const uint64_t sig = turret_sig(ps);
        if (sig == turret_built_sig) return;
        turret_built_sig = sig;
        std::vector<TPos> np;
        for (const auto& p : ps) if (p.piece == static_cast<uint8_t>(dc::game::BuildPiece::Turret)) {
            const float x = (p.col + 0.5f) * dc::world::TILE, z = (p.row + 0.5f) * dc::world::TILE;
            np.push_back({ x, terrain.height(x, z), z });
        }
        const std::size_t n = np.size();
        turret_pos = std::move(np);
        turret_aim.resize(n, { 1.0f, 0.0f, 0.0f });   // resize preserves existing entries by index
        turret_cd.resize(n, 0.0f);
        turret_flash.resize(n, 0.0f);
    };
    rebuild_turrets();
    // --- MORTAR artillery: positions rebuilt from Mortar pieces (parallel CD survives frames). ---
    std::vector<TPos>  mortar_pos;
    std::vector<float> mortar_cd;
    uint64_t mortar_built_sig = ~0ull;
    auto mortar_sig = [&](const std::vector<dc::game::BasePiece>& ps) {
        uint64_t s = 1469598103934665603ull; uint32_t n = 0;
        for (const auto& p : ps) if (p.piece == static_cast<uint8_t>(dc::game::BuildPiece::Mortar)) {
            s = (s ^ static_cast<uint32_t>(p.col * 73856093 ^ p.row * 19349663)) * 1099511628211ull; ++n;
        }
        return s ^ (static_cast<uint64_t>(n) << 1);
    };
    auto rebuild_mortars = [&]() {
        const auto& ps = live_pieces();
        const uint64_t sig = mortar_sig(ps);
        if (sig == mortar_built_sig) return;
        mortar_built_sig = sig;
        std::vector<TPos> np;
        for (const auto& p : ps) if (p.piece == static_cast<uint8_t>(dc::game::BuildPiece::Mortar)) {
            const float x = (p.col + 0.5f) * dc::world::TILE, z = (p.row + 0.5f) * dc::world::TILE;
            np.push_back({ x, terrain.height(x, z), z });
        }
        mortar_cd.resize(np.size(), 2.5f);   // first shot a couple seconds after building
        mortar_pos = std::move(np);
    };
    rebuild_mortars();
    // In-flight mortar shells (host sims the AoE on impact; every peer renders the arc + boom).
    struct MortarShell { vec3 from, impact; float t = 0.0f, dur = dc::game::MORTAR_SHELL_TIME; };
    std::vector<MortarShell> mortar_shells;
    // Travelling turret tracer rounds (cosmetic; the damage is instant at fire time). Each
    // peer spawns them from the synced phase pulse so everyone sees the same fire.
    struct TBullet { vec3 pos, vel; float life; bool red = false; };
    std::vector<TBullet> turret_bullets;
    // Nearest live enemy to a turret within range (xz). Shared by host damage + render aim.
    auto turret_target = [&](float tx, float tz) -> const dc::entity::Entity* {
        const dc::entity::Entity* best = nullptr; float bd2 = TURRET_RANGE * TURRET_RANGE;
        for (const auto& e : entities.items) {
            if (e.type != dc::entity::EntityType::Enemy || !e.alive) continue;
            const float dx = e.position[0] - tx, dz = e.position[2] - tz;
            const float d2 = dx*dx + dz*dz;
            if (d2 < bd2) { bd2 = d2; best = &e; }
        }
        return best;
    };
    dc::renderer::Mesh turret_mesh;   // rebuilt each frame (body + aimed gun)
    // ENEMY-BASE turrets: a red ring around the enemy core that fires on our pushing army (and
    // the player if close). The enemy ADDS turrets over time (like we buy ours) — only the first
    // `enemy_turret_n` of this precomputed ring are active, growing with run_time.
    const int   ENEMY_TURRET_MAX = 8;
    const float ENEMY_TURRET_RING = 6.5f, ENEMY_TURRET_RANGE = 17.0f, ENEMY_TURRET_DAMAGE = 12.0f, ENEMY_TURRET_CD = 0.7f;
    int enemy_turret_n = 1;   // grows over the run (host sets it; clients read it from the snapshot via run_time)
    std::vector<TPos>  eturret_pos(ENEMY_TURRET_MAX);
    std::vector<TPos>  eturret_aim(ENEMY_TURRET_MAX);
    std::vector<float> eturret_cd(ENEMY_TURRET_MAX, 0.0f);
    std::vector<float> eturret_flash(ENEMY_TURRET_MAX, 0.0f);
    for (int i = 0; i < ENEMY_TURRET_MAX; ++i) {
        const float a = 6.2831853f * i / ENEMY_TURRET_MAX;
        const float x = enemy_core_pos[0] + std::cos(a) * ENEMY_TURRET_RING, z = enemy_core_pos[2] + std::sin(a) * ENEMY_TURRET_RING;
        eturret_pos[i] = { x, terrain.height(x, z), z };
        eturret_aim[i] = { -std::cos(a), 0.0f, -std::sin(a) };   // aim inward/down-lane initially
        eturret_flash[i] = i * 0.08f;
    }
    // Base build-piece meshes: one per kind (so each draws in its own color) + a ghost preview.
    // Rebuilt each frame from the placed pieces (cheap for a base-sized layout).
    dc::renderer::Mesh build_mesh[static_cast<int>(dc::game::BuildPiece::Count)];
    dc::renderer::Mesh build_ghost_mesh;

    dc::renderer::Camera camera;

    dc::entity::Player player;
    player.position[0] = (map->spawn_col + 0.5f) * dc::world::TILE;
    player.position[1] = dc::world::EYE_HEIGHT;
    player.position[2] = (map->spawn_row + 0.5f) * dc::world::TILE;
    glm_vec3_copy(player.position, player_prev);   // seed velocity tracking (no first-frame spike)

    dc::input::Input input;

    // Networking transport (enet). Standalone = no socket; host listens; client joins.
    // (Declared earlier, above the base-building state, since those helpers read net.role.)
    if (net_role == dc::net::Role::Host) {
        if (net.start_host(net_port)) std::printf("[net] hosting on port %u\n", net_port);
        else std::fprintf(stderr, "[net] failed to host on port %u\n", net_port);
    } else if (net_role == dc::net::Role::Client) {
        if (net.start_client(connect_ip, net_port)) std::printf("[net] connecting to %s:%u\n", connect_ip, net_port);
        else std::fprintf(stderr, "[net] failed to start client\n");
    }
    std::vector<dc::net::Event> net_events;

    // Replication state. A "remote" is another player we render (not simulate locally).
    // One thrown sword's render state on a remote player (several at once with Swordstorm).
    struct ThrownVis { float x = 0.0f, y = 0.0f, z = 0.0f, spin = 0.0f, size = 1.0f; };
    struct Remote {
        uint32_t id; vec3 pos; float yaw, pitch, anim_time; bool moving;
        float damage_dealt = 0.0f;   // total damage to enemies (from snapshot; scoreboard)
        uint8_t elements = 0;        // elemental sword brands bitmask (for particles)
        uint8_t minions = 0;         // gunner minion count (for the orbiting drones)
        float  minion_range = 18.0f; // drone targeting range (for laser visuals)
        float  trail_life = 0.0f;    // trailblazer segment lifetime (>0 = leaving a fire trail)
        bool ghost = false;   // dead player: render faint + translucent, no gear
        bool burning = false; // on fire (flamethrower) -> emit flame motes
        bool punching = false, blocking = false;
        float punch_time = 0.0f, block_time = 0.0f, hit_flash = 0.0f, sword_scale = 1.0f;
        // Specials (render-only mirror of the owner's thrown/orbit state). Thrown swords
        // are a list (Swordstorm), filled from the owner-keyed snapshot ThrownState.
        std::vector<ThrownVis> throwns;
        bool orbit_active = false; int orbit_count = 0; float orbit_angle = 0.0f, orbit_spin = 0.0f, orbit_radius = 0.0f;
        bool bash_active = false; float bash_radius = 0.0f;
    };
    std::vector<Remote> remotes;
    std::vector<dc::renderer::Mat4> remote_part_world;   // scratch for posing remotes
    // Host side: one simulated body per connected client (host runs their movement).
    struct HostClient {
        uint32_t id, peer; dc::entity::Player body; dc::net::InputCmd input; float anim_time = 0.0f;
        std::vector<uint32_t> orbit_hits;                // per-client orbit hit set (host-side damage)
        float orbit_tick_cd = 0.0f;                      // host-run orbit damage cadence
        float orbit_tick = 0.25f, orbit_spin_mult = 1.0f;// Orbit Tempo, carried in the cast
        int currency = 0;                                // this client's own wallet (host-authoritative)
        double damage_dealt = 0.0;                       // running total dealt to enemies (scoreboard)
        float  minion_fire_cd = 0.0f;                    // host: this client's gunner volley timer
        bool   board_prev = false;                       // host-side edge detect for this client's board key
        float  respawn_timer = -1.0f;                    // >=0: downed, counting down to revive
        vec3   prev_pos = {0,0,0};                       // last frame's position (for shot-leading velocity)
        // Specials are host-run from the client's reliable *Cast events: the host owns
        // the motion + damage on its clock and broadcasts the state. (Same model as bash.)
        bool  bash_active = false; float bash_time = 0.0f, bash_radius = 0.0f;
        float bash_max_radius = 0.0f, bash_damage = 0.0f, bash_knockback = 0.0f, bash_duration = 0.0f;
        std::vector<uint32_t> bash_hits;
        bool  orbit_active = false; float orbit_time = 0.0f, orbit_angle = 0.0f, orbit_spin = 0.0f;
        float orbit_duration = 0.0f, orbit_radius = 0.0f, orbit_hit_radius = 0.0f, orbit_damage = 0.0f, orbit_knockback = 0.0f;
        int   orbit_count = 0;
        // In-flight thrown swords for this client (Swordstorm volleys -> several at once).
        // Each carries its own motion + the damage params from its cast.
        struct HcThrown {
            bool  returning = false;
            float pos[3] = {0,0,0}, dir[3] = {0,0,0}, traveled = 0.0f, spin = 0.0f, size = 1.0f;
            float speed = 0.0f, distance = 0.0f, radius = 0.0f, damage = 0.0f, knockback = 0.0f;
            std::vector<uint32_t> hit_ids;
        };
        std::vector<HcThrown> throwns;
    };
    std::vector<HostClient> host_clients;
    uint32_t next_player_id = 1;   // host = 0; clients get 1,2,...
    uint32_t my_id = 0;            // client: our id (assigned by host)

    float anim_time = 0.0f;                       // walk-clip clock (advances while moving)
    float punch_time = 0.0f;                      // punch-clip clock (advances while punching)
    float block_time = 0.0f;                      // block-clip clock (advances while blocking)
    bool  punching = false;
    bool  blocking = false;
    bool  exhausted = false;                           // winded: must recover before blocking
    bool  shift_prev = false;                          // edge-trigger for the dodge-dash
    float roll_t = 0.0f;                               // knight dodge-roll timer (counts down to 0)
    int   roll_dir = 0;                                // 0 = forward roll, 1 = left, 2 = right side-roll
    float roll_yaw = 0.0f;                             // facing for a forward roll (world dir of the dodge)
    const float ROLL_DUR = 0.5f;                       // seconds for one full forward roll
    std::unordered_set<uint32_t> troll_slamming;       // troll ids mid-slam (edge-detect dust burst)
    bool  paused = false;                              // ESC pause menu (frees the cursor)
    bool  esc_prev = false;                            // edge-detect ESC
    bool  pause_click_prev = false;                    // edge-detect the quit-button click
    bool  menu_click_prev = false;                     // edge-detect the chest-menu buy click
    bool  levelup_click_prev = false;                  // edge-detect the level-up card click
    int   inventory[UPGRADE_COUNT] = {0, 0, 0, 0};     // how many of each upgrade picked (RoR2-style stacks)
    bool  punch_struck = false;                        // strike lands once per punch
    float attack_cd = 0.0f;                            // weapon cooldown between swings
    bool  punch_is_throw = false;                      // this punch clip is a sword throw
    float throw_cd = 0.0f;                             // cooldown between throws
    // In-flight thrown swords. Usually one, but the Swordstorm upgrade launches a small
    // fan of them at once. While any are in flight the hand is empty.
    struct ThrownSword {
        bool  active = false, returning = false;
        vec3  pos = {0.0f, 0.0f, 0.0f};
        vec3  dir = {0.0f, 0.0f, 0.0f};
        float traveled = 0.0f;
        float spin = 0.0f;
        std::vector<uint32_t> hit_ids;   // enemies hit this pass (cleared on the return leg)
    };
    std::vector<ThrownSword> throwns;
    // Wizard staff bolts. On host/standalone, `bolts` holds every in-flight bolt (host +
    // clients), simulated + damaging here. Clients send a BoltCast and render `render_bolts`
    // mirrored from the snapshot (they don't sim damage locally).
    struct Bolt { vec3 pos, dir; float traveled = 0.0f, radius = 0.5f, damage = 18.0f, knockback = 6.0f;
                  bool big = false; uint32_t owner = 0; std::vector<uint32_t> hit_ids; };
    std::vector<Bolt> bolts;
    struct BoltVis { vec3 pos; bool big; };
    std::vector<BoltVis> render_bolts;   // client: bolts to draw, from the snapshot
    struct BoltSpark { vec3 pos; float age = 0.0f, life = 0.38f, sz = 0.11f; };
    std::vector<BoltSpark> bolt_sparks;  // persistent fading particle trail streaming behind staff bolts
    float bolt_cd = 0.0f;                // small-bolt (LMB) cooldown
    constexpr float BOLT_SPEED = 32.0f, BOLT_RANGE = 32.0f;
    // Orbit special (2): spinning swords circling the player for a short time.
    struct Orbit {
        bool  active = false;
        float time = 0.0f;       // seconds remaining
        float angle = 0.0f;      // revolve angle around the player
        float spin = 0.0f;       // each sword's own spin
        float tick = 0.0f;       // time until the next damage tick
        std::vector<uint32_t> hit_ids;
    } orbit;
    float orbit_cd = 0.0f;       // cooldown between casts
    // Shield-bash nova (3): an expanding sphere that shoves everything back.
    struct Bash {
        bool  active = false;
        float time = 0.0f;       // seconds since cast
        float radius = 0.0f;     // current shockwave radius (grows over bash_duration)
        std::vector<uint32_t> hit_ids;   // enemies already shoved this cast (hit once as the front passes)
    } bash;
    float bash_cd = 0.0f;        // cooldown between bashes
    std::vector<dc::renderer::Mat4> part_world;        // posed per-part transforms (player)
    std::vector<dc::renderer::Mat4> chest_part_world;  // posed per-part transforms (a chest)
    std::vector<dc::renderer::AnimLayer> layers;       // reused each frame
    bool e_prev = false;                               // for edge-triggered interact
    bool taunt_prev = false;                            // edge-triggered player insult (I key)
    bool g_prev = false;                               // edge-triggered debug enemy spawn (melee)
    bool h_prev = false;                               // edge-triggered debug enemy spawn (ranged)
    bool j_prev = false;                               // edge-triggered debug enemy spawn (flying)
    bool v_prev = false;                               // edge-triggered first/third-person toggle (V)
    bool first_person = false;                         // false = third-person (default); V toggles
    bool grave_prev = false;                           // edge-triggered hitbox toggle (`)
    bool show_hitboxes = false;                        // ` : draw combat cones / hitboxes + debug readout

    const float base_knockback   = player.stats.knockback;  // for clearing upgrades on death
    const float base_dash_speed  = player.dash_speed;       // Lunge upgrade resets to this
    const float base_dash_iframes = player.dash_iframes;    // Afterimage upgrade resets to this
    const float base_crit_chance = player.crit_chance;      // Keen Edge resets to this
    const float base_crit_mult   = player.crit_mult;        // Deathblow resets to this
    const float base_health_regen = player.health_regen;    // Regeneration resets to this
    const float base_fire_dps = player.fire_dps, base_ice_slow = player.ice_slow, base_earth_knock = player.earth_knock;
    const float base_minion_damage = player.minion_damage;   // Munitions resets to this; Gunner count -> 0
    const float base_minion_range = player.minion_range;     // Drone Sensors resets to this
    const float base_trail_damage = player.trail_damage, base_trail_life = player.trail_life;
    const float base_supersonic = player.supersonic_damage;
    const int   base_orbit_count = player.weapon ? player.weapon->orbit_count : 3;   // More Blades resets to this
    const float base_bash_radius = player.shield ? player.shield->bash_radius : 4.5f; // Wider Nova resets to this

    // Reset the run (solo death = game over -> start over). Player back to spawn at
    // full health/stamina, currency + upgrades cleared, enemies/coins reset.
    auto reset_run = [&]() {
        core_health = CORE_MAX_HEALTH; shield_health = shield_max; shield_flash = 0.0f;   // base + shield restored
        enemy_core_health = CORE_MAX_HEALTH;   // the enemy base is rebuilt too
        allies.clear(); net_allies.clear();    // the lane army resets with the run
        boats.clear(); net_boats.clear(); subs.clear();   // sink any naval units
        naval_mines.clear(); net_mines.clear();
        boat_spawn_cd = 6.0f;
        // Reset the enemy economy/advisor AI for the new run.
        enemy_gold = START_GOLD; enemy_rate = 2.0f; our_gold_rate = 1.0f; gold_drop_accum = 0.0; gold_drop_timer = 1.0f;
        enemy_speed_mult = 1.0f; enemy_troop_cap = 14; enemy_build_radius = 9.0f; ai_target = -1; ai_decide_cd = 0.0f; ai_resample_cd = 0.0f;
        for (int i = 0; i < 16; ++i) ai_unmet[i] = 0.0f;
        ebarracks.clear();
        ebarracks.push_back({ static_cast<uint8_t>(skeleton_loaded ? dc::entity::EnemyKind::Skeleton : dc::entity::EnemyKind::Melee), 0, 0.0f,
                              enemy_core_pos[0] - 3.0f*dc::world::TILE, enemy_core_pos[2] });
        enemy_turret_n = 1;
        slime_patches.clear(); net_slime_patches.clear(); slime_track.clear();   // clear slime
        barracks_unlocked = 1u;                 // only Grunt unlocked again
        rally_active = false;                   // clear any hold order
        for (int i = 0; i < dc::game::MOB_TYPE_COUNT; ++i) type_hold_x[i] = -1.0f;   // all types back to auto
        tod = 0.0f; day_num = 1;   // back to dawn of day 1
        player.position[0] = (map->spawn_col + 0.5f) * dc::world::TILE;
        player.position[1] = dc::world::EYE_HEIGHT;
        player.position[2] = (map->spawn_row + 0.5f) * dc::world::TILE;
        player.vel_y = 0.0f;
        player.health = player.stats.max_health;
        player.stamina = player.stamina_max;
        player.knock_vel[0] = player.knock_vel[2] = 0.0f;
        player.dash_vel[0] = player.dash_vel[2] = 0.0f; player.iframes = 0.0f; player.dash_cd = 0.0f;
        player.hit_flash = 0.0f;
        player.burn_time = 0.0f; player.burn_dps = 0.0f;
        throwns.clear();
        bolts.clear(); render_bolts.clear();
        orbit.active = false;
        bash.active = false;
        if (menu_chest >= 0) { menu_chest = -1; window.set_relative_mouse(true); }
        // Clear all chest upgrades.
        player.stamina_mult = 0.5f;   // base half-cost (matches Player default)
        player.damage_mult = 1.0f;
        player.swing_reach_bonus = 0.0f;
        player.swing_cone_bonus = 0.0f;
        player.sword_scale = 1.0f;
        player.cooldown_mult = 1.0f;
        player.dash_speed = base_dash_speed;
        player.dash_iframes = base_dash_iframes;
        player.crit_chance = base_crit_chance;
        player.crit_mult = base_crit_mult;
        player.health_regen = base_health_regen;
        player.fire_dps = base_fire_dps; player.ice_slow = base_ice_slow; player.earth_knock = base_earth_knock;
        player.minion_count = 0; player.minion_damage = base_minion_damage; player.minion_range = base_minion_range;
        player.trail_damage = base_trail_damage; player.trail_life = base_trail_life;
        player.supersonic_damage = base_supersonic;
        player.stats.knockback = base_knockback;
        // XP / leveling + autocast unlocks back to a fresh run.
        player.level = 1; player.xp = 0.0f; player.xp_to_next = dc::entity::xp_for_level(1);
        player.spell_slots = 2;
        player.orbit_unlocked = false; player.forcefield_unlocked = false;
        player.orbit_cd_mult = 1.0f; player.forcefield_cd_mult = 1.0f; player.autocast_cd_mult = 1.0f;
        player.orbit_spin_mult = 1.0f; player.orbit_tick_mult = 1.0f;
        player.throw_count = 1;
        if (player.weapon) player.weapon->orbit_count = base_orbit_count;
        if (player.shield) player.shield->bash_radius = base_bash_radius;
        pending_levelups = 0; xp_orbs.clear(); levelup_open = false;
        for (int& n : inventory) n = 0;   // empty the item stacks
        currency = START_GOLD;
        host_damage = 0.0; my_damage = 0.0f;   // reset the damage leaderboard
        respawn_timer = -1.0f;
        // Revive + reset every connected client (clears wallet, refills health, sends
        // them back to spawn). The full-health bodies go out in the next snapshot, so
        // ghosts come back to life on their own screens.
        for (auto& hc : host_clients) {
            hc.currency = START_GOLD;
            hc.damage_dealt = 0.0;
            hc.body.health = hc.body.stats.max_health;
            hc.body.knock_vel[0] = hc.body.knock_vel[2] = 0.0f;
            hc.body.dash_vel[0] = hc.body.dash_vel[2] = 0.0f; hc.body.iframes = 0.0f;
            hc.body.hit_flash = 0.0f; hc.body.burn_time = 0.0f; hc.body.burn_dps = 0.0f;
            hc.body.position[0] = (map->spawn_col + 0.5f) * dc::world::TILE;
            hc.body.position[1] = dc::world::EYE_HEIGHT;
            hc.body.position[2] = (map->spawn_row + 0.5f) * dc::world::TILE;
        }
        run_time = 0.0f;
        seed_base();   // base doesn't carry across runs: back to the default turret ring
        shield_radius = base.build_radius;
        piece_hp.assign(base.pieces.size(), 1.0f);   // rebuild runtime HP for the fresh layout
        for (std::size_t i = 0; i < base.pieces.size(); ++i) piece_hp[i] = piece_full_hp(base.pieces[i].piece);
        for (auto& ch : chests) {   // re-close + restock every chest, drop any lock
            ch.opened = false; ch.open_t = 0.0f; ch.locked_by = NO_LOCK; ch.lock_time = 0.0f;
            for (bool& t : ch.taken) t = false;
        }
        menu_chest = -1;
        for (auto& dv : drone_vendors) dv.bought = false;   // restock drone vendors
        coins.clear();
        entities.items.clear();
        entities.projectiles.clear();
        if (net.role != dc::net::Role::Client)
            for (const auto& es : map->enemies)
                entities.spawn_enemy((es.col + 0.5f) * dc::world::TILE, (es.row + 0.5f) * dc::world::TILE);
        for (auto& sp : spawners) sp.accum = 0.0f;
    };

    // Pause-menu quit button (NDC), shared by hit-testing and drawing.
    const float QX0 = -0.15f, QX1 = 0.15f, QY0 = -0.09f, QY1 = 0.09f;

    // Chest purchase-menu card layout in NDC (4 fixed slots; taken ones render empty).
    const float CARD_W = 0.18f, CARD_GAP = 0.05f, CARD_TOP = 0.42f, CARD_BOT = -0.42f;
    auto card_x0 = [&](int i) {
        const float total = 4 * CARD_W + 3 * CARD_GAP;
        return -total * 0.5f + i * (CARD_W + CARD_GAP);
    };

    // The local player's appearance + class (declared here so level-up picks can filter by
    // class; loaded from disk below). id 0 = host.
    const char* APPEARANCE_PATH = "player_appearance.dat";
    dc::game::Appearance my_look;
    dc::game::load_appearance(my_look, APPEARANCE_PATH);   // ok if missing -> defaults
    // Local player buys an item: apply its upgrade + grow the stack.
    auto apply_pickup = [&](Upgrade u) { apply_upgrade(player, u); inventory[static_cast<int>(u)]++; };
    // Draw up to 4 distinct currently-eligible upgrades for a level-up pick. Builds the
    // eligible pool, then samples without replacement (Fisher-Yates on the pool prefix).
    auto open_levelup = [&]() {
        Upgrade pool[UPGRADE_COUNT];
        int np = 0;
        for (int i = 0; i < UPGRADE_COUNT; ++i) {
            const Upgrade u = static_cast<Upgrade>(i);
            if (upgrade_eligible(player, u) && upgrade_for_class(u, my_look.weapon_class)) pool[np++] = u;
        }
        levelup_card_count = np < 4 ? np : 4;
        for (int i = 0; i < levelup_card_count; ++i) {
            levelup_rng = levelup_rng * 1664525u + 1013904223u;
            const int j = i + static_cast<int>((levelup_rng >> 8) % static_cast<uint32_t>(np - i));
            Upgrade tmp = pool[i]; pool[i] = pool[j]; pool[j] = tmp;   // swap chosen into place
            levelup_cards[i] = pool[i];
        }
        levelup_open = levelup_card_count > 0;
        if (levelup_open) window.set_relative_mouse(false);
    };
    const float CHEST_REACH = 3.0f;            // how close you must be to open a chest
    const float CHEST_LOCK_TIME = 30.0f;       // host: auto-release a lock held this long (safety)
    // Gunner minions: loosely follow the owner, volley-fire the nearest enemy in range.
    const float MINION_FIRE_INTERVAL = 0.6f;   // drone range is a per-player stat now (player.minion_range)
    const float MINION_FOLLOW_RADIUS = 2.3f;   // loose formation radius around the owner (a bit further back)
    const float MINION_FOLLOW_K = 2.6f;        // follow stiffness (lower = laggier; eases toward the slot)
    float minion_fire_cd = 0.0f;               // host/standalone: local player's volley timer
    float insulter_taunt_cd = 1.5f;            // host: cadence for Insulter/Bill heckles
    // Per-owner drone positions, simulated per-peer for visuals (firing is host-side).
    struct DroneSwarm { uint32_t id; vec3 pos[4]; bool spawned[4]; };
    std::vector<DroneSwarm> swarms;
    // Trailblazer fire trails: persistent per-owner segment lists (every peer simulates
    // them for visuals; the host damages enemies standing in them).
    struct TrailSeg { float x, z, age; };
    struct Trail { uint32_t id; float dmg, life, last_x, last_z; bool init; std::vector<TrailSeg> segs; };
    std::vector<Trail> trails;
    const float TRAIL_DROP = 0.55f, TRAIL_RADIUS = 1.1f;

    // Supersonic dodge shockwave: host-resolved AoE blasts queued this frame + the local
    // spiral-gust visual timer.
    const float SUPERSONIC_RADIUS = 5.0f, SUPERSONIC_KNOCK = 70.0f, SS_ANIM_TIME = 0.45f;
    struct Blast { float x, z, dmg; uint32_t owner; };
    std::vector<Blast> supersonic_blasts;
    float ss_anim = 0.0f; vec3 ss_pos = {0.0f, 0.0f, 0.0f};
    float block_flash = 0.0f;   // >0 right after the block bubble absorbs a hit (tints it red)

    // Updraft launch + out-of-bounds safety net.
    const float UPDRAFT_RADIUS = 1.3f, UPDRAFT_LAUNCH = 28.0f;   // launches you high (reach plateaus)
    const float UPDRAFT_PUSH = 42.0f;                            // big horizontal gust in the entry direction (decays)
    const float FALL_MARGIN = 4.0f, FALL_Y = -25.0f;           // outside this xz box (or below FALL_Y) = fell out
    const float worldW = map->width * dc::world::TILE, worldH = map->height * dc::world::TILE;
    auto on_updraft = [&](float x, float z) {
        for (const auto& u : updrafts) { const float dx = x - u.x, dz = z - u.z;
            if (dx*dx + dz*dz <= UPDRAFT_RADIUS*UPDRAFT_RADIUS) return true; }
        return false;
    };
    // Launch off a pad while grounded on it: a strong upward kick plus a decaying gust in
    // (mvx,mvz) — the direction the body was moving on entry (0,0 = straight up).
    auto apply_updraft = [&](dc::entity::Player& b, float mvx, float mvz) {
        if (!(b.on_ground && on_updraft(b.position[0], b.position[2]))) return;
        b.vel_y = UPDRAFT_LAUNCH; b.on_ground = false;
        b.knock_vel[0] += mvx * UPDRAFT_PUSH; b.knock_vel[2] += mvz * UPDRAFT_PUSH;
    };
    auto apply_fallout = [&](dc::entity::Player& b) {   // out of the map -> return to spawn, lose 25% max hp
        const bool oob = b.position[0] < -FALL_MARGIN || b.position[0] > worldW + FALL_MARGIN
                      || b.position[2] < -FALL_MARGIN || b.position[2] > worldH + FALL_MARGIN
                      || b.position[1] < FALL_Y;
        if (!oob) return;
        b.position[0] = (map->spawn_col + 0.5f) * dc::world::TILE;
        b.position[2] = (map->spawn_row + 0.5f) * dc::world::TILE;
        b.position[1] = terrain.height(b.position[0], b.position[2]) + dc::world::EYE_HEIGHT;
        b.vel_y = 0.0f; b.on_ground = true;
        b.knock_vel[0] = b.knock_vel[2] = 0.0f; b.dash_vel[0] = b.dash_vel[2] = 0.0f;
        if (b.health > 0.0f) {   // penalty (don't touch ghosts)
            b.health -= b.stats.max_health * 0.25f;
            if (b.health < 0.0f) b.health = 0.0f;
            b.hit_flash = dc::entity::FLASH_TIME;
        }
    };

    // Grant XP to the LOCAL player and roll any level-ups (queued so a big absorb can
    // grant several picks). Used by both the host (its own pickups) and a client (from
    // the host's XpGranted events). Defined here so the event handler can reach it.
    auto add_xp = [&](float amount) {
        player.xp += amount;
        while (player.xp >= player.xp_to_next) {
            player.xp -= player.xp_to_next;
            player.level++;
            player.xp_to_next = dc::entity::xp_for_level(player.level);
            pending_levelups++;
        }
    };

    // Enemy taunts: floating yellow insults (+ TTS) over attacking enemies. Host picks
    // them (rate-limited + capped); a Taunt event replicates each to everyone.
    struct FloatTaunt { vec3 pos; float age; std::string text; };
    std::vector<FloatTaunt> taunts;
    float taunt_cd = 0.0f;
    float react_cd[16] = {0};   // per-player cooldown so reactive taunts don't spam on every tick
    constexpr float TAUNT_LIFE = 2.8f, TAUNT_INTERVAL = 1.6f, TAUNT_RANGE = 9.0f;
    constexpr int   MAX_TAUNTS = 3;     // at most this many on screen / talking at once
    auto spawn_taunt = [&](float x, float y, float z, const std::string& text, bool broadcast) {
        if (text.empty()) return;
        FloatTaunt t; t.pos[0] = x; t.pos[1] = y; t.pos[2] = z; t.age = 0.0f; t.text = text;
        taunts.push_back(t);
        speak_async(text);
        if (broadcast && net.role == dc::net::Role::Host) {
            dc::net::TauntState ts{}; ts.x = x; ts.y = y; ts.z = z;
            std::strncpy(ts.text, text.c_str(), sizeof ts.text - 1);
            unsigned char buf[1 + sizeof ts]; buf[0] = static_cast<unsigned char>(dc::net::MsgType::Taunt);
            std::memcpy(buf + 1, &ts, sizeof ts); net.broadcast(buf, sizeof buf, true);
        }
    };

    bool running = true;   // shared by the lobby and the main game loop

    // ---- Character appearance (skin, drawn face, silly bone scales) -------------------
    // Loaded from disk as the default; edited in the lobby; saved on Start. (my_look + its
    // load moved earlier so the level-up picker can filter upgrades by class.)
    // Everyone's look by player id (id 0 = host). Populated from Appearance messages;
    // my own id's entry mirrors my_look. Used to render each player's face/skin/bones.
    std::vector<std::pair<uint32_t, dc::game::Appearance>> looks;
    auto look_for = [&](uint32_t id) -> dc::game::Appearance& {
        for (auto& kv : looks) if (kv.first == id) return kv.second;
        looks.push_back({ id, dc::game::Appearance{} });
        return looks.back().second;
    };
    // Resolve an insult to speak. ~35% of the time it pulls one of a player's typed custom
    // lines (owner_id = that player, or 0xFFFFFFFF to sample everyone's); otherwise a canned
    // reactive/ambient line. Host-side; the resolved string is what gets replicated.
    auto pick_line = [&](bool reactive, uint32_t owner_id) -> std::string {
        std::vector<std::string> customs;
        auto add = [&](const dc::game::Appearance& a) { if (a.custom1[0]) customs.push_back(a.custom1); if (a.custom2[0]) customs.push_back(a.custom2); };
        if (owner_id == 0xFFFFFFFFu) { for (auto& kv : looks) add(kv.second); }
        else add(look_for(owner_id));
        spark_rng = spark_rng * 1664525u + 1013904223u;
        if (!customs.empty() && (spark_rng % 100u) < 35u) {
            spark_rng = spark_rng * 1664525u + 1013904223u;
            return customs[spark_rng % customs.size()];
        }
        // Otherwise GENERATE a fresh procedural insult (templates + word banks).
        return reactive ? dc::game::reactive_generate(spark_rng)
                        : dc::game::taunt_generate(spark_rng);
    };
    // A random nearby enemy fires a reactive insult back (host-authoritative; broadcast so every
    // peer sees + hears the same line). Used when a player hurls an insult of their own.
    auto enemy_insult_back = [&](float x, float z) {
        if (net.role == dc::net::Role::Client) return;
        std::vector<dc::entity::Entity*> nearby;
        for (auto& e : entities.items)
            if (e.alive && e.type == dc::entity::EntityType::Enemy) {
                const float dx = e.position[0]-x, dz = e.position[2]-z;
                if (dx*dx + dz*dz < 20.0f*20.0f) nearby.push_back(&e);
            }
        if (nearby.empty()) return;
        spark_rng = spark_rng * 1664525u + 1013904223u;
        dc::entity::Entity* e = nearby[(spark_rng >> 8) % nearby.size()];
        spawn_taunt(e->position[0], e->position[1] + 1.7f, e->position[2], pick_line(true, 0xFFFFFFFFu), true);
    };

    dc::renderer::Mesh face_mesh;   // rebuilt per color when drawing pixel faces
    // Append a player's pixel face (painted cells only) to per-color vertex lists, on a
    // plate centered at `center`, facing `fwd`, spanned by `right`/`up`, `cell` wide.
    auto append_face = [&](std::vector<float>* by_color, const vec3 center, const vec3 fwd,
                           const vec3 right, const vec3 up, float cell, const dc::game::Appearance& look) {
        for (int row = 0; row < dc::game::FACE_H; ++row)
            for (int col = 0; col < dc::game::FACE_W; ++col) {
                const int idx = look.face[row * dc::game::FACE_W + col];
                if (idx <= 0 || idx >= dc::game::PALETTE_N) continue;
                const float lx = (col - (dc::game::FACE_W - 1) * 0.5f) * cell;
                const float ly = ((dc::game::FACE_H - 1) * 0.5f - row) * cell;
                const float h = cell * 0.5f;
                auto P = [&](float ox, float oy, vec3 out) {
                    out[0] = center[0] + right[0]*(lx+ox) + up[0]*(ly+oy);
                    out[1] = center[1] + right[1]*(lx+ox) + up[1]*(ly+oy);
                    out[2] = center[2] + right[2]*(lx+ox) + up[2]*(ly+oy);
                };
                vec3 a,b,c,d; P(-h,-h,a); P(h,-h,b); P(h,h,c); P(-h,h,d);
                std::vector<float>& v = by_color[idx];
                auto V = [&](const vec3 p){ v.insert(v.end(), {p[0],p[1],p[2], fwd[0],fwd[1],fwd[2], 0.f,0.f,0.f}); };
                V(a); V(b); V(c); V(a); V(c); V(d);
            }
    };
    auto draw_faces = [&](std::vector<float>* by_color) {
        for (int c = 1; c < dc::game::PALETTE_N; ++c) {
            if (by_color[c].empty()) continue;
            face_mesh.upload(by_color[c]);
            float r,g,b; dc::game::palette_rgb(c, r, g, b);
            vec3 col = { r, g, b };
            renderer.draw_terrain(face_mesh, col, true);
            by_color[c].clear();
        }
    };
    // Per-node silly-bone scale vector for a look. Passed to pose_model, which applies it
    // AFTER animation sampling (so it survives the walk/punch clips) — and because the
    // hand bones are scaled in the same pose, the sword/shield/helmet attach correctly
    // (they hang off the scaled hand/head world matrices and scale with them).
    auto bone_scale_for = [&](const dc::game::Appearance& look) {
        std::vector<float> bs(model_data.nodes.size(), 1.0f);
        auto set = [&](int node, float m) { if (node >= 0 && node < static_cast<int>(bs.size())) bs[node] = m; };
        set(model_data.body_node, look.bone_torso);
        set(model_data.head_node, look.bone_head);
        set(model_data.arm_l_node, look.bone_arms); set(model_data.arm_r_node, look.bone_arms);
        set(model_data.hand_l_node, look.bone_hands); set(model_data.hand_r_node, look.bone_hands);
        return bs;
    };
    // Draw a player's pixel face on a plate in front of the head at world `head`, facing
    // along `yaw` (the look direction). Used for the local avatar and every remote.
    auto draw_face_at = [&](const vec3 head, float yaw, const dc::game::Appearance& look) {
        vec3 fwd = { std::cos(yaw), 0.0f, std::sin(yaw) };
        vec3 right = { fwd[2], 0.0f, -fwd[0] };
        vec3 up = { 0.0f, 1.0f, 0.0f };
        vec3 c = { head[0] + fwd[0] * 0.24f, head[1] + 0.08f, head[2] + fwd[2] * 0.24f };   // on the head front surface
        std::vector<float> fcol[dc::game::PALETTE_N];
        append_face(fcol, c, fwd, right, up, 0.018f * look.bone_head, look);
        draw_faces(fcol);
    };

    // ===================== Pre-game: lobby + character customizer =====================
    // Gate the game behind a screen where you draw your face, pick a skin tone, set silly
    // bone scales, and (host) wait for players + click START. Clients wait for the host.
    // Networking (host/connect) is already live from the CLI; this just delays the sim.
    {
        bool started = false;
        bool sent_look = false;                 // client: have we sent our look to the host yet
        float preview_yaw = 0.0f;               // drag to spin the character
        bool  drag_rot = false, paint_prev = false;
        int   active_box = -1;                  // which custom-insult box is focused (-1 = none)
        window.set_relative_mouse(false);       // free cursor for the UI
        window.set_text_input(true);            // enable typing for the custom-insult boxes
        uint64_t lprev = SDL_GetTicksNS();
        while (running && !started) {
            running = window.pump_events(input);
            const uint64_t now = SDL_GetTicksNS();
            const float dt = (now - lprev) / 1e9f; lprev = now;

            // --- Networking: accept clients (host), exchange looks, receive START. ---
            net_events.clear();
            net.poll(net_events);
            for (auto& ev : net_events) {
                if (ev.type == dc::net::Event::Connect && net.role == dc::net::Role::Host) {
                    HostClient hc; hc.id = next_player_id++; hc.peer = ev.peer;
                    hc.currency = START_GOLD;
                    hc.body.position[0] = (map->spawn_col + 0.5f) * dc::world::TILE;
                    hc.body.position[1] = dc::world::EYE_HEIGHT;
                    hc.body.position[2] = (map->spawn_row + 0.5f) * dc::world::TILE;
                    host_clients.push_back(hc);
                    unsigned char buf[5]; buf[0] = static_cast<unsigned char>(dc::net::MsgType::AssignId);
                    std::memcpy(buf + 1, &hc.id, 4); net.broadcast(buf, sizeof buf, true);
                    // Tell the newcomer everyone's look so far (host + already-joined clients).
                    auto send_look = [&](uint32_t id, const dc::game::Appearance& a) {
                        unsigned char b[1 + 4 + sizeof(dc::game::Appearance)];
                        b[0] = static_cast<unsigned char>(dc::net::MsgType::Appearance);
                        std::memcpy(b + 1, &id, 4); std::memcpy(b + 5, &a, sizeof a);
                        net.send_to_peer(ev.peer, b, sizeof b, true);
                    };
                    send_look(0, my_look);
                    for (auto& kv : looks) if (kv.first != 0) send_look(kv.first, kv.second);
                } else if (ev.type == dc::net::Event::Receive && !ev.data.empty()) {
                    const auto mt = static_cast<dc::net::MsgType>(ev.data[0]);
                    if (mt == dc::net::MsgType::AssignId && ev.data.size() >= 5) {
                        std::memcpy(&my_id, ev.data.data() + 1, 4);   // client learns its id
                    } else if (mt == dc::net::MsgType::Appearance && ev.data.size() >= 5 + sizeof(dc::game::Appearance)) {
                        uint32_t id; std::memcpy(&id, ev.data.data() + 1, 4);
                        dc::game::Appearance a; std::memcpy(&a, ev.data.data() + 5, sizeof a);
                        look_for(id) = a;
                        if (net.role == dc::net::Role::Host) {   // relay a client's look to everyone else
                            for (auto& hc : host_clients) if (hc.id != id) {
                                unsigned char b[1 + 4 + sizeof a];
                                b[0] = static_cast<unsigned char>(dc::net::MsgType::Appearance);
                                std::memcpy(b + 1, &id, 4); std::memcpy(b + 5, &a, sizeof a);
                                net.send_to_peer(hc.peer, b, sizeof b, true);
                            }
                        }
                    } else if (mt == dc::net::MsgType::StartGame) {
                        started = true;   // client: host kicked off the game
                    }
                }
            }
            // Client: push our look to the host once we have an id (and again on edits via a dirty flag would be nicer).
            if (net.role == dc::net::Role::Client && my_id != 0 && !sent_look) {
                unsigned char b[1 + 4 + sizeof(dc::game::Appearance)];
                b[0] = static_cast<unsigned char>(dc::net::MsgType::Appearance);
                std::memcpy(b + 1, &my_id, 4); std::memcpy(b + 5, &my_look, sizeof my_look);
                net.send_to_host(b, sizeof b, true); sent_look = true;
            }

            // --- Mouse + UI ---
            int ww, wh; window.window_size(ww, wh);
            float mx, my; input.mouse_pos(mx, my);
            const float nx = ww > 0 ? (mx / ww) * 2.0f - 1.0f : 0.0f;
            const float ny = wh > 0 ? 1.0f - (my / wh) * 2.0f : 0.0f;
            const bool lmb = input.mouse_down(SDL_BUTTON_LEFT);
            bool dirty = false;

            const bool click = lmb && !paint_prev;
            auto hit = [&](float x0, float y0, float x1, float y1) { return nx >= x0 && nx <= x1 && ny >= y0 && ny <= y1; };

            // Custom insult boxes (two): click to focus + type; a Play button speaks it.
            char* boxes[2] = { my_look.custom1, my_look.custom2 };
            const float BOX_X0 = -0.28f, BOX_X1 = 0.30f, PLAY_X0 = 0.33f, PLAY_X1 = 0.45f;
            const float box_y[2] = { -0.52f, -0.64f };
            const bool on_box = hit(BOX_X0, box_y[1], PLAY_X1, box_y[0] + 0.085f);

            // Drag (anywhere but the insult boxes) spins the character preview.
            if (lmb && !on_box && (drag_rot || !paint_prev)) { drag_rot = true; preview_yaw += input.mouse_dx * 0.01f; }
            if (!lmb) drag_rot = false;

            // Class selection: two big buttons — Knight (Sword) / Wizard (Staff).
            if (click && hit(-0.62f, 0.06f, -0.10f, 0.40f)) { my_look.weapon_class = 0; dirty = true; }   // Knight
            if (click && hit( 0.10f, 0.06f,  0.62f, 0.40f)) { my_look.weapon_class = 1; dirty = true; }   // Wizard
            for (int bi = 0; bi < 2; ++bi) {
                if (click && hit(BOX_X0, box_y[bi], BOX_X1, box_y[bi] + 0.085f)) active_box = bi;
                if (click && hit(PLAY_X0, box_y[bi], PLAY_X1, box_y[bi] + 0.085f) && boxes[bi][0]) speak_async(boxes[bi]);
            }
            // Clicking anywhere else (and not on a box) drops focus.
            if (click && !(hit(BOX_X0, box_y[1], PLAY_X1, box_y[0] + 0.085f))) active_box = -1;
            // Apply typed text / backspace to the focused box (shell-safe characters only).
            if (active_box >= 0) {
                char* box = boxes[active_box];
                int len = static_cast<int>(std::strlen(box));
                for (int k = 0; k < input.backspaces && len > 0; ++k) box[--len] = 0;
                for (char ch : input.text_input) {
                    if (len >= 46) break;
                    const bool safe = (ch >= 32 && ch < 127) && ch != '"' && ch != '\'' && ch != '`' && ch != '$' && ch != '\\';
                    if (safe) { box[len++] = ch; box[len] = 0; }
                }
                if (input.backspaces || !input.text_input.empty()) dirty = true;
            }

            // START / WAITING
            const bool can_start = (net.role != dc::net::Role::Client);
            if (click && can_start && hit(0.55f, -0.92f, 0.90f, -0.80f)) {
                dc::game::save_appearance(my_look, APPEARANCE_PATH);   // persist as default
                if (net.role == dc::net::Role::Host) {
                    unsigned char b[1]; b[0] = static_cast<unsigned char>(dc::net::MsgType::StartGame);
                    net.broadcast(b, sizeof b, true);
                }
                started = true;
            }
            paint_prev = lmb;
            // Keep my own look entry in sync for rendering + relays.
            look_for(my_id) = my_look;
            if (dirty && net.role == dc::net::Role::Client && my_id != 0) sent_look = false;  // resend edits

            // --- Render: 3D character preview + 2D UI ---
            int fbw, fbh; window.framebuffer_size(fbw, fbh);
            renderer.begin_frame(*map, camera, player, dt, fbw, fbh);
            renderer.set_ambient(2.6f);                                 // bright fill so the preview is lit
            { float lp[3] = {0,0,0}, lc[3] = {0,0,0}, lr[1] = {1}; renderer.set_lights(0, lp, lc, lr); }
            // Pose the chosen CLASS MODEL (gear baked in) at rest, in front of the camera.
            {
                dc::renderer::ModelData& pmd = class_md(my_look.weapon_class);
                std::vector<dc::renderer::AnimLayer> el;
                dc::renderer::pose_model(pmd, el, 0.0f, part_world);
                vec3 fwd; player.front(fwd); fwd[1] = 0.0f;
                float fl = std::sqrt(fwd[0]*fwd[0] + fwd[2]*fwd[2]); if (fl > 1e-4f) { fwd[0] /= fl; fwd[2] /= fl; }
                const float feet = player.position[1] - dc::world::EYE_HEIGHT;
                vec3 ppos = { player.position[0] + fwd[0]*2.6f, feet, player.position[2] + fwd[2]*2.6f };
                const float base_ang = std::atan2(fwd[2], fwd[0]);   // face the camera by default
                const float ang = base_ang + preview_yaw;
                mat4 place; glm_mat4_identity(place);
                glm_translate(place, ppos);
                glm_rotate_y(place, ang + glm_rad(-90.0f), place);   // MODEL_YAW_OFFSET
                vec3 white = { 1.0f, 1.0f, 1.0f };   // baked material colors carry the class look
                renderer.draw_model(class_mdl(my_look.weapon_class), part_world, place, white);
            }

            // 2D UI overlay: two big class buttons, custom-insult boxes, START.
            std::vector<float> hud;
            auto rect = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a){
                hud.insert(hud.end(), { x0,y0,0,r,g,b,a, x1,y0,0,r,g,b,a, x1,y1,0,r,g,b,a,
                                        x0,y0,0,r,g,b,a, x1,y1,0,r,g,b,a, x0,y1,0,r,g,b,a }); };
            { const bool kn = my_look.weapon_class == 0, wz = my_look.weapon_class == 1;
              rect(-0.66f, 0.02f, -0.06f, 0.44f, kn ? 0.18f : 0.10f, kn ? 0.18f : 0.10f, kn ? 0.22f : 0.13f, 0.95f);  // knight backing
              rect(-0.62f, 0.06f, -0.10f, 0.40f, kn ? 0.62f : 0.30f, kn ? 0.64f : 0.32f, kn ? 0.72f : 0.36f, 1.0f);   // knight
              rect( 0.06f, 0.02f,  0.66f, 0.44f, wz ? 0.10f : 0.10f, wz ? 0.10f : 0.10f, wz ? 0.22f : 0.13f, 0.95f);   // wizard backing
              rect( 0.10f, 0.06f,  0.62f, 0.40f, wz ? 0.32f : 0.22f, wz ? 0.30f : 0.22f, wz ? 0.85f : 0.36f, 1.0f); }  // wizard
            const bool can_start2 = (net.role != dc::net::Role::Client);
            rect(0.55f, -0.92f, 0.90f, -0.80f, can_start2 ? 0.2f : 0.3f, can_start2 ? 0.7f : 0.3f, 0.3f, 1.0f);  // START
            for (int bi = 0; bi < 2; ++bi) {   // custom insult fields + play
                const bool act = (active_box == bi);
                rect(BOX_X0 - 0.006f, box_y[bi] - 0.006f, BOX_X1 + 0.006f, box_y[bi] + 0.091f, act ? 0.95f : 0.6f, act ? 0.85f : 0.6f, act ? 0.3f : 0.65f, 1.0f);  // border
                rect(BOX_X0, box_y[bi], BOX_X1, box_y[bi] + 0.085f, 0.12f, 0.12f, 0.15f, 1.0f);   // field
                rect(PLAY_X0, box_y[bi], PLAY_X1, box_y[bi] + 0.085f, 0.2f, 0.55f, 0.85f, 1.0f);  // play
            }
            renderer.draw_hud(hud);

            // Text labels.
            vec3 white = {0.95f,0.95f,1.0f}, gold = {1.0f,0.85f,0.3f};
            renderer.draw_text("CHOOSE YOUR CLASS", -0.45f, 0.74f, 24.0f, gold, 1.0f, fbw, fbh);
            renderer.draw_text("KNIGHT", -0.50f, 0.30f, 26.0f, white, 1.0f, fbw, fbh);
            renderer.draw_text("sword + shield", -0.50f, 0.15f, 13.0f, white, 1.0f, fbw, fbh);
            renderer.draw_text("WIZARD", 0.22f, 0.30f, 26.0f, white, 1.0f, fbw, fbh);
            renderer.draw_text("staff bolts", 0.22f, 0.15f, 13.0f, white, 1.0f, fbw, fbh);
            renderer.draw_text("drag to spin", -0.12f, -0.30f, 13.0f, white, 1.0f, fbw, fbh);
            // Custom insult fields.
            renderer.draw_text("Custom insults (enemies say these)", -0.28f, -0.42f, 13.0f, gold, 1.0f, fbw, fbh);
            {
                char* boxes[2] = { my_look.custom1, my_look.custom2 };
                vec3 grey = { 0.5f, 0.5f, 0.55f };
                for (int bi = 0; bi < 2; ++bi) {
                    char shown[64]; std::snprintf(shown, sizeof shown, "%s%s", boxes[bi], (active_box == bi) ? "_" : "");
                    const char* disp = boxes[bi][0] ? shown : (active_box == bi ? "_" : "type here...");
                    const bool lit = boxes[bi][0] || active_box == bi;
                    renderer.draw_text(disp, BOX_X0 + 0.015f, box_y[bi] + 0.022f, 14.0f, lit ? white : grey, 1.0f, fbw, fbh);
                    renderer.draw_text("Play", PLAY_X0 + 0.005f, box_y[bi] + 0.022f, 11.0f, white, 1.0f, fbw, fbh);
                }
            }
            if (net.role == dc::net::Role::Client) {
                renderer.draw_text("Waiting for host to start...", 0.45f, -0.86f, 16.0f, gold, 1.0f, fbw, fbh);
            } else {
                renderer.draw_text("START", 0.60f, -0.875f, 20.0f, white, 1.0f, fbw, fbh);
                char ct[48]; std::snprintf(ct, sizeof ct, "Players: %d", 1 + static_cast<int>(host_clients.size()));
                renderer.draw_text(ct, 0.55f, -0.75f, 15.0f, white, 1.0f, fbw, fbh);
            }
            window.swap();
        }
        if (!running) { net.shutdown(); return 0; }
        window.set_text_input(false);      // done typing custom insults
        window.set_relative_mouse(true);   // back to mouselook for the game
        look_for(my_id) = my_look;         // keep peer looks gathered in the lobby; refresh mine
    }

    // Class identity drives a play-style split: the KNIGHT tanks (more HP, cheap efficient
    // blocking), the WIZARD evades (less HP, costly blocking, but a longer, farther dodge
    // with more i-frames). Applied once here and re-applied on each respawn.
    const bool is_wizard_class = (my_look.weapon_class == 1);
    auto apply_class = [&]() {
        if (is_wizard_class) {
            player.stats.max_health = 65.0f;                 // glass cannon
            player.dash_speed += 12.0f;                      // dodge goes farther
            player.dash_iframes += 0.28f;                    // invincible noticeably longer
            player.dash_cooldown = std::max(0.4f, player.dash_cooldown - 0.15f);
            if (player.shield) { player.shield->block_rate *= 2.4f; player.shield->stamina_per_sec *= 1.9f; }  // blocking is expensive
        } else {
            player.stats.max_health = 135.0f;                // tanky
            if (player.shield) player.shield->block_rate *= 0.65f;   // efficient blocking
        }
        player.health = player.stats.max_health;
    };
    apply_class();

    uint64_t prev = SDL_GetTicksNS();
    while (running) {
        running = window.pump_events(input);

        // Service the network: handle connects, client inputs, and snapshots.
        net_events.clear();
        net.poll(net_events);
        for (auto& ev : net_events) {
            if (ev.type == dc::net::Event::Connect) {
                if (net.role == dc::net::Role::Host) {
                    HostClient hc; hc.id = next_player_id++; hc.peer = ev.peer;
                    hc.currency = START_GOLD;   // dev: clients also start with gold
                    hc.body.position[0] = (map->spawn_col + 0.5f) * dc::world::TILE;
                    hc.body.position[1] = dc::world::EYE_HEIGHT;
                    hc.body.position[2] = (map->spawn_row + 0.5f) * dc::world::TILE;
                    host_clients.push_back(hc);
                    unsigned char buf[5];
                    buf[0] = static_cast<unsigned char>(dc::net::MsgType::AssignId);
                    std::memcpy(buf + 1, &hc.id, 4);
                    net.broadcast(buf, sizeof buf, true);   // (2-player: only one client to hear it)
                    // Send the newcomer everyone's appearance (host + already-known peers).
                    for (auto& kv : looks) {
                        unsigned char b[1 + 4 + sizeof(dc::game::Appearance)];
                        b[0] = static_cast<unsigned char>(dc::net::MsgType::Appearance);
                        std::memcpy(b + 1, &kv.first, 4); std::memcpy(b + 5, &kv.second, sizeof kv.second);
                        net.send_to_peer(ev.peer, b, sizeof b, true);
                    }
                    std::printf("[net] client connected -> id %u\n", hc.id);
                }
            } else if (ev.type == dc::net::Event::Disconnect) {
                for (std::size_t i = 0; i < host_clients.size(); ++i)
                    if (host_clients[i].peer == ev.peer) {
                        for (auto& ch : chests) if (ch.locked_by == host_clients[i].id) ch.locked_by = NO_LOCK;  // free any held lock
                        host_clients[i] = host_clients.back(); host_clients.pop_back(); break;
                    }
            } else if (ev.type == dc::net::Event::Receive && !ev.data.empty()) {
                const auto mt = static_cast<dc::net::MsgType>(ev.data[0]);
                if (net.role == dc::net::Role::Host && mt == dc::net::MsgType::Input
                    && ev.data.size() >= 1 + sizeof(dc::net::InputCmd)) {
                    dc::net::InputCmd in; std::memcpy(&in, ev.data.data() + 1, sizeof in);
                    for (auto& hc : host_clients) if (hc.peer == ev.peer) { hc.input = in; break; }
                } else if (net.role == dc::net::Role::Host && mt == dc::net::MsgType::BashCast
                           && ev.data.size() >= 1 + sizeof(dc::net::BashCast)) {
                    // A client cast the bash. Start a host-run nova for it (advanced +
                    // damaged on the host's clock in the per-client specials block).
                    dc::net::BashCast bc; std::memcpy(&bc, ev.data.data() + 1, sizeof bc);
                    for (auto& hc : host_clients) if (hc.peer == ev.peer) {
                        hc.bash_active = true; hc.bash_time = 0.0f; hc.bash_radius = 0.0f; hc.bash_hits.clear();
                        hc.bash_max_radius = bc.radius; hc.bash_damage = bc.damage;
                        hc.bash_knockback = bc.knockback; hc.bash_duration = bc.duration;
                        break;
                    }
                } else if (net.role == dc::net::Role::Host && mt == dc::net::MsgType::OrbitCast
                           && ev.data.size() >= 1 + sizeof(dc::net::OrbitCast)) {
                    dc::net::OrbitCast oc; std::memcpy(&oc, ev.data.data() + 1, sizeof oc);
                    for (auto& hc : host_clients) if (hc.peer == ev.peer) {
                        hc.orbit_active = true; hc.orbit_time = 0.0f; hc.orbit_angle = 0.0f; hc.orbit_spin = 0.0f;
                        hc.orbit_tick_cd = 0.0f; hc.orbit_hits.clear();
                        hc.orbit_duration = oc.duration; hc.orbit_radius = oc.radius;
                        hc.orbit_hit_radius = oc.hit_radius; hc.orbit_damage = oc.damage;
                        hc.orbit_knockback = oc.knockback; hc.orbit_count = oc.count;
                        hc.orbit_tick = oc.tick > 0.0f ? oc.tick : 0.25f; hc.orbit_spin_mult = oc.spin > 0.0f ? oc.spin : 1.0f;
                        break;
                    }
                } else if (net.role == dc::net::Role::Host && mt == dc::net::MsgType::ThrownCast
                           && ev.data.size() >= 1 + sizeof(dc::net::ThrownCast)) {
                    dc::net::ThrownCast tc; std::memcpy(&tc, ev.data.data() + 1, sizeof tc);
                    for (auto& hc : host_clients) if (hc.peer == ev.peer) {
                        HostClient::HcThrown t;   // append a sword (a Swordstorm volley sends several casts)
                        t.pos[0] = tc.ox; t.pos[1] = tc.oy; t.pos[2] = tc.oz;
                        t.dir[0] = tc.dx; t.dir[1] = tc.dy; t.dir[2] = tc.dz;
                        t.speed = tc.speed; t.distance = tc.distance; t.radius = tc.radius;
                        t.damage = tc.damage; t.knockback = tc.knockback; t.size = tc.size;
                        hc.throwns.push_back(std::move(t));
                        break;
                    }
                } else if (net.role == dc::net::Role::Host && mt == dc::net::MsgType::BoltCast
                           && ev.data.size() >= 1 + sizeof(dc::net::BoltCast)) {
                    dc::net::BoltCast bc; std::memcpy(&bc, ev.data.data() + 1, sizeof bc);
                    uint32_t oid = 0; for (auto& hc : host_clients) if (hc.peer == ev.peer) { oid = hc.id; break; }
                    Bolt b; b.pos[0]=bc.ox; b.pos[1]=bc.oy; b.pos[2]=bc.oz; b.dir[0]=bc.dx; b.dir[1]=bc.dy; b.dir[2]=bc.dz;
                    b.radius=bc.radius; b.damage=bc.damage; b.knockback=bc.knockback; b.big=bc.big!=0; b.owner=oid;
                    bolts.push_back(std::move(b));   // host sims + damages it like its own bolts
                } else if (net.role == dc::net::Role::Host && mt == dc::net::MsgType::DashCast
                           && ev.data.size() >= 1 + sizeof(dc::net::DashCast)) {
                    dc::net::DashCast dc; std::memcpy(&dc, ev.data.data() + 1, sizeof dc);
                    for (auto& hc : host_clients) if (hc.peer == ev.peer) {
                        hc.body.dash_vel[0] = dc.dx * dc.speed; hc.body.dash_vel[2] = dc.dz * dc.speed;
                        hc.body.iframes = dc.iframes; hc.body.dash_decay = dc.decay;
                        if (dc.supersonic > 0.0f)   // queue the dodge shockwave at the client's body
                            supersonic_blasts.push_back({ hc.body.position[0], hc.body.position[2], dc.supersonic, hc.id });
                        break;
                    }
                } else if (net.role == dc::net::Role::Host && mt == dc::net::MsgType::OpenChest
                           && ev.data.size() >= 5) {
                    // A client wants to open a chest's menu. Grant only if it has items
                    // left and nobody else holds the lock (one player at a time). The host
                    // processes events serially, so two requests can't both lock it.
                    uint32_t idx; std::memcpy(&idx, ev.data.data() + 1, 4);
                    HostClient* hc = nullptr;
                    for (auto& c : host_clients) if (c.peer == ev.peer) { hc = &c; break; }
                    if (hc && idx < chests.size() && chests[idx].remaining() > 0
                        && chests[idx].locked_by == NO_LOCK) {
                        chests[idx].opened = true;                 // lid open (stays open)
                        chests[idx].locked_by = hc->id;            // lock to this client
                        chests[idx].lock_time = CHEST_LOCK_TIME;
                        unsigned char buf[1 + 4];
                        buf[0] = static_cast<unsigned char>(dc::net::MsgType::ChestGranted);
                        std::memcpy(buf + 1, &idx, 4);
                        net.send_to_peer(ev.peer, buf, sizeof buf, true);   // -> client opens its menu
                    }
                } else if (net.role == dc::net::Role::Host && mt == dc::net::MsgType::BuyItem
                           && ev.data.size() >= 9) {
                    // A client buys a slot. Valid only if it holds the lock, the slot is
                    // untaken, and it can afford it. Grant -> client applies the upgrade.
                    uint32_t idx, slot; std::memcpy(&idx, ev.data.data() + 1, 4); std::memcpy(&slot, ev.data.data() + 5, 4);
                    HostClient* hc = nullptr;
                    for (auto& c : host_clients) if (c.peer == ev.peer) { hc = &c; break; }
                    if (hc && idx < chests.size() && slot < 4 && chests[idx].locked_by == hc->id
                        && !chests[idx].taken[slot] && currency >= chests[idx].cost) {   // shared team gold
                        currency -= chests[idx].cost;
                        chests[idx].taken[slot] = true;            // replicated in the snapshot
                        chests[idx].locked_by = NO_LOCK;           // buying closes the menu -> unlock
                        unsigned char gbuf[1 + 1];
                        gbuf[0] = static_cast<unsigned char>(dc::net::MsgType::ItemGranted);
                        gbuf[1] = static_cast<unsigned char>(chests[idx].contents[slot]);
                        net.send_to_peer(ev.peer, gbuf, sizeof gbuf, true);
                    }
                } else if (net.role == dc::net::Role::Host && mt == dc::net::MsgType::ReleaseChest
                           && ev.data.size() >= 5) {
                    // Client closed the menu without buying -> drop its lock.
                    uint32_t idx; std::memcpy(&idx, ev.data.data() + 1, 4);
                    HostClient* hc = nullptr;
                    for (auto& c : host_clients) if (c.peer == ev.peer) { hc = &c; break; }
                    if (hc && idx < chests.size() && chests[idx].locked_by == hc->id) chests[idx].locked_by = NO_LOCK;
                } else if (net.role == dc::net::Role::Host && mt == dc::net::MsgType::BuyDrone
                           && ev.data.size() >= 5) {
                    // Client buys a ground drone: validate (unbought, near, can afford, room
                    // for another minion) -> mark bought, deduct, grant the minion.
                    uint32_t idx; std::memcpy(&idx, ev.data.data() + 1, 4);
                    HostClient* hc = nullptr;
                    for (auto& c : host_clients) if (c.peer == ev.peer) { hc = &c; break; }
                    if (hc && idx < drone_vendors.size() && !drone_vendors[idx].bought
                        && currency >= DRONE_COST && hc->input.minion_count < 4) {   // shared team gold
                        const float dx = drone_vendors[idx].x - hc->body.position[0];
                        const float dz = drone_vendors[idx].z - hc->body.position[2];
                        if (dx*dx + dz*dz <= (CHEST_REACH + 1.5f) * (CHEST_REACH + 1.5f)) {
                            currency -= DRONE_COST; drone_vendors[idx].bought = true;
                            unsigned char gbuf[1]; gbuf[0] = static_cast<unsigned char>(dc::net::MsgType::DroneGranted);
                            net.send_to_peer(ev.peer, gbuf, sizeof gbuf, true);
                        }
                    }
                } else if (net.role == dc::net::Role::Host && mt == dc::net::MsgType::BuildPlace
                           && ev.data.size() >= 1 + sizeof(dc::net::BuildEdit)) {
                    dc::net::BuildEdit be; std::memcpy(&be, ev.data.data() + 1, sizeof be);
                    HostClient* hc = nullptr; for (auto& c : host_clients) if (c.peer == ev.peer) { hc = &c; break; }
                    if (hc) host_place(be.col, be.row, be.piece, be.rot, currency);   // shared team gold
                } else if (net.role == dc::net::Role::Host && mt == dc::net::MsgType::BuildRemove
                           && ev.data.size() >= 1 + sizeof(dc::net::BuildEdit)) {
                    dc::net::BuildEdit be; std::memcpy(&be, ev.data.data() + 1, sizeof be);
                    HostClient* hc = nullptr; for (auto& c : host_clients) if (c.peer == ev.peer) { hc = &c; break; }
                    if (hc) host_remove(be.col, be.row, currency);
                } else if (net.role == dc::net::Role::Host && mt == dc::net::MsgType::BuyBaseArea) {
                    HostClient* hc = nullptr; for (auto& c : host_clients) if (c.peer == ev.peer) { hc = &c; break; }
                    if (hc) host_buy_area(currency);
                } else if (net.role == dc::net::Role::Host && mt == dc::net::MsgType::BuyUnlock
                           && ev.data.size() >= 2) {
                    HostClient* hc = nullptr; for (auto& c : host_clients) if (c.peer == ev.peer) { hc = &c; break; }
                    if (hc) host_unlock(static_cast<int>(ev.data[1]), currency);
                } else if (net.role == dc::net::Role::Host && mt == dc::net::MsgType::BuyBarracksUp
                           && ev.data.size() >= 4) {
                    HostClient* hc = nullptr; for (auto& c : host_clients) if (c.peer == ev.peer) { hc = &c; break; }
                    if (hc) host_upgrade_barracks(static_cast<int>(ev.data[2]), static_cast<int>(ev.data[3]), static_cast<int>(ev.data[1]), currency);
                } else if (net.role == dc::net::Role::Host && mt == dc::net::MsgType::RallyCmd
                           && ev.data.size() >= 10) {
                    rally_active = ev.data[1] != 0;
                    if (rally_active) { std::memcpy(&rally_pos[0], ev.data.data() + 2, 4); std::memcpy(&rally_pos[2], ev.data.data() + 6, 4); }
                } else if (net.role == dc::net::Role::Host && mt == dc::net::MsgType::HoldCmd
                           && ev.data.size() >= 7) {
                    const int ty = ev.data[1];
                    if (ty >= 0 && ty < dc::game::MOB_TYPE_COUNT) {
                        if (ev.data[2]) std::memcpy(&type_hold_x[ty], ev.data.data() + 3, 4);
                        else type_hold_x[ty] = -1.0f;
                    }
                } else if (mt == dc::net::MsgType::Appearance && ev.data.size() >= 5 + sizeof(dc::game::Appearance)) {
                    uint32_t id; std::memcpy(&id, ev.data.data() + 1, 4);
                    dc::game::Appearance a; std::memcpy(&a, ev.data.data() + 5, sizeof a);
                    look_for(id) = a;
                    if (net.role == dc::net::Role::Host)   // relay a client's look to the others
                        for (auto& hc : host_clients) if (hc.id != id) {
                            unsigned char b[1 + 4 + sizeof a];
                            b[0] = static_cast<unsigned char>(dc::net::MsgType::Appearance);
                            std::memcpy(b + 1, &id, 4); std::memcpy(b + 5, &a, sizeof a);
                            net.send_to_peer(hc.peer, b, sizeof b, true);
                        }
                } else if (net.role == dc::net::Role::Client && mt == dc::net::MsgType::Taunt
                           && ev.data.size() >= 1 + sizeof(dc::net::TauntState)) {
                    dc::net::TauntState ts; std::memcpy(&ts, ev.data.data() + 1, sizeof ts);
                    ts.text[sizeof ts.text - 1] = 0;
                    spawn_taunt(ts.x, ts.y, ts.z, std::string(ts.text), false);   // float + speak locally
                } else if (net.role == dc::net::Role::Host && mt == dc::net::MsgType::Taunt
                           && ev.data.size() >= 1 + sizeof(dc::net::TauntState)) {
                    // A client hurled an insult: show it here + relay to every OTHER client (the
                    // sender already shows its own), so all screens match.
                    dc::net::TauntState ts; std::memcpy(&ts, ev.data.data() + 1, sizeof ts);
                    ts.text[sizeof ts.text - 1] = 0;
                    spawn_taunt(ts.x, ts.y, ts.z, std::string(ts.text), false);
                    for (auto& hc : host_clients) if (hc.peer != ev.peer) {
                        unsigned char buf[1 + sizeof ts]; buf[0] = static_cast<unsigned char>(dc::net::MsgType::Taunt);
                        std::memcpy(buf + 1, &ts, sizeof ts); net.send_to_peer(hc.peer, buf, sizeof buf, true);
                    }
                    enemy_insult_back(ts.x, ts.z);   // a nearby enemy claps back at the heckling client
                } else if (net.role == dc::net::Role::Client && mt == dc::net::MsgType::XpGranted
                           && ev.data.size() >= 1 + sizeof(float)) {
                    float amount; std::memcpy(&amount, ev.data.data() + 1, sizeof amount);
                    add_xp(amount);   // gain XP + level up locally (with our own upgrade choices)
                } else if (net.role == dc::net::Role::Client && mt == dc::net::MsgType::DroneGranted) {
                    if (player.minion_count < 4) player.minion_count++;   // got the drone
                } else if (net.role == dc::net::Role::Client && mt == dc::net::MsgType::ItemGranted
                           && ev.data.size() >= 2) {
                    // Host approved our purchase: apply the upgrade + close the menu.
                    uint8_t ut = ev.data[1];
                    if (ut < UPGRADE_COUNT) apply_pickup(static_cast<Upgrade>(ut));
                    if (menu_chest >= 0) { menu_chest = -1; window.set_relative_mouse(true); }
                } else if (net.role == dc::net::Role::Client && mt == dc::net::MsgType::AssignId && ev.data.size() >= 5) {
                    std::memcpy(&my_id, ev.data.data() + 1, 4);
                } else if (net.role == dc::net::Role::Client && mt == dc::net::MsgType::ChestGranted
                           && ev.data.size() >= 5) {
                    // Host gave us the lock: open the purchase menu for that chest.
                    uint32_t idx; std::memcpy(&idx, ev.data.data() + 1, 4);
                    if (idx < chests.size()) { menu_chest = static_cast<int>(idx); window.set_relative_mouse(false); }
                } else if (net.role == dc::net::Role::Client && mt == dc::net::MsgType::Snapshot && ev.data.size() >= 5) {
                    const unsigned char* p = ev.data.data() + 1;
                    auto read_u32 = [&]() { uint32_t v; std::memcpy(&v, p, 4); p += 4; return v; };
                    // Players: our own pos/health from the host; everyone else -> remotes.
                    uint32_t np = read_u32();
                    remotes.clear();
                    for (uint32_t k = 0; k < np; ++k) {
                        dc::net::PlayerState s; std::memcpy(&s, p, sizeof s); p += sizeof s;
                        if (s.id == my_id) {
                            player.position[0] = s.x; player.position[1] = s.y; player.position[2] = s.z;
                            player.health = s.health;
                            currency = s.currency;   // our own wallet (host-authoritative)
                            my_damage = s.damage_dealt;   // our own total (host-authoritative)
                            player.stamina -= s.block_spent;   // stamina the host spent resolving our blocks
                            if (s.block_spent > 0.0f) block_flash = 0.3f;   // bubble absorbed a hit -> flash red
                            if (player.stamina < 0.0f) player.stamina = 0.0f;
                            if (s.hit_flash > player.hit_flash) player.hit_flash = s.hit_flash;  // host says we got hit
                            player.burn_time = s.burning ? 0.3f : 0.0f;   // host owns the DoT; this just gates our fire visual
                        } else {
                            Remote r{}; r.id = s.id;
                            r.pos[0] = s.x; r.pos[1] = s.y; r.pos[2] = s.z;
                            r.yaw = s.yaw; r.pitch = s.pitch; r.anim_time = s.anim_time; r.moving = s.moving != 0;
                            r.damage_dealt = s.damage_dealt;
                            r.elements = s.elements;
                            r.minions = s.minions;
                            r.minion_range = s.minion_range;
                            r.trail_life = s.trail_life;
                            r.ghost = s.health <= 0.0f;
                            r.punching = s.punching != 0; r.blocking = s.blocking != 0;
                            r.punch_time = s.punch_time; r.block_time = s.block_time;
                            r.hit_flash = s.hit_flash; r.sword_scale = s.sword_scale;
                            r.burning = s.burning != 0;
                            // thrown swords are filled from the owner-keyed ThrownState list below
                            r.bash_active = s.bash_active != 0; r.bash_radius = s.bash_radius;
                            r.orbit_active = s.orbit_active != 0; r.orbit_count = s.orbit_count;
                            r.orbit_angle = s.orbit_angle; r.orbit_spin = s.orbit_spin; r.orbit_radius = s.orbit_radius;
                            remotes.push_back(r);
                        }
                    }
                    // Enemies + coins: render-only mirrors of the host's state.
                    uint32_t ne = read_u32();
                    entities.items.clear();
                    for (uint32_t k = 0; k < ne; ++k) {
                        dc::net::EnemyState e; std::memcpy(&e, p, sizeof e); p += sizeof e;
                        dc::entity::Entity en;
                        en.type = dc::entity::EntityType::Enemy; en.alive = true;
                        en.position[0] = e.x; en.position[1] = 0.0f; en.position[2] = e.z; en.yaw = e.yaw;
                        en.anim_time = e.anim_time; en.attacking = e.attacking != 0;
                        en.attack_time = e.attack_time; en.hit_flash = e.hit_flash;
                        en.punch_anim = e.punch_anim;
                        en.health = e.health01; en.stats.max_health = 1.0f;   // frac for the over-head bar
                        en.healthbar_time = e.healthbar_time;
                        en.kind = static_cast<dc::entity::EnemyKind>(e.kind);
                        en.burn_time = (e.status & 1) ? 1.0f : 0.0f;   // sentinels so the render emits fire/ice
                        en.slow_time = (e.status & 2) ? 1.0f : 0.0f;
                        en.elite     = (e.status & 4) != 0;            // golden elite (bigger + aura)
                        entities.items.push_back(en);
                    }
                    uint32_t nc = read_u32();
                    coins.clear();
                    for (uint32_t k = 0; k < nc; ++k) {
                        dc::net::CoinState c; std::memcpy(&c, p, sizeof c); p += sizeof c;
                        Coin co; co.pos[0] = c.x; co.pos[1] = 0.0f; co.pos[2] = c.z; coins.push_back(co);
                    }
                    // XP orbs: render-only mirror (host owns pickups; we just draw them).
                    uint32_t nxo = read_u32();
                    xp_orbs.clear();
                    for (uint32_t k = 0; k < nxo; ++k) {
                        dc::net::XPOrbState xs; std::memcpy(&xs, p, sizeof xs); p += sizeof xs;
                        XPOrb o; o.pos[0] = xs.x; o.pos[1] = 0.0f; o.pos[2] = xs.z;
                        o.bob = (k * 1.2566f);   // vary the shimmer phase per orb
                        xp_orbs.push_back(o);
                    }
                    // Thrown swords (owner-keyed): distribute to each remote; skip our own id
                    // (we predict our own swords locally).
                    for (auto& r : remotes) r.throwns.clear();
                    uint32_t nth = read_u32();
                    for (uint32_t k = 0; k < nth; ++k) {
                        dc::net::ThrownState ts; std::memcpy(&ts, p, sizeof ts); p += sizeof ts;
                        if (ts.owner == my_id) continue;
                        for (auto& r : remotes) if (r.id == ts.owner) {
                            r.throwns.push_back({ ts.x, ts.y, ts.z, ts.spin, ts.size });
                            break;
                        }
                    }
                    // Chest open-state (same map order as ours): mirror the host's opens.
                    uint32_t nh = read_u32();
                    for (uint32_t k = 0; k < nh; ++k) {
                        unsigned char o = *p++;
                        unsigned char tk = *p++;
                        if (k < chests.size()) {
                            chests[k].opened = o != 0;
                            for (int j = 0; j < 4; ++j) chests[k].taken[j] = (tk & (1u << j)) != 0;
                        }
                    }
                    // Drone vendors: mirror the bought flags.
                    uint32_t nv = read_u32();
                    for (uint32_t k = 0; k < nv; ++k) { unsigned char b = *p++; if (k < drone_vendors.size()) drone_vendors[k].bought = b != 0; }
                    // Projectiles: render-only mirror of the host's flying shots.
                    uint32_t npr = read_u32();
                    entities.projectiles.clear();
                    for (uint32_t k = 0; k < npr; ++k) {
                        dc::net::ProjectileState ps; std::memcpy(&ps, p, sizeof ps); p += sizeof ps;
                        dc::entity::Projectile pr;
                        pr.pos[0] = ps.x; pr.pos[1] = ps.y; pr.pos[2] = ps.z;
                        pr.color[0] = ps.r; pr.color[1] = ps.g; pr.color[2] = ps.b;
                        pr.vel[0] = ps.vx; pr.vel[1] = ps.vy; pr.vel[2] = ps.vz; pr.radius = ps.radius; pr.beam = ps.beam != 0;
                        entities.projectiles.push_back(pr);
                    }
                    // Damage numbers spawned by the host this tick: float them locally.
                    uint32_t nd = read_u32();
                    for (uint32_t k = 0; k < nd; ++k) {
                        dc::net::DamageNumState ds; std::memcpy(&ds, p, sizeof ds); p += sizeof ds;
                        dmg_numbers.push_back({ {ds.x, ds.y, ds.z}, ds.amount, 0.0f, ds.crit != 0 });
                    }
                    // Enemy deaths this tick: play the same sand-crumble locally.
                    uint32_t ndeath = read_u32();
                    for (uint32_t k = 0; k < ndeath; ++k) {
                        float dxyz[3]; std::memcpy(dxyz, p, sizeof dxyz); p += sizeof dxyz;
                        burst_sand(dxyz[0], dxyz[1], dxyz[2]);
                    }
                    uint32_t nboom = read_u32();              // demon explosions this tick
                    for (uint32_t k = 0; k < nboom; ++k) {
                        float bxyz[3]; std::memcpy(bxyz, p, sizeof bxyz); p += sizeof bxyz;
                        burst_fire(bxyz[0], bxyz[1], bxyz[2]);
                    }
                    uint32_t nbolt = read_u32();              // wizard staff bolts (render-only)
                    render_bolts.clear();
                    for (uint32_t k = 0; k < nbolt; ++k) {
                        dc::net::BoltState bs; std::memcpy(&bs, p, sizeof bs); p += sizeof bs;
                        render_bolts.push_back({ { bs.x, bs.y, bs.z }, bs.big != 0 });
                    }
                    std::memcpy(&tod, p, 4); p += 4;          // shared day/night clock
                    { uint32_t dn; std::memcpy(&dn, p, 4); p += 4; day_num = static_cast<int>(dn); }
                    std::memcpy(&core_health, p, 4); p += 4;  // base health for the bar
                    std::memcpy(&shield_health, p, 4); p += 4;
                    if (shield_health < shield_prev - 0.5f) shield_flash = 0.3f;   // dropped -> flash red
                    shield_prev = shield_health;
                    std::memcpy(&enemy_core_health, p, 4); p += 4;   // enemy base health (objective)
                    // Player base: buildable radius (mirrors the dome) + placed pieces.
                    std::memcpy(&shield_radius, p, 4); p += 4;
                    uint32_t nbp; std::memcpy(&nbp, p, 4); p += 4;
                    if (nbp < 100000u) {
                        net_pieces.resize(nbp); net_piece_hp.resize(nbp, 0.0f);
                        if (nbp) {
                            std::memcpy(net_pieces.data(), p, nbp * sizeof(dc::game::BasePiece));
                            p += nbp * sizeof(dc::game::BasePiece);
                            std::memcpy(net_piece_hp.data(), p, nbp * sizeof(float));
                            p += nbp * sizeof(float);
                        }
                    }
                    uint32_t na; std::memcpy(&na, p, 4); p += 4;   // friendly lane mobs (render-only)
                    if (na < 100000u) {
                        net_allies.resize(na);
                        if (na) { std::memcpy(net_allies.data(), p, na * sizeof(dc::net::AllyState));
                                  p += na * sizeof(dc::net::AllyState); }
                    }
                    std::memcpy(&barracks_unlocked, p, 4); p += 4;   // unlocked mob types (for our menu)
                    { uint8_t ra; std::memcpy(&ra, p, 1); p += 1; rally_active = ra != 0;
                      std::memcpy(&rally_pos[0], p, 4); p += 4; std::memcpy(&rally_pos[2], p, 4); p += 4; }
                    std::memcpy(type_hold_x, p, dc::game::MOB_TYPE_COUNT * 4); p += dc::game::MOB_TYPE_COUNT * 4;
                    { uint32_t nb; std::memcpy(&nb, p, 4); p += 4;   // naval units (render-only)
                      if (nb < 100000u) { net_boats.resize(nb);
                          if (nb) { std::memcpy(net_boats.data(), p, nb * sizeof(dc::net::BoatState)); p += nb * sizeof(dc::net::BoatState); } } }
                    { uint32_t nsp; std::memcpy(&nsp, p, 4); p += 4;   // slime puddles (render + slow)
                      if (nsp < 100000u) { net_slime_patches.resize(nsp);
                          if (nsp) { std::memcpy(net_slime_patches.data(), p, nsp * sizeof(dc::net::SlimePatchState)); p += nsp * sizeof(dc::net::SlimePatchState); } } }
                    { uint32_t nm; std::memcpy(&nm, p, 4); p += 4;     // enemy sea-mines
                      if (nm < 100000u) { net_mines.resize(nm);
                          if (nm) { std::memcpy(net_mines.data(), p, nm * sizeof(dc::net::MineState)); p += nm * sizeof(dc::net::MineState); } } }
                }
            }
        }

        uint64_t now = SDL_GetTicksNS();
        float dt = static_cast<float>(now - prev) / 1.0e9f;
        prev = now;
        if (dt > 0.05f) dt = 0.05f;
        run_time += dt;   // survival timer
        // The enemy builds up its turret ring over time (one more roughly every 35s, capped).
        enemy_turret_n = std::min(ENEMY_TURRET_MAX, 1 + static_cast<int>(run_time / 35.0f));
        // Advance the day/night clock (host/standalone owns it; clients get it via snapshot).
        if (net.role != dc::net::Role::Client) { tod += dt; if (tod >= CYCLE_LEN) { tod -= CYCLE_LEN; ++day_num; LOGLINE("=== NEW DAY ==="); } }

        // ESC toggles the pause menu, which frees the cursor (so you can alt-tab to
        // the other window during net testing). Re-captures on resume.
        bool esc_now = input.key_down(SDL_SCANCODE_ESCAPE);
        if (esc_now && !esc_prev) {
            if (cmd_map) {                    // Esc closes the command minimap
                cmd_map = false; cmd_drag = -1; window.set_relative_mouse(true);
            } else if (spawn_menu) {           // Esc closes the muster menu
                spawn_menu = false; window.set_relative_mouse(true);
            } else if (upgrade_menu) {         // Esc closes the barracks upgrade menu
                upgrade_menu = false; window.set_relative_mouse(true);
            } else if (building_mode) {        // Esc cancels build mode
                building_mode = false;
            } else if (menu_chest >= 0) {
                // Close the chest menu without buying: tell the host to drop our lock.
                if (net.role == dc::net::Role::Client) {
                    uint32_t idx = static_cast<uint32_t>(menu_chest);
                    unsigned char buf[1 + 4]; buf[0] = static_cast<unsigned char>(dc::net::MsgType::ReleaseChest);
                    std::memcpy(buf + 1, &idx, 4); net.send_to_host(buf, sizeof buf, true);
                } else {
                    chests[menu_chest].locked_by = NO_LOCK;
                }
                menu_chest = -1; window.set_relative_mouse(true);
            } else {                           // only open the quit/pause menu when nothing else is up
                paused = !paused;
                if (paused) { window.set_relative_mouse(false); pause_click_prev = true; }
                else window.set_relative_mouse(true);
            }
        }
        esc_prev = esc_now;

        // Hold Tab: damage leaderboard + your items. Frees the cursor to hover items;
        // releases back to mouselook (unless another menu owns the cursor).
        bool tab_now = input.key_down(SDL_SCANCODE_TAB);
        if (tab_now && !tab_prev && !paused && menu_chest < 0) { scoreboard = true;  window.set_relative_mouse(false); }
        if (!tab_now && tab_prev && scoreboard)                { scoreboard = false; if (!paused && menu_chest < 0) window.set_relative_mouse(true); }
        tab_prev = tab_now;

        // A queued level-up opens the upgrade-pick overlay, but never on top of another
        // menu (chest/pause/scoreboard) — it waits its turn.
        if (!levelup_open && pending_levelups > 0 && menu_chest < 0 && !paused && !scoreboard)
            open_levelup();

        // Any open UI (upgrade cards, pause, scoreboard, level-up, or muster menu) freezes control.
        // M toggles the command minimap (frees the cursor to drag pins).
        {
            const bool m_now = input.key_down(SDL_SCANCODE_M);
            if (m_now && !cmd_m_prev && !paused && menu_chest < 0 && !levelup_open && !spawn_menu && player.health > 0.0f)
                { cmd_map = !cmd_map; window.set_relative_mouse(!cmd_map); cmd_drag = -1; }
            cmd_m_prev = m_now;
        }
        const bool ui_open = menu_chest >= 0 || paused || scoreboard || levelup_open || spawn_menu || upgrade_menu || cmd_map;

        // Command minimap: drag each mob TYPE's pin along the lane strip to set a hold line; drag
        // it OFF the strip (left into the AUTO bay, or below its row) to put that type back on
        // auto-march. Host applies directly; clients send HoldCmd.
        auto set_hold = [&](int ty, bool active, float x) {
            if (net.role == dc::net::Role::Client) {
                unsigned char buf[1 + 1 + 1 + 4]; buf[0] = static_cast<unsigned char>(dc::net::MsgType::HoldCmd);
                buf[1] = static_cast<unsigned char>(ty); buf[2] = active ? 1 : 0; std::memcpy(buf + 3, &x, 4);
                net.send_to_host(buf, sizeof buf, true);
            } else type_hold_x[ty] = active ? x : -1.0f;
        };
        const float CM_SX0 = -0.58f, CM_SX1 = 0.58f;   // lane strip extent (NDC x)
        auto cm_rowY = [&](int t) { return 0.30f - t * 0.085f; };
        auto cm_pinx = [&](int t) {                    // current pin NDC-x for a type (parked left if auto)
            if (type_hold_x[t] < 0.0f) return CM_SX0 - 0.14f;
            const float f = (type_hold_x[t] - core_pos[0]) / (enemy_core_pos[0] - core_pos[0]);
            return CM_SX0 + (f < 0 ? 0 : f > 1 ? 1 : f) * (CM_SX1 - CM_SX0);
        };
        if (cmd_map) {
            float mx, my; input.mouse_pos(mx, my); int ww, wh; window.framebuffer_size(ww, wh);
            const float mxn = ww > 0 ? (mx / ww) * 2.0f - 1.0f : 0.0f;
            const float myn = wh > 0 ? 1.0f - (my / wh) * 2.0f : 0.0f;
            const bool lmb = input.mouse_down(SDL_BUTTON_LEFT);
            cmd_drag_mx = mxn; cmd_drag_my = myn;
            if (lmb && !cmd_lmb_prev && cmd_drag < 0) {   // grab a pin
                for (int t = 0; t < dc::game::MOB_TYPE_COUNT; ++t) {
                    if (!(barracks_unlocked & (1u << t))) continue;
                    if (std::fabs(mxn - cm_pinx(t)) < 0.045f && std::fabs(myn - cm_rowY(t)) < 0.04f) { cmd_drag = t; break; }
                }
            }
            if (cmd_drag >= 0 && !lmb) {   // release -> commit
                const int t = cmd_drag; cmd_drag = -1;
                const bool to_auto = (mxn < CM_SX0 - 0.04f) || (myn < cm_rowY(t) - 0.10f) || (myn > cm_rowY(t) + 0.10f);
                if (to_auto) set_hold(t, false, 0.0f);
                else {
                    float f = (mxn - CM_SX0) / (CM_SX1 - CM_SX0); f = f < 0 ? 0 : (f > 1 ? 1 : f);
                    set_hold(t, true, core_pos[0] + f * (enemy_core_pos[0] - core_pos[0]));
                }
            }
            cmd_lmb_prev = lmb;
        } else cmd_lmb_prev = false;
        // Dead = ghost: you can still walk around to spectate, but can't fight, block,
        // use specials, or buy chests. Movement is intentionally NOT gated on this.
        const bool dead = player.health <= 0.0f;

        // Muster menu (E at base): 1-4 UNLOCK a locked mob type (pay) or SELECT an unlocked one
        // to build (jumps into build mode with that barracks). E/Esc closes it.
        if (spawn_menu) {
            // 1-8 UNLOCK a locked mob type (pay) or SELECT an unlocked one (jumps into build mode
            // with that barracks). Defenses are placed via build mode (B). 9 = expand build area.
            // Select a mob type k: unlock it (free) if locked, else jump into build mode with it.
            auto select_mob = [&](int k) {
                if (k < 0 || k >= dc::game::MOB_TYPE_COUNT) return;
                if (barracks_unlocked & (1u << k)) {
                    build_sel = static_cast<int>(dc::game::BuildPiece::Barracks);
                    build_tier = k; building_mode = true; spawn_menu = false;
                    window.set_relative_mouse(true);
                    for (int j = 0; j < 6; ++j) digit_prev[j] = true;
                } else if (net.role == dc::net::Role::Client) {
                    unsigned char buf[2] = { static_cast<unsigned char>(dc::net::MsgType::BuyUnlock), static_cast<unsigned char>(k) };
                    net.send_to_host(buf, 2, true);
                } else host_unlock(k, currency);
            };
            // Keys 1-9 are shortcuts for the first nine types; the rest (and all of them) are
            // reachable by CLICKING the row.
            const int kscan[9] = { SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4,
                                   SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9 };
            for (int k = 0; k < dc::game::MOB_TYPE_COUNT && k < 9; ++k) {
                const bool d = input.key_down(kscan[k]);
                if (d && !spawn_digit_prev[k]) select_mob(k);
                spawn_digit_prev[k] = d;
            }
            // Mouse click on a menu row (works for every type + the expand-area row).
            {
                int ww, wh; window.framebuffer_size(ww, wh);
                float mx, my; input.mouse_pos(mx, my);
                const float mxn = mx / ww * 2.0f - 1.0f, myn = 1.0f - my / wh * 2.0f;
                const bool lmb = input.mouse_down(SDL_BUTTON_LEFT);
                if (lmb && !spawn_lmb_prev && mxn > -0.62f && mxn < 0.62f) {
                    for (int k = 0; k < dc::game::MOB_TYPE_COUNT; ++k) {
                        const float ry = 0.40f - k * 0.072f;
                        if (myn > ry - 0.035f && myn < ry + 0.045f) { select_mob(k); break; }
                    }
                    const float ry8 = 0.40f - dc::game::MOB_TYPE_COUNT * 0.072f;
                    if (myn > ry8 - 0.035f && myn < ry8 + 0.045f) {
                        if (net.role == dc::net::Role::Client) { unsigned char b = static_cast<unsigned char>(dc::net::MsgType::BuyBaseArea); net.send_to_host(&b, 1, true); }
                        else host_buy_area(currency);
                    }
                }
                spawn_lmb_prev = lmb;
            }
            // 0 buys a buildable-AREA expansion — more room to add spawns + defenses.
            {
                const bool d = input.key_down(SDL_SCANCODE_0);
                if (d && !spawn_digit_prev[9]) {
                    if (net.role == dc::net::Role::Client) { unsigned char b = static_cast<unsigned char>(dc::net::MsgType::BuyBaseArea); net.send_to_host(&b, 1, true); }
                    else host_buy_area(currency);
                }
                spawn_digit_prev[9] = d;
            }
            if (input.key_down(SDL_SCANCODE_E) && !e_prev) { spawn_menu = false; window.set_relative_mouse(true); }
        }

        // Barracks UPGRADE menu (E near a barracks): 1-4 buy HP/DEF/SPEED/RATE for THIS barracks,
        // S sells it for 50% back, E/Esc closes. Closes itself if the barracks is gone.
        if (upgrade_menu) {
            const int bidx = piece_index_at(upg_col, upg_row);
            if (bidx < 0 || base.pieces[bidx].piece != static_cast<uint8_t>(dc::game::BuildPiece::Barracks)) {
                upgrade_menu = false; window.set_relative_mouse(true);
            } else {
                const int uscan[5] = { SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5 };
                for (int s = 0; s < dc::game::BARRACKS_UP_STATS; ++s) {
                    const bool d = input.key_down(uscan[s]);
                    if (d && !upg_digit_prev[s]) {
                        if (net.role == dc::net::Role::Client) {
                            unsigned char buf[4] = { static_cast<unsigned char>(dc::net::MsgType::BuyBarracksUp),
                                static_cast<unsigned char>(s), static_cast<unsigned char>(upg_col & 0xFF), static_cast<unsigned char>(upg_row & 0xFF) };
                            net.send_to_host(buf, 4, true);
                        } else host_upgrade_barracks(upg_col, upg_row, s, currency);
                    }
                    upg_digit_prev[s] = d;
                }
                // S = sell this barracks (50% refund).
                static bool sell_prev = false;
                const bool sd = input.key_down(SDL_SCANCODE_S);
                if (sd && !sell_prev) {
                    if (net.role == dc::net::Role::Client) {
                        dc::net::BuildEdit be{}; be.col = static_cast<int16_t>(upg_col); be.row = static_cast<int16_t>(upg_row);
                        unsigned char buf[1 + sizeof be]; buf[0] = static_cast<unsigned char>(dc::net::MsgType::BuildRemove);
                        std::memcpy(buf + 1, &be, sizeof be); net.send_to_host(buf, sizeof buf, true);
                    } else host_remove(upg_col, upg_row, currency);
                    upgrade_menu = false; window.set_relative_mouse(true);
                }
                sell_prev = sd;
                if (input.key_down(SDL_SCANCODE_E) && !e_prev) { upgrade_menu = false; window.set_relative_mouse(true); }
            }
        }

        // --- Build mode (B): place/remove base pieces with the crosshair --------------------
        // B toggles it. You keep mouselook + movement; the mouse buttons + number keys drive
        // building instead of combat (combat is suppressed below while building_mode is on).
        {
            const bool b_now = input.key_down(SDL_SCANCODE_B);
            if (b_now && !b_prev && !ui_open && !dead) building_mode = !building_mode;
            b_prev = b_now;
            // Other menus cancel build mode, but the MUSTER menu is excluded: selecting a mob
            // type there intentionally jumps straight into build mode (closing the menu).
            if (paused || menu_chest >= 0 || scoreboard || levelup_open || dead) building_mode = false;
        }
        build_has_target = false; build_valid = false;
        if (building_mode && !spawn_menu) {
            // 1..N select a PLACEABLE piece (Barracks come from the muster menu; Water is terrain,
            // not buyable). R rotates 90°.
            const int BUILD_KINDS[6] = { static_cast<int>(dc::game::BuildPiece::Barricade),
                                         static_cast<int>(dc::game::BuildPiece::Landmine),
                                         static_cast<int>(dc::game::BuildPiece::Turret),
                                         static_cast<int>(dc::game::BuildPiece::Vacuum),
                                         static_cast<int>(dc::game::BuildPiece::Shipyard),   // Sub Pen disabled for now
                                         static_cast<int>(dc::game::BuildPiece::Mortar) };
            const int kscan[6] = { SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6 };
            for (int k = 0; k < 6; ++k) {
                const bool d = input.key_down(kscan[k]);
                if (d && !digit_prev[k]) build_sel = BUILD_KINDS[k];
                digit_prev[k] = d;
            }
            const bool r_now = input.key_down(SDL_SCANCODE_R);
            if (r_now && !bld_r_prev) build_rot = (build_rot + 1) & 3;
            bld_r_prev = r_now;
            // T cycles the Barracks MOB TYPE (Grunt/Soldier/Brute/Scavenger).
            const bool t_now = input.key_down(SDL_SCANCODE_T);
            if (t_now && !bld_t_prev && build_sel == static_cast<int>(dc::game::BuildPiece::Barracks))
                build_tier = (build_tier + 1) % dc::game::MOB_TYPE_COUNT;
            if (t_now && !bld_t_prev && build_sel == static_cast<int>(dc::game::BuildPiece::Shipyard))
                shipyard_type = (shipyard_type + 1) % 3;   // 0 warship, 1 minelayer, 2 minesweeper
            bld_t_prev = t_now;
            // For barracks, the placement "rotation" field carries the chosen mob type.
            const int place_rot = (build_sel == static_cast<int>(dc::game::BuildPiece::Barracks)) ? build_tier
                                : (build_sel == static_cast<int>(dc::game::BuildPiece::Shipyard)) ? shipyard_type : build_rot;
            // March the look ray to the ground to find the targeted tile.
            vec3 aim; player.front(aim);
            float t = 0.0f; vec3 hp = { player.position[0], player.position[1], player.position[2] };
            for (int s = 0; s < 100; ++s) {
                t += 0.2f;
                vec3 pnt = { player.position[0] + aim[0]*t, player.position[1] + aim[1]*t, player.position[2] + aim[2]*t };
                if (pnt[1] <= terrain.height(pnt[0], pnt[2])) { glm_vec3_copy(pnt, hp); build_has_target = true; break; }
            }
            if (build_has_target) {
                build_col = static_cast<int>(std::floor(hp[0] / dc::world::TILE));
                build_row = static_cast<int>(std::floor(hp[2] / dc::world::TILE));
                build_valid = tile_buildable(build_col, build_row);
            }
            // Aiming at an existing BARRACKS? 6/7/8/9 buy an upgrade for THAT barracks' troops.
            {
                const int bidx = build_has_target ? piece_index_at(build_col, build_row) : -1;
                const bool on_bar = bidx >= 0 && base.pieces[bidx].piece == static_cast<uint8_t>(dc::game::BuildPiece::Barracks);
                const int upscan[4] = { SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9 };
                for (int s = 0; s < 4; ++s) {
                    const bool d = input.key_down(upscan[s]);
                    if (d && !barup_prev[s] && on_bar) {
                        if (net.role == dc::net::Role::Client) {
                            unsigned char buf[4] = { static_cast<unsigned char>(dc::net::MsgType::BuyBarracksUp),
                                                     static_cast<unsigned char>(s),
                                                     static_cast<unsigned char>(build_col & 0xFF), static_cast<unsigned char>(build_row & 0xFF) };
                            net.send_to_host(buf, 4, true);
                        } else host_upgrade_barracks(build_col, build_row, s, currency);
                    }
                    barup_prev[s] = d;
                }
            }
            const bool is_client = (net.role == dc::net::Role::Client);
            // LMB place, RMB remove (edge-triggered).
            const bool lmb = input.mouse_down(SDL_BUTTON_LEFT), rmb = input.mouse_down(SDL_BUTTON_RIGHT);
            if (lmb && !bld_lmb_prev && build_has_target && build_valid) {
                if (is_client) {
                    dc::net::BuildEdit be{}; be.col = static_cast<int16_t>(build_col); be.row = static_cast<int16_t>(build_row);
                    be.piece = static_cast<uint8_t>(build_sel); be.rot = static_cast<uint8_t>(place_rot);
                    unsigned char buf[1 + sizeof be]; buf[0] = static_cast<unsigned char>(dc::net::MsgType::BuildPlace);
                    std::memcpy(buf + 1, &be, sizeof be); net.send_to_host(buf, sizeof buf, true);
                } else host_place(build_col, build_row, build_sel, place_rot, currency);
            }
            if (rmb && !bld_rmb_prev && build_has_target) {
                if (is_client) {
                    dc::net::BuildEdit be{}; be.col = static_cast<int16_t>(build_col); be.row = static_cast<int16_t>(build_row);
                    unsigned char buf[1 + sizeof be]; buf[0] = static_cast<unsigned char>(dc::net::MsgType::BuildRemove);
                    std::memcpy(buf + 1, &be, sizeof be); net.send_to_host(buf, sizeof buf, true);
                } else host_remove(build_col, build_row, currency);
            }
            bld_lmb_prev = lmb; bld_rmb_prev = rmb;
            // F buys a buildable-area expansion (grows the dome).
            const bool f_now = input.key_down(SDL_SCANCODE_F);
            if (f_now && !bld_f_prev) {
                if (is_client) { unsigned char b = static_cast<unsigned char>(dc::net::MsgType::BuyBaseArea); net.send_to_host(&b, 1, true); }
                else host_buy_area(currency);
            }
            bld_f_prev = f_now;
        } else {
            bld_lmb_prev = false; bld_rmb_prev = false;   // don't carry a click across mode exit
        }
        // Refresh turret positions from the (possibly just-edited, or replicated) piece layout.
        // The base is NOT saved to disk — it lives only for the current run.
        base_dirty = false;
        rebuild_turrets();
        rebuild_mortars();

        // RALLY command: C sets a hold-position rally at the crosshair ground point; X clears it
        // (mobs resume pushing the enemy core). Host applies directly; clients ask via RallyCmd.
        if (!ui_open && !dead && !building_mode) {
            const bool c_now = input.key_down(SDL_SCANCODE_C);
            if (c_now && !rally_c_prev) {
                vec3 aim; player.front(aim);
                float t = 0.0f; vec3 hp = { player.position[0], player.position[1], player.position[2] }; bool hit = false;
                for (int s = 0; s < 120; ++s) {
                    t += 0.25f;
                    vec3 pnt = { player.position[0] + aim[0]*t, player.position[1] + aim[1]*t, player.position[2] + aim[2]*t };
                    if (pnt[1] <= terrain.height(pnt[0], pnt[2])) { glm_vec3_copy(pnt, hp); hit = true; break; }
                }
                if (hit) {
                    if (net.role == dc::net::Role::Client) {
                        unsigned char buf[1 + 1 + 8]; buf[0] = static_cast<unsigned char>(dc::net::MsgType::RallyCmd);
                        buf[1] = 1; std::memcpy(buf + 2, &hp[0], 4); std::memcpy(buf + 6, &hp[2], 4);
                        net.send_to_host(buf, sizeof buf, true);
                    } else { rally_active = true; rally_pos[0] = hp[0]; rally_pos[2] = hp[2]; rally_pos[1] = hp[1]; }
                }
            }
            rally_c_prev = c_now;
            const bool x_now = input.key_down(SDL_SCANCODE_X);
            if (x_now && !rally_x_prev) {
                if (net.role == dc::net::Role::Client) {
                    unsigned char buf[1 + 1 + 8] = {0}; buf[0] = static_cast<unsigned char>(dc::net::MsgType::RallyCmd); buf[1] = 0;
                    net.send_to_host(buf, sizeof buf, true);
                } else rally_active = false;
            }
            rally_x_prev = x_now;
        }

        // I: hurl a random insult yourself (floating text + TTS) — networked so every peer sees
        // + hears the SAME line (the generated string is transmitted, not re-rolled per peer).
        {
            const bool i_now = input.key_down(SDL_SCANCODE_I);
            if (i_now && !taunt_prev && !ui_open && !dead) {
                std::string text = dc::game::taunt_generate(spark_rng);
                const float hx = player.position[0], hy = player.position[1] + 0.6f, hz = player.position[2];
                if (net.role == dc::net::Role::Client) {
                    spawn_taunt(hx, hy, hz, text, false);   // show + speak locally now
                    dc::net::TauntState ts{}; ts.x = hx; ts.y = hy; ts.z = hz;
                    std::strncpy(ts.text, text.c_str(), sizeof ts.text - 1);
                    unsigned char buf[1 + sizeof ts]; buf[0] = static_cast<unsigned char>(dc::net::MsgType::Taunt);
                    std::memcpy(buf + 1, &ts, sizeof ts); net.send_to_host(buf, sizeof buf, true);
                } else {
                    spawn_taunt(hx, hy, hz, text, true);   // host: local + broadcast to all clients
                    enemy_insult_back(hx, hz);             // a nearby enemy claps back
                }
            }
            taunt_prev = i_now;
        }

        if (!ui_open) player.add_look(input.mouse_dx, input.mouse_dy);   // freeze look in any menu
        // You can still WALK during a level-up pick (look stays frozen so the cursor can click
        // cards); other menus still lock movement.
        const bool move_lock = (menu_chest >= 0 || paused || scoreboard || spawn_menu || upgrade_menu);
        float forward = move_lock ? 0.0f : (input.key_down(SDL_SCANCODE_W) ? 1.0f : 0.0f)
                                         - (input.key_down(SDL_SCANCODE_S) ? 1.0f : 0.0f);
        float strafe  = move_lock ? 0.0f : (input.key_down(SDL_SCANCODE_D) ? 1.0f : 0.0f)
                                         - (input.key_down(SDL_SCANCODE_A) ? 1.0f : 0.0f);
        bool moving = (forward != 0.0f || strafe != 0.0f);
        // Exhaustion: hitting 0 stamina winds you until it recovers past a threshold.
        const float EXHAUST_RECOVER = player.stamina_max * 0.25f;
        if (player.stamina <= 0.0f) exhausted = true;
        else if (player.stamina >= EXHAUST_RECOVER) exhausted = false;
        player.speed = dc::entity::MOVE_SPEED;
        // Jump costs stamina, and only fires when it actually launches (on the ground +
        // affordable + not exhausted). Spend it here; player.update does the launch.
        const bool player_in_water = in_water(player.position[0], player.position[2]);   // for swim FX only now
        bool jump = !move_lock && input.key_down(SDL_SCANCODE_SPACE) && player.on_ground
                  && !exhausted && player.stamina >= dc::entity::JUMP_STAMINA;
        if (jump) player.stamina -= dc::entity::JUMP_STAMINA * player.stamina_mult;

        // Shift, edge-triggered: a dodge-strafe burst (stamina, brief i-frames) in the move
        // direction. Holding Shift then carries into a sprint (below) — strafe into a run.
        bool shift_now = input.key_down(SDL_SCANCODE_LSHIFT);
        if (shift_now && !shift_prev && !ui_open && !dead
            && player.dash_cd <= 0.0f && player.stamina >= player.dash_cost) {
            // Dash direction from the move input (world space), backward when neutral.
            const float cy = std::cos(player.yaw), sy = std::sin(player.yaw);
            float dx = cy * forward - sy * strafe;   // walk*fwd + right*strafe; right = (-sin,0,cos)
            float dz = sy * forward + cy * strafe;
            float dl = std::sqrt(dx * dx + dz * dz);
            if (dl < 1e-4f) { dx = -cy; dz = -sy; dl = 1.0f; }   // standing -> dash backward
            dx /= dl; dz /= dl;
            player.dash_vel[0] = dx * player.dash_speed; player.dash_vel[2] = dz * player.dash_speed;
            player.iframes = player.dash_iframes;
            player.dash_cd = player.dash_cooldown * player.cooldown_mult;
            player.stamina -= player.dash_cost * player.stamina_mult;
            if (my_look.weapon_class == 1) {  // wizard: dissolve into a mist of motes
                burst_mist(player.position[0], player.position[1] - dc::world::EYE_HEIGHT + 0.9f, player.position[2], 70);
            } else {                           // knight: a DIRECTIONAL dodge keyed off the input axis
                const bool standing = (std::fabs(forward) < 1e-3f && std::fabs(strafe) < 1e-3f);
                if (standing || (std::fabs(forward) >= std::fabs(strafe) && forward < 0.0f)) {
                    // backward (or neutral) dodge: a backward HOP, no roll.
                    player.vel_y = dc::entity::JUMP_SPEED * 0.7f; player.on_ground = false;
                    roll_t = 0.0f;
                } else if (std::fabs(forward) >= std::fabs(strafe)) {
                    roll_dir = 0; roll_t = ROLL_DUR;            // forward shoulder-roll (covered)
                    roll_yaw = std::atan2(dz, dx);             // somersault toward the actual dodge dir
                } else {
                    roll_dir = (strafe > 0.0f) ? 2 : 1;         // side-roll: right (D) / left (A)
                    roll_t = ROLL_DUR;
                }
            }
            if (player.supersonic_damage > 0.0f) {   // spiral-gust visual on every peer; host queues the damage
                ss_anim = SS_ANIM_TIME; glm_vec3_copy(player.position, ss_pos);
                if (net.role != dc::net::Role::Client)
                    supersonic_blasts.push_back({ player.position[0], player.position[2], player.supersonic_damage, my_id });
            }
            if (net.role == dc::net::Role::Client) {
                dc::net::DashCast dc{}; dc.dx = dx; dc.dz = dz;
                dc.speed = player.dash_speed; dc.iframes = player.dash_iframes; dc.decay = player.dash_decay;
                dc.supersonic = player.supersonic_damage;
                unsigned char buf[1 + sizeof dc];
                buf[0] = static_cast<unsigned char>(dc::net::MsgType::DashCast);
                std::memcpy(buf + 1, &dc, sizeof dc);
                net.send_to_host(buf, sizeof buf, true);
            }
        }
        shift_prev = shift_now;
        if (roll_t > 0.0f) roll_t -= dt;
        // Sprint: keep holding Shift (after the dash burst) to run faster — strafe into a
        // sprint. Drains stamina while running (pauses regen below); stops when you run out
        // or are exhausted.
        const bool sprinting = input.key_down(SDL_SCANCODE_LSHIFT) && !ui_open && !dead
                             && moving && !exhausted && player.stamina > 0.0f;
        player.speed = sprinting ? dc::entity::MOVE_SPEED * 1.7f : dc::entity::MOVE_SPEED;
        if (in_slime(player.position[0], player.position[2])) player.speed *= SLIME_SLOW;   // slime mires you
        player.update(forward, strafe, jump, dt, *map, terrain);   // water no longer slows movement (simplified)
        // Swimming: little blue splash motes kick up around your feet while wading + moving.
        if (player_in_water && moving) {
            auto jr = [&]() { spark_rng = spark_rng * 1664525u + 1013904223u; return (spark_rng >> 8) * (1.0f / 16777216.0f); };
            for (int s = 0; s < 3 && sparks.size() < 2200; ++s) {
                Spark sp;
                const float gy = terrain.height(player.position[0], player.position[2]);
                sp.pos[0] = player.position[0] + (jr()-0.5f)*0.8f; sp.pos[1] = gy + 0.1f; sp.pos[2] = player.position[2] + (jr()-0.5f)*0.8f;
                const float ang = jr()*6.2831853f, spd = 0.6f + jr()*1.4f;
                sp.vel[0] = std::cos(ang)*spd; sp.vel[1] = 1.6f + jr()*1.8f; sp.vel[2] = std::sin(ang)*spd;
                sp.color[0] = 0.45f + jr()*0.2f; sp.color[1] = 0.7f; sp.color[2] = 1.0f;   // watery blue
                sp.age = 0.0f; sp.life = 0.4f + jr()*0.35f; sp.grav = 7.0f; sp.size_mul = 1.4f; sp.alpha_mul = 0.8f;
                sparks.push_back(sp);
            }
        }
        {   // launch off a pad (predicted on every peer), flung in the current move direction
            const float cy = std::cos(player.yaw), sy = std::sin(player.yaw);
            float dx = cy*forward - sy*strafe, dz = sy*forward + cy*strafe, dl = std::sqrt(dx*dx + dz*dz);
            if (dl > 1e-4f) { dx /= dl; dz /= dl; } else { dx = dz = 0.0f; }
            apply_updraft(player, dx, dz);
        }
        if (net.role != dc::net::Role::Client) apply_fallout(player);   // damage is host-authoritative

        // Walk clock: advance while moving, reset when idle.
        if (moving) anim_time += dt; else anim_time = 0.0f;

        // --- Networked player sync (host side) ---
        // The client sends its InputCmd later, once combat flags (strike/blocking)
        // for this frame are known. Here the host advances each client's body.
        if (net.role == dc::net::Role::Host) {
            // Host simulates each connected client's body from their latest input.
            // (The combined snapshot is broadcast later, after enemies/coins update.)
            for (auto& hc : host_clients) {
                hc.body.yaw = hc.input.yaw; hc.body.pitch = hc.input.pitch;
                hc.body.update(hc.input.forward, hc.input.strafe, hc.input.jump != 0, dt, *map, terrain);
                {   // host runs clients' launch (flung in their move direction) + safety net
                    const float cy = std::cos(hc.input.yaw), sy = std::sin(hc.input.yaw);
                    float dx = cy*hc.input.forward - sy*hc.input.strafe, dz = sy*hc.input.forward + cy*hc.input.strafe;
                    const float dl = std::sqrt(dx*dx + dz*dz);
                    if (dl > 1e-4f) { dx /= dl; dz /= dl; } else { dx = dz = 0.0f; }
                    apply_updraft(hc.body, dx, dz);
                }
                apply_fallout(hc.body);
                bool m = (hc.input.forward != 0.0f || hc.input.strafe != 0.0f);
                hc.anim_time = m ? hc.anim_time + dt : 0.0f;
                if (hc.body.hit_flash > 0.0f) hc.body.hit_flash -= dt;   // decay the flash
                if (hc.body.health > 0.0f) {                              // passive regen (not ghosts)
                    hc.body.health += hc.input.health_regen * dt;          // client's own (upgradeable) regen
                    if (hc.body.health > hc.body.stats.max_health) hc.body.health = hc.body.stats.max_health;
                }
            }
            // (Remotes are rebuilt after combat, so hit_flash this frame is included.)
        }

        // Block held (right mouse): needs a shield and some stamina. The shield is on the
        // right arm and the swing is on the left, so you can do both at once (each drains
        // its own stamina).
        bool block_held = player.shield.has_value() && !exhausted && !ui_open && !dead && !building_mode
                        && input.mouse_down(SDL_BUTTON_RIGHT);

        // Attack (left mouse): one-shot swing, gated by cooldown, not-blocking, and
        // enough stamina. Weapon sets playback/cooldown/cost; fists fall back.
        const float PUNCH_STRIKE = 0.18f;  // when in the clip the hit lands (clip seconds)
        const float atk_speed  = player.weapon ? player.weapon->attack_speed     : dc::entity::UNARMED_ATTACK_SPEED;
        const float atk_cd_dur = player.weapon ? player.weapon->cooldown         : dc::entity::UNARMED_COOLDOWN;
        const float swing_cost = player.weapon ? player.weapon->stamina_per_swing : dc::entity::UNARMED_STAMINA;
        const float throw_cost = player.weapon ? player.weapon->stamina_per_throw : 1e9f;  // no weapon -> can't throw
        if (attack_cd > 0.0f) attack_cd -= dt;
        if (throw_cd  > 0.0f) throw_cd  -= dt;
        if (orbit_cd  > 0.0f) orbit_cd  -= dt;
        if (bash_cd   > 0.0f) bash_cd   -= dt;

        // True when any living enemy is within `r` (xz) of the local player. Autocasts
        // only fire when there's something to hit, so they idle when you're exploring.
        // Uses entities.items, which is the live list on the host and the replicated
        // mirror on clients (both have enemy positions).
        auto enemy_within = [&](float r) {
            const float r2 = r * r;
            for (const auto& e : entities.items) {
                if (!e.alive) continue;
                const float dx = e.position[0] - player.position[0];
                const float dz = e.position[2] - player.position[2];
                if (dx * dx + dz * dz <= r2) return true;
            }
            return false;
        };
        const float ORBIT_TRIGGER = (my_look.weapon_class == 1) ? 11.0f : 7.0f;   // wizards trigger orbit from further (they kite)

        // Force Nova autocast: once unlocked it occupies a spell slot and fires on its own
        // cooldown whenever an enemy is inside its blast radius. Costs no stamina; the
        // expanding sphere shoves enemies back (resolved in the update block below).
        if (player.forcefield_unlocked && player.shield && !ui_open && !dead && !bash.active
            && bash_cd <= 0.0f && enemy_within(player.shield->bash_radius)) {
            bash.active = true; bash.time = 0.0f; bash.radius = 0.0f; bash.hit_ids.clear();
            bash_cd = player.shield->bash_cooldown * player.forcefield_cd_mult * player.autocast_cd_mult;
            // Event model: tell the host once (reliable). It runs the damage + tells
            // everyone else; we just predicted the visual above.
            if (net.role == dc::net::Role::Client) {
                dc::net::BashCast bc;
                bc.radius = player.shield->bash_radius;
                bc.damage = player.shield->bash_damage * player.damage_mult;
                bc.knockback = player.shield->bash_knockback;
                bc.duration = player.shield->bash_duration;
                unsigned char buf[1 + sizeof bc];
                buf[0] = static_cast<unsigned char>(dc::net::MsgType::BashCast);
                std::memcpy(buf + 1, &bc, sizeof bc);
                net.send_to_host(buf, sizeof buf, true);   // reliable: a cast can't be dropped
            }
        }

        // Orbit autocast: once unlocked it occupies a spell slot and fires on its own
        // cooldown whenever an enemy is near. Costs no stamina; spinning swords circle you.
        if (player.orbit_unlocked && player.weapon && !ui_open && !dead && !orbit.active
            && orbit_cd <= 0.0f && enemy_within(ORBIT_TRIGGER)) {
            orbit.active = true;
            orbit.time = player.weapon->orbit_duration;
            orbit.angle = 0.0f; orbit.spin = 0.0f; orbit.tick = 0.0f;
            orbit.hit_ids.clear();
            orbit_cd = player.weapon->orbit_cooldown * player.orbit_cd_mult * player.autocast_cd_mult;
            if (net.role == dc::net::Role::Client) {   // reliable cast: host runs damage + broadcasts
                const auto& w = *player.weapon;
                dc::net::OrbitCast oc;
                oc.duration = w.orbit_duration; oc.radius = w.orbit_radius;
                oc.hit_radius = w.orbit_hit_radius * player.sword_scale;
                oc.damage = w.orbit_damage * player.damage_mult; oc.knockback = player.stats.knockback;
                oc.count = w.orbit_count;
                oc.tick = 0.25f * player.orbit_tick_mult; oc.spin = player.orbit_spin_mult;
                unsigned char buf[1 + sizeof oc];
                buf[0] = static_cast<unsigned char>(dc::net::MsgType::OrbitCast);
                std::memcpy(buf + 1, &oc, sizeof oc);
                net.send_to_host(buf, sizeof buf, true);
            }
        }
        if (bolt_cd > 0.0f) bolt_cd -= dt;
        const bool wizard = (my_look.weapon_class == 1);
        // Wizard staff: LMB = quick bolt, MMB = bigger bolt(s) (MultiThrow adds more). The
        // bolt damages enemies as it flies. Host/standalone sims it; a client casts to the host.
        auto fire_bolts = [&](bool big) {
            vec3 f; player.front(f);
            const int n = big ? std::max(1, player.throw_count) : 1;
            const float spread = 0.13f;
            const float rad = (big ? 1.0f : 0.5f) * player.sword_scale;
            const float dmg = (big ? 46.0f : 18.0f) * player.damage_mult;
            const float kb  = big ? 14.0f : 5.0f;
            for (int t = 0; t < n; ++t) {
                const float off = (n > 1) ? (t - (n - 1) * 0.5f) * spread : 0.0f;
                const float ca = std::cos(off), sa = std::sin(off);
                vec3 d = { f[0]*ca - f[2]*sa, f[1], f[0]*sa + f[2]*ca };
                if (net.role == dc::net::Role::Client) {
                    dc::net::BoltCast bc;
                    bc.ox = player.position[0]; bc.oy = player.position[1]; bc.oz = player.position[2];
                    bc.dx = d[0]; bc.dy = d[1]; bc.dz = d[2];
                    bc.radius = rad; bc.damage = dmg; bc.knockback = kb; bc.big = big ? 1 : 0; bc.count = 1;
                    unsigned char buf[1 + sizeof bc]; buf[0] = static_cast<unsigned char>(dc::net::MsgType::BoltCast);
                    std::memcpy(buf + 1, &bc, sizeof bc); net.send_to_host(buf, sizeof buf, true);
                } else {
                    Bolt b; glm_vec3_copy(player.position, b.pos); glm_vec3_copy(d, b.dir);
                    b.radius = rad; b.damage = dmg; b.knockback = kb; b.big = big; b.owner = my_id;
                    bolts.push_back(std::move(b));
                }
            }
        };
        // Wizard fires staff bolts (half the knight's swing rate by default). While the fire
        // button is held the staff is held OUT in a steady cast pose (set after the punch
        // update below) — it doesn't re-swing the arm each shot.
        const bool wiz_firing = wizard && !ui_open && !dead && !building_mode
                              && (input.mouse_down(SDL_BUTTON_LEFT) || input.mouse_down(SDL_BUTTON_MIDDLE));
        if (wizard && !ui_open && !dead && !building_mode) {
            if (input.mouse_down(SDL_BUTTON_MIDDLE) && throw_cd <= 0.0f) { fire_bolts(true);  throw_cd = 1.7f * player.cooldown_mult; }
            else if (input.mouse_down(SDL_BUTTON_LEFT) && bolt_cd <= 0.0f) { fire_bolts(false); bolt_cd = 0.44f * player.cooldown_mult; }
        }
        // Knight: start a melee swing (LMB) or a sword throw (MMB) — both play the punch clip;
        // the difference is resolved at the strike frame below.
        if (!wizard && !punching && throwns.empty() && !ui_open && !dead && !building_mode) {
            if (player.weapon && input.mouse_down(SDL_BUTTON_MIDDLE)
                && throw_cd <= 0.0f && player.stamina >= throw_cost) {
                punching = true; punch_time = 0.0f; punch_struck = false; punch_is_throw = true;
                player.stamina -= throw_cost * player.stamina_mult;
            } else if (input.mouse_down(SDL_BUTTON_LEFT)
                       && attack_cd <= 0.0f && player.stamina >= swing_cost) {
                punching = true; punch_time = 0.0f; punch_struck = false; punch_is_throw = false;
                player.stamina -= swing_cost * player.stamina_mult;
            }
        }
        bool player_strike = false;        // true only on the frame a MELEE swing connects
        if (punching) {
            punch_time += dt * atk_speed;
            if (!punch_struck && punch_time >= PUNCH_STRIKE) {
                punch_struck = true;
                if (punch_is_throw && player.weapon) {
                    // Release: detach the sword(s) as spinning projectiles flying forward.
                    // Swordstorm (throw_count > 1) launches a horizontal fan; a single throw
                    // flies straight along the 3D look direction (yaw + pitch).
                    vec3 f; player.front(f);   // already normalized
                    const int n = player.throw_count < 1 ? 1 : player.throw_count;
                    const float SPREAD = 0.16f;   // radians between adjacent swords in the fan
                    for (int t = 0; t < n; ++t) {
                        const float off = (n > 1) ? (t - (n - 1) * 0.5f) * SPREAD : 0.0f;
                        const float ca = std::cos(off), sa = std::sin(off);
                        // Rotate the aim around world-Y so the fan spreads left/right.
                        vec3 d = { f[0] * ca - f[2] * sa, f[1], f[0] * sa + f[2] * ca };
                        ThrownSword ts; ts.active = true; ts.returning = false;
                        ts.traveled = 0.0f; ts.spin = 0.0f;
                        glm_vec3_copy(player.position, ts.pos);
                        glm_vec3_copy(d, ts.dir);
                        throwns.push_back(std::move(ts));
                        if (net.role == dc::net::Role::Client) {   // reliable cast: host flies + damages it
                            const auto& w = *player.weapon;
                            dc::net::ThrownCast tc;
                            tc.dx = d[0]; tc.dy = d[1]; tc.dz = d[2];
                            tc.ox = player.position[0]; tc.oy = player.position[1]; tc.oz = player.position[2];
                            tc.speed = w.throw_speed; tc.distance = w.throw_distance;
                            tc.radius = w.throw_radius * w.throw_size * player.sword_scale;
                            tc.damage = w.throw_damage * player.damage_mult; tc.knockback = player.stats.knockback;
                            tc.size = w.throw_size;
                            unsigned char buf[1 + sizeof tc];
                            buf[0] = static_cast<unsigned char>(dc::net::MsgType::ThrownCast);
                            std::memcpy(buf + 1, &tc, sizeof tc);
                            net.send_to_host(buf, sizeof buf, true);
                        }
                    }
                    throw_cd = player.weapon->throw_cooldown * player.cooldown_mult;
                } else {
                    player_strike = true;
                    // Telegraph the SWING ARC: a fan of bright motes spanning the ACTUAL damage
                    // cone (reach + half-angle, at swing height), so the area that does damage is
                    // obvious. Two motes per angle (mid + outer reach) fill the wedge.
                    vec3 aimf; player.front(aimf);
                    const float ayaw = std::atan2(aimf[2], aimf[0]);
                    const float reach = (player.weapon ? player.weapon->reach : dc::entity::UNARMED_REACH) + player.swing_reach_bonus;
                    float ccos = (player.weapon ? player.weapon->cone_cos : dc::entity::UNARMED_CONE) - player.swing_cone_bonus;
                    if (ccos < -0.5f) ccos = -0.5f; if (ccos > 1.0f) ccos = 1.0f;
                    const float halfa = std::acos(ccos);   // cone half-angle
                    const float cy = terrain.height(player.position[0], player.position[2]) + 1.05f;   // chest-height sweep
                    const int N = 26;
                    for (int i = 0; i < N; ++i) {
                        const float t = (i / static_cast<float>(N - 1)) * 2.0f - 1.0f;   // -1..1 across the arc
                        const float a = ayaw + t * halfa;
                        const float dcs = std::cos(a), dsn = std::sin(a);
                        for (int rr = 0; rr < 2; ++rr) {
                            const float rad = reach * (rr == 0 ? 0.62f : 1.0f);
                            Spark s;
                            s.pos[0] = player.position[0] + dcs * rad;
                            s.pos[1] = cy;
                            s.pos[2] = player.position[2] + dsn * rad;
                            s.vel[0] = dcs * 1.5f; s.vel[1] = 0.6f; s.vel[2] = dsn * 1.5f;   // gentle outward+up sparkle
                            s.color[0] = 0.75f; s.color[1] = 0.92f; s.color[2] = 1.0f;       // bright cyan-white edge
                            s.age = 0.0f; s.life = 0.26f;
                            sparks.push_back(s);
                        }
                    }
                }
            }
            if (!model_data.punch.valid() || punch_time >= model_data.punch.duration) {
                punching = false;
                if (!punch_is_throw) attack_cd = atk_cd_dur * player.cooldown_mult;
            }
        }
        // Wizard: while firing, hold the staff OUT in a steady cast pose (freeze the punch
        // clip at its extended frame) instead of re-swinging. Overrides the punch update above.
        if (wiz_firing) { punching = true; punch_struck = true; punch_is_throw = false; punch_time = 0.34f; }

        // Block animation: raise the shield while held (right arm — independent of the
        // left-arm swing, so you can block and swing together). It only actually mitigates
        // once the raise finishes (block_time reaches the clip end); block_speed scales it.
        const float block_speed = player.shield ? player.shield->block_speed : 1.0f;
        blocking = block_held;                              // animating the raise/hold
        if (blocking) block_time += dt * block_speed; else block_time = 0.0f;
        bool block_ready = blocking && model_data.block.valid()
                         && block_time >= model_data.block.duration;   // shield fully up


        // Stamina: blocking or sprinting drains it (and pauses regen); otherwise it
        // regenerates. (Dash and jump spend a lump sum at their triggers above.)
        if (blocking)       player.stamina -= player.shield->stamina_per_sec * dt * player.stamina_mult;
        else if (sprinting) player.stamina -= dc::entity::RUN_STAMINA_PER_SEC * dt * player.stamina_mult;
        else                player.stamina += player.stamina_regen * dt;
        if (player.stamina > player.stamina_max) player.stamina = player.stamina_max;
        if (player.stamina < 0.0f) player.stamina = 0.0f;

        // Passive health regen while alive (host-authoritative; clients get it via the
        // snapshot). Never regen a ghost — that would revive the dead.
        if (net.role != dc::net::Role::Client && player.health > 0.0f) {
            player.health += player.health_regen * dt;
            if (player.health > player.stats.max_health) player.health = player.stats.max_health;
        }

        // Interact (E, edge-triggered): open the nearest chest's purchase menu if it has
        // items left and isn't in use (one player at a time).
        bool e_now = input.key_down(SDL_SCANCODE_E);
        if (e_now && !e_prev && !ui_open && !dead) {
            // Standing at your base core: E opens the MUSTER menu (unlock/select mob types).
            const float dcx = core_pos[0] - player.position[0], dcz = core_pos[2] - player.position[2];
            const bool at_core = (dcx*dcx + dcz*dcz <= (CORE_RAD + 4.5f)*(CORE_RAD + 4.5f));
            if (at_core) { spawn_menu = true; window.set_relative_mouse(false); }
            else {
            // Standing at one of YOUR barracks: E opens its upgrade menu.
            int ubest = -1; float ubest2 = 9.0f;   // within ~3 tiles
            for (std::size_t i = 0; i < base.pieces.size(); ++i) {
                if (base.pieces[i].piece != static_cast<uint8_t>(dc::game::BuildPiece::Barracks)) continue;
                const float bx = (base.pieces[i].col + 0.5f) * dc::world::TILE, bz = (base.pieces[i].row + 0.5f) * dc::world::TILE;
                const float dx = bx - player.position[0], dz = bz - player.position[2], d2 = dx*dx + dz*dz;
                if (d2 < ubest2) { ubest2 = d2; ubest = static_cast<int>(i); }
            }
            if (ubest >= 0) {
                upgrade_menu = true; upg_col = base.pieces[ubest].col; upg_row = base.pieces[ubest].row;
                window.set_relative_mouse(false);
            } else {
            // Drone vendor in reach takes priority over a chest: pay DRONE_COST -> +1 minion.
            int dbest = -1; float dbest2 = CHEST_REACH * CHEST_REACH;
            for (std::size_t i = 0; i < drone_vendors.size(); ++i) {
                if (drone_vendors[i].bought) continue;
                const float dx = drone_vendors[i].x - player.position[0], dz = drone_vendors[i].z - player.position[2];
                const float d2 = dx*dx + dz*dz;
                if (d2 < dbest2) { dbest2 = d2; dbest = static_cast<int>(i); }
            }
            if (dbest >= 0) {   // standing at a drone vendor: try to buy (don't also open a chest)
                if (currency >= DRONE_COST && player.minion_count < 4) {
                    if (net.role == dc::net::Role::Client) {       // host validates + grants -> we add the drone
                        uint32_t idx = static_cast<uint32_t>(dbest);
                        unsigned char buf[1 + 4]; buf[0] = static_cast<unsigned char>(dc::net::MsgType::BuyDrone);
                        std::memcpy(buf + 1, &idx, 4); net.send_to_host(buf, sizeof buf, true);
                    } else {                                       // host/standalone: buy now
                        currency -= DRONE_COST; drone_vendors[dbest].bought = true; player.minion_count++;
                    }
                }
            } else {
                int best = -1; float best_d2 = CHEST_REACH * CHEST_REACH;
                for (std::size_t i = 0; i < chests.size(); ++i) {
                    if (chests[i].remaining() == 0) continue;        // depleted: nothing to buy
                    const float cx = (chests[i].col + 0.5f) * dc::world::TILE;
                    const float cz = (chests[i].row + 0.5f) * dc::world::TILE;
                    const float dx = cx - player.position[0], dz = cz - player.position[2];
                    const float d2 = dx * dx + dz * dz;
                    if (d2 < best_d2) { best_d2 = d2; best = static_cast<int>(i); }
                }
                if (best >= 0) {
                    if (net.role == dc::net::Role::Client) {         // host grants the lock -> ChestGranted opens our menu
                        uint32_t idx = static_cast<uint32_t>(best);
                        unsigned char buf[1 + 4]; buf[0] = static_cast<unsigned char>(dc::net::MsgType::OpenChest);
                        std::memcpy(buf + 1, &idx, 4); net.send_to_host(buf, sizeof buf, true);
                    } else if (chests[best].locked_by == NO_LOCK) {  // host/standalone: open now (lock to id 0)
                        chests[best].opened = true; chests[best].locked_by = 0; chests[best].lock_time = CHEST_LOCK_TIME;
                        menu_chest = best; window.set_relative_mouse(false);
                    }
                }
            }
            }   // end !barracks
            }   // end !at_core
        }
        e_prev = e_now;

        // Purchase menu: click a remaining item to buy ONE for the chest's price, which
        // closes the menu (and releases the lock for the next player).
        if (menu_chest >= 0 && menu_chest < static_cast<int>(chests.size())) {
            Chest& mc = chests[menu_chest];
            float mx, my; input.mouse_pos(mx, my);
            int ww, wh; window.window_size(ww, wh);
            const float nx = (ww > 0) ? (mx / ww) * 2.0f - 1.0f : 0.0f;
            const float ny = (wh > 0) ? 1.0f - (my / wh) * 2.0f : 0.0f;
            const bool click = input.mouse_down(SDL_BUTTON_LEFT);
            if (click && !menu_click_prev) {
                for (int s = 0; s < 4; ++s) {
                    if (mc.taken[s]) continue;
                    const float x0 = card_x0(s), x1 = x0 + CARD_W;
                    if (nx < x0 || nx > x1 || ny < CARD_BOT || ny > CARD_TOP) continue;
                    if (currency < mc.cost) break;               // can't afford it
                    if (net.role == dc::net::Role::Client) {      // host validates + grants (ItemGranted closes the menu)
                        unsigned char buf[1 + 4 + 4]; buf[0] = static_cast<unsigned char>(dc::net::MsgType::BuyItem);
                        uint32_t ci = static_cast<uint32_t>(menu_chest), sl = static_cast<uint32_t>(s);
                        std::memcpy(buf + 1, &ci, 4); std::memcpy(buf + 5, &sl, 4);
                        net.send_to_host(buf, sizeof buf, true);
                    } else {                                      // host/standalone: buy now
                        currency -= mc.cost; mc.taken[s] = true; apply_pickup(mc.contents[s]);
                        mc.locked_by = NO_LOCK; menu_chest = -1; window.set_relative_mouse(true);
                    }
                    break;
                }
            }
            menu_click_prev = click;
        }

        // Level-up overlay: click one of the offered cards to apply it. If more level-ups
        // are queued the next one opens immediately (re-drawn next frame).
        if (levelup_open) {
            float mx, my; input.mouse_pos(mx, my);
            int ww, wh; window.window_size(ww, wh);
            const float nx = (ww > 0) ? (mx / ww) * 2.0f - 1.0f : 0.0f;
            const float ny = (wh > 0) ? 1.0f - (my / wh) * 2.0f : 0.0f;
            const bool click = input.mouse_down(SDL_BUTTON_LEFT);
            if (click && !levelup_click_prev) {
                for (int s = 0; s < levelup_card_count; ++s) {
                    const float x0 = card_x0(s), x1 = x0 + CARD_W;
                    if (nx < x0 || nx > x1 || ny < CARD_BOT || ny > CARD_TOP) continue;
                    apply_pickup(levelup_cards[s]);
                    levelup_open = false;
                    if (pending_levelups > 0) pending_levelups--;
                    if (pending_levelups == 0 && menu_chest < 0 && !paused && !scoreboard)
                        window.set_relative_mouse(true);
                    break;
                }
            }
            levelup_click_prev = click;
        }


        // Pause menu: click the red Quit button to exit.
        if (paused) {
            float mx, my; input.mouse_pos(mx, my);
            int ww, wh; window.window_size(ww, wh);
            const float nx = (ww > 0) ? (mx / ww) * 2.0f - 1.0f : 0.0f;
            const float ny = (wh > 0) ? 1.0f - (my / wh) * 2.0f : 0.0f;
            const bool click = input.mouse_down(SDL_BUTTON_LEFT);
            if (click && !pause_click_prev && nx >= QX0 && nx <= QX1 && ny >= QY0 && ny <= QY1)
                running = false;
            pause_click_prev = click;
        }

        // Advance each opened chest's lid toward fully open (then hold).
        const float chest_dur = chest_data.open.valid() ? chest_data.open.duration : 0.0f;
        for (auto& ch : chests) {
            if (ch.opened && ch.open_t < chest_dur) {
                ch.open_t += dt;
                if (ch.open_t > chest_dur) ch.open_t = chest_dur;
            }
        }
        // Host: keep my own menu's lock alive while it's open; expire stale locks (a
        // client that opened a menu and never closed it / dropped) after the timeout.
        if (net.role != dc::net::Role::Client) {
            if (menu_chest >= 0 && menu_chest < static_cast<int>(chests.size())) chests[menu_chest].lock_time = CHEST_LOCK_TIME;
            for (auto& ch : chests)
                if (ch.locked_by != NO_LOCK) { ch.lock_time -= dt; if (ch.lock_time <= 0.0f) ch.locked_by = NO_LOCK; }
        }

        // Build this frame's dynamic point lights: torch flames, firing flamethrowers, and
        // glowing projectiles. The shaders sum the nearest MAX_LIGHTS to the player, so a
        // shot streaking past briefly lights up the walls/floor/enemies around it.
        const float t_now = static_cast<float>(now) / 1.0e9f;
        const float LIGHT_RADIUS = 14.0f;                  // torches/pillars throw light a long way
        const vec3  LIGHT_BASE = { 2.2f, 1.35f, 0.7f };    // warm + bright; scaled by flicker
        struct LightCand { float d2, px, py, pz, r, g, b, rad; };
        std::vector<LightCand> lcands;
        auto cand = [&](float px, float py, float pz, float r, float g, float b, float rad) {
            const float dx = px - player.position[0], dz = pz - player.position[2];
            lcands.push_back({ dx*dx + dz*dz, px, py, pz, r, g, b, rad });
        };
        for (std::size_t i = 0; i < torches.size(); ++i) {
            float fl = dc::fx::flicker(t_now + static_cast<float>(i) * 1.7f);
            torches[i].ps.update(dt, torches[i].flame_pos, fl);   // (still flicker every torch's particles)
            cand(torches[i].flame_pos[0], torches[i].flame_pos[1], torches[i].flame_pos[2],
                 LIGHT_BASE[0]*fl, LIGHT_BASE[1]*fl, LIGHT_BASE[2]*fl, LIGHT_RADIUS);
        }
        for (const auto& en : entities.items) {   // firing flamethrowers glow orange
            if (en.type != dc::entity::EntityType::Enemy || en.kind != dc::entity::EnemyKind::Flamethrower || !en.attacking) continue;
            const float fl = 1.2f + 0.3f * dc::fx::flicker(t_now * 1.3f);
            cand(en.position[0], terrain.height(en.position[0], en.position[2]) + 1.2f, en.position[2],
                 1.5f*fl, 0.6f*fl, 0.2f*fl, dc::entity::FLAME_LIGHT_RADIUS);
        }
        for (const auto& pr : entities.projectiles)   // every glowing shot is a small moving light
            cand(pr.pos[0], pr.pos[1], pr.pos[2], pr.color[0]*1.4f, pr.color[1]*1.4f, pr.color[2]*1.4f,
                 pr.beam ? 5.5f : 4.5f);
        // A soft glow around each living player so the dark world stays readable near you.
        const float GLOW_R = 6.5f; const vec3 GLOW_C = { 0.55f, 0.58f, 0.72f };
        if (!dead) cand(player.position[0], player.position[1] - dc::world::EYE_HEIGHT + 1.0f, player.position[2],
                        GLOW_C[0], GLOW_C[1], GLOW_C[2], GLOW_R);
        for (const auto& rp : remotes)
            if (!rp.ghost) cand(rp.pos[0], rp.pos[1] - dc::world::EYE_HEIGHT + 1.0f, rp.pos[2],
                                GLOW_C[0], GLOW_C[1], GLOW_C[2], GLOW_R);
        // The core pylon glows (brighter while it still has health), lighting its surroundings.
        if (core_health > 0.0f) {
            const float pulse = 1.0f + 0.15f * dc::fx::flicker(t_now * 0.7f);
            cand(core_pos[0], core_pos[1] + CORE_H * 0.6f, core_pos[2],
                 0.4f*pulse, 1.3f*pulse, 1.5f*pulse, 12.0f);
        }
        // Keep the nearest MAX_LIGHTS to the player and upload them.
        const int MAXL = dc::renderer::Renderer::MAX_LIGHTS;
        if (static_cast<int>(lcands.size()) > MAXL)
            std::nth_element(lcands.begin(), lcands.begin() + MAXL, lcands.end(),
                             [](const LightCand& a, const LightCand& b) { return a.d2 < b.d2; });
        const int lcount = std::min<int>(static_cast<int>(lcands.size()), MAXL);
        float lpos[dc::renderer::Renderer::MAX_LIGHTS * 3];
        float lcol[dc::renderer::Renderer::MAX_LIGHTS * 3];
        float lrad[dc::renderer::Renderer::MAX_LIGHTS];
        for (int i = 0; i < lcount; ++i) {
            lpos[i*3] = lcands[i].px; lpos[i*3+1] = lcands[i].py; lpos[i*3+2] = lcands[i].pz;
            lcol[i*3] = lcands[i].r;  lcol[i*3+1] = lcands[i].g;  lcol[i*3+2] = lcands[i].b;
            lrad[i] = lcands[i].rad;
        }
        renderer.set_lights(lcount, lpos, lcol, lrad);

        // Debug: G spawns a melee enemy, H a ranged one, a couple tiles ahead.
        bool g_now = input.key_down(SDL_SCANCODE_G);
        if (g_now && !g_prev && net.role != dc::net::Role::Client) {
            vec3 f; player.front(f);
            entities.spawn_enemy(player.position[0] + f[0] * 2.0f, player.position[2] + f[2] * 2.0f);
        }
        g_prev = g_now;
        bool h_now = input.key_down(SDL_SCANCODE_H);
        if (h_now && !h_prev && net.role != dc::net::Role::Client) {
            vec3 f; player.front(f);
            entities.spawn_enemy(player.position[0] + f[0] * 2.0f, player.position[2] + f[2] * 2.0f,
                                 dc::entity::EnemyKind::Ranged);
        }
        h_prev = h_now;
        bool j_now = input.key_down(SDL_SCANCODE_J);
        if (j_now && !j_prev && net.role != dc::net::Role::Client) {
            vec3 f; player.front(f);
            entities.spawn_enemy(player.position[0] + f[0] * 4.0f, player.position[2] + f[2] * 4.0f,
                                 dc::entity::EnemyKind::Flying);
        }
        j_prev = j_now;

        // V toggles first/third person (third is the default). ` toggles hitbox/cone
        // rendering + the debug readout.
        bool v_now = input.key_down(SDL_SCANCODE_V);
        if (v_now && !v_prev) first_person = !first_person;
        v_prev = v_now;
        bool grave_now = input.key_down(SDL_SCANCODE_GRAVE);
        if (grave_now && !grave_prev) {   // ` : debug cheat — +200 gold to the shared pool (Shift+` toggles hitboxes)
            if (input.key_down(SDL_SCANCODE_LSHIFT) || input.key_down(SDL_SCANCODE_RSHIFT)) {
                show_hitboxes = !show_hitboxes; window.set_title(show_hitboxes ? "dungeoncrawl  [hitboxes]" : "dungeoncrawl");
            } else {
                currency += 200; spawn_taunt(player.position[0], player.position[1] + 3.0f, player.position[2], "+200 gold", false);
            }
        }
        grave_prev = grave_now;

        frame_hits.clear();   // collect this frame's damage events (specials + melee below)

        // A travelling attack (thrown sword / bolt) that overlaps an enemy boat damages it once
        // per pass (deduped via hit_ids). Returns the damage dealt. Host-authoritative.
        auto hit_boats = [&](const vec3 pos, float radius, float damage, std::vector<uint32_t>& hit_ids) -> float {
            float dealt = 0.0f;
            const float rr = 1.6f + radius;
            for (auto& b : boats) {
                if (b.kind != 0 || b.team != 0 || b.health <= 0.0f) continue;   // only ENEMY boats (no friendly fire)
                const float dx = b.pos[0]-pos[0], dz = b.pos[2]-pos[2];
                if (dx*dx + dz*dz > rr*rr) continue;
                bool done = false; for (uint32_t id : hit_ids) if (id == b.id) { done = true; break; }
                if (done) continue;
                hit_ids.push_back(b.id);
                const float dd = (damage < b.health) ? damage : b.health;
                b.health -= damage; dealt += dd;
            }
            for (auto& s : subs) {   // a SURFACED enemy sub is exposed to player fire too
                if (s.team != 0 || s.kind != 2 || s.health <= 0.0f) continue;
                const float dx = s.pos[0]-pos[0], dz = s.pos[2]-pos[2];
                if (dx*dx + dz*dz > rr*rr) continue;
                bool done = false; for (uint32_t id : hit_ids) if (id == s.id) { done = true; break; }
                if (done) continue;
                hit_ids.push_back(s.id);
                const float dd = (damage < s.health) ? damage : s.health;
                s.health -= damage; dealt += dd;
            }
            return dealt;
        };

        // Thrown sword: spin, fly out `throw_distance`, then boomerang back to the
        // player; damage enemies in its path (once per leg). Runs before the enemy
        // sim so kills/knockback are folded into this frame's update.
        if (!throwns.empty() && player.weapon) {
            const auto& w = *player.weapon;
            const float step = w.throw_speed * dt;
            for (auto& th : throwns) {
                th.spin += 22.0f * dt;                  // procedural horizontal spin
                if (!th.returning) {
                    th.pos[0] += th.dir[0] * step;       // fly straight along the 3D aim
                    th.pos[1] += th.dir[1] * step;
                    th.pos[2] += th.dir[2] * step;
                    th.traveled += step;
                    if (th.traveled >= w.throw_distance) { th.returning = true; th.hit_ids.clear(); }
                } else {
                    // Boomerang back to the player's current eye, in 3D.
                    float hx = player.position[0] - th.pos[0];
                    float hy = player.position[1] - th.pos[1];
                    float hz = player.position[2] - th.pos[2];
                    float hd = std::sqrt(hx * hx + hy * hy + hz * hz);
                    if (hd < 1.0f) th.active = false;    // caught -> sword back in hand
                    else { th.pos[0] += hx / hd * step; th.pos[1] += hy / hd * step; th.pos[2] += hz / hd * step; }
                }
                if (net.role != dc::net::Role::Client) {  // damage is host-authoritative; client flight is cosmetic
                    const float trad = w.throw_radius * w.throw_size * player.sword_scale;
                    host_damage += dc::entity::radius_attack(entities, th.pos, trad,
                                              w.throw_damage * player.damage_mult, player.stats.knockback, th.hit_ids, &frame_hits);
                    host_damage += hit_boats(th.pos, trad, w.throw_damage * player.damage_mult, th.hit_ids);
                }
            }
            // Drop swords that were caught (swap-pop; order doesn't matter for rendering).
            for (std::size_t i = 0; i < throwns.size();) {
                if (!throwns[i].active) { throwns[i] = throwns.back(); throwns.pop_back(); }
                else ++i;
            }
        }

        // Wizard staff bolts: fly straight, piercing enemies (each hit once via hit_ids), and
        // despawn at max range. Host/standalone owns the motion + damage; clients render the
        // replicated positions instead. Bolts credit their owner's damage.
        if (net.role != dc::net::Role::Client && !bolts.empty()) {
            const float step = BOLT_SPEED * dt;
            for (auto& b : bolts) {
                b.pos[0] += b.dir[0]*step; b.pos[1] += b.dir[1]*step; b.pos[2] += b.dir[2]*step;
                b.traveled += step;
                float dealt = dc::entity::radius_attack(entities, b.pos, b.radius, b.damage, b.knockback, b.hit_ids, &frame_hits);
                dealt += hit_boats(b.pos, b.radius, b.damage, b.hit_ids);   // staff bolts pepper boats too
                if (b.owner == my_id) host_damage += dealt;
                else for (auto& hc : host_clients) if (hc.id == b.owner) { hc.damage_dealt += dealt; break; }
            }
            for (std::size_t i = 0; i < bolts.size();) {
                if (bolts[i].traveled > BOLT_RANGE || bolts[i].hit_ids.size() >= 3) {   // fizzles after 3 hits
                    bolts[i] = bolts.back(); bolts.pop_back();
                } else ++i;
            }
        }

        // Orbit special: revolve + spin the swords; damage on periodic ticks (each
        // tick all swords share one hit set, so an enemy takes one hit per tick).
        if (orbit.active && player.weapon) {
            const auto& w = *player.weapon;
            orbit.time -= dt;
            if (orbit.time <= 0.0f) orbit.active = false;
            orbit.angle += 3.0f * player.orbit_spin_mult * dt;     // revolve speed (Orbit Tempo speeds this up)
            orbit.spin  += 22.0f * player.orbit_spin_mult * dt;    // each sword's own spin
            orbit.tick  -= dt;
            if (orbit.active && orbit.tick <= 0.0f) {
                orbit.tick = 0.25f * player.orbit_tick_mult;       // damage re-tick (Orbit Tempo shortens this)
                orbit.hit_ids.clear();
                for (int i = 0; i < w.orbit_count; ++i) {
                    float a = orbit.angle + (6.2831853f * i) / w.orbit_count;
                    vec3 p = { player.position[0] + std::cos(a) * w.orbit_radius, 0.0f,
                               player.position[2] + std::sin(a) * w.orbit_radius };
                    if (net.role != dc::net::Role::Client)   // host-authoritative damage
                        host_damage += dc::entity::radius_attack(entities, p, w.orbit_hit_radius * player.sword_scale,
                                                  w.orbit_damage * player.damage_mult, player.stats.knockback, orbit.hit_ids, &frame_hits);
                }
            }
        }

        // Shield bash nova: an expanding sphere; radius_attack with a persistent hit set
        // shoves each enemy once as the growing front reaches it (knockback points
        // outward from the player, so everyone flies back).
        if (bash.active && player.shield) {
            const auto& s = *player.shield;
            bash.time += dt;
            bash.radius = (bash.time / s.bash_duration) * s.bash_radius;
            if (net.role != dc::net::Role::Client) {
                vec3 c = { player.position[0], 0.0f, player.position[2] };
                host_damage += dc::entity::radius_attack(entities, c, bash.radius,
                                          s.bash_damage * player.damage_mult, s.bash_knockback, bash.hit_ids, &frame_hits);
            }
            if (bash.time >= s.bash_duration) bash.active = false;
        }

        // Each connected client's specials are HOST-RUN from their reliable *Cast
        // events: the host owns the motion + damage on its own clock (same as the local
        // player's specials), and broadcasts the state. No trust in a per-frame stream.
        if (net.role == dc::net::Role::Host) {
            for (auto& hc : host_clients) {
                // Thrown swords: fly out along each cast dir, then boomerang to the client's
                // live body; damage (2D xz) once per leg via each sword's own hit set. A
                // Swordstorm volley puts several in flight at once.
                for (auto& t : hc.throwns) {
                    t.spin += 22.0f * dt;
                    const float step = t.speed * dt;
                    bool caught = false;
                    if (!t.returning) {
                        t.pos[0] += t.dir[0]*step; t.pos[1] += t.dir[1]*step; t.pos[2] += t.dir[2]*step;
                        t.traveled += step;
                        if (t.traveled >= t.distance) { t.returning = true; t.hit_ids.clear(); }
                    } else {
                        const float bx = hc.body.position[0]-t.pos[0], by = hc.body.position[1]-t.pos[1], bz = hc.body.position[2]-t.pos[2];
                        const float bd = std::sqrt(bx*bx + by*by + bz*bz);
                        if (bd < 1.0f) caught = true;
                        else { t.pos[0]+=bx/bd*step; t.pos[1]+=by/bd*step; t.pos[2]+=bz/bd*step; }
                    }
                    if (!caught) {
                        vec3 tp = { t.pos[0], 0.0f, t.pos[2] };
                        hc.damage_dealt += dc::entity::radius_attack(entities, tp, t.radius, t.damage, t.knockback, t.hit_ids, &frame_hits);
                    } else t.speed = -1.0f;   // mark for removal
                }
                for (std::size_t i = 0; i < hc.throwns.size();) {   // drop caught swords
                    if (hc.throwns[i].speed < 0.0f) { hc.throwns[i] = hc.throwns.back(); hc.throwns.pop_back(); }
                    else ++i;
                }
                // Orbit: revolve + tick damage on the host's cadence (Orbit Tempo carried in the cast).
                if (hc.orbit_active) {
                    hc.orbit_time += dt;
                    hc.orbit_angle += 3.0f * hc.orbit_spin_mult * dt;
                    hc.orbit_spin  += 22.0f * hc.orbit_spin_mult * dt;
                    hc.orbit_tick_cd -= dt;
                    if (hc.orbit_tick_cd <= 0.0f) {
                        hc.orbit_tick_cd = hc.orbit_tick;
                        hc.orbit_hits.clear();
                        for (int i = 0; i < hc.orbit_count; ++i) {
                            float a = hc.orbit_angle + (6.2831853f * i) / hc.orbit_count;
                            vec3 op = { hc.body.position[0] + std::cos(a) * hc.orbit_radius, 0.0f,
                                        hc.body.position[2] + std::sin(a) * hc.orbit_radius };
                            hc.damage_dealt += dc::entity::radius_attack(entities, op, hc.orbit_hit_radius,
                                                      hc.orbit_damage, hc.orbit_knockback, hc.orbit_hits, &frame_hits);
                        }
                    }
                    if (hc.orbit_time >= hc.orbit_duration) hc.orbit_active = false;
                } else if (!hc.orbit_hits.empty()) {
                    hc.orbit_hits.clear();
                }
                // Bash nova (host-run from the BashCast event): expand + shove outward.
                if (hc.bash_active) {
                    hc.bash_time += dt;
                    hc.bash_radius = (hc.bash_duration > 1e-4f)
                                   ? (hc.bash_time / hc.bash_duration) * hc.bash_max_radius : 0.0f;
                    vec3 c = { hc.body.position[0], 0.0f, hc.body.position[2] };
                    hc.damage_dealt += dc::entity::radius_attack(entities, c, hc.bash_radius,
                                              hc.bash_damage, hc.bash_knockback, hc.bash_hits, &frame_hits);
                    if (hc.bash_time >= hc.bash_duration) hc.bash_active = false;
                }
            }
        }

        // Combat targets: the local player (index 0) plus every connected client
        // (host-authoritative co-op). Enemies chase the nearest of them and each
        // player's strike is resolved against the enemies in one tick.
        std::vector<dc::entity::PlayerCombat> players;
        dc::entity::PlayerCombat pc{};
        pc.id = my_id;                 // host/standalone = 0; client = its assigned id
        pc.alive = !dead;              // ghosts aren't targeted and deal no damage
        pc.invincible = player.iframes > 0.0f;   // mid-dodge: no damage lands
        glm_vec3_copy(player.position, pc.pos);
        { const float idt = dt > 1e-4f ? 1.0f / dt : 0.0f;   // velocity, so enemies lead their shots
          pc.vel[0] = (player.position[0] - player_prev[0]) * idt;
          pc.vel[1] = (player.position[1] - player_prev[1]) * idt;
          pc.vel[2] = (player.position[2] - player_prev[2]) * idt;
          glm_vec3_copy(player.position, player_prev); }
        { vec3 aim; player.front(aim); glm_vec3_copy(aim, pc.aim); }   // 3D look dir for the swing cone
        pc.yaw = player.yaw;
        pc.strike = player_strike;
        pc.strike_knockback = player.stats.knockback;
        pc.weight           = player.stats.weight;
        // Feet height above our own ground (0 standing, >0 mid-jump): drives the swing's
        // vertical reach so a jump can clip the hovering flyer.
        pc.strike_height = player.position[1]
                         - (terrain.height(player.position[0], player.position[2]) + dc::world::EYE_HEIGHT);
        // Weapon drives damage/reach/cone; fall back to fists when unarmed.
        if (player.weapon) {
            pc.strike_damage = player.stats.attack_damage + player.weapon->attack_bonus;
            pc.strike_reach  = player.weapon->reach;
            pc.strike_cos    = player.weapon->cone_cos;
        } else {
            pc.strike_damage = player.stats.attack_damage;
            pc.strike_reach  = dc::entity::UNARMED_REACH;
            pc.strike_cos    = dc::entity::UNARMED_CONE;
        }
        // The INSULTER enemy's aura saps your team's attacks by 33% within its radius. Host-side
        // (the host resolves all damage); 1.0 on clients.
        const float INSULT_AURA = 9.0f;
        auto insult_mult = [&](float x, float z) -> float {
            if (net.role == dc::net::Role::Client) return 1.0f;
            for (const auto& e : entities.items)
                if (e.alive && e.type == dc::entity::EntityType::Enemy && e.kind == dc::entity::EnemyKind::Insulter) {
                    const float dx = e.position[0]-x, dz = e.position[2]-z;
                    if (dx*dx + dz*dz < INSULT_AURA*INSULT_AURA) return 0.67f;
                }
            return 1.0f;
        };
        // The friendly BILL mob's aura: weakens ENEMY attacks by 33% near it (mirror of the
        // enemy Insulter). Applied to the damage YOUR units take below.
        auto bill_mult = [&](float x, float z) -> float {
            // Strongest nearby Bill wins; his DEF/upgrade level deepens the damage cut: -33% base,
            // -8% more per upgrade level (down to ~-73%).
            float best = 1.0f;
            for (const auto& a : allies)
                if (dc::game::mob_type(a.kind).visual == dc::game::MobVisual::Insulter) {
                    const float dx = a.pos[0]-x, dz = a.pos[2]-z;
                    if (dx*dx + dz*dz < INSULT_AURA*INSULT_AURA) {
                        const float m = std::max(0.27f, 0.67f - 0.08f * a.up);
                        if (m < best) best = m;
                    }
                }
            return best;
        };
        // Apply upgrade modifiers (red damage, blue longer+wider swing) + the insult debuff.
        pc.strike_damage *= player.damage_mult * insult_mult(player.position[0], player.position[2]);
        pc.strike_reach  += player.swing_reach_bonus;
        pc.strike_cos    -= player.swing_cone_bonus;
        if (pc.strike_cos < -0.5f) pc.strike_cos = -0.5f;   // cap arc width
        // Shield drives blocking; only mitigates once fully raised (block_ready). The
        // sim spends stamina to negate damage, so it needs our current stamina + rate.
        pc.stamina = player.stamina;
        if (player.shield) {
            pc.blocking   = block_ready;
            pc.block_cos  = player.shield->block_cos;
            pc.block_rate = player.shield->block_rate * player.stamina_mult;   // green upgrade cheapens blocking too
        } else {
            pc.blocking = false;   // no shield -> can't block
        }
        pc.crit_chance = player.crit_chance; pc.crit_mult = player.crit_mult;
        pc.burn_dps = player.fire_dps; pc.burn_duration = player.fire_duration;
        pc.slow_factor = player.ice_slow; pc.slow_duration = player.ice_duration;
        pc.earth_knock = player.earth_knock;
        pc.priority = 2.5f;   // enemies prefer the HERO over a same-range mob
        players.push_back(pc);

        // Boat boarding intent: F toggles riding a nearby friendly boat (when not in a UI/build).
        // Clients forward the edge to the host, which owns the boats and resolves it below.
        const bool board_edge = input.key_down(SDL_SCANCODE_F) && !board_prev
                                && !ui_open && !building_mode && !dead;
        board_prev = input.key_down(SDL_SCANCODE_F);

        // Client: send our input + resolved combat loadout now that pc is built (its
        // weapon/upgrade-derived stats). The host resolves our strike/block against
        // the enemies with our real stats; our pos/health come back in the snapshot.
        if (net.role == dc::net::Role::Client) {
            dc::net::InputCmd cmd;
            cmd.forward = forward; cmd.strafe = strafe; cmd.jump = jump ? 1 : 0;
            cmd.board = board_edge ? 1 : 0;
            cmd.yaw = player.yaw; cmd.pitch = player.pitch;
            cmd.strike = player_strike ? 1 : 0; cmd.blocking = block_ready ? 1 : 0;
            cmd.anim_punch = punching ? 1 : 0; cmd.anim_block = blocking ? 1 : 0;
            cmd.punch_time = punch_time; cmd.block_time = block_time;
            cmd.strike_damage = pc.strike_damage; cmd.strike_reach = pc.strike_reach;
            cmd.strike_cos = pc.strike_cos; cmd.strike_knockback = pc.strike_knockback;
            cmd.weight = pc.weight; cmd.block_cos = pc.block_cos; cmd.block_rate = pc.block_rate;
            cmd.stamina = player.stamina;
            cmd.sword_scale = player.sword_scale;
            cmd.crit_chance = player.crit_chance; cmd.crit_mult = player.crit_mult;
            cmd.health_regen = player.health_regen;
            cmd.fire_dps = player.fire_dps; cmd.fire_duration = player.fire_duration;
            cmd.slow_factor = player.ice_slow; cmd.slow_duration = player.ice_duration;
            cmd.earth_knock = player.earth_knock;
            cmd.minion_count = static_cast<uint8_t>(player.minion_count); cmd.minion_damage = player.minion_damage; cmd.minion_range = player.minion_range;
            cmd.trail_damage = player.trail_damage; cmd.trail_life = player.trail_life;
            // (Specials are no longer streamed — they're sent as reliable cast events
            // at their trigger points; the host runs + broadcasts them.)
            unsigned char buf[1 + sizeof cmd];
            buf[0] = static_cast<unsigned char>(dc::net::MsgType::Input);
            std::memcpy(buf + 1, &cmd, sizeof cmd);
            net.send_to_host(buf, sizeof buf, false);
        }

        // Each client fights with its own resolved loadout (weapon + upgrades),
        // sent in its InputCmd. Strike/block are gameplay flags from the same input.
        for (auto& hc : host_clients) {
            dc::entity::PlayerCombat cc{};
            cc.id = hc.id;
            cc.alive = hc.body.health > 0.0f;
            cc.invincible = hc.body.iframes > 0.0f;   // mid-dodge i-frames
            glm_vec3_copy(hc.body.position, cc.pos);
            { const float idt = dt > 1e-4f ? 1.0f / dt : 0.0f;   // velocity for shot leading
              cc.vel[0] = (hc.body.position[0] - hc.prev_pos[0]) * idt;
              cc.vel[1] = (hc.body.position[1] - hc.prev_pos[1]) * idt;
              cc.vel[2] = (hc.body.position[2] - hc.prev_pos[2]) * idt;
              glm_vec3_copy(hc.body.position, hc.prev_pos); }
            cc.yaw = hc.body.yaw;
            cc.strike           = hc.input.strike != 0;
            // 3D aim from the client's reported yaw+pitch.
            cc.aim[0] = std::cos(hc.input.pitch) * std::cos(hc.input.yaw);
            cc.aim[1] = std::sin(hc.input.pitch);
            cc.aim[2] = std::cos(hc.input.pitch) * std::sin(hc.input.yaw);
            cc.strike_height    = hc.body.position[1]
                                - (terrain.height(hc.body.position[0], hc.body.position[2]) + dc::world::EYE_HEIGHT);
            cc.strike_damage    = hc.input.strike_damage * insult_mult(hc.body.position[0], hc.body.position[2]);
            cc.strike_reach     = hc.input.strike_reach;
            cc.strike_cos       = hc.input.strike_cos;
            cc.strike_knockback = hc.input.strike_knockback;
            cc.weight           = hc.input.weight;
            cc.blocking         = hc.input.blocking != 0;
            cc.block_cos        = hc.input.block_cos;
            cc.block_rate       = hc.input.block_rate;
            cc.stamina          = hc.input.stamina;   // its reported stamina drives block negation
            cc.crit_chance      = hc.input.crit_chance; cc.crit_mult = hc.input.crit_mult;
            cc.burn_dps = hc.input.fire_dps; cc.burn_duration = hc.input.fire_duration;
            cc.slow_factor = hc.input.slow_factor; cc.slow_duration = hc.input.slow_duration;
            cc.earth_knock = hc.input.earth_knock;
            cc.priority = 2.5f;   // client heroes are preferred targets too
            players.push_back(cc);
        }

        // Friendly lane mobs join the combat list as PlayerCombat entries: enemies path to and
        // attack them (hits mapped back below), and their `strike` cone damages enemies through
        // the same resolution the players use. They march toward the enemy base and chip its core.
        int ally_start = -1;
        if (net.role != dc::net::Role::Client && !allies.empty()) {
            ally_start = static_cast<int>(players.size());
            for (std::size_t i = 0; i < allies.size(); ++i) {
                Ally& a = allies[i];
                const dc::game::MobType& mt = dc::game::mob_type(a.kind);
                // Find the nearest live enemy within aggro range (for combat / self-defense).
                dc::entity::Entity* tgt = nullptr; float bd2 = dc::game::ALLY_AGGRO * dc::game::ALLY_AGGRO;
                for (auto& e : entities.items) {
                    if (e.type != dc::entity::EntityType::Enemy || !e.alive) continue;
                    const float dx = e.position[0]-a.pos[0], dz = e.position[2]-a.pos[2], d2 = dx*dx+dz*dz;
                    if (d2 < bd2) { bd2 = d2; tgt = &e; }
                }
                // Once we've broken INTO the enemy base, commit to wrecking the core — ignore
                // skirmishing mobs (so a push doesn't stall at their doorstep).
                const float ecx = enemy_core_pos[0]-a.pos[0], ecz = enemy_core_pos[2]-a.pos[2];
                const float ebound = dc::game::mob_type(a.kind).scavenger ? 0.0f : (ENEMY_TURRET_RING + 2.5f);
                if (ecx*ecx + ecz*ecz <= ebound*ebound) tgt = nullptr;   // inside their base -> attack the core
                // Pick where to go. Fighters chase the enemy, else march on the enemy base.
                // Scavengers ignore the lane and roam to the nearest dropped coin (they only
                // fight if an enemy is right next to them — decent self-defense).
                float tx, tz; bool march_core = false;
                if (mt.scavenger) {
                    const Coin* nc = nullptr; float cb2 = 1e18f;
                    for (const auto& cn : coins) {
                        const float dx = cn.pos[0]-a.pos[0], dz = cn.pos[2]-a.pos[2], d2 = dx*dx+dz*dz;
                        if (d2 < cb2) { cb2 = d2; nc = &cn; }
                    }
                    if (nc) { tx = nc->pos[0]; tz = nc->pos[2]; }
                    else if (tgt && bd2 < 9.0f) { tx = tgt->position[0]; tz = tgt->position[2]; }   // defend if cornered
                    else { tx = a.pos[0] + 2.0f; tz = a.pos[2]; }   // idle drift if nothing to grab
                } else if (tgt) { tx = tgt->position[0]; tz = tgt->position[2]; }
                else if (a.kind < dc::game::MOB_TYPE_COUNT && type_hold_x[a.kind] >= 0.0f) {
                    // Command-map HOLD: advance to the type's front-line X, then sit (still fight
                    // anything that wanders into range).
                    tx = type_hold_x[a.kind]; tz = a.pos[2];
                }
                else if (rally_active) { tx = rally_pos[0]; tz = rally_pos[2]; }   // hold the rally point
                else { tx = enemy_core_pos[0]; tz = enemy_core_pos[2]; march_core = true; }
                float dx = tx - a.pos[0], dz = tz - a.pos[2];
                float d = std::sqrt(dx*dx + dz*dz); if (d < 1e-4f) { dx = 1.0f; dz = 0.0f; d = 1.0f; }
                const float hx = dx / d, hz = dz / d;
                const float reach = mt.reach;   // per-type (mage/flier zap from afar)
                // Gentle lateral WANDER so the army doesn't march in a perfectly straight line —
                // rotate the heading by a slow per-mob sine while traveling (not when a target's in reach).
                float shx = hx, shz = hz;
                if (!tgt || d > reach + 1.0f) {
                    const float wob = std::sin(t_now*1.0f + static_cast<float>(i)*2.399f) * 0.30f
                                    + std::sin(t_now*0.37f + static_cast<float>(i)) * 0.12f;
                    const float cw = std::cos(wob), sw = std::sin(wob);
                    shx = hx*cw - hz*sw; shz = hx*sw + hz*cw;
                }
                a.yaw = std::atan2(shz, shx);
                // Stand OFF the enemy core (don't walk into the tower): stop at its radius + reach.
                const float stop = march_core ? (CORE_RAD + reach * 0.6f)
                                  : ((!mt.scavenger && tgt) ? reach : 0.4f);
                a.moving = (d > stop);
                if (d > stop) {
                    const float smul = in_slime(a.pos[0], a.pos[2]) ? SLIME_SLOW : 1.0f;   // your mobs mired by enemy slime
                    const float step = std::min(dc::game::ALLY_SPEED * a.speed_mul * smul * mt.speed * dt, d - stop);   // mt.speed: knights are slow
                    a.pos[0] += shx * step; a.pos[2] += shz * step;
                }
                // Separation (boids): push apart from crowded same-team neighbors so the army
                // spreads into a loose front instead of collapsing into a single-file line. Fliers
                // only separate from other fliers (they live at a different height than ground mobs).
                {
                    const float SEP_R = 1.7f * a.size_mul;
                    float sepx = 0.0f, sepz = 0.0f;
                    for (std::size_t j = 0; j < allies.size(); ++j) {
                        if (j == i) continue;
                        if (dc::game::mob_type(allies[j].kind).flies != mt.flies) continue;
                        const float ox = a.pos[0]-allies[j].pos[0], oz = a.pos[2]-allies[j].pos[2];
                        const float od2 = ox*ox + oz*oz;
                        if (od2 < SEP_R*SEP_R && od2 > 1e-5f) {
                            const float od = std::sqrt(od2), w = (SEP_R - od) / SEP_R;
                            sepx += (ox/od) * w; sepz += (oz/od) * w;
                        }
                    }
                    const float sm = std::sqrt(sepx*sepx + sepz*sepz);
                    if (sm > 1e-4f) {
                        if (sm > 1.0f) { sepx /= sm; sepz /= sm; }   // cap so a dense blob doesn't fling
                        const float sep_step = dc::game::ALLY_SPEED * a.speed_mul * mt.speed * dt * 0.85f;
                        a.pos[0] += sepx * sep_step; a.pos[2] += sepz * sep_step;
                    }
                }
                // Fliers (flier/bat) hover; everyone else stands on the ground.
                a.pos[1] = terrain.height(a.pos[0], a.pos[2]) + dc::world::EYE_HEIGHT + (mt.flies ? 1.6f : 0.0f);
                if (a.attack_cd > 0.0f) a.attack_cd -= dt;
                const bool is_knight = (mt.visual == dc::game::MobVisual::Knight);
                // Strike an enemy in reach (resolved via the cone below), or — for fighters with
                // no enemy near — chip the enemy core when adjacent to it. The KNIGHT instead winds
                // up a rear-and-SLAM: a telegraphed AoE that lands a moment later (handled below).
                bool striking = false;
                const float ereach = (tgt) ? std::sqrt(bd2) : 1e9f;
                const float ecx0 = enemy_core_pos[0]-a.pos[0], ecz0 = enemy_core_pos[2]-a.pos[2];
                const bool core_in_reach = march_core && (ecx0*ecx0 + ecz0*ecz0 <= (reach + CORE_RAD + 0.5f)*(reach + CORE_RAD + 0.5f)) && enemy_core_health > 0.0f;
                if (a.attack_cd <= 0.0f) {
                    const bool in_strike = (tgt && ereach <= reach + 0.4f) || core_in_reach;
                    if (is_knight) {
                        if (in_strike && a.slam < 0.0f) { a.slam = 0.30f; a.atk = 1.0f; a.attack_cd = dc::game::ALLY_ATTACK_CD * 1.6f; }  // rear up; contact in 0.30s
                    } else if (tgt && ereach <= reach + 0.4f) { striking = true; a.attack_cd = dc::game::ALLY_ATTACK_CD; }
                    else if (core_in_reach) {
                        enemy_core_health -= mt.damage; if (enemy_core_health < 0.0f) enemy_core_health = 0.0f;
                        a.attack_cd = dc::game::ALLY_ATTACK_CD;
                    }
                }
                // Knight rear-and-slam: advance the windup; on CONTACT deal a big AoE + knockback +
                // explosion at the hooves (and a heavy chunk to the core if we're battering it).
                if (a.atk > 0.0f) a.atk = std::max(0.0f, a.atk - dt / 0.55f);
                if (a.slam >= 0.0f) {
                    a.slam -= dt;
                    if (a.slam < 0.0f) {
                        const float fx = a.pos[0] + std::cos(a.yaw)*1.7f, fz = a.pos[2] + std::sin(a.yaw)*1.7f;
                        vec3 fp = { fx, terrain.height(fx, fz), fz };
                        std::vector<uint32_t> hitset;
                        const float dmg = mt.damage * 2.0f * insult_mult(a.pos[0], a.pos[2]);   // MASSIVE
                        host_damage += dc::entity::radius_attack(entities, fp, 3.4f, dmg, 42.0f, hitset, &frame_hits);
                        if (march_core && enemy_core_health > 0.0f) {
                            const float cx = enemy_core_pos[0]-a.pos[0], cz = enemy_core_pos[2]-a.pos[2];
                            if (cx*cx+cz*cz <= (reach + CORE_RAD + 1.5f)*(reach + CORE_RAD + 1.5f)) {
                                enemy_core_health -= dmg; if (enemy_core_health < 0.0f) enemy_core_health = 0.0f;
                            }
                        }
                        frame_booms.push_back(fp[0]); frame_booms.push_back(fp[1]+0.3f); frame_booms.push_back(fp[2]);
                    }
                }
                dc::entity::PlayerCombat ac{};
                ac.id = ALLY_ID_BASE + static_cast<uint32_t>(i);
                ac.alive = true; ac.invincible = false;
                glm_vec3_copy(a.pos, ac.pos);
                ac.weight = 3.0f; ac.block_rate = 0.0f;
                ac.strike = striking;
                ac.strike_damage = mt.damage * insult_mult(a.pos[0], a.pos[2]);   // heckled mobs hit softer
                ac.strike_reach = reach;
                ac.strike_cos = -0.3f; ac.strike_knockback = 4.0f; ac.crit_mult = 1.0f;
                ac.taunt = (mt.visual == dc::game::MobVisual::Insulter);   // Bill grabs aggro off nearby enemies
                if (tgt) { const float ex = tgt->position[0]-a.pos[0], ez = tgt->position[2]-a.pos[2];
                           const float el = std::sqrt(ex*ex+ez*ez); ac.aim[0] = el>1e-4f?ex/el:1.0f; ac.aim[2] = el>1e-4f?ez/el:0.0f; }
                else { ac.aim[0] = dx/d; ac.aim[2] = dz/d; }
                ac.aim[1] = 0.0f;
                players.push_back(ac);
            }
        }

        // The core is ALWAYS a target now (the lane war never pauses): enemies path to + attack
        // it like a player (closest-first, retarget-on-hit), but it never strikes back. Night is
        // purely cosmetic. Host owns the sim, so only add it there.
        int core_index = -1;
        if (net.role != dc::net::Role::Client && core_health > 0.0f) {
            dc::entity::PlayerCombat cc{};
            cc.id = CORE_ID; cc.alive = true; cc.invincible = false;
            cc.pos[0] = core_pos[0]; cc.pos[1] = core_pos[1] + 1.5f; cc.pos[2] = core_pos[2];
            cc.weight = 1e9f;          // immovable
            cc.block_rate = 0.0f;      // can't block
            cc.strike = false;         // never attacks
            core_index = static_cast<int>(players.size());
            players.push_back(cc);
        }

        // Friendly WARSHIPS join the target list so enemy mobs aggro + attack them (their hits map
        // back to the boat's HP below). Boats are big, heavy targets — they soak a LOT.
        int boat_start = -1; std::vector<uint32_t> boat_combat_ids;
        if (net.role != dc::net::Role::Client) {
            boat_start = static_cast<int>(players.size());
            for (auto& b : boats) if (b.team == 1 && b.kind == 0 && b.health > 0.0f) {
                dc::entity::PlayerCombat cc{};
                cc.id = 0xB0A70000u + b.id; cc.alive = true; cc.invincible = false;
                cc.pos[0] = b.pos[0]; cc.pos[1] = b.pos[1] + 1.0f; cc.pos[2] = b.pos[2];
                cc.weight = 80.0f;        // heavy: shrugs off knockback
                cc.block_rate = 0.0f; cc.strike = false;   // it fights with cannons, not this melee system
                players.push_back(cc);
                boat_combat_ids.push_back(b.id);
            }
        }

        // Whoever is pushing FURTHEST into the enemy base (highest x, excluding the immovable core)
        // pulls extra aggro — and especially so if it's the hero. Satisfies "target me when I'm
        // closest to their base" + "prefer the player".
        if (net.role != dc::net::Role::Client) {
            const int num_heroes = 1 + static_cast<int>(host_clients.size());
            int front = -1; float frontx = -1e30f;
            for (std::size_t i = 0; i < players.size(); ++i) {
                if (!players[i].alive || players[i].weight > 1e8f) continue;   // skip the core
                if (players[i].pos[0] > frontx) { frontx = players[i].pos[0]; front = static_cast<int>(i); }
            }
            if (front >= 0) players[front].priority = (front < num_heroes) ? 6.0f : 2.0f;
        }

        // One flow field per player (parallel to `players`), so an enemy can path to
        // its committed target — not just whoever's nearest. A handful of small BFS;
        // cheap at these player counts.
        std::vector<dc::world::FlowField> flows;
        flows.reserve(players.size());
        for (auto& p : players) {
            int gc = static_cast<int>(p.pos[0] / dc::world::TILE);
            int gr = static_cast<int>(p.pos[2] / dc::world::TILE);
            flows.push_back(dc::world::compute_flow(*map, gc, gr, &tile_heights, dc::entity::ENEMY_MAX_CLIMB));
        }

        // Enemy sim is host-authoritative; clients render replicated enemies instead.
        frame_deaths.clear();
        frame_death_xp.clear();
        frame_death_gold.clear();
        frame_booms.clear();
        std::vector<dc::entity::EnemyHitPlayer> hits;
        if (net.role != dc::net::Role::Client) {
            // Friendly Insulter aura ALSO lowers nearby enemies' DEFENSE — they take more damage
            // (mirror of how an enemy Insulter weakens your attacks). Strongest nearby Bill wins.
            for (auto& e : entities.items) {
                if (!e.alive || e.type != dc::entity::EntityType::Enemy) continue;
                float m = 1.0f;
                for (const auto& a : allies)
                    if (dc::game::mob_type(a.kind).visual == dc::game::MobVisual::Insulter) {
                        const float dx = a.pos[0]-e.position[0], dz = a.pos[2]-e.position[2];
                        if (dx*dx + dz*dz < INSULT_AURA*INSULT_AURA) {
                            const float mm = std::min(2.2f, 1.40f + 0.08f * a.up);   // +40% base, +8%/upgrade level
                            if (mm > m) m = mm;
                        }
                    }
                e.dmg_taken_mult = m;
            }
            dc::entity::update_enemies(entities, *map, flows, players, hits, dt, &frame_deaths, &frame_hits, &tile_heights, &frame_death_xp, &frame_death_gold, enemy_speed_mult);
            // Advance ranged enemies' shots; their hits add into the same `hits`.
            dc::entity::update_projectiles(entities, *map, players, hits, dt, &frame_booms);

            // --- Enemy BOATS: patrol the river, lob exploding cannonballs at players + allies.
            // They only travel in water (constrained to the channel) and bob on the surface.
            {
                // (Enemy boats/subs are now PURCHASED by the economy AI below, not free-spawned here.)
                // --- Boat boarding (host-authoritative). `who`: 0 = host-local player, else a client id.
                auto eject_rider = [&](Boat& b) {
                    if (b.rider < 0) return;
                    float ex = b.pos[0], ez = b.pos[2];   // step off onto the nearest dry bank
                    for (float off = 1.0f; off <= 9.0f; off += 1.0f) {
                        if (!in_water(b.pos[0], b.pos[2]+off)) { ez = b.pos[2]+off; break; }
                        if (!in_water(b.pos[0], b.pos[2]-off)) { ez = b.pos[2]-off; break; }
                    }
                    const float ey = terrain.height(ex, ez) + dc::world::EYE_HEIGHT;
                    if (b.rider == 0) { player.position[0]=ex; player.position[1]=ey; player.position[2]=ez; }
                    else for (auto& hc : host_clients) if ((int)hc.id == b.rider) { hc.body.position[0]=ex; hc.body.position[1]=ey; hc.body.position[2]=ez; break; }
                    b.rider = -1;
                };
                auto try_board = [&](int who, bool edge, float px, float pz) {
                    if (!edge) return;
                    for (auto& b : boats) if (b.rider == who) { eject_rider(b); return; }   // already aboard -> dismount
                    Boat* best = nullptr; float bd = BOARD_RANGE * BOARD_RANGE;
                    for (auto& b : boats) if (b.team == 1 && b.rider < 0 && b.health > 0.0f) {
                        const float dx = b.pos[0]-px, dz = b.pos[2]-pz, d2 = dx*dx+dz*dz;
                        if (d2 < bd) { bd = d2; best = &b; }
                    }
                    if (best) best->rider = who;
                };
                try_board(0, board_edge, player.position[0], player.position[2]);
                for (auto& hc : host_clients) {
                    const bool e = hc.input.board && !hc.board_prev; hc.board_prev = hc.input.board != 0;
                    try_board((int)hc.id, e, hc.body.position[0], hc.body.position[2]);
                }
                const float lay_near = river_x0 + 4.0f, lay_far = enemy_core_pos[0] - 12.0f, lay_mid = (lay_near+lay_far)*0.5f;
                for (auto& b : boats) {
                    if (b.role == 1) {
                        // MINELAYER: patrols the river dropping mines that blow up the OTHER team's
                        // warships. (Slow, no guns — kill it with a warship, or sweep its mines.)
                        if (b.surf < lay_near || b.surf > lay_far) b.surf = lay_mid;   // roam target x
                        const float tx = b.surf, tz = channel_center(tx);
                        float dx = tx-b.pos[0], dz = tz-b.pos[2]; const float d = std::sqrt(dx*dx+dz*dz);
                        if (d < 2.0f) b.surf = (b.surf < lay_mid) ? lay_far : lay_near;   // reached -> head to the far end
                        else { const float step = dc::game::MINELAYER_SPEED*dt; const float nx=b.pos[0]+dx/d*step, nz=b.pos[2]+dz/d*step;
                            if (in_water(nx,nz)) { b.pos[0]=nx; b.pos[2]=nz; } else b.pos[2]+=(channel_center(b.pos[0])-b.pos[2])*std::min(1.0f,step);
                            b.yaw = std::atan2(dz,dx); }
                        b.pos[1] = terrain.height(b.pos[0], b.pos[2]);
                        if (b.fire_cd > 0.0f) b.fire_cd -= dt;
                        if (b.fire_cd <= 0.0f) {
                            int tmines = 0; for (auto& m : naval_mines) if (m.team == b.team) ++tmines;
                            if (tmines < dc::game::MINE_CAP_PER_TEAM) {
                                NavalMine m; m.id = next_mine_id++; m.team = b.team; m.arm = 0.0f;
                                m.pos[0]=b.pos[0]; m.pos[2]=b.pos[2]; m.pos[1]=terrain.height(b.pos[0],b.pos[2]);
                                naval_mines.push_back(m);
                            }
                            b.fire_cd = dc::game::MINELAYER_DROP_CD;
                        }
                        continue;
                    }
                    if (b.role == 2) {
                        // MINESWEEPER: a kamikaze rowboat that drives into the ENEMY team's mines (the
                        // mine then detonates on it — clearing the mine at the cost of this cheap hull).
                        const uint8_t opp = b.team == 1 ? 0 : 1;
                        NavalMine* tgt = nullptr; float bd2 = 1e18f;
                        for (auto& m : naval_mines) if (m.team == opp && m.arm >= 0.0f) {
                            const float dx=m.pos[0]-b.pos[0], dz=m.pos[2]-b.pos[2], d2=dx*dx+dz*dz;
                            if (d2 < bd2) { bd2 = d2; tgt = &m; } }
                        float tx, tz;
                        if (tgt) { tx = tgt->pos[0]; tz = tgt->pos[2]; }
                        else { tx = (b.team==1) ? lay_far : lay_near; tz = channel_center(tx); }   // none to sweep: drift forward
                        float dx = tx-b.pos[0], dz = tz-b.pos[2]; const float d = std::sqrt(dx*dx+dz*dz);
                        if (d > 0.1f) { const float step = dc::game::MINESWEEPER_SPEED*dt; const float nx=b.pos[0]+dx/d*step, nz=b.pos[2]+dz/d*step;
                            if (in_water(nx,nz)) { b.pos[0]=nx; b.pos[2]=nz; } else b.pos[2]+=(channel_center(b.pos[0])-b.pos[2])*std::min(1.0f,step);
                            b.yaw = std::atan2(dz,dx); }
                        b.pos[1] = terrain.height(b.pos[0], b.pos[2]);
                        continue;
                    }
                    if (b.team == 1) {
                        // FRIENDLY warship: if a player is RIDING it they steer with WASD (and the
                        // cannons still auto-fire below); otherwise it auto-hunts the nearest ENEMY
                        // boat, else advances on the enemy base and shells its core.
                        Boat* eb = nullptr; float ed2 = BOAT_RANGE * BOAT_RANGE;
                        for (auto& o : boats) if (o.team == 0 && o.health > 0.0f) {
                            const float ex = o.pos[0]-b.pos[0], ez = o.pos[2]-b.pos[2], e2 = ex*ex+ez*ez;
                            if (e2 < ed2) { ed2 = e2; eb = &o; } }
                        if (b.rider >= 0) {
                            // Steer from the rider's movement intent + look yaw (host-local or a client).
                            float rf = 0.0f, rs = 0.0f, ryaw = b.yaw;
                            if (b.rider == 0) { rf = forward; rs = strafe; ryaw = player.yaw; }
                            else for (auto& hc : host_clients) if ((int)hc.id == b.rider) { rf = hc.input.forward; rs = hc.input.strafe; ryaw = hc.input.yaw; break; }
                            const float wx = std::cos(ryaw), wz = std::sin(ryaw);     // walk dir
                            const float rgx = -std::sin(ryaw), rgz = std::cos(ryaw);  // right dir
                            float mx = wx*rf + rgx*rs, mz = wz*rf + rgz*rs;
                            const float ml = std::sqrt(mx*mx+mz*mz);
                            if (ml > 0.05f) {
                                mx/=ml; mz/=ml;
                                const float step = BOAT_SPEED * 2.4f * dt;   // riders sail noticeably faster
                                const float nx = b.pos[0]+mx*step, nz = b.pos[2]+mz*step;
                                if (in_water(nx, nz))            { b.pos[0]=nx; b.pos[2]=nz; }
                                else if (in_water(nx, b.pos[2])) { b.pos[0]=nx; }   // slide along the bank
                                else if (in_water(b.pos[0], nz)) { b.pos[2]=nz; }
                                b.yaw = std::atan2(mz, mx);
                            }
                            b.pos[1] = terrain.height(b.pos[0], b.pos[2]);
                            // Pin the rider's avatar onto the deck (host-authoritative; replicates to all).
                            const float deck_eye = b.pos[1] + 1.1f + dc::world::EYE_HEIGHT;
                            if (b.rider == 0) { player.position[0]=b.pos[0]; player.position[1]=deck_eye; player.position[2]=b.pos[2]; }
                            else for (auto& hc : host_clients) if ((int)hc.id == b.rider) { hc.body.position[0]=b.pos[0]; hc.body.position[1]=deck_eye; hc.body.position[2]=b.pos[2]; break; }
                        } else {
                            float tx, tz;
                            if (eb) { tx = eb->pos[0]; tz = eb->pos[2]; b.yaw = std::atan2(tz-b.pos[2], tx-b.pos[0]); }
                            else {   // advance toward the enemy end of the river
                                tx = std::min(enemy_core_pos[0]-10.0f, b.pos[0]+6.0f); tz = channel_center(tx);
                                float dx=tx-b.pos[0], dz=tz-b.pos[2]; const float d=std::sqrt(dx*dx+dz*dz);
                                if (d>0.1f) { const float step=BOAT_SPEED*dt; const float nx=b.pos[0]+dx/d*step, nz=b.pos[2]+dz/d*step;
                                    if (in_water(nx,nz)) { b.pos[0]=nx; b.pos[2]=nz; } else b.pos[2]+=(channel_center(b.pos[0])-b.pos[2])*std::min(1.0f,step);
                                    b.yaw = std::atan2(dz,dx); }
                            }
                            b.pos[1] = terrain.height(b.pos[0], b.pos[2]);
                        }
                        if (b.fire_cd > 0.0f) b.fire_cd -= dt;
                        if (b.fire_cd <= 0.0f) {
                            float shot_x = 0, shot_y = 0, shot_z = 0; bool fired = false;
                            if (eb && ed2 <= BOAT_RANGE*BOAT_RANGE) {
                                eb->health -= 50.0f; b.fire_cd = BOAT_FIRE_CD;
                                shot_x = eb->pos[0]; shot_y = eb->pos[1]+1.0f; shot_z = eb->pos[2]; fired = true;
                            } else {   // no enemy boat: shell the nearest enemy MOB in range, else the core
                                dc::entity::Entity* em = nullptr; float emd2 = BOAT_RANGE*BOAT_RANGE;
                                for (auto& e : entities.items) if (e.alive && e.type == dc::entity::EntityType::Enemy) {
                                    const float ex=e.position[0]-b.pos[0], ez=e.position[2]-b.pos[2], e2=ex*ex+ez*ez;
                                    if (e2 < emd2) { emd2 = e2; em = &e; } }
                                if (em) {
                                    em->health -= 45.0f; b.fire_cd = BOAT_FIRE_CD;
                                    shot_x = em->position[0]; shot_y = em->position[1]+0.8f; shot_z = em->position[2]; fired = true;
                                } else {
                                    const float cx = enemy_core_pos[0]-b.pos[0], cz = enemy_core_pos[2]-b.pos[2];
                                    if (cx*cx+cz*cz <= (BOAT_RANGE)*(BOAT_RANGE) && enemy_core_health > 0.0f) {
                                        enemy_core_health -= 30.0f; if (enemy_core_health<0) enemy_core_health=0; b.fire_cd = BOAT_FIRE_CD;
                                        shot_x = enemy_core_pos[0]; shot_y = enemy_core_pos[1]+1.5f; shot_z = enemy_core_pos[2]; fired = true;
                                    }
                                }
                            }
                            if (fired) {   // visible tracer shell from the forward cannon MUZZLE toward the target
                                const float mca=std::cos(b.yaw), msa=std::sin(b.yaw);
                                TBullet tb; tb.pos[0]=b.pos[0]+mca*1.7f; tb.pos[1]=b.pos[1]+1.3f; tb.pos[2]=b.pos[2]+msa*1.7f;
                                float vx=shot_x-tb.pos[0], vy=shot_y-tb.pos[1], vz=shot_z-tb.pos[2];
                                float vl=std::sqrt(vx*vx+vy*vy+vz*vz); if (vl<1e-3f) vl=1.0f;
                                const float spd=45.0f; tb.vel[0]=vx/vl*spd; tb.vel[1]=vy/vl*spd; tb.vel[2]=vz/vl*spd; tb.life=1.0f; tb.red=false;
                                turret_bullets.push_back(tb);
                            }
                        }
                        // Depth-charge any SURFACED enemy sub in range (mirror of enemy boats vs your subs).
                        for (auto& es : subs) if (es.team == 0 && es.kind == 2) {
                            const float ex = es.pos[0]-b.pos[0], ez = es.pos[2]-b.pos[2];
                            if (ex*ex + ez*ez < BOAT_RANGE*BOAT_RANGE) es.health -= 40.0f * dt;
                        }
                        continue;
                    }
                    // Find the nearest target in range FIRST — a boat HOLDS and bombards while a
                    // target is near, and only creeps forward down the river when the coast is clear.
                    const dc::entity::PlayerCombat* tgt = nullptr; float bd2 = BOAT_RANGE * BOAT_RANGE;
                    for (auto& p : players) { if (!p.alive) continue;
                        const float ex = p.pos[0]-b.pos[0], ez = p.pos[2]-b.pos[2], e2 = ex*ex+ez*ez;
                        if (e2 < bd2) { bd2 = e2; tgt = &p; } }
                    if (tgt) {
                        // Hold position; turn the HULL to face the target (cannons aim along it).
                        b.yaw = std::atan2(tgt->pos[2]-b.pos[2], tgt->pos[0]-b.pos[0]);
                    } else {
                        // No target: creep slowly toward the player base, hugging the channel.
                        const float tx = std::max(river_x0 + 2.0f, b.pos[0] - 6.0f);
                        const float tz = channel_center(tx);
                        float dx = tx - b.pos[0], dz = tz - b.pos[2];
                        const float d = std::sqrt(dx*dx + dz*dz);
                        if (d > 0.1f) {
                            const float step = BOAT_SPEED * dt;
                            const float nx = b.pos[0] + dx/d*step, nz = b.pos[2] + dz/d*step;
                            if (in_water(nx, nz)) { b.pos[0] = nx; b.pos[2] = nz; }
                            else b.pos[2] += (channel_center(b.pos[0]) - b.pos[2]) * std::min(1.0f, step);
                            b.yaw = std::atan2(dz, dx);
                        }
                    }
                    b.pos[1] = terrain.height(b.pos[0], b.pos[2]);
                    // Melee: the player's swing connects if the boat is in reach this frame.
                    if (player_strike) {
                        const float ex = b.pos[0]-player.position[0], ez = b.pos[2]-player.position[2];
                        if (ex*ex + ez*ez < (pc.strike_reach + 1.8f)*(pc.strike_reach + 1.8f)) {
                            b.health -= pc.strike_damage; host_damage += pc.strike_damage;
                        }
                    }
                    // Nearby friendly mobs hack at the hull (continuous chip).
                    for (const auto& a : allies) {
                        const float ex = b.pos[0]-a.pos[0], ez = b.pos[2]-a.pos[2];
                        if (ex*ex + ez*ez < 2.6f*2.6f) b.health -= dc::game::mob_type(a.kind).damage * dt;
                    }
                    // Trade broadsides with friendly warships in range (two-way naval combat).
                    for (auto& fb : boats) if (fb.team == 1) {
                        const float ex = fb.pos[0]-b.pos[0], ez = fb.pos[2]-b.pos[2];
                        if (ex*ex + ez*ez < BOAT_RANGE*BOAT_RANGE) fb.health -= 22.0f * dt;
                    }
                    // A SURFACED sub (kind 2) is exposed — enemy boats hammer it; submerged subs are untouchable.
                    for (auto& s : subs) if (s.kind == 2) {
                        const float ex = s.pos[0]-b.pos[0], ez = s.pos[2]-b.pos[2];
                        if (ex*ex + ez*ez < BOAT_RANGE*BOAT_RANGE) s.health -= 40.0f * dt;
                    }
                    // Fire an exploding cannonball at the nearest player/ally/core in range.
                    if (b.fire_cd > 0.0f) b.fire_cd -= dt;
                    if (b.fire_cd <= 0.0f) {
                        const dc::entity::PlayerCombat* tgt = nullptr; float bd2 = BOAT_RANGE * BOAT_RANGE;
                        for (auto& p : players) { if (!p.alive) continue;
                            const float ex = p.pos[0]-b.pos[0], ez = p.pos[2]-b.pos[2], e2 = ex*ex+ez*ez;
                            if (e2 < bd2) { bd2 = e2; tgt = &p; } }
                        if (tgt) {
                            b.fire_cd = BOAT_FIRE_CD;
                            b.yaw = std::atan2(tgt->pos[2]-b.pos[2], tgt->pos[0]-b.pos[0]);   // train the bow cannon on the target
                            const float mca=std::cos(b.yaw), msa=std::sin(b.yaw);
                            dc::entity::Projectile pr;
                            pr.pos[0]=b.pos[0]+mca*1.7f; pr.pos[1]=b.pos[1]+1.3f; pr.pos[2]=b.pos[2]+msa*1.7f;   // from the forward muzzle
                            float ex=tgt->pos[0]-pr.pos[0], ey=(tgt->pos[1])-pr.pos[1], ez=tgt->pos[2]-pr.pos[2];
                            float el=std::sqrt(ex*ex+ey*ey+ez*ez); if (el<1e-3f) el=1.0f;
                            const float spd = 24.0f;
                            pr.vel[0]=ex/el*spd; pr.vel[1]=ey/el*spd; pr.vel[2]=ez/el*spd;
                            pr.damage=42.0f; pr.knockback=20.0f; pr.life=2.6f; pr.radius=0.7f;
                            pr.explodes=true; pr.blast=4.5f; pr.owner_id=b.id;
                            pr.color[0]=0.15f; pr.color[1]=0.15f; pr.color[2]=0.18f;   // dark iron ball
                            entities.projectiles.push_back(pr);
                        }
                    }
                }
                // Cull sunk boats (a death burst of water + smoke). Toss any rider into the drink first.
                for (std::size_t i = 0; i < boats.size();) {
                    if (boats[i].health <= 0.0f) {
                        eject_rider(boats[i]);
                        frame_booms.push_back(boats[i].pos[0]); frame_booms.push_back(boats[i].pos[1]+0.5f); frame_booms.push_back(boats[i].pos[2]);
                        boats[i] = boats.back(); boats.pop_back();
                    } else ++i;
                }
            }

            // --- SLIME trails: each living slime drops a puddle on a timer; a slime that died
            // this frame bursts into a BIG puddle. Patches age out. (Slow applied to player +
            // allies below; enemies are immune.) ---
            {
                std::unordered_map<uint32_t, SlimeTrack> next;
                for (auto& e : entities.items) {
                    if (e.type != dc::entity::EntityType::Enemy || !e.alive || e.kind != dc::entity::EnemyKind::Slime) continue;
                    auto it = slime_track.find(e.id);
                    float cd = (it != slime_track.end()) ? it->second.cd - dt : 0.0f;
                    if (cd <= 0.0f) {
                        slime_patches.push_back({ { e.position[0], terrain.height(e.position[0], e.position[2]), e.position[2] }, 2.6f, 7.0f, 7.0f });
                        cd = 0.45f;
                    }
                    next[e.id] = { cd, e.position[0], e.position[2] };
                }
                // Deaths: slimes tracked last frame but gone now -> big burst at their last spot.
                for (auto& kv : slime_track)
                    if (next.find(kv.first) == next.end())
                        slime_patches.push_back({ { kv.second.x, terrain.height(kv.second.x, kv.second.z), kv.second.z }, 5.5f, 12.0f, 12.0f });
                slime_track.swap(next);
                // Age out + cap.
                for (std::size_t i = 0; i < slime_patches.size();) {
                    slime_patches[i].life -= dt;
                    if (slime_patches[i].life <= 0.0f) { slime_patches[i] = slime_patches.back(); slime_patches.pop_back(); }
                    else ++i;
                }
                if (slime_patches.size() > 400) slime_patches.erase(slime_patches.begin(), slime_patches.begin() + (slime_patches.size() - 400));
            }
            // (Water no longer slows enemies — simplified. The river is visual + the boats' domain.)
            // NOTE: the enemy base can ONLY be destroyed by your MOBS pushing the lane — the
            // hero player clears enemies but can't damage the tower directly.
            // When a player gets hit, the nearest enemy gloats with a reactive line (the
            // enemy "that dealt it"). Per-player cooldown + the global cap keep it sane.
            auto react_for = [&](int pi, uint32_t attacker_id, float px, float pz) {
                if (pi >= 16 || react_cd[pi] > 0.0f || static_cast<int>(taunts.size()) >= MAX_TAUNTS) return;
                int best = -1;
                // Prefer the exact enemy that dealt it (melee, flame, OR the ranged shooter).
                for (std::size_t e = 0; e < entities.items.size(); ++e)
                    if (entities.items[e].type == dc::entity::EntityType::Enemy && entities.items[e].id == attacker_id) { best = static_cast<int>(e); break; }
                if (best < 0) {   // fallback: nearest enemy (attacker already despawned)
                    float bd = 14.0f * 14.0f;
                    for (std::size_t e = 0; e < entities.items.size(); ++e) {
                        const auto& en = entities.items[e];
                        if (en.type != dc::entity::EntityType::Enemy) continue;
                        const float dx = en.position[0] - px, dz = en.position[2] - pz, d2 = dx*dx + dz*dz;
                        if (d2 < bd) { bd = d2; best = static_cast<int>(e); }
                    }
                }
                if (best < 0) return;
                const auto& en = entities.items[best];
                const float hy = terrain.height(en.position[0], en.position[2])
                               + (en.kind == dc::entity::EnemyKind::Flying ? dc::entity::FLY_HOVER + 1.4f : 2.4f);
                // The hit player's own custom insults may get thrown back at them.
                const uint32_t pid = (pi == 0) ? my_id : host_clients[pi - 1].id;
                spawn_taunt(en.position[0], hy, en.position[2], pick_line(true, pid), true);
                react_cd[pi] = 2.2f;
            };
            // out[0] -> local player.
            const dc::entity::EnemyHitPlayer& hit = hits[0];
            if (hit.hit && hit.damage > 0.0f && !dead) react_for(0, hit.attacker_id, player.position[0], player.position[2]);
            player.health -= hit.damage * bill_mult(player.position[0], player.position[2]);   // Bill aura softens it
            if (player.health < 0.0f) player.health = 0.0f;
            player.knock_vel[0] += hit.knock[0];        // integrated (with collision) in player.update next frame
            player.knock_vel[2] += hit.knock[2];
            if (hit.hit) player.hit_flash = dc::entity::FLASH_TIME;   // damage got through -> flash red
            if (hit.ignite_time > 0.0f) { player.burn_time = hit.ignite_time; player.burn_dps = hit.ignite_dps; }  // set ablaze
            player.stamina -= hit.stamina_cost;                      // blocking spent this much stamina
            if (hit.blocked) block_flash = 0.3f;                     // bubble absorbed a hit -> flash red
            if (player.stamina < 0.0f) player.stamina = 0.0f;
            host_damage += hit.dealt;                                // melee damage we dealt this tick
            // out[i+1] -> connected clients' bodies (health + knockback only; their
            // own stamina/flash are cosmetic and handled client-side for now).
            for (std::size_t i = 0; i < host_clients.size(); ++i) {
                const dc::entity::EnemyHitPlayer& h = hits[i + 1];
                auto& b = host_clients[i].body;
                if (h.hit && h.damage > 0.0f && b.health > 0.0f) react_for(static_cast<int>(i + 1), h.attacker_id, b.position[0], b.position[2]);
                b.health -= h.damage * bill_mult(b.position[0], b.position[2]);
                if (b.health < 0.0f) b.health = 0.0f;
                b.knock_vel[0] += h.knock[0];
                b.knock_vel[2] += h.knock[2];
                if (h.hit) b.hit_flash = dc::entity::FLASH_TIME;   // unblocked -> flash red
                if (h.ignite_time > 0.0f) { b.burn_time = h.ignite_time; b.burn_dps = h.ignite_dps; }
                host_clients[i].damage_dealt += h.dealt;           // melee damage this client dealt
            }

            // Burn DoT (flamethrower): tick fire damage on every burning player while it
            // lasts. Host-authoritative; the resulting health goes out in the snapshot.
            auto tick_burn = [&](dc::entity::Player& b) {
                if (b.burn_time <= 0.0f) return;
                b.burn_time -= dt;
                b.health -= b.burn_dps * dt;
                if (b.health < 0.0f) b.health = 0.0f;
            };
            tick_burn(player);
            for (auto& hc : host_clients) tick_burn(hc.body);

            // Base damage this tick (night only): the shield soaks it first (and flashes
            // red); only the overflow once the shield is down reaches the core. The shield
            // recharges on solar power during the day.
            if (core_index >= 0 && core_index < static_cast<int>(hits.size())) {
                float dmg = hits[core_index].damage;
                if (dmg > 0.0f) {
                    shield_flash = 0.3f;
                    const float absorbed = dmg < shield_health ? dmg : shield_health;
                    shield_health -= absorbed; dmg -= absorbed;
                    core_health -= dmg;
                    if (core_health < 0.0f) core_health = 0.0f;
                }
            }
            // Friendly mobs take their enemy hits (parallel to `players`) and the slain are pruned.
            if (ally_start >= 0) {
                for (std::size_t i = 0; i < allies.size(); ++i) {
                    const int hidx = ally_start + static_cast<int>(i);
                    if (hidx < static_cast<int>(hits.size())) {
                        allies[i].health -= hits[hidx].damage * bill_mult(allies[i].pos[0], allies[i].pos[2]) * allies[i].def_mult;
                        host_damage += hits[hidx].dealt;   // credit damage the mob dealt to enemies
                    }
                }
                for (std::size_t i = 0; i < allies.size();)
                    if (allies[i].health <= 0.0f) { allies[i] = allies.back(); allies.pop_back(); }
                    else ++i;
            }
            // Enemy melee/ranged hits on a friendly WARSHIP chip its hull (matched by id; the boat
            // block culls a sunk hull next frame, ejecting any rider).
            if (boat_start >= 0) {
                for (std::size_t j = 0; j < boat_combat_ids.size(); ++j) {
                    const int hidx = boat_start + static_cast<int>(j);
                    if (hidx < static_cast<int>(hits.size()) && hits[hidx].damage > 0.0f)
                        for (auto& b : boats) if (b.id == boat_combat_ids[j]) { b.health -= hits[hidx].damage; break; }
                }
            }
            // Shield recharges continuously now (night is cosmetic).
            shield_health = std::min(shield_max, shield_health + SHIELD_RECHARGE * dt);

            // Solar turrets: ALWAYS active (day + night) — they never power down, just hold
            // their last aim when nothing's in range. Each on its own cooldown, hits the
            // nearest enemy in range. Host-authoritative damage.
            for (int i = 0; i < static_cast<int>(turret_pos.size()); ++i) {
                if (turret_cd[i] > 0.0f) turret_cd[i] -= dt;
                const dc::entity::Entity* tgt = turret_target(turret_pos[i].x, turret_pos[i].z);
                if (tgt && turret_cd[i] <= 0.0f) {
                    vec3 c = { tgt->position[0], tgt->position[1], tgt->position[2] };
                    std::vector<uint32_t> one;
                    dc::entity::radius_attack(entities, c, 0.6f, TURRET_DAMAGE, 4.0f, one, &frame_hits);
                    turret_cd[i] = TURRET_FIRE_INTERVAL;
                }
            }

            // --- MORTAR artillery: very slow, long-range lobbed shells with a big AoE. Each picks
            // the densest enemy cluster in [MIN, RANGE] and lobs a shell that detonates on arrival. ---
            for (int i = 0; i < static_cast<int>(mortar_pos.size()); ++i) {
                if (mortar_cd[i] > 0.0f) { mortar_cd[i] -= dt; continue; }
                const float mx = mortar_pos[i].x, mz = mortar_pos[i].z;
                const dc::entity::Entity* best = nullptr; int bestN = -1; float bestD2 = 0.0f;
                for (const auto& e : entities.items) {
                    if (!e.alive || e.type != dc::entity::EntityType::Enemy) continue;
                    const float dx = e.position[0]-mx, dz = e.position[2]-mz, d2 = dx*dx + dz*dz;
                    if (d2 < dc::game::MORTAR_MIN_RANGE*dc::game::MORTAR_MIN_RANGE || d2 > dc::game::MORTAR_RANGE*dc::game::MORTAR_RANGE) continue;
                    int cnt = 0;   // how many enemies cluster around this one (maximize splash value)
                    for (const auto& o : entities.items) if (o.alive && o.type == dc::entity::EntityType::Enemy) {
                        const float ox = o.position[0]-e.position[0], oz = o.position[2]-e.position[2];
                        if (ox*ox + oz*oz < dc::game::MORTAR_BLAST*dc::game::MORTAR_BLAST) ++cnt;
                    }
                    if (cnt > bestN || (cnt == bestN && d2 > bestD2)) { bestN = cnt; best = &e; bestD2 = d2; }
                }
                if (best) {
                    MortarShell sh;
                    sh.from[0]=mx; sh.from[1]=mortar_pos[i].y+1.2f; sh.from[2]=mz;
                    sh.impact[0]=best->position[0]; sh.impact[1]=terrain.height(best->position[0], best->position[2]); sh.impact[2]=best->position[2];
                    mortar_shells.push_back(sh);
                    mortar_cd[i] = dc::game::MORTAR_CD;
                }
            }
            // Advance in-flight shells; on arrival, detonate a big AoE + a boom.
            for (std::size_t s = 0; s < mortar_shells.size(); ) {
                mortar_shells[s].t += dt;
                if (mortar_shells[s].t >= mortar_shells[s].dur) {
                    vec3 c = { mortar_shells[s].impact[0], mortar_shells[s].impact[1], mortar_shells[s].impact[2] };
                    std::vector<uint32_t> ids;
                    dc::entity::radius_attack(entities, c, dc::game::MORTAR_BLAST, dc::game::MORTAR_DAMAGE, 11.0f, ids, &frame_hits);
                    frame_booms.push_back(c[0]); frame_booms.push_back(c[1]+0.4f); frame_booms.push_back(c[2]);
                    mortar_shells[s] = mortar_shells.back(); mortar_shells.pop_back();
                } else ++s;
            }

            // ENEMY-BASE turrets fire on the nearest of our mobs (then players) in range.
            for (int i = 0; i < enemy_turret_n; ++i) {
                if (eturret_cd[i] > 0.0f) eturret_cd[i] -= dt;
                const float bx = eturret_pos[i].x, bz = eturret_pos[i].z;
                float bd2 = ENEMY_TURRET_RANGE * ENEMY_TURRET_RANGE; float* targHP = nullptr; float tgx = bx, tgz = bz;
                auto consider = [&](float px, float pz, float* hp) {
                    const float dx = px - bx, dz = pz - bz, d2 = dx*dx + dz*dz;
                    if (d2 < bd2) { bd2 = d2; targHP = hp; tgx = px; tgz = pz; }
                };
                for (auto& a : allies) consider(a.pos[0], a.pos[2], &a.health);          // mobs are the priority
                for (auto& s : subs) if (s.kind == 2) consider(s.pos[0], s.pos[2], &s.health);  // an emerged sub draws fire
                if (player.health > 0.0f) consider(player.position[0], player.position[2], &player.health);
                for (auto& hc : host_clients) if (hc.body.health > 0.0f) consider(hc.body.position[0], hc.body.position[2], &hc.body.health);
                if (targHP && eturret_cd[i] <= 0.0f) {
                    *targHP -= ENEMY_TURRET_DAMAGE; eturret_cd[i] = ENEMY_TURRET_CD;
                    // Visible red tracer from the muzzle to the target (host-authoritative cosmetic).
                    const float my = eturret_pos[i].y + 1.1f, ty = terrain.height(tgx, tgz) + 0.9f;
                    float vx = tgx-bx, vy = ty-my, vz = tgz-bz; float vl = std::sqrt(vx*vx+vy*vy+vz*vz); if (vl < 1e-3f) vl = 1.0f;
                    const float spd = 48.0f;
                    TBullet tb; tb.pos[0]=bx; tb.pos[1]=my; tb.pos[2]=bz;
                    tb.vel[0]=vx/vl*spd; tb.vel[1]=vy/vl*spd; tb.vel[2]=vz/vl*spd; tb.life=0.7f; tb.red=true;
                    turret_bullets.push_back(tb);
                }
            }
            // Prune mobs the enemy turrets just killed (the ally-hit pass also prunes, next frame).
            for (std::size_t i = 0; i < allies.size();)
                if (allies[i].health <= 0.0f) { allies[i] = allies.back(); allies.pop_back(); } else ++i;

            // Defensive build pieces (host-authoritative): barricades block + soak enemies and
            // repair by day; landmines detonate on the first enemy to step near. piece_hp is
            // replicated so every peer renders the same wall HP / spent mines.
            piece_hp.resize(base.pieces.size(), 0.0f);   // safety: stay parallel to the layout
            for (std::size_t pi = 0; pi < base.pieces.size(); ++pi) {
                const auto& bp = base.pieces[pi];
                const float bx = (bp.col + 0.5f) * dc::world::TILE, bz = (bp.row + 0.5f) * dc::world::TILE;
                if (bp.piece == static_cast<uint8_t>(dc::game::BuildPiece::Barricade)) {
                    float& hp = piece_hp[pi];
                    if (hp <= 0.0f) {   // broken: slowly rebuilds when not under attack
                        hp = std::min(dc::game::BARRICADE_MAX_HP, hp + dc::game::BARRICADE_REGEN * dt);
                        continue;
                    }
                    bool touched = false;
                    const float RR = dc::game::BARRICADE_BLOCK_R + 0.5f;   // block ring (+ enemy body)
                    for (auto& e : entities.items) {
                        if (e.type != dc::entity::EntityType::Enemy || !e.alive) continue;
                        float dx = e.position[0] - bx, dz = e.position[2] - bz;
                        const float d2 = dx*dx + dz*dz;
                        if (d2 >= RR*RR) continue;
                        float d = std::sqrt(d2); if (d < 1e-4f) { dx = 1.0f; dz = 0.0f; d = 1.0f; }
                        e.position[0] = bx + dx/d * RR; e.position[2] = bz + dz/d * RR;   // shove back to the wall face
                        touched = true;
                    }
                    if (touched) hp -= dc::game::BARRICADE_CHIP_DPS * dt;       // enemies gnaw it down
                    else hp = std::min(dc::game::BARRICADE_MAX_HP, hp + dc::game::BARRICADE_REGEN * dt);
                } else if (bp.piece == static_cast<uint8_t>(dc::game::BuildPiece::Landmine)) {
                    if (piece_hp[pi] < 1.0f) {   // spent/arming: ~8s to fully re-arm, can't trip yet
                        piece_hp[pi] = std::min(1.0f, piece_hp[pi] + dt / 8.0f);
                        continue;
                    }
                    bool trip = false;
                    for (auto& e : entities.items) {
                        if (e.type != dc::entity::EntityType::Enemy || !e.alive) continue;
                        const float dx = e.position[0] - bx, dz = e.position[2] - bz;
                        if (dx*dx + dz*dz < dc::game::LANDMINE_TRIGGER_R * dc::game::LANDMINE_TRIGGER_R) { trip = true; break; }
                    }
                    if (trip) {
                        piece_hp[pi] = 0.0f;
                        const float by = terrain.height(bx, bz) + 0.4f;
                        vec3 c = { bx, by, bz };
                        std::vector<uint32_t> none;
                        host_damage += dc::entity::radius_attack(entities, c, dc::game::LANDMINE_BLAST_R,
                                                                 dc::game::LANDMINE_DAMAGE, dc::game::LANDMINE_KNOCK, none, &frame_hits);
                        frame_booms.push_back(bx); frame_booms.push_back(by); frame_booms.push_back(bz);   // reuse the explosion FX
                    }
                } else if (bp.piece == static_cast<uint8_t>(dc::game::BuildPiece::Barracks)) {
                    // Each barracks is one MOB TYPE (stored in the piece's `rot`). It's a one-time
                    // purchase that then spawns its mob every type.interval seconds for FREE (only
                    // the global cap throttles it).
                    const dc::game::MobType& mt = dc::game::mob_type(bp.rot);
                    // This barracks' UPGRADE levels buff only its own troops (HP/DEF/SPEED) + its spawn RATE.
                    const float up_hp = dc::game::barracks_hp_mult(bp.up[0]);
                    const float up_def = dc::game::barracks_def_mult(bp.up[1]);
                    const float up_spd = dc::game::barracks_spd_mult(bp.up[2]);
                    const float up_rate = dc::game::barracks_rate_mult(bp.up[3]);
                    // Per-TYPE active cap: each barracks keeps cap(kind)+capacity-upgrade alive; pooled
                    // across all barracks of this kind (and never beyond the global ALLY_CAP).
                    int kind_cap = 0, alive_kind = 0;
                    for (const auto& q : base.pieces)
                        if (q.piece == static_cast<uint8_t>(dc::game::BuildPiece::Barracks) && q.rot == bp.rot)
                            kind_cap += mt.cap + dc::game::barracks_cap_bonus(q.up[4]);
                    for (const auto& a : allies) if (a.kind == bp.rot) ++alive_kind;
                    if (piece_hp[pi] > 0.0f) piece_hp[pi] -= dt;
                    if (piece_hp[pi] <= 0.0f && alive_kind < kind_cap && static_cast<int>(allies.size()) < dc::game::ALLY_CAP) {
                        piece_hp[pi] = mt.interval / up_rate;   // upgraded barracks spawn faster
                        ally_rng = ally_rng * 1664525u + 1013904223u;
                        const float jx = ((ally_rng >> 9) % 100) / 100.0f - 0.5f;
                        const float jz = ((ally_rng >> 17) % 100) / 100.0f - 0.5f;
                        Ally a; a.pos[0] = bx + jx * 1.9f; a.pos[2] = bz + jz * 1.9f;
                        a.pos[1] = terrain.height(a.pos[0], a.pos[2]) + dc::world::EYE_HEIGHT;
                        a.max_hp = mt.hp * up_hp; a.health = a.max_hp; a.attack_cd = 0.0f; a.kind = bp.rot;
                        a.def_mult = up_def; a.up = static_cast<uint8_t>(dc::game::barracks_up_total(bp));
                        // Per-mob variety: slight random speed + size (so a squad isn't clones).
                        ally_rng = ally_rng * 1664525u + 1013904223u;
                        a.size_mul  = 0.85f + ((ally_rng >> 8) % 100) / 100.0f * 0.45f;   // 0.85..1.30
                        a.speed_mul = (0.85f + ((ally_rng >> 16) % 100) / 100.0f * 0.40f) * up_spd;  // ×upgrade speed
                        allies.push_back(a);
                    }
                } else if (bp.piece == static_cast<uint8_t>(dc::game::BuildPiece::SubPen)) {
                    // SUBMARINES are disabled for now — a Sub Pen does nothing (kept so saved bases load).
                } else if (bp.piece == static_cast<uint8_t>(dc::game::BuildPiece::Shipyard)) {
                    // A Shipyard builds the BOAT TYPE it was placed with (rot: 0 warship / 1 minelayer /
                    // 2 minesweeper) and keeps one alive — the per-type cap equals the number of yards
                    // of that type, so it only rebuilds once one is lost.
                    const uint8_t want_role = bp.rot <= 2 ? bp.rot : 0;
                    if (piece_hp[pi] > 0.0f) piece_hp[pi] -= dt;
                    int num_yards = 0, my_boats = 0;
                    for (const auto& q : base.pieces)
                        if (q.piece == static_cast<uint8_t>(dc::game::BuildPiece::Shipyard) && (q.rot <= 2 ? q.rot : 0) == want_role) ++num_yards;
                    for (auto& b : boats) if (b.team == 1 && b.role == want_role) ++my_boats;
                    if (piece_hp[pi] <= 0.0f && my_boats < num_yards) {
                        piece_hp[pi] = (want_role == 2) ? 4.0f : 8.0f;   // sweepers rebuild fast (they die a lot)
                        Boat b; b.id = next_boat_id++; b.kind = 0; b.team = 1; b.role = want_role; b.fire_cd = 1.5f;
                        b.health = want_role==1 ? dc::game::MINELAYER_HP : want_role==2 ? dc::game::MINESWEEPER_HP : FRIENDLY_BOAT_HP;
                        b.pos[0] = river_x0 + 2.0f; b.pos[2] = channel_center(b.pos[0]);
                        b.pos[1] = terrain.height(b.pos[0], b.pos[2]); b.yaw = 0.0f;
                        boats.push_back(b);
                    }
                }
            }

            // --- SUBMARINES (both teams). A sub travels submerged toward its prey, surfaces in
            // range to torpedo it, then dives. Surface ships + turrets can only hit a sub while
            // it's SURFACED, but SUBS CAN HIT EACH OTHER EVEN SUBMERGED (the only underwater duel).
            // Friendly subs (team 1) hunt enemy boats/subs then the enemy core; enemy subs (team 0)
            // hunt your warships/subs then your core. ---
            for (auto& s : subs) {
                const uint8_t et = s.team == 1 ? 0 : 1;   // the team this sub is hunting
                Boat* tgt = nullptr; float bd2 = 1e18f;
                for (auto& b : boats) if (b.kind == 0 && b.team == et && b.health > 0.0f) {   // enemy surface ships
                    const float dx = b.pos[0]-s.pos[0], dz = b.pos[2]-s.pos[2], d2 = dx*dx+dz*dz;
                    if (d2 < bd2) { bd2 = d2; tgt = &b; }
                }
                for (auto& o : subs) if (&o != &s && o.team == et && o.health > 0.0f) {        // enemy subs (even submerged!)
                    const float dx = o.pos[0]-s.pos[0], dz = o.pos[2]-s.pos[2], d2 = dx*dx+dz*dz;
                    if (d2 < bd2) { bd2 = d2; tgt = &o; }
                }
                // No ship/sub to fight? Push to the enemy BASE and torpedo its core.
                const float* core_pos_t = s.team == 1 ? enemy_core_pos : core_pos;
                float tx, tz; bool core_target = false;
                if (tgt) { tx = tgt->pos[0]; tz = tgt->pos[2]; }
                else { tx = core_pos_t[0]; tz = core_pos_t[2];
                       core_target = (s.team == 1 ? enemy_core_health : core_health) > 0.0f; }
                float dx = tx - s.pos[0], dz = tz - s.pos[2]; const float d = std::sqrt(dx*dx+dz*dz);
                const float fire_at = tgt ? dc::game::SUB_RANGE : (dc::game::SUB_RANGE + CORE_RAD);
                const bool in_range = (tgt || core_target) && d <= fire_at;
                if (!in_range && d > 0.1f) {   // submerged transit toward the target (stay in water)
                    const float step = dc::game::SUB_SPEED * dt;
                    const float nx = s.pos[0]+dx/d*step, nz = s.pos[2]+dz/d*step;
                    if (in_water(nx, nz)) { s.pos[0]=nx; s.pos[2]=nz; }
                    else { s.pos[0]=nx; s.pos[2] += (channel_center(s.pos[0]) - s.pos[2]) * std::min(1.0f, step); }   // hug channel toward goal
                    s.yaw = std::atan2(dz, dx);
                }
                s.pos[1] = terrain.height(s.pos[0], s.pos[2]);
                if (s.fire_cd > 0.0f) s.fire_cd -= dt;
                if (s.surf > 0.0f) s.surf -= dt;
                if (in_range) {
                    s.surf = 0.6f;   // STAY surfaced the whole time it's engaging (refreshed each frame)
                    s.yaw = std::atan2(tz-s.pos[2], tx-s.pos[0]);
                    if (s.fire_cd <= 0.0f) {
                        s.fire_cd = dc::game::SUB_FIRE_CD;
                        if (tgt) { tgt->health -= dc::game::SUB_DAMAGE; }   // boat OR sub (pointer into the live vectors)
                        else if (s.team == 1) { enemy_core_health -= dc::game::SUB_DAMAGE; if (enemy_core_health < 0) enemy_core_health = 0; }
                        else {   // enemy sub on YOUR core: eat the shield first, then the core
                            float dmg = dc::game::SUB_DAMAGE;
                            if (shield_health > 0.0f) { const float a = std::min(shield_health, dmg); shield_health -= a; dmg -= a; shield_flash = 0.3f; }
                            if (dmg > 0.0f) { core_health -= dmg; if (core_health < 0) core_health = 0; }
                        }
                        host_damage += dc::game::SUB_DAMAGE;
                        // Visible TORPEDO tracer streaking from the bow toward the target.
                        TBullet tb; tb.pos[0]=s.pos[0]+std::cos(s.yaw)*1.6f; tb.pos[1]=terrain.height(s.pos[0],s.pos[2])+0.5f; tb.pos[2]=s.pos[2]+std::sin(s.yaw)*1.6f;
                        float vx=tx-tb.pos[0], vy=0.2f, vz=tz-tb.pos[2]; float vl=std::sqrt(vx*vx+vz*vz); if (vl<1e-3f) vl=1.0f;
                        const float spd=30.0f; tb.vel[0]=vx/vl*spd; tb.vel[1]=vy; tb.vel[2]=vz/vl*spd; tb.life=1.2f; tb.red=(s.team==0);
                        turret_bullets.push_back(tb);
                    }
                }
                s.kind = (s.surf > 0.0f) ? 2 : 1;   // 2 surfaced (engaging), 1 submerged periscope (transit)
            }
            for (std::size_t i = 0; i < subs.size();)
                if (subs[i].health <= 0.0f) {
                    frame_booms.push_back(subs[i].pos[0]); frame_booms.push_back(subs[i].pos[1]+0.4f); frame_booms.push_back(subs[i].pos[2]);
                    subs[i] = subs.back(); subs.pop_back();
                } else ++i;

            // INSULTERS (enemy) + BILLS (friendly) don't fight — they just HECKLE. On a shared
            // cadence, pick one at random and have it spit an insult (floating text + TTS).
            insulter_taunt_cd -= dt;
            if (insulter_taunt_cd <= 0.0f) {
                insulter_taunt_cd = 1.8f;
                struct Heck { float x, y, z; };
                std::vector<Heck> hecks;
                for (auto& e : entities.items)
                    if (e.alive && e.type == dc::entity::EntityType::Enemy && e.kind == dc::entity::EnemyKind::Insulter)
                        hecks.push_back({ e.position[0], e.position[1] + 1.8f, e.position[2] });
                for (auto& a : allies)
                    if (dc::game::mob_type(a.kind).visual == dc::game::MobVisual::Insulter)
                        hecks.push_back({ a.pos[0], a.pos[1] + 0.7f, a.pos[2] });
                if (!hecks.empty() && static_cast<int>(taunts.size()) < MAX_TAUNTS) {
                    spark_rng = spark_rng * 1664525u + 1013904223u;
                    const Heck& h = hecks[(spark_rng >> 8) % hecks.size()];
                    spawn_taunt(h.x, h.y, h.z, pick_line(false, 0xFFFFFFFFu), true);
                }
            }

            // Gunner minions: on a per-player volley timer, every minion shoots the nearest
            // enemy in range (focus-fire; total = count * damage). Host-authoritative, so a
            // damage number goes out in frame_hits and the kill drops loot next tick.
            auto minion_volley = [&](float px, float pz, int count, float dmg, float range) -> float {
                if (count <= 0) return 0.0f;
                dc::entity::Entity* best = nullptr; float bd2 = range * range;
                for (auto& en : entities.items) {
                    if (en.type != dc::entity::EntityType::Enemy || !en.alive || en.health <= 0.0f) continue;
                    const float dx = en.position[0] - px, dz = en.position[2] - pz, d2 = dx*dx + dz*dz;
                    if (d2 < bd2) { bd2 = d2; best = &en; }
                }
                if (!best) return 0.0f;
                const float vol = dmg * count;
                const float dealt = (vol < best->health) ? vol : (best->health > 0.0f ? best->health : 0.0f);
                best->health -= vol; best->hit_flash = dc::entity::FLASH_TIME;
                frame_hits.push_back({ {best->position[0], best->position[1], best->position[2]}, vol, false });
                return dealt;
            };
            minion_fire_cd -= dt;
            if (minion_fire_cd <= 0.0f && player.minion_count > 0) {
                host_damage += minion_volley(player.position[0], player.position[2], player.minion_count, player.minion_damage, player.minion_range);
                minion_fire_cd = MINION_FIRE_INTERVAL;
            }
            for (auto& hc : host_clients) {
                hc.minion_fire_cd -= dt;
                if (hc.minion_fire_cd <= 0.0f && hc.input.minion_count > 0) {
                    hc.damage_dealt += minion_volley(hc.body.position[0], hc.body.position[2], hc.input.minion_count, hc.input.minion_damage, hc.input.minion_range);
                    hc.minion_fire_cd = MINION_FIRE_INTERVAL;
                }
            }

            // Supersonic dodge shockwaves queued this frame (local + clients): one big
            // knockback + damage burst each, credited to the dasher.
            for (const auto& bl : supersonic_blasts) {
                std::vector<uint32_t> hit;
                const vec3 c = { bl.x, 0.0f, bl.z };
                const float dealt = dc::entity::radius_attack(entities, c, SUPERSONIC_RADIUS, bl.dmg, SUPERSONIC_KNOCK, hit, &frame_hits);
                if (bl.owner == 0) host_damage += dealt;
                else for (auto& hc : host_clients) if (hc.id == bl.owner) { hc.damage_dealt += dealt; break; }
            }
            supersonic_blasts.clear();

            // Rebuild remotes from the (now combat-resolved) client bodies so their
            // hit-flash/swing/block show this frame.
            remotes.clear();
            for (auto& hc : host_clients) {
                bool m = (hc.input.forward != 0.0f || hc.input.strafe != 0.0f);
                Remote r{}; r.id = hc.id;
                r.pos[0] = hc.body.position[0]; r.pos[1] = hc.body.position[1]; r.pos[2] = hc.body.position[2];
                r.yaw = hc.body.yaw; r.pitch = hc.body.pitch; r.anim_time = hc.anim_time; r.moving = m;
                r.ghost = hc.body.health <= 0.0f;
                r.punching = hc.input.anim_punch != 0; r.blocking = hc.input.anim_block != 0;
                r.punch_time = hc.input.punch_time; r.block_time = hc.input.block_time;
                r.hit_flash = hc.body.hit_flash; r.sword_scale = hc.input.sword_scale;
                r.damage_dealt = static_cast<float>(hc.damage_dealt);
                for (const auto& t : hc.throwns)
                    r.throwns.push_back({ t.pos[0], t.pos[1], t.pos[2], t.spin, t.size });
                r.orbit_active = hc.orbit_active; r.orbit_count = hc.orbit_count;
                r.orbit_angle = hc.orbit_angle; r.orbit_spin = hc.orbit_spin;
                r.orbit_radius = hc.orbit_radius;
                r.bash_active = hc.bash_active; r.bash_radius = hc.bash_radius;
                remotes.push_back(r);
            }
        }
        if (player.hit_flash > 0.0f) player.hit_flash -= dt;   // decay the flash (cosmetic, both sides)

        // Trailblazer: drop burning segments behind each player leaving a trail (every
        // peer, for visuals); the host damages enemies standing in the fire.
        {
            struct TOwner { uint32_t id; float x, z, life, dmg; };
            std::vector<TOwner> owners;
            if (player.trail_life > 0.0f && !dead)
                owners.push_back({ my_id, player.position[0], player.position[2], player.trail_life, player.trail_damage });
            for (const auto& rp : remotes) if (!rp.ghost && rp.trail_life > 0.0f) {
                float dmg = 0.0f;   // host knows each client's trail damage (clients don't damage)
                if (net.role != dc::net::Role::Client)
                    for (auto& hc : host_clients) if (hc.id == rp.id) { dmg = hc.input.trail_damage; break; }
                owners.push_back({ rp.id, rp.pos[0], rp.pos[2], rp.trail_life, dmg });
            }
            for (std::size_t i = 0; i < trails.size();) {   // drop trails whose owner is gone
                bool present = false; for (auto& o : owners) if (o.id == trails[i].id) { present = true; break; }
                if (!present) { trails[i] = trails.back(); trails.pop_back(); } else ++i;
            }
            for (auto& o : owners) {
                Trail* t = nullptr; for (auto& tr : trails) if (tr.id == o.id) { t = &tr; break; }
                if (!t) { trails.push_back({ o.id, 0, 0, 0, 0, false, {} }); t = &trails.back(); }
                t->dmg = o.dmg; t->life = o.life;
                const float mdx = o.x - t->last_x, mdz = o.z - t->last_z;
                if (!t->init || mdx*mdx + mdz*mdz > TRAIL_DROP*TRAIL_DROP) {
                    t->segs.push_back({ o.x, o.z, 0.0f }); t->last_x = o.x; t->last_z = o.z; t->init = true;
                }
                for (std::size_t i = 0; i < t->segs.size();) {
                    t->segs[i].age += dt;
                    if (t->segs[i].age >= t->life) { t->segs[i] = t->segs.back(); t->segs.pop_back(); } else ++i;
                }
            }
            if (net.role != dc::net::Role::Client) {   // host: burn enemies standing in a trail
                for (auto& t : trails) {
                    if (t.dmg <= 0.0f) continue;
                    for (auto& e : entities.items) {
                        if (e.type != dc::entity::EntityType::Enemy || !e.alive || e.health <= 0.0f) continue;
                        bool inFire = false;
                        for (auto& sg : t.segs) { const float dx = e.position[0]-sg.x, dz = e.position[2]-sg.z;
                            if (dx*dx + dz*dz < TRAIL_RADIUS*TRAIL_RADIUS) { inFire = true; break; } }
                        if (!inFire) continue;
                        const float dealt = t.dmg * dt;
                        e.health -= dealt; e.hit_flash = dc::entity::FLASH_TIME;
                        if (t.id == 0) host_damage += dealt;
                        else for (auto& hc : host_clients) if (hc.id == t.id) { hc.damage_dealt += dealt; break; }
                    }
                }
            }
        }

        // Floating damage numbers: age + cull existing, then spawn this frame's hits.
        // (On a client frame_hits is empty — it spawns from the snapshot section instead.)
        for (std::size_t i = 0; i < dmg_numbers.size();) {
            dmg_numbers[i].age += dt;
            if (dmg_numbers[i].age >= DMG_LIFE) { dmg_numbers[i] = dmg_numbers.back(); dmg_numbers.pop_back(); }
            else ++i;
        }
        for (const auto& hn : frame_hits)
            dmg_numbers.push_back({ {hn.pos[0], hn.pos[1], hn.pos[2]}, hn.amount, 0.0f, hn.crit });

        // Loot drops: for each enemy that died this frame (frame_deaths holds xyz triples,
        // frame_death_xp the parallel per-kill XP that also encodes difficulty), crumble the
        // body to sand, then scatter gold coins + a blue XP orb around the death point so
        // they don't stack. Rarer (tougher) enemies drop more gold and more XP.
        {
            auto frand = [&]() { spark_rng = spark_rng * 1664525u + 1013904223u; return (spark_rng >> 8) * (1.0f / 16777216.0f); };
            const float xp_scale = 1.0f + run_time / 90.0f;     // late-run kills are worth a bit more
            for (std::size_t i = 0; i + 2 < frame_deaths.size(); i += 3) {
                const std::size_t k = i / 3;
                const float cx = frame_deaths[i], cy = frame_deaths[i + 1], cz = frame_deaths[i + 2];
                burst_sand(cx, cy, cz);   // host/standalone crumble (clients do it off the snapshot)
                const float raw = (k < frame_death_xp.size()) ? frame_death_xp[k] : 10.0f;   // base per-kind difficulty
                // Scatter a point within `rad` of the death spot (uniform over the disc).
                auto scattered = [&](float rad, float& ox, float& oz) {
                    const float ang = frand() * 6.2831853f, r = rad * std::sqrt(frand());
                    ox = cx + std::cos(ang) * r; oz = cz + std::sin(ang) * r;
                };
                // Total gold = the per-kind rank value (min 5 for a grunt). Split into a few
                // coins each worth a couple, so the pile reads + magnets nicely.
                int gold = (k < frame_death_gold.size()) ? static_cast<int>(std::round(frame_death_gold[k])) : 3;
                if (gold < 2) gold = 2;
                gold_drop_accum += gold;   // the enemy AI sizes its OWN income off the gold we farm
                int ncoins = (gold + 2) / 3; if (ncoins < 1) ncoins = 1; if (ncoins > 12) ncoins = 12;
                const float per = static_cast<float>(gold) / ncoins;
                for (int g = 0; g < ncoins; ++g) {
                    Coin c; c.pos[1] = cy; c.value = per; scattered(0.9f, c.pos[0], c.pos[2]);
                    coins.push_back(c);
                }
                XPOrb o; o.pos[1] = cy; scattered(0.8f, o.pos[0], o.pos[2]);
                o.value = raw * xp_scale;
                o.bob = frand() * 6.2831853f;
                xp_orbs.push_back(o);
            }
            // Demon fireball explosions this frame (host/standalone; clients do it off the snapshot).
            for (std::size_t i = 0; i + 2 < frame_booms.size(); i += 3)
                burst_fire(frame_booms[i], frame_booms[i + 1], frame_booms[i + 2]);
        }

        // Loot repulsion: coins + XP orbs gently shove each other apart (like magnets)
        // so a drop pile spreads into a readable layout instead of stacking. Host/
        // standalone only — clients get the spread-out positions via the snapshot.
        if (net.role != dc::net::Role::Client) {
            std::vector<float*> lx, lz;
            lx.reserve(coins.size() + xp_orbs.size()); lz.reserve(coins.size() + xp_orbs.size());
            for (auto& c : coins)   { lx.push_back(&c.pos[0]); lz.push_back(&c.pos[2]); }
            for (auto& o : xp_orbs) { lx.push_back(&o.pos[0]); lz.push_back(&o.pos[2]); }
            const std::size_t n = lx.size();
            if (n > 1 && n <= 400) {           // O(n^2); cap so a huge pile can't stall a frame
                const float SEP = 0.55f, PUSH = 2.5f;
                for (std::size_t a = 0; a < n; ++a)
                    for (std::size_t b = a + 1; b < n; ++b) {
                        float dx = *lx[b] - *lx[a], dz = *lz[b] - *lz[a];
                        float d2 = dx * dx + dz * dz;
                        if (d2 > SEP * SEP) continue;
                        if (d2 < 1e-6f) { *lx[a] -= 0.02f; *lx[b] += 0.02f; continue; }   // exact overlap: split
                        float d = std::sqrt(d2), step = (SEP - d) * 0.5f * PUSH * dt / d;
                        *lx[a] -= dx * step; *lz[a] -= dz * step;
                        *lx[b] += dx * step; *lz[b] += dz * step;
                    }
            }
        }

        // Coins: settle briefly (so they're always visible), then magnet toward the
        // NEAREST player and collect on contact into that player's own wallet
        // (per-player economy). Host-authoritative; clients render replicated coins
        // and read their balance back from the snapshot.
        const float MAGNET_RADIUS = 6.5f, COLLECT_RADIUS = 0.6f, COIN_SPEED = 9.0f, COIN_SETTLE = 0.35f;   // big pickup magnet
        if (net.role != dc::net::Role::Client) {
            // All LIVING collectors: the local player + every connected client, each
            // with its own wallet to credit. Ghosts (dead players) don't collect.
            struct Collector { float x, z; int* wallet; };
            std::vector<Collector> collectors;
            // Gold is a SHARED TEAM POOL: every collector credits the same wallet (`currency`).
            if (!dead) collectors.push_back({ player.position[0], player.position[2], &currency });
            for (auto& hc : host_clients)
                if (hc.body.health > 0.0f)
                    collectors.push_back({ hc.body.position[0], hc.body.position[2], &currency });
            // Scavenger mobs are collectors too — that's their whole job: roam + bank coins.
            for (const auto& a : allies)
                if (dc::game::mob_type(a.kind).scavenger)
                    collectors.push_back({ a.pos[0], a.pos[2], &currency });
            // A VACUUM build piece slowly drags loot toward the base core from far away (range
            // ~1/5 of the battlefield); the core then collects it into the shared pool.
            bool vacuum_on = false;
            for (const auto& pc : base.pieces) if (pc.piece == static_cast<uint8_t>(dc::game::BuildPiece::Vacuum)) { vacuum_on = true; break; }
            const float VAC_RANGE = (map->width * dc::world::TILE) / 5.0f;
            if (vacuum_on) collectors.push_back({ core_pos[0], core_pos[2], &currency });   // core banks vacuumed loot

            for (std::size_t i = 0; i < coins.size() && !collectors.empty();) {
                coins[i].age += dt;
                if (coins[i].age < COIN_SETTLE) { ++i; continue; }   // sit until visible
                if (vacuum_on) {   // slow long-range drift toward the core
                    const float dx = core_pos[0]-coins[i].pos[0], dz = core_pos[2]-coins[i].pos[2];
                    const float d = std::sqrt(dx*dx+dz*dz);
                    if (d > MAGNET_RADIUS && d < VAC_RANGE) {
                        const float step = dc::game::VACUUM_PULL_SPEED * dt;
                        coins[i].pos[0] += dx/d * step; coins[i].pos[2] += dz/d * step;
                    }
                }
                // Find the nearest collector to this coin.
                int best = 0; float best_d = 1e30f, bdx = 0.0f, bdz = 0.0f;
                for (std::size_t c = 0; c < collectors.size(); ++c) {
                    float dx = collectors[c].x - coins[i].pos[0];
                    float dz = collectors[c].z - coins[i].pos[2];
                    float d = std::sqrt(dx * dx + dz * dz);
                    if (d < best_d) { best_d = d; best = static_cast<int>(c); bdx = dx; bdz = dz; }
                }
                if (best_d < COLLECT_RADIUS) {
                    *collectors[best].wallet += static_cast<int>(coins[i].value);
                    coins[i] = coins.back(); coins.pop_back(); continue;
                }
                if (best_d < MAGNET_RADIUS && best_d > 1e-4f) {
                    float step = COIN_SPEED * dt;
                    coins[i].pos[0] += bdx / best_d * step;
                    coins[i].pos[2] += bdz / best_d * step;
                }
                ++i;
            }
        }

        // XP orbs: host-authoritative like coins. Settle, then magnet to the nearest
        // living player and grant XP on contact — to the host player directly, or to a
        // client via a reliable XpGranted event so it levels up on its own screen with
        // its own (client-side) upgrade choices. Clients just render the replicated orbs.
        if (net.role != dc::net::Role::Client) {
            struct XpCollector { float x, z; int client_idx; };   // client_idx < 0 => host/local player
            std::vector<XpCollector> cols;
            if (!dead) cols.push_back({ player.position[0], player.position[2], -1 });
            for (std::size_t c = 0; c < host_clients.size(); ++c)
                if (host_clients[c].body.health > 0.0f)
                    cols.push_back({ host_clients[c].body.position[0], host_clients[c].body.position[2], static_cast<int>(c) });
            const float XP_MAGNET = 8.0f, XP_COLLECT = 0.7f, XP_SPEED = 11.0f, XP_SETTLE = 0.25f;   // big pickup magnet
            // Vacuum: same slow drift toward the core (the nearest player absorbs it there).
            bool xp_vacuum_on = false;
            for (const auto& pc : base.pieces) if (pc.piece == static_cast<uint8_t>(dc::game::BuildPiece::Vacuum)) { xp_vacuum_on = true; break; }
            const float XP_VAC_RANGE = (map->width * dc::world::TILE) / 5.0f;
            for (std::size_t i = 0; i < xp_orbs.size() && !cols.empty();) {
                xp_orbs[i].age += dt;
                if (xp_orbs[i].age < XP_SETTLE) { ++i; continue; }
                if (xp_vacuum_on) {
                    const float dx = core_pos[0]-xp_orbs[i].pos[0], dz = core_pos[2]-xp_orbs[i].pos[2];
                    const float d = std::sqrt(dx*dx+dz*dz);
                    if (d > XP_MAGNET && d < XP_VAC_RANGE) {
                        const float step = dc::game::VACUUM_PULL_SPEED * dt;
                        xp_orbs[i].pos[0] += dx/d * step; xp_orbs[i].pos[2] += dz/d * step;
                    }
                }
                int best = 0; float best_d = 1e30f, bdx = 0.0f, bdz = 0.0f;
                for (std::size_t c = 0; c < cols.size(); ++c) {
                    const float dx = cols[c].x - xp_orbs[i].pos[0], dz = cols[c].z - xp_orbs[i].pos[2];
                    const float d = std::sqrt(dx * dx + dz * dz);
                    if (d < best_d) { best_d = d; best = static_cast<int>(c); bdx = dx; bdz = dz; }
                }
                if (best_d < XP_COLLECT) {
                    const float val = xp_orbs[i].value;
                    if (cols[best].client_idx < 0) add_xp(val);   // host player levels up locally
                    else {                                        // tell the client to gain XP + level up
                        unsigned char buf[1 + sizeof(float)];
                        buf[0] = static_cast<unsigned char>(dc::net::MsgType::XpGranted);
                        std::memcpy(buf + 1, &val, sizeof(float));
                        net.send_to_peer(host_clients[cols[best].client_idx].peer, buf, sizeof buf, true);
                    }
                    xp_orbs[i] = xp_orbs.back(); xp_orbs.pop_back(); continue;
                }
                if (best_d < XP_MAGNET && best_d > 1e-4f) {
                    const float step = XP_SPEED * dt;
                    xp_orbs[i].pos[0] += bdx / best_d * step;
                    xp_orbs[i].pos[2] += bdz / best_d * step;
                }
                ++i;
            }
        }

        // Host: broadcast the combined world snapshot (players + enemies + coins),
        // built now that everything has advanced this frame.
        if (net.role == dc::net::Role::Host && !host_clients.empty()) {
            std::vector<unsigned char> buf;
            buf.push_back(static_cast<unsigned char>(dc::net::MsgType::Snapshot));
            auto put = [&](const void* d, std::size_t n) {
                const unsigned char* b = static_cast<const unsigned char*>(d);
                buf.insert(buf.end(), b, b + n);
            };
            uint32_t np = 1 + static_cast<uint32_t>(host_clients.size()); put(&np, 4);
            { dc::net::PlayerState s{}; s.id = 0;
              s.x = player.position[0]; s.y = player.position[1]; s.z = player.position[2];
              s.yaw = player.yaw; s.pitch = player.pitch; s.anim_time = anim_time;
              s.health = player.health; s.moving = moving ? 1 : 0; s.currency = currency;
              s.damage_dealt = static_cast<float>(host_damage);
              s.elements = elem_mask(player.fire_dps, player.ice_slow, player.earth_knock);
              s.minions = static_cast<uint8_t>(player.minion_count); s.minion_range = player.minion_range;
              s.trail_life = player.trail_life;
              s.punching = punching ? 1 : 0; s.blocking = blocking ? 1 : 0;
              s.punch_time = punch_time; s.block_time = block_time;
              s.hit_flash = player.hit_flash; s.sword_scale = player.sword_scale;
              s.burning = player.burn_time > 0.0f ? 1 : 0;
              // (thrown swords ride the owner-keyed ThrownState list below, not PlayerState)
              if (orbit.active && player.weapon) {
                  s.orbit_active = 1; s.orbit_count = player.weapon->orbit_count;
                  s.orbit_angle = orbit.angle; s.orbit_spin = orbit.spin; s.orbit_radius = player.weapon->orbit_radius;
              }
              s.bash_active = bash.active ? 1 : 0; s.bash_radius = bash.radius;
              put(&s, sizeof s); }
            for (std::size_t ci = 0; ci < host_clients.size(); ++ci) {
                auto& hc = host_clients[ci];
                bool m = (hc.input.forward != 0.0f || hc.input.strafe != 0.0f);
                dc::net::PlayerState s{}; s.id = hc.id;
                // Stamina the host's block resolution spent for this client this tick;
                // the client subtracts it from its own (client-authoritative) stamina.
                if (ci + 1 < hits.size()) s.block_spent = hits[ci + 1].stamina_cost;
                s.x = hc.body.position[0]; s.y = hc.body.position[1]; s.z = hc.body.position[2];
                s.yaw = hc.body.yaw; s.pitch = hc.body.pitch; s.anim_time = hc.anim_time;
                s.health = hc.body.health; s.moving = m ? 1 : 0; s.currency = currency;   // shared pool to every client
                s.damage_dealt = static_cast<float>(hc.damage_dealt);
                s.elements = elem_mask(hc.input.fire_dps, hc.input.slow_factor, hc.input.earth_knock);
                s.minions = hc.input.minion_count; s.minion_range = hc.input.minion_range;
                s.trail_life = hc.input.trail_life;
                s.punching = hc.input.anim_punch; s.blocking = hc.input.anim_block;
                s.punch_time = hc.input.punch_time; s.block_time = hc.input.block_time;
                s.hit_flash = hc.body.hit_flash; s.sword_scale = hc.input.sword_scale;
                s.burning = hc.body.burn_time > 0.0f ? 1 : 0;
                s.orbit_active = hc.orbit_active ? 1 : 0; s.orbit_count = hc.orbit_count;
                s.orbit_angle = hc.orbit_angle; s.orbit_spin = hc.orbit_spin; s.orbit_radius = hc.orbit_radius;
                s.bash_active = hc.bash_active ? 1 : 0; s.bash_radius = hc.bash_radius;
                put(&s, sizeof s);
            }
            uint32_t ne = 0; for (auto& en : entities.items) if (en.type == dc::entity::EntityType::Enemy) ++ne;
            put(&ne, 4);
            for (auto& en : entities.items) {
                if (en.type != dc::entity::EntityType::Enemy) continue;
                dc::net::EnemyState e{}; e.x = en.position[0]; e.z = en.position[2]; e.yaw = en.yaw;
                e.anim_time = en.anim_time; e.attack_time = en.attack_time; e.hit_flash = en.hit_flash;
                e.punch_anim = en.punch_anim;
                e.health01 = en.stats.max_health > 0.0f ? en.health / en.stats.max_health : 0.0f;
                e.healthbar_time = en.healthbar_time;
                e.attacking = en.attacking ? 1 : 0; e.kind = static_cast<uint8_t>(en.kind);
                e.status = (en.burn_time > 0.0f ? 1 : 0) | (en.slow_time > 0.0f ? 2 : 0) | (en.elite ? 4 : 0);
                put(&e, sizeof e);
            }
            uint32_t nc = static_cast<uint32_t>(coins.size()); put(&nc, 4);
            for (auto& c : coins) { dc::net::CoinState cs{}; cs.x = c.pos[0]; cs.z = c.pos[2]; put(&cs, sizeof cs); }
            // XP orbs (render-only on clients; the host owns pickups + XP awards).
            uint32_t nxo = static_cast<uint32_t>(xp_orbs.size()); put(&nxo, 4);
            for (auto& o : xp_orbs) { dc::net::XPOrbState xs{}; xs.x = o.pos[0]; xs.z = o.pos[2]; put(&xs, sizeof xs); }
            // In-flight thrown swords (owner-keyed): the host player's (owner 0) plus every
            // client's, so each peer renders all of everyone's swords (minus its own, which
            // it predicts locally).
            {
                std::vector<dc::net::ThrownState> ts;
                if (player.weapon) for (const auto& th : throwns)
                    ts.push_back({ th.pos[0], th.pos[1], th.pos[2], th.spin, player.weapon->throw_size, 0u });
                for (auto& hc : host_clients) for (const auto& th : hc.throwns)
                    ts.push_back({ th.pos[0], th.pos[1], th.pos[2], th.spin, th.size, hc.id });
                uint32_t nth = static_cast<uint32_t>(ts.size()); put(&nth, 4);
                for (auto& t : ts) put(&t, sizeof t);
            }
            // Chest open-state (stable map order, so an index identifies the same chest
            // on every peer). One byte each — cheap, and lets clients render opens.
            uint32_t nh = static_cast<uint32_t>(chests.size()); put(&nh, 4);
            for (auto& ch : chests) {
                unsigned char o = ch.opened ? 1 : 0; put(&o, 1);
                unsigned char tk = 0; for (int k = 0; k < 4; ++k) if (ch.taken[k]) tk |= (1u << k);  // which slots bought
                put(&tk, 1);
            }
            // Drone vendors: just the bought flags (positions are deterministic).
            uint32_t nv = static_cast<uint32_t>(drone_vendors.size()); put(&nv, 4);
            for (auto& dv : drone_vendors) { unsigned char b = dv.bought ? 1 : 0; put(&b, 1); }
            // In-flight projectiles (ranged enemy shots) for clients to render.
            uint32_t npr = static_cast<uint32_t>(entities.projectiles.size()); put(&npr, 4);
            for (auto& pr : entities.projectiles) {
                dc::net::ProjectileState ps{}; ps.x = pr.pos[0]; ps.y = pr.pos[1]; ps.z = pr.pos[2];
                ps.r = pr.color[0]; ps.g = pr.color[1]; ps.b = pr.color[2];
                ps.vx = pr.vel[0]; ps.vy = pr.vel[1]; ps.vz = pr.vel[2]; ps.radius = pr.radius; ps.beam = pr.beam ? 1 : 0;
                put(&ps, sizeof ps);
            }
            // Damage numbers spawned this tick (so every client floats the same numbers).
            uint32_t nd = static_cast<uint32_t>(frame_hits.size()); put(&nd, 4);
            for (const auto& hn : frame_hits) {
                dc::net::DamageNumState ds{}; ds.x = hn.pos[0]; ds.y = hn.pos[1]; ds.z = hn.pos[2];
                ds.amount = hn.amount; ds.crit = hn.crit ? 1 : 0;
                put(&ds, sizeof ds);
            }
            // Enemy deaths this tick (xyz triples) so clients play the same sand-crumble.
            uint32_t ndeath = static_cast<uint32_t>(frame_deaths.size() / 3); put(&ndeath, 4);
            put(frame_deaths.data(), frame_deaths.size() * sizeof(float));
            // Demon fireball explosions this tick (xyz triples) -> same fire burst everywhere.
            uint32_t nboom = static_cast<uint32_t>(frame_booms.size() / 3); put(&nboom, 4);
            put(frame_booms.data(), frame_booms.size() * sizeof(float));
            // Wizard staff bolts in flight (host owns them) -> everyone renders them.
            uint32_t nbolt = static_cast<uint32_t>(bolts.size()); put(&nbolt, 4);
            for (auto& b : bolts) { dc::net::BoltState bs{}; bs.x=b.pos[0]; bs.y=b.pos[1]; bs.z=b.pos[2]; bs.big=b.big?1:0; put(&bs, sizeof bs); }
            put(&tod, 4);          // day/night clock, so clients share the cycle
            { uint32_t dn = static_cast<uint32_t>(day_num); put(&dn, 4); }
            put(&core_health, 4);    // base health, for the bar on every screen
            put(&shield_health, 4);  // shield health, for the dome on every screen
            put(&enemy_core_health, 4);   // enemy base health (the lane objective)
            // The player-built base: buildable radius + every placed piece (so clients render
            // the same floors/walls/stairs/doors/turrets and the same dome size).
            put(&base.build_radius, 4);
            { uint32_t nbp = static_cast<uint32_t>(base.pieces.size()); put(&nbp, 4); }
            if (!base.pieces.empty()) {
                put(base.pieces.data(), base.pieces.size() * sizeof(dc::game::BasePiece));
                piece_hp.resize(base.pieces.size(), 0.0f);
                put(piece_hp.data(), base.pieces.size() * sizeof(float));   // barricade HP / mine armed
            }
            // Friendly lane mobs, so every client renders the same army.
            { uint32_t na = static_cast<uint32_t>(allies.size()); put(&na, 4); }
            for (const auto& a : allies) {
                dc::net::AllyState as{}; as.x = a.pos[0]; as.z = a.pos[2]; as.yaw = a.yaw;
                as.health01 = a.max_hp > 0.0f ? a.health / a.max_hp : 0.0f; as.kind = a.kind; as.size = a.size_mul; as.atk = a.atk; as.up = a.up;
                put(&as, sizeof as);
            }
            put(&barracks_unlocked, 4);   // which mob types are unlocked (for the muster menu)
            { uint8_t ra = rally_active ? 1 : 0; put(&ra, 1); put(&rally_pos[0], 4); put(&rally_pos[2], 4); }  // mob rally point
            put(type_hold_x, dc::game::MOB_TYPE_COUNT * 4);   // per-type command-map hold positions
            { uint32_t nb = static_cast<uint32_t>(boats.size() + subs.size()); put(&nb, 4); }   // naval units (boats + subs)
            for (const auto& b : boats) {
                dc::net::BoatState bs{}; bs.x = b.pos[0]; bs.z = b.pos[2]; bs.yaw = b.yaw;
                const float maxhp = b.role==1 ? dc::game::MINELAYER_HP : b.role==2 ? dc::game::MINESWEEPER_HP
                                  : (b.team==1 ? FRIENDLY_BOAT_HP : BOAT_MAX_HP);
                bs.health01 = b.health / maxhp;
                // render kind: 0/4 = enemy/friendly warship, 8/9 = friendly/enemy minelayer, 10/11 = sweeper
                if (b.role == 1)      bs.kind = b.team==1 ? 8 : 9;
                else if (b.role == 2) bs.kind = b.team==1 ? 10 : 11;
                else                  bs.kind = (b.team == 1) ? 4 : 0;
                put(&bs, sizeof bs);
            }
            for (const auto& s : subs) {   // friendly subs (kind 1 submerged / 2 surfaced)
                dc::net::BoatState bs{}; bs.x = s.pos[0]; bs.z = s.pos[2]; bs.yaw = s.yaw;
                bs.health01 = (s.kind == 2) ? (s.health / dc::game::SUB_MAX_HP) : 1.0f;   // show the bar only when surfaced/vulnerable
                bs.kind = (s.team == 0) ? (s.kind + 4) : s.kind;   // 5/6 = ENEMY sub submerged/surfaced
                put(&bs, sizeof bs);
            }
            { uint32_t nsp = static_cast<uint32_t>(slime_patches.size()); put(&nsp, 4); }   // slime puddles
            for (const auto& s : slime_patches) {
                dc::net::SlimePatchState ss{}; ss.x = s.pos[0]; ss.z = s.pos[2]; ss.radius = s.radius;
                ss.life01 = s.max_life > 0.0f ? s.life / s.max_life : 0.0f; put(&ss, sizeof ss);
            }
            { uint32_t nm = static_cast<uint32_t>(naval_mines.size()); put(&nm, 4); }   // sea-mines (both teams)
            for (const auto& m : naval_mines) {
                dc::net::MineState ms{}; ms.x = m.pos[0]; ms.z = m.pos[2]; ms.armed = m.arm; ms.team = m.team; put(&ms, sizeof ms);
            }
            net.broadcast(buf.data(), buf.size(), false);
        }

        // Death is NOT permanent: a downed player ghosts for RESPAWN_DELAY seconds, then revives at
        // full health back at base. The run only ENDS when a CORE falls (win = enemy's, loss = ours).
        if (net.role != dc::net::Role::Client && death_flash <= 0.0f) {
            const float RESPAWN_DELAY = 20.0f;
            // local host player
            if (player.health <= 0.0f) {
                if (respawn_timer < 0.0f) respawn_timer = RESPAWN_DELAY;
                respawn_timer -= dt;
                if (respawn_timer <= 0.0f) {
                    player.health = player.stats.max_health;
                    player.position[0] = (map->spawn_col + 0.5f) * dc::world::TILE;
                    player.position[1] = dc::world::EYE_HEIGHT;
                    player.position[2] = (map->spawn_row + 0.5f) * dc::world::TILE;
                    respawn_timer = -1.0f;
                }
            } else respawn_timer = -1.0f;
            // each connected client revives the same way
            for (auto& hc : host_clients) {
                if (hc.body.health <= 0.0f) {
                    if (hc.respawn_timer < 0.0f) hc.respawn_timer = RESPAWN_DELAY;
                    hc.respawn_timer -= dt;
                    if (hc.respawn_timer <= 0.0f) {
                        hc.body.health = hc.body.stats.max_health;
                        hc.body.position[0] = (map->spawn_col + 0.5f) * dc::world::TILE;
                        hc.body.position[1] = dc::world::EYE_HEIGHT;
                        hc.body.position[2] = (map->spawn_row + 0.5f) * dc::world::TILE;
                        hc.respawn_timer = -1.0f;
                    }
                } else hc.respawn_timer = -1.0f;
            }
            if (enemy_core_health <= 0.0f) { LOGLINE(">>> WIN: enemy core destroyed <<<"); reset_run(); victory_flash = 2.0f; }
            else if (core_health <= 0.0f) { LOGLINE(">>> LOSS: our core fell <<<"); reset_run(); death_flash = 1.2f; }
        }
        if (death_flash > 0.0f) death_flash -= dt;
        if (victory_flash > 0.0f) victory_flash -= dt;

        // Enemies get tougher (HP + damage) as the run wears on; the AI applies this on spawn.
        // (Enemy power no longer scales with time — it comes from PAID barracks upgrades the AI buys.)
        // Frontline: how far our army/hero has pushed toward the ENEMY base (0..1). The enemy AI
        // reads this to decide when to wall up with turrets.
        float front_x = core_pos[0];
        for (const auto& a : allies) if (a.pos[0] > front_x) front_x = a.pos[0];
        if (player.position[0] > front_x) front_x = player.position[0];
        const float lane_len = enemy_core_pos[0] - core_pos[0];
        const float front_frac = lane_len > 1.0f
            ? std::min(1.0f, std::max(0.0f, (front_x - core_pos[0]) / lane_len)) : 0.0f;

        if (net.role != dc::net::Role::Client) {
            // ===================== ENEMY ECONOMY + ADVISOR AI =====================
            // The enemy can SEE exactly what we field. It earns gold a touch faster than we farm it,
            // a roster of advisors each rate (0..10) how badly it needs a given item right now, and
            // the next thing it saves toward is sampled by those weights — then bought when affordable.
            auto rand01 = [](uint32_t& s){ s = s*1664525u + 1013904223u; return (s >> 8) * (1.0f/16777216.0f); };

            // -- income: measure our gold-on-the-ground rate (1s window, smoothed) and set the
            // enemy's income to ~20% above it + a per-day floor (we have a strong hero, so they get more).
            gold_drop_timer -= dt;
            if (gold_drop_timer <= 0.0f) {
                our_gold_rate = our_gold_rate * 0.6f + static_cast<float>(gold_drop_accum) * 0.4f;
                gold_drop_accum = 0.0; gold_drop_timer = 1.0f;
            }
            enemy_rate = our_gold_rate * 0.90f + (0.9f + 0.45f * (day_num - 1));   // enemy earns ~our rate + a gentler per-day floor (was 1.2x + 1.5+0.7/day)
            enemy_gold += enemy_rate * dt;
            // Passive base income for the player team — smooths cash flow so it doesn't fully
            // depend on kills. NOT counted in gold_drop_accum, so the enemy AI can't scale off it.
            team_passive_accum += 1.6 * dt;
            while (team_passive_accum >= 1.0) { currency += 1; team_passive_accum -= 1.0; }

            // -- sensing: our forces vs theirs --
            int aliveK[10] = {}; int troops_alive = 0;
            for (auto& e : entities.items) if (e.alive && e.type == dc::entity::EntityType::Enemy) {
                aliveK[static_cast<int>(e.kind)]++; troops_alive++;
            }
            (void)aliveK;
            int e_boats = 0, e_layers = 0, e_sweepers = 0;   // enemy warships / minelayers / sweepers
            for (auto& b : boats) if (b.team == 0 && b.health > 0.0f) { if (b.role==0) e_boats++; else if (b.role==1) e_layers++; else e_sweepers++; }
            int p_warships = 0, p_yards = 0, player_mines = 0;
            float our_max_mspeed = 1.0f;
            for (auto& b : boats) if (b.team == 1 && b.health > 0.0f && b.role == 0) p_warships++;
            for (auto& m : naval_mines) if (m.team == 1) player_mines++;   // our mines the enemy may want to sweep
            for (auto& a : allies) { const float sp = dc::game::mob_type(a.kind).speed; if (sp > our_max_mspeed) our_max_mspeed = sp; }
            for (auto& q : base.pieces) if (q.piece == (uint8_t)dc::game::BuildPiece::Shipyard) p_yards++;
            const int seen_boats  = p_warships + p_yards;     // docks count even before a hull launches
            const float our_speed   = dc::game::ALLY_SPEED * our_max_mspeed;   // our fastest mob type
            const float their_speed = dc::entity::ENEMY_SPEED * enemy_speed_mult;

            // -- item catalog. The first 9 BUILD A BARRACKS (a persistent spawner of that mob type);
            // the rest are one-off / upgrade purchases. Costs are gold (one-time). --
            using EK = dc::entity::EnemyKind;
            enum { IT_BAR_SKEL, IT_BAR_RANGED, IT_BAR_BAT, IT_BAR_FLIER, IT_BAR_FLAME, IT_BAR_TROLL,
                   IT_BAR_DEMON, IT_BAR_INSULT, IT_BAR_SLIME, IT_BOAT, IT_SWEEPER, IT_LAYER, IT_TURRET, IT_CAVALRY, IT_UPGRADE, IT_N };
            const float COST[IT_N] = { 80,110,90,130,150,240,400,160,130,  260,90,150,150,170,90 };
            static const char* const NAME[IT_N] = { "Bks:Skel","Bks:Rangd","Bks:Bat","Bks:Flier","Bks:Flame","Bks:Troll",
                                                    "Bks:Demon","Bks:Inslt","Bks:Slime","Boat","Sweeper","Minelayr","Turret","Cavalry","Upgrade" };
            const EK BARKIND[9] = { EK::Skeleton, EK::Ranged, EK::Bat, EK::Flying, EK::Flamethrower, EK::Troll, EK::Demon, EK::Insulter, EK::Slime };
            auto ekcap = [](EK k) -> int { switch (k) { case EK::Bat: return 20; case EK::Ranged: return 7;
                case EK::Flamethrower: return 6; case EK::Slime: return 6; case EK::Troll: return 4;
                case EK::Demon: return 3; case EK::Insulter: return 2; default: return 10; } };
            // how many barracks of each kind the enemy already owns
            int barcount[9] = {};
            for (auto& b : ebarracks) for (int t=0;t<9;++t) if (b.kind == (uint8_t)BARKIND[t]) { barcount[t]++; break; }
            // Capacity race: if WE out-swarm the enemy (more total active-unit capacity), it leans
            // toward high-capacity barracks types (and upgrades) to catch up.
            int enemy_total_cap = 0; for (auto& b : ebarracks) enemy_total_cap += ekcap((EK)b.kind) + dc::game::barracks_cap_bonus(b.up);
            int player_total_cap = 0;
            for (auto& q : base.pieces) if (q.piece == (uint8_t)dc::game::BuildPiece::Barracks)
                player_total_cap += dc::game::mob_type(q.rot).cap + dc::game::barracks_cap_bonus(q.up[4]);
            const float swarm_need = std::min(3.5f, std::max(0.0f, (player_total_cap - enemy_total_cap) / 28.0f));
            // The laggard barracks (lowest level, below max) — the next upgrade target.
            int lag_idx = -1, lag_lvl = 99;
            for (std::size_t i = 0; i < ebarracks.size(); ++i) if (ebarracks[i].up < dc::game::BARRACKS_UP_MAX && ebarracks[i].up < lag_lvl) { lag_lvl = ebarracks[i].up; lag_idx = (int)i; }
            // Find a free TILE in the enemy's base (Open, within the build radius, clear of the core +
            // other barracks). The enemy places barracks on tiles just like we do; when none are free,
            // it must EXPAND. Returns whether a tile exists (and where).
            const float ecx = enemy_core_pos[0], ecz = enemy_core_pos[2];
            auto find_enemy_tile = [&](float& ox, float& oz) -> bool {
                const float T = dc::world::TILE; const int span = (int)(enemy_build_radius / T);
                const int ccol = (int)(ecx / T), crow = (int)(ecz / T);
                float bestd = 1e18f; bool found = false;
                for (int dr = -span; dr <= span; ++dr) for (int dc = -span; dc <= span; ++dc) {
                    const int col = ccol+dc, row = crow+dr;
                    const float tx = (col+0.5f)*T, tz = (row+0.5f)*T;
                    const float rx = tx-ecx, rz = tz-ecz, rd2 = rx*rx+rz*rz;
                    if (rd2 > enemy_build_radius*enemy_build_radius) continue;
                    if (rd2 < (CORE_RAD+2.0f)*(CORE_RAD+2.0f)) continue;          // keep clear of the core
                    if (map->at(col,row) != dc::world::Cell::Open) continue;       // must be a real floor tile
                    bool clash = false;
                    for (auto& b : ebarracks) { const float bx=b.x-tx, bz=b.z-tz; if (bx*bx+bz*bz < (2.0f*T)*(2.0f*T)) { clash=true; break; } }
                    if (clash) continue;
                    if (rd2 < bestd) { bestd = rd2; ox = tx; oz = tz; found = true; }   // fill from the core outward
                }
                return found;
            };
            float etile_x = 0.0f, etile_z = 0.0f;
            bool room = find_enemy_tile(etile_x, etile_z);   // is there an open base tile to build on?
            // CHEAT: the enemy never runs out of room — it just grows its base FOR FREE until a tile
            // opens up (no capacity limit, unlike the player).
            for (int grow = 0; !room && grow < 6 && enemy_build_radius < 40.0f; ++grow) {
                enemy_build_radius += 3.0f; room = find_enemy_tile(etile_x, etile_z);
            }
            const bool avail[IT_N] = {
                room, room, room && bat_loaded, room, room, room && troll_loaded, room && demon_loaded, room && insulter_loaded, room,
                true, true /*sweeper*/, true /*minelayer*/, enemy_turret_n < ENEMY_TURRET_MAX, enemy_speed_mult < 2.2f,
                lag_idx >= 0   // UPGRADE: available when some barracks can still be leveled up
            };
            auto dayramp = [&](int start, float per){ float v=(day_num - start + 1)*per; return v<0?0.0f:v; };

            // -- advisors: raw need (before the overdue ramp). 10 stays RARE (handled below). A barracks
            // advisor wants its type by some day (more critical each day after), and is sated once built. --
            auto raw_score = [&](int it) -> float {
                float s = 0.0f;
                switch (it) {
                    // barracks types: base want + (when out-swarmed) a boost toward HIGH-CAPACITY kinds.
                    case IT_BAR_SKEL:   s = 4.0f - barcount[0]*3.5f; break;                       // always want a skeleton line
                    case IT_BAR_RANGED: s = 2.5f + 0.12f*day_num - barcount[1]*3.5f; break;
                    case IT_BAR_BAT:    s = dayramp(2,0.5f) - barcount[2]*3.5f; break;
                    case IT_BAR_FLIER:  s = dayramp(2,0.45f) - barcount[3]*3.5f; break;
                    case IT_BAR_FLAME:  s = dayramp(3,0.5f) - barcount[4]*3.5f; break;
                    case IT_BAR_TROLL:  s = dayramp(5,0.6f) - barcount[5]*4.0f; break;
                    case IT_BAR_DEMON:  s = dayramp(6,0.7f) - barcount[6]*4.0f; break;            // wants a demon line by day 6, sated once built
                    case IT_BAR_INSULT: s = dayramp(4,0.5f) - barcount[7]*4.0f; break;
                    case IT_BAR_SLIME:  s = dayramp(3,0.35f) - barcount[8]*3.5f; break;
                    case IT_BOAT:    s = dayramp(2,0.3f) + (seen_boats>0?4.5f:0.0f) - e_boats*2.5f; break;   // counter our navy
                    case IT_LAYER:   s = (seen_boats>0 ? 2.5f + seen_boats*3.0f : 0.5f) + dayramp(2,0.2f) - e_layers*2.2f; break;  // mine our warships
                    case IT_SWEEPER: s = (player_mines>0 ? 1.5f + player_mines*1.3f : 0.0f) - e_sweepers*1.5f; break;             // clear our mines
                    case IT_TURRET: s = (front_frac>0.75f ? 5.0f + (front_frac-0.75f)*20.0f : front_frac*1.5f) - std::max(0,enemy_turret_n-1)*0.7f; break;
                    case IT_CAVALRY:s = (our_speed > their_speed*1.05f ? (our_speed/their_speed - 1.0f)*12.0f : 0.0f) - (enemy_speed_mult-1.0f)*7.0f; break;
                    // self-upgrade the laggard barracks: wanted more as days pass + when out-swarmed.
                    case IT_UPGRADE: s = (lag_idx >= 0) ? (3.2f + 0.5f*day_num + swarm_need*1.5f - lag_lvl*0.5f) : 0.0f; break;   // paid upgrades are the enemy's ONLY power scaling now -> invest more
                }
                // When the player out-swarms us, favour HIGH-CAPACITY barracks types to keep up.
                if (it <= IT_BAR_SLIME && swarm_need > 0.0f) s += swarm_need * ekcap(BARKIND[it]) / 7.0f;
                return s;
            };
            // overdue ramp + the rare 10. A persistently-unmet strong need climbs toward "critical".
            auto score = [&](int it) -> float {
                float s = raw_score(it);
                if (s >= 5.5f) ai_unmet[it] += dt; else ai_unmet[it] = std::max(0.0f, ai_unmet[it] - dt*2.0f);
                s += std::min(3.5f, ai_unmet[it] / (CYCLE_LEN*0.5f) * 4.0f);   // half a day ignored ≈ +4 (clamped)
                if (it == IT_TURRET && front_frac > 0.93f) s = 10.0f;          // they're at the gate: wall up or lose
                if (it == IT_LAYER && seen_boats >= 2 && e_layers == 0) s = std::max(s, 9.0f);   // their fleet is unanswered
                return s < 0.0f ? 0.0f : (s > 10.0f ? 10.0f : s);
            };

            // -- buy effects --
            auto buy = [&](int it){
                if (it <= IT_BAR_SLIME) {   // build a barracks of that mob type on the free tile we found
                    EnemyBarracks nb; nb.kind = (uint8_t)BARKIND[it]; nb.cd = 0.0f; nb.x = etile_x; nb.z = etile_z;
                    ebarracks.push_back(nb);
                    return;
                }
                switch (it) {
                    case IT_BOAT: { Boat b; b.id=next_boat_id++; b.kind=0; b.team=0; b.health=BOAT_MAX_HP; b.fire_cd=1.5f;
                        b.pos[0]=enemy_core_pos[0]-12.0f; b.pos[2]=channel_center(b.pos[0]); b.pos[1]=terrain.height(b.pos[0],b.pos[2]); b.yaw=3.14159f; boats.push_back(b); } break;
                    case IT_LAYER: { Boat b; b.id=next_boat_id++; b.kind=0; b.team=0; b.role=1; b.health=dc::game::MINELAYER_HP; b.fire_cd=2.0f;
                        b.pos[0]=enemy_core_pos[0]-10.0f; b.pos[2]=channel_center(b.pos[0]); b.pos[1]=terrain.height(b.pos[0],b.pos[2]); b.yaw=3.14159f; boats.push_back(b); } break;
                    case IT_SWEEPER: { Boat b; b.id=next_boat_id++; b.kind=0; b.team=0; b.role=2; b.health=dc::game::MINESWEEPER_HP; b.fire_cd=0.0f;
                        b.pos[0]=enemy_core_pos[0]-10.0f; b.pos[2]=channel_center(b.pos[0]); b.pos[1]=terrain.height(b.pos[0],b.pos[2]); b.yaw=3.14159f; boats.push_back(b); } break;
                    case IT_TURRET:  if (enemy_turret_n < ENEMY_TURRET_MAX) enemy_turret_n++; break;
                    case IT_CAVALRY: enemy_speed_mult = std::min(2.2f, enemy_speed_mult + 0.18f); break;
                    case IT_UPGRADE: if (lag_idx >= 0 && lag_idx < (int)ebarracks.size() && ebarracks[lag_idx].up < dc::game::BARRACKS_UP_MAX) ebarracks[lag_idx].up++; break;   // level the laggard barracks
                }
            };

            // -- decide on a cadence: (re)pick a savings target, then buy it once affordable --
            ai_decide_cd -= dt; ai_resample_cd -= dt;
            if (ai_decide_cd <= 0.0f) {
                ai_decide_cd = 0.5f;
                const float cur = (ai_target >= 0) ? score(ai_target) : 0.0f;
                const bool need_pick = (ai_target < 0) || (ai_resample_cd <= 0.0f) || cur < 2.0f
                                       || (ai_target < IT_N && !avail[ai_target]);
                if (need_pick) {
                    ai_resample_cd = CYCLE_LEN * 0.5f;   // re-evaluate the appended goal every half-day
                    const float MAX_SAVE = 90.0f;        // only chase goals reachable within ~90s
                    float w[IT_N], W = 0.0f;
                    for (int it=0; it<IT_N; ++it) {
                        float sc = avail[it] ? score(it) : 0.0f;
                        const float need = COST[it] - (float)enemy_gold;
                        const float tta  = need <= 0.0f ? 0.0f : need / std::max(0.5f, enemy_rate);
                        if (sc < 1.0f || tta > MAX_SAVE) sc = 0.0f;
                        w[it] = sc*sc; W += w[it];        // square -> favor the stronger advisors
                    }
                    if (W > 0.0f) {
                        float r = rand01(ai_rng) * W, acc = 0.0f; ai_target = IT_BAR_SKEL;
                        for (int it=0; it<IT_N; ++it) { acc += w[it]; if (r <= acc) { ai_target = it; break; } }
                    }
                }
                if (ai_target >= 0 && enemy_gold >= COST[ai_target]) {
                    const int bought = ai_target;
                    enemy_gold -= COST[bought];
                    buy(bought);
                    ai_unmet[bought] = 0.0f;
                    ai_target = -1;   // re-sample next decision
                    char lb[128];
                    std::snprintf(lb, sizeof lb, "BUY %-8s $%-3d  (score %.1f, $%d left)  pSub=%d pBoat=%d front=%.2f",
                        NAME[bought], (int)COST[bought], score(bought), (int)enemy_gold, player_mines, seen_boats, front_frac);
                    LOGLINE(lb);
                }
                char st[112];
                std::snprintf(st, sizeof st, "AI $%d (+%.1f/s)  want:%s  bks %zu(r%.0f)  troops %d/%d  spd x%.2f  turr %d",
                    (int)enemy_gold, enemy_rate, ai_target >= 0 ? NAME[ai_target] : "-",
                    ebarracks.size(), enemy_build_radius, troops_alive, enemy_troop_cap, enemy_speed_mult, enemy_turret_n);
                ai_status = st;
            }

            // -- BARRACKS PRODUCTION: each enemy barracks spawns its mob on a timer (free, like ours),
            // from its own tile, while the lane is below the concurrent cap. Runs every frame. --
            {
                int live = troops_alive;
                auto barint = [](uint8_t k) -> float {
                    switch ((dc::entity::EnemyKind)k) {
                        case dc::entity::EnemyKind::Demon: return 9.0f;
                        case dc::entity::EnemyKind::Troll: return 7.0f;
                        case dc::entity::EnemyKind::Insulter: return 6.0f;
                        case dc::entity::EnemyKind::Flamethrower: return 5.0f;
                        case dc::entity::EnemyKind::Slime: return 4.5f;
                        case dc::entity::EnemyKind::Flying: return 4.0f;
                        default: return 3.2f;   // skeleton / ranged / bat
                    }
                };
                auto kcap = [](uint8_t k) -> int {   // per-barracks active cap by kind (weak swarmers high)
                    switch ((dc::entity::EnemyKind)k) {
                        case dc::entity::EnemyKind::Bat: return 20;
                        case dc::entity::EnemyKind::Ranged: return 7;
                        case dc::entity::EnemyKind::Flamethrower: return 6;
                        case dc::entity::EnemyKind::Slime: return 6;
                        case dc::entity::EnemyKind::Troll: return 4;
                        case dc::entity::EnemyKind::Demon: return 3;
                        case dc::entity::EnemyKind::Insulter: return 2;
                        default: return 10;   // skeleton / melee / flying
                    }
                };
                // CHEAT: the closer we push, the faster + the higher each barracks' cap (panic boost).
                const float panic = 1.0f + 0.5f * front_frac;
                for (auto& b : ebarracks) {
                    b.cd -= dt;
                    if (b.cd > 0.0f) continue;
                    // Per-TYPE active cap (pooled across same-kind barracks, + this barracks' cap upgrade).
                    int type_cap = 0;
                    for (const auto& q : ebarracks) if (q.kind == b.kind) type_cap += kcap(q.kind) + dc::game::barracks_cap_bonus(q.up);
                    if (live >= 200 || aliveK[b.kind] >= static_cast<int>(type_cap * panic)) { b.cd = 1.0f; continue; }
                    b.cd = barint(b.kind) / panic;
                    const float jx = (rand01(ai_rng)-0.5f)*1.9f, jz = (rand01(ai_rng)-0.5f)*1.9f;   // spawn in a slightly different spot each time
                    auto& e = entities.spawn_enemy(b.x+jx, b.z+jz, (dc::entity::EnemyKind)b.kind, false);
                    // Enemy power comes from PAID barracks upgrades (like ours) — no free time-based
                    // scaling: +40% HP / +28% damage per upgrade level the AI bought for this barracks.
                    const float um = 1.0f + 0.40f * b.up, ud = 1.0f + 0.28f * b.up;
                    e.stats.max_health *= um; e.health = e.stats.max_health; e.stats.attack_damage *= ud;
                    ++live; ++aliveK[b.kind];
                }
            }

            // -- SEA-MINES: arm, then detonate on the OTHER team's boat that drifts into the blast.
            // (A warship just takes a chunk — it needs several; a minelayer/sweeper dies outright.) --
            for (auto& m : naval_mines) {
                if (m.arm < 1.0f) m.arm = std::min(1.0f, m.arm + dt * 0.5f);   // ~2s to arm
                if (m.arm < 1.0f) continue;
                const float BLAST = dc::game::MINE_BLAST_R;
                const uint8_t opp = m.team == 1 ? 0 : 1;   // hits boats of the OTHER team
                bool blew = false;
                for (auto& b : boats) if (b.team == opp && b.health > 0.0f) {
                    const float dx=b.pos[0]-m.pos[0], dz=b.pos[2]-m.pos[2];
                    if (dx*dx+dz*dz < BLAST*BLAST) { b.health -= dc::game::MINE_DAMAGE; blew = true; break; }
                }
                if (blew) {
                    // A loud watery BLAST: a cluster of bursts spread around + a tall central plume,
                    // so the mine clearly explodes with particles on every screen (booms replicate).
                    auto boom = [&](float bx, float by, float bz){ frame_booms.push_back(bx); frame_booms.push_back(by); frame_booms.push_back(bz); };
                    boom(m.pos[0], m.pos[1]+0.3f, m.pos[2]);
                    for (int k=0;k<5;++k){ const float a0 = k*1.2566f + m.pos[0];
                        boom(m.pos[0]+std::cos(a0)*1.2f, m.pos[1]+0.2f, m.pos[2]+std::sin(a0)*1.2f); }
                    boom(m.pos[0], m.pos[1]+1.4f, m.pos[2]);   // central water plume
                    m.arm = -1.0f;
                }
            }
            for (std::size_t i=0;i<naval_mines.size();) {
                if (naval_mines[i].arm < 0.0f) { naval_mines[i]=naval_mines.back(); naval_mines.pop_back(); } else ++i;
            }

            // -- periodic SNAPSHOT to the session log (full picture of both economies + the board) --
            log_timer -= dt;
            if (log_timer <= 0.0f) {
                log_timer = 5.0f;
                char ls[256];
                std::snprintf(ls, sizeof ls,
                    "SNAP our$=%d ourRate=%.1f/s | eGold=%d eRate=%.1f/s bks=%zu(r%.0f) troops=%d/%d eBoat=%d eLayer=%d eSweep=%d eTurret=%d eSpd=%.2f"
                    " | pWarship=%d pMines=%d ourSpd=%.2f allies=%zu front=%.2f core=%.0f eCore=%.0f want=%s",
                    currency, our_gold_rate, (int)enemy_gold, enemy_rate, ebarracks.size(), enemy_build_radius, troops_alive, enemy_troop_cap,
                    e_boats, e_layers, e_sweepers, enemy_turret_n, enemy_speed_mult,
                    seen_boats, player_mines, our_max_mspeed, allies.size(), front_frac, core_health, enemy_core_health,
                    ai_target >= 0 ? NAME[ai_target] : "-");
                LOGLINE(ls);
            }
        }

        // Taunts: age out the live ones, and (host) occasionally pick a nearby attacking
        // enemy to hurl an insult — rate-limited and capped so a big horde doesn't shout
        // all at once. Each new taunt replicates to everyone (floating text + TTS).
        for (std::size_t i = 0; i < taunts.size();) {
            taunts[i].age += dt;
            if (taunts[i].age >= TAUNT_LIFE) { taunts[i] = taunts.back(); taunts.pop_back(); }
            else ++i;
        }
        for (float& rc : react_cd) if (rc > 0.0f) rc -= dt;
        if (net.role != dc::net::Role::Client) {
            if (taunt_cd > 0.0f) taunt_cd -= dt;
            if (taunt_cd <= 0.0f && static_cast<int>(taunts.size()) < MAX_TAUNTS) {
                // Gather player positions (local + clients) to test "nearby".
                float pxz[16][2]; int np = 0;
                if (!dead) { pxz[np][0] = player.position[0]; pxz[np][1] = player.position[2]; ++np; }
                for (auto& hc : host_clients) if (hc.body.health > 0.0f && np < 16) { pxz[np][0] = hc.body.position[0]; pxz[np][1] = hc.body.position[2]; ++np; }
                // Find attacking enemies within range of any player; pick one at random.
                int best = -1; int seen = 0;
                for (std::size_t e = 0; e < entities.items.size(); ++e) {
                    const auto& en = entities.items[e];
                    if (en.type != dc::entity::EntityType::Enemy || !en.attacking) continue;
                    bool near = false;
                    for (int p = 0; p < np; ++p) {
                        const float dx = en.position[0] - pxz[p][0], dz = en.position[2] - pxz[p][1];
                        if (dx*dx + dz*dz <= TAUNT_RANGE * TAUNT_RANGE) { near = true; break; }
                    }
                    if (!near) continue;
                    // Reservoir sample one candidate without building a list.
                    ++seen; spark_rng = spark_rng * 1664525u + 1013904223u;
                    if (static_cast<int>(spark_rng % static_cast<uint32_t>(seen)) == 0) best = static_cast<int>(e);
                }
                if (best >= 0) {
                    const auto& en = entities.items[best];
                    const float hy = terrain.height(en.position[0], en.position[2])
                                   + (en.kind == dc::entity::EnemyKind::Flying ? dc::entity::FLY_HOVER + 1.4f : 2.4f);
                    spark_rng = spark_rng * 1664525u + 1013904223u;
                    spawn_taunt(en.position[0], hy, en.position[2], pick_line(false, 0xFFFFFFFFu), true);
                    taunt_cd = TAUNT_INTERVAL;
                }
            }
        }

        // Local player's hand-bone world position this frame (set at the sword render
        // below), so elemental sparks emit right off the blade.
        vec3 blade_pos = {0.0f, 0.0f, 0.0f}; bool blade_ok = false;
        // Wizard magic shields raised this frame (local + remotes), drawn in the particle pass.
        struct MagicShield { float x, y, z, yaw; };
        std::vector<MagicShield> magic_shields;

        // Build the animation layers for this frame: walk drives the body, punch
        // is masked to the armL bone so you can punch while walking. (No layers
        // when idle -> rest pose.)
        layers.clear();
        // The player renders as its CLASS MODEL (knight/wizard, gear baked in). Pose with
        // that model's own walk/punch/block clips + bone nodes.
        dc::renderer::ModelData& pmd = class_md(my_look.weapon_class);
        const bool class_baked = class_custom(my_look.weapon_class);
        // Knight dodge-roll: a full-body roll clip (somersault + tuck) overrides everything.
        // Pick the forward / left / right variant from roll_dir (backward dodge sets roll_t=0).
        const dc::renderer::Animation* roll_clip =
            (roll_dir == 1 && pmd.roll_l.valid()) ? &pmd.roll_l :
            (roll_dir == 2 && pmd.roll_r.valid()) ? &pmd.roll_r : &pmd.roll;
        const bool rolling = (roll_t > 0.0f && my_look.weapon_class == 0 && roll_clip->valid());
        if (rolling) {
            const float dur = roll_clip->duration;
            layers.push_back({ roll_clip, (1.0f - roll_t / ROLL_DUR) * dur, -1, false });  // whole body, one-shot
        } else {
            // Idle breathing/sway as the base layer when standing still (walk replaces it when moving).
            if (!moving && pmd.idle.valid()) layers.push_back({ &pmd.idle, run_time, -1 });
            if (moving)   layers.push_back({ &pmd.walk,  anim_time,  -1 });
            if (punching) layers.push_back({ &pmd.punch, punch_time, pmd.arm_l_node });
            if (blocking) layers.push_back({ &pmd.block, block_time, pmd.arm_r_node, false });  // right arm, one-shot: play to end and hold (no loop)
        }
        // Pose the player, and grab the head + hand bone world matrices as gear sockets.
        dc::renderer::Mat4 head_world;
        dc::renderer::Mat4 l_hand_world;
        dc::renderer::Mat4 r_hand_world;
        // Third person (default): pitch the BODY so your avatar hinges to aim, like remotes.
        // First person (V): head-look only (body undrawn; gear aims via the viewmodel tilt).
        const bool tp = !first_person;
        std::vector<float> my_bscale = bone_scale_for(my_look);   // silly proportions (gear follows the scaled hands)
        dc::renderer::pose_model(pmd, layers, tp ? 0.0f : player.pitch, part_world,
                                 { pmd.head_node, pmd.hand_l_node, pmd.hand_r_node },
                                 { &head_world, &l_hand_world, &r_hand_world },
                                 tp ? player.pitch : 0.0f, &my_bscale);

        // Avatar placement: stand at the player's XZ, facing the look direction.
        // The model's origin sits at its waist (local feet at y~=-1.0), so lift it so
        // the feet rest on the floor; follow the player's vertical position so the
        // avatar rises with the camera on a jump.
        const float MODEL_FOOT_LIFT = 1.0f;
        const float MODEL_YAW_OFFSET = glm_rad(-90.0f);  // tune to face forward (try -90 / 0 / 180)
        float feet_y = (player.position[1] - dc::world::EYE_HEIGHT) + MODEL_FOOT_LIFT;
        // Wading bob: sink a little + bob up and down on the water surface.
        if (player_in_water) feet_y += -0.35f + std::sin(t_now * 3.0f) * 0.1f;
        mat4 placement;
        glm_mat4_identity(placement);
        vec3 foot = { player.position[0], feet_y, player.position[2] };
        glm_translate(placement, foot);
        // A forward roll faces the DIRECTION of the dodge (so a diagonal W+A/W+D somersault
        // goes where you're actually travelling); all other states face the camera yaw.
        const float place_yaw = (rolling && roll_dir == 0) ? roll_yaw : player.yaw;
        glm_rotate_y(placement, -place_yaw + MODEL_YAW_OFFSET, placement);

        int w, h; window.framebuffer_size(w, h);
        camera.third_person = tp;   // third person by default; V drops to first person
        renderer.begin_frame(*map, camera, player, dt, w, h);
        // Day/night ambient: bright noon -> dim night (the dynamic lights carry the night).
        renderer.set_ambient(0.5f + 3.0f * daylight01());
        renderer.draw_map(mesh);
        renderer.draw_terrain(terrain_mesh, terrain_color);
        // --- GRASS: single-color POINTY blades on flat grassy ground around the player. Blades
        // grow IN from the ground toward the rim (height eases to 0) so they don't pop as you walk.
        {
            static dc::renderer::Mesh grass_mesh;
            std::vector<float> gv;
            const float GR = 26.0f, SP = 1.0f;
            const float px = player.position[0], pz = player.position[2];
            auto h2 = [](int a, int b){ uint32_t x = (uint32_t)(a*73856093) ^ (uint32_t)(b*19349663); x ^= x>>13; x*=0x5bd1e995u; x^=x>>15; return (x & 0xffffu)/65535.0f; };
            const int c0 = (int)std::floor((px-GR)/SP), c1 = (int)std::floor((px+GR)/SP);
            const int r0 = (int)std::floor((pz-GR)/SP), r1 = (int)std::floor((pz+GR)/SP);
            // a single tapered TRIANGLE blade (wide base -> sharp point) — never bulbous.
            auto blade = [&](float bx, float gy, float bz, float tx, float ty, float tz, float w, float ux, float uz){
                auto P=[&](float x,float y,float z){ gv.insert(gv.end(), { x,y,z, 0.0f,1.0f,0.0f, 0.f,0.f,0.f }); };
                P(bx-ux*w, gy, bz-uz*w); P(bx+ux*w, gy, bz+uz*w); P(tx, ty, tz);
            };
            for (int gc = c0; gc <= c1; ++gc) for (int gr = r0; gr <= r1; ++gr) {
                const float hx = h2(gc, gr), hz = h2(gc+7, gr-3), hh = h2(gc-5, gr+11);
                const float bx = (gc+hx)*SP, bz = (gr+hz)*SP;
                const float dx = bx-px, dz = bz-pz, d2 = dx*dx+dz*dz;
                if (d2 > GR*GR) continue;
                if (in_water(bx, bz)) continue;
                const float gy = terrain.height(bx, bz);
                if (gy > 6.0f) continue;                                  // not on the high rocky plateaus
                if (std::fabs(terrain.height(bx+0.4f,bz)-gy) + std::fabs(terrain.height(bx,bz+0.4f)-gy) > 0.45f) continue;  // skip steep/rock
                const float r = std::sqrt(d2)/GR, edge = 1.0f - r*r;      // ease height to 0 toward the rim
                if (hh > 0.55f) continue;                                 // ~half the cells get a tuft
                const float height = (0.26f + hh*0.34f) * edge;
                if (height < 0.03f) continue;
                const float sway = std::sin(t_now*1.5f + bx*0.5f + bz*0.45f) * 0.10f * height + 0.03f;
                const float tx = bx + 0.5f*sway, tz = bz + 0.85f*sway;    // wind-leaned tip
                const float w = 0.035f;
                blade(bx, gy, bz, tx, gy+height, tz, w, 1.0f, 0.0f);      // crossed pair for a bit of volume
                blade(bx, gy, bz, tx, gy+height, tz, w, 0.0f, 1.0f);
            }
            if (!gv.empty()) { grass_mesh.upload(gv); vec3 c = {0.30f, 0.50f, 0.20f}; renderer.draw_terrain(grass_mesh, c, true); }
        }
        // Base-area floor: a tinted disc on the ground within the buildable radius, so players
        // can see exactly where they can build. Rebuilt each frame (radius grows when you buy area).
        {
            static dc::renderer::Mesh base_floor_mesh;
            std::vector<float> fv;
            const int SEG = 64;
            const float rr = shield_radius;
            auto V = [&](float x, float z) {
                fv.insert(fv.end(), { x, terrain.height(x, z) + 0.03f, z, 0.0f, 1.0f, 0.0f, 0.f, 0.f, 0.f });
            };
            for (int i = 0; i < SEG; ++i) {
                const float a0 = 6.2831853f * i / SEG, a1 = 6.2831853f * (i + 1) / SEG;
                V(core_pos[0], core_pos[2]);
                V(core_pos[0] + std::cos(a0)*rr, core_pos[2] + std::sin(a0)*rr);
                V(core_pos[0] + std::cos(a1)*rr, core_pos[2] + std::sin(a1)*rr);
            }
            base_floor_mesh.upload(fv);
            vec3 base_tint = { 0.20f, 0.34f, 0.46f };   // cool blue-grey "your turf" tint
            renderer.draw_terrain(base_floor_mesh, base_tint, true);
        }
        // Slime puddles: flat sickly-green discs on the ground (fade as they age).
        {
            const bool cl = (net.role == dc::net::Role::Client);
            const std::size_t ns = cl ? net_slime_patches.size() : slime_patches.size();
            if (ns > 0) {
                static dc::renderer::Mesh slime_mesh;
                std::vector<float> sv;
                const int SEG = 14;
                auto V = [&](float x, float z) { sv.insert(sv.end(), { x, terrain.height(x, z) + 0.04f, z, 0.0f, 1.0f, 0.0f, 0.f, 0.f, 0.f }); };
                for (std::size_t i = 0; i < ns; ++i) {
                    float cx, cz, rad;
                    if (cl) { const auto& s = net_slime_patches[i]; cx=s.x; cz=s.z; rad=s.radius; }
                    else    { const auto& s = slime_patches[i]; cx=s.pos[0]; cz=s.pos[2]; rad=s.radius; }
                    for (int k = 0; k < SEG; ++k) {
                        const float a0 = 6.2831853f*k/SEG, a1 = 6.2831853f*(k+1)/SEG;
                        V(cx, cz); V(cx+std::cos(a0)*rad, cz+std::sin(a0)*rad); V(cx+std::cos(a1)*rad, cz+std::sin(a1)*rad);
                    }
                }
                slime_mesh.upload(sv);
                vec3 goo = { 0.35f, 0.85f, 0.20f };   // sickly green
                renderer.draw_terrain(slime_mesh, goo, true);
            }
        }
        // The RIVER: a winding water channel down the lane, REBUILT each frame with animated
        // ripples (a moving sine surface) so it reads as flowing water. Two stacked layers (a
        // darker deep sheet + a brighter rippling top) make it look a little thicker.
        {
            std::vector<float> rv;
            const float x0 = core_pos[0] + 8.0f, x1 = enemy_core_pos[0] - 8.0f;
            const float rt = t_now;
            auto centerhalf = [&](float xx, float& cz, float& hh) {
                const float t = (xx - x0) / (x1 - x0);
                cz = riverZ + std::sin(t * 6.2831853f * 1.6f) * 4.5f;
                const float bump = std::exp(-((t - 0.5f) * (t - 0.5f)) / (2.0f * 0.018f));
                hh = 3.2f + bump * 10.0f;
            };
            // surface Y at (x,z): the water sits a bit ABOVE the flat ground; ripple troughs are
            // kept above ground (base 0.30 > total ripple amplitude 0.13) so the ground never
            // clips through a wave trough.
            auto surf = [&](float x, float z, float base) {
                return terrain.height(x, z) + base
                     + std::sin(x * 0.9f + rt * 2.3f) * 0.06f
                     + std::sin(z * 1.3f - rt * 1.7f) * 0.04f
                     + std::sin((x + z) * 0.5f + rt * 3.1f) * 0.03f;
            };
            auto V = [&](float x, float y, float z) { rv.insert(rv.end(), { x, y, z, 0.0f, 1.0f, 0.0f, 0.f, 0.f, 0.f }); };
            for (float x = x0; x < x1; x += 1.0f) {
                const float xb = std::min(x + 1.0f, x1);
                float ca, ha, cb, hb; centerhalf(x, ca, ha); centerhalf(xb, cb, hb);
                const float base = 0.30f;
                V(x,  surf(x, ca-ha, base), ca - ha); V(xb, surf(xb, cb-hb, base), cb - hb); V(xb, surf(xb, cb+hb, base), cb + hb);
                V(x,  surf(x, ca-ha, base), ca - ha); V(xb, surf(xb, cb+hb, base), cb + hb); V(x,  surf(x, ca+ha, base), ca + ha);
            }
            river_mesh.upload(rv);
            vec3 deep = { 0.06f, 0.20f, 0.42f };
            renderer.draw_water(river_mesh, deep);   // reflective animated water (ripples + sun glints)
        }
        const float GHOST_ALPHA = 0.18f;               // dead remote players render faint + translucent
        // First person (default): don't draw our own body/helmet — only the held sword
        // and shield, as a "viewmodel" tilted by pitch about the eye so the gear aims
        // where we point. Third person (default): draw the full body + helmet instead,
        // and attach the gear normally (no view tilt). (`tp` computed above.)
        mat4 vm_place;
        {
            mat4 tilt; glm_mat4_identity(tilt);
            vec3 eye = { player.position[0], player.position[1], player.position[2] };
            vec3 neg = { -eye[0], -eye[1], -eye[2] };
            glm_translate(tilt, eye);
            glm_rotate(tilt, player.pitch, renderer.cam_right);   // tilt up/down about camera-right
            glm_translate(tilt, neg);
            glm_mat4_mul(tilt, placement, vm_place);
        }
        // First-person viewmodel: the rig's hands sit at waist height, below the view —
        // lift the gear up + push it forward along the look so both sword and shield
        // read on screen. (Tune these two if it sits too high/low or clips.)
        if (!camera.third_person) {
            const float VM_RAISE = 0.2f, VM_FWD = 0.5f;   // low: gear just peeks into the lower view
            vec3 fr; player.front(fr);
            vec3 d = { fr[0] * VM_FWD, VM_RAISE + fr[1] * VM_FWD, fr[2] * VM_FWD };
            mat4 off; glm_mat4_identity(off); glm_translate(off, d);
            glm_mat4_mul(off, vm_place, vm_place);   // world-space lift, applied to the whole viewmodel
        }
        // The matrix the hand-attached gear hangs off of: real placement in 3rd person,
        // the pitch-tilted viewmodel placement in 1st person.
        mat4 gear_place;
        glm_mat4_copy(tp ? placement : vm_place, gear_place);

        if (tp) {   // third-person: draw the class body (gear baked in)
            vec3 body_color = { 1.0f, 1.0f, 1.0f };   // white -> baked class colors show
            if (!class_baked) { body_color[0] = 0.52f; body_color[1] = 0.55f; body_color[2] = 0.62f;   // generic fallback tint
                                if (my_look.weapon_class == 1) { body_color[0] = 0.22f; body_color[1] = 0.26f; body_color[2] = 0.62f; } }
            if (dead) { vec3 pale = { 0.55f, 0.65f, 0.95f }; glm_vec3_copy(pale, body_color); }
            else if (player.hit_flash > 0.0f) {
                vec3 red = { 1.0f, 0.1f, 0.1f };
                glm_vec3_lerp(body_color, red, player.hit_flash / dc::entity::FLASH_TIME, body_color);
            }
            // Wizard dodge: the model fully VANISHES while dodging, leaving only a mist trail,
            // and reappears when the i-frames end.
            bool draw_body = true;
            if (!dead && my_look.weapon_class == 1 && player.iframes > 0.0f) {
                draw_body = false;   // gone -> just the trailing mist marks the path
                burst_mist(player.position[0], player.position[1] - dc::world::EYE_HEIGHT + 0.9f, player.position[2], 6);
            }
            if (draw_body)
                renderer.draw_model(class_mdl(my_look.weapon_class), part_world, placement, body_color, dead ? GHOST_ALPHA : 1.0f);
            if (!dead && !class_baked) {   // only the fallback rig needs a drawn face / helmet
                mat4 hp; glm_mat4_mul(placement, head_world.m, hp);
                vec3 hcen = { hp[3][0], hp[3][1], hp[3][2] };
                draw_face_at(hcen, player.yaw, my_look);
                mat4 helmet_place; glm_mat4_mul(placement, head_world.m, helmet_place);
                vec3 helmet_color = { 1.0f, 1.0f, 1.0f };
                renderer.draw_model(helmet_model, helmet_offset, helmet_place, helmet_color);
            }
        }

        // Gear hidden while a ghost (dead = no weapon).
        if (!dead) {
        // Mark the hand bone for elemental sparks (the weapon itself is baked into the class
        // model). The fallback rig still draws a separate sword/staff.
        const bool is_wiz = (my_look.weapon_class == 1);
        if (player.weapon && (throwns.empty() || is_wiz)) {
            mat4 sword_place;
            glm_mat4_mul(gear_place, l_hand_world.m, sword_place);
            blade_pos[0] = sword_place[3][0]; blade_pos[1] = sword_place[3][1]; blade_pos[2] = sword_place[3][2];
            blade_ok = true;   // emit sparks from the hand bone
            if (!class_baked) {
                vec3 sws = { player.sword_scale, player.sword_scale, player.sword_scale };
                glm_scale(sword_place, sws);
                vec3 sword_color = { 0.8f, 0.8f, 0.9f };
                renderer.draw_model(sword_model, sword_offset, sword_place, sword_color);
            }
        }
        // Thrown sword: spinning in flight. Match the in-hand size by reusing the
        // hand bone's world scale (the player rig is ~0.22x), times the throw_size
        // upgrade. Without this it'd draw at full model scale (way too big).
        if (!throwns.empty()) {
            float rig_scale = std::sqrt(l_hand_world.m[0][0] * l_hand_world.m[0][0]
                                      + l_hand_world.m[0][1] * l_hand_world.m[0][1]
                                      + l_hand_world.m[0][2] * l_hand_world.m[0][2]);
            float s = rig_scale * player.sword_scale * (player.weapon ? player.weapon->throw_size : 1.0f);
            vec3 sc = { s, s, s };
            vec3 sword_color = { 0.85f, 0.85f, 0.95f };
            for (const auto& th : throwns) {
                mat4 tplace;
                glm_mat4_identity(tplace);
                vec3 tpos = { th.pos[0], th.pos[1], th.pos[2] };   // 3D flight (follows aim)
                glm_translate(tplace, tpos);
                glm_rotate_y(tplace, th.spin, tplace);
                glm_scale(tplace, sc);
                renderer.draw_model(sword_model, sword_offset, tplace, sword_color);
            }
        }

        // Orbit special: spinning swords (knight) or glowing arcane ORBS (wizard) circling you.
        if (orbit.active && player.weapon) {
            float rig_scale = std::sqrt(l_hand_world.m[0][0] * l_hand_world.m[0][0]
                                      + l_hand_world.m[0][1] * l_hand_world.m[0][1]
                                      + l_hand_world.m[0][2] * l_hand_world.m[0][2]);
            float s = rig_scale * player.sword_scale;
            const bool wiz = (my_look.weapon_class == 1);
            const auto& w = *player.weapon;
            std::vector<float> orb_geo;   // wizard: 3D glowing spheres drawn IMMEDIATELY (the particle
                                          // buffer is cleared after this point, so billboards would vanish).
            auto sph = [&](float cx, float cy, float cz, float rad) {
                const int ST = 4, SL = 7; const float PI = 3.14159265f;
                auto sp = [](float t, float p, float& x, float& y, float& z){ x=std::sin(t)*std::cos(p); y=std::cos(t); z=std::sin(t)*std::sin(p); };
                auto V = [&](float x,float y,float z){ orb_geo.insert(orb_geo.end(), { cx+x*rad, cy+y*rad, cz+z*rad, x,y,z, 0.f,0.f,0.f }); };
                for (int i=0;i<ST;++i){ const float t0=PI*i/ST,t1=PI*(i+1)/ST;
                    for (int j=0;j<SL;++j){ const float p0=2*PI*j/SL,p1=2*PI*(j+1)/SL; float ax,ay,az,bx,by,bz,c2x,c2y,c2z,dx,dy,dz;
                        sp(t0,p0,ax,ay,az);sp(t1,p0,bx,by,bz);sp(t1,p1,c2x,c2y,c2z);sp(t0,p1,dx,dy,dz);
                        V(ax,ay,az);V(bx,by,bz);V(c2x,c2y,c2z); V(ax,ay,az);V(c2x,c2y,c2z);V(dx,dy,dz); } }
            };
            for (int i = 0; i < w.orbit_count; ++i) {
                float a = orbit.angle + (6.2831853f * i) / w.orbit_count;
                vec3 opos = { player.position[0] + std::cos(a) * w.orbit_radius,
                              (player.position[1] - dc::world::EYE_HEIGHT) + 1.2f,
                              player.position[2] + std::sin(a) * w.orbit_radius };
                if (wiz) {
                    sph(opos[0], opos[1], opos[2], 0.26f);   // a glowing arcane orb (3D, drawn below)
                } else {
                    mat4 op; glm_mat4_identity(op);
                    glm_translate(op, opos);
                    glm_rotate_y(op, orbit.spin, op);
                    vec3 osc = { s, s, s }; glm_scale(op, osc);
                    vec3 oc = { 0.85f, 0.85f, 0.95f };
                    renderer.draw_model(sword_model, sword_offset, op, oc);
                }
            }
            if (!orb_geo.empty()) {
                static dc::renderer::Mesh orb_mesh; orb_mesh.upload(orb_geo);
                vec3 oc = { 0.85f, 0.45f, 1.35f };   // glowing violet arcane energy
                renderer.draw_glow(orb_mesh, oc);
            }
        }

        // Shield: the knight's is baked into the class model; the wizard raises a translucent
        // magic barrier (drawn in the particle pass below). The fallback rig uses the prop.
        if (player.shield && !class_baked) {
            mat4 shield_place;
            glm_mat4_mul(gear_place, r_hand_world.m, shield_place);
            vec3 shield_color = { 0.5f, 0.5f, 0.8f };
            renderer.draw_model(shield_model, shield_offset, shield_place, shield_color);
        }
        // Wizard magic shield: a glowing translucent hex barrier off the right hand while
        // blocking (rendered in the particle pass). Position from the posed hand bone.
        if (player.shield && my_look.weapon_class == 1 && blocking) {
            mat4 mw; glm_mat4_mul(gear_place, r_hand_world.m, mw);
            vec3 f; player.front(f);
            magic_shields.push_back({ mw[3][0] + f[0]*0.35f, mw[3][1], mw[3][2] + f[2]*0.35f, player.yaw });
        }
        }   // end if (!dead)

        // Draw each chest, lid posed by its open_t (open clip only animates the lid).
        // White tint -> the per-part material colors from the .glb show through unchanged.
        vec3 chest_color = { 1.0f, 1.0f, 1.0f };
        for (const auto& ch : chests) {
            std::vector<dc::renderer::AnimLayer> cl = {{ &chest_data.open, ch.open_t, -1, false }};  // one-shot: hold open
            dc::renderer::pose_model(chest_data, cl, 0.0f, chest_part_world);
            mat4 cplace;
            glm_mat4_identity(cplace);
            float ccx = (ch.col + 0.5f) * dc::world::TILE, ccz = (ch.row + 0.5f) * dc::world::TILE;
            vec3 cpos = { ccx, terrain.height(ccx, ccz), ccz };   // sit on the terrain surface
            glm_translate(cplace, cpos);            // move to the tile (origin at the chest's base)
            vec3 cscale = { 0.75f, 0.75f, 0.75f };
            glm_scale(cplace, cscale);              // half size, scaled around its base -> stays on floor
            renderer.draw_model(chest_model, chest_part_world, cplace, chest_color);
        }

        // Draw enemies — reuse the player model; melee tinted green, ranged purple,
        // posed by their own walk/attack clocks and facing the player. Flyers are
        // hovering cubes (batched + drawn solid below) to prove out flying enemies.
        vec3 enemy_color  = { 0.25f, 0.80f, 0.30f };
        vec3 ranged_color = { 0.70f, 0.30f, 0.85f };
        vec3 flame_color  = { 0.85f, 0.18f, 0.12f };   // flamethrower bruiser: angry red
        std::vector<float> flyer_verts;   // batched cube faces (9-float world-space verts)
        std::vector<float> slime_blob_verts;   // slime enemy bodies (drawn green)
        auto box_face_into = [&](std::vector<float>& buf, float ax,float ay,float az, float bx,float by,float bz,
                            float cx,float cy,float cz, float dx,float dy,float dz, float nx,float ny,float nz) {
            auto V = [&](float x,float y,float z){ buf.insert(buf.end(), {x,y,z,nx,ny,nz,0.f,0.f,0.f}); };
            V(ax,ay,az); V(bx,by,bz); V(cx,cy,cz);  V(ax,ay,az); V(cx,cy,cz); V(dx,dy,dz);
        };
        auto cube_into = [&](std::vector<float>& buf, float cx,float cy,float cz, float hx,float hy,float hz) {
            const float x0=cx-hx,x1=cx+hx,y0=cy-hy,y1=cy+hy,z0=cz-hz,z1=cz+hz;
            box_face_into(buf, x0,y0,z1,x1,y0,z1,x1,y1,z1,x0,y1,z1, 0,0,1);   box_face_into(buf, x1,y0,z0,x0,y0,z0,x0,y1,z0,x1,y1,z0, 0,0,-1);
            box_face_into(buf, x1,y0,z1,x1,y0,z0,x1,y1,z0,x1,y1,z1, 1,0,0);   box_face_into(buf, x0,y0,z0,x0,y0,z1,x0,y1,z1,x0,y1,z0, -1,0,0);
            box_face_into(buf, x0,y1,z1,x1,y1,z1,x1,y1,z0,x0,y1,z0, 0,1,0);   box_face_into(buf, x0,y0,z0,x1,y0,z0,x1,y0,z1,x0,y0,z1, 0,-1,0);
        };
        auto append_cube = [&](float cx,float cy,float cz, float hx,float hy,float hz) { cube_into(flyer_verts, cx,cy,cz, hx,hy,hz); };
        for (const auto& en : entities.items) {
            if (en.type != dc::entity::EntityType::Enemy) continue;
            if (en.kind == dc::entity::EnemyKind::Slime) {   // a wobbling green blob (no humanoid model)
                const float gx = en.position[0], gz = en.position[2], gy = terrain.height(gx, gz);
                const float wob = 1.0f + 0.12f * std::sin(t_now * 4.0f + gx);
                const float w = 1.3f * (en.elite ? dc::entity::ELITE_SCALE : 1.0f);
                cube_into(slime_blob_verts, gx, gy + 0.55f / wob, gz, w * wob, 0.55f / wob, w * wob);   // squash + jiggle
                cube_into(slime_blob_verts, gx, gy + 0.95f / wob, gz, w * 0.55f * wob, 0.4f / wob, w * 0.55f * wob);  // bulge on top
                continue;
            }
            if (en.kind == dc::entity::EnemyKind::Bat && bat_loaded) {   // hovering bat, wings flapping
                const float gx = en.position[0], gz = en.position[2];
                const float cy = terrain.height(gx, gz) + dc::entity::FLY_HOVER + 0.12f * std::sin(t_now * 3.0f + gx);
                std::vector<dc::renderer::AnimLayer> bl;
                bl.push_back({ &bat_data.walk, t_now * 1.6f, -1 });   // flap continuously
                dc::renderer::pose_model(bat_data, bl, 0.0f, enemy_part_world);
                mat4 eplace; glm_mat4_identity(eplace);
                vec3 epos = { gx, cy, gz };
                glm_translate(eplace, epos);
                glm_rotate_y(eplace, -en.yaw + MODEL_YAW_OFFSET, eplace);   // face the target (same convention as the skeleton)
                if (en.elite) { vec3 es = {dc::entity::ELITE_SCALE, dc::entity::ELITE_SCALE, dc::entity::ELITE_SCALE}; glm_scale(eplace, es); }
                vec3 col = { 1.0f, 1.0f, 1.0f };   // white tint -> the model's purple/black materials show
                if (en.elite) { vec3 gold = {1.0f, 0.82f, 0.25f}; glm_vec3_copy(gold, col); }
                if (en.hit_flash > 0.0f) { vec3 red = {1.0f,0.2f,0.2f}; glm_vec3_lerp(col, red, en.hit_flash/dc::entity::FLASH_TIME, col); }
                renderer.draw_model(bat_model, enemy_part_world, eplace, col);
                continue;
            }
            if (en.kind == dc::entity::EnemyKind::Flying) {   // hovering eye (cube fallback)
                const float gx = en.position[0], gz = en.position[2];
                const float cy = terrain.height(gx, gz) + dc::entity::FLY_HOVER;
                if (eye_loaded) {
                    // Face the target: en.yaw tracks it horizontally; pitch toward the nearest
                    // player so the eye peers down/up. The tentacles undulate via the looping
                    // "walk" clip. Gentle hover bob.
                    const float bob = 0.12f * std::sin(t_now * 2.0f + gx);
                    const float eye_y = cy + bob;
                    float best = 1e30f, ty = eye_y;
                    auto consider = [&](float px, float py, float pz) {
                        const float dx = px - gx, dz = pz - gz, d2 = dx*dx + dz*dz;
                        if (d2 < best) { best = d2; ty = py; }
                    };
                    if (!dead) consider(player.position[0], player.position[1], player.position[2]);
                    for (const auto& rp : remotes) if (!rp.ghost) consider(rp.pos[0], rp.pos[1], rp.pos[2]);
                    const float horiz = std::sqrt(best) > 0.3f ? std::sqrt(best) : 0.3f;
                    float eye_pitch = std::atan2(ty - eye_y, horiz);          // + = look up
                    if (eye_pitch >  1.3f) eye_pitch =  1.3f;
                    if (eye_pitch < -1.3f) eye_pitch = -1.3f;
                    std::vector<dc::renderer::AnimLayer> evl;
                    if (eye_data.walk.valid()) evl.push_back({ &eye_data.walk, t_now * 1.0f, -1 });   // tentacle wave
                    dc::renderer::pose_model(eye_data, evl, 0.0f, enemy_part_world);
                    mat4 eplace; glm_mat4_identity(eplace);
                    vec3 epos = { gx, eye_y, gz };
                    glm_translate(eplace, epos);
                    glm_rotate_y(eplace, -en.yaw + MODEL_YAW_OFFSET, eplace);   // yaw toward target (+Y model, like the bat)
                    glm_rotate_x(eplace, eye_pitch, eplace);                    // pitch up/down about the local right axis
                    if (en.elite) { vec3 es = {dc::entity::ELITE_SCALE, dc::entity::ELITE_SCALE, dc::entity::ELITE_SCALE}; glm_scale(eplace, es); }
                    vec3 col = { 1.0f, 1.0f, 1.0f };
                    if (en.elite) { vec3 gold = {1.0f, 0.82f, 0.25f}; glm_vec3_copy(gold, col); }
                    if (en.hit_flash > 0.0f) { vec3 red = {1.0f,0.2f,0.2f}; glm_vec3_lerp(col, red, en.hit_flash/dc::entity::FLASH_TIME, col); }
                    renderer.draw_model(eye_model, enemy_part_world, eplace, col);
                } else {
                    const float cs = en.elite ? dc::entity::ELITE_SCALE : 1.0f;
                    append_cube(gx, cy, gz, 0.7f * cs, 1.0f * cs, 0.7f * cs);
                }
                continue;
            }
            // Skeletons use their own model + clips; everything else uses the shared rig.
            // Pick the model per kind: skeleton (melee), gnome (flamethrower), mage (ranged);
            // each falls back to the shared player rig + tint if its .glb is missing.
            const bool is_skel  = (en.kind == dc::entity::EnemyKind::Skeleton) && skeleton_loaded;
            const bool is_gnome = (en.kind == dc::entity::EnemyKind::Flamethrower) && gnome_loaded;
            const bool is_mage  = (en.kind == dc::entity::EnemyKind::Ranged) && mage_loaded;
            const bool is_troll = (en.kind == dc::entity::EnemyKind::Troll) && troll_loaded;
            const bool is_demon = (en.kind == dc::entity::EnemyKind::Demon) && demon_loaded;
            const bool is_insult = (en.kind == dc::entity::EnemyKind::Insulter) && insulter_loaded;
            const bool custom   = is_skel || is_gnome || is_mage || is_troll || is_demon || is_insult;
            const dc::renderer::ModelData& md = is_skel ? skeleton_data : is_gnome ? gnome_data : is_mage ? mage_data
                                              : is_troll ? troll_data : is_demon ? demon_data : is_insult ? insulter_data : model_data;
            dc::renderer::Model& mdl = is_skel ? skeleton_model : is_gnome ? gnome_model : is_mage ? mage_model
                                     : is_troll ? troll_model : is_demon ? demon_model : is_insult ? insulter_model : player_model;
            // The troll's weighted swing and the demon's yell animate the whole body, so play
            // their attack UNMASKED; other enemies mask the punch to armL (so they swing while walking).
            const bool full_body_attack = is_troll || is_demon || is_insult;   // insulter raises his RIGHT arm to flip the bird — full body
            std::vector<dc::renderer::AnimLayer> el;
            // Troll: its clip's slam CONTACT is the final keyframe, so sample NORMALIZED against
            // the wind-up — the club hits exactly when the damage lands (no early/late slam).
            float punch_sample = en.attack_time;
            if (is_troll && md.punch.duration > 0.0f && dc::entity::TROLL_WINDUP > 0.0f)
                punch_sample = (en.attack_time / dc::entity::TROLL_WINDUP) * md.punch.duration;
            if (en.attacking)        el.push_back({ &md.punch, punch_sample, full_body_attack ? -1 : md.arm_l_node });
            else if (en.anim_time > 0.0f) el.push_back({ &md.walk, en.anim_time, -1 });
            else if (md.idle.valid()) el.push_back({ &md.idle, t_now + en.position[0], -1 });   // standing still: breathe
            dc::renderer::pose_model(md, el, 0.0f, enemy_part_world);
            mat4 eplace;
            glm_mat4_identity(eplace);
            vec3 epos = { en.position[0], terrain.height(en.position[0], en.position[2]) + MODEL_FOOT_LIFT, en.position[2] };
            glm_translate(eplace, epos);
            glm_rotate_y(eplace, -en.yaw + MODEL_YAW_OFFSET, eplace);
            const float big = (is_troll || is_demon) ? 1.9f : 1.0f;   // trolls + demons are huge
            // Per-enemy size variety from a stable hash of its id (same on every peer; cosmetic).
            const float evary = 0.85f + (en.id % 7) * (0.40f / 6.0f);   // 0.85..1.25
            { vec3 bs = { big*evary, big*evary, big*evary }; glm_scale(eplace, bs); }
            if (en.elite) { vec3 es = {dc::entity::ELITE_SCALE, dc::entity::ELITE_SCALE, dc::entity::ELITE_SCALE}; glm_scale(eplace, es); }
            vec3 col;
            if (custom) { vec3 white = { 1.0f, 1.0f, 1.0f }; glm_vec3_copy(white, col); }   // white tint -> the model's own material colors show
            else glm_vec3_copy(en.kind == dc::entity::EnemyKind::Ranged ? ranged_color
                             : en.kind == dc::entity::EnemyKind::Flamethrower ? flame_color : enemy_color, col);
            if (en.elite) { vec3 gold = { 1.0f, 0.78f, 0.25f }; glm_vec3_lerp(col, gold, 0.6f, col); }  // golden sheen
            if (en.hit_flash > 0.0f) {                 // flash red when struck
                vec3 red = { 1.0f, 0.1f, 0.1f };
                glm_vec3_lerp(col, red, en.hit_flash / dc::entity::FLASH_TIME, col);
            }
            // Heckled by a friendly BILL's aura -> sickly YELLOW tint (their attacks are weakened).
            {
                bool debuffed = false;
                const bool clb = (net.role == dc::net::Role::Client);
                if (clb) { for (const auto& a : net_allies) if (dc::game::mob_type(a.kind).visual == dc::game::MobVisual::Insulter) {
                        const float dx=a.x-en.position[0], dz=a.z-en.position[2]; if (dx*dx+dz*dz < 81.0f) { debuffed=true; break; } } }
                else { for (const auto& a : allies) if (dc::game::mob_type(a.kind).visual == dc::game::MobVisual::Insulter) {
                        const float dx=a.pos[0]-en.position[0], dz=a.pos[2]-en.position[2]; if (dx*dx+dz*dz < 81.0f) { debuffed=true; break; } } }
                if (debuffed) { vec3 yellow = { 1.0f, 0.93f, 0.2f }; glm_vec3_lerp(col, yellow, 0.55f, col); }
            }
            renderer.draw_model(mdl, enemy_part_world, eplace, col);
            if (en.kind == dc::entity::EnemyKind::Insulter) {   // ranting motes orbit the insulter
                const auto& R2 = renderer.cam_right; const auto& U2 = renderer.cam_up;
                const float cy = terrain.height(en.position[0], en.position[2]) + 2.2f;
                for (int m = 0; m < 5; ++m) {
                    const float ph = t_now * 3.0f + m * 1.2566f + en.id;
                    const float rad = 0.8f + 0.25f * std::sin(t_now * 2.0f + m);
                    const float px = en.position[0] + std::cos(ph)*rad, pz = en.position[2] + std::sin(ph)*rad;
                    const float py = cy + 0.3f * std::sin(t_now*4.0f + m), s = 0.1f;
                    auto P = [&](float u, float v) { particle_verts.insert(particle_verts.end(), {
                        px + R2[0]*u + U2[0]*v, py + R2[1]*u + U2[1]*v, pz + R2[2]*u + U2[2]*v, 1.0f, 0.85f, 0.15f, 0.9f }); };
                    P(-s,-s);P(s,-s);P(s,s); P(-s,-s);P(s,s);P(-s,s);
                }
            }
        }

        // Friendly lane mobs: posed skeletons tinted ALLY BLUE, marching the lane (+ a small
        // health bar). Host sims them; clients render the replicated set.
        if (skeleton_loaded) {
            const bool cl = (net.role == dc::net::Role::Client);
            const std::size_t na = cl ? net_allies.size() : allies.size();
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            std::vector<float> horse_v, knight_v, ally_slime;   // Knight: horse+rider; ally_slime: green blobs (drawn after the loop)
            auto hboxto = [&](std::vector<float>& buf, float cx, float cy, float cz, float hx, float hy, float hz, float ca, float sa) {
                auto Vt = [&](float lx, float ly, float lz, float nx, float ny, float nz) {
                    const float rx = ca*lx - sa*lz, rz = sa*lx + ca*lz;
                    const float rnx = ca*nx - sa*nz, rnz = sa*nx + ca*nz;
                    buf.insert(buf.end(), { cx+rx, cy+ly, cz+rz, rnx, ny, rnz, 0.f, 0.f, 0.f });
                };
                const float X0=-hx,X1=hx,Y0=-hy,Y1=hy,Z0=-hz,Z1=hz;
                Vt(X1,Y0,Z0,1,0,0);Vt(X1,Y1,Z0,1,0,0);Vt(X1,Y1,Z1,1,0,0); Vt(X1,Y0,Z0,1,0,0);Vt(X1,Y1,Z1,1,0,0);Vt(X1,Y0,Z1,1,0,0);
                Vt(X0,Y0,Z1,-1,0,0);Vt(X0,Y1,Z1,-1,0,0);Vt(X0,Y1,Z0,-1,0,0); Vt(X0,Y0,Z1,-1,0,0);Vt(X0,Y1,Z0,-1,0,0);Vt(X0,Y0,Z0,-1,0,0);
                Vt(X0,Y1,Z0,0,1,0);Vt(X1,Y1,Z0,0,1,0);Vt(X1,Y1,Z1,0,1,0); Vt(X0,Y1,Z0,0,1,0);Vt(X1,Y1,Z1,0,1,0);Vt(X0,Y1,Z1,0,1,0);
                Vt(X0,Y0,Z1,0,-1,0);Vt(X1,Y0,Z1,0,-1,0);Vt(X1,Y0,Z0,0,-1,0); Vt(X0,Y0,Z1,0,-1,0);Vt(X1,Y0,Z0,0,-1,0);Vt(X0,Y0,Z0,0,-1,0);
                Vt(X0,Y0,Z1,0,0,1);Vt(X1,Y0,Z1,0,0,1);Vt(X1,Y1,Z1,0,0,1); Vt(X0,Y0,Z1,0,0,1);Vt(X1,Y1,Z1,0,0,1);Vt(X0,Y1,Z1,0,0,1);
                Vt(X1,Y0,Z0,0,0,-1);Vt(X0,Y0,Z0,0,0,-1);Vt(X0,Y1,Z0,0,0,-1); Vt(X1,Y0,Z0,0,0,-1);Vt(X0,Y1,Z0,0,0,-1);Vt(X1,Y1,Z0,0,0,-1);
            };
            for (std::size_t i = 0; i < na; ++i) {
                float ax, az, ayaw, hfrac, asize, aatk = 0.0f; uint8_t akind = 0, aup = 0; bool amoving = true;
                if (cl) { const auto& a = net_allies[i]; ax=a.x; az=a.z; ayaw=a.yaw; hfrac=a.health01; akind=a.kind; asize=a.size; aatk=a.atk; aup=a.up; }
                else    { const auto& a = allies[i]; ax=a.pos[0]; az=a.pos[2]; ayaw=a.yaw; hfrac=a.max_hp>0?a.health/a.max_hp:0.0f; akind=a.kind; asize=a.size_mul; aatk=a.atk; aup=a.up; amoving=a.moving; }
                // The mob's MODEL mirrors the matching enemy: ground=skeleton, mage=mage,
                // bat=bat, flier=eye, demon=demon, scavenger=scavenger (white-tinted so its own
                // materials show). Falls back to the skeleton if a model is missing.
                const dc::game::MobVisual vis = dc::game::mob_type(akind).visual;
                dc::renderer::ModelData* amdp = &skeleton_data; dc::renderer::Model* amdlp = &skeleton_model;
                bool white = false;   // white tint -> the model's own colors show through
                switch (vis) {
                    case dc::game::MobVisual::Scavenger: if (scavenger_loaded) { amdp=&scavenger_data; amdlp=&scavenger_model; white=true; } break;
                    case dc::game::MobVisual::Mage:      if (mage_loaded)      { amdp=&mage_data; amdlp=&mage_model; white=true; } break;
                    case dc::game::MobVisual::Bat:       if (bat_loaded)       { amdp=&bat_data; amdlp=&bat_model; white=true; } break;
                    case dc::game::MobVisual::Flier:     if (eye_loaded)       { amdp=&eye_data; amdlp=&eye_model; white=true; } break;
                    case dc::game::MobVisual::Demon:     if (demon_loaded)     { amdp=&demon_data; amdlp=&demon_model; white=true; } break;
                    case dc::game::MobVisual::Insulter:  if (insulter_loaded)  { amdp=&insulter_data; amdlp=&insulter_model; white=true; } break;
                    case dc::game::MobVisual::Flame:     if (gnome_loaded)     { amdp=&gnome_data; amdlp=&gnome_model; white=true; } break;
                    case dc::game::MobVisual::Troll:     if (troll_loaded)     { amdp=&troll_data; amdlp=&troll_model; white=true; } break;
                    case dc::game::MobVisual::Drone:     if (drone_loaded)     { amdp=&drone_data; amdlp=&drone_model; white=true; } break;
                    default: break;   // Slime is procedural (handled below)
                }
                dc::renderer::ModelData& amd = *amdp; dc::renderer::Model& amdl = *amdlp;
                std::vector<dc::renderer::AnimLayer> al;
                if (!amoving && amd.idle.valid())   // standing at the front / held: breathe instead of moonwalk
                    al.push_back({ &amd.idle, t_now + static_cast<float>(i), -1 });
                else
                    al.push_back({ &amd.walk, t_now * 1.8f + static_cast<float>(i), -1 });
                dc::renderer::pose_model(amd, al, 0.0f, enemy_part_world);
                mat4 apl; glm_mat4_identity(apl);
                float afy = terrain.height(ax, az) + MODEL_FOOT_LIFT;
                if (in_water(ax, az)) afy += -0.35f + std::sin(t_now * 3.0f + i) * 0.1f;   // wade + bob
                if (dc::game::mob_type(akind).flies) afy += 1.6f + std::sin(t_now*2.0f+i)*0.15f;   // hover
                if (vis == dc::game::MobVisual::Knight) {
                    // MOUNTED KNIGHT — a Tree-Sentinel cavalier model (barded warhorse + lance/shield
                    // rider). The model's PUNCH clip is the rear-and-slam (its origin is at the hind
                    // hooves, so the rear pitches about them naturally).
                    const float hca = std::cos(ayaw), hsa = std::sin(ayaw);
                    const float gy = terrain.height(ax, az) + MODEL_FOOT_LIFT;
                    const float rgx = -hsa, rgz = hca;     // beam (right) axis  (for the slam shockwave)
                    const float F0 = hca, F2 = hsa;        // forward axis (horizontal)
                    if (mounted_knight_loaded) {
                        std::vector<dc::renderer::AnimLayer> kl;
                        if (aatk > 0.001f && mounted_knight_data.punch.valid())
                            kl.push_back({ &mounted_knight_data.punch, (1.0f - aatk) * mounted_knight_data.punch.duration, -1, false });   // rear -> slam
                        else if (amoving)
                            kl.push_back({ &mounted_knight_data.walk, t_now * 1.6f + static_cast<float>(i), -1 });
                        else if (mounted_knight_data.idle.valid())
                            kl.push_back({ &mounted_knight_data.idle, t_now + static_cast<float>(i), -1 });
                        dc::renderer::pose_model(mounted_knight_data, kl, 0.0f, enemy_part_world);
                        mat4 kpl; glm_mat4_identity(kpl);
                        vec3 kpos = { ax, gy, az }; glm_translate(kpl, kpos);   // origin = hind hooves on the ground
                        glm_rotate_y(kpl, -ayaw + MODEL_YAW_OFFSET, kpl);
                        const float ks = 1.05f * asize; { vec3 ksc = { ks, ks, ks }; glm_scale(kpl, ksc); }
                        vec3 ktint = { 0.55f, 0.72f, 1.0f };   // ally blue (matches the other friendly mobs)
                        renderer.draw_model(mounted_knight_model, enemy_part_world, kpl, ktint);
                    }
                    // a slam SHOCKWAVE — an expanding ground ring of embers — when the hooves land.
                    if (aatk > 0.04f && aatk < 0.55f) {
                        const auto& R2 = renderer.cam_right; const auto& U2 = renderer.cam_up;
                        const float prog = (0.55f - aatk) / 0.51f;        // 0..1 as it lands + after
                        const float rad = 0.4f + prog*3.0f, fade = 1.0f - prog;
                        const float fx = ax + F0*1.7f, fz = az + F2*1.7f, fy = gy + 0.12f;
                        for (int k=0;k<14;++k){ const float a0 = k*0.4488f;
                            const float rx = std::cos(a0)*rad, rz = std::sin(a0)*rad, s = 0.22f*fade;
                            const float cxp=fx+rgx*rx+F0*rz, czp=fz+rgz*rx+F2*rz;
                            auto P=[&](float u,float v){ particle_verts.insert(particle_verts.end(), {
                                cxp+R2[0]*u+U2[0]*v, fy+0.2f+R2[1]*u+U2[1]*v, czp+R2[2]*u+U2[2]*v, 1.0f,0.7f,0.3f,fade }); };
                            P(-s,-s);P(s,-s);P(s,s); P(-s,-s);P(s,s);P(-s,s); }
                    }
                }
                vec3 apos = { ax, afy, az };
                glm_translate(apl, apos);
                glm_rotate_y(apl, -ayaw + MODEL_YAW_OFFSET, apl);
                // SLIME ally: a wobbling green blob, no humanoid model.
                if (vis == dc::game::MobVisual::Slime) {
                    const float gy = terrain.height(ax, az) + MODEL_FOOT_LIFT;
                    const float wob = 1.0f + 0.12f * std::sin(t_now*4.0f + ax), w = 0.8f * asize;
                    hboxto(ally_slime, ax, gy + 0.55f/wob, az, w*wob, 0.55f/wob, w*wob, 1.0f, 0.0f);
                    hboxto(ally_slime, ax, gy + 0.95f/wob, az, w*0.55f*wob, 0.4f/wob, w*0.55f*wob, 1.0f, 0.0f);
                }
                const float dscale = (vis == dc::game::MobVisual::Demon || vis == dc::game::MobVisual::Troll) ? 1.7f : 1.0f;   // demons + trolls are big
                { vec3 asc = { asize*dscale, asize*dscale, asize*dscale }; glm_scale(apl, asc); }
                // Tint EVERY ally toward team BLUE (like the skeleton grunts) so they're clearly
                // distinguishable from the natural-colored enemy mobs.
                vec3 acol;
                if (white) { acol[0]=0.50f; acol[1]=0.70f; acol[2]=1.0f; }   // own-material mobs shifted blue
                else { acol[0]=0.35f; acol[1]=0.85f; acol[2]=0.9f; }         // skeleton grunts: cyan-blue
                if (vis != dc::game::MobVisual::Knight && vis != dc::game::MobVisual::Slime)   // those are fully procedural
                    renderer.draw_model(amdl, enemy_part_world, apl, acol);
                // BILL constantly rants: a swirl of yellow motes orbits him (camera-facing billboards).
                if (dc::game::mob_type(akind).visual == dc::game::MobVisual::Insulter) {
                    const auto& R2 = renderer.cam_right; const auto& U2 = renderer.cam_up;
                    const float cy = terrain.height(ax, az) + 1.4f;
                    for (int m = 0; m < 5; ++m) {
                        const float ph = t_now * 3.0f + m * 1.2566f + i;
                        const float rad = 0.7f + 0.25f * std::sin(t_now * 2.0f + m);
                        const float px = ax + std::cos(ph) * rad, pz = az + std::sin(ph) * rad;
                        const float py = cy + 0.3f * std::sin(t_now * 4.0f + m);
                        const float s = 0.1f;
                        auto P = [&](float u, float v) { particle_verts.insert(particle_verts.end(), {
                            px + R2[0]*u + U2[0]*v, py + R2[1]*u + U2[1]*v, pz + R2[2]*u + U2[2]*v, 1.0f, 0.9f, 0.15f, 0.9f }); };
                        P(-s,-s);P(s,-s);P(s,s); P(-s,-s);P(s,s);P(-s,s);
                    }
                }
                vec3 mid = { ax, terrain.height(ax, az) + 2.4f, az };
                const float BW = 0.5f, BH = 0.06f;
                const float hf = hfrac < 0.0f ? 0.0f : (hfrac > 1.0f ? 1.0f : hfrac);
                auto bar = [&](float lx, float rx, float r, float g, float b, float aa) {
                    auto P = [&](float u, float v) { particle_verts.insert(particle_verts.end(), {
                        mid[0]+R[0]*u+U[0]*v, mid[1]+R[1]*u+U[1]*v, mid[2]+R[2]*u+U[2]*v, r,g,b,aa }); };
                    P(lx,-BH); P(rx,-BH); P(rx,BH); P(lx,-BH); P(rx,BH); P(lx,BH);
                };
                bar(-BW, BW, 0.1f, 0.1f, 0.12f, 0.4f);
                bar(-BW, -BW + 2.0f*BW*hf, 0.3f, 0.7f, 1.0f, 0.85f);
                // UPGRADE LEVEL: a row of gold pips above the bar (one per upgrade; 0 = none shown).
                if (aup > 0) {
                    const int pips = aup > 10 ? 10 : aup;
                    const float py = 0.22f, pw = 0.05f, gap = 0.13f, x0 = -(pips-1)*gap*0.5f;
                    for (int p = 0; p < pips; ++p) {
                        const float cx = x0 + p*gap;
                        auto Q=[&](float u,float v){ particle_verts.insert(particle_verts.end(), {
                            mid[0]+R[0]*(cx+u)+U[0]*(py+v), mid[1]+R[1]*(cx+u)+U[1]*(py+v), mid[2]+R[2]*(cx+u)+U[2]*(py+v), 1.0f, 0.85f, 0.2f, 0.95f }); };
                        Q(-pw,-pw);Q(pw,-pw);Q(pw,pw); Q(-pw,-pw);Q(pw,pw);Q(-pw,pw);
                    }
                }
            }
            if (!horse_v.empty()) {   // all warhorses in one brown batch
                static dc::renderer::Mesh horse_mesh;
                horse_mesh.upload(horse_v);
                vec3 horse_col = { 0.34f, 0.22f, 0.13f };   // chestnut brown
                renderer.draw_terrain(horse_mesh, horse_col, true);
            }
            if (!knight_v.empty()) {   // the steel riders + lances
                static dc::renderer::Mesh knight_mesh;
                knight_mesh.upload(knight_v);
                vec3 knight_col = { 0.62f, 0.66f, 0.72f };   // burnished steel
                renderer.draw_terrain(knight_mesh, knight_col, true);
            }
            if (!ally_slime.empty()) {   // friendly slimes (green goo)
                static dc::renderer::Mesh aslime_mesh;
                aslime_mesh.upload(ally_slime);
                vec3 goo = { 0.35f, 0.85f, 0.30f };
                renderer.draw_terrain(aslime_mesh, goo, true);
            }
            // Rally flag: a glowing cyan banner where you've ordered the mobs to hold.
            if (rally_active) {
                const float gx = rally_pos[0], gz = rally_pos[2], gy = terrain.height(gx, gz);
                const float pulse = 0.7f + 0.3f * dc::fx::flicker(t_now * 2.0f);
                auto quad = [&](float cx, float cy, float cz, float w, float h, float r, float g, float b) {
                    auto P = [&](float u, float v) { particle_verts.insert(particle_verts.end(), {
                        cx + R[0]*u + U[0]*v, cy + R[1]*u + U[1]*v, cz + R[2]*u + U[2]*v, r, g, b, 0.85f }); };
                    P(-w,-h);P(w,-h);P(w,h); P(-w,-h);P(w,h);P(-w,h);
                };
                for (float yy = 0.2f; yy < 2.4f; yy += 0.3f) quad(gx, gy + yy, gz, 0.05f, 0.16f, 0.2f, 0.9f, 1.0f);   // pole
                quad(gx + R[0]*0.45f, gy + 2.2f + R[1]*0.45f, gz + R[2]*0.45f, 0.42f, 0.26f, 0.25f*pulse, 1.0f*pulse, 0.9f*pulse);   // banner
            }
        }

        if (!flyer_verts.empty()) {                       // upload + draw this frame's flyer cubes
            flyer_mesh.upload(flyer_verts);
            vec3 flyer_color = { 0.85f, 0.25f, 0.30f };   // menacing red
            renderer.draw_terrain(flyer_mesh, flyer_color, true);   // flat color, not terrain-blended
        }
        if (!slime_blob_verts.empty()) {                  // slime enemy bodies (green)
            static dc::renderer::Mesh slime_blob_mesh;
            slime_blob_mesh.upload(slime_blob_verts);
            vec3 slime_col = { 0.30f, 0.80f, 0.18f };
            renderer.draw_terrain(slime_blob_mesh, slime_col, true);
        }

        // Naval units: enemy BOATS — a dark hull + deck + mast + two cannons, bobbing on the
        // river and yawed to their heading. Built fresh each frame (cheap), drawn flat-shaded.
        {
            const bool cl = (net.role == dc::net::Role::Client);
            const std::size_t nb = cl ? net_boats.size() : (boats.size() + subs.size());   // host draws boats THEN subs
            if (nb > 0) {
                std::vector<float> bv, sailv, sailv_friend, subv;   // subv = friendly sub hull (yellow)
                std::vector<float> sub_dark, sub_lens, sub_red;     // sub_dark = navy tower, sub_lens = blue lens, sub_red = ENEMY sub hull+lens
                std::vector<float> mine_rack;                       // dark iron mine balls on a minelayer's deck
                std::vector<float> cannon_v;                        // dark steel deck cannons on warships
                auto boxrb = [&](std::vector<float>& buf, float cx, float cy, float cz, float hx, float hy, float hz, float ca, float sa) {
                    auto Vt = [&](float lx, float ly, float lz, float nx, float ny, float nz) {
                        const float rx = ca*lx - sa*lz, rz = sa*lx + ca*lz;
                        const float rnx = ca*nx - sa*nz, rnz = sa*nx + ca*nz;
                        buf.insert(buf.end(), { cx+rx, cy+ly, cz+rz, rnx, ny, rnz, 0.f, 0.f, 0.f });
                    };
                    const float X0=-hx,X1=hx,Y0=-hy,Y1=hy,Z0=-hz,Z1=hz;
                    Vt(X1,Y0,Z0,1,0,0);Vt(X1,Y1,Z0,1,0,0);Vt(X1,Y1,Z1,1,0,0); Vt(X1,Y0,Z0,1,0,0);Vt(X1,Y1,Z1,1,0,0);Vt(X1,Y0,Z1,1,0,0);
                    Vt(X0,Y0,Z1,-1,0,0);Vt(X0,Y1,Z1,-1,0,0);Vt(X0,Y1,Z0,-1,0,0); Vt(X0,Y0,Z1,-1,0,0);Vt(X0,Y1,Z0,-1,0,0);Vt(X0,Y0,Z0,-1,0,0);
                    Vt(X0,Y1,Z0,0,1,0);Vt(X1,Y1,Z0,0,1,0);Vt(X1,Y1,Z1,0,1,0); Vt(X0,Y1,Z0,0,1,0);Vt(X1,Y1,Z1,0,1,0);Vt(X0,Y1,Z1,0,1,0);
                    Vt(X0,Y0,Z1,0,-1,0);Vt(X1,Y0,Z1,0,-1,0);Vt(X1,Y0,Z0,0,-1,0); Vt(X0,Y0,Z1,0,-1,0);Vt(X1,Y0,Z0,0,-1,0);Vt(X0,Y0,Z0,0,-1,0);
                    Vt(X0,Y0,Z1,0,0,1);Vt(X1,Y0,Z1,0,0,1);Vt(X1,Y1,Z1,0,0,1); Vt(X0,Y0,Z1,0,0,1);Vt(X1,Y1,Z1,0,0,1);Vt(X0,Y1,Z1,0,0,1);
                    Vt(X1,Y0,Z0,0,0,-1);Vt(X0,Y0,Z0,0,0,-1);Vt(X0,Y1,Z0,0,0,-1); Vt(X1,Y0,Z0,0,0,-1);Vt(X0,Y1,Z0,0,0,-1);Vt(X1,Y1,Z0,0,0,-1);
                };
                const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
                for (std::size_t i = 0; i < nb; ++i) {
                    float bx, bz, byaw, hf; uint8_t bkind;
                    if (cl) { const auto& b = net_boats[i]; bx=b.x; bz=b.z; byaw=b.yaw; hf=b.health01; bkind=b.kind; }
                    else if (i < boats.size()) { const auto& b = boats[i]; bx=b.pos[0]; bz=b.pos[2]; byaw=b.yaw; hf=b.health/(b.team==1?FRIENDLY_BOAT_HP:BOAT_MAX_HP); bkind=(b.team==1)?4:b.kind; }
                    else { const auto& s = subs[i - boats.size()]; bx=s.pos[0]; bz=s.pos[2]; byaw=s.yaw; hf=1.0f; bkind=s.kind; }
                    const float ca = std::cos(byaw), sa = std::sin(byaw);
                    const float wbase = terrain.height(bx, bz);
                    // SUBS: friendly (kind 1/2) draw YELLOW; ENEMY (kind 5/6) draw RED. Submerged = a
                    // periscope + wake sliver; surfaced = a hull + conning tower + spinning prop.
                    const bool esub = (bkind == 5 || bkind == 6);
                    const uint8_t sk = esub ? static_cast<uint8_t>(bkind - 4) : bkind;
                    std::vector<float>& hullbuf = esub ? sub_red : subv;
                    std::vector<float>& lensbuf = esub ? sub_red : sub_lens;
                    if (sk == 1) {   // submerged: a chunky periscope (navy tube + lens) above the surface
                        const float wy = wbase + 0.35f + std::sin(t_now*3.0f + i) * 0.05f;
                        boxrb(sub_dark, bx, wy + 0.6f, bz, 0.16f, 0.9f, 0.16f, ca, sa);                 // periscope mast (navy), top at wy+1.5
                        boxrb(lensbuf, bx + ca*0.34f, wy + 1.62f, bz + sa*0.34f, 0.34f, 0.15f, 0.15f, ca, sa);  // scope head LENS, sits above the mast
                        boxrb(hullbuf, bx, wy, bz, 1.0f, 0.12f, 0.5f, ca, sa);                          // a sliver of the hull
                        continue;
                    }
                    if (sk == 2) {   // surfaced: hull, navy tower, periscope lens, spinning prop
                        const float sgy = wbase + 0.2f + std::sin(t_now*2.0f + i) * 0.06f;
                        boxrb(hullbuf,  bx, sgy, bz, 2.0f, 0.4f, 0.75f, ca, sa);          // sub hull
                        boxrb(sub_dark, bx, sgy + 0.6f, bz, 0.55f, 0.45f, 0.45f, ca, sa); // conning tower (navy)
                        boxrb(sub_dark, bx, sgy + 1.05f, bz, 0.08f, 0.45f, 0.08f, ca, sa);// periscope mast (navy), top at sgy+1.5
                        boxrb(lensbuf, bx + ca*0.22f, sgy + 1.66f, bz + sa*0.22f, 0.2f, 0.13f, 0.13f, ca, sa); // lens, above the mast
                        // Spinning propeller at the stern: a camera-facing fan of 3 grey blades.
                        const float pcx = bx - ca*2.05f, pcz = bz - sa*2.05f, pcy = sgy + 0.18f;
                        const float spin = t_now * 16.0f;
                        for (int k = 0; k < 3; ++k) {
                            const float a0 = spin + k * 2.0944f, L = 0.7f, W = 0.12f;
                            const float dx = std::cos(a0), dy = std::sin(a0), ex = -std::sin(a0), ey = std::cos(a0);
                            auto PV = [&](float along, float wide) {
                                const float u = dx*along + ex*wide, v = dy*along + ey*wide;
                                particle_verts.insert(particle_verts.end(), {
                                    pcx + R[0]*u + U[0]*v, pcy + R[1]*u + U[1]*v, pcz + R[2]*u + U[2]*v, 0.72f, 0.72f, 0.75f, 0.95f }); };
                            PV(0,-W);PV(L,-W);PV(L,W); PV(0,-W);PV(L,W);PV(0,W);
                        }
                        continue;
                    }
                    if (bkind == 8 || bkind == 9) {   // MINELAYER: the boat MODEL barge with mine racks + a team flag
                        const float gy = wbase + 0.55f + std::sin(t_now*1.6f + i)*0.07f;   // ride ON the water
                        if (boat_loaded) {
                            std::vector<dc::renderer::AnimLayer> bl; bl.push_back({ &boat_data.idle, t_now + static_cast<float>(i), -1 });   // barge: gentle bob, oars shipped
                            dc::renderer::pose_model(boat_data, bl, 0.0f, enemy_part_world);
                            mat4 bpl; glm_mat4_identity(bpl);
                            vec3 bpos = { bx, gy, bz }; glm_translate(bpl, bpos);
                            glm_rotate_y(bpl, -byaw + MODEL_YAW_OFFSET, bpl);
                            { vec3 bsc = { 1.75f, 1.5f, 1.95f }; glm_scale(bpl, bsc); }
                            vec3 btint; if (bkind==8) { btint[0]=0.82f; btint[1]=0.86f; btint[2]=1.0f; } else { btint[0]=1.0f; btint[1]=0.84f; btint[2]=0.72f; }
                            renderer.draw_model(boat_model, enemy_part_world, bpl, btint);
                        }
                        for (int k=0;k<4;++k){ const float off=(k-1.5f)*0.52f;
                            boxrb(mine_rack, bx+ca*off, gy+0.62f, bz+sa*off, 0.20f,0.20f,0.20f, ca, sa); }   // mine balls on deck
                        boxrb(bv, bx-ca*1.6f, gy+1.0f, bz-sa*1.6f, 0.07f, 0.9f, 0.07f, ca, sa);   // flag pole (stern)
                        boxrb(bkind==8 ? sailv_friend : sailv, bx-ca*1.6f, gy+1.7f, bz-sa*1.6f, 0.06f, 0.3f, 0.5f, ca, sa);  // team flag
                        vec3 mid = { bx, gy + 1.8f, bz }; const float hfc = hf<0?0:(hf>1?1:hf);
                        auto bar=[&](float lx,float rx,float r,float g,float bb,float aa){ auto P=[&](float u,float v){ particle_verts.insert(particle_verts.end(),{
                            mid[0]+R[0]*u+U[0]*v, mid[1]+R[1]*u+U[1]*v, mid[2]+R[2]*u+U[2]*v, r,g,bb,aa}); }; P(lx,-0.1f);P(rx,-0.1f);P(rx,0.1f);P(lx,-0.1f);P(rx,0.1f);P(lx,0.1f); };
                        bar(-1.0f,1.0f,0.3f,0.05f,0.05f,0.4f); bar(-1.0f,-1.0f+2.0f*hfc,0.3f,0.8f,0.4f,0.85f);
                        continue;
                    }
                    if (bkind == 10 || bkind == 11) {   // MINESWEEPER: the rowboat MODEL with a skeleton rowing it
                        const float gy = wbase + 0.50f + std::sin(t_now*2.2f + i)*0.08f;   // ride ON the water (surface is wbase+0.30)
                        if (boat_loaded) {
                            std::vector<dc::renderer::AnimLayer> bl; bl.push_back({ &boat_data.walk, t_now*1.6f + static_cast<float>(i), -1 });   // oars row
                            dc::renderer::pose_model(boat_data, bl, 0.0f, enemy_part_world);
                            mat4 bpl; glm_mat4_identity(bpl);
                            vec3 bpos = { bx, gy, bz }; glm_translate(bpl, bpos);
                            glm_rotate_y(bpl, -byaw + MODEL_YAW_OFFSET, bpl);
                            { vec3 bsc = { 0.85f, 0.85f, 0.85f }; glm_scale(bpl, bsc); }
                            vec3 btint; if (bkind==10) { btint[0]=0.85f; btint[1]=0.88f; btint[2]=1.0f; } else { btint[0]=1.0f; btint[1]=0.72f; btint[2]=0.6f; }
                            renderer.draw_model(boat_model, enemy_part_world, bpl, btint);
                        }
                        if (skeleton_loaded) {   // a skeleton seated in the hull, working the oars
                            std::vector<dc::renderer::AnimLayer> sl; sl.push_back({ &skeleton_data.walk, t_now*4.0f + static_cast<float>(i), -1 });
                            dc::renderer::pose_model(skeleton_data, sl, 0.0f, enemy_part_world);
                            mat4 spl; glm_mat4_identity(spl);
                            vec3 spos = { bx, gy + 0.18f, bz }; glm_translate(spl, spos);
                            glm_rotate_y(spl, -byaw + MODEL_YAW_OFFSET, spl);
                            { vec3 ssc = { 0.55f, 0.55f, 0.55f }; glm_scale(spl, ssc); }
                            vec3 sc; if (bkind==10) { sc[0]=0.7f; sc[1]=0.85f; sc[2]=1.0f; } else { sc[0]=1.0f; sc[1]=0.55f; sc[2]=0.5f; }
                            renderer.draw_model(skeleton_model, enemy_part_world, spl, sc);
                        }
                        continue;
                    }
                    // WARSHIP: the wooden boat MODEL (scaled up) for the hull, with a tall mast +
                    // square sail and deck CANNONS that point forward. The hull steers to face its
                    // target (b.yaw aims at it), so the forward cannons are trained on the target and
                    // their muzzles are where the shells spawn.
                    const float gy = wbase + 0.65f + std::sin(t_now * 1.6f + i) * 0.08f;   // ride ON the water (surface is wbase+0.30)
                    if (boat_loaded) {
                        std::vector<dc::renderer::AnimLayer> bl; bl.push_back({ &boat_data.walk, t_now*1.1f + static_cast<float>(i), -1 });
                        dc::renderer::pose_model(boat_data, bl, 0.0f, enemy_part_world);
                        mat4 bpl; glm_mat4_identity(bpl);
                        vec3 bpos = { bx, gy, bz }; glm_translate(bpl, bpos);
                        glm_rotate_y(bpl, -byaw + MODEL_YAW_OFFSET, bpl);
                        { vec3 bsc = { 2.05f, 1.7f, 2.05f }; glm_scale(bpl, bsc); }   // warship-sized
                        vec3 btint; if (bkind==4) { btint[0]=0.82f; btint[1]=0.86f; btint[2]=1.0f; } else { btint[0]=1.0f; btint[1]=0.85f; btint[2]=0.74f; }
                        renderer.draw_model(boat_model, enemy_part_world, bpl, btint);
                    }
                    boxrb(bv, bx, gy+2.0f, bz, 0.14f, 1.7f, 0.14f, ca, sa);   // tall mast
                    boxrb(bv, bx, gy+3.5f, bz, 0.08f, 0.08f, 1.7f, ca, sa);   // yard (cross spar, spans the beam)
                    // Two deck cannons (turret base + forward barrel) at the bow and amidships.
                    auto cannon = [&](float fwd){
                        const float cxp = bx + ca*fwd, czp = bz + sa*fwd;
                        boxrb(cannon_v, cxp, gy+0.95f, czp, 0.22f, 0.22f, 0.26f, ca, sa);                       // turret base
                        boxrb(cannon_v, cxp + ca*0.5f, gy+1.04f, czp + sa*0.5f, 0.5f, 0.1f, 0.1f, ca, sa);     // barrel (points fwd at the target)
                    };
                    cannon(1.1f); cannon(-0.2f);
                    // a big square SAIL hung off the yard (wide across the beam), FIXED to the mast.
                    // Friendly warships fly a BLUE sail; enemies a cream one.
                    boxrb(bkind == 4 ? sailv_friend : sailv, bx, gy+2.5f, bz, 0.06f, 1.0f, 1.5f, ca, sa);
                    // health bar over the hull
                    const float hfc = hf<0?0:(hf>1?1:hf);
                    vec3 mid = { bx, gy + 3.0f, bz };
                    auto bar = [&](float lx, float rx, float rr, float gg, float bb, float aa) {
                        auto P=[&](float u,float v){ particle_verts.insert(particle_verts.end(), {
                            mid[0]+R[0]*u+U[0]*v, mid[1]+R[1]*u+U[1]*v, mid[2]+R[2]*u+U[2]*v, rr,gg,bb,aa }); };
                        P(lx,-0.1f);P(rx,-0.1f);P(rx,0.1f); P(lx,-0.1f);P(rx,0.1f);P(lx,0.1f);
                    };
                    bar(-1.2f, 1.2f, 0.3f,0.05f,0.05f,0.4f); bar(-1.2f, -1.2f+2.4f*hfc, 1.0f,0.25f,0.15f,0.85f);
                }
                boat_mesh.upload(bv);
                vec3 hull = { 0.22f, 0.16f, 0.12f };   // dark wood
                renderer.draw_terrain(boat_mesh, hull, true);
                if (!sailv.empty()) {
                    static dc::renderer::Mesh sail_mesh;
                    sail_mesh.upload(sailv);
                    vec3 sail = { 0.88f, 0.84f, 0.74f };   // cream canvas (enemy)
                    renderer.draw_terrain(sail_mesh, sail, true);
                }
                if (!sailv_friend.empty()) {
                    static dc::renderer::Mesh sailf_mesh;
                    sailf_mesh.upload(sailv_friend);
                    vec3 sail = { 0.3f, 0.55f, 1.0f };   // blue canvas (your warships)
                    renderer.draw_terrain(sailf_mesh, sail, true);
                }
                if (!subv.empty()) {
                    static dc::renderer::Mesh subv_mesh;
                    subv_mesh.upload(subv);
                    vec3 subcol = { 0.95f, 0.85f, 0.2f };   // bright yellow hull
                    renderer.draw_terrain(subv_mesh, subcol, true);
                }
                if (!sub_dark.empty()) {
                    static dc::renderer::Mesh subd_mesh;
                    subd_mesh.upload(sub_dark);
                    vec3 navy = { 0.12f, 0.16f, 0.28f };   // navy conning tower / periscope mast
                    renderer.draw_terrain(subd_mesh, navy, true);
                }
                if (!sub_lens.empty()) {
                    static dc::renderer::Mesh subl_mesh;
                    subl_mesh.upload(sub_lens);
                    vec3 lens = { 0.25f, 0.6f, 1.0f };   // glowing blue periscope lens
                    renderer.draw_terrain(subl_mesh, lens, true);
                }
                if (!sub_red.empty()) {
                    static dc::renderer::Mesh subr_mesh;
                    subr_mesh.upload(sub_red);
                    vec3 red = { 0.85f, 0.18f, 0.16f };   // ENEMY sub hull + lens
                    renderer.draw_terrain(subr_mesh, red, true);
                }
                if (!mine_rack.empty()) {
                    static dc::renderer::Mesh rack_mesh;
                    rack_mesh.upload(mine_rack);
                    vec3 iron = { 0.12f, 0.12f, 0.14f };   // dark iron mines on the minelayer deck
                    renderer.draw_terrain(rack_mesh, iron, true);
                }
                if (!cannon_v.empty()) {
                    static dc::renderer::Mesh cannon_mesh;
                    cannon_mesh.upload(cannon_v);
                    vec3 steel = { 0.16f, 0.16f, 0.18f };   // dark steel warship cannons
                    renderer.draw_terrain(cannon_mesh, steel, true);
                }
            }
        }

        // Enemy SEA-MINES: a dark spiked ball bobbing in the river, with a blinking red light once armed.
        {
            const bool cl2 = (net.role == dc::net::Role::Client);
            const std::size_t nmn = cl2 ? net_mines.size() : naval_mines.size();
            if (nmn > 0) {
                std::vector<float> mv;
                auto mbox = [&](float cx,float cy,float cz,float h){
                    auto Vt = [&](float lx,float ly,float lz,float nx,float ny,float nz){ mv.insert(mv.end(), { cx+lx, cy+ly, cz+lz, nx, ny, nz, 0.f,0.f,0.f }); };
                    const float X0=-h,X1=h,Y0=-h,Y1=h,Z0=-h,Z1=h;
                    Vt(X1,Y0,Z0,1,0,0);Vt(X1,Y1,Z0,1,0,0);Vt(X1,Y1,Z1,1,0,0); Vt(X1,Y0,Z0,1,0,0);Vt(X1,Y1,Z1,1,0,0);Vt(X1,Y0,Z1,1,0,0);
                    Vt(X0,Y0,Z1,-1,0,0);Vt(X0,Y1,Z1,-1,0,0);Vt(X0,Y1,Z0,-1,0,0); Vt(X0,Y0,Z1,-1,0,0);Vt(X0,Y1,Z0,-1,0,0);Vt(X0,Y0,Z0,-1,0,0);
                    Vt(X0,Y1,Z0,0,1,0);Vt(X1,Y1,Z0,0,1,0);Vt(X1,Y1,Z1,0,1,0); Vt(X0,Y1,Z0,0,1,0);Vt(X1,Y1,Z1,0,1,0);Vt(X0,Y1,Z1,0,1,0);
                    Vt(X0,Y0,Z1,0,-1,0);Vt(X1,Y0,Z1,0,-1,0);Vt(X1,Y0,Z0,0,-1,0); Vt(X0,Y0,Z1,0,-1,0);Vt(X1,Y0,Z0,0,-1,0);Vt(X0,Y0,Z0,0,-1,0);
                    Vt(X0,Y0,Z1,0,0,1);Vt(X1,Y0,Z1,0,0,1);Vt(X1,Y1,Z1,0,0,1); Vt(X0,Y0,Z1,0,0,1);Vt(X1,Y1,Z1,0,0,1);Vt(X0,Y1,Z1,0,0,1);
                    Vt(X1,Y0,Z0,0,0,-1);Vt(X0,Y0,Z0,0,0,-1);Vt(X0,Y1,Z0,0,0,-1); Vt(X1,Y0,Z0,0,0,-1);Vt(X0,Y1,Z0,0,0,-1);Vt(X1,Y1,Z0,0,0,-1);
                };
                const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
                for (std::size_t i = 0; i < nmn; ++i) {
                    float mx, mz, arm; uint8_t mteam;
                    if (cl2) { const auto& m = net_mines[i]; mx=m.x; mz=m.z; arm=m.armed; mteam=m.team; }
                    else     { const auto& m = naval_mines[i]; mx=m.pos[0]; mz=m.pos[2]; arm=m.arm; mteam=m.team; }
                    const float wy = terrain.height(mx, mz) + 0.45f + std::sin(t_now*2.0f + i)*0.08f;
                    mbox(mx, wy, mz, 0.34f);                                   // body
                    mbox(mx, wy+0.45f, mz, 0.07f); mbox(mx, wy-0.45f, mz, 0.07f);  // spikes
                    mbox(mx+0.45f, wy, mz, 0.07f); mbox(mx-0.45f, wy, mz, 0.07f);
                    mbox(mx, wy, mz+0.45f, 0.07f); mbox(mx, wy, mz-0.45f, 0.07f);
                    const float blink = arm >= 1.0f ? (0.45f + 0.55f*std::sin(t_now*7.0f + i)) : 0.12f;
                    const float s = 0.12f;
                    const float lr = mteam==1 ? 0.2f*blink : 1.0f, lg = mteam==1 ? 0.6f*blink : 0.15f*blink, lb = mteam==1 ? 1.0f : 0.1f*blink;  // friendly mines blue, enemy red
                    auto P=[&](float u,float v){ particle_verts.insert(particle_verts.end(), {
                        mx + R[0]*u + U[0]*v, wy+0.2f + R[1]*u + U[1]*v, mz + R[2]*u + U[2]*v, lr, lg, lb, 0.95f }); };
                    P(-s,-s);P(s,-s);P(s,s); P(-s,-s);P(s,s);P(-s,s);
                }
                if (!mv.empty()) {
                    static dc::renderer::Mesh mine_mesh;
                    mine_mesh.upload(mv);
                    vec3 mcol = { 0.10f, 0.11f, 0.13f };   // dark iron
                    renderer.draw_terrain(mine_mesh, mcol, true);
                }
            }
        }

        // Draw remote players (other connected clients), blue-tinted, posed by their
        // replicated walk clock + head pitch.
        for (const auto& rp : remotes) {
            // Same layered pose as the local avatar: walk + masked punch + masked block.
            const dc::game::Appearance& rlook = look_for(rp.id);   // this player's custom look + class
            dc::renderer::ModelData& rmd = class_md(rlook.weapon_class);
            const bool r_baked = class_custom(rlook.weapon_class);
            std::vector<dc::renderer::AnimLayer> rl;
            if (!rp.moving && rmd.idle.valid()) rl.push_back({ &rmd.idle, run_time + static_cast<float>(rp.id), -1 });   // idle breathing
            if (rp.moving)   rl.push_back({ &rmd.walk,  rp.anim_time,  -1 });
            if (rp.punching) rl.push_back({ &rmd.punch, rp.punch_time, rmd.arm_l_node });
            if (rp.blocking) rl.push_back({ &rmd.block, rp.block_time, rmd.arm_r_node, false });
            dc::renderer::Mat4 r_head, r_lhand, r_rhand;
            // Pitch the body (not the head) so the arms + held gear aim with the look;
            // the head tilts too since it's a child of the body bone.
            std::vector<float> r_bscale = bone_scale_for(rlook);   // their silly proportions (networked via Appearance)
            dc::renderer::pose_model(rmd, rl, 0.0f, remote_part_world,
                                     { rmd.head_node, rmd.hand_l_node, rmd.hand_r_node },
                                     { &r_head, &r_lhand, &r_rhand }, rp.pitch, &r_bscale);
            float rfeet = (rp.pos[1] - dc::world::EYE_HEIGHT) + MODEL_FOOT_LIFT;
            mat4 rplace;
            glm_mat4_identity(rplace);
            vec3 rpos = { rp.pos[0], rfeet, rp.pos[2] };
            glm_translate(rplace, rpos);
            glm_rotate_y(rplace, -rp.yaw + MODEL_YAW_OFFSET, rplace);

            vec3 remote_color = { 1.0f, 1.0f, 1.0f };   // white -> baked class colors
            if (!r_baked) { remote_color[0] = 0.52f; remote_color[1] = 0.55f; remote_color[2] = 0.62f;
                            if (rlook.weapon_class == 1) { remote_color[0] = 0.22f; remote_color[1] = 0.26f; remote_color[2] = 0.62f; } }
            if (rp.ghost) {
                vec3 pale = { 0.55f, 0.65f, 0.95f };   // dead teammate: faint wisp
                glm_vec3_copy(pale, remote_color);
            } else if (rp.hit_flash > 0.0f) {          // flash red when hit (like the local player)
                vec3 red = { 1.0f, 0.1f, 0.1f };
                glm_vec3_lerp(remote_color, red, rp.hit_flash / dc::entity::FLASH_TIME, remote_color);
            }
            renderer.draw_model(class_mdl(rlook.weapon_class), remote_part_world, rplace, remote_color, rp.ghost ? GHOST_ALPHA : 1.0f);

            if (!rp.ghost) {   // a ghost teammate shows only its faint body, no gear/effects
            if (!r_baked) {   // fallback rig: separate face/helmet/weapon
                { mat4 hp; glm_mat4_mul(rplace, r_head.m, hp);
                  vec3 hcen = { hp[3][0], hp[3][1], hp[3][2] };
                  draw_face_at(hcen, rp.yaw, rlook); }
                mat4 r_helmet; glm_mat4_mul(rplace, r_head.m, r_helmet);
                vec3 helmet_white = { 1.0f, 1.0f, 1.0f };
                renderer.draw_model(helmet_model, helmet_offset, r_helmet, helmet_white);
                mat4 r_sword; glm_mat4_mul(rplace, r_lhand.m, r_sword);
                vec3 rsws = { rp.sword_scale, rp.sword_scale, rp.sword_scale };
                glm_scale(r_sword, rsws);
                vec3 sword_color = { 0.8f, 0.8f, 0.9f };
                renderer.draw_model(sword_model, sword_offset, r_sword, sword_color);
            }

            // Shield: knight's is baked; the fallback rig uses the prop. Wizard raises a
            // magic barrier (particle pass) while blocking.
            if (!r_baked) {
                mat4 r_shield; glm_mat4_mul(rplace, r_rhand.m, r_shield);
                vec3 shield_color = { 0.5f, 0.5f, 0.8f };
                renderer.draw_model(shield_model, shield_offset, r_shield, shield_color);
            }
            if (rlook.weapon_class == 1 && rp.blocking) {
                mat4 mw; glm_mat4_mul(rplace, r_rhand.m, mw);
                const float fwx = std::cos(rp.yaw), fwz = std::sin(rp.yaw);
                magic_shields.push_back({ mw[3][0] + fwx*0.35f, mw[3][1], mw[3][2] + fwz*0.35f, rp.yaw });
            }

            // Remote specials: match the local render. rig_scale (the rig's hand-bone
            // scale, ~0.22) comes from the posed hand matrix, same as the local avatar.
            if (!rp.throwns.empty() || rp.orbit_active) {
                float rig_scale = std::sqrt(r_lhand.m[0][0] * r_lhand.m[0][0]
                                          + r_lhand.m[0][1] * r_lhand.m[0][1]
                                          + r_lhand.m[0][2] * r_lhand.m[0][2]);
                vec3 spc = { 0.85f, 0.85f, 0.95f };
                for (const auto& th : rp.throwns) {
                    float s = rig_scale * rp.sword_scale * th.size;
                    mat4 tp; glm_mat4_identity(tp);
                    vec3 tpos = { th.x, th.y, th.z };
                    glm_translate(tp, tpos);
                    glm_rotate_y(tp, th.spin, tp);
                    vec3 sc = { s, s, s }; glm_scale(tp, sc);
                    renderer.draw_model(sword_model, sword_offset, tp, spc);
                }
                if (rp.orbit_active && rp.orbit_count > 0) {
                    float s = rig_scale * rp.sword_scale;
                    for (int i = 0; i < rp.orbit_count; ++i) {
                        float a = rp.orbit_angle + (6.2831853f * i) / rp.orbit_count;
                        mat4 op; glm_mat4_identity(op);
                        vec3 opos = { rp.pos[0] + std::cos(a) * rp.orbit_radius,
                                      (rp.pos[1] - dc::world::EYE_HEIGHT) + 1.2f,   // ride the remote's ground
                                      rp.pos[2] + std::sin(a) * rp.orbit_radius };
                        glm_translate(op, opos);
                        glm_rotate_y(op, rp.orbit_spin, op);
                        vec3 osc = { s, s, s }; glm_scale(op, osc);
                        renderer.draw_model(sword_model, sword_offset, op, spc);
                    }
                }
            }
            }   // end if (!rp.ghost)
        }

        // Stone pillars (static, flat-shaded so they read as grey stone, not terrain).
        renderer.draw_terrain(pillar_mesh, pillar_color, true);
        renderer.draw_terrain(grass_mesh, grass_color, true);   // scattered grass tufts

        // The core pylon: a bright cyan column (flat-shaded so it self-glows). Brightens
        // while healthy, dims toward red as it's destroyed.
        if (core_health > 0.0f) {
            const float frac = core_health / CORE_MAX_HEALTH;
            const float pulse = 0.8f + 0.2f * dc::fx::flicker(t_now * 0.9f);
            vec3 ccol = { (1.0f - frac) * 1.0f * pulse, (0.5f + 0.5f*frac) * pulse, (0.4f + 0.6f*frac) * pulse };
            if (glyph_loaded) {   // detailed runed monolith (emissive glyphs stay lit; stone tinted by health)
                static std::vector<dc::renderer::Mat4> glyph_pw;   // reused scratch; re-posed each frame for the idle sway + orbiting shards
                std::vector<dc::renderer::AnimLayer> gil;
                if (glyph_data.idle.valid()) gil.push_back({ &glyph_data.idle, t_now, -1 });
                dc::renderer::pose_model(glyph_data, gil, 0.0f, glyph_pw);
                mat4 gpl; glm_mat4_identity(gpl);
                vec3 gpos = { core_pos[0], core_pos[1] - 0.5f, core_pos[2] }; glm_translate(gpl, gpos);
                vec3 gsc = { 1.3f, 1.3f, 1.3f }; glm_scale(gpl, gsc);   // large
                vec3 stone_tint = { 0.6f + 0.4f*frac, 0.7f + 0.3f*frac, 0.8f + 0.2f*frac };  // reddens as it's destroyed
                renderer.draw_model(glyph_model, glyph_pw, gpl, stone_tint);
            } else {
                renderer.draw_terrain(core_mesh, ccol, true);
            }

            // Rising energy motes off the pylon (the "producing particles").
            auto jit = [&]() { spark_rng = spark_rng * 1664525u + 1013904223u; return (spark_rng >> 8) * (1.0f / 16777216.0f); };
            for (int k = 0; k < 2 && sparks.size() < 1800; ++k) {
                Spark s;
                s.pos[0] = core_pos[0] + (jit()-0.5f)*2.0f; s.pos[1] = core_pos[1] + jit()*CORE_H; s.pos[2] = core_pos[2] + (jit()-0.5f)*2.0f;
                s.vel[0] = (jit()-0.5f)*0.3f; s.vel[1] = 1.4f + jit()*1.4f; s.vel[2] = (jit()-0.5f)*0.3f;
                s.color[0] = 0.4f; s.color[1] = 0.9f; s.color[2] = 1.0f;
                s.age = 0.0f; s.life = 1.0f + jit()*0.6f; s.grav = -0.5f; s.size_mul = 1.3f; s.alpha_mul = 1.8f;
                sparks.push_back(s);
            }
            // Always-on health bar above the pylon (it's the objective).
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            const float BW = 2.0f, BH = 0.18f;
            vec3 mid = { core_pos[0], core_pos[1] + CORE_H + 1.3f, core_pos[2] };
            auto bar = [&](float lx, float rx, float r, float g, float b, float a) {
                auto P = [&](float u, float v) { particle_verts.insert(particle_verts.end(), {
                    mid[0]+R[0]*u+U[0]*v, mid[1]+R[1]*u+U[1]*v, mid[2]+R[2]*u+U[2]*v, r,g,b,a }); };
                P(lx,-BH); P(rx,-BH); P(rx,BH);  P(lx,-BH); P(rx,BH); P(lx,BH);
            };
            bar(-BW, BW, 0.5f, 0.05f, 0.05f, 0.18f);                          // track
            bar(-BW, -BW + 2.0f*BW*frac, 0.2f, 1.0f, 0.45f, 0.65f);          // fill
            // Shield bar just above the health bar (blue), so you can read shield HP.
            const float sfrac = shield_max > 0.0f ? shield_health / shield_max : 0.0f;
            mid[1] += BH * 3.0f;
            bar(-BW, BW, 0.05f, 0.1f, 0.3f, 0.16f);                            // track
            if (sfrac > 0.0f) bar(-BW, -BW + 2.0f*BW*sfrac, 0.35f, 0.65f, 1.0f, 0.7f);  // blue fill
        }

        // The ENEMY base core (red) at the far end — the lane objective. Mirrors our pylon but
        // tinted hostile red, with red rising motes + an always-on health bar.
        if (enemy_core_health > 0.0f && glyph_loaded) {
            const float frac = enemy_core_health / CORE_MAX_HEALTH;
            static std::vector<dc::renderer::Mat4> egl_pw;   // re-posed each frame for idle (offset phase so the two cores aren't in lockstep)
            std::vector<dc::renderer::AnimLayer> egil;
            if (glyph_data.idle.valid()) egil.push_back({ &glyph_data.idle, t_now + 1.7f, -1 });
            dc::renderer::pose_model(glyph_data, egil, 0.0f, egl_pw);
            mat4 gpl; glm_mat4_identity(gpl);
            vec3 gpos = { enemy_core_pos[0], enemy_core_pos[1] - 0.5f, enemy_core_pos[2] }; glm_translate(gpl, gpos);
            vec3 gsc = { 1.3f, 1.3f, 1.3f }; glm_scale(gpl, gsc);
            vec3 red_tint = { 0.95f, 0.16f + 0.10f*frac, 0.14f + 0.08f*frac };
            renderer.draw_model(glyph_model, egl_pw, gpl, red_tint);
            auto jit = [&]() { spark_rng = spark_rng * 1664525u + 1013904223u; return (spark_rng >> 8) * (1.0f / 16777216.0f); };
            for (int k = 0; k < 2 && sparks.size() < 1800; ++k) {
                Spark s;
                s.pos[0] = enemy_core_pos[0] + (jit()-0.5f)*2.0f; s.pos[1] = enemy_core_pos[1] + jit()*CORE_H; s.pos[2] = enemy_core_pos[2] + (jit()-0.5f)*2.0f;
                s.vel[0] = (jit()-0.5f)*0.3f; s.vel[1] = 1.4f + jit()*1.4f; s.vel[2] = (jit()-0.5f)*0.3f;
                s.color[0] = 1.0f; s.color[1] = 0.3f; s.color[2] = 0.2f;
                s.age = 0.0f; s.life = 1.0f + jit()*0.6f; s.grav = -0.5f; s.size_mul = 1.3f; s.alpha_mul = 1.8f;
                sparks.push_back(s);
            }
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            const float BW = 2.0f, BH = 0.18f;
            vec3 mid = { enemy_core_pos[0], enemy_core_pos[1] + CORE_H + 1.3f, enemy_core_pos[2] };
            auto bar = [&](float lx, float rx, float r, float g, float b, float a) {
                auto P = [&](float u, float v) { particle_verts.insert(particle_verts.end(), {
                    mid[0]+R[0]*u+U[0]*v, mid[1]+R[1]*u+U[1]*v, mid[2]+R[2]*u+U[2]*v, r,g,b,a }); };
                P(lx,-BH); P(rx,-BH); P(rx,BH);  P(lx,-BH); P(rx,BH); P(lx,BH);
            };
            bar(-BW, BW, 0.3f, 0.05f, 0.05f, 0.18f);                       // track
            bar(-BW, -BW + 2.0f*BW*frac, 1.0f, 0.25f, 0.15f, 0.7f);        // red fill
        }

        // Solar turrets: cylinder body + a cylinder gun. By day the gun droops to the ground
        // (charging/idle, grey); at night it wakes (orange) and tracks the nearest enemy,
        // spitting red tracers. Built fresh each frame so the gun can aim.
        {
            std::vector<float> tv;
            auto add_cyl = [&](const vec3 p0, const vec3 p1, float rad, int sides) {
                float dx = p1[0]-p0[0], dy = p1[1]-p0[1], dz = p1[2]-p0[2];
                float dl = std::sqrt(dx*dx+dy*dy+dz*dz); if (dl < 1e-4f) return;
                dx/=dl; dy/=dl; dz/=dl;
                float ux = 0, uy = 1, uz = 0; if (std::fabs(dy) > 0.99f) { ux = 1; uy = 0; }
                float w1x = dy*uz-dz*uy, w1y = dz*ux-dx*uz, w1z = dx*uy-dy*ux;
                float w1l = std::sqrt(w1x*w1x+w1y*w1y+w1z*w1z); if (w1l>1e-4f){w1x/=w1l;w1y/=w1l;w1z/=w1l;}
                float w2x = dy*w1z-dz*w1y, w2y = dz*w1x-dx*w1z, w2z = dx*w1y-dy*w1x;
                auto V = [&](float x,float y,float z){ tv.insert(tv.end(), {x,y,z,0,1,0,0,0,0}); };
                for (int k = 0; k < sides; ++k) {
                    float a0 = 6.2831853f*k/sides, a1 = 6.2831853f*(k+1)/sides;
                    float c0=std::cos(a0),s0=std::sin(a0),c1=std::cos(a1),s1=std::sin(a1);
                    float o0x=(w1x*c0+w2x*s0)*rad,o0y=(w1y*c0+w2y*s0)*rad,o0z=(w1z*c0+w2z*s0)*rad;
                    float o1x=(w1x*c1+w2x*s1)*rad,o1y=(w1y*c1+w2y*s1)*rad,o1z=(w1z*c1+w2z*s1)*rad;
                    V(p0[0]+o0x,p0[1]+o0y,p0[2]+o0z); V(p0[0]+o1x,p0[1]+o1y,p0[2]+o1z); V(p1[0]+o1x,p1[1]+o1y,p1[2]+o1z);
                    V(p0[0]+o0x,p0[1]+o0y,p0[2]+o0z); V(p1[0]+o1x,p1[1]+o1y,p1[2]+o1z); V(p1[0]+o0x,p1[1]+o0y,p1[2]+o0z);
                }
            };
            const bool night = is_night();
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            // --- MORTARS: the heavy mortar MODEL at each piece + arcing shells in flight. ---
            if (mortar_loaded && !mortar_pos.empty()) {
                static std::vector<dc::renderer::Mat4> mortar_pw;
                if (mortar_pw.empty()) dc::renderer::pose_model(mortar_data, {}, 0.0f, mortar_pw);
                for (const auto& mp : mortar_pos) {
                    mat4 mpl; glm_mat4_identity(mpl);
                    vec3 mpos = { mp.x, mp.y, mp.z }; glm_translate(mpl, mpos);
                    glm_rotate_y(mpl, MODEL_YAW_OFFSET, mpl);
                    vec3 mtint = { 0.85f, 0.85f, 0.92f };
                    renderer.draw_model(mortar_model, mortar_pw, mpl, mtint);
                }
            }
            // Arcing shells: a glowing sphere on a parabola from muzzle -> impact + a smoke puff.
            for (const auto& sh : mortar_shells) {
                const float u = sh.dur > 0.0f ? (sh.t / sh.dur) : 1.0f;
                const float px = sh.from[0] + (sh.impact[0]-sh.from[0]) * u;
                const float pz = sh.from[2] + (sh.impact[2]-sh.from[2]) * u;
                const float arc = 9.0f * u * (1.0f - u);   // parabolic lob height
                const float py = sh.from[1] + (sh.impact[1]-sh.from[1]) * u + arc;
                const float s = 0.22f;
                auto P = [&](float a, float b){ particle_verts.insert(particle_verts.end(), {
                    px + R[0]*a + U[0]*b, py + R[1]*a + U[1]*b, pz + R[2]*a + U[2]*b, 1.0f, 0.75f, 0.30f, 1.0f }); };
                P(-s,-s);P(s,-s);P(s,s); P(-s,-s);P(s,s);P(-s,s);
            }
            // Turret housing model posed once (static); each turret draws it yawed to its
            // HELD aim. Barrel is procedural (3D). Turrets never power down — they hold the
            // last aim when idle — and fire visible tracer rounds on a per-turret timer.
            std::vector<dc::renderer::Mat4> turret_pw;
            if (turret_loaded) dc::renderer::pose_model(turret_data, {}, 0.0f, turret_pw);
            for (int i = 0; i < static_cast<int>(turret_pos.size()); ++i) {
                const TPos& tp = turret_pos[i];
                vec3 pivot = { tp.x, tp.y + 1.1f, tp.z };
                const dc::entity::Entity* tgt = turret_target(tp.x, tp.z);   // active day + night
                if (tgt) {   // update the held aim toward the target (else keep last)
                    vec3 d = { tgt->position[0] - pivot[0],
                               (terrain.height(tgt->position[0], tgt->position[2]) + 0.8f) - pivot[1],
                               tgt->position[2] - pivot[2] };
                    float dl = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
                    if (dl > 1e-4f) turret_aim[i] = { d[0]/dl, d[1]/dl, d[2]/dl };
                }
                vec3 dir = { turret_aim[i].x, turret_aim[i].y, turret_aim[i].z };
                // Body model (yawed to the held aim) at full color — never droops/dims.
                if (turret_loaded) {
                    const float tyaw = std::atan2(dir[2], dir[0]);
                    mat4 tpl; glm_mat4_identity(tpl);
                    vec3 tpos = { tp.x, tp.y, tp.z }; glm_translate(tpl, tpos);
                    glm_rotate_y(tpl, -tyaw + MODEL_YAW_OFFSET, tpl);
                    vec3 tmc = { 1.0f, 1.0f, 1.0f };
                    renderer.draw_model(turret_model, turret_pw, tpl, tmc);
                } else {
                    vec3 base = { tp.x, tp.y, tp.z }, top = { tp.x, tp.y + 1.3f, tp.z };
                    add_cyl(base, top, 0.5f, 10);
                }
                vec3 muzzle = { pivot[0] + dir[0]*1.4f, pivot[1] + dir[1]*1.4f, pivot[2] + dir[2]*1.4f };
                add_cyl(pivot, muzzle, 0.18f, 8);             // gun barrel along the held aim
                // Fire a visible tracer round on the per-turret timer (same on every peer).
                if (turret_flash[i] > 0.0f) turret_flash[i] -= dt;
                if (tgt && turret_flash[i] <= 0.0f) {
                    turret_flash[i] = TURRET_FIRE_INTERVAL;
                    const float spd = 50.0f;
                    TBullet b; b.pos[0]=muzzle[0]; b.pos[1]=muzzle[1]; b.pos[2]=muzzle[2];
                    b.vel[0]=dir[0]*spd; b.vel[1]=dir[1]*spd; b.vel[2]=dir[2]*spd; b.life = 0.7f;
                    turret_bullets.push_back(b);
                }
            }
            // ENEMY-BASE turrets: red bodies aimed at our nearest mob/player, firing red tracers.
            {
                // Nearest of our units to a point (allies on host, net_allies on clients, + player).
                auto enearest = [&](float tx, float tz, float& ox, float& oy, float& oz) -> bool {
                    float bd2 = ENEMY_TURRET_RANGE * ENEMY_TURRET_RANGE; bool found = false;
                    auto consider = [&](float px, float pz) {
                        const float dx = px-tx, dz = pz-tz, d2 = dx*dx+dz*dz;
                        if (d2 < bd2) { bd2 = d2; ox = px; oz = pz; oy = terrain.height(px,pz)+0.9f; found = true; }
                    };
                    if (net.role == dc::net::Role::Client) { for (const auto& a : net_allies) consider(a.x, a.z); }
                    else                                   { for (const auto& a : allies)     consider(a.pos[0], a.pos[2]); }
                    if (player.health > 0.0f) consider(player.position[0], player.position[2]);
                    return found;
                };
                for (int i = 0; i < enemy_turret_n; ++i) {
                    const TPos& tp = eturret_pos[i];
                    vec3 pivot = { tp.x, tp.y + 1.1f, tp.z };
                    float ox, oy, oz; const bool have = enearest(tp.x, tp.z, ox, oy, oz);
                    if (have) { vec3 d = { ox-pivot[0], oy-pivot[1], oz-pivot[2] };
                                float dl = std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
                                if (dl > 1e-4f) eturret_aim[i] = { d[0]/dl, d[1]/dl, d[2]/dl }; }
                    vec3 dir = { eturret_aim[i].x, eturret_aim[i].y, eturret_aim[i].z };
                    if (turret_loaded) {
                        const float tyaw = std::atan2(dir[2], dir[0]);
                        mat4 tpl; glm_mat4_identity(tpl);
                        vec3 tpos = { tp.x, tp.y, tp.z }; glm_translate(tpl, tpos);
                        glm_rotate_y(tpl, -tyaw + MODEL_YAW_OFFSET, tpl);
                        vec3 red = { 1.0f, 0.30f, 0.26f };
                        renderer.draw_model(turret_model, turret_pw, tpl, red);
                    }
                    vec3 muzzle = { pivot[0]+dir[0]*1.4f, pivot[1]+dir[1]*1.4f, pivot[2]+dir[2]*1.4f };
                    if (eturret_flash[i] > 0.0f) eturret_flash[i] -= dt;
                    // Host emits tracers from the sim (coupled to real shots); clients approximate here.
                    if (net.role == dc::net::Role::Client && have && eturret_flash[i] <= 0.0f) {
                        eturret_flash[i] = ENEMY_TURRET_CD;
                        const float spd = 48.0f;
                        TBullet b; b.pos[0]=muzzle[0]; b.pos[1]=muzzle[1]; b.pos[2]=muzzle[2];
                        b.vel[0]=dir[0]*spd; b.vel[1]=dir[1]*spd; b.vel[2]=dir[2]*spd; b.life = 0.7f; b.red = true;
                        turret_bullets.push_back(b);
                    }
                }
            }
            // Advance + draw turret tracer rounds: bright additive bolts with a short trail.
            for (std::size_t k = 0; k < turret_bullets.size();) {
                TBullet& b = turret_bullets[k];
                b.life -= dt;
                b.pos[0]+=b.vel[0]*dt; b.pos[1]+=b.vel[1]*dt; b.pos[2]+=b.vel[2]*dt;
                if (b.life <= 0.0f) { turret_bullets[k]=turret_bullets.back(); turret_bullets.pop_back(); continue; }
                float vl = std::sqrt(b.vel[0]*b.vel[0]+b.vel[1]*b.vel[1]+b.vel[2]*b.vel[2]); if (vl<1e-4f) vl=1.0f;
                vec3 vn = { b.vel[0]/vl, b.vel[1]/vl, b.vel[2]/vl };
                // Fat, bright bolts with a 6-segment trail so they read even across the whole lane.
                const float headw = b.red ? 0.40f : 0.28f, seg = b.red ? 0.30f : 0.22f;
                for (int s = 0; s < 6; ++s) {
                    const float back = -s * seg, w = headw - s*(headw*0.14f), bri = 1.0f - s*0.15f;
                    vec3 m = { b.pos[0]+vn[0]*back, b.pos[1]+vn[1]*back, b.pos[2]+vn[2]*back };
                    const float cg = b.red ? 0.18f*bri : 0.82f*bri, cb = b.red ? 0.14f*bri : 0.30f*bri;
                    auto P=[&](float u,float v){ particle_verts.insert(particle_verts.end(), {
                        m[0]+R[0]*u+U[0]*v, m[1]+R[1]*u+U[1]*v, m[2]+R[2]*u+U[2]*v, 1.0f, cg, cb, 1.0f}); };
                    P(-w,-w);P(w,-w);P(w,w); P(-w,-w);P(w,w);P(-w,w);
                }
                ++k;
            }
            turret_mesh.upload(tv);
            vec3 tcol = { 0.55f, 0.56f, 0.62f };   // barrel steel (the housing model carries its own colors)
            renderer.draw_terrain(turret_mesh, tcol, true);

            // ENEMY BARRACKS: the dark-red huts the AI built on its base tiles (host-owned). Each
            // pulses a little when it's about to spawn. (Spawned enemies replicate normally.)
            if (net.role != dc::net::Role::Client && !ebarracks.empty() && barracks_loaded) {
                static std::vector<dc::renderer::Mat4> ebk_pw;
                if (ebk_pw.empty()) dc::renderer::pose_model(barracks_data, {}, 0.0f, ebk_pw);
                for (auto& b : ebarracks) {
                    const float h = terrain.height(b.x, b.z);
                    mat4 ebpl; glm_mat4_identity(ebpl);
                    vec3 ebpos = { b.x, h, b.z }; glm_translate(ebpl, ebpos);
                    glm_rotate_y(ebpl, 3.14159265f + MODEL_YAW_OFFSET, ebpl);   // door faces the lane (toward the player)
                    vec3 ebtint = { 1.0f, 0.42f, 0.38f };   // enemy red barracks
                    renderer.draw_model(barracks_model, ebk_pw, ebpl, ebtint);
                }
            }

            // --- Player-placed DEFENSES: barricades (tinted by remaining HP) + landmines.
            // Turret pieces are drawn by the turret system above (same layout). ---
            {
                const float T = dc::world::TILE, WH = dc::world::WALL_HEIGHT;
                // Emit one axis box (optionally yawed about Y by ca/sa) into a 9-float-vertex buffer.
                auto emit_box = [](std::vector<float>& v, float cx, float cy, float cz,
                                   float hx, float hy, float hz, float ca, float sa) {
                    auto V = [&](float lx, float ly, float lz, float nx, float ny, float nz) {
                        const float rx = ca*lx - sa*lz, rz = sa*lx + ca*lz;
                        const float rnx = ca*nx - sa*nz, rnz = sa*nx + ca*nz;
                        v.insert(v.end(), { cx+rx, cy+ly, cz+rz, rnx, ny, rnz, 0.0f, 0.0f, 0.0f });
                    };
                    const float X0=-hx,X1=hx,Y0=-hy,Y1=hy,Z0=-hz,Z1=hz;
                    V(X1,Y0,Z0,1,0,0);V(X1,Y1,Z0,1,0,0);V(X1,Y1,Z1,1,0,0); V(X1,Y0,Z0,1,0,0);V(X1,Y1,Z1,1,0,0);V(X1,Y0,Z1,1,0,0);
                    V(X0,Y0,Z1,-1,0,0);V(X0,Y1,Z1,-1,0,0);V(X0,Y1,Z0,-1,0,0); V(X0,Y0,Z1,-1,0,0);V(X0,Y1,Z0,-1,0,0);V(X0,Y0,Z0,-1,0,0);
                    V(X0,Y1,Z0,0,1,0);V(X1,Y1,Z0,0,1,0);V(X1,Y1,Z1,0,1,0); V(X0,Y1,Z0,0,1,0);V(X1,Y1,Z1,0,1,0);V(X0,Y1,Z1,0,1,0);
                    V(X0,Y0,Z1,0,-1,0);V(X1,Y0,Z1,0,-1,0);V(X1,Y0,Z0,0,-1,0); V(X0,Y0,Z1,0,-1,0);V(X1,Y0,Z0,0,-1,0);V(X0,Y0,Z0,0,-1,0);
                    V(X0,Y0,Z1,0,0,1);V(X1,Y0,Z1,0,0,1);V(X1,Y1,Z1,0,0,1); V(X0,Y0,Z1,0,0,1);V(X1,Y1,Z1,0,0,1);V(X0,Y1,Z1,0,0,1);
                    V(X1,Y0,Z0,0,0,-1);V(X0,Y0,Z0,0,0,-1);V(X0,Y1,Z0,0,0,-1); V(X1,Y0,Z0,0,0,-1);V(X0,Y1,Z0,0,0,-1);V(X1,Y1,Z0,0,0,-1);
                };
                // Build geometry for one defensive piece into `v`; returns its draw color in col.
                auto emit_barricade = [&](std::vector<float>& v, float cx, float cz, float h, float ca, float sa, float frac, vec3 col) {
                    if (frac <= 0.0f) {   // broken: a low rubble heap
                        emit_box(v, cx, h+0.22f, cz, T*0.5f, 0.22f, 0.32f, ca, sa);
                        col[0]=0.30f; col[1]=0.27f; col[2]=0.23f; return;
                    }
                    // a sturdy chest-high palisade; tints from oak (full) toward charred red (low)
                    emit_box(v, cx, h+WH*0.40f, cz, T*0.5f, WH*0.40f, 0.22f, ca, sa);
                    col[0]=0.42f + 0.30f*(1.0f-frac); col[1]=0.30f*frac + 0.08f; col[2]=0.18f*frac + 0.05f;
                };
                auto emit_mine = [&](std::vector<float>& v, float cx, float cz, float h, bool armed, vec3 col) {
                    emit_box(v, cx, h+0.10f, cz, 0.42f, 0.10f, 0.42f, 1.0f, 0.0f);   // ground pad
                    emit_box(v, cx, h+0.26f, cz, 0.16f, 0.10f, 0.16f, 1.0f, 0.0f);   // trigger bump
                    if (armed) { col[0]=0.80f; col[1]=0.12f; col[2]=0.10f; }         // armed: red
                    else       { col[0]=0.22f; col[1]=0.22f; col[2]=0.24f; }         // spent: dark grey
                };
                // A barracks: a little hut + roof, tinted by the mob type it musters.
                static const float TYCOL[4][3] = {
                    {0.35f,0.62f,1.0f}, {0.25f,0.85f,0.75f}, {0.6f,0.45f,0.95f}, {0.55f,0.45f,0.20f} };
                auto emit_barracks = [&](std::vector<float>& v, float cx, float cz, float h, int tier, vec3 col) {
                    emit_box(v, cx, h+0.7f, cz, 0.85f, 0.7f, 0.85f, 1.0f, 0.0f);    // walls
                    emit_box(v, cx, h+1.55f, cz, 1.0f, 0.22f, 1.0f, 1.0f, 0.0f);    // roof slab
                    const int ti = (tier >= 0 && tier < 4) ? tier : 0;
                    col[0]=TYCOL[ti][0]; col[1]=TYCOL[ti][1]; col[2]=TYCOL[ti][2];
                };
                // A water pool: a sunken slab filling the tile (lower than the ground so it reads
                // as a dug-out pool), drawn translucent blue in the particle pass below.
                auto emit_water = [&](std::vector<float>& v, float cx, float cz, float h, vec3 col) {
                    emit_box(v, cx, h - 0.18f, cz, T*0.5f, 0.2f, T*0.5f, 1.0f, 0.0f);
                    col[0]=0.12f; col[1]=0.35f; col[2]=0.62f;
                };
                // A Sub Pen: a low concrete dock + a sub silhouette poking out (steel grey).
                auto emit_subpen = [&](std::vector<float>& v, float cx, float cz, float h, vec3 col) {
                    emit_box(v, cx, h+0.25f, cz, T*0.5f, 0.25f, T*0.5f, 1.0f, 0.0f);   // dock pad
                    emit_box(v, cx, h+0.6f, cz, T*0.42f, 0.28f, 0.35f, 1.0f, 0.0f);    // sub hull in dock
                    emit_box(v, cx, h+1.0f, cz, 0.25f, 0.3f, 0.25f, 1.0f, 0.0f);       // conning tower
                    col[0]=0.42f; col[1]=0.46f; col[2]=0.52f;
                };
                const auto& ps = live_pieces();
                const auto& hps = live_hp();
                for (std::size_t i = 0; i < ps.size(); ++i) {
                    const auto& pc = ps[i];
                    const float cx=(pc.col+0.5f)*T, cz=(pc.row+0.5f)*T, h=terrain.height(cx,cz);
                    const float ca=std::cos(pc.rot*1.57079633f), sa=std::sin(pc.rot*1.57079633f);
                    const float php = (i < hps.size()) ? hps[i] : 0.0f;
                    std::vector<float> v; vec3 col = {0.5f,0.5f,0.5f};
                    if (pc.piece == static_cast<uint8_t>(dc::game::BuildPiece::Barricade))
                        emit_barricade(v, cx, cz, h, ca, sa, php / dc::game::BARRICADE_MAX_HP, col);
                    else if (pc.piece == static_cast<uint8_t>(dc::game::BuildPiece::Landmine))
                        emit_mine(v, cx, cz, h, php > 0.5f, col);
                    else if (pc.piece == static_cast<uint8_t>(dc::game::BuildPiece::Barracks)) {
                        if (barracks_loaded) {   // the timber barracks MODEL, tinted toward its mob-type color
                            static std::vector<dc::renderer::Mat4> bk_pw;
                            if (bk_pw.empty()) dc::renderer::pose_model(barracks_data, {}, 0.0f, bk_pw);
                            const int ti = (pc.rot >= 0 && pc.rot < 4) ? pc.rot : 0;
                            mat4 bkpl; glm_mat4_identity(bkpl);
                            vec3 bkpos = { cx, h, cz }; glm_translate(bkpl, bkpos);
                            glm_rotate_y(bkpl, MODEL_YAW_OFFSET, bkpl);
                            vec3 bktint = { 0.62f + 0.38f*TYCOL[ti][0], 0.62f + 0.38f*TYCOL[ti][1], 0.62f + 0.38f*TYCOL[ti][2] };
                            renderer.draw_model(barracks_model, bk_pw, bkpl, bktint);
                            continue;
                        }
                        emit_barracks(v, cx, cz, h, pc.rot, col);
                    }
                    else if (pc.piece == static_cast<uint8_t>(dc::game::BuildPiece::Water))
                        emit_water(v, cx, cz, h, col);
                    else if (pc.piece == static_cast<uint8_t>(dc::game::BuildPiece::SubPen))
                        emit_subpen(v, cx, cz, h, col);
                    else if (pc.piece == static_cast<uint8_t>(dc::game::BuildPiece::Shipyard)) {
                        emit_subpen(v, cx, cz, h, col);   // dock; tinted brown (wooden shipyard)
                        col[0]=0.40f; col[1]=0.30f; col[2]=0.18f;
                    }
                    else continue;   // turrets drawn by the turret system
                    if (!v.empty()) { build_mesh[0].upload(v); renderer.draw_terrain(build_mesh[0], col, true); }
                }
                // Ghost preview of the piece you're about to place (green = valid, red = blocked).
                if (building_mode && build_has_target) {
                    std::vector<float> gv;
                    const float cx=(build_col+0.5f)*T, cz=(build_row+0.5f)*T, h=terrain.height(cx,cz);
                    const float ca=std::cos(build_rot*1.57079633f), sa=std::sin(build_rot*1.57079633f);
                    vec3 dummy;
                    if (build_sel == static_cast<int>(dc::game::BuildPiece::Barricade)) emit_barricade(gv, cx, cz, h, ca, sa, 1.0f, dummy);
                    else if (build_sel == static_cast<int>(dc::game::BuildPiece::Landmine)) emit_mine(gv, cx, cz, h, true, dummy);
                    else if (build_sel == static_cast<int>(dc::game::BuildPiece::Barracks)) emit_barracks(gv, cx, cz, h, build_tier, dummy);
                    else if (build_sel == static_cast<int>(dc::game::BuildPiece::Water)) emit_water(gv, cx, cz, h, dummy);
                    else if (build_sel == static_cast<int>(dc::game::BuildPiece::SubPen)) emit_subpen(gv, cx, cz, h, dummy);
                    else if (build_sel == static_cast<int>(dc::game::BuildPiece::Shipyard)) emit_subpen(gv, cx, cz, h, dummy);
                    else emit_box(gv, cx, h+0.6f, cz, 0.5f, 0.6f, 0.5f, 1.0f, 0.0f);   // turret placeholder
                    if (!gv.empty()) {
                        build_ghost_mesh.upload(gv);
                        vec3 gcol; if (build_valid){ gcol[0]=0.3f; gcol[1]=1.0f; gcol[2]=0.4f; }
                                   else { gcol[0]=1.0f; gcol[1]=0.3f; gcol[2]=0.3f; }
                        renderer.draw_terrain(build_ghost_mesh, gcol, true);
                    }
                }
            }

            // Core shield dome enclosing the turrets. CHARGED bright blue at night (brighter
            // with more shield health), flashing RED when it absorbs a hit; DOWN/grey by day
            // (and a broken grey flicker if its health is depleted at night).
            {
                if (shield_flash > 0.0f) shield_flash -= dt;   // host sets on absorb, client on drop
                const float SR = shield_radius;
                vec3 c = { core_pos[0], core_pos[1] + 2.0f, core_pos[2] };
                const float frac = shield_max > 0.0f ? shield_health / shield_max : 0.0f;
                const float fl = shield_flash > 0.0f ? shield_flash / 0.3f : 0.0f;
                float sr, sg, sb, sa;
                if (night && frac > 0.0f) {                    // charged: blue, redder on hit
                    sr = 0.25f + 0.75f * fl;
                    sg = 0.55f * (1.0f - fl) + 0.12f * fl;
                    sb = 1.0f  * (1.0f - fl) + 0.12f * fl;
                    sa = 0.12f + 0.14f * frac + 0.18f * fl;
                } else if (night) {                            // depleted at night: broken flicker
                    sr = 0.45f; sg = 0.42f; sb = 0.45f;
                    sa = 0.03f + 0.025f * dc::fx::flicker(t_now * 5.0f);
                } else {                                        // day: dome idle, but a faint blue
                    sr = 0.25f; sg = 0.45f; sb = 0.75f;          // bubble is still clearly present
                    sa = 0.10f;
                }
                const int RINGS = 12, SEGS = 22; const float sz = 0.16f;
                for (int ri = 1; ri < RINGS; ++ri) {
                    const float th = 3.14159265f * ri / RINGS, st = std::sin(th), ct = std::cos(th);
                    for (int si = 0; si < SEGS; ++si) {
                        const float phi = 6.2831853f * si / SEGS + run_time * 0.2f;   // slow shimmer
                        const float px = c[0] + SR * st * std::cos(phi);
                        const float py = c[1] + SR * ct;
                        const float pz = c[2] + SR * st * std::sin(phi);
                        auto P = [&](float u, float v) { particle_verts.insert(particle_verts.end(), {
                            px + R[0]*u + U[0]*v, py + R[1]*u + U[1]*v, pz + R[2]*u + U[2]*v, sr, sg, sb, sa }); };
                        P(-sz,-sz); P(sz,-sz); P(sz,sz); P(-sz,-sz); P(sz,sz); P(-sz,sz);
                    }
                }
            }
        }

        // Draw torch models (opaque, posed once at rest above). The flame part
        // glows via its emissive material.
        vec3 torch_tint = { 1.0f, 1.0f, 1.0f };
        for (const auto& tr : torches)
            renderer.draw_model(torch_model, torch_part_world, const_cast<vec4*>(tr.placement), torch_tint);

        // Particle pass last: build camera-facing billboards for every flame and
        // draw them additively over the opaque scene.
        const float PARTICLE_SIZE = 0.12f;
        particle_verts.clear();
        for (const auto& tr : torches)
            dc::fx::append_billboards(tr.ps, renderer.cam_right, renderer.cam_up, PARTICLE_SIZE, particle_verts);

        // Coins: glowing gold billboards that bob just off the floor.
        {
            const float cs = 0.25f;
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            for (const auto& c : coins) {
                float bob = terrain.height(c.pos[0], c.pos[2]) + 0.45f + 0.08f * std::sin(t_now * 5.0f + c.pos[0]);
                vec3 ctr = { c.pos[0], bob, c.pos[2] };
                auto P = [&](float ax, float ay) {
                    particle_verts.insert(particle_verts.end(), {
                        ctr[0] + (R[0] * ax + U[0] * ay) * cs,
                        ctr[1] + (R[1] * ax + U[1] * ay) * cs,
                        ctr[2] + (R[2] * ax + U[2] * ay) * cs,
                        1.0f, 0.85f, 0.2f, 1.0f });
                };
                P(-1,-1); P(1,-1); P(1,1);
                P(-1,-1); P(1,1); P(-1,1);
            }
        }

        // Wizard staff bolts: an OBLONG glowing-blue ORB (a UV sphere stretched along its travel
        // direction) with a bright bluish-white core, trailed by a fading blue PARTICLE stream.
        {
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            std::vector<float> bolt_geo;
            // A UV sphere stretched by `lenScale` along unit-ish dir (n) -> an ellipsoid pointing
            // the way the bolt flies. Emits proper ellipsoid normals so draw_glow's depth reads right.
            auto orb_into = [&](float cx, float cy, float cz, float nx, float ny, float nz, float rad, float lenScale) {
                float fl = std::sqrt(nx*nx + ny*ny + nz*nz);
                if (fl < 1e-4f) { nx = 0; ny = 0; nz = 1; fl = 1; }
                nx /= fl; ny /= fl; nz /= fl;
                const float upx = (std::fabs(ny) < 0.9f) ? 0.0f : 1.0f, upy = (std::fabs(ny) < 0.9f) ? 1.0f : 0.0f, upz = 0.0f;
                float ux = upy*nz - upz*ny, uy = upz*nx - upx*nz, uz = upx*ny - upy*nx;   // u = up x n
                float ul = std::sqrt(ux*ux + uy*uy + uz*uz); ux/=ul; uy/=ul; uz/=ul;
                const float vx = ny*uz - nz*uy, vy = nz*ux - nx*uz, vz = nx*uy - ny*ux;   // v = n x u
                const int ST = 4, SL = 7; const float PI = 3.14159265f;
                auto sp = [](float t, float p, float& x, float& y, float& z){ x = std::sin(t)*std::cos(p); y = std::cos(t); z = std::sin(t)*std::sin(p); };
                auto V = [&](float sx, float sy, float sz) {
                    const float px = cx + (ux*sx + vx*sy)*rad + nx*sz*rad*lenScale;
                    const float py = cy + (uy*sx + vy*sy)*rad + ny*sz*rad*lenScale;
                    const float pz = cz + (uz*sx + vz*sy)*rad + nz*sz*rad*lenScale;
                    const float gnu = sx/rad, gnv = sy/rad, gnn = sz/(rad*lenScale);
                    float wnx = ux*gnu + vx*gnv + nx*gnn, wny = uy*gnu + vy*gnv + ny*gnn, wnz = uz*gnu + vz*gnv + nz*gnn;
                    float nl = std::sqrt(wnx*wnx + wny*wny + wnz*wnz); if (nl < 1e-5f) nl = 1;
                    bolt_geo.insert(bolt_geo.end(), { px, py, pz, wnx/nl, wny/nl, wnz/nl, 0.f, 0.f, 0.f });
                };
                for (int i = 0; i < ST; ++i) { const float t0 = PI*i/ST, t1 = PI*(i+1)/ST;
                    for (int j = 0; j < SL; ++j) { const float p0 = 2*PI*j/SL, p1 = 2*PI*(j+1)/SL;
                        float a1,a2,a3,b1,b2,b3,c1,c2,c3,d1,d2,d3;
                        sp(t0,p0,a1,a2,a3); sp(t1,p0,b1,b2,b3); sp(t1,p1,c1,c2,c3); sp(t0,p1,d1,d2,d3);
                        V(a1,a2,a3);V(b1,b2,b3);V(c1,c2,c3); V(a1,a2,a3);V(c1,c2,c3);V(d1,d2,d3); } }
            };
            auto draw_bolt = [&](float x, float y, float z, bool big, float dx, float dy, float dz) {
                orb_into(x, y, z, dx, dy, dz, big ? 0.26f : 0.16f, 1.9f);   // stretched ~1.9x along travel
            };
            if (net.role == dc::net::Role::Client) for (const auto& b : render_bolts) draw_bolt(b.pos[0], b.pos[1], b.pos[2], b.big, 0, 0, 1);
            else for (const auto& b : bolts) draw_bolt(b.pos[0], b.pos[1], b.pos[2], b.big, b.dir[0], b.dir[1], b.dir[2]);
            if (!bolt_geo.empty()) {
                static dc::renderer::Mesh bolt_mesh;
                bolt_mesh.upload(bolt_geo);
                vec3 bcol = { 0.40f, 0.62f, 1.30f };   // glowing blue (draw_glow adds the hot bluish-white core)
                renderer.draw_glow(bolt_mesh, bcol);
            }
            // Fading PARTICLE TRAIL: age + cull existing sparks, spawn fresh ones at each live
            // bolt's tail, then draw them all as additive blue billboards that fade with age.
            for (std::size_t s = 0; s < bolt_sparks.size(); ) {
                bolt_sparks[s].age += dt;
                if (bolt_sparks[s].age >= bolt_sparks[s].life) { bolt_sparks[s] = bolt_sparks.back(); bolt_sparks.pop_back(); }
                else ++s;
            }
            auto spawn_spark = [&](float x, float y, float z, float dx, float dy, float dz, bool big) {
                if (bolt_sparks.size() >= 700) return;
                float dl = std::sqrt(dx*dx + dy*dy + dz*dz); if (dl < 1e-4f) { dx = 0; dy = 0; dz = 1; dl = 1; }
                const float back = big ? 0.24f : 0.16f;
                const float jx = std::sin(x*12.9f + z*7.7f), jy = std::sin(y*9.3f + x*4.1f), jz = std::sin(z*6.1f + y*8.8f);
                BoltSpark spk;
                spk.pos[0] = x - dx/dl*back + jx*0.05f; spk.pos[1] = y - dy/dl*back + jy*0.05f; spk.pos[2] = z - dz/dl*back + jz*0.05f;
                spk.life = 0.38f; spk.sz = big ? 0.16f : 0.11f;
                bolt_sparks.push_back(spk);
            };
            if (net.role == dc::net::Role::Client) for (const auto& b : render_bolts) spawn_spark(b.pos[0], b.pos[1], b.pos[2], 0, 0, 1, b.big);
            else for (const auto& b : bolts) spawn_spark(b.pos[0], b.pos[1], b.pos[2], b.dir[0], b.dir[1], b.dir[2], b.big);
            for (const auto& spk : bolt_sparks) {
                const float a = 1.0f - spk.age / spk.life, sz = spk.sz * a;
                auto P = [&](float u, float v) { particle_verts.insert(particle_verts.end(), {
                    spk.pos[0] + R[0]*u + U[0]*v, spk.pos[1] + R[1]*u + U[1]*v, spk.pos[2] + R[2]*u + U[2]*v,
                    0.45f*a, 0.65f*a, 1.0f*a, a }); };
                P(-sz,-sz);P(sz,-sz);P(sz,sz); P(-sz,-sz);P(sz,sz);P(-sz,sz);
            }
        }

        // Wizard magic shields: a glowing translucent hexagon barrier facing the block dir.
        for (const auto& ms : magic_shields) {
            const float fwx = std::cos(ms.yaw), fwz = std::sin(ms.yaw);
            const float rx = fwz, rz = -fwx;            // right = perp to forward in xz
            const float shimmer = 0.85f + 0.15f * std::sin(t_now * 8.0f);
            auto hexpt = [&](float ang, float rad, float& ox, float& oy, float& oz) {
                ox = ms.x + rx * std::cos(ang) * rad;
                oy = ms.y + std::sin(ang) * rad;
                oz = ms.z + rz * std::cos(ang) * rad;
            };
            const float RAD = 0.95f;
            for (int k = 0; k < 6; ++k) {
                const float a0 = 6.2831853f * k / 6, a1 = 6.2831853f * (k + 1) / 6;
                float x0,y0,z0,x1,y1,z1;
                hexpt(a0, RAD, x0, y0, z0); hexpt(a1, RAD, x1, y1, z1);
                // filled translucent face (center + two rim points)
                particle_verts.insert(particle_verts.end(), {
                    ms.x, ms.y, ms.z, 0.35f, 0.65f, 1.0f, 0.16f * shimmer,
                    x0, y0, z0, 0.35f, 0.65f, 1.0f, 0.10f * shimmer,
                    x1, y1, z1, 0.35f, 0.65f, 1.0f, 0.10f * shimmer });
                // brighter rim band (a thin quad along the edge)
                float x0i,y0i,z0i,x1i,y1i,z1i; hexpt(a0, RAD*0.82f, x0i,y0i,z0i); hexpt(a1, RAD*0.82f, x1i,y1i,z1i);
                particle_verts.insert(particle_verts.end(), {
                    x0i,y0i,z0i, 0.6f,0.85f,1.0f,0.5f*shimmer,  x0,y0,z0, 0.6f,0.85f,1.0f,0.6f*shimmer,  x1,y1,z1, 0.6f,0.85f,1.0f,0.6f*shimmer,
                    x0i,y0i,z0i, 0.6f,0.85f,1.0f,0.5f*shimmer,  x1,y1,z1, 0.6f,0.85f,1.0f,0.6f*shimmer,  x1i,y1i,z1i, 0.6f,0.85f,1.0f,0.5f*shimmer });
            }
        }

        // XP orbs: blue glowing motes that bob just off the floor, with a couple of
        // smaller sparkle billboards orbiting each for a "little particles" shimmer.
        {
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            auto quad = [&](float cx, float cy, float cz, float sz, float r, float g, float b, float a) {
                vec3 ctr = { cx, cy, cz };
                auto P = [&](float ax, float ay) {
                    particle_verts.insert(particle_verts.end(), {
                        ctr[0] + (R[0] * ax + U[0] * ay) * sz,
                        ctr[1] + (R[1] * ax + U[1] * ay) * sz,
                        ctr[2] + (R[2] * ax + U[2] * ay) * sz, r, g, b, a });
                };
                P(-1,-1); P(1,-1); P(1,1);
                P(-1,-1); P(1,1); P(-1,1);
            };
            for (const auto& o : xp_orbs) {
                const float ph = t_now * 4.0f + o.bob;
                const float bob = terrain.height(o.pos[0], o.pos[2]) + 0.5f + 0.08f * std::sin(ph);
                quad(o.pos[0], bob, o.pos[2], 0.20f, 0.35f, 0.55f, 1.0f, 0.9f);   // soft outer glow
                quad(o.pos[0], bob, o.pos[2], 0.10f, 0.70f, 0.90f, 1.0f, 1.0f);   // bright core
                for (int s = 0; s < 3; ++s) {
                    const float a = ph * 1.7f + s * 2.094f;
                    quad(o.pos[0] + std::cos(a) * 0.18f, bob + std::sin(a * 1.3f) * 0.12f,
                         o.pos[2] + std::sin(a) * 0.18f, 0.045f, 0.55f, 0.85f, 1.0f, 0.9f);   // sparkles
                }
            }
        }

        // Elite aura: golden motes swirling around each rare elite enemy so they read as
        // dangerous at a glance (on every screen — elite is replicated).
        {
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            auto quad = [&](float cx, float cy, float cz, float sz, float r, float g, float b, float a) {
                vec3 ctr = { cx, cy, cz };
                auto P = [&](float ax, float ay) {
                    particle_verts.insert(particle_verts.end(), {
                        ctr[0] + (R[0] * ax + U[0] * ay) * sz,
                        ctr[1] + (R[1] * ax + U[1] * ay) * sz,
                        ctr[2] + (R[2] * ax + U[2] * ay) * sz, r, g, b, a });
                };
                P(-1,-1); P(1,-1); P(1,1);
                P(-1,-1); P(1,1); P(-1,1);
            };
            for (const auto& en : entities.items) {
                if (en.type != dc::entity::EntityType::Enemy || !en.elite) continue;
                const float gy = (en.kind == dc::entity::EnemyKind::Flying)
                               ? terrain.height(en.position[0], en.position[2]) + dc::entity::FLY_HOVER
                               : terrain.height(en.position[0], en.position[2]);
                const float cx = en.position[0], cz = en.position[2], cy = gy + 1.0f;
                const int N = 8;
                for (int i = 0; i < N; ++i) {
                    const float a = t_now * 1.5f + i * (6.2831853f / N);
                    const float rr = 0.8f + 0.15f * std::sin(t_now * 2.0f + i);
                    const float yy = cy + 0.8f * std::sin(t_now * 1.3f + i * 1.7f);
                    quad(cx + std::cos(a) * rr, yy, cz + std::sin(a) * rr, 0.075f, 1.0f, 0.82f, 0.3f, 0.95f);
                }
            }
        }

        // Enemy projectiles: ALL render as 3D glowing cylinders aligned to their travel
        // direction (round rods from any angle, not flat quads), trailing a few embers.
        // Eye "lasers" are long + thin; other shots (e.g. the purple enemy's) are shorter,
        // fatter bolts. Color comes from each shot.
        {
            for (const auto& pr : entities.projectiles) {
                const vec3 ctr = { pr.pos[0], pr.pos[1], pr.pos[2] };
                const float cr = pr.color[0], cg = pr.color[1], cb = pr.color[2];
                // Orthonormal frame: long axis = travel direction; w1,w2 span the cross-section.
                float lx = pr.vel[0], ly = pr.vel[1], lz = pr.vel[2];
                float ll = std::sqrt(lx*lx + ly*ly + lz*lz);
                if (ll > 1e-4f) { lx /= ll; ly /= ll; lz /= ll; } else { lx = 1; ly = 0; lz = 0; }
                float ux = 0, uy = 1, uz = 0; if (std::fabs(ly) > 0.99f) { ux = 1; uy = 0; }   // avoid parallel
                float w1x = ly*uz - lz*uy, w1y = lz*ux - lx*uz, w1z = lx*uy - ly*ux;
                float w1l = std::sqrt(w1x*w1x + w1y*w1y + w1z*w1z);
                if (w1l > 1e-4f) { w1x /= w1l; w1y /= w1l; w1z /= w1l; }
                float w2x = ly*w1z - lz*w1y, w2y = lz*w1x - lx*w1z, w2z = lx*w1y - ly*w1x;
                const float psz = pr.radius / dc::entity::RANGED_SHOT_RADIUS;   // elite shots are fatter
                const float HALF = (pr.beam ? 0.9f : 0.45f) * (pr.beam ? 1.0f : psz);   // bolt grows; laser stays long
                const float core = (pr.beam ? 0.10f : 0.15f) * psz, glow = (pr.beam ? 0.26f : 0.34f) * psz;
                auto cyl = [&](float rad, float r, float g, float b, float a) {
                    const int SIDES = 8;
                    auto vert = [&](float ang, float end) {
                        const float c = std::cos(ang), s = std::sin(ang);
                        const float ox = (w1x*c + w2x*s) * rad, oy = (w1y*c + w2y*s) * rad, oz = (w1z*c + w2z*s) * rad;
                        particle_verts.insert(particle_verts.end(), {
                            ctr[0] + lx*end + ox, ctr[1] + ly*end + oy, ctr[2] + lz*end + oz, r, g, b, a });
                    };
                    for (int k = 0; k < SIDES; ++k) {
                        const float a0 = 6.2831853f * k / SIDES, a1 = 6.2831853f * (k + 1) / SIDES;
                        vert(a0, -HALF); vert(a1, -HALF); vert(a1, HALF);   // side quad -> 2 tris
                        vert(a0, -HALF); vert(a1,  HALF); vert(a0, HALF);
                    }
                };
                cyl(core, cr, cg, cb, 1.0f);                            // bright solid core rod
                cyl(glow, cr*0.6f + 0.4f, cg*0.4f, cb*0.4f, 0.30f);    // wider faint glow tube
                // Leave a little fading trail of embers behind the shot (drops a mote each
                // frame at its spot; the shot flies on, the motes linger + fade).
                if (sparks.size() < 1500) {
                    auto jit = [&]() { spark_rng = spark_rng * 1664525u + 1013904223u; return ((spark_rng >> 8) * (1.0f / 16777216.0f)) - 0.5f; };
                    Spark s;
                    s.pos[0] = ctr[0] + jit() * 0.16f; s.pos[1] = ctr[1] + jit() * 0.16f; s.pos[2] = ctr[2] + jit() * 0.16f;
                    s.vel[0] = s.vel[1] = s.vel[2] = 0.0f;
                    s.color[0] = cr; s.color[1] = cg * 0.5f + 0.1f; s.color[2] = cb * 0.5f + 0.1f;
                    s.age = 0.0f; s.life = 0.35f; s.grav = 0.0f; s.size_mul = 1.2f; s.alpha_mul = 1.6f;
                    sparks.push_back(s);
                }
            }
        }

        // Shield-bash nova: an expanding translucent shell of white points (a sphere
        // growing from the caster), additive + fading as it spreads. Drawn for the local
        // caster (predicted) and any remote that's bashing.
        {
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            auto bash_sphere = [&](float cx, float cy, float cz, float radius, float alpha,
                                   float cr, float cg, float cb) {
                if (radius <= 0.05f || alpha <= 0.0f) return;
                const float ps = 0.16f;
                const int N = 64;
                for (int i = 0; i < N; ++i) {
                    const float gy = 1.0f - 2.0f * (i + 0.5f) / N;           // -1..1
                    const float rr = std::sqrt(std::max(0.0f, 1.0f - gy * gy));
                    const float phi = i * 2.399963f;                         // golden angle
                    const float px = cx + std::cos(phi) * rr * radius;
                    const float py = cy + gy * radius;
                    const float pz = cz + std::sin(phi) * rr * radius;
                    auto P = [&](float ax, float ay) {
                        particle_verts.insert(particle_verts.end(), {
                            px + (R[0]*ax + U[0]*ay) * ps, py + (R[1]*ax + U[1]*ay) * ps,
                            pz + (R[2]*ax + U[2]*ay) * ps, cr, cg, cb, alpha });
                    };
                    P(-1,-1); P(1,-1); P(1,1);  P(-1,-1); P(1,1); P(-1,1);
                }
            };
            // Wizard's nova reads as a MAGIC wave (violet); the knight's is white shockwave.
            const bool wiz_nova = (my_look.weapon_class == 1);
            if (bash.active && player.shield) {
                const float prog = bash.time / player.shield->bash_duration;
                if (wiz_nova) bash_sphere(player.position[0], (player.position[1] - dc::world::EYE_HEIGHT) + 1.0f,
                                          player.position[2], bash.radius, (1.0f - prog) * 0.6f, 0.65f, 0.35f, 1.0f);
                else          bash_sphere(player.position[0], (player.position[1] - dc::world::EYE_HEIGHT) + 1.0f,
                                          player.position[2], bash.radius, (1.0f - prog) * 0.6f, 1.0f, 1.0f, 1.0f);
            }
            for (const auto& rp : remotes)
                if (rp.bash_active) {
                    const bool rwiz = (look_for(rp.id).weapon_class == 1);
                    bash_sphere(rp.pos[0], (rp.pos[1] - dc::world::EYE_HEIGHT) + 1.0f, rp.pos[2],
                                rp.bash_radius, 0.4f, rwiz ? 0.65f : 1.0f, rwiz ? 0.35f : 1.0f, 1.0f);
                }
        }

        // Chest price tags: the cost in 7-segment digits, billboarded above each chest
        // that still has items, shrinking with distance and culled when far (declutter).
        // Depleted chests show nothing (and stay open).
        {
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            const float COST_MAX = 0.30f;                 // world height cap (near)
            const float COST_NEAR = 6.0f, COST_CULL = 22.0f;
            // one billboarded quad in the (R,U) plane, offset (u,v) from `base`, scaled by sc
            auto quad = [&](const vec3 base, float u0, float v0, float u1, float v1, float sc) {
                auto P = [&](float u, float v) {
                    particle_verts.insert(particle_verts.end(), {
                        base[0] + (R[0]*u + U[0]*v) * sc, base[1] + (R[1]*u + U[1]*v) * sc,
                        base[2] + (R[2]*u + U[2]*v) * sc, 1.0f, 0.85f, 0.2f, 1.0f });
                };
                P(u0,v0); P(u1,v0); P(u1,v1);
                P(u0,v0); P(u1,v1); P(u0,v1);
            };
            for (const auto& ch : chests) {
                if (ch.remaining() == 0) continue;   // depleted: no price
                float cx = (ch.col + 0.5f) * dc::world::TILE, cz = (ch.row + 0.5f) * dc::world::TILE;
                float dx = cx - player.position[0], dz = cz - player.position[2];
                float dist = std::sqrt(dx * dx + dz * dz);
                if (dist > COST_CULL) continue;                       // too far: don't render
                float sc = COST_MAX;
                if (dist > COST_NEAR) sc *= COST_NEAR / dist;          // shrink with distance (capped near)

                char num[16]; std::snprintf(num, sizeof num, "%d", ch.cost);
                const int n = static_cast<int>(std::strlen(num));
                const float dw = 0.6f, dh = 1.0f, dt = 0.16f, gap = dw + 0.3f;
                const float total = n * gap - 0.3f;
                vec3 base = { cx, terrain.height(cx, cz) + 2.3f, cz };   // float above the chest
                for (int i = 0; i < n; ++i) {
                    float ox = -total * 0.5f + i * gap;                // center the number, per-digit u offset
                    seven_seg(num[i] - '0', dw, dh, dt, [&](float u0, float v0, float u1, float v1) {
                        quad(base, ox + u0, v0, ox + u1, v1, sc);
                    });
                }
            }
        }

        // Drone vendors: a steel-blue gunner drone resting on the ground with its price
        // (gold 7-seg) floating above. Disappears once bought.
        {
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            for (const auto& dv : drone_vendors) {
                if (dv.bought) continue;
                const float gy = terrain.height(dv.x, dv.z);
                // The drone for sale: the actual drone model sitting on the ground (props at
                // rest, not animating), with a gentle bob. Falls back to a glowing disc.
                const float by = gy + 0.30f + std::sin(t_now * 2.0f + dv.x) * 0.05f;
                if (drone_loaded) {
                    static std::vector<dc::renderer::Mat4> dvend_pw;
                    if (dvend_pw.empty()) dc::renderer::pose_model(drone_data, {}, 0.0f, dvend_pw);  // rest pose (static)
                    mat4 dvm; glm_mat4_identity(dvm);
                    vec3 dp = { dv.x, by, dv.z }; glm_translate(dvm, dp);
                    glm_rotate_y(dvm, t_now * 0.4f + dv.x, dvm);   // slow idle turn
                    vec3 dsc = { 0.8f, 0.8f, 0.8f }; glm_scale(dvm, dsc);
                    vec3 white = { 1.0f, 1.0f, 1.0f };
                    renderer.draw_model(drone_model, dvend_pw, dvm, white);
                } else {
                    const int N = 14; const float s = 0.33f;
                    for (int k = 0; k < N; ++k) {
                        const float a0 = 6.2831853f * k / N, a1 = 6.2831853f * (k + 1) / N;
                        auto P = [&](float u, float v) {
                            particle_verts.insert(particle_verts.end(), {
                                dv.x + (R[0]*u + U[0]*v), by + (R[1]*u + U[1]*v), dv.z + (R[2]*u + U[2]*v), 0.55f, 0.7f, 0.95f, 0.95f });
                        };
                        P(0, 0); P(s*std::cos(a0), s*std::sin(a0)); P(s*std::cos(a1), s*std::sin(a1));
                    }
                }
                // Floating price (gold), like a chest tag.
                auto pquad = [&](const vec3 base, float u0, float v0, float u1, float v1, float sc) {
                    auto P = [&](float u, float v) {
                        particle_verts.insert(particle_verts.end(), {
                            base[0] + (R[0]*u + U[0]*v) * sc, base[1] + (R[1]*u + U[1]*v) * sc,
                            base[2] + (R[2]*u + U[2]*v) * sc, 1.0f, 0.85f, 0.2f, 1.0f });
                    };
                    P(u0,v0); P(u1,v0); P(u1,v1);  P(u0,v0); P(u1,v1); P(u0,v1);
                };
                char num[16]; std::snprintf(num, sizeof num, "%d", DRONE_COST);
                const int n = static_cast<int>(std::strlen(num));
                const float dw = 0.6f, dh = 1.0f, dt2 = 0.16f, gap = dw + 0.3f, total = n * gap - 0.3f;
                vec3 base = { dv.x, gy + 1.7f, dv.z };
                for (int i = 0; i < n; ++i) {
                    float ox = -total * 0.5f + i * gap;
                    seven_seg(num[i] - '0', dw, dh, dt2, [&](float u0, float v0, float u1, float v1) {
                        pquad(base, ox + u0, v0, ox + u1, v1, 0.3f);
                    });
                }
            }
        }

        // Updraft pads: a column of cool-blue motes rising from each pad (looping phase =
        // a continuous stream), so the launch spots are obvious.
        {
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            const float colH = 3.2f; const int M = 12;
            for (const auto& u : updrafts) {
                const float base = terrain.height(u.x, u.z) + 0.1f;
                for (int i = 0; i < M; ++i) {
                    const float ph = std::fmod(run_time * 1.6f + colH * i / M, colH);   // 0..colH rising
                    const float al = (1.0f - ph / colH) * 0.5f;                          // fade toward the top
                    const float mx = u.x + std::sin(run_time * 3.0f + i * 1.7f) * 0.25f;
                    const float mz = u.z + std::cos(run_time * 2.3f + i * 1.1f) * 0.25f;
                    const float my = base + ph, s = 0.07f;
                    auto P = [&](float uu, float vv) {
                        particle_verts.insert(particle_verts.end(), {
                            mx + (R[0]*uu + U[0]*vv), my + (R[1]*uu + U[1]*vv), mz + (R[2]*uu + U[2]*vv), 0.3f, 0.7f, 1.0f, al });
                    };
                    P(-s,-s); P(s,-s); P(s,s); P(-s,-s); P(s,s); P(-s,s);
                }
            }
        }


        // Trailblazer fire: orange embers along each trail segment, rising + fading as the
        // segment burns out.
        {
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            for (auto& t : trails) {
                if (t.life <= 0.0f) continue;
                for (auto& sg : t.segs) {
                    const float fade = 1.0f - sg.age / t.life;
                    if (fade <= 0.0f) continue;
                    const float by = terrain.height(sg.x, sg.z) + 0.15f + (1.0f - fade) * 0.5f;   // rise as it ages
                    const float sz = 0.16f * fade + 0.05f, al = fade * 0.5f;
                    const float jx = std::sin(run_time * 7.0f + sg.x) * 0.12f * fade;
                    const float jz = std::cos(run_time * 6.0f + sg.z) * 0.12f * fade;
                    const float px = sg.x + jx, pz = sg.z + jz;
                    auto P = [&](float u, float v) {
                        particle_verts.insert(particle_verts.end(), {
                            px + (R[0]*u + U[0]*v), by + (R[1]*u + U[1]*v), pz + (R[2]*u + U[2]*v), 1.0f, 0.45f, 0.1f, al });
                    };
                    P(-sz,-sz); P(sz,-sz); P(sz,sz); P(-sz,-sz); P(sz,sz); P(-sz,sz);
                }
            }
        }

        // Supersonic dodge gust: a counter-clockwise spiral of white motes bursting out
        // from where you dodged, expanding + fading over its short life (less spherical).
        if (ss_anim > 0.0f) {
            ss_anim -= dt;
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            const float prog = 1.0f - ss_anim / SS_ANIM_TIME;       // 0..1 expand
            const float maxR = SUPERSONIC_RADIUS * prog;
            const float al = (1.0f - prog) * 0.6f;                  // fade as it expands
            const float baseY = terrain.height(ss_pos[0], ss_pos[2]) + 0.5f;
            const int ARMS = 3, PER = 14;
            for (int arm = 0; arm < ARMS; ++arm)
                for (int j = 1; j <= PER; ++j) {
                    const float fr = static_cast<float>(j) / PER;
                    const float rr = maxR * fr;
                    const float ang = arm * (6.2831853f / ARMS) + fr * 6.0f + run_time * 4.0f;   // CCW swirl
                    const float px = ss_pos[0] + std::cos(ang) * rr, pz = ss_pos[2] + std::sin(ang) * rr;
                    const float py = baseY + fr * 0.5f, sz = 0.06f;
                    auto P = [&](float u, float v) {
                        particle_verts.insert(particle_verts.end(), {
                            px + (R[0]*u + U[0]*v), py + (R[1]*u + U[1]*v), pz + (R[2]*u + U[2]*v), 0.85f, 0.92f, 1.0f, al });
                    };
                    P(-sz,-sz); P(sz,-sz); P(sz,sz); P(-sz,-sz); P(sz,sz); P(-sz,sz);
                }
        }

        // Block bubble: while blocking, a faint transparent sphere surrounds you (the shield
        // is omnidirectional now). It flashes red for a moment whenever it absorbs a hit.
        if (block_flash > 0.0f) block_flash -= dt;
        if (blocking && !dead) {
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            const float fl = block_flash > 0.0f ? block_flash / 0.3f : 0.0f;   // 0..1 red pulse
            // Calm blue normally; lerp toward red on absorb.
            const float cr = 0.35f + 0.65f * fl, cg = 0.55f * (1.0f - fl) + 0.10f * fl, cb = 0.95f * (1.0f - fl) + 0.10f * fl;
            const float al = 0.10f + 0.22f * fl;   // subtle, brighter on a hit
            const vec3 c = { player.position[0], player.position[1] - dc::world::EYE_HEIGHT + 1.0f, player.position[2] };
            const float rad = 1.4f, sz = 0.09f;
            const int RINGS = 6, SEGS = 12;
            for (int ri = 0; ri <= RINGS; ++ri) {
                const float theta = 3.14159265f * ri / RINGS;
                const float st = std::sin(theta), ct = std::cos(theta);
                for (int si = 0; si < SEGS; ++si) {
                    const float phi = 6.2831853f * si / SEGS;
                    const float px = c[0] + rad * st * std::cos(phi);
                    const float py = c[1] + rad * ct;
                    const float pz = c[2] + rad * st * std::sin(phi);
                    auto P = [&](float u, float v) {
                        particle_verts.insert(particle_verts.end(), {
                            px + (R[0]*u + U[0]*v), py + (R[1]*u + U[1]*v), pz + (R[2]*u + U[2]*v), cr, cg, cb, al });
                    };
                    P(-sz,-sz); P(sz,-sz); P(sz,sz); P(-sz,-sz); P(sz,sz); P(-sz,sz);
                }
            }
        }

        // Melee forcefield bursts: when a melee enemy punches, an expanding shell of
        // orange shockwave motes blasts outward to MELEE_FORCEFIELD_RADIUS and fades. Driven
        // by the replicated punch_anim, so every peer sees it.
        {
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            std::unordered_set<uint32_t> slamming_now;
            for (const auto& en : entities.items) {
                if (en.type != dc::entity::EntityType::Enemy || en.punch_anim <= 0.0f) continue;
                const bool troll_en = (en.kind == dc::entity::EnemyKind::Troll);
                const float ff_rad  = troll_en ? dc::entity::TROLL_RADIUS : dc::entity::MELEE_FORCEFIELD_RADIUS;
                // Troll slam: kick up a wide ring of ground dust ONCE, on the rising edge of the
                // replicated punch_anim, so every peer sees debris across the whole AoE.
                if (troll_en) {
                    slamming_now.insert(en.id);
                    if (!troll_slamming.count(en.id))
                        burst_dust(en.position[0], en.position[2], dc::entity::TROLL_RADIUS, 160);
                }
                const float prog = 1.0f - en.punch_anim / dc::entity::PUNCH_ANIM_TIME;   // 0..1 expand
                const float rr   = ff_rad * prog;
                const float al   = (1.0f - prog) * 0.7f;                                  // fade as it grows
                const float sz   = 0.10f + 0.10f * prog;                                  // motes grow with the shell
                const vec3  c    = { en.position[0], terrain.height(en.position[0], en.position[2]) + 1.0f, en.position[2] };
                const int RINGS = 5, SEGS = 12;
                for (int ri = 0; ri <= RINGS; ++ri) {
                    const float theta = 3.14159265f * ri / RINGS;        // 0..pi (pole to pole)
                    const float st = std::sin(theta), ct = std::cos(theta);
                    for (int si = 0; si < SEGS; ++si) {
                        const float phi = 6.2831853f * si / SEGS + prog * 2.0f;   // slight swirl as it expands
                        const float px = c[0] + rr * st * std::cos(phi);
                        const float py = c[1] + rr * ct;
                        const float pz = c[2] + rr * st * std::sin(phi);
                        auto P = [&](float u, float v) {
                            particle_verts.insert(particle_verts.end(), {
                                px + (R[0]*u + U[0]*v), py + (R[1]*u + U[1]*v), pz + (R[2]*u + U[2]*v),
                                1.0f, 0.55f, 0.2f, al });
                        };
                        P(-sz,-sz); P(sz,-sz); P(sz,sz); P(-sz,-sz); P(sz,sz); P(-sz,sz);
                    }
                }
            }
            troll_slamming.swap(slamming_now);   // remember this frame's slammers for edge detection
        }

        // Enemy health bars: a subtle, semi-transparent red bar over the head of any enemy
        // hit in the last HEALTHBAR_TIME seconds (fading out over the final second). A faint
        // full-width track plus a brighter red fill proportional to remaining health.
        {
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            const float BARW = 0.9f, BARH = 0.10f;
            auto quad = [&](float lx, float rx, const vec3 mid, float y, float r, float g, float b, float a) {
                // lx..rx are offsets along camera-right from `mid`; y offsets along camera-up.
                auto P = [&](float u, float v) {
                    particle_verts.insert(particle_verts.end(), {
                        mid[0] + R[0]*u + U[0]*v, mid[1] + R[1]*u + U[1]*v, mid[2] + R[2]*u + U[2]*v, r, g, b, a });
                };
                P(lx, y - BARH); P(rx, y - BARH); P(rx, y + BARH);
                P(lx, y - BARH); P(rx, y + BARH); P(lx, y + BARH);
            };
            for (const auto& en : entities.items) {
                if (en.type != dc::entity::EntityType::Enemy || en.healthbar_time <= 0.0f) continue;
                const float frac = en.stats.max_health > 0.0f
                                 ? std::fmax(0.0f, std::fmin(1.0f, en.health / en.stats.max_health)) : 0.0f;
                const float headY = terrain.height(en.position[0], en.position[2])
                                  + (en.kind == dc::entity::EnemyKind::Flying ? dc::entity::FLY_HOVER : 0.0f) + 2.5f;
                vec3 mid = { en.position[0], headY, en.position[2] };
                const float fade = std::fmin(1.0f, en.healthbar_time);   // fade out over the last second
                const float L = -BARW, Rr = BARW;
                quad(L, Rr, mid, 0.0f, 0.5f, 0.05f, 0.05f, 0.12f * fade);                       // faint full track
                if (frac > 0.0f) quad(L, L + (Rr - L) * frac, mid, 0.0f, 1.0f, 0.12f, 0.10f, 0.5f * fade);  // red fill
            }
        }

        // Floating damage numbers: billboarded 7-seg digits over each hit enemy. World-
        // sized, so perspective shrinks them with distance and grows them up close (like
        // the chest price tags). They rise + fade over their life; crits are gold + bigger.
        if (!dmg_numbers.empty()) {
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            auto quad = [&](const vec3 base, float u0, float v0, float u1, float v1, float sc,
                            float r, float g, float b, float al) {
                auto P = [&](float u, float v) {
                    particle_verts.insert(particle_verts.end(), {
                        base[0] + (R[0]*u + U[0]*v) * sc, base[1] + (R[1]*u + U[1]*v) * sc,
                        base[2] + (R[2]*u + U[2]*v) * sc, r, g, b, al });
                };
                P(u0,v0); P(u1,v0); P(u1,v1);
                P(u0,v0); P(u1,v1); P(u0,v1);
            };
            for (const auto& fn : dmg_numbers) {
                const float t = fn.age / DMG_LIFE;
                const float al = (1.0f - t) * (1.0f - t);          // ease-out fade
                const float sc = fn.crit ? 0.36f : 0.26f;          // crits a touch larger
                const float r = 1.0f, g = fn.crit ? 0.82f : 1.0f, b = fn.crit ? 0.18f : 1.0f;  // gold crit / white
                char num[16]; std::snprintf(num, sizeof num, "%d", static_cast<int>(fn.amount + 0.5f));
                const int n = static_cast<int>(std::strlen(num));
                const float dw = 0.55f, dh = 0.9f, dtk = 0.14f, gap = dw + 0.28f;
                const float total = n * gap - 0.28f;
                vec3 base = { fn.pos[0],
                              terrain.height(fn.pos[0], fn.pos[2]) + fn.pos[1] + 1.8f + fn.age * DMG_RISE,
                              fn.pos[2] };
                for (int i = 0; i < n; ++i) {
                    float ox = -total * 0.5f + i * gap;
                    seven_seg(num[i] - '0', dw, dh, dtk, [&](float u0, float v0, float u1, float v1) {
                        quad(base, ox + u0, v0, ox + u1, v1, sc, r, g, b, al);
                    });
                }
            }
        }

        // Debug: draw the combat cones as flat fans on the floor in front of the
        // player (reuses the additive particle pass: 7 floats/vertex pos+rgba).
        // Red = sword/attack arc; blue = shield block arc.
        if (show_hitboxes) {
            auto draw_cone = [&](float cx, float cz, float center_yaw, float half, float radius,
                                 float r, float g, float b) {
                const int segs = 18;
                auto push = [&](float x, float z) {   // drape just above the terrain so hills don't bury it
                    particle_verts.insert(particle_verts.end(), { x, terrain.height(x, z) + 0.12f, z, r, g, b, 0.22f });
                };
                for (int s = 0; s < segs; ++s) {
                    float a0 = (center_yaw - half) + (2.0f * half) * (s)     / segs;
                    float a1 = (center_yaw - half) + (2.0f * half) * (s + 1) / segs;
                    push(cx, cz);                                                       // apex
                    push(cx + std::cos(a0) * radius, cz + std::sin(a0) * radius);
                    push(cx + std::cos(a1) * radius, cz + std::sin(a1) * radius);
                }
            };
            // The player's swing is a real 3D cone around the aim direction (so you can
            // see it tilt up at the flyer). Apex at the swing origin; ring at `radius`.
            auto draw_cone3d = [&](const vec3 dir, float half, float radius, float r, float g, float b) {
                vec3 apex = { player.position[0],
                              player.position[1] - dc::world::EYE_HEIGHT + dc::entity::STRIKE_ORIGIN_Y,
                              player.position[2] };
                vec3 up = { 0.0f, 1.0f, 0.0f }, u, v;
                glm_vec3_cross(const_cast<float*>(dir), up, u);
                if (glm_vec3_norm(u) < 1e-3f) { vec3 alt = {1,0,0}; glm_vec3_cross(const_cast<float*>(dir), alt, u); }
                glm_vec3_normalize(u);
                glm_vec3_cross(const_cast<float*>(dir), u, v); glm_vec3_normalize(v);
                const int segs = 20; const float cs = std::cos(half), sn = std::sin(half);
                auto ring = [&](float phi, vec3 out) {
                    for (int k = 0; k < 3; ++k)
                        out[k] = apex[k] + radius * (cs * dir[k] + sn * (std::cos(phi) * u[k] + std::sin(phi) * v[k]));
                };
                auto push = [&](const vec3 p) {
                    particle_verts.insert(particle_verts.end(), { p[0], p[1], p[2], r, g, b, 0.22f });
                };
                for (int s = 0; s < segs; ++s) {
                    vec3 p0, p1;
                    ring(6.2831853f * s / segs, p0);
                    ring(6.2831853f * (s + 1) / segs, p1);
                    push(apex); push(p0); push(p1);
                }
            };
            if (punching)                                                               // attack: red 3D cone, mid-swing
                draw_cone3d(pc.aim, std::acos(pc.strike_cos), pc.strike_reach, 0.9f, 0.15f, 0.15f);
            if (blocking)                                                               // block: blue arc on the floor
                draw_cone(player.position[0], player.position[2], player.yaw,
                          std::acos(pc.block_cos), pc.strike_reach * 0.9f, 0.2f, 0.5f, 1.0f);
            for (const auto& en : entities.items)                                       // enemy swings: orange
                if (en.type == dc::entity::EntityType::Enemy && en.attacking)
                    draw_cone(en.position[0], en.position[2], en.attack_yaw,
                              std::acos(dc::entity::ENEMY_ATTACK_CONE), dc::entity::ENEMY_ATTACK_REACH,
                              1.0f, 0.55f, 0.1f);
            for (const auto& sp : spawners)                                              // spawn zones: green disc
                draw_cone(sp.pos[0], sp.pos[2], 0.0f, 3.14159265f, sp.radius, 0.1f, 0.9f, 0.2f);
            if (player.weapon)                                                          // thrown hit area: red disc
                for (const auto& th : throwns)
                    draw_cone(th.pos[0], th.pos[2], 0.0f, 3.14159265f,
                              player.weapon->throw_radius * player.weapon->throw_size * player.sword_scale, 0.9f, 0.2f, 0.2f);
        }
        // Elemental sword sparks: spawn from each player's blade (by their element mask),
        // then simulate + draw as small billboards. Fire rises, ice sinks, earth scatters.
        {
            auto frand = [&]() { spark_rng = spark_rng * 1664525u + 1013904223u; return (spark_rng >> 8) * (1.0f / 16777216.0f); };
            auto emit = [&](const vec3 base, uint8_t mask) {
                if (mask == 0 || sparks.size() > 600) return;
                auto add = [&](float vx, float vy, float vz, float r, float g, float b, float life) {
                    Spark s;
                    s.pos[0] = base[0] + (frand() - 0.5f) * 0.12f; s.pos[1] = base[1] + (frand() - 0.5f) * 0.12f;
                    s.pos[2] = base[2] + (frand() - 0.5f) * 0.12f;
                    s.vel[0] = vx; s.vel[1] = vy; s.vel[2] = vz;
                    s.color[0] = r; s.color[1] = g; s.color[2] = b; s.age = 0.0f; s.life = life;
                    sparks.push_back(s);
                };
                // Spawn very rarely (roughly one per second per element).
                if ((mask & 1) && frand() < 0.04f) add((frand()-0.5f)*0.4f, 1.2f+frand()*0.8f, (frand()-0.5f)*0.4f, 1.0f, 0.5f, 0.12f, 0.5f);  // fire: rise
                if ((mask & 2) && frand() < 0.035f) add((frand()-0.5f)*0.5f, -0.5f-frand()*0.4f, (frand()-0.5f)*0.5f, 0.45f, 0.8f, 1.0f, 0.7f);  // ice: sink
                if ((mask & 4) && frand() < 0.03f) { float a = frand()*6.2831853f; add(std::cos(a)*0.8f, 0.2f, std::sin(a)*0.8f, 0.6f, 0.45f, 0.22f, 0.55f); }  // earth: scatter
            };
            if (!dead && blade_ok) {  // off the local player's hand bone, nudged forward into view
                vec3 f; player.front(f);
                vec3 base = { blade_pos[0] + f[0] * 0.45f, blade_pos[1] + 0.1f, blade_pos[2] + f[2] * 0.45f };
                emit(base, elem_mask(player.fire_dps, player.ice_slow, player.earth_knock));
            }
            for (const auto& rp : remotes) {
                if (rp.ghost || rp.elements == 0) continue;
                const float c = std::cos(rp.yaw), s2 = std::sin(rp.yaw);
                vec3 base = { rp.pos[0] + c*0.5f, rp.pos[1] - 0.3f, rp.pos[2] + s2*0.5f };
                emit(base, rp.elements);
            }
            // Burning / slowed enemies emit fire / ice off their bodies.
            for (const auto& en : entities.items) {
                if (en.type != dc::entity::EntityType::Enemy) continue;
                const uint8_t em = (en.burn_time > 0.0f ? 1 : 0) | (en.slow_time > 0.0f ? 2 : 0);
                if (!em) continue;
                const float ey = terrain.height(en.position[0], en.position[2])
                               + (en.kind == dc::entity::EnemyKind::Flying ? dc::entity::FLY_HOVER : 0.0f) + 0.9f;
                vec3 base = { en.position[0], ey, en.position[2] };
                emit(base, em);
            }
            // Flamethrower spray: a firing Pyro belches a cone of flame motes forward along
            // its facing — bright orange/yellow, rising as they fly + fade (hot -> smoke).
            auto add_flame = [&](float x, float y, float z, float vx, float vy, float vz, float life, float warm) {
                if (sparks.size() > 1800) return;
                Spark s;
                s.pos[0] = x; s.pos[1] = y; s.pos[2] = z;
                s.vel[0] = vx; s.vel[1] = vy; s.vel[2] = vz;
                s.color[0] = 1.0f; s.color[1] = 0.45f + 0.35f * warm; s.color[2] = 0.12f * warm;
                s.age = 0.0f; s.life = life; s.grav = -2.2f;   // negative grav -> rises
                s.size_mul = 2.0f; s.alpha_mul = 2.2f;
                sparks.push_back(s);
            };
            for (const auto& en : entities.items) {
                if (en.type != dc::entity::EntityType::Enemy || en.kind != dc::entity::EnemyKind::Flamethrower || !en.attacking) continue;
                const float fx = std::cos(en.attack_yaw), fz = std::sin(en.attack_yaw);
                const float my = terrain.height(en.position[0], en.position[2]) + 1.2f;
                const float mx = en.position[0] + fx * 0.6f, mz = en.position[2] + fz * 0.6f;
                for (int k = 0; k < 6; ++k) {                       // a steady gout each frame
                    const float spread = (frand() - 0.5f) * 0.5f;   // fan out sideways
                    const float rx = -fz, rz = fx;                  // perpendicular (spray width)
                    const float reach = 3.0f + frand() * 5.0f;      // varied travel speed -> cone depth
                    add_flame(mx + rx * spread, my + (frand() - 0.5f) * 0.3f, mz + rz * spread,
                              fx * reach + rx * spread * 2.0f, 0.6f + frand() * 1.0f, fz * reach + rz * spread * 2.0f,
                              0.35f + frand() * 0.3f, frand());
                }
            }
            // Players who are on fire smolder with flame motes (local player + burning peers).
            auto emit_burn = [&](float x, float y, float z) {
                for (int k = 0; k < 3; ++k)
                    add_flame(x + (frand() - 0.5f) * 0.5f, y + frand() * 1.2f, z + (frand() - 0.5f) * 0.5f,
                              (frand() - 0.5f) * 0.6f, 1.2f + frand() * 1.2f, (frand() - 0.5f) * 0.6f,
                              0.3f + frand() * 0.25f, frand());
            };
            if (!dead && player.burn_time > 0.0f)
                emit_burn(player.position[0], player.position[1] - dc::world::EYE_HEIGHT, player.position[2]);
            for (const auto& rp : remotes)
                if (!rp.ghost && rp.burning) emit_burn(rp.pos[0], rp.pos[1] - dc::world::EYE_HEIGHT, rp.pos[2]);
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            for (std::size_t i = 0; i < sparks.size();) {
                sparks[i].age += dt;
                if (sparks[i].age >= sparks[i].life) { sparks[i] = sparks.back(); sparks.pop_back(); continue; }
                sparks[i].vel[1] -= sparks[i].grav * dt;   // debris falls; elemental sparks have grav 0
                sparks[i].pos[0] += sparks[i].vel[0]*dt; sparks[i].pos[1] += sparks[i].vel[1]*dt; sparks[i].pos[2] += sparks[i].vel[2]*dt;
                const float fade = 1.0f - sparks[i].age / sparks[i].life;
                const float al = fade * 0.28f * sparks[i].alpha_mul;   // dim (additive) but visible
                const float sz = (0.04f * fade + 0.012f) * sparks[i].size_mul;
                const vec3& p = sparks[i].pos; const vec3& cc = sparks[i].color;
                auto P = [&](float u, float v) {
                    particle_verts.insert(particle_verts.end(), {
                        p[0] + (R[0]*u + U[0]*v), p[1] + (R[1]*u + U[1]*v), p[2] + (R[2]*u + U[2]*v), cc[0], cc[1], cc[2], al });
                };
                P(-sz,-sz); P(sz,-sz); P(sz,sz); P(-sz,-sz); P(sz,sz); P(-sz,sz);
                ++i;
            }
        }

        // Gunner minions: each drone EASES toward a loose slot near its owner (lags when
        // you move, so it reads as reacting), and fires small red lasers at the nearest
        // enemy in range. Simulated per-peer for visuals; the host does the real damage.
        {
            struct Owner { uint32_t id; float x, y, z; int count; float range; };
            std::vector<Owner> owners;
            if (!dead) owners.push_back({ my_id, player.position[0], player.position[1], player.position[2], player.minion_count, player.minion_range });
            for (const auto& rp : remotes) if (!rp.ghost && rp.minions > 0)
                owners.push_back({ rp.id, rp.pos[0], rp.pos[1], rp.pos[2], rp.minions, rp.minion_range });
            for (std::size_t i = 0; i < swarms.size();) {   // drop swarms whose owner is gone
                bool present = false; for (auto& o : owners) if (o.id == swarms[i].id) { present = true; break; }
                if (!present) { swarms[i] = swarms.back(); swarms.pop_back(); } else ++i;
            }
            const auto& R = renderer.cam_right; const auto& U = renderer.cam_up;
            const float fx = R[1]*U[2]-R[2]*U[1], fy = R[2]*U[0]-R[0]*U[2], fz = R[0]*U[1]-R[1]*U[0];  // cam forward
            // A thin camera-facing red laser ribbon from A to B.
            auto beam = [&](float ax,float ay,float az, float bx,float by,float bz, float wid, float al) {
                float lx=bx-ax, ly=by-ay, lz=bz-az; const float ll=std::sqrt(lx*lx+ly*ly+lz*lz);
                if (ll < 1e-3f) return; lx/=ll; ly/=ll; lz/=ll;
                float wx=ly*fz-lz*fy, wy=lz*fx-lx*fz, wz=lx*fy-ly*fx; const float wl=std::sqrt(wx*wx+wy*wy+wz*wz);
                if (wl>1e-4f){wx/=wl;wy/=wl;wz/=wl;} else {wx=R[0];wy=R[1];wz=R[2];}
                auto V=[&](float ex,float ey,float ez,float sg){ particle_verts.insert(particle_verts.end(),
                    { ex+wx*wid*sg, ey+wy*wid*sg, ez+wz*wid*sg, 1.0f, 0.12f, 0.06f, al }); };
                V(ax,ay,az,-1); V(bx,by,bz,-1); V(bx,by,bz,1);  V(ax,ay,az,-1); V(bx,by,bz,1); V(ax,ay,az,1);
            };
            const float ease = 1.0f - std::exp(-MINION_FOLLOW_K * dt);   // exponential lag toward the slot
            for (auto& o : owners) {
                if (o.count <= 0) continue;
                DroneSwarm* sw = nullptr;
                for (auto& s : swarms) if (s.id == o.id) { sw = &s; break; }
                if (!sw) { swarms.push_back({ o.id, {}, {} }); sw = &swarms.back(); }
                for (int i = 0; i < o.count && i < 4; ++i) {
                    const float ang = 6.2831853f * i / 4 + 0.7f;   // fixed loose formation slot
                    const float tx = o.x + std::cos(ang) * MINION_FOLLOW_RADIUS;
                    const float ty = o.y - 0.4f;
                    const float tz = o.z + std::sin(ang) * MINION_FOLLOW_RADIUS;
                    if (!sw->spawned[i]) { sw->pos[i][0] = tx; sw->pos[i][1] = ty; sw->pos[i][2] = tz; sw->spawned[i] = true; }
                    sw->pos[i][0] += (tx - sw->pos[i][0]) * ease;   // ease in (laggy)
                    sw->pos[i][1] += (ty - sw->pos[i][1]) * ease;
                    sw->pos[i][2] += (tz - sw->pos[i][2]) * ease;
                    const float mx = sw->pos[i][0], my = sw->pos[i][1] + std::sin(run_time*3.0f + i) * 0.06f, mz = sw->pos[i][2];
                    if (drone_loaded) {   // flying-robot quadcopter with spinning rotors
                        std::vector<dc::renderer::AnimLayer> dl;
                        dl.push_back({ &drone_data.walk, run_time * 4.0f + i, -1 });   // spin the props
                        dc::renderer::pose_model(drone_data, dl, 0.0f, enemy_part_world);
                        mat4 dpl; glm_mat4_identity(dpl);
                        vec3 dpos = { mx, my, mz }; glm_translate(dpl, dpos);
                        glm_rotate_y(dpl, run_time * 0.6f + i, dpl);            // slow idle yaw
                        vec3 dsc = { 0.7f, 0.7f, 0.7f }; glm_scale(dpl, dsc);   // small
                        vec3 white = { 1.0f, 1.0f, 1.0f };
                        renderer.draw_model(drone_model, enemy_part_world, dpl, white);
                    } else {
                        const float s = 0.17f; const int N = 12;
                        for (int k = 0; k < N; ++k) {
                            const float a0 = 6.2831853f * k / N, a1 = 6.2831853f * (k + 1) / N;
                            auto P = [&](float u, float v) {
                                particle_verts.insert(particle_verts.end(), {
                                    mx + (R[0]*u + U[0]*v), my + (R[1]*u + U[1]*v), mz + (R[2]*u + U[2]*v), 0.55f, 0.7f, 0.95f, 0.9f });
                            };
                            P(0, 0); P(s*std::cos(a0), s*std::sin(a0)); P(s*std::cos(a1), s*std::sin(a1));
                        }
                    }
                    // Fire a red laser at the nearest enemy in range, in brief bursts.
                    const bool firing = std::fmod(run_time * 1.6f + i * 0.31f, MINION_FIRE_INTERVAL) < 0.12f;
                    if (firing) {
                        float bd2 = o.range * o.range; const dc::entity::Entity* tgt = nullptr;
                        for (const auto& en : entities.items) {
                            if (en.type != dc::entity::EntityType::Enemy || !en.alive) continue;
                            const float ex = en.position[0]-mx, ez = en.position[2]-mz, e2 = ex*ex + ez*ez;
                            if (e2 < bd2) { bd2 = e2; tgt = &en; }
                        }
                        if (tgt) {
                            const float ey = terrain.height(tgt->position[0], tgt->position[2])
                                           + (tgt->kind == dc::entity::EnemyKind::Flying ? dc::entity::FLY_HOVER : 1.0f);
                            beam(mx, my, mz, tgt->position[0], ey, tgt->position[2], 0.045f, 0.95f);
                        }
                    }
                }
            }
        }

        renderer.draw_particles(particle_verts);

        // Debug readout in the title bar (cheap stand-in for on-screen text).
        if (show_hitboxes) {
            char buf[160];
            const char* atk = punching ? "SWING" : "-";
            const char* blk = block_ready ? "BLOCK" : (blocking ? "raising" : "-");
            std::snprintf(buf, sizeof buf,
                          "dungeoncrawl  [DBG]  hp:%.0f  stam:%.0f  coins:%d  enemies:%zu  atk:%s  shield:%s",
                          player.health, player.stamina, currency, entities.items.size(), atk, blk);
            window.set_title(buf);
        }

        // HUD: green stamina bar, bottom-left (NDC rects via the reused particle shader).
        {
            std::vector<float> hud;
            auto hud_rect = [&](float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
                hud.insert(hud.end(), {
                    x0,y0,0.0f, r,g,b,a,  x1,y0,0.0f, r,g,b,a,  x1,y1,0.0f, r,g,b,a,
                    x0,y0,0.0f, r,g,b,a,  x1,y1,0.0f, r,g,b,a,  x0,y1,0.0f, r,g,b,a });
            };
            auto clamp01 = [](float f) { return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f); };
            // One HUD triangle (pos3 + rgba, like hud_rect). Lets us draw placeholder
            // item shapes beyond axis-aligned rects.
            auto hud_tri = [&](float ax, float ay, float bx, float by, float cx, float cy,
                               float r, float g, float b, float a) {
                hud.insert(hud.end(), { ax,ay,0.0f, r,g,b,a,  bx,by,0.0f, r,g,b,a,  cx,cy,0.0f, r,g,b,a });
            };
            // Framebuffer size + aspect, for pixel-sized text and aspect-correct icons
            // (x extents divided by aspect so squares look square and circles round).
            int fbw, fbh; window.framebuffer_size(fbw, fbh);
            const float aspect = (fbw > 0 && fbh > 0) ? static_cast<float>(fbw) / fbh : 1.0f;
            // A placeholder item icon: shape `sh` centered at (cx,cy) with NDC-y half-size s.
            auto icon_shape = [&](float cx, float cy, float s, IconShape sh,
                                  float r, float g, float b, float a) {
                const float sx = s / aspect;
                switch (sh) {
                    case IconShape::Square:
                        hud_rect(cx - sx, cy - s, cx + sx, cy + s, r, g, b, a);
                        break;
                    case IconShape::Triangle:
                        hud_tri(cx, cy + s, cx - sx, cy - s, cx + sx, cy - s, r, g, b, a);
                        break;
                    case IconShape::Diamond:
                        hud_tri(cx, cy + s, cx + sx, cy, cx, cy - s, r, g, b, a);
                        hud_tri(cx, cy + s, cx, cy - s, cx - sx, cy, r, g, b, a);
                        break;
                    case IconShape::Circle: {
                        const int N = 16;
                        for (int k = 0; k < N; ++k) {
                            const float a0 = 6.2831853f * k / N, a1 = 6.2831853f * (k + 1) / N;
                            hud_tri(cx, cy, cx + sx * std::cos(a0), cy + s * std::sin(a0),
                                    cx + sx * std::cos(a1), cy + s * std::sin(a1), r, g, b, a);
                        }
                        break;
                    }
                    case IconShape::Cross: {
                        const float t = s * 0.38f;            // arm half-thickness
                        hud_rect(cx - t / aspect, cy - s, cx + t / aspect, cy + s, r, g, b, a);  // vertical bar
                        hud_rect(cx - sx, cy - t, cx + sx, cy + t, r, g, b, a);                  // horizontal bar
                        break;
                    }
                    case IconShape::Arrow:   // right-pointing triangle (motion)
                        hud_tri(cx + sx, cy, cx - sx, cy + s, cx - sx, cy - s, r, g, b, a);
                        break;
                    case IconShape::Hourglass:   // bowtie: two triangles meeting at the center
                        hud_tri(cx - sx, cy + s, cx + sx, cy + s, cx, cy, r, g, b, a);
                        hud_tri(cx - sx, cy - s, cx + sx, cy - s, cx, cy, r, g, b, a);
                        break;
                    case IconShape::Star: {      // 4-point sparkle: a thin vertical + thin horizontal diamond
                        const float th = 0.34f;
                        hud_tri(cx, cy + s, cx + sx * th, cy, cx, cy - s, r, g, b, a);
                        hud_tri(cx, cy + s, cx, cy - s, cx - sx * th, cy, r, g, b, a);
                        hud_tri(cx - sx, cy, cx, cy + s * th, cx + sx, cy, r, g, b, a);
                        hud_tri(cx - sx, cy, cx + sx, cy, cx, cy - s * th, r, g, b, a);
                        break;
                    }
                    case IconShape::Pentagon: {  // fan with a point straight up
                        const int N = 5;
                        for (int k = 0; k < N; ++k) {
                            const float a0 = 1.5707963f + 6.2831853f * k / N, a1 = 1.5707963f + 6.2831853f * (k + 1) / N;
                            hud_tri(cx, cy, cx + sx * std::cos(a0), cy + s * std::sin(a0),
                                    cx + sx * std::cos(a1), cy + s * std::sin(a1), r, g, b, a);
                        }
                        break;
                    }
                    case IconShape::Hexagon: {   // flat-top hexagon fan
                        const int N = 6;
                        for (int k = 0; k < N; ++k) {
                            const float a0 = 6.2831853f * k / N, a1 = 6.2831853f * (k + 1) / N;
                            hud_tri(cx, cy, cx + sx * std::cos(a0), cy + s * std::sin(a0),
                                    cx + sx * std::cos(a1), cy + s * std::sin(a1), r, g, b, a);
                        }
                        break;
                    }
                    case IconShape::Flame:    // tall narrow triangle (apex up)
                        hud_tri(cx, cy + s, cx - sx * 0.7f, cy - s, cx + sx * 0.7f, cy - s, r, g, b, a);
                        break;
                    case IconShape::IceShard:  // apex-down triangle
                        hud_tri(cx, cy - s, cx - sx, cy + s, cx + sx, cy + s, r, g, b, a);
                        break;
                    case IconShape::Brick:     // wide short rectangle
                        hud_rect(cx - sx, cy - s * 0.55f, cx + sx, cy + s * 0.55f, r, g, b, a);
                        break;
                    case IconShape::Pip: {     // small filled circle (minion)
                        const int N = 12;
                        for (int k = 0; k < N; ++k) {
                            const float a0 = 6.2831853f * k / N, a1 = 6.2831853f * (k + 1) / N;
                            hud_tri(cx, cy, cx + sx * 0.7f * std::cos(a0), cy + s * 0.7f * std::sin(a0),
                                    cx + sx * 0.7f * std::cos(a1), cy + s * 0.7f * std::sin(a1), r, g, b, a);
                        }
                        break;
                    }
                    case IconShape::Ammo:      // tall narrow rectangle (bullet)
                        hud_rect(cx - sx * 0.38f, cy - s, cx + sx * 0.38f, cy + s, r, g, b, a);
                        break;
                    case IconShape::Streak:    // diagonal comet streak (two triangles)
                        hud_tri(cx - sx, cy - s, cx - sx * 0.4f, cy - s, cx + sx, cy + s, r, g, b, a);
                        hud_tri(cx - sx, cy - s, cx + sx, cy + s, cx + sx * 0.4f, cy + s, r, g, b, a);
                        break;
                    case IconShape::Boom: {    // starburst: spikes radiating out
                        const int N = 8;
                        for (int k = 0; k < N; ++k) {
                            const float a0 = 6.2831853f * k / N, a1 = 6.2831853f * (k + 0.5f) / N;
                            hud_tri(cx, cy, cx + sx * std::cos(a0), cy + s * std::sin(a0),
                                    cx + sx * 0.45f * std::cos(a1), cy + s * 0.45f * std::sin(a1), r, g, b, a);
                            const float a2 = 6.2831853f * (k + 1) / N;
                            hud_tri(cx, cy, cx + sx * 0.45f * std::cos(a1), cy + s * 0.45f * std::sin(a1),
                                    cx + sx * std::cos(a2), cy + s * std::sin(a2), r, g, b, a);
                        }
                        break;
                    }
                    case IconShape::Ring: {    // annulus ring (orbit autocast)
                        const int N = 20; const float ir = 0.55f;   // inner radius fraction
                        for (int k = 0; k < N; ++k) {
                            const float a0 = 6.2831853f * k / N, a1 = 6.2831853f * (k + 1) / N;
                            const float c0 = std::cos(a0), s0 = std::sin(a0), c1 = std::cos(a1), s1 = std::sin(a1);
                            hud_tri(cx + sx * c0, cy + s * s0, cx + sx * c1, cy + s * s1, cx + sx * ir * c1, cy + s * ir * s1, r, g, b, a);
                            hud_tri(cx + sx * c0, cy + s * s0, cx + sx * ir * c1, cy + s * ir * s1, cx + sx * ir * c0, cy + s * ir * s0, r, g, b, a);
                        }
                        break;
                    }
                    case IconShape::Burst: {   // thin ring + center dot (nova blast)
                        const int N = 20; const float ir = 0.72f;
                        for (int k = 0; k < N; ++k) {
                            const float a0 = 6.2831853f * k / N, a1 = 6.2831853f * (k + 1) / N;
                            const float c0 = std::cos(a0), s0 = std::sin(a0), c1 = std::cos(a1), s1 = std::sin(a1);
                            hud_tri(cx + sx * c0, cy + s * s0, cx + sx * c1, cy + s * s1, cx + sx * ir * c1, cy + s * ir * s1, r, g, b, a);
                            hud_tri(cx + sx * c0, cy + s * s0, cx + sx * ir * c1, cy + s * ir * s1, cx + sx * ir * c0, cy + s * ir * s0, r, g, b, a);
                            hud_tri(cx, cy, cx + sx * 0.34f * c0, cy + s * 0.34f * s0, cx + sx * 0.34f * c1, cy + s * 0.34f * s1, r, g, b, a);
                        }
                        break;
                    }
                    case IconShape::Slot: {    // square frame (a spell slot)
                        const float ty = s * 0.26f, tx = ty / aspect;
                        hud_rect(cx - sx, cy + s - ty, cx + sx, cy + s, r, g, b, a);   // top
                        hud_rect(cx - sx, cy - s, cx + sx, cy - s + ty, r, g, b, a);   // bottom
                        hud_rect(cx - sx, cy - s, cx - sx + tx, cy + s, r, g, b, a);   // left
                        hud_rect(cx + sx - tx, cy - s, cx + sx, cy + s, r, g, b, a);   // right
                        break;
                    }
                    case IconShape::Sword: {   // blade + crossguard + grip + pommel (melee damage)
                        hud_rect(cx - sx*0.13f, cy - s*0.45f, cx + sx*0.13f, cy + s*0.78f, r, g, b, a);  // blade
                        hud_tri(cx, cy + s, cx - sx*0.13f, cy + s*0.78f, cx + sx*0.13f, cy + s*0.78f, r, g, b, a);  // tip
                        hud_rect(cx - sx*0.62f, cy - s*0.45f, cx + sx*0.62f, cy - s*0.30f, r, g, b, a);  // crossguard
                        hud_rect(cx - sx*0.12f, cy - s*0.78f, cx + sx*0.12f, cy - s*0.45f, r, g, b, a);  // grip
                        hud_rect(cx - sx*0.20f, cy - s, cx + sx*0.20f, cy - s*0.78f, r, g, b, a);        // pommel
                        break;
                    }
                    case IconShape::Heart: {   // two lobes + a V point (regen)
                        hud_rect(cx - sx*0.95f, cy + s*0.05f, cx - sx*0.05f, cy + s, r, g, b, a);  // left lobe
                        hud_rect(cx + sx*0.05f, cy + s*0.05f, cx + sx*0.95f, cy + s, r, g, b, a);  // right lobe
                        hud_tri(cx - sx*0.95f, cy + s*0.10f, cx + sx*0.95f, cy + s*0.10f, cx, cy - s, r, g, b, a);  // point
                        break;
                    }
                    case IconShape::Clock: {   // ring + two hands (cooldown)
                        const int N = 18; const float ir = 0.66f;
                        for (int k = 0; k < N; ++k) {
                            const float a0 = 6.2831853f*k/N, a1 = 6.2831853f*(k+1)/N;
                            const float c0=std::cos(a0),s0=std::sin(a0),c1=std::cos(a1),s1=std::sin(a1);
                            hud_tri(cx+sx*c0,cy+s*s0, cx+sx*c1,cy+s*s1, cx+sx*ir*c1,cy+s*ir*s1, r, g, b, a);
                            hud_tri(cx+sx*c0,cy+s*s0, cx+sx*ir*c1,cy+s*ir*s1, cx+sx*ir*c0,cy+s*ir*s0, r, g, b, a);
                        }
                        hud_rect(cx - sx*0.07f, cy, cx + sx*0.07f, cy + s*0.55f, r, g, b, a);  // minute hand
                        hud_rect(cx, cy - s*0.07f, cx + sx*0.42f, cy + s*0.07f, r, g, b, a);   // hour hand
                        break;
                    }
                    case IconShape::Hammer: {  // head + handle (knockback)
                        hud_rect(cx - sx*0.70f, cy + s*0.45f, cx + sx*0.70f, cy + s, r, g, b, a);   // head
                        hud_rect(cx - sx*0.14f, cy - s, cx + sx*0.14f, cy + s*0.45f, r, g, b, a);   // handle
                        break;
                    }
                    case IconShape::Boot: {    // L-shape (stamina / footwork)
                        hud_rect(cx - sx*0.35f, cy - s*0.20f, cx + sx*0.15f, cy + s, r, g, b, a);   // leg
                        hud_rect(cx - sx*0.35f, cy - s, cx + sx*0.85f, cy - s*0.20f, r, g, b, a);   // foot
                        break;
                    }
                    case IconShape::Shield: {  // box body + point bottom (i-frames)
                        hud_rect(cx - sx, cy - s*0.15f, cx + sx, cy + s, r, g, b, a);
                        hud_tri(cx - sx, cy - s*0.15f, cx + sx, cy - s*0.15f, cx, cy - s, r, g, b, a);
                        break;
                    }
                    case IconShape::Crescent: {  // top arc (wider swing)
                        const int N = 12; const float ir = 0.60f, lo = 0.35f, hi = 2.79f;  // ~20..160 deg
                        for (int k = 0; k < N; ++k) {
                            const float a0 = lo+(hi-lo)*k/N, a1 = lo+(hi-lo)*(k+1)/N;
                            const float c0=std::cos(a0),s0=std::sin(a0),c1=std::cos(a1),s1=std::sin(a1);
                            hud_tri(cx+sx*c0,cy+s*s0, cx+sx*c1,cy+s*s1, cx+sx*ir*c1,cy+s*ir*s1, r, g, b, a);
                            hud_tri(cx+sx*c0,cy+s*s0, cx+sx*ir*c1,cy+s*ir*s1, cx+sx*ir*c0,cy+s*ir*s0, r, g, b, a);
                        }
                        break;
                    }
                    case IconShape::Drone: {   // body + rotor bar + legs (gunner)
                        hud_rect(cx - sx*0.28f, cy - s*0.30f, cx + sx*0.28f, cy + s*0.28f, r, g, b, a);  // body
                        hud_rect(cx - sx, cy + s*0.50f, cx + sx, cy + s*0.66f, r, g, b, a);              // rotor bar
                        hud_rect(cx - sx, cy + s*0.66f, cx - sx*0.60f, cy + s*0.92f, r, g, b, a);        // left rotor
                        hud_rect(cx + sx*0.60f, cy + s*0.66f, cx + sx, cy + s*0.92f, r, g, b, a);        // right rotor
                        hud_tri(cx - sx*0.28f, cy - s*0.30f, cx - sx*0.55f, cy - s, cx, cy - s*0.30f, r, g, b, a);  // leg L
                        hud_tri(cx + sx*0.28f, cy - s*0.30f, cx + sx*0.55f, cy - s, cx, cy - s*0.30f, r, g, b, a);  // leg R
                        break;
                    }
                    case IconShape::Fang: {    // two downward fangs (crit damage)
                        hud_tri(cx - sx, cy + s, cx - sx*0.10f, cy + s, cx - sx*0.55f, cy - s, r, g, b, a);
                        hud_tri(cx + sx*0.10f, cy + s, cx + sx, cy + s, cx + sx*0.55f, cy - s, r, g, b, a);
                        break;
                    }
                }
            };
            // Cursor in NDC, but only while a menu is open (otherwise the mouse drives the
            // first-person look and is captured). Used for hover-to-describe.
            const bool cursor_free = (menu_chest >= 0 || paused || scoreboard || levelup_open || upgrade_menu);
            float mxn = -2.0f, myn = -2.0f;
            if (cursor_free) {
                float mx, my; input.mouse_pos(mx, my);
                int ww, wh; window.window_size(ww, wh);
                mxn = (ww > 0) ? (mx / ww) * 2.0f - 1.0f : 0.0f;
                myn = (wh > 0) ? 1.0f - (my / wh) * 2.0f : 0.0f;
            }
            // Hovered-item tooltip, resolved during the icon/card build below and drawn
            // (panel into `hud`, text after) just before/after draw_hud.
            const UpgradeDef* tip = nullptr;
            float tip_x = 0.0f, tip_y = 0.0f;        // desired top-left anchor (NDC)

            // Scoreboard data (hold Tab): every player's damage total, sorted descending.
            // Insertion sort — player counts are tiny, avoids pulling in <algorithm>.
            struct ScoreRow { uint32_t id; double dmg; bool local; };
            std::vector<ScoreRow> scores;
            if (scoreboard) {
                scores.push_back({ my_id, (net.role == dc::net::Role::Client) ? (double)my_damage : host_damage, true });
                for (const auto& rp : remotes) scores.push_back({ rp.id, (double)rp.damage_dealt, false });
                for (std::size_t a = 1; a < scores.size(); ++a) {
                    ScoreRow key = scores[a]; std::size_t b = a;
                    while (b > 0 && scores[b - 1].dmg < key.dmg) { scores[b] = scores[b - 1]; --b; }
                    scores[b] = key;
                }
            }
            // Format a damage value as 12 / 3.4K / 1.2M.
            auto fmt_dmg = [](double v, char* out, std::size_t n) {
                if (v >= 1e6)      std::snprintf(out, n, "%.1fM", v / 1e6);
                else if (v >= 1e3) std::snprintf(out, n, "%.1fK", v / 1e3);
                else               std::snprintf(out, n, "%.0f", v);
            };
            // Scoreboard panel + leaderboard-row layout (shared by the rects here and the
            // text pass after draw_hud).
            const float sb_x0 = -0.55f, sb_x1 = 0.55f, sb_top = 0.72f, sb_bot = -0.62f;
            const float sb_row_h = 0.085f, sb_rows_top = 0.50f;
            const float sb_item_s = 0.05f, sb_item_chip = 0.064f, sb_item_step = 0.135f;
            const float sb_items_y = sb_rows_top - scores.size() * sb_row_h - 0.12f;
            // Shared item-icon row layout (top-left, under the coin counter): reused by
            // the hud-build loop and the stack-count text loop after draw_hud.
            const float inv_s = 0.034f, inv_chip = 0.047f, inv_step = 0.094f;
            const float inv_y = 0.72f, inv_x0 = -0.92f;
            const float x0 = -0.96f, x1 = -0.56f;
            // Health bar (red), just above the stamina bar.
            const float hy0 = -0.88f, hy1 = -0.84f;
            hud_rect(x0, hy0, x1, hy1, 0.05f, 0.05f, 0.05f, 0.6f);
            float hf = clamp01(player.stats.max_health > 0.0f ? player.health / player.stats.max_health : 0.0f);
            // linear interp from dark gray to bright red as health goes from 0% to 100%
            hud_rect(x0, hy0, x0 + (x1 - x0) * hf, hy1, 0.9f, 0.2f, 0.2f, 0.95f);
            // Stamina bar (green).
            const float y0 = -0.93f, y1 = -0.89f;
            hud_rect(x0, y0, x1, y1, 0.05f, 0.05f, 0.05f, 0.6f);
            float sf = clamp01(player.stamina_max > 0.0f ? player.stamina / player.stamina_max : 0.0f);
            hud_rect(x0, y0, x0 + (x1 - x0) * sf, y1, 0.2f, 0.9f, 0.3f, 0.95f);
            // XP bar (blue), under the stamina bar. Fills toward the next level.
            const float xpy0 = -0.975f, xpy1 = -0.945f;
            hud_rect(x0, xpy0, x1, xpy1, 0.04f, 0.05f, 0.09f, 0.6f);
            float xpf = clamp01(player.xp_to_next > 0.0f ? player.xp / player.xp_to_next : 0.0f);
            hud_rect(x0, xpy0, x0 + (x1 - x0) * xpf, xpy1, 0.30f, 0.62f, 1.0f, 0.95f);
            // Coin counter (top-left): a gold coin icon + the currency as 7-segment
            // digits built from rects — no font/text renderer needed.
            auto draw_digit = [&](float bx, float by, float w, float h, float t, int d) {
                seven_seg(d, w, h, t, [&](float u0, float v0, float u1, float v1) {
                    hud_rect(bx + u0, by + v0, bx + u1, by + v1, 1.0f, 0.85f, 0.2f, 1.0f);
                });
            };
            {
                const float dw = 0.035f, dh = 0.07f, dt = 0.011f, gap = dw + dt * 2.0f;
                const float by = 0.86f;
                hud_rect(-0.95f, by, -0.91f, by + dh, 1.0f, 0.85f, 0.2f, 1.0f);   // coin icon
                char num[16]; std::snprintf(num, sizeof num, "%d", currency);
                float dx = -0.88f;
                for (char* p = num; *p; ++p) { draw_digit(dx, by, dw, dh, dt, *p - '0'); dx += gap; }
            }
            // Level readout (blue, by the XP bar): the player's current level in 7-seg digits.
            {
                const float dw = 0.018f, dh = 0.036f, dt = 0.006f, gap = dw + dt * 2.0f;
                const float ly = -0.972f;
                char num[16]; std::snprintf(num, sizeof num, "%d", player.level);
                float dx = x1 + 0.018f;
                for (char* p = num; *p; ++p) {
                    seven_seg(*p - '0', dw, dh, dt, [&](float u0, float v0, float u1, float v1) {
                        hud_rect(dx + u0, ly + v0, dx + u1, ly + v1, 0.45f, 0.72f, 1.0f, 1.0f);
                    });
                    dx += gap;
                }
            }

            // Compass (top center): a horizontal strip showing the bearing to N/E/S/W, the
            // base, chests, and other players so you can't get lost. Each marker maps its
            // world bearing (relative to where you're facing) to an x within a ~±106° arc;
            // markers behind that arc are culled. Cardinal letters are drawn in the text pass.
            float compass_x[4] = {0,0,0,0}; bool compass_vis[4] = {false,false,false,false};
            const float COMPASS_LY = 0.965f;   // y for the cardinal letters (text pass)
            {
                const float VIS = 1.85f;             // visible arc half-width (radians)
                const float CXc = 0.0f, CW = 0.55f;  // strip center x + half-width (NDC)
                const float CYb = 0.905f, CYt = 0.99f, my = 0.93f;
                const float fwx = std::cos(player.yaw), fwz = std::sin(player.yaw);
                auto bearing_x = [&](float dx, float dz, float& sx) {
                    float len = std::sqrt(dx * dx + dz * dz);
                    if (len < 1e-4f) return false;
                    dx /= len; dz /= len;
                    const float dot = fwx * dx + fwz * dz, cross = fwx * dz - fwz * dx;
                    const float rel = std::atan2(cross, dot);
                    if (std::fabs(rel) > VIS) return false;
                    sx = CXc + (rel / VIS) * CW;
                    return true;
                };
                hud_rect(CXc - CW - 0.012f, CYb, CXc + CW + 0.012f, CYt, 0.05f, 0.06f, 0.09f, 0.45f);   // backdrop
                hud_rect(CXc - 0.004f, CYb, CXc + 0.004f, CYt, 0.9f, 0.9f, 0.95f, 0.85f);               // heading tick
                float sx;
                if (bearing_x(core_pos[0] - player.position[0], core_pos[2] - player.position[2], sx))
                    icon_shape(sx, my, 0.024f, IconShape::Burst, 0.25f, 0.8f, 1.0f, 1.0f);              // base
                for (const auto& ch : chests) {
                    if (ch.remaining() == 0) continue;                                                  // depleted -> skip
                    const float wx = (ch.col + 0.5f) * dc::world::TILE, wz = (ch.row + 0.5f) * dc::world::TILE;
                    if (bearing_x(wx - player.position[0], wz - player.position[2], sx))
                        icon_shape(sx, my, 0.02f, IconShape::Diamond, 1.0f, 0.82f, 0.2f, 1.0f);         // chest
                }
                for (const auto& rp : remotes)
                    if (bearing_x(rp.pos[0] - player.position[0], rp.pos[2] - player.position[2], sx))
                        icon_shape(sx, my, 0.02f, IconShape::Circle, 0.3f, 0.95f, 0.45f, 1.0f);         // other player
                const float cdir[4][2] = { {0,-1}, {1,0}, {0,1}, {-1,0} };   // N, E, S, W
                for (int i = 0; i < 4; ++i) compass_vis[i] = bearing_x(cdir[i][0], cdir[i][1], compass_x[i]);
            }

            // Item inventory (top-left, under the coins): one chip per upgrade held, with
            // its placeholder shape + color. Stack counts are drawn as text after draw_hud;
            // hovering a chip (cursor free) shows its description.
            {
                float ix = inv_x0;
                for (int i = 0; i < UPGRADE_COUNT; ++i) {
                    if (inventory[i] <= 0) continue;
                    const UpgradeDef& d = upgrade_def(static_cast<Upgrade>(i));
                    const float sxr = inv_chip / aspect;
                    hud_rect(ix - sxr, inv_y - inv_chip, ix + sxr, inv_y + inv_chip, 0.08f, 0.08f, 0.10f, 0.8f);
                    icon_shape(ix, inv_y, inv_s, d.shape, d.r, d.g, d.b, 1.0f);
                    if (cursor_free && !scoreboard && mxn >= ix - sxr && mxn <= ix + sxr
                        && myn >= inv_y - inv_chip && myn <= inv_y + inv_chip) {
                        tip = &d; tip_x = ix - sxr; tip_y = inv_y - inv_chip - 0.012f;
                    }
                    ix += inv_step;
                }
            }

            // Middle-mouse throw (Swordstorm) indicator, just left of the spell slots: a
            // storm icon + a little mouse with its MIDDLE button lit (so players know the
            // bind), with the throw cooldown sweeping over it. Always shown with a weapon.
            if (player.weapon) {
                const float bh = 0.12f, bw = bh / aspect, by = -0.95f, by1 = by + bh;
                const float bx0 = 0.55f - 0.05f - bw, bx1 = bx0 + bw;
                // Small mouse glyph with the middle button highlighted.
                auto draw_mouse = [&](float cx, float cy, float s) {
                    const float w = s * 0.66f / aspect, h = s;
                    hud_rect(cx - w, cy - h, cx + w, cy + h, 0.85f, 0.85f, 0.9f, 0.95f);          // body
                    const float ix = w * 0.6f, iy = h * 0.72f;
                    hud_rect(cx - ix, cy - iy, cx + ix, cy + iy, 0.12f, 0.12f, 0.15f, 1.0f);       // inset
                    const float mw = w * 0.2f;
                    hud_rect(cx - mw, cy + h * 0.06f, cx + mw, cy + iy, 1.0f, 0.85f, 0.25f, 1.0f); // middle button (lit)
                };
                hud_rect(bx0 - 0.007f, by - 0.007f, bx1 + 0.007f, by1 + 0.007f, 0.95f, 0.8f, 0.35f, 0.9f);  // gold border = manual
                hud_rect(bx0, by, bx1, by1, 0.10f, 0.10f, 0.12f, 0.85f);                                    // backing
                icon_shape((bx0 + bx1) * 0.5f, (by + by1) * 0.5f - 0.012f, bh * 0.30f, IconShape::Streak, 0.95f, 0.9f, 0.7f, 1.0f);  // storm
                draw_mouse(bx0 + bw * 0.5f, by1 - bh * 0.26f, bh * 0.16f);                           // MMB hint (top)
                const float cd = player.weapon->throw_cooldown * player.cooldown_mult;              // cooldown sweep
                const float frac = cd > 0.0f ? throw_cd / cd : 0.0f;
                if (frac > 0.0f) { const float fh = bh * (frac > 1.0f ? 1.0f : frac); hud_rect(bx0, by, bx1, by + fh, 1.0f, 1.0f, 1.0f, 0.40f); }
            }

            // Spell slots (bottom-right): one box per slot. An unlocked autocast fills a
            // slot with its icon; empty slots show a plain frame. A white sweep fills the
            // box while the autocast is recharging (height = remaining / total).
            {
                struct SpellSlot { IconShape sh; float r, g, b, frac; };
                std::vector<SpellSlot> spells;
                if (player.orbit_unlocked && player.weapon) {
                    const float cd = player.weapon->orbit_cooldown * player.orbit_cd_mult * player.autocast_cd_mult;
                    spells.push_back({ IconShape::Ring, 0.35f, 0.85f, 0.95f, cd > 0.0f ? orbit_cd / cd : 0.0f });
                }
                if (player.forcefield_unlocked && player.shield) {
                    const float cd = player.shield->bash_cooldown * player.forcefield_cd_mult * player.autocast_cd_mult;
                    spells.push_back({ IconShape::Burst, 0.85f, 0.90f, 1.0f, cd > 0.0f ? bash_cd / cd : 0.0f });
                }
                const float bh = 0.12f, bw = bh / aspect, by = -0.95f, gap = bw + 0.035f, x_start = 0.55f;
                for (int i = 0; i < player.spell_slots; ++i) {
                    const float bx0 = x_start + i * gap, bx1 = bx0 + bw, by1 = by + bh;
                    hud_rect(bx0 - 0.007f, by - 0.007f, bx1 + 0.007f, by1 + 0.007f, 0.85f, 0.85f, 0.9f, 0.9f);  // border
                    hud_rect(bx0, by, bx1, by1, 0.10f, 0.10f, 0.12f, 0.85f);                                    // backing
                    if (i < static_cast<int>(spells.size())) {
                        const SpellSlot& sp = spells[i];
                        icon_shape((bx0 + bx1) * 0.5f, (by + by1) * 0.5f, bh * 0.34f, sp.sh, sp.r, sp.g, sp.b, 1.0f);
                        if (sp.frac > 0.0f) {                                                                  // cooldown sweep
                            const float fh = bh * (sp.frac > 1.0f ? 1.0f : sp.frac);
                            hud_rect(bx0, by, bx1, by + fh, 1.0f, 1.0f, 1.0f, 0.40f);
                        }
                    } else {
                        icon_shape((bx0 + bx1) * 0.5f, (by + by1) * 0.5f, bh * 0.30f, IconShape::Slot, 0.30f, 0.30f, 0.35f, 0.8f);  // empty frame
                    }
                }
            }

            // Death flash: full-screen red overlay that fades out.
            if (death_flash > 0.0f)
                hud_rect(-1.0f, -1.0f, 1.0f, 1.0f, 0.7f, 0.0f, 0.0f, clamp01(death_flash / 1.2f) * 0.6f);
            // Victory flash: full-screen gold overlay (enemy base destroyed).
            if (victory_flash > 0.0f)
                hud_rect(-1.0f, -1.0f, 1.0f, 1.0f, 0.9f, 0.7f, 0.1f, clamp01(victory_flash / 2.0f) * 0.5f);
            // Build & muster menu panel (rects here; labels in the text pass below).
            if (spawn_menu) {
                hud_rect(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.6f);        // dim
                hud_rect(-0.66f, -0.62f, 0.66f, 0.56f, 0.08f, 0.09f, 0.13f, 0.97f); // panel (larger)
                for (int k = 0; k < dc::game::MOB_TYPE_COUNT; ++k) {               // mob-type rows
                    const float ry = 0.40f - k * 0.072f;
                    const bool unlocked = (barracks_unlocked & (1u << k)) != 0;
                    if (unlocked) hud_rect(-0.62f, ry - 0.035f, 0.62f, ry + 0.045f, 0.14f, 0.22f, 0.32f, 0.9f);
                    else          hud_rect(-0.62f, ry - 0.035f, 0.62f, ry + 0.045f, 0.20f, 0.12f, 0.10f, 0.9f);
                }
                const float ry8 = 0.40f - dc::game::MOB_TYPE_COUNT * 0.072f;       // expand-area row
                hud_rect(-0.62f, ry8 - 0.035f, 0.62f, ry8 + 0.045f, 0.22f, 0.18f, 0.10f, 0.9f);
            }
            // Barracks upgrade menu panel.
            if (upgrade_menu) {
                hud_rect(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.55f);
                hud_rect(-0.5f, -0.28f, 0.5f, 0.42f, 0.08f, 0.10f, 0.14f, 0.97f);
                for (int k = 0; k < 5; ++k) { const float ry = 0.24f - k * 0.085f;
                    hud_rect(-0.46f, ry - 0.035f, 0.46f, ry + 0.04f, 0.14f, 0.20f, 0.30f, 0.9f); }
                hud_rect(-0.46f, -0.255f, 0.46f, -0.185f, 0.28f, 0.14f, 0.12f, 0.9f);   // sell row
            }

            // Command minimap: a lane strip per unlocked mob type with a draggable HOLD pin.
            if (cmd_map) {
                hud_rect(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.55f);      // dim
                hud_rect(-0.78f, -0.55f, 0.78f, 0.50f, 0.07f, 0.09f, 0.12f, 0.96f); // panel
                // your army's current front line (furthest-advanced ally), as a vertical tick.
                float frontf = 0.0f;
                for (const auto& a : allies) { const float f = (a.pos[0]-core_pos[0])/(enemy_core_pos[0]-core_pos[0]); if (f > frontf) frontf = f; }
                for (int t = 0; t < dc::game::MOB_TYPE_COUNT; ++t) {
                    if (!(barracks_unlocked & (1u << t))) continue;
                    const float ry = cm_rowY(t);
                    hud_rect(CM_SX0, ry - 0.012f, CM_SX1, ry + 0.012f, 0.16f, 0.20f, 0.26f, 0.95f);   // lane strip
                    hud_rect(CM_SX0 - 0.004f, ry - 0.02f, CM_SX0 + 0.012f, ry + 0.02f, 0.3f, 0.6f, 1.0f, 0.95f);  // your base (blue)
                    hud_rect(CM_SX1 - 0.012f, ry - 0.02f, CM_SX1 + 0.004f, ry + 0.02f, 1.0f, 0.3f, 0.2f, 0.95f);  // enemy base (red)
                    const float fx = CM_SX0 + frontf * (CM_SX1 - CM_SX0);   // front-line tick
                    hud_rect(fx - 0.003f, ry - 0.016f, fx + 0.003f, ry + 0.016f, 0.9f, 0.9f, 0.4f, 0.7f);
                    // AUTO bay (left of the strip)
                    const bool autom = type_hold_x[t] < 0.0f;
                    hud_rect(CM_SX0 - 0.18f, ry - 0.022f, CM_SX0 - 0.10f, ry + 0.022f, autom?0.2f:0.1f, autom?0.4f:0.14f, autom?0.25f:0.16f, 0.9f);
                    // the draggable pin (follows the cursor while dragging this row's pin)
                    const float px = (cmd_drag == t) ? cmd_drag_mx : cm_pinx(t);
                    const float py = (cmd_drag == t) ? cmd_drag_my : ry;
                    hud_rect(px - 0.016f, py - 0.03f, px + 0.016f, py + 0.03f, 0.35f, 1.0f, 0.7f, 1.0f);  // pin (cyan)
                    // LIVE UNIT DOTS on this row: white = your mobs of this type, red = the matching
                    // enemy kind. Small, to keep it readable.
                    const float lanelen = (enemy_core_pos[0] - core_pos[0]);
                    auto dot = [&](float wx, float r, float g, float b) {
                        float f = lanelen > 1.0f ? (wx - core_pos[0]) / lanelen : 0.0f; f = clamp01(f);
                        const float dx = CM_SX0 + f * (CM_SX1 - CM_SX0);
                        hud_rect(dx - 0.004f, ry - 0.009f, dx + 0.004f, ry + 0.009f, r, g, b, 0.95f);
                    };
                    const bool is_cl = (net.role == dc::net::Role::Client);
                    if (is_cl) { for (const auto& a : net_allies) if (a.kind == t) dot(a.x, 1.0f, 1.0f, 1.0f); }
                    else       { for (const auto& a : allies)     if (a.kind == t) dot(a.pos[0], 1.0f, 1.0f, 1.0f); }
                    // matching enemy kind for this player type's role (host has live entities).
                    int ek = -1;
                    switch (dc::game::mob_type(t).visual) {
                        case dc::game::MobVisual::Ground:   ek = (int)dc::entity::EnemyKind::Skeleton; break;
                        case dc::game::MobVisual::Mage:     ek = (int)dc::entity::EnemyKind::Ranged;   break;
                        case dc::game::MobVisual::Bat:      ek = (int)dc::entity::EnemyKind::Bat;      break;
                        case dc::game::MobVisual::Flier:    ek = (int)dc::entity::EnemyKind::Flying;   break;
                        case dc::game::MobVisual::Demon:    ek = (int)dc::entity::EnemyKind::Demon;    break;
                        case dc::game::MobVisual::Insulter: ek = (int)dc::entity::EnemyKind::Insulter; break;
                        case dc::game::MobVisual::Flame:    ek = (int)dc::entity::EnemyKind::Flamethrower; break;
                        case dc::game::MobVisual::Troll:    ek = (int)dc::entity::EnemyKind::Troll;    break;
                        case dc::game::MobVisual::Slime:    ek = (int)dc::entity::EnemyKind::Slime;    break;
                        default: break;
                    }
                    if (!is_cl && ek >= 0) for (const auto& e : entities.items)
                        if (e.alive && e.type == dc::entity::EntityType::Enemy &&
                            ((int)e.kind == ek || (ek == (int)dc::entity::EnemyKind::Skeleton && e.kind == dc::entity::EnemyKind::Melee)))
                            dot(e.position[0], 1.0f, 0.25f, 0.2f);
                }
            }

            // Chest purchase menu: dim + 4 cards (icon band + border); sold slots are blank.
            // Hovering a buyable card shows its description tooltip. Text drawn after draw_hud.
            if (menu_chest >= 0 && menu_chest < static_cast<int>(chests.size())) {
                const Chest& mc = chests[menu_chest];
                hud_rect(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.5f);   // dim backdrop
                for (int s = 0; s < 4; ++s) {
                    const float cx0 = card_x0(s), cx1 = cx0 + CARD_W;
                    const bool sold = mc.taken[s];
                    const UpgradeDef& d = upgrade_def(mc.contents[s]);
                    const bool hot = cursor_free && !sold && mxn >= cx0 && mxn <= cx1 && myn >= CARD_BOT && myn <= CARD_TOP;
                    const float bw = hot ? 0.014f : 0.008f;
                    hud_rect(cx0 - bw, CARD_BOT - bw, cx1 + bw, CARD_TOP + bw, 0.97f, 0.97f, 0.97f, 0.97f);  // border
                    if (sold) { hud_rect(cx0, CARD_BOT, cx1, CARD_TOP, 0.09f, 0.09f, 0.10f, 0.95f); continue; }
                    hud_rect(cx0, CARD_BOT, cx1, CARD_TOP, 0.12f, 0.12f, 0.14f, 0.96f);                      // backing
                    hud_rect(cx0, 0.08f, cx1, CARD_TOP, d.r * 0.5f, d.g * 0.5f, d.b * 0.5f, 0.9f);           // color band
                    icon_shape((cx0 + cx1) * 0.5f, 0.25f, 0.10f, d.shape, d.r, d.g, d.b, 1.0f);
                    if (hot) { tip = &d; tip_x = mxn + 0.012f; tip_y = myn - 0.012f; }
                }
            }

            // Level-up menu: dim + the (up to 4) eligible upgrade cards. Same layout as a
            // chest; click one to gain it. Title text drawn after draw_hud.
            if (levelup_open) {
                hud_rect(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.22f);   // light dim (stay see-through; you can still move)
                for (int s = 0; s < levelup_card_count; ++s) {
                    const float cx0 = card_x0(s), cx1 = cx0 + CARD_W;
                    const UpgradeDef& d = upgrade_def(levelup_cards[s]);
                    const bool hot = cursor_free && mxn >= cx0 && mxn <= cx1 && myn >= CARD_BOT && myn <= CARD_TOP;
                    const float bw = hot ? 0.014f : 0.008f;
                    hud_rect(cx0 - bw, CARD_BOT - bw, cx1 + bw, CARD_TOP + bw, 0.95f, 0.95f, 0.95f, hot ? 0.9f : 0.6f);  // border
                    hud_rect(cx0, CARD_BOT, cx1, CARD_TOP, 0.12f, 0.12f, 0.14f, 0.72f);                      // backing (translucent)
                    hud_rect(cx0, 0.08f, cx1, CARD_TOP, d.r * 0.5f, d.g * 0.5f, d.b * 0.5f, 0.62f);          // color band
                    icon_shape((cx0 + cx1) * 0.5f, 0.25f, 0.10f, d.shape, d.r, d.g, d.b, 1.0f);
                    if (hot) { tip = &d; tip_x = mxn + 0.012f; tip_y = myn - 0.012f; }
                }
            }

            // First-person crosshair: a small + at screen center (skip while a menu's up).
            // Correct the horizontal extent by aspect so it isn't stretched wide.
            if (!ui_open && !dead) {
                int cw, chh; window.window_size(cw, chh);
                const float aspect = (cw > 0 && chh > 0) ? static_cast<float>(cw) / chh : 1.0f;
                const float t = 0.004f, len = 0.022f;       // thickness, arm length (NDC-y units)
                const float tx = t / aspect, lx = len / aspect;
                hud_rect(-lx, -t, lx, t, 0.95f, 0.95f, 0.95f, 0.85f);   // horizontal arm
                hud_rect(-tx, -len, tx, len, 0.95f, 0.95f, 0.95f, 0.85f);   // vertical arm
            }

            // Pause menu (ESC): dim + a red Quit button (click to exit; ESC to resume).
            if (paused) {
                hud_rect(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.55f);  // dim backdrop
                hud_rect(QX0 - 0.01f, QY0 - 0.01f, QX1 + 0.01f, QY1 + 0.01f, 0.95f, 0.95f, 0.95f, 0.95f);  // border
                hud_rect(QX0, QY0, QX1, QY1, 0.8f, 0.15f, 0.15f, 0.95f);     // red = quit
            }

            // Scoreboard (hold Tab): dim + a centered panel. Row highlight for your own
            // row, and a row of your item chips (hoverable). Text drawn after draw_hud.
            if (scoreboard) {
                hud_rect(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.6f);                    // dim
                hud_rect(sb_x0 - 0.012f, sb_bot - 0.012f, sb_x1 + 0.012f, sb_top + 0.012f, 0.9f, 0.9f, 0.95f, 0.95f);  // border
                hud_rect(sb_x0, sb_bot, sb_x1, sb_top, 0.07f, 0.07f, 0.10f, 0.97f);            // panel
                // Highlight the local player's leaderboard row.
                for (std::size_t i = 0; i < scores.size(); ++i) {
                    const float ry1 = sb_rows_top - i * sb_row_h, ry0 = ry1 - sb_row_h;
                    if (scores[i].local)
                        hud_rect(sb_x0 + 0.02f, ry0 + 0.006f, sb_x1 - 0.02f, ry1 - 0.006f, 0.20f, 0.35f, 0.55f, 0.55f);
                }
                // Your item chips, centered, below the rows. Hover -> tooltip.
                int held = 0; for (int i = 0; i < UPGRADE_COUNT; ++i) if (inventory[i] > 0) ++held;
                float ix = -((held - 1) * 0.5f) * sb_item_step;
                for (int i = 0; i < UPGRADE_COUNT; ++i) {
                    if (inventory[i] <= 0) continue;
                    const UpgradeDef& d = upgrade_def(static_cast<Upgrade>(i));
                    const float sxr = sb_item_chip / aspect;
                    hud_rect(ix - sxr, sb_items_y - sb_item_chip, ix + sxr, sb_items_y + sb_item_chip, 0.10f, 0.10f, 0.13f, 0.95f);
                    icon_shape(ix, sb_items_y, sb_item_s, d.shape, d.r, d.g, d.b, 1.0f);
                    if (mxn >= ix - sxr && mxn <= ix + sxr
                        && myn >= sb_items_y - sb_item_chip && myn <= sb_items_y + sb_item_chip) {
                        tip = &d; tip_x = ix - sxr; tip_y = sb_items_y - sb_item_chip - 0.012f;
                    }
                    ix += sb_item_step;
                }
            }

            // Tooltip panel (sized to the wider of name/description, clamped on-screen).
            // Its rects go into the HUD now; the text is drawn just after draw_hud.
            const float tip_px = 15.0f;
            float tip_panel_x = 0.0f, tip_panel_top = 0.0f, tip_lineh = 0.0f;
            if (tip) {
                const float wn = renderer.text_width(tip->name, tip_px, fbw);
                const float wd = renderer.text_width(tip->desc, tip_px, fbw);
                const float pad = 0.014f;
                const float pw = (wn > wd ? wn : wd) + pad * 2.0f;
                tip_lineh = tip_px / fbh * 2.0f * 1.5f;
                const float ph = tip_lineh * 2.0f + pad;
                float px = tip_x, top = tip_y;
                if (px + pw > 0.98f)   px = 0.98f - pw;
                if (px < -0.98f)       px = -0.98f;
                if (top > 0.98f)       top = 0.98f;
                if (top - ph < -0.98f) top = -0.98f + ph;
                tip_panel_x = px; tip_panel_top = top;
                hud_rect(px - 0.006f, top - ph - 0.006f, px + pw + 0.006f, top + 0.006f, 0.9f, 0.9f, 0.95f, 0.95f);  // border
                hud_rect(px, top - ph, px + pw, top, 0.06f, 0.06f, 0.09f, 0.96f);                                    // panel
            }

            renderer.draw_hud(hud);

            // --- Text overlays (separate glyph pass, drawn on top of the HUD rects) ---
            // Day/time clock (top-center, just under the compass strip): e.g. "Day 2  7:14 PM".
            // Tinted warm by day, cool blue at night so the phase reads at a glance.
            {
                char clk[32]; clock_str(clk, sizeof clk);
                const float cpx = 22.0f;
                const float w = renderer.text_width(clk, cpx, fbw);
                vec3 col;
                if (is_night()) { col[0] = 0.55f; col[1] = 0.7f; col[2] = 1.0f; }
                else            { col[0] = 1.0f;  col[1] = 0.92f; col[2] = 0.6f; }
                renderer.draw_text(clk, -w * 0.5f, 0.86f, cpx, col, 1.0f, fbw, fbh);
            }
            // Controls hint (bottom, dim) — only when no menu is up, so the player can discover
            // the lane-command keys. Suppressed during any overlay.
            if (!ui_open && !building_mode && !dead) {
                vec3 hint = { 0.55f, 0.6f, 0.66f };
                const char* h = "E muster/build   B build   M command map   C/X rally   I taunt";
                const float hw = renderer.text_width(h, 11.0f, fbw);
                renderer.draw_text(h, -hw * 0.5f, -0.965f, 11.0f, hint, 0.8f, fbw, fbh);
            }
            // Big centered banner on win/lose.
            if (victory_flash > 0.0f) {
                vec3 gold = { 1.0f, 0.85f, 0.2f };
                const float vpx = 64.0f, w = renderer.text_width("VICTORY!", vpx, fbw);
                renderer.draw_text("VICTORY!", -w * 0.5f, 0.05f, vpx, gold, clamp01(victory_flash / 2.0f), fbw, fbh);
            }
            // Downed: big respawn countdown (you ghost meanwhile and can spectate).
            if (respawn_timer > 0.0f) {
                vec3 red = { 1.0f, 0.4f, 0.35f }, dim = { 0.85f, 0.85f, 0.9f };
                const float dw = renderer.text_width("YOU DIED", 56.0f, fbw);
                renderer.draw_text("YOU DIED", -dw * 0.5f, 0.12f, 56.0f, red, 1.0f, fbw, fbh);
                char rc[48]; std::snprintf(rc, sizeof rc, "respawning in %d", (int)std::ceil(respawn_timer));
                const float cw = renderer.text_width(rc, 26.0f, fbw);
                renderer.draw_text(rc, -cw * 0.5f, 0.02f, 26.0f, dim, 1.0f, fbw, fbh);
            }
            // Command minimap labels.
            if (cmd_map) {
                vec3 gold = {1.0f,0.85f,0.3f}, white = {0.9f,0.95f,1.0f}, dim = {0.55f,0.6f,0.66f};
                renderer.draw_text("COMMAND  -  drag a pin onto the lane to HOLD, off = auto-march.  M/E close", -0.76f, 0.44f, 12.0f, gold, 1.0f, fbw, fbh);
                for (int t = 0; t < dc::game::MOB_TYPE_COUNT; ++t) {
                    if (!(barracks_unlocked & (1u << t))) continue;
                    const float ry = (0.30f - t * 0.085f) + 0.03f;
                    renderer.draw_text(dc::game::mob_type(t).name, -0.76f, ry, 12.0f, white, 1.0f, fbw, fbh);
                    renderer.draw_text("auto", -0.745f, (0.30f - t*0.085f) - 0.005f, 9.0f, dim, 1.0f, fbw, fbh);
                }
            }
            // Build & muster menu labels.
            if (spawn_menu) {
                vec3 gold = {1.0f,0.85f,0.3f}, white = {0.95f,0.95f,0.95f}, grey = {0.6f,0.62f,0.68f};
                renderer.draw_text("MUSTER  -  1-8 mob barracks, 9 area  (defenses + ships via B)  E close", -0.62f, 0.48f, 14.0f, gold, 1.0f, fbw, fbh);
                for (int k = 0; k < dc::game::MOB_TYPE_COUNT; ++k) {
                    const dc::game::MobType& mt = dc::game::mob_type(k);
                    const bool unlocked = (barracks_unlocked & (1u << k)) != 0;
                    const float ry = 0.40f - k * 0.072f;
                    char line[128];
                    if (unlocked)
                        std::snprintf(line, sizeof line, "[%d] %-9s buy $%d   /%.0fs  (%.0fhp %.0fdmg)",
                                      k+1, mt.name, mt.place_cost, mt.interval, mt.hp, mt.damage);
                    else
                        std::snprintf(line, sizeof line, "[%d] %-9s  press to UNLOCK (free)", k+1, mt.name);
                    renderer.draw_text(line, -0.60f, ry - 0.005f, 15.0f, unlocked ? white : grey, 1.0f, fbw, fbh);
                }
                {   // expand-area row
                    char line[128];
                    if (base.build_radius < dc::game::BASE_AREA_MAX)
                        std::snprintf(line, sizeof line, "[0] Expand build area  buy $%d", dc::game::base_area_cost(base.build_radius));
                    else std::snprintf(line, sizeof line, "[9] Build area MAXED");
                    const float ry = 0.40f - dc::game::MOB_TYPE_COUNT * 0.072f;
                    renderer.draw_text(line, -0.60f, ry - 0.005f, 15.0f, gold, 1.0f, fbw, fbh);
                }
            }
            // Barracks upgrade menu labels.
            if (upgrade_menu) {
                const int bidx = piece_index_at(upg_col, upg_row);
                if (bidx >= 0 && base.pieces[bidx].piece == static_cast<uint8_t>(dc::game::BuildPiece::Barracks)) {
                    const auto& bp = base.pieces[bidx];
                    vec3 gold = {1.0f,0.85f,0.3f}, white = {0.95f,0.95f,0.95f}, red = {1.0f,0.55f,0.45f}, dimc = {0.6f,0.65f,0.72f};
                    char hdr[80]; std::snprintf(hdr, sizeof hdr, "UPGRADE  %s barracks   (Lv %d, cap %d)", dc::game::mob_type(bp.rot).name, dc::game::barracks_up_total(bp), dc::game::mob_type(bp.rot).cap + dc::game::barracks_cap_bonus(bp.up[4]));
                    renderer.draw_text(hdr, -0.46f, 0.34f, 15.0f, gold, 1.0f, fbw, fbh);
                    const char* nm[5] = { "Health", "Defense", "Speed", "Spawn Rate", "Capacity" };
                    for (int s = 0; s < 5; ++s) {
                        const float ry = 0.24f - s * 0.085f;
                        char line[96];
                        if (bp.up[s] >= dc::game::BARRACKS_UP_MAX) std::snprintf(line, sizeof line, "[%d] %-10s  L%d  MAX", s+1, nm[s], bp.up[s]);
                        else std::snprintf(line, sizeof line, "[%d] %-10s  L%d -> L%d   $%d", s+1, nm[s], bp.up[s], bp.up[s]+1, dc::game::barracks_upgrade_cost(bp.up[s]));
                        renderer.draw_text(line, -0.44f, ry - 0.005f, 15.0f, white, 1.0f, fbw, fbh);
                    }
                    char sell[64]; std::snprintf(sell, sizeof sell, "[S] Sell barracks  (+$%d)", (dc::game::mob_type(bp.rot).place_cost * 3) / 4);
                    renderer.draw_text(sell, -0.44f, -0.235f, 15.0f, red, 1.0f, fbw, fbh);
                    renderer.draw_text("E / Esc  close", -0.10f, 0.37f, 13.0f, dimc, 1.0f, fbw, fbh);
                }
            }
            // Build-mode palette: current piece + (for barracks) the mob type + key hints.
            if (building_mode) {
                vec3 c = {0.9f,0.95f,1.0f}, hint = {0.6f,0.65f,0.75f};
                char line[128];
                if (build_sel == static_cast<int>(dc::game::BuildPiece::Barracks)) {
                    const dc::game::MobType& mt = dc::game::mob_type(build_tier);
                    std::snprintf(line, sizeof line, "BUILD: Barracks [%s]  $%d   (T: type)", mt.name, mt.place_cost);
                } else if (build_sel == static_cast<int>(dc::game::BuildPiece::Shipyard)) {
                    const char* bt = shipyard_type==1 ? "Minelayer" : shipyard_type==2 ? "Minesweeper" : "Warship";
                    std::snprintf(line, sizeof line, "BUILD: Shipyard [%s]  $%d   (T: type)", bt, dc::game::piece_cost(dc::game::BuildPiece::Shipyard));
                } else {
                    const dc::game::BuildPiece bp = static_cast<dc::game::BuildPiece>(build_sel);
                    std::snprintf(line, sizeof line, "BUILD: %s  $%d", dc::game::piece_name(bp), dc::game::piece_cost(bp));
                }
                renderer.draw_text(line, -0.32f, -0.74f, 15.0f, c, 1.0f, fbw, fbh);
                renderer.draw_text("1 wall  2 mine  3 turret  4 vacuum  5 shipyard  6 mortar   (T: ship type)   R rot   LMB place   RMB remove   B exit",
                                   -0.52f, -0.80f, 13.0f, hint, 1.0f, fbw, fbh);
                // Barracks capacity readout when the barracks tool is selected.
                if (build_sel == static_cast<int>(dc::game::BuildPiece::Barracks)) {
                    int nbar = 0; for (const auto& q : base.pieces) if (q.piece == static_cast<uint8_t>(dc::game::BuildPiece::Barracks)) ++nbar;
                    const int capb = dc::game::barracks_capacity(base.build_radius);
                    char cap[48]; std::snprintf(cap, sizeof cap, "Barracks  %d / %d   (expand area for more)", nbar, capb);
                    vec3 cc; if (nbar >= capb) { cc[0]=1.0f; cc[1]=0.5f; cc[2]=0.4f; } else { cc[0]=0.7f; cc[1]=0.9f; cc[2]=0.75f; }
                    renderer.draw_text(cap, -0.30f, -0.69f, 12.0f, cc, 1.0f, fbw, fbh);
                }
                // Hovering an EXISTING barracks -> show its per-stat upgrade panel (keys 6-9).
                const int hbidx = build_has_target ? piece_index_at(build_col, build_row) : -1;
                if (hbidx >= 0 && base.pieces[hbidx].piece == static_cast<uint8_t>(dc::game::BuildPiece::Barracks)) {
                    const auto& up = base.pieces[hbidx].up;
                    auto cst = [&](int s){ return up[s] >= dc::game::BARRACKS_UP_MAX ? 0 : dc::game::barracks_upgrade_cost(up[s]); };
                    char ul[160];
                    std::snprintf(ul, sizeof ul, "UPGRADE troops:  [6]HP L%d($%d)  [7]DEF L%d($%d)  [8]SPD L%d($%d)  [9]Rate L%d($%d)",
                        up[0],cst(0), up[1],cst(1), up[2],cst(2), up[3],cst(3));
                    vec3 ug = {1.0f,0.85f,0.35f};
                    renderer.draw_text(ul, -0.62f, -0.86f, 11.0f, ug, 1.0f, fbw, fbh);
                }
            }
            // Item stack counts: bottom-right of each inventory chip.
            {
                float ix = inv_x0;
                for (int i = 0; i < UPGRADE_COUNT; ++i) {
                    if (inventory[i] <= 0) continue;
                    char cnt[8]; std::snprintf(cnt, sizeof cnt, "%d", inventory[i]);
                    const float cpx = 14.0f;
                    const float w = renderer.text_width(cnt, cpx, fbw);
                    vec3 white = {1.0f, 1.0f, 1.0f};
                    renderer.draw_text(cnt, ix + inv_chip / aspect - w - 0.004f, inv_y - 0.028f,
                                       cpx, white, 1.0f, fbw, fbh);
                    ix += inv_step;
                }
            }
            // Scoreboard text: title, column headers, ranked rows, and your item counts.
            if (scoreboard) {
                vec3 white = {0.95f, 0.97f, 1.0f}, gray = {0.70f, 0.75f, 0.82f}, gold = {1.0f, 0.85f, 0.30f};
                { const char* t = "SCOREBOARD"; const float tpx = 26.0f;
                  renderer.draw_text(t, -renderer.text_width(t, tpx, fbw) * 0.5f, sb_top - 0.045f, tpx, gold, 1.0f, fbw, fbh); }
                { const float hpx = 12.0f;
                  renderer.draw_text("PLAYER", sb_x0 + 0.04f, sb_rows_top + 0.028f, hpx, gray, 1.0f, fbw, fbh);
                  const char* dh = "DAMAGE"; const float w = renderer.text_width(dh, hpx, fbw);
                  renderer.draw_text(dh, sb_x1 - 0.04f - w, sb_rows_top + 0.028f, hpx, gray, 1.0f, fbw, fbh); }
                const float rpx = 18.0f;
                for (std::size_t i = 0; i < scores.size(); ++i) {
                    char name[28];
                    const char* you = scores[i].local ? " (you)" : "";
                    if (scores[i].id == 0) std::snprintf(name, sizeof name, "Host%s", you);
                    else                   std::snprintf(name, sizeof name, "P%u%s", (unsigned)scores[i].id, you);
                    char dmg[16]; fmt_dmg(scores[i].dmg, dmg, sizeof dmg);
                    const float ty = (sb_rows_top - i * sb_row_h) - 0.014f;
                    renderer.draw_text(name, sb_x0 + 0.04f, ty, rpx, white, 1.0f, fbw, fbh);
                    const float w = renderer.text_width(dmg, rpx, fbw);
                    renderer.draw_text(dmg, sb_x1 - 0.04f - w, ty, rpx, gold, 1.0f, fbw, fbh);
                }
                renderer.draw_text("ITEMS", sb_x0 + 0.04f, sb_items_y + sb_item_chip + 0.03f, 12.0f, gray, 1.0f, fbw, fbh);
                int held = 0; for (int i = 0; i < UPGRADE_COUNT; ++i) if (inventory[i] > 0) ++held;
                if (held == 0) {
                    const char* none = "(none yet)";
                    renderer.draw_text(none, -renderer.text_width(none, 15.0f, fbw) * 0.5f, sb_items_y, 15.0f, gray, 1.0f, fbw, fbh);
                } else {
                    float ix = -((held - 1) * 0.5f) * sb_item_step;
                    for (int i = 0; i < UPGRADE_COUNT; ++i) {
                        if (inventory[i] <= 0) continue;
                        char cnt[8]; std::snprintf(cnt, sizeof cnt, "%d", inventory[i]);
                        const float cpx = 15.0f; const float w = renderer.text_width(cnt, cpx, fbw);
                        vec3 wht = {1.0f, 1.0f, 1.0f};
                        renderer.draw_text(cnt, ix + sb_item_chip / aspect - w - 0.004f, sb_items_y - 0.04f, cpx, wht, 1.0f, fbw, fbh);
                        ix += sb_item_step;
                    }
                }
            }

            // Quit-button label (pause menu).
            if (paused) {
                const float qpx = 22.0f;
                const float w = renderer.text_width("QUIT", qpx, fbw);
                vec3 white = {1.0f, 0.95f, 0.95f};
                renderer.draw_text("QUIT", (QX0 + QX1) * 0.5f - w * 0.5f, (QY0 + QY1) * 0.5f + 0.02f,
                                   qpx, white, 1.0f, fbw, fbh);
            }
            // Tooltip text over its panel.
            if (tip) {
                vec3 white = {0.97f, 0.97f, 1.0f}, gray = {0.72f, 0.77f, 0.85f};
                renderer.draw_text(tip->name, tip_panel_x + 0.014f, tip_panel_top - 0.006f, tip_px, white, 1.0f, fbw, fbh);
                renderer.draw_text(tip->desc, tip_panel_x + 0.014f, tip_panel_top - tip_lineh - 0.006f, tip_px, gray, 1.0f, fbw, fbh);
            }

            // Chest purchase menu text: title + each card's name and price ("SOLD" for
            // bought slots). Cards/icons are drawn in the HUD pass above; hover -> tooltip.
            if (menu_chest >= 0 && menu_chest < static_cast<int>(chests.size())) {
                const Chest& mc = chests[menu_chest];
                vec3 white = {0.97f, 0.97f, 1.0f}, gold = {1.0f, 0.85f, 0.3f}, dimc = {0.5f, 0.5f, 0.55f};
                { const char* t = "CHOOSE ONE"; renderer.draw_text(t, -renderer.text_width(t, 22.0f, fbw) * 0.5f, CARD_TOP + 0.10f, 22.0f, gold, 1.0f, fbw, fbh); }
                for (int s = 0; s < 4; ++s) {
                    const float cx0 = card_x0(s), cxc = cx0 + CARD_W * 0.5f;
                    if (mc.taken[s]) {
                        const char* t = "SOLD";
                        renderer.draw_text(t, cxc - renderer.text_width(t, 16.0f, fbw) * 0.5f, 0.04f, 16.0f, dimc, 1.0f, fbw, fbh);
                        continue;
                    }
                    const UpgradeDef& d = upgrade_def(mc.contents[s]);
                    renderer.draw_text(d.name, cxc - renderer.text_width(d.name, 15.0f, fbw) * 0.5f, -0.02f, 15.0f, white, 1.0f, fbw, fbh);
                    char price[16]; std::snprintf(price, sizeof price, "%d g", mc.cost);
                    const bool afford = currency >= mc.cost;
                    renderer.draw_text(price, cxc - renderer.text_width(price, 14.0f, fbw) * 0.5f, -0.30f, 14.0f, afford ? gold : dimc, 1.0f, fbw, fbh);
                }
            }

            // Level-up menu text: title (with the new level) + each card's name.
            if (levelup_open) {
                vec3 white = {0.97f, 0.97f, 1.0f}, cyanc = {0.45f, 0.85f, 1.0f};
                char title[32]; std::snprintf(title, sizeof title, "LEVEL %d", player.level);
                renderer.draw_text(title, -renderer.text_width(title, 24.0f, fbw) * 0.5f, CARD_TOP + 0.10f, 24.0f, cyanc, 1.0f, fbw, fbh);
                for (int s = 0; s < levelup_card_count; ++s) {
                    const float cxc = card_x0(s) + CARD_W * 0.5f;
                    const UpgradeDef& d = upgrade_def(levelup_cards[s]);
                    renderer.draw_text(d.name, cxc - renderer.text_width(d.name, 15.0f, fbw) * 0.5f, -0.02f, 15.0f, white, 1.0f, fbw, fbh);
                }
            }

            // Compass cardinal letters (N/E/S/W), above the strip; North highlighted.
            {
                const char* lbl[4] = { "N", "E", "S", "W" };
                vec3 nclr = {1.0f, 0.55f, 0.45f}, oclr = {0.9f, 0.92f, 1.0f};
                for (int i = 0; i < 4; ++i) {
                    if (!compass_vis[i]) continue;
                    float* cc = (i == 0) ? nclr : oclr;
                    renderer.draw_text(lbl[i], compass_x[i] - renderer.text_width(lbl[i], 16.0f, fbw) * 0.5f,
                                       COMPASS_LY, 16.0f, cc, 1.0f, fbw, fbh);
                }
            }

            // Enemy taunts: project each over its enemy's head, rising + fading in angry yellow.
            // Held back ~0.22s so the spoken line (which has synth startup latency) lands with it.
            for (const auto& t : taunts) {
                if (t.age < 0.22f) continue;
                vec4 wp = { t.pos[0], t.pos[1] + t.age * 0.6f, t.pos[2], 1.0f }, clip;
                glm_mat4_mulv(renderer.viewproj, wp, clip);
                if (clip[3] <= 0.05f) continue;                       // behind the camera
                const float ndcx = clip[0] / clip[3], ndcy = clip[1] / clip[3];
                if (ndcx < -1.1f || ndcx > 1.1f || ndcy < -1.1f || ndcy > 1.1f) continue;
                const char* txt = t.text.c_str();
                const float w = renderer.text_width(txt, 17.0f, fbw);
                vec3 yellow = { 1.0f, 0.92f, 0.15f };
                renderer.draw_text(txt, ndcx - w * 0.5f, ndcy, 17.0f, yellow, 1.0f - t.age / TAUNT_LIFE, fbw, fbh);
            }

            // Debug overlay text (toggle with V): on-screen readout via the glyph renderer.
            if (show_hitboxes) {
                int tw, th; window.framebuffer_size(tw, th);
                char buf[160];
                const char* atk = punching ? "SWING" : "-";
                const char* blk = block_ready ? "BLOCK" : (blocking ? "raising" : "-");
                std::snprintf(buf, sizeof buf,
                              "hp %.0f   stam %.0f   coins %d   enemies %zu   atk %s   shield %s",
                              player.health, player.stamina, currency, entities.items.size(), atk, blk);
                vec3 tcol = {0.85f, 0.95f, 1.0f};
                renderer.draw_text(buf, -0.97f, 0.80f, 18.0f, tcol, 0.9f, tw, th);
                vec3 acol = {1.0f, 0.7f, 0.55f};   // enemy AI brain readout
                renderer.draw_text(ai_status.c_str(), -0.97f, 0.74f, 18.0f, acol, 0.9f, tw, th);
            }
        }

        window.swap();

        if (smoke) { std::printf("smoke: one frame rendered, exiting\n"); break; }
    }

    chest_model.destroy();
    helmet_model.destroy();
    shield_model.destroy();
    torch_model.destroy();
    pillar_mesh.destroy();
    core_mesh.destroy();
    turret_mesh.destroy();
    player_model.destroy();
    sword_model.destroy();
    if (eye_loaded) eye_model.destroy();
    mesh.destroy();
    terrain_mesh.destroy();
    flyer_mesh.destroy();
    renderer.shutdown();
    net.shutdown();
    window.shutdown();
    return 0;
}
