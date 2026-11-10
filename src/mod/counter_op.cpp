// Counter-Operative (pdz_counterop, default off): docs/COUNTEROP_STUDY.md stages 0-2 and the minimum of stage 3.
//
// A normal split-screen co-op mission (game type 1 kept, so every co-op path of Rare's keeps working). Player 1 is
// the agent. Player 2 is the counter-operative: Rare's co-op human, SPAWNED at a guard's spot (log 312 redesign: he
// no longer takes a live guard's body), with that guard's gun and on the guards' side through Rare's own team masks
// (study section 5, option b): player 2's team bits (+128) = 0x0001 (ALPHA, which every SP guard counts among its
// allies, 0xFFFF) and his enemy mask (+132) = 0x0002 (BETA, the humans' bit); his allies (+130) = 0xFFFF, so all
// three masks are a team-0 guard's (the NPC constructor 8252BD50..8252BD88 via 8252FC18). Rare's friend test
// (8251EBD0: (other +128) & (my +128 | +130) and not (other +128) & my +132; vtable +460 swaps the two while +5640 >
// 0) then makes every character treat him as it treats a team-0 guard: the guards' sense (8257F620) and target step
// (82580508) ignore him, the hit handler (82549040, co-op: friend(victim, attacker)) cancels hits between him and the
// guards, and he and the agent hurt each other (the agent's auto-aim filter 824EF8B8 takes him as a target). By
// default no guard is touched (pdz_counterop_take_guard off): a spot is a free setup position. When player 2 dies,
// Rare's co-op revive (824573A8: no prompt, it is the un-die: pad move, restore, get-up animation with an
// invulnerability timer) is held 2.5 s, then run with player 2 out of the world until Rare's get-up is over (death
// state +2584 = 0, +5928 clear, participant +928 <= 0; 8 s cap), then the next spot, facing the agent (825AD3B0).
// He can't pick up items (Rare's can-pick-up test 82586238 answers no for him). The agent down = Rare's co-op game
// over (82E426F8 |= 2, 8222FF58), as Rare does when every human is down; the agent is never revived. Stage 1: Rare's
// setup filter answers with the counter-op kind bit 0x40 instead of co-op's 0x20
// (sub_82279928's hook in cut_scenarios.cpp calls PdzCounterOpFilter), so Rare's own counter-op spawn set is used.
//
// Guard spots (logs 310-312): a level's guards are all built at load; the ones a level script shows later are
// hidden through Rare's script despawn (825818E0: out of the world list and physics, +6272 bit 0x10), so the world
// list holds only the live ones (the Jungle: 5-6 of 75 at the start). Rare's own script lookup by id (821794A8 ->
// 82178FF8) walks the object registry at [[game +28] +20] (+676 / +680: groups, each with a list at +16 / +20 of
// nodes {object, _, next}), which holds hidden objects too; the spots are enumerated from it (and the world list,
// as a cross-check). A spot is an SP guard (vtable 820C45A0), alive, not taken, eligible by its setup (character
// +84): the mission's id list (section 4), or the no-id rule (team 0 or a template clone, a combat type 48 and up
// that is not a non-combatant, a gun in hand, and the id not on the mission's denylist of named / scripted / boss /
// hostage / vehicle-crew characters: the study's excluded.md). Dormant (hidden) spots come first, then live guards;
// at least 15 m from the agent and off his screen when any qualify, relaxed step by step (logged); one of the three
// nearest qualifying at random. Player 2 is moved there (Rare's move 825AD148; a dormant guard's setup position +4,
// a live guard's +48), given the guard's gun type (Rare's give 8254C378, then full reserves through Rare's ammo
// adders 82583F80 / 82584118, which clamp to each type's maximum), and the guard is hidden (825818E0) and kept
// hidden if a script shows it later. "Guards left" = eligible spots not taken (dormant + live); after a death,
// 20 s with none = out of guards (player 2 spectates, the mission goes on). That is pdz_counterop_take_guard (off by
// default): a hidden guard is not a death (825818E0 only sets +6272 0x10 and takes it out of the world, physics and
// team lists), so a script waiting for its group would wait, and a re-hidden wave guard never arrives. Default: no
// guard is hidden or re-hidden; a spot is the setup position (+4) of an eligible guard, live (its area is active)
// first, then dead (a cleared area), then dormant, free when no character is within 1.2 m and not the last life's
// spot; the gun is that guard's (else the last life's), the health a fresh guard's (setup +256 ratio 1).
//
// Look (log 312: an SP guard body on the live human crashed Rare's weapon / animation update; log 311 before it was
// the give's ammo arguments): player 2 is BUILT as one of the mission's guards (body and head) the way Rare builds a
// player: the SP player factory 825B3D58 builds the co-op partner from his setup (+52 body, +56 head) and, in co-op,
// only while player 1 has no next human in Rare's list (+6792, 825B3E00); the hook on it (pdz_counterop_guard_look)
// writes the chosen body and head ids into the setup for the duration of Rare's call and restores them, and Rare's own
// human constructor (825A6B20) wires everything from them: body entry 82278788 -> +5264, 82414070(entry, 1) unless
// +68 is set, instance 82410A60 -> +72, 82546340; head entry 822787E0 -> +5268, 82414618 unless +12 is set, instance
// 82411048 on the body -> +4104, the hair bones -> +4852 / +4856 (-1 is normal for a helmeted head); the animator
// from the body entry's +64 (825464D0 -> 821E82A0); the face set from the setup's head id (8262C060 -> 8262C890).
// Body / head ids are the level's list numbering (= a guard setup's +244 / +248; log 313). The candidates are a
// per-mission table of the placed guards' (body, head) pairs, most common first (decoded from each level's setups;
// Datacore and Arena have none: the co-op look). Each is checked with Rare's own lookup (82413FD8, null instead of
// a crash) before anything is requested: the body in the list, its animation set (+64, from its +12 name at level
// load, 822788D8 -> 821EE240) one Rare builds humans with (the male "henchman" set or the female "jo" set), its
// model (+4) and ragdoll (+32, Jack's fallback) and their +8 (82414070 reads both unchecked), and the head in the
// head list with its model and +8 (82411048 reads it unchecked). Player hands: an SP guard body has none (+16 =
// "_noname_", and the loader's fallback, the trooper hands, ships only in Trinity Escape), so 82414070 stores 0 at
// +20 and the first-person hands (825B43E0, run when his participant attaches, 825A99D8) are never made: log 312's
// null hands. The chosen body's +20 then gets the co-op body's own player hands (the ones that level ships for
// him), before Rare's call when the body is already loaded and after it otherwise (the constructor's loader writes
// +20); nothing but 825B43E0 reads +20 (guards never do). Any check failing = the next pair; a head failing = the
// co-op head; nothing usable = the co-op look with no requests at all. Every step is a 'pdz_counterop: look:' line.
//
// Health (the N64 rule, player_spawn_anti: health = (host max damage - host damage) * 0.125, capped at a full bar,
// where a basic guard has 4 damage points and the bar is 1.0: a full guard = half a bar): player 2's health and max
// (participant +980, what the human's health getter 825A9408 returns as the max) = (the guard's health / its spawn
// health) x his co-op max x pdz_counterop_health_scale (0.5), floor 25. An NPC has no max-health field (its getter
// 8252D658 returns +120 for SP guards), so the spawn health is the setup's +256 (the value the guard constructor
// copies to +120; 100 for every placed Jungle guard but the one-hit opener 'sta1' = 1.0, log 312). Armour (+124)
// stays 0.
//
// Kept: player 2 out of the world (hidden, locked) and spectating while waiting (his camera follows the agent),
// cutscene handling (no picks during one), agent down = mission failed, team bits re-applied when reset.
// Proof lines: 'hit:' (82549040 probe: who hit whom, health before / after), 'pickup blocked', 'get-up', 'spawn (f)'
// (facing), 'the agent is down'. Not done (known gaps): level triggers / objectives / key items that are not
// weapon-type pickups still accept him, props he shoots still take damage, no HUD.
// Menu (counter_op_menu.cpp, pdz_counterop_menu): MISSION MODE's COUNTER OPERATIVE entry arms the mode for the
// local co-op games that follow (On() = pdz_counterop || PdzCounterOpArmed()); Datacore / Arena are refused there;
// the debriefing header names the result.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

#include "guest_function.h"
#include "objective_bots.h"
#include "render/game_pace.h" // SpLevelName

REXCVAR_DEFINE_BOOL(pdz_counterop, false, "Gameplay",
                    "Counter-Operative (docs/COUNTEROP_STUDY.md; off by default): in a split-screen co-op mission, "
                    "player 2 plays a guard against the agent (player 1). He spawns at a free guard spot (a live "
                    "guard's setup position first, away from the agent and off his screen when possible; no guard "
                    "is removed), with its gun, facing the agent, and the guards treat him as one of theirs; when "
                    "he dies he spawns at another spot once Rare's get-up is over, out of sight (no revive at the "
                    "pad); he can't pick up items. The agent down = mission failed (never revived). Datacore and "
                    "Arena are not supported (no placed guards). Set it before starting the mission (Rare's "
                    "counter-op spawn set is chosen at level load). One 'pdz_counterop:' line per event")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_counterop_guard_look, true, "Gameplay",
                    "Counter-Operative: build player 2 with one of the mission's guard bodies and heads (chosen when "
                    "the level loads, through Rare's own player factory; first-person hands are the co-op partner's); "
                    "off = the co-op partner's look")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(pdz_counterop_health_scale, 0.5, "Gameplay",
                      "Counter-Operative: player 2's health and max health on each spawn = (the guard's health / its "
                      "spawn health) x the player's max x this, floor 25. Default 0.5 = N64 Perfect Dark's rule "
                      "(0.125 per damage point of a 4-point guard: a full guard gives half a bar)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_counterop_take_guard, true, "Gameplay",
                    "Counter-Operative, N64's rule (default on): each spawn REPLACES one of the level's guards (one of "
                    "the nearest to the agent, off-screen and calm first): that guard is taken out of the level "
                    "(Rare's script despawn), player 2 appears where it stood with its gun and its share of health, "
                    "and when player 2 dies the guard counts as killed (Rare's death path), so the level's counts and "
                    "scripts move on as in co-op. Player 2 is never an extra guard; with no eligible guard left he "
                    "spectates. Off: he spawns at a free guard setup position and no guard is touched")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_counterop_clones, true, "Gameplay",
                    "Counter-Operative (default on): missions whose guards are all runtime clones of spawn templates "
                    "(Datacore, Arena: no placed guards, so no eligible id list) are played too: player 2 replaces the "
                    "clones the no-id rule accepts (a combat type with a gun in hand), and since their waves have no "
                    "fixed count he is never out for good there: with no clone in play he spectates until the next one "
                    "spawns. Off = those missions are refused on the menu and play as plain co-op (0.9.3 behaviour)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_counterop_block_pickups, true, "Gameplay",
                    "Counter-Operative: player 2 can't pick up weapons, ammo or other weapon-type items (Rare's "
                    "can-pick-up test answers no for him); his own guns come from his spots")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_counterop_fix_out_freeze, true, "Gameplay",
                    "Counter-Operative (default on): while player 2 is out of the world his view follows the agent "
                    "through Rare's camera update, run once more for his camera. That extra call gets Rare's own "
                    "frame step (and his camera's three sway angles are put back in range if they ever left it). Off = "
                    "the old call, which passed on whatever the agent's camera update left in the step register: an "
                    "infinite or huge step made the angles infinite and Rare's angle wrap loop (82214E20) never end, "
                    "so the game froze (Nightclub start, the Jungle after Trinity Escape)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(pdz_counterop_test_max_lives, 0, "Testing",
                     "TESTING TOOL (counter-op): cap player 2's lives (replaced guards) at this many, so a test reaches "
                     "'no eligible guard left' quickly; 0 = off (the mission's own count)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(pdz_counterop_test_kill_p2, 0, "Testing",
                     "TESTING TOOL (counter-op): each time this goes up by one (when player 2 is alive and in the "
                     "world), player 2 is put 3 m in front of the agent, facing him, with 0.5 health, so the agent's "
                     "next hit (an input script's trigger presses) kills him through Rare's own death path")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_counterop_test_kill_agent, false, "Testing",
                    "TESTING TOOL (counter-op): when this turns on, the agent (player 1) gets 0.5 health once, so the "
                    "next hit kills him through Rare's own death path (the end of a counter-op game; run without "
                    "pdz_test_god_mode)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

bool PdzCounterOpArmed(); // counter_op_menu.cpp: chosen in MISSION MODE

REX_EXTERN(__imp__sub_8222F740); // co-op per-participant death states (every frame in a level, from 8222D4A8)
REX_EXTERN(__imp__sub_8222FD98); // co-op: game over? (every human down)
REX_EXTERN(__imp__sub_824573A8); // co-op revive (participant)
REX_EXTERN(__imp__sub_825B3D58); // the SP player factory: (setup) -> human (player 2's call once player 1 exists)
REX_EXTERN(sub_8222FF58);        // game over
REX_EXTERN(sub_825AD148);        // move a character (character, &position, 0)
REX_EXTERN(sub_8252DE88);        // guard vtable +0x2E4: Kill(guard, killer, death type, apply damage) - level scripts
                                 // call it on live guards (8231AD18, 821DBA2C, 822FFA78)
REX_EXTERN(sub_825AD0E0);        // Rare's script teleport (character, &position) -> 825AD148(c, pos, 1)
REX_EXTERN(sub_82550B08);        // guard vtable +0x2E8: die (stops weapon, animation and AI parts)
REX_EXTERN(sub_8252D6A8);        // guard vtable +0x158: set health (guard, f1, r5 0)
REX_EXTERN(sub_825AD3B0);        // a human's look angles (character, f1 pitch, f2 yaw; radians, forward = (sin, cos) in
                                 // x / z): what Rare's spawn 825ADF80 calls after its move (825AE2A4 / 825AE2B4)
REX_EXTERN(__imp__sub_82586238); // can this character pick this item up? (item, character, four out bytes)
REX_EXTERN(__imp__sub_82549040); // a character's hit handler (victim, hit, attacker, ...): friend test, damage
REX_EXTERN(sub_8254C378);        // give an item (character, type, 0, 0, 0, 0)
REX_EXTERN(sub_82583F80);        // weapon: add primary reserve ammo (item, amount, 1 add), clamped to the type's max
REX_EXTERN(sub_82584118);        // weapon: add secondary reserve ammo (needs the entry's +4 definition)
REX_EXTERN(sub_825818E0);        // hide / restore a character (character, 1 hide): Rare's script despawn, guard vtable +832
// The generic steps of sub_825818E0's hide / restore paths (no class-specific calls), used on player 2 (a human
// has no hide of its own: his vtable +832 is an unrelated getter).
REX_EXTERN(sub_82189B08); // hide: (character)
REX_EXTERN(sub_82517600); // physics out: (object)
REX_EXTERN(sub_82187E98); // team lists out: (character)
REX_EXTERN(sub_82527D78); // world out: (world, character)
REX_EXTERN(sub_82156E20); // (character, 1)
REX_EXTERN(sub_82187D18); // team lists in: (character)
REX_EXTERN(sub_82517530); // physics in: (object, 0)
REX_EXTERN(sub_82527C90); // world in: (world, character)
REX_EXTERN(sub_82156EF0); // (character, 1)
REX_EXTERN(sub_8254D8F0); // a player's action lock: (character, 64-bit mask), counters at +5785
REX_EXTERN(sub_8254D968); // its unlock
REX_EXTERN(__imp__sub_82214C48); // the gameplay camera's update (camera mode, target character, f1)
// The look (what the player factory and the human constructor use).
REX_EXTERN(sub_82413FD8); // an asset by name: (name, fallback name or 0) -> asset or 0 (82414070's own lookup;
                          // "_noname_" -> 0; a missing name -> the fallback's asset, if any)

namespace
{
  constexpr uint32_t kGame = 0x82D6BFD8;            // -> +0 game state (+8 lists, +28 registry, +40 game type), +24 world, +68 cutscene
  constexpr uint32_t kStateInLevel = 21;
  constexpr uint32_t kWorldFirst = 43212, kWorldEnd = 43216; // world: object nodes (+4 object, +12 next)
  // Rare's object registry (82178FF8): [game +28] +20 -> groups from +676 to +680 (next at +36), each with a node
  // list at +4 +16 .. +4 +20 (node: +0 object, +8 next). Hidden objects are still in it.
  constexpr uint32_t kRegistry = 28, kRegistryBase = 20, kGroupFirst = 676, kGroupEnd = 680, kGroupNext = 36;
  constexpr uint32_t kGroupListFirst = 4 + 16, kGroupListEnd = 4 + 20, kNodeObject = 0, kNodeNext = 8;
  constexpr uint32_t kParticipantList = 0x82E34224; // -> participant (+4 next)
  constexpr uint32_t kPlayerOne = 0x82E3C3E4;       // player 1's character
  constexpr uint32_t kLevelState = 0x82E426F8;      // byte: 0x1 completed, 0x2 failed, 0x40 checkpoint
  constexpr uint32_t kGuardVtable = 0x820C45A0, kHumanVtable = 0x820C7DE0;
  constexpr uint32_t kFilterTable = 0x8208ABC8;     // setup filter masks (+49 index, 8 bytes each, mask at +4)
  // participant
  constexpr uint32_t kPartCharacter = 964, kPartDead = 992, kPartDeathState = 2584, kPartUser = 1680;
  constexpr uint32_t kPartMaxHealth = 980; // the human's max health (825A9408 with a nonzero flag)
  // character
  constexpr uint32_t kPosition = 48, kSetup = 84, kHealth = 120, kArmour = 124, kTeamBits = 128, kAllies = 130,
                     kEnemies = 132;
  constexpr uint32_t kHand0 = 4076, kHand1 = 4080, kItemType = 12, kBetrayal = 6276, kViewYaw = 0x1670;
  constexpr uint32_t kHidden = 6272;   // bit 0x10: hidden by sub_825818E0 (a level script's dormant guards too)
  constexpr uint32_t kBodyId = 5280, kHeadId = 5284, kParticipantOf = 6560; // both classes: the ids; the human's participant
  // setup record (268 bytes for a character; the co-op partner's player setup is a 68-byte record with the same
  // +52 body / +56 head)
  // +52 is a pointer to the 4-character id string (the file holds an offset into its string tail; log 306).
  constexpr uint32_t kSetupPosition = 4, kSetupFilter = 49, kSetupId = 52, kSetupTeam = 56, kSetupType = 244,
                     kSetupHealth = 256;
  constexpr uint32_t kDescBody = 52, kDescHead = 56; // the player setup's ids (825B3D58 / 825A6B20)
  constexpr uint8_t kTeamTemplate = 8; // a spawn template: the guard is a runtime clone of it
  // Body / head list entries (game +8 -> +0 body list, +12 head list; list: +0 entries, +8 count; entry +0 = id).
  constexpr uint32_t kListsBody = 0, kListsHead = 12, kListEntries = 0, kListCount = 8;
  constexpr uint32_t kBodyModelRef = 4, kBodyHandsRef = 16, kBodyThirdRef = 32, kBodyHands = 20, kBodyAnimSet = 64,
                     kBodyAssets = 68;
  // 82414070's FALLBACK names for its lookups (82413FD8's second argument, used when the first name is missing):
  // the model (+4 -> +68, fallback the placeholder body), the hands (+16 -> +20, only with flag 1 = a human; flag 0
  // stores 0) and the ragdoll (+32 -> +36, fallback Jack's ragdoll), whose +8 it reads without a check, as it reads
  // the model's +8 (log 313's crash). 82414070 returns at once when +68 is set (the body already loaded).
  // (the model's fallback, 0x820B601C, is not used: the probe looks the model up strictly)
  constexpr uint32_t kHandsFallback = 0x820B604C, kRagdollFallback = 0x820B6078;
  constexpr uint32_t kPlaceholderBody = 86; // Rare's fallback body (82278788): never a candidate
  constexpr uint32_t kBodyAnimName = 12;     // the animation set's name (822788D8 resolves it into +64)
  constexpr uint32_t kHeadModelRef = 4, kHeadAssets = 12; // head entry: model name, model (82414618)
  // Rare's human list: head [82E3C3E4], +6792 next, +6796 previous (unlinked in 825A6F88). In co-op 825B3D58 builds
  // a human only while player 1's next is 0, i.e. player 2's call.
  constexpr uint32_t kP1Partner = 6792;
  constexpr uint32_t kBodyEntryOf = 5264, kHeadEntryOf = 5268; // a human's list entries (825A6B20)
  constexpr uint32_t kFirstPersonHands = 7796; // the two first-person hands instances (+7796 / +7800, 825B43E0)
  // The two animation sets Rare builds humans with: the male set (Jack, Jonathan, the CI partner, multiplayer men;
  // every male guard body) and the female set (Jo, Chandra; the female guard bodies).
  constexpr const char *kHumanAnimSets[] = {"aid_xml_test_henchmananims", "aid_xml_test_joanims"};
  // The gameplay camera mode (third_person.cpp, same in TU3): after its update, the camera position, the look point
  // and the eye.
  constexpr uint32_t kCamPosition = 4, kCamLook = 16, kCamEye = 188;
  constexpr float kSpectateBack = 120.0f, kSpectateUp = 30.0f; // cm: behind and above the agent's eye
  constexpr uint16_t kP2Team = 0x0001, kP2Enemies = 0x0002; // study section 5 (b)
  constexpr uint16_t kP2Allies = 0xFFFF; // a team-0 guard's allies (8252BD74): he answers the friend test as one
  // Rare's get-up after a co-op revive (824573A8 -> participant method 82474338 -> 824578D8 starts it, the character
  // update 8254CCC8..8254CDBC counts it down and ends it with 82457B40): the character's flag +5928 and timer +5924,
  // the participant's invulnerability +928 (the hit handler reads it at 8254989C; the update counts it down).
  constexpr uint32_t kGetupFlag = 5928, kGetupTimer = 5924, kPartInvulnerable = 928;
  constexpr double kGetupCapSeconds = 8.0; // the get-up wait's cap (Rare's update may not run while he is out)
  constexpr float kFreeRadius = 120.0f;    // cm: a spot with any character this close is taken
  constexpr double kHideWaitSeconds = 3.0; // the first hide waits this long at most for player 2's camera object
                                           // (Jungle: noted after 1.0 s; Temple: not within 1 s)
  constexpr double kCalmOnlySeconds = 10.0; // take: a search takes only calm guards for this long, then any
  constexpr uint32_t kHitLogFirst = 24, kHitLogEvery = 50; // the hit probe's rate limit
  constexpr double kNextGuardSeconds = 2.5;
  constexpr double kRetrySeconds = 1.0;       // a failed pick is tried again this often
  constexpr double kOutOfGuardsSeconds = 20.0; // after a death: no spot for this long = out of guards
  constexpr double kFirstPickSeconds = 3.0;   // the first pick waits this long (the opening cutscene starts in it)
  constexpr uint64_t kLockAll = ~0ull;        // every action bit of the player's lock
  constexpr uint32_t kChildFirst = 88, kChildNext = 96; // a character's attached objects
  constexpr float kMinSpawnDistance = 1500.0f; // cm: a spot this far from the agent is preferred
  constexpr float kMinHealth = 25.0f;          // the floor of the health rule
  constexpr uint32_t kFullAmmo = 9999;         // added to each reserve by Rare's adders, clamped to the type's max
  constexpr uint32_t kWeaponList = 48, kWeaponListCount = 24, kWeaponEntryBytes = 2196, kWeaponDefType = 1144,
                     kWeaponEntry = 1248, kWeaponEntryDef = 0, kWeaponEntrySecondaryDef = 4;

  // Eligible guard ids per mission (the study's eligible table, scratchpad counterop_C\eligible.tsv; names as
  // pdz_sp_60_levels has them).
  struct MissionIds
  {
    const char *mission;
    const char *ids;
  };
// Eligible and denied ids per mission: the scope study's lists, with every id the mission's level script uses one by one (looked up by id and then handled singly, at level start or later) moved to denied (the F_ids study, 8 Nov 2026: static analysis of the TU3 level scripts). Ids the scripts reach only through group lists stay eligible: a replaced guard dies through Rare's kill when player 2 dies, so the groups' all-dead checks still resolve.
constexpr MissionIds kEligible[] = {
    {"nightclub", "s20a,s19b,s19a,s20b,s20c,s01a,s20d,s21d,s21b,s21c,s05d,s21a,s22d,s22a,s22c,s22b,s11b,s11c,s10c,s03b,s11d,s11e,s08c,s08b,s08a,s09a,s09d,s09c,s09b,s05a,s05b,s05c,s03a,s12b,s12c,s12d,s16a,s16b"}, // 38 ids (removed: s01b,s02c,s06a)
    {"subway", "lop2,saf5,arm4,arp2,arp3,saf1,armp,arp4,saf3,saf4,war1,saf2,arm3,lorp,zsb4,fac1,bik3,bik4,bik2,bik5,bik1,fac2,arm2,arm1,zsb5,zsb7,zsb6,zsb3,fac3,zsb2,zsb1,zsb8,s18b,18-1,out3,out4,out1,out2,inn2,inn3,inn4,inn1,inn6,sin2,sin3,inn5,zsba,sin4,zsb9"}, // 49 ids (removed: lor1,s16a,s16b,16-3)
    {"rooftops", "s141,s142,s133,s143,top2,s131,s123,gf32,gf33,top1,s112,s111,top3,s122,s121,s113,gf31,mff2,tff1,tff2,s132,bfb4,mff1,gft1,gft2,gft3,ggo1,ggo2,ggo3,s194,s193,bff2,bff1,17-3,17-2,s17a,s192,s191,bff4,bfm3,bfb3,bfm2,bfm1"}, // 43 ids (removed: s20a,s15b,s15a,bfb1,bfb2)
    {"mansion", "tow2,tow4,tow3,pat1,pat2,fre4,fre5,pat3,fre2,fre1,fre3,rsp1,lsp1,cpt1,s06c,wpt1,mgp1,ocg1,wpt2"}, // 19 ids (removed: mgg1,tow1,end1,ocp1)
    {"laboratory", "s3g7,s3g4,s3g5,s3g8,s1m3,s1m2,s1m4,s1m5,s3g3,s3gb,s3g2,s3gc,s11b,s11c,s11a,s11d,s9g2,s9g5,s9g6,s9g3,s9g4,s9g7,s9g8,s6s3,s6s2,s6s1,s9p2,s9p1,s9g1,s9p3,s7s4,s7s2,s7s1,s7s3"}, // 34 ids (removed: s3g1,s1m1,s6s5,s6s4)
    {"river", "s3g5,s3g4,s5g1,s3g1,s2g5,s3g3,s3g2,s4g5,s4g1,s4g3,s4g4,s4g2,s2g4,s1g1,s20b,s20a,s19b,s19a,s19d,s19c,s20c,s2g1,s1g5,s2g3,s2g2,s1g2,s20d,s1g4,s1g3,s16g,s16h,s16e,s16f,s16i,s17b,s17c,s16j,s17a,s16d,s15g,s15h,s15e,s15f,s15i,s16b,s16c,s15j,s16a,s17f,s17g,s17d,s17e,s17h,s17i,s17j,s8g3,s8g2,s9g1,s10a,s10b,s9g2,s9g3,s6g3,s6g4,s6g1,s6g2,s6g5,s6kc,s6g6,s6g7,s10c,s13a,s13b,s12f,s12g,s15c,s15d,s15a,s15b,s12e,s11c,s11d,s11b,s12c,s12d,s12a,s12b"}, // 87 ids (removed: s8g4)
    {"trinity1", "s03e,s07l,s03d,s14c,s14d,s14a,s14b,s14e,s14h,s14i,s14f,s14g,s14j,s01d,s01c,s03b,s03c,s03a,s06e,s06d,s14k,s14l,s06c,s06b,s18a,s18b,s18c,s08a,s07j,s08b,s17c,s15b,s16c,s17a,s17b,s16a,s16b,s07i,s06a,s07k,s07a,s07h,s07n,s07m,s07f,s07g,s07c,s07b,s07e,s07d"}, // 50 ids (removed: s01b,s15a,s07z)
    {"trinity2", "s07g,s07h,s08a,s01c,s01d,s01e,s07f,s07b,s07a,s06h,s07e,s07d,s07c,s08e,s08f,s09a,s08b,s08c,s08d,s09b,s01a,s01b,s09c,s03f,s04a,s03d,s03e,s04d,s04e,s04b,s04c,s01f,s03b,s03c,s03a,s04f,s06b,s06c,s05h,s06a,s06f,s06g,s06d,s06e,s05b,s05c,s05a,s05f,s05g,s05d,s05e,s09d,s12d,s12c,s12b,s12e,s17g,s17h,s12a,s18a,s18b,s17j,s11f,s11e,s17e,s14b,s14c,s12h,s12i,s14a,s13a,s12j,s14d,s17b,s17c,s17d,s17a,s12g,s12f,s10f,s10e,s10d,s11a,s18d,s18c,s11b,s10c,s09g,s09f,s09e,s09h,s10b,s10a,s09i,s18e,s11c,s11d"}, // 97 ids (removed: s17f,s17i)
    {"jungle", "s44c,s10e,s10f,s10c,s09d,s10d,s11g,s11f,s11h,s11j,s11i,s11b,s11a,s11c,s11e,s11d,s09c,s6e1,s5l6,s6e2,s6e4,s6e3,s5l2,s5l1,s5l3,s5l5,s5l4,s7c1,s7c8,s7c7,s09b,s09a,s7c3,s7c2,s7c4,s7c6,s7c5,s02a,s02i,s02g,s03d,s03e,s03c,s03a,s03b,s02b,s02c,s44b,s11p,s44a,s43b,s43c,s11l,s11k,s11m,s11o,s11n,s41a,s41b,s41c,s03f,s03g,s03h,s42c,s43a,s42a,s42b"}, // 67 ids (removed: s10b,s10a,s02j,s02h,sta1,s02e,s02f,s02d)
    {"temple", "s2b1,s1a1,s1a2,s2b4,s2b3,s2b2,s08c,s08e,s08d,s06g,s06f,s06h,s08f,s10a,s09f,s10c,s10b,s09e,s09b,s09a,s09d,s09c,s4p3,s4p4,s4p2,s3b1,s4p1,s5c1,s06c,s06d,s06e,s06b,s5c2,s5c3"}, // 34 ids (removed: s4b2,s4b1)
    {"outpost", "s14e,s14f,s14g,s14b,s14c,s14d,s16a,s16b,s17a,s14h,s15a,s15b,s14a,s11a,s11b,s12a,s10a,s10b,s10c,s13c,s13d,s13e,s12b,s13a,s13b,s03a,s03b,s02b,s02c,s03c,s03d,s01c,s01d,s01a,s01b,s01e,s01h,s02a,s01f,s01g,s08f,s08g,s08d,s08e,s08h,s09c,s09d,s09a,s09b,s07a,s07b,s08b,s08c,s07d,s08a,s17b,s27k,s34d,s34e,s34f,s27a,s27b,s27c,s26a,s34c,s34b,s27h,s27i,s27g,s27d,s27e,s27f,s19d,s19c,s19b,s20a,s21c,s21b,s21a,s17e,s17d,s17c,s18a,s19a,s18c,s18b,s24b,s24a,s23e,s34a,s24c,s22c,s22b,s22a,s23a,s23d,s23c,s23b"}, // 98 ids (removed: s27l)
    {"bridge", "s22b,s22c,s20b,s21a,s22a,s40b,s40a,s40d,s40c,s20a,s14a,s12b,s15b,s15a,s10a,s12a,s11a,s16a,s18c,s18b,s19a,s18d,s17a,s16b,s18a,s17b,s26d,s26c,s26b,s27a,s27c,s27b,s26a,s23d,s23c,s22d,s23a,s23b,s30a,s09b,s44j,s44i,s45c,s45b,s45a,s44h,s44c,s44b,s44a,s44d,s44g,s44f,s44e,s45f,s45e,s45d,s45g,s45j,s45i,s45h,s04b,s05a,s05b,s04a,s03b,s03c,s03d,s06a,s08a,s08b,s09a,s07b,s06b,s06c,s07a,s02c,s02d,s03a,s02a,s02b"}, // 80 ids (removed: s25a,s44k,s44l)
};

constexpr MissionIds kDenied[] = {
    {"datacore", "Grd1,L02S,Al01,tut0,tut1,jack"},
    {"nightclub", "rin1,s18c,s25a,s25b,sc31,s18b,sc38,sc33,sc36,s02d,sc32,sc30,s11a,s10d,rin2,s18a,s10a,s10b,s04a,r2bu,rin3,sc10,sc11,sc14,sc09,sc06,sc07,sc08,sc21,sc22,sc23,sc20,sc15,sc17,sc19,s12a,s02b,s02a,s02e,sc03,sc04,sc05,sc02,sc01,s01b,s02c,s06a"}, // +3 from the scripts: s01b,s02c,s06a
    {"subway", "phon,sub1,sub4,sub5,sub2,sub3,was1,sub6,zieg,jack,s19b,s20a,sak2,s20c,s20b,s17c,s17b,s19a,s17a,sak1,dspa,16-3,lor1,s16a,s16b"}, // +4 from the scripts: 16-3,lor1,s16a,s16b
    {"rooftops", "s16a,zip1,krad,sni2,sni1,sni3,bfb1,bfb2,jack,s15a,s15b,s20a"}, // +6 from the scripts: bfb1,bfb2,jack,s15a,s15b,s20a
    {"mansion", "ter1,ter2,ter3,civ2,civ3,mai1,ter4,ter8,chan,ter5,ter6,ter7,s4s2,s5s2,rdo1,s5s1,bik1,s4s1,civ1,end2,rdo2,end1,mgg1,ocp1,tow1"}, // +4 from the scripts: end1,mgg1,ocp1,tow1
    {"laboratory", "s2s2,s2s1,s4l4,s4l5,s4l3,s4l1,s4l2,s101,jack,s12a,s12b,s5l1,s5l3,s5l2,s1m1,s3g1,s6s4,s6s5"}, // +4 from the scripts: s1m1,s3g1,s6s4,s6s5
    {"river", "s22a,s21f,s23a,s22b,s21e,s21c,s21d,s21a,s21b,s23b,s24b,s24a,s8g1,s7g1,s14a,s11a,jack,s8g4"}, // +2 from the scripts: jack,s8g4
    {"trinity1", "s02a,s11b,s11c,s11a,s10h,s11d,s11f,s11g,s10g,s11e,s02b,s02c,s01a,clay,drca,virg,radd,s10a,s09h,radr,s09b,s09a,s10c,s10d,s10e,s10b,s10f,s09g,s09f,s09c,s09d,s09e,rada,s01b,s07z,s15a"}, // +3 from the scripts: s01b,s07z,s15a
    {"trinity2", "s30b,s30a,s02a,s02b,s16a,s21a,s15a,s21c,s31c,s31b,s21b,s31a,s17f,s17i,s30p"}, // +3 from the scripts: s17f,s17i,s30p
    {"jungle", "s31p,s08a,s01a,s12a,s12b,s02d,s02e,s02f,s02h,s02j,s10a,s10b,s30p,sta1"}, // +9 from the scripts: s02d,s02e,s02f,s02h,s02j,s10a,s10b,s30p,sta1
    {"temple", "s08b,s08a,s07a,s06a,s4b1,s4b2"}, // +2 from the scripts: s4b1,s4b2
    {"outpost", "s04b,s04c,s04a,s04d,s04e,s07c,s31c,s32a,s32b,s31b,s31a,s30p,bff1,s30b,s25a,s27j,s28a,s28b,s27l,s30a"}, // +2 from the scripts: s27l,s30a
    {"bridge", "s30m,s35i,s30c,s10b,s28a,s24a,s29a,s35d,s35c,s35b,s35e,s35h,s35g,s35f,s35a,s31a,s29b,s32a,s33b,s33a,s32b,s47b,s47a,s46a,s47c,s48b,s48a,s47d,s01d,s01e,s01f,s01c,s01j,s01a,s01b,s01g,s25a,s44k,s44l"}, // +3 from the scripts: s25a,s44k,s44l
    {"arena", "Grd1,zlsp"},
};

  // The placed guards' looks per mission, most common first: (body = setup +244, head = setup +248), from each
  // level's setups (team 0, combat type, under the counter-op filter, not denied; the head is that body's most
  // common one; counts in the comments). No row (Datacore, Arena) = the co-op look.
  struct LookPair
  {
    uint32_t body, head;
  };
  struct MissionLook
  {
    const char *mission;
    LookPair pairs[5];
  };
  constexpr MissionLook kLook[] = {
      {"nightclub", {{48, 3}, {62, 53}}},                           // gangster 30, warehouse worker 9
      {"subway", {{50, 9}, {62, 54}, {75, 90}, {51, 8}, {48, 95}}}, // henchman 15, worker 13, biker 7, henchman 2 7
      {"rooftops", {{50, 10}, {51, 9}}},                            // henchman 34, henchman 2 14
      {"mansion", {{52, 14}, {53, 14}}},                            // Private Force A 17, B 6
      {"laboratory", {{52, 14}, {53, 14}, {54, 21}}},               // Private Force A 21, B 12, security girl 6
      {"river", {{52, 14}, {53, 17}, {54, 22}}},                    // Private Force A 56, B 31, security girl 6
      {"trinity1", {{84, 73}, {69, 77}, {70, 79}, {67, 72}}},       // trooper 19, platform workers 15 / 11, guard 8
      {"trinity2", {{84, 71}, {69, 71}, {70, 76}, {67, 72}}},       // trooper 46, platform workers 36 / 9, guard 8
      {"jungle", {{56, 29}, {52, 14}}},                             // jungle trooper 62 (heads 29-34), Private Force 13
      {"temple", {{56, 29}, {84, 33}, {52, 14}}},                   // jungle trooper 20, trooper 9, Private Force 4
      {"outpost", {{57, 35}, {55, 35}}},                            // desert trooper 66, urban trooper 33
      {"bridge", {{57, 35}, {55, 35}, {84, 35}}},                   // desert trooper 39, urban trooper 35, trooper 5
  };

  uint32_t Load32(const uint8_t *base, uint32_t a)
  {
    const uint8_t *p = base + a;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }
  uint16_t Load16(const uint8_t *base, uint32_t a) { return uint16_t((base[a] << 8) | base[a + 1]); }
  void Store16(uint8_t *base, uint32_t a, uint16_t v)
  {
    base[a] = uint8_t(v >> 8);
    base[a + 1] = uint8_t(v);
  }
  void Store32(uint8_t *base, uint32_t a, uint32_t v)
  {
    base[a] = uint8_t(v >> 24);
    base[a + 1] = uint8_t(v >> 16);
    base[a + 2] = uint8_t(v >> 8);
    base[a + 3] = uint8_t(v);
  }
  float LoadFloat(const uint8_t *base, uint32_t a)
  {
    const uint32_t bits = Load32(base, a);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
  }
  void StoreFloat(uint8_t *base, uint32_t a, float f)
  {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    Store32(base, a, bits);
  }
  bool Ptr(uint32_t a) { return a >= 0x10000u && a < 0xF0000000u; }

  uint32_t Call(PPCContext &ctx, uint8_t *base, PPCFunc *fn, uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0,
                uint32_t r6 = 0, uint32_t r7 = 0, uint32_t r8 = 0)
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = r3;
    frame.ctx.r4.u64 = r4;
    frame.ctx.r5.u64 = r5;
    frame.ctx.r6.u64 = r6;
    frame.ctx.r7.u64 = r7;
    frame.ctx.r8.u64 = r8;
    fn(frame.ctx, base);
    return frame.ctx.r3.u32;
  }

  float Dist3(float ax, float ay, float az, float bx, float by, float bz)
  {
    const float dx = ax - bx, dy = ay - by, dz = az - bz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
  }

  // A guest C string (printable, at most max characters); "" when unreadable.
  std::string GuestString(const uint8_t *base, uint32_t p, size_t max = 63)
  {
    std::string out;
    for (size_t i = 0; Ptr(p) && i < max; ++i)
    {
      const char ch = char(base[p + i]);
      if (!ch)
      {
        break;
      }
      if (ch < 32 || ch > 126)
      {
        return "";
      }
      out += ch;
    }
    return out;
  }

  // The setup id: setup +52 points at its string (log 306). Rare's 82142B70 packs up to 4 characters; here the first
  // goes in the top byte, as the lists above are packed. 0 = unreadable (no pointer, empty or not printable).
  uint32_t SetupIdKey(const uint8_t *base, uint32_t setup, uint32_t *ptr_out = nullptr)
  {
    const uint32_t p = Ptr(setup) ? Load32(base, setup + kSetupId) : 0;
    if (ptr_out)
    {
      *ptr_out = p;
    }
    if (!Ptr(p))
    {
      return 0;
    }
    uint32_t key = 0;
    for (uint32_t i = 0; i < 4; ++i)
    {
      const uint8_t ch = base[p + i];
      if (!ch)
      {
        break;
      }
      if (ch < 0x21 || ch > 0x7E)
      {
        return 0;
      }
      key |= uint32_t(ch) << (24 - 8 * i);
    }
    return key;
  }

  std::string IdChars(uint32_t key)
  {
    std::string chars;
    for (int i = 24; i >= 0 && ((key >> i) & 0xFF); i -= 8)
    {
      chars += char((key >> i) & 0xFF);
    }
    return chars;
  }

  // An id for the log: the number (hex and decimal), then the characters read from its string.
  std::string IdLog(uint32_t key, uint32_t ptr)
  {
    return fmt::format("0x{:08X} / {} ('{}' from the string at {:08X})", key, key, IdChars(key), ptr);
  }

  void ParseIds(const char *list, std::unordered_set<uint32_t> &out)
  {
    const std::string ids = list;
    for (size_t i = 0; i + 4 <= ids.size(); i += 5)
    {
      out.insert((uint32_t(uint8_t(ids[i])) << 24) | (uint32_t(uint8_t(ids[i + 1])) << 16) |
                 (uint32_t(uint8_t(ids[i + 2])) << 8) | uint8_t(ids[i + 3]));
    }
  }

  // Non-combatants by character type (study section 4): scientists 58-61, party guests 71-74, club guests / staff
  // 79-83. Named / main characters are below 48.
  bool CombatType(uint32_t type)
  {
    return type >= 48 && type < 256 && !(type >= 58 && type <= 61) && !(type >= 71 && type <= 74) &&
           !(type >= 79 && type <= 83);
  }

  // Player 2's look, chosen by the factory hook at level load (before Start): kept apart from the per-level state.
  struct Look
  {
    bool decided = false, chosen = false;
    std::string mission, why;
    uint32_t coop_body = 0, coop_head = 0, body = 0, head = 0;
    // D15 (log 312's cause): the chosen body was loaded by Rare for a guard (82414070 with flag 0: no player hands,
    // entry +20 = 0) and the human constructor (825A6B20) skips the loader when +68 is set, so player 2 would get
    // null hands and Rare's first-person weapon / animation update crashes on the next frame. These hands (Rare's
    // own lookup, as the loader's flag-1 step) are written to the entry's +20 just before the factory call.
    // A guard body has no player hands at all (+16 "_noname_", the trooper fallback not shipped): fill_hands is then
    // the co-op body's, written before the call (body loaded) or after it (the constructor's loader writes +20).
    uint32_t fill_hands = 0, fill_entry = 0;
    std::string hands_why;
  } g_look;

  struct Spot
  {
    uint32_t character = 0;
    uint32_t setup = 0;
    uint32_t id = 0;      // the setup id as Rare's key packs it (4 characters, the first in the top byte); 0 = none
    uint32_t id_ptr = 0;  // where its string is
    uint32_t type = 0;    // character type / body id
    bool by_list = false; // eligible by the mission's id list (else by the no-id rule)
    const char *why = ""; // the no-id rule's reason
    bool dormant = false; // hidden (a level script shows it later)
    bool dead = false;    // the guard is dead (its setup position is a cleared area; no-take mode only)
    float x = 0.0f, y = 0.0f, z = 0.0f; // the spot: a dormant guard's setup position, a live guard's own (take
                                        // mode); every guard's setup position otherwise
    float distance = 0.0f;
    bool on_screen = false;
    // Calm: not perceiving a hostile this frame (+6268 bit 0x400000, set by the target step 82580508), never engaged
    // (+6252 bit 0x2) or shot (+6252 bit 0x80), no current target (+0x317C). The take prefers calm guards.
    bool calm = true;
  };

  struct State
  {
    bool started = false;
    bool unsupported = false;
    bool spectating = false;
    bool game_over_logged = false;
    bool in_cutscene = false;
    bool allow_revive = false;
    std::string mission;
    std::unordered_set<uint32_t> ids;    // this mission's eligible ids
    uint32_t rule_live = 0;              // the last scan's live, untaken candidates by the no-id rule (clone missions)
    std::unordered_set<uint32_t> denied; // this mission's denied ids (the no-id rule never takes them)
    uint32_t state_object = 0;
    uint32_t p1p = 0, p2p = 0, p1c = 0, p2c = 0;
    uint32_t revive_participant = 0;
    std::chrono::steady_clock::time_point revive_at{};
    int lives = 0;
    uint32_t team_fixes = 0;
    uint32_t last_weapon = ~0u;
    uint32_t scratch = 0; // guest vec3 for the move
    std::unordered_set<uint32_t> taken;
    std::unordered_map<uint32_t, uint32_t> rehidden; // a taken guard a script showed again: times re-hidden
    // Player 2 out of the world (hidden, locked) until he takes a spot, and the pick's retries.
    bool out = false;
    bool need_guard = false;
    bool after_death = false;
    std::chrono::steady_clock::time_point need_since{}, next_try{}, last_diag{};
    uint32_t diag_lines = 0, tries = 0;
    // Spectate camera while out (log 306): the agent's last camera (eye, look point), player 2's camera updates.
    bool agent_cam = false;
    float agent_eye[3] = {}, agent_look[3] = {};
    std::chrono::steady_clock::time_point out_since{};
    uint32_t p2_cam_updates = 0, p1_cam_updates = 0, spectate_writes = 0;
    bool spectate_reported = false;
    // Player 2's camera object (noted while he is in the world), driven from the agent's camera update while he is out
    // (Rare doesn't update it then); and the first hide waits for it (up to kHideWaitSeconds).
    uint32_t p2_mode = 0;
    bool in_spectate_call = false;
    // pdz_counterop_fix_out_freeze: the spectate calls made, the leftover step values that would have been passed
    // before the fix (not a plausible step: not finite, negative or over 0.25 s) and the largest, the angle resets.
    uint32_t spectate_calls = 0, bad_leftover = 0, angle_resets = 0;
    double max_leftover = 0.0;
    // N64 take: the guard this life replaced (killed through Rare's death path when player 2 dies), the listed ids
    // replaced or seen dead (the eligible-left count), and the kill waiting for the next tick.
    bool hide_if_no_spot = false; // after a revive: hide only if the first pick finds no spot
    bool cam_field_tried = false;  // the agent's camera object looked up in his character once
    bool p2_mode_logged = false;
    uint32_t health_clamps = 0;
    uint32_t cam_field = 0;
    uint32_t current_guard = 0, kill_guard = 0, kill_stage = 0;
    bool kill_ready = false; // the revive has moved player 2 away: the replaced guard's kill can start
    float kill_at[3] = {};
    std::unordered_set<uint32_t> taken_ids, dead_ids;
    bool hide_pending = false;
    std::chrono::steady_clock::time_point hide_pending_since{};
    // Health: the co-op max health, restored on a stop; what the last spawn set, checked once 5 s later.
    float coop_max_health = 0.0f;
    bool max_health_set = false;
    float set_health = 0.0f;
    std::chrono::steady_clock::time_point health_check_at{};
    bool health_check = false;
    bool registry_warned = false;
    // No-take mode: the last life's spot (not used twice in a row) and gun (when a spot's guard holds none).
    uint32_t last_setup = 0, last_gun = 0;
    // After Rare's revive: player 2 waits out of the world until Rare's get-up is over (no revive seen anywhere).
    bool wait_getup = false;
    std::chrono::steady_clock::time_point getup_since{};
    // Any revive asked for the dead agent (blocked).
    uint32_t agent_revives_blocked = 0;
    // Probes: hits between player 2 and anyone (82549040), pick-ups refused.
    uint32_t hit_lines = 0, pickups_blocked = 0;
  } g;

  // Setup filter counters (PdzCounterOpFilter, at level load; logged at the start).
  uint32_t g_filter_added = 0, g_filter_removed = 0, g_filter_seen = 0;
  std::mt19937 g_rng{0xC0DE0Bu};
  bool g_played = false; // the last level started as a supported counter-op game (the debriefing reads it)
  bool g_giving = false; // our give of player 2's gun is running (the pick-up block lets it through)

  // pdz_counterop forces the mode for every co-op game (tests); otherwise it runs when chosen in the menu.
  bool On() { return REXCVAR_GET(pdz_counterop) || PdzCounterOpArmed(); }

  // The level's game state object while in a co-op level, else 0.
  uint32_t CoopLevel(const uint8_t *base)
  {
    const uint32_t so = Load32(base, kGame);
    if (!Ptr(so) || Load32(base, so + 8) != kStateInLevel || Load32(base, so + 40) != 1)
    {
      return 0;
    }
    return so;
  }

  bool InCutscene(const uint8_t *base)
  {
    const uint32_t player = Load32(base, kGame + 68);
    return Ptr(player) && Load32(base, player + 12) == 3;
  }

  // Player 1 (the agent) and player 2 (the counter-op): the participants whose characters are humans; player 1's is
  // Rare's player one.
  void FindPlayers(const uint8_t *base, uint32_t &p1p, uint32_t &p1c, uint32_t &p2p, uint32_t &p2c)
  {
    p1p = p1c = p2p = p2c = 0;
    const uint32_t one = Load32(base, kPlayerOne);
    for (uint32_t p = Load32(base, kParticipantList), n = 0; Ptr(p) && n < 16; p = Load32(base, p + 4), ++n)
    {
      const uint32_t c = Load32(base, p + kPartCharacter);
      if (!Ptr(c) || Load32(base, c) != kHumanVtable)
      {
        continue;
      }
      if (c == one && !p1p)
      {
        p1p = p;
        p1c = c;
      }
      else if (!p2p)
      {
        p2p = p;
        p2c = c;
      }
    }
  }

  bool Alive(PPCContext &ctx, uint8_t *base, uint32_t c)
  {
    return Ptr(c) && LoadFloat(base, c + kHealth) > 0.0f && !objective_bots::CharacterDown(ctx, base, c);
  }

  // On the agent's screen (approximate): within 60 m and 50 degrees of his view yaw (+0x1670, degrees; forward =
  // (sin yaw, cos yaw) in x / z, the convention of Rare's setup yaws).
  bool OnAgentScreen(const uint8_t *base, uint32_t agent, float x, float z, float distance)
  {
    if (distance > 6000.0f)
    {
      return false;
    }
    const float yaw = LoadFloat(base, agent + kViewYaw) * 3.14159265f / 180.0f;
    const float dx = x - LoadFloat(base, agent + kPosition);
    const float dz = z - LoadFloat(base, agent + kPosition + 8);
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len < 1.0f)
    {
      return true;
    }
    const float dot = (dx * std::sin(yaw) + dz * std::cos(yaw)) / len;
    return dot > std::cos(50.0f * 3.14159265f / 180.0f);
  }

  uint32_t HandWeaponType(const uint8_t *base, uint32_t c)
  {
    for (const uint32_t hand : {kHand1, kHand0})
    {
      const uint32_t w = Load32(base, c + hand);
      if (Ptr(w))
      {
        return Load32(base, w + kItemType);
      }
    }
    return 0;
  }

  // The weapon type Rare's give (8254C378) takes (log 308: the hand item's +12 above is not it; give -> 824F4D28 ->
  // 82587338 -> 82582750 -> 825843D0 looks the type up in the level's weapon list with 824F2C10 and reads the result
  // without a check, so a type the list doesn't hold is a null read). The list: game +48 -> entries at +0 (2196 bytes
  // each; +0 the definition, whose +1144 is the type), count byte +24. A held weapon keeps its entry at +1248
  // (825843D0). Returns the type of the item in a hand when that entry is one of the list's, else 0 and why.
  uint32_t GiveableWeaponType(const uint8_t *base, uint32_t c, std::string &why)
  {
    const uint32_t list = Load32(base, kGame + kWeaponList);
    const uint32_t entries = Ptr(list) ? Load32(base, list) : 0;
    const uint32_t count = Ptr(list) ? base[list + kWeaponListCount] : 0;
    if (!Ptr(entries) || !count)
    {
      why = "the level's weapon list is empty or missing";
      return 0;
    }
    why = "no item in either hand";
    for (const uint32_t hand : {kHand1, kHand0})
    {
      const uint32_t w = Load32(base, c + hand);
      if (!Ptr(w))
      {
        continue;
      }
      const uint32_t entry = Load32(base, w + kWeaponEntry);
      if (!Ptr(entry) || entry < entries || entry >= entries + count * kWeaponEntryBytes ||
          (entry - entries) % kWeaponEntryBytes != 0)
      {
        why = fmt::format("the item in hand ({:08X}) is not one of the level's {} weapons (+1248 = {:08X})", w, count,
                          entry);
        continue;
      }
      const uint32_t def = Load32(base, entry);
      const uint32_t type = Ptr(def) ? Load32(base, def + kWeaponDefType) : 0;
      if (!type)
      {
        why = fmt::format("the weapon list entry {:08X} has no definition / type", entry);
        continue;
      }
      // The same search as 824F2C10: the type must be found in the list, or give reads null.
      for (uint32_t i = 0; i < count; ++i)
      {
        const uint32_t d = Load32(base, entries + i * kWeaponEntryBytes);
        if (Ptr(d) && Load32(base, d + kWeaponDefType) == type)
        {
          return type;
        }
      }
      why = fmt::format("weapon type {} is not in the level's weapon list", type);
    }
    return 0;
  }

  bool TakeGuard() { return REXCVAR_GET(pdz_counterop_take_guard); }

  // A character's current health. A human's is his participant's +980 ([+0x19A0]+0x3D4: the human vtable's getter
  // 825A9408 / setter 825A9468 with flag 0; the damage handler 825AAF50 lowers it); +120 is a human's max (flag 1).
  // A guard's is +120. (co_jungle_3 logged +120 for player 2 and so never saw his health fall.)
  float Health(const uint8_t *base, uint32_t c)
  {
    if (!Ptr(c))
    {
      return 0.0f;
    }
    if (Load32(base, c) == kHumanVtable)
    {
      const uint32_t participant = Load32(base, c + kParticipantOf);
      if (Ptr(participant))
      {
        return LoadFloat(base, participant + kPartMaxHealth);
      }
    }
    return LoadFloat(base, c + kHealth);
  }

  int32_t TestMaxLives() { return REXCVAR_GET(pdz_counterop_test_max_lives); }

  // N64 take: the mission's listed eligible ids not yet replaced and not seen dead (guards the level hasn't built yet
  // count: it builds them later). 0 = player 2 is out for good. pdz_counterop_test_max_lives caps it for tests.
  uint32_t EligibleLeft()
  {
    uint32_t left = 0;
    if (g.ids.empty())
    {
      // A clone mission (pdz_counterop_clones; Datacore, Arena): the waves have no fixed count, so player 2 is never
      // out for good; the live clones of the last scan are reported, never below 1.
      left = std::max<uint32_t>(g.rule_live, 1u);
    }
    for (const uint32_t id : g.ids)
    {
      left += !g.taken_ids.count(id) && !g.dead_ids.count(id);
    }
    if (TestMaxLives() > 0)
    {
      left = std::min<uint32_t>(left, uint32_t(std::max(0, TestMaxLives() - g.lives)));
    }
    return left;
  }

  // A team-0 guard's three masks (8252BD50..8252BD88). Called before ShowP2 on a spawn, so Rare's team lists
  // (82187D18 files a character under each bit of +128) file him under the guards' bit, as Rare files every guard.
  void ApplyTeam(uint8_t *base, uint32_t c)
  {
    Store16(base, c + kTeamBits, kP2Team);
    Store16(base, c + kAllies, kP2Allies);
    Store16(base, c + kEnemies, kP2Enemies);
  }

  bool TeamApplied(const uint8_t *base, uint32_t c)
  {
    return Load16(base, c + kTeamBits) == kP2Team && Load16(base, c + kAllies) == kP2Allies &&
           Load16(base, c + kEnemies) == kP2Enemies;
  }

  // Player 2 faces the agent (Rare's look-angle setter, as Rare's spawn calls it after its move: yaw =
  // atan2(dx, dz) in radians, pitch 0). Logged with the view yaw (+0x1670, degrees) it left.
  void FaceAgent(PPCContext &ctx, uint8_t *base)
  {
    if (!Ptr(g.p1c) || !Ptr(g.p2c))
    {
      return;
    }
    const float dx = LoadFloat(base, g.p1c + kPosition) - LoadFloat(base, g.p2c + kPosition);
    const float dz = LoadFloat(base, g.p1c + kPosition + 8) - LoadFloat(base, g.p2c + kPosition + 8);
    if (!std::isfinite(dx) || !std::isfinite(dz) || dx * dx + dz * dz < 1.0f)
    {
      REXLOG_INFO("pdz_counterop: spawn (f): facing not set (the agent's position is unusable or on top of him)");
      return;
    }
    const double yaw = std::atan2(double(dx), double(dz));
    const float before = LoadFloat(base, g.p2c + kViewYaw);
    {
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = g.p2c;
      frame.ctx.f1.f64 = 0.0;
      frame.ctx.f2.f64 = yaw;
      sub_825AD3B0(frame.ctx, base);
    }
    REXLOG_INFO("pdz_counterop: spawn (f): facing the agent: yaw {:.1f} deg given (Rare's 825AD3B0, radians {:.3f}); "
                "view yaw +0x1670 {:.1f} -> {:.1f}",
                yaw * 57.29578, yaw, before, LoadFloat(base, g.p2c + kViewYaw));
  }

  // The guard goes through Rare's own script despawn (guard vtable +832, sub_825818E0(character, 1): physics and
  // world removal, its hidden flag at +6272 bit 0x10; level scripts hide whole groups with it, 8232C2E0). Not a
  // death: no corpse, no dropped gun, no kill event (a script waiting for "all dead" doesn't count it).
  std::string RemoveGuard(PPCContext &ctx, uint8_t *base, uint32_t guard)
  {
    if (Load32(base, guard + kHidden) & 0x10)
    {
      return std::string("already hidden (dormant): left so, kept hidden from now on");
    }
    Call(ctx, base, sub_825818E0, guard, 1);
    return (Load32(base, guard + kHidden) & 0x10)
               ? std::string("hidden through Rare's script despawn (vtable +832: out of the world, no corpse, not a death)")
               : std::string("hide call made, but its flag (+6272 0x10) is not set");
  }

  void CallEachChild(PPCContext &ctx, uint8_t *base, uint32_t c, PPCFunc *fn, bool with_zero)
  {
    uint32_t child = Load32(base, c + kChildFirst);
    for (int n = 0; Ptr(child) && n < 64; ++n, child = Load32(base, child + kChildNext))
    {
      if (with_zero)
      {
        Call(ctx, base, fn, child, 0);
      }
      else
      {
        Call(ctx, base, fn, child);
      }
    }
  }

  // Player 2 out of the world (log 304: he stayed Joanna's co-op partner): the generic steps of Rare's hide
  // (sub_825818E0's hide path: 82189B08, physics out for his attached objects and himself, team lists out, world out,
  // 82156E20), so he isn't drawn, collided with, seen or targeted; and Rare's player action lock on every bit
  // (8254D8F0), so he can't move, fire, use or pick anything up. He stays alive for Rare's co-op rules (no "partner
  // down", no revive); his view follows the agent.
  void HideP2(PPCContext &ctx, uint8_t *base, const char *why)
  {
    if (g.out || !Ptr(g.p2c))
    {
      return;
    }
    const uint32_t c = g.p2c, world = Load32(base, kGame + 24);
    Call(ctx, base, sub_82189B08, c);
    CallEachChild(ctx, base, c, sub_82517600, false);
    Call(ctx, base, sub_82187E98, c);
    Call(ctx, base, sub_82517600, c);
    if (Ptr(world))
    {
      Call(ctx, base, sub_82527D78, world, c);
    }
    Call(ctx, base, sub_82156E20, c, 1);
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = c;
    frame.ctx.r4.u64 = kLockAll;
    sub_8254D8F0(frame.ctx, base);
    g.out = true;
    g.out_since = std::chrono::steady_clock::now();
    g.p2_cam_updates = g.p1_cam_updates = g.spectate_writes = 0;
    g.spectate_reported = false;
    REXLOG_INFO("pdz_counterop: player 2 out of the world ({}): hidden (Rare's hide steps: physics, team lists, world) "
                "and locked (Rare's action lock, every bit); he counts as alive for the co-op rules (no partner game "
                "over, no revive); his body stays at ({:.0f}, {:.0f}, {:.0f}), his view follows the agent (spectate "
                "camera)",
                why, LoadFloat(base, c + kPosition), LoadFloat(base, c + kPosition + 4), LoadFloat(base, c + kPosition + 8));
  }

  // Back in the world (the reverse: sub_825818E0's restore path without its NPC-only repositioning), unlocked.
  void ShowP2(PPCContext &ctx, uint8_t *base)
  {
    if (!g.out || !Ptr(g.p2c))
    {
      return;
    }
    const uint32_t c = g.p2c, world = Load32(base, kGame + 24);
    CallEachChild(ctx, base, c, sub_82517530, true);
    Call(ctx, base, sub_82187D18, c);
    Call(ctx, base, sub_82517530, c, 0);
    if (Ptr(world))
    {
      Call(ctx, base, sub_82527C90, world, c);
    }
    Call(ctx, base, sub_82156EF0, c, 1);
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = c;
    frame.ctx.r4.u64 = kLockAll;
    sub_8254D968(frame.ctx, base);
    g.out = false;
  }

  // ---- The spots ---------------------------------------------------------------------------------------------

  // Why a pick found what it found: the characters through each test, by source and by rule.
  struct PickDiag
  {
    uint32_t registry_objects = 0, world_objects = 0, characters = 0, guards = 0; // SP guards (any state)
    uint32_t by_list = 0, by_rule = 0, denied = 0, unlisted_failed = 0;           // eligibility by setup
    uint32_t eligible = 0, live = 0, dormant = 0, dead = 0, taken = 0, unspawned = 0;
    uint32_t clones = 0, no_id = 0;                                               // the no-id rule's sources
    uint32_t occupied = 0, repeat = 0, no_setup = 0; // no-take mode: a character within 1.2 m; the last life's spot
    std::vector<std::pair<uint32_t, uint32_t>> unlisted; // the first 10 setup ids not in the list (key, string)
    bool registry_ok = false;
  };

  std::string CountsText(const PickDiag &d)
  {
    std::string ids;
    for (const auto &[id, ptr] : d.unlisted)
    {
      ids += (ids.empty() ? "" : ",") + IdChars(id);
    }
    if (!TakeGuard())
    {
      return fmt::format("free spots {} ({} live guards' + {} dead guards' + {} dormant guards' setup positions; no "
                         "guard touched; by the id list {}, by the no-id rule {}; {} occupied (a character within "
                         "{:.1f} m), {} the last life's, {} without a setup or clones; {} denied, {} unlisted "
                         "and failing the rule{}; {} of the {} listed ids with no guard built)",
                         d.eligible, d.live, d.dead, d.dormant, d.by_list, d.by_rule, d.occupied, kFreeRadius / 100.0f,
                         d.repeat, d.no_setup, d.denied, d.unlisted_failed,
                         ids.empty() ? std::string() : " (" + ids + ")", d.unspawned, g.ids.size());
    }
    return fmt::format("spots left {} ({} dormant + {} live; by the id list {}, by the no-id rule {}; {} taken, {} "
                       "dead; {} denied, {} unlisted and failing the rule{}; {} of the {} listed ids with no guard "
                       "built)",
                       d.eligible, d.dormant, d.live, d.by_list, d.by_rule, d.taken, d.dead, d.denied,
                       d.unlisted_failed, ids.empty() ? std::string() : " (" + ids + ")", d.unspawned, g.ids.size());
  }

  // Every object Rare's registry holds (hidden ones included), then the world list; each once.
  void CollectObjects(const uint8_t *base, std::vector<uint32_t> &out, PickDiag &d)
  {
    std::unordered_set<uint32_t> seen;
    const uint32_t so = Load32(base, kGame);
    const uint32_t registry = Ptr(so) ? Load32(base, so + kRegistry) : 0;
    if (Ptr(registry))
    {
      const uint32_t reg = registry + kRegistryBase;
      const uint32_t group_end = Load32(base, reg + kGroupEnd);
      uint32_t group = Load32(base, reg + kGroupFirst);
      for (int gi = 0; Ptr(group) && group != group_end && gi < 4096; ++gi, group = Load32(base, group + kGroupNext))
      {
        const uint32_t end = Load32(base, group + kGroupListEnd);
        uint32_t node = Load32(base, group + kGroupListFirst);
        for (int ni = 0; Ptr(node) && node != end && ni < 16384; ++ni, node = Load32(base, node + kNodeNext))
        {
          const uint32_t o = Load32(base, node + kNodeObject);
          if (Ptr(o) && seen.insert(o).second)
          {
            out.push_back(o);
            ++d.registry_objects;
          }
        }
      }
    }
    d.registry_ok = d.registry_objects > 0;
    const uint32_t world = Load32(base, kGame + 24);
    if (Ptr(world))
    {
      const uint32_t end = Load32(base, world + kWorldEnd);
      uint32_t node = Load32(base, world + kWorldFirst);
      for (int i = 0; Ptr(node) && node != end && i < 16384; ++i, node = Load32(base, node + 12))
      {
        const uint32_t o = Load32(base, node + 4);
        ++d.world_objects;
        if (Ptr(o) && seen.insert(o).second)
        {
          out.push_back(o);
        }
      }
    }
  }

  // The positions of the characters in the world list (guards alive or dead, humans, bots), player 2 left out: a
  // no-take spot is free when none is within kFreeRadius.
  void WorldCharacterPositions(const uint8_t *base, std::vector<std::array<float, 3>> &out)
  {
    const uint32_t world = Load32(base, kGame + 24);
    if (!Ptr(world))
    {
      return;
    }
    const uint32_t end = Load32(base, world + kWorldEnd);
    uint32_t node = Load32(base, world + kWorldFirst);
    for (int i = 0; Ptr(node) && node != end && i < 16384; ++i, node = Load32(base, node + 12))
    {
      const uint32_t o = Load32(base, node + 4);
      if (!Ptr(o) || o == g.p2c)
      {
        continue;
      }
      const uint32_t vt = Load32(base, o);
      if (vt == kGuardVtable || vt == kHumanVtable || vt == 0x820BF3B8u)
      {
        out.push_back({LoadFloat(base, o + kPosition), LoadFloat(base, o + kPosition + 4),
                       LoadFloat(base, o + kPosition + 8)});
      }
    }
  }

  // The eligible spots, nearest the agent first, and the counts. Take mode (pdz_counterop_take_guard): live or
  // dormant guards not taken. Otherwise every eligible guard's setup position that is free, whatever the guard's
  // state (it is never touched).
  std::vector<Spot> Spots(PPCContext &ctx, uint8_t *base, PickDiag &d)
  {
    std::vector<Spot> out;
    if (!Ptr(g.p1c))
    {
      return out;
    }
    const bool take = TakeGuard();
    std::vector<uint32_t> objects;
    CollectObjects(base, objects, d);
    std::vector<std::array<float, 3>> others;
    if (!take)
    {
      WorldCharacterPositions(base, others);
    }
    std::unordered_set<uint32_t> seen_ids;
    const float ax = LoadFloat(base, g.p1c + kPosition), ay = LoadFloat(base, g.p1c + kPosition + 4),
                az = LoadFloat(base, g.p1c + kPosition + 8);
    for (const uint32_t c : objects)
    {
      const uint32_t vt = Load32(base, c);
      d.characters += vt == kGuardVtable || vt == kHumanVtable || vt == 0x820BF3B8u; // guard, human, MP bot
      if (vt != kGuardVtable)
      {
        continue;
      }
      ++d.guards;
      if (c == g.kill_guard)
      {
        continue; // a replaced guard restored for Rare's kill (Trinity Escape: counted as a live spot for a tick)
      }
      const uint32_t setup = Load32(base, c + kSetup);
      const uint8_t team = Ptr(setup) ? base[setup + kSetupTeam] : 0;
      uint32_t id_ptr = 0;
      const uint32_t id = SetupIdKey(base, setup, &id_ptr);
      if (id)
      {
        seen_ids.insert(id);
      }
      if (take && g.taken.count(c))
      {
        ++d.taken;
        continue;
      }
      if (Ptr(setup) && team != 0 && team != kTeamTemplate)
      {
        continue; // an ally or a third party
      }
      Spot e;
      e.type = Ptr(setup) ? Load32(base, setup + kSetupType) : Load32(base, c + kBodyId);
      if (id && g.denied.count(id))
      {
        ++d.denied;
        continue;
      }
      if (id && g.ids.count(id))
      {
        e.by_list = true;
        ++d.by_list;
      }
      else
      {
        // The no-id rule: a runtime clone of a spawn template, a guard whose setup id can't be read, or an id the
        // list doesn't know: a combat type with a gun in hand.
        e.why = !Ptr(setup)             ? "no setup"
                : team == kTeamTemplate ? "runtime clone of a spawn template"
                : !id                   ? "id unreadable"
                                        : "id not in the list";
        ++(team == kTeamTemplate && Ptr(setup) ? d.clones : d.no_id);
        if (!CombatType(e.type) || !HandWeaponType(base, c))
        {
          if (id)
          {
            ++d.unlisted_failed;
            if (d.unlisted.size() < 10 &&
                std::find_if(d.unlisted.begin(), d.unlisted.end(),
                             [id](const std::pair<uint32_t, uint32_t> &u) { return u.first == id; }) ==
                    d.unlisted.end())
            {
              d.unlisted.emplace_back(id, id_ptr);
            }
          }
          continue;
        }
        ++d.by_rule;
      }
      const bool alive = Alive(ctx, base, c);
      if (!alive && id && g.ids.count(id))
      {
        g.dead_ids.insert(id); // the eligible-left count (EligibleLeft)
      }
      if (!alive && take)
      {
        ++d.dead;
        continue;
      }
      e.dormant = alive && (Load32(base, c + kHidden) & 0x10) != 0;
      e.dead = !alive;
      e.calm = !(Load32(base, c + 6268) & 0x400000u) && !(Load32(base, c + 6252) & 0x82u) && !Load32(base, c + 0x317C);
      e.character = c;
      e.setup = setup;
      e.id = id;
      e.id_ptr = id_ptr;
      if (!take)
      {
        // No-take mode: the guard's setup position (where Rare placed it: a floor spot of an area the level uses),
        // free of characters, and not the spot of the life just lost.
        if (!Ptr(setup) || team == kTeamTemplate)
        {
          ++d.no_setup; // no setup, or a clone whose setup is its spawner template's (that spot may be off the map)
          continue;
        }
        if (setup == g.last_setup)
        {
          ++d.repeat;
          continue;
        }
        e.x = LoadFloat(base, setup + kSetupPosition);
        e.y = LoadFloat(base, setup + kSetupPosition + 4);
        e.z = LoadFloat(base, setup + kSetupPosition + 8);
        if (!std::isfinite(e.x) || !std::isfinite(e.y) || !std::isfinite(e.z))
        {
          continue;
        }
        bool free = true;
        for (const auto &p : others)
        {
          if (Dist3(e.x, e.y, e.z, p[0], p[1], p[2]) < kFreeRadius)
          {
            free = false;
            break;
          }
        }
        if (!free)
        {
          ++d.occupied;
          continue;
        }
        e.distance = Dist3(e.x, e.y, e.z, ax, ay, az);
        e.on_screen = OnAgentScreen(base, g.p1c, e.x, e.z, e.distance);
        ++(e.dead ? d.dead : e.dormant ? d.dormant : d.live);
        out.push_back(e);
        continue;
      }
      if (e.dormant && Ptr(setup))
      {
        e.x = LoadFloat(base, setup + kSetupPosition);
        e.y = LoadFloat(base, setup + kSetupPosition + 4);
        e.z = LoadFloat(base, setup + kSetupPosition + 8);
      }
      else
      {
        e.x = LoadFloat(base, c + kPosition);
        e.y = LoadFloat(base, c + kPosition + 4);
        e.z = LoadFloat(base, c + kPosition + 8);
      }
      if (!std::isfinite(e.x) || !std::isfinite(e.y) || !std::isfinite(e.z))
      {
        continue;
      }
      e.distance = Dist3(e.x, e.y, e.z, ax, ay, az);
      e.on_screen = OnAgentScreen(base, g.p1c, e.x, e.z, e.distance);
      ++(e.dormant ? d.dormant : d.live);
      out.push_back(e);
    }
    for (const uint32_t id : g.ids)
    {
      d.unspawned += !seen_ids.count(id);
    }
    d.eligible = uint32_t(out.size());
    if (take)
    {
      uint32_t live = 0;
      for (const Spot &e : out)
      {
        live += !e.by_list && !e.dead; // clone missions (pdz_counterop_clones): what EligibleLeft counts
      }
      g.rule_live = live;
    }
    std::sort(out.begin(), out.end(), [](const Spot &a, const Spot &b) { return a.distance < b.distance; });
    return out;
  }

  // The pick: dormant spots first, at least kMinSpawnDistance from the agent and off his screen; relaxed step by
  // step (dormant far; live far off-screen; live far; dormant any; live any); one of the three nearest qualifying at
  // random. Returns the step's name.
  // No-take mode: a live guard's setup position (an area the level is using, reachable: the guards got there) first,
  // then a dead guard's (a cleared area), then a dormant guard's (a later wave's spot: possibly behind a door the
  // script opens later, so last), each far and off-screen first.
  const char *Pick(const std::vector<Spot> &spots, Spot &out)
  {
    // kind: 0 live, 1 dead, 2 dormant, 3 any
    struct Step
    {
      const char *name;
      int kind;
      bool far, off_screen;
    };
    // N64 take: a guard the level has in play (live or dormant), calm first, off-screen first, far first.
    static constexpr Step kTakeSteps[] = {
        {"a calm guard, far and off-screen", 4, true, true},
        {"a calm guard, off-screen", 4, false, true},
        {"a guard, far and off-screen", 3, true, true},
        {"a calm guard, far", 4, true, false},
        {"a guard, off-screen", 3, false, true},
        {"a guard (none off-screen)", 3, false, false},
    };
    static constexpr Step kFreeSteps[] = {
        {"a live guard's setup position, far and off-screen", 0, true, true},
        {"a dead guard's setup position, far and off-screen", 1, true, true},
        {"a live guard's setup position, far", 0, true, false},
        {"a dead guard's setup position, far", 1, true, false},
        {"a dormant guard's setup position, far and off-screen", 2, true, true},
        {"any free setup position, off-screen (none far)", 3, false, true},
        {"any free setup position (none far, none off-screen)", 3, false, false},
    };
    const bool take = TakeGuard();
    const Step *steps = take ? kTakeSteps : kFreeSteps;
    const size_t count = take ? std::size(kTakeSteps) : std::size(kFreeSteps);
    // Take: only calm guards for the first kCalmOnlySeconds of a search (a first spot or after a death); then any.
    const bool calm_only =
        take && std::chrono::steady_clock::now() - g.need_since < std::chrono::milliseconds(int(kCalmOnlySeconds * 1000));
    for (size_t si = 0; si < count; ++si)
    {
      const Step &s = steps[si];
      if (calm_only && s.kind != 4)
      {
        continue;
      }
      std::vector<const Spot *> list;
      for (const Spot &e : spots)
      {
        const int kind = e.dead ? 1 : e.dormant ? 2 : 0;
        // kind 3 = any; kind 4 = any calm guard (take mode)
        if ((s.kind == 3 || kind == s.kind || (s.kind == 4 && e.calm)) && (!s.far || e.distance >= kMinSpawnDistance) &&
            (!s.off_screen || !e.on_screen))
        {
          list.push_back(&e); // spots are sorted nearest first
        }
      }
      if (!list.empty())
      {
        // N64 (take): one of the 9 nearest the agent; no-take: one of the 3 nearest.
        const size_t n = std::min<size_t>(list.size(), take ? 9 : 3);
        out = *list[std::uniform_int_distribution<size_t>(0, n - 1)(g_rng)];
        return s.name;
      }
    }
    return nullptr;
  }

  // ---- The spawn ----------------------------------------------------------------------------------------------

  // Player 2's participant, when it is still in Rare's list (a character may be gone after a level change).
  bool ParticipantListed(const uint8_t *base, uint32_t participant)
  {
    for (uint32_t p = Load32(base, kParticipantList), n = 0; Ptr(p) && n < 16; p = Load32(base, p + 4), ++n)
    {
      if (p == participant)
      {
        return true;
      }
    }
    return false;
  }

  // The N64 rule (see the header): health and max = (the guard's health / its spawn health) x the co-op max x the
  // scale, capped at the co-op max, floor kMinHealth. Armour stays 0.
  void ApplyHealth(uint8_t *base, const Spot &e)
  {
    const float spawn_health = Ptr(e.setup) ? LoadFloat(base, e.setup + kSetupHealth) : 0.0f;
    // No-take mode: nothing is taken from the guard, so player 2 is a fresh guard of that setup (ratio 1).
    const float guard_health = TakeGuard() || !(spawn_health > 0.0f) ? LoadFloat(base, e.character + kHealth)
                                                                     : spawn_health;
    const double scale = REXCVAR_GET(pdz_counterop_health_scale);
    const float cap = g.coop_max_health > 0.0f ? g.coop_max_health : 100.0f;
    float ratio = spawn_health > 0.0f && std::isfinite(spawn_health) ? guard_health / spawn_health : 1.0f;
    if (!std::isfinite(ratio) || ratio < 0.0f)
    {
      ratio = 0.0f;
    }
    if (ratio > 1.0f)
    {
      ratio = 1.0f;
    }
    float hp = float(double(ratio) * double(cap) * scale);
    const char *note = "";
    if (!std::isfinite(hp) || hp > cap)
    {
      hp = cap;
      note = " (capped)";
    }
    if (hp < kMinHealth)
    {
      hp = kMinHealth;
      note = " (floor)";
    }
    const uint32_t participant = Load32(base, g.p2c + kParticipantOf);
    const float before = LoadFloat(base, g.p2c + kHealth);
    const float before_max = Ptr(participant) ? LoadFloat(base, participant + kPartMaxHealth) : -1.0f;
    StoreFloat(base, g.p2c + kHealth, hp);
    if (Ptr(participant))
    {
      StoreFloat(base, participant + kPartMaxHealth, hp);
      g.max_health_set = true;
    }
    StoreFloat(base, g.p2c + kArmour, 0.0f);
    g.set_health = hp;
    g.health_check = true;
    g.health_check_at = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    REXLOG_INFO("pdz_counterop: spawn (d): health: the guard's {:.2f} of its spawn health {:.2f} (setup +256; its "
                "getter has no max) = {:.2f} x the co-op max {:.0f} x {:.3f} = {:.1f}{}: player 2's health {:.1f} -> "
                "{:.1f}, max health {} -> {:.1f}{}; armour 0 (the guard's {:.1f} not copied)",
                guard_health, spawn_health, ratio, cap, scale, hp, note, before, hp,
                before_max >= 0.0f ? fmt::format("{:.1f}", before_max) : std::string("?"), hp,
                Ptr(participant) ? "" : " (no participant at +6560: max not set)",
                LoadFloat(base, e.character + kArmour));
  }

  // Player 2 spawns at this spot: (a) back in the world (unhidden, unlocked; the spectate camera stops), (b) moved
  // to the spot, (c) the guards' side and the guard's gun with full reserves (only a type Rare's give can look up;
  // otherwise skipped: he keeps his gun), (d) the health rule, (e) the guard hidden (a dormant one left hidden) and
  // kept so. One line per step.
  void SpawnAt(PPCContext &ctx, uint8_t *base, const Spot &e, const PickDiag &d, const char *step, const char *why)
  {
    const uint32_t guard = e.character;
    const bool take = TakeGuard();
    std::string no_gun;
    uint32_t gun = GiveableWeaponType(base, guard, no_gun);
    if (!gun && !take && g.last_gun)
    {
      gun = g.last_gun; // a dead guard's gun is usually dropped: the last life's (from this level's list)
      REXLOG_INFO("pdz_counterop: spawn: the spot's guard has no giveable gun ({}): the last life's type {} instead",
                  no_gun, gun);
    }
    if (take)
    {
      g.taken.insert(guard);
      g.current_guard = guard;
      if (e.id)
      {
        g.taken_ids.insert(e.id);
      }
    }
    else
    {
      g.last_setup = e.setup;
    }
    ++g.lives;
    if (take)
    {
      REXLOG_INFO("pdz_counterop: replaces guard {:08X} '{}' (life {}): eligible guards left {} (of {} listed: {} "
                  "replaced, {} seen dead){}",
                  guard, IdChars(e.id), g.lives, EligibleLeft(), g.ids.size(), g.taken_ids.size(), g.dead_ids.size(),
                  TestMaxLives() > 0 ? fmt::format("; test cap {} lives", TestMaxLives()) : std::string());
    }
    REXLOG_INFO("pdz_counterop: {}: player 2 (life {}) spawns at {}: guard {:08X} (setup {:08X}, id {}; {}; body {}, "
                "head {}) at ({:.0f}, {:.0f}, {:.0f}), {:.1f} m from the agent, {} the agent's screen; {}",
                why, g.lives, step, guard, e.setup, e.id ? IdLog(e.id, e.id_ptr) : std::string("none"),
                e.by_list ? fmt::format("rule: the {} id list", g.mission)
                          : fmt::format("rule: no id ({}): type 48+, not a non-combatant, armed, not denied", e.why),
                Load32(base, guard + kBodyId), Load32(base, guard + kHeadId), e.x, e.y, e.z, e.distance / 100.0f,
                e.on_screen ? "on" : "off", CountsText(d));
    // (a) the guards' masks first, so ShowP2's team-list filing (82187D18, by the bits of +128) matches them
    ApplyTeam(base, g.p2c);
    const bool was_out = g.out;
    ShowP2(ctx, base);
    REXLOG_INFO("pdz_counterop: spawn (a): player 2 {}", was_out ? fmt::format("back in the world (unhidden: world, "
                                                                               "team lists, physics; unlocked) after {} "
                                                                               "pick tries; spectate camera off",
                                                                               g.tries)
                                                                 : std::string("was already in the world"));
    // (b)
    if (!g.scratch)
    {
      g.scratch = REX_KERNEL_MEMORY()->SystemHeapAlloc(16);
    }
    if (g.scratch)
    {
      StoreFloat(base, g.scratch, e.x);
      StoreFloat(base, g.scratch + 4, e.y);
      StoreFloat(base, g.scratch + 8, e.z);
      Call(ctx, base, sub_825AD148, g.p2c, g.scratch, 0);
      REXLOG_INFO("pdz_counterop: spawn (b): player 2 moved to ({:.0f}, {:.0f}, {:.0f}) ({}; now at ({:.0f}, {:.0f}, "
                  "{:.0f}))",
                  e.x, e.y, e.z,
                  !take        ? (e.dead      ? "a dead guard's setup position"
                                  : e.dormant ? "a dormant guard's setup position"
                                              : "a live guard's setup position (the guard is elsewhere)")
                  : e.dormant ? "the dormant guard's setup position"
                              : "the live guard's position",
                  LoadFloat(base, g.p2c + kPosition), LoadFloat(base, g.p2c + kPosition + 4),
                  LoadFloat(base, g.p2c + kPosition + 8));
      FaceAgent(ctx, base); // (f)
    }
    else
    {
      REXLOG_WARN("pdz_counterop: spawn (b): no guest scratch memory for the move: player 2 not moved");
    }
    // (c) the side and the gun, as in log 310 (no ammo amounts in the give: log 311 crashed in its secondary-ammo
    // block, which reads the entry's +4 definition, null for a one-ammo gun); then full reserves through Rare's adders.
    ApplyTeam(base, g.p2c); // again: a no-op unless something between (a) and here reset them
    if (gun)
    {
      g.last_gun = gun;
      g_giving = true; // the pick-up block (82586238's hook) lets our own give through
      uint32_t item = Call(ctx, base, sub_8254C378, g.p2c, gun, 0, 0, 0, 0);
      g_giving = false;
      std::string ammo = "no item: no ammo added";
      if (!Ptr(item))
      {
        // co_outpost life 2: he still held that gun after Rare's revive, so the give made no new item: the gun in
        // his hand of that type gets the reserves instead.
        for (const uint32_t hand : {kHand0, kHand1})
        {
          const uint32_t held = Load32(base, g.p2c + hand);
          if (Ptr(held) && Load32(base, held + kItemType) == gun)
          {
            item = held;
            ammo = "already held";
            break;
          }
        }
      }
      if (Ptr(item))
      {
        const uint32_t entry = Load32(base, item + kWeaponEntry);
        const uint32_t def = Ptr(entry) ? Load32(base, entry + kWeaponEntryDef) : 0;
        const uint32_t def2 = Ptr(entry) ? Load32(base, entry + kWeaponEntrySecondaryDef) : 0;
        if (Ptr(def))
        {
          Call(ctx, base, sub_82583F80, item, kFullAmmo, 1);
          ammo = "primary reserve filled (82583F80)";
          if (Ptr(def2))
          {
            Call(ctx, base, sub_82584118, item, kFullAmmo, 1);
            ammo += ", secondary reserve filled (82584118)";
          }
          else
          {
            ammo += ", no secondary definition (entry +4 null)";
          }
        }
        else
        {
          ammo = fmt::format("the item's weapon entry ({:08X}) has no definition: no ammo added", entry);
        }
      }
      // Drawn (co_outpost: the partner started unarmed and the give only put it in his inventory; his first-person
      // view had empty hands): Rare's weapon switcher (character +5780), its select (vtable slot 0: switcher, item,
      // rest 0), as the bots' draw in cut_scenarios.cpp and Rare's own draws do.
      std::string drawn = "already in his hand";
      if (Ptr(item) && Load32(base, g.p2c + kHand0) != item && Load32(base, g.p2c + kHand1) != item)
      {
        const uint32_t switcher = Load32(base, g.p2c + 5780);
        PPCFunc *select = Ptr(switcher) ? pdz::GuestFunction(Load32(base, Load32(base, switcher)), "counter-op draw")
                                        : nullptr;
        if (select)
        {
          rex::CallFrame frame(ctx);
          frame.ctx.r1.u64 = (ctx.r1.u32 - 0x800) & ~0xFu;
          frame.ctx.r3.u64 = switcher;
          frame.ctx.r4.u64 = item;
          frame.ctx.r5.u64 = 0;
          frame.ctx.r6.u64 = 0;
          frame.ctx.r7.u64 = 0;
          frame.ctx.r8.u64 = 0;
          frame.ctx.r9.u64 = 0;
          frame.ctx.r10.u64 = 0;
          select(frame.ctx, base);
          drawn = fmt::format("drawn through his weapon switcher {:08X}", switcher);
        }
        else
        {
          drawn = "NOT drawn (no weapon switcher)";
        }
      }
      REXLOG_INFO("pdz_counterop: spawn (c): guards' side (team bits {:04X}, enemies {:04X}); gun type {} given ({}); {}; "
                  "{}",
                  Load16(base, g.p2c + kTeamBits), Load16(base, g.p2c + kEnemies), gun,
                  item ? fmt::format("item {:08X}", item) : std::string("give returned none"), ammo, drawn);
    }
    else
    {
      REXLOG_INFO("pdz_counterop: spawn (c): guards' side (team bits {:04X}, enemies {:04X}); gun skipped ({}): player "
                  "2 keeps his gun",
                  Load16(base, g.p2c + kTeamBits), Load16(base, g.p2c + kEnemies), no_gun);
    }
    // (d)
    ApplyHealth(base, e);
    // (e)
    if (take)
    {
      const std::string removed = RemoveGuard(ctx, base, guard);
      REXLOG_INFO("pdz_counterop: spawn (e): guard {:08X} {}", guard, removed);
    }
    else
    {
      REXLOG_INFO("pdz_counterop: spawn (e): guard {:08X} untouched ({}; +6272 {:08X}): no guard hidden, the level's "
                  "count and scripts unchanged; player 2's masks {:04X} / {:04X} / {:04X}",
                  guard, e.dead ? "dead" : e.dormant ? "dormant" : "live", Load32(base, guard + kHidden),
                  Load16(base, g.p2c + kTeamBits), Load16(base, g.p2c + kAllies), Load16(base, g.p2c + kEnemies));
    }
    g.tries = 0;
  }

  // A taken guard a level script shows again goes back out (kept hidden; logged the first 3 times per guard).
  void KeepTakenHidden(PPCContext &ctx, uint8_t *base)
  {
    for (const uint32_t guard : g.taken)
    {
      if (!Ptr(guard) || Load32(base, guard) != kGuardVtable || (Load32(base, guard + kHidden) & 0x10))
      {
        continue;
      }
      Call(ctx, base, sub_825818E0, guard, 1);
      const uint32_t n = ++g.rehidden[guard];
      if (n <= 3)
      {
        REXLOG_INFO("pdz_counterop: a level script showed the taken guard {:08X} (id '{}'): hidden again ({}{})", guard,
                    IdChars(SetupIdKey(base, Load32(base, guard + kSetup))),
                    (Load32(base, guard + kHidden) & 0x10) ? "ok" : "flag still clear",
                    n == 3 ? "; further times not logged" : "");
      }
    }
  }

  // pdz_counterop_fix_out_freeze (logs 022 and co_trinity_escape_2, hang dump 12:59:38): the spectate call of Rare's
  // camera update for player 2's camera (the agent's camera hook below) used to copy the registers as Rare's update
  // for the agent left them, so its f1 (the frame step; sub_82214C48 keeps it in f24) was whatever that update
  // computed last. The update adds step x rate to the camera's three sway angles (+124 / +128 / +132, 82214DCC) and
  // wraps each with "while (angle > pi) angle -= 2 pi" (82214E20 / E3C / E58): a step of infinity (or one large
  // enough that 2 pi is below the float's precision) never leaves that loop. The dump: the game thread in that loop
  // on player 2's camera (r30 = 021230E0, the noted camera), the angle +inf against pi; the agent's camera fine.
  // Now the call gets Rare's own step (sanitised: finite, 0..0.25 s, else 1/60 s), and the three angles are reset to
  // 0 if they are ever non-finite or over 1000 (before the call; Rare's update for him never runs while he is out).
  constexpr uint32_t kCamSway = 124;   // the three sway angles (radians, wrapped to -pi..pi by Rare's update)
  constexpr double kMaxStep = 0.25;    // a plausible frame step (s)
  void SpectateStepFix(uint8_t *base, PPCContext &frame, double step, bool on)
  {
    const double leftover = frame.f1.f64;
    const bool step_ok = std::isfinite(step) && step >= 0.0 && step <= kMaxStep;
    const double use = step_ok ? step : 1.0 / 60.0;
    if (on)
    {
      frame.f1.f64 = use;
    }
    ++g.spectate_calls;
    const bool bad = !std::isfinite(leftover) || leftover < 0.0 || leftover > kMaxStep;
    if (bad)
    {
      ++g.bad_leftover;
    }
    if (std::isfinite(leftover) && std::fabs(leftover) > g.max_leftover)
    {
      g.max_leftover = std::fabs(leftover);
    }
    if (g.spectate_calls <= 2 || (bad && g.bad_leftover <= 5))
    {
      REXLOG_INFO("pdz_counterop_fix_out_freeze ({}): spectate camera update {}: {}; the step register as the agent's "
                  "update left it was {}{}",
                  on ? "on" : "OFF", g.spectate_calls,
                  on ? fmt::format("Rare's step {:.4f} s given{}", use,
                                   step_ok ? "" : fmt::format(" (Rare's own was {}: not usable)", step))
                     : std::string("that leftover is passed on as the step (the old call)"),
                  leftover,
                  bad ? fmt::format(" (NOT a step: {} so far; passed on, it freezes the game)", g.bad_leftover)
                      : std::string(" (a plausible step this time)"));
    }
    if (!on)
    {
      return;
    }
    std::string reset;
    for (uint32_t i = 0; i < 3; ++i)
    {
      const float a = LoadFloat(base, g.p2_mode + kCamSway + i * 4);
      if (!std::isfinite(a) || std::fabs(a) > 1000.0f)
      {
        StoreFloat(base, g.p2_mode + kCamSway + i * 4, 0.0f);
        reset += fmt::format("{}+{} {} -> 0", reset.empty() ? "" : ", ", kCamSway + i * 4, a);
      }
    }
    if (!reset.empty() && ++g.angle_resets <= 5)
    {
      REXLOG_WARN("pdz_counterop_fix_out_freeze: player 2's camera {:08X} sway angles out of range, reset before Rare's "
                  "wrap loop: {} ({} time(s))",
                  g.p2_mode, reset, g.angle_resets);
    }
  }

  // Stage 0 probes at the start: the level's body / head lists, player 2's masks, his look and health.
  void LogProbes(const uint8_t *base)
  {
    const uint32_t lists = Load32(base, kGame + 8);
    for (const uint32_t at : {kListsBody, kListsHead})
    {
      const uint32_t list = Ptr(lists) ? Load32(base, lists + at) : 0;
      const int32_t count = Ptr(list) ? int32_t(Load32(base, list + kListCount)) : 0;
      const uint32_t entries = Ptr(list) ? Load32(base, list + kListEntries) : 0;
      std::string ids;
      for (int32_t i = 0; Ptr(entries) && i < count && i < 160; ++i)
      {
        const uint32_t e = Load32(base, entries + uint32_t(i) * 4);
        ids += fmt::format("{}{}", ids.empty() ? "" : ",", Ptr(e) ? int32_t(Load32(base, e)) : -1);
      }
      REXLOG_INFO("pdz_counterop: probe: the level's {} list: {} entries: {}", at ? "head" : "body", count,
                  ids.empty() ? std::string("none") : ids);
    }
    REXLOG_INFO("pdz_counterop: probe: player 2 {:08X}: team bits {:04X}, allies {:04X}, enemies {:04X}, +6276 {:08X} "
                "(0x400 without 0x800 = Rare's betrayal switch: his first hit on a friend clears his bits); player 1 "
                "{:08X}: team bits {:04X}",
                g.p2c, Load16(base, g.p2c + kTeamBits), Load16(base, g.p2c + kAllies), Load16(base, g.p2c + kEnemies),
                Load32(base, g.p2c + kBetrayal), g.p1c, Load16(base, g.p1c + kTeamBits));
    const uint32_t participant = Load32(base, g.p2c + kParticipantOf);
    REXLOG_INFO("pdz_counterop: probe: player 2 is built as body {} / head {} ({}); health {:.1f}, max health {:.1f} "
                "(participant {:08X}{}), armour {:.1f}; health scale {:.3f}, floor {:.0f}",
                Load32(base, g.p2c + kBodyId), Load32(base, g.p2c + kHeadId),
                g_look.decided ? (g_look.chosen ? fmt::format("the look chosen at load: {}", g_look.why)
                                                : fmt::format("the co-op look: {}", g_look.why))
                               : std::string("the co-op look: the factory hook made no decision"),
                LoadFloat(base, g.p2c + kHealth), g.coop_max_health, participant,
                participant == g.p2p ? "" : " (not his participant from the list!)", LoadFloat(base, g.p2c + kArmour),
                REXCVAR_GET(pdz_counterop_health_scale), kMinHealth);
    // The look as built, after his participant attached (825A99D8 -> 825B43E0 made the first-person hands).
    const uint32_t body_entry = Load32(base, g.p2c + kBodyEntryOf), head_entry = Load32(base, g.p2c + kHeadEntryOf);
    const uint32_t hand0 = Load32(base, g.p2c + kFirstPersonHands), hand1 = Load32(base, g.p2c + kFirstPersonHands + 4);
    REXLOG_INFO("pdz_counterop: look: at the start player 2 wears body {} ('{}', animation set +64 {:08X}) / head {} "
                "('{}'); body instance {:08X}, head instance {:08X}; player hands +20 {:08X}, first-person hands "
                "{:08X} / {:08X}{}",
                Load32(base, g.p2c + kBodyId),
                Ptr(body_entry) ? GuestString(base, Load32(base, body_entry + kBodyModelRef)) : std::string("?"),
                Ptr(body_entry) ? Load32(base, body_entry + kBodyAnimSet) : 0, Load32(base, g.p2c + kHeadId),
                Ptr(head_entry) ? GuestString(base, Load32(base, head_entry + kHeadModelRef)) : std::string("?"),
                Load32(base, g.p2c + 72), Load32(base, g.p2c + 4104),
                Ptr(body_entry) ? Load32(base, body_entry + kBodyHands) : 0, hand0, hand1,
                Ptr(hand0) && Ptr(hand1) ? "" : " (NO first-person hands: log 312's crash case)");
  }

  void Reset(uint8_t *base, const char *why)
  {
    if (g.started)
    {
      std::string restored;
      if (g.max_health_set && Ptr(g.p2p) && ParticipantListed(base, g.p2p))
      {
        StoreFloat(base, g.p2p + kPartMaxHealth, g.coop_max_health);
        restored = fmt::format("; his max health back to {:.0f}", g.coop_max_health);
      }
      REXLOG_INFO("pdz_counterop: stopped ({}): player 2 lives {}, team bits re-applied {} time(s){}; spectate camera "
                  "calls {} ({} with a leftover step register that was not a step, largest finite {:.3g}; angle "
                  "resets {}; pdz_counterop_fix_out_freeze {})",
                  why, g.lives, g.team_fixes, restored, g.spectate_calls, g.bad_leftover, g.max_leftover,
                  g.angle_resets, REXCVAR_GET(pdz_counterop_fix_out_freeze) ? "on" : "off");
    }
    const uint32_t scratch = g.scratch;
    g = State{};
    g.scratch = scratch;
  }

  void Start(PPCContext &ctx, uint8_t *base)
  {
    g.started = true;
    g_played = false;
    std::string raw;
    g.mission = pdz::pace::SpLevelName(base, &raw);
    for (const MissionIds &m : kEligible)
    {
      if (g.mission == m.mission)
      {
        ParseIds(m.ids, g.ids);
      }
    }
    for (const MissionIds &m : kDenied)
    {
      if (g.mission == m.mission)
      {
        ParseIds(m.ids, g.denied);
      }
    }
    const uint32_t participant = Load32(base, g.p2c + kParticipantOf);
    g.coop_max_health = Ptr(participant) ? LoadFloat(base, participant + kPartMaxHealth) : 100.0f;
    REXLOG_INFO("pdz_counterop: start: mission {} (Rare's '{}'), agent {:08X} (participant {:08X}, user {}), counter-op "
                "{:08X} (participant {:08X}, user {}); setup filter at load: {} setups checked, {} spawned for "
                "counter-op that co-op would not, {} co-op setups left out; {} eligible ids, {} denied ids",
                g.mission.empty() ? "?" : g.mission, raw, g.p1c, g.p1p, int32_t(Load32(base, g.p1p + kPartUser)), g.p2c,
                g.p2p, int32_t(Load32(base, g.p2p + kPartUser)), g_filter_seen, g_filter_added, g_filter_removed,
                g.ids.size(), g.denied.size());
    LogProbes(base);
    if (g.ids.empty() && !REXCVAR_GET(pdz_counterop_clones))
    {
      g.unsupported = true;
      REXLOG_INFO("pdz_counterop: {}: no eligible guards (Datacore and Arena have only runtime clones; "
                  "pdz_counterop_clones is off): plain co-op",
                  g.mission.empty() ? "this level" : g.mission);
      return;
    }
    if (g.ids.empty())
    {
      REXLOG_INFO("pdz_counterop: {}: no listed guards (Datacore / Arena: runtime clones only): pdz_counterop_clones: "
                  "player 2 replaces the clones the no-id rule accepts (a combat type with a gun in hand) and is never "
                  "out for good; the eligible count is the live clones of the last scan",
                  g.mission.empty() ? "this level" : g.mission);
    }
    // Player 2 is never Joanna's partner: out of the world until his first spot. Picks are tried once a second,
    // from kFirstPickSeconds on and never during a cutscene (log 304: the one pick ran before the opening cutscene).
    g_played = true;
    // The hide waits until Rare has updated player 2's camera once (its object is then known, so the spectate view
    // can drive it while he is out), at most kHideWaitSeconds.
    if (Ptr(g.p2_mode))
    {
      HideP2(ctx, base, "until his first spot");
    }
    else
    {
      g.hide_pending = true;
      g.hide_pending_since = std::chrono::steady_clock::now();
    }
    g.need_guard = true;
    g.after_death = false;
    g.need_since = std::chrono::steady_clock::now();
    g.next_try = g.need_since + std::chrono::milliseconds(int(kFirstPickSeconds * 1000));
    REXLOG_INFO("pdz_counterop: start: picks tried once a second from {:.0f} s on, outside cutscenes", kFirstPickSeconds);
  }

  // One pick try (the retry loop): true when player 2 spawned.
  bool TryPick(PPCContext &ctx, uint8_t *base, const char *why)
  {
    PickDiag diag;
    const std::vector<Spot> spots = Spots(ctx, base, diag);
    if (!diag.registry_ok && !g.registry_warned)
    {
      g.registry_warned = true;
      REXLOG_WARN("pdz_counterop: Rare's object registry ([game +28] +20, +676..+680) gave no objects: dormant guards "
                  "can't be found; only the world list ({} objects) is used",
                  diag.world_objects);
    }
    Spot e;
    ++g.tries;
    const char *step = Pick(spots, e);
    if (step)
    {
      if (TakeGuard())
      {
        --diag.eligible;
        ++diag.taken;
        --(e.dormant ? diag.dormant : diag.live);
      }
      SpawnAt(ctx, base, e, diag, step, why);
      return true;
    }
    // One line a second for the first 10 failures, then one every 10 s.
    const auto now = std::chrono::steady_clock::now();
    if (g.diag_lines < 10 || now - g.last_diag >= std::chrono::seconds(10))
    {
      ++g.diag_lines;
      g.last_diag = now;
      REXLOG_INFO("pdz_counterop: pick {} found no spot (try {}): {}; registry objects {}, world objects {}, characters "
                  "{}, SP guards {}; no-id sources: {} clones, {} unreadable or unlisted ids",
                  why, g.tries, CountsText(diag), diag.registry_objects, diag.world_objects, diag.characters,
                  diag.guards, diag.clones, diag.no_id);
    }
    return false;
  }

  // Every frame in a co-op level (after Rare's death states).
  // Testing: makes the next real hit on a player fatal, so Rare's own hit and death path runs (co_jungle: a call of
  // the player handler 825AAF50 left the health alone: that handler is the hit feedback, rumble and screen effects;
  // the shared damage routine 82546BC8 takes stack arguments). The health is set to 0.5; player 2 is also moved 3 m in
  // front of the agent (forward = (sin yaw, cos yaw) of his view yaw, as OnAgentScreen) and turned to face him, so an
  // input script's trigger presses (auto-aim on with a controller) hit him.
  void TestKill(PPCContext &ctx, uint8_t *base, uint32_t victim, uint32_t agent)
  {
    if (victim == g.p2c && Ptr(agent) && g.scratch)
    {
      const float yaw = LoadFloat(base, agent + kViewYaw) * 3.14159265f / 180.0f;
      StoreFloat(base, g.scratch, LoadFloat(base, agent + kPosition) + 300.0f * std::sin(yaw));
      StoreFloat(base, g.scratch + 4, LoadFloat(base, agent + kPosition + 4));
      StoreFloat(base, g.scratch + 8, LoadFloat(base, agent + kPosition + 8) + 300.0f * std::cos(yaw));
      Call(ctx, base, sub_825AD148, g.p2c, g.scratch, 0);
      FaceAgent(ctx, base);
    }
    if (Load32(base, victim) == kHumanVtable && Ptr(Load32(base, victim + kParticipantOf)))
    {
      StoreFloat(base, Load32(base, victim + kParticipantOf) + kPartMaxHealth, 0.5f); // a human's health (Health)
    }
    else
    {
      StoreFloat(base, victim + kHealth, 0.5f);
    }
  }

  // N64 take: player 2 died, so the guard he replaced dies too, through Rare's own kill (the call level scripts make on
  // live guards): stage 1 = Rare's restore of the hidden guard (825818E0(g, 0)) and Rare's script teleport to where
  // player 2 died; stage 2 (the next tick) = Kill(guard, the agent, 5, 1), which applies all its health as damage
  // and fires the setup's on-death event links (82518128: how level scripts hear of a death) and the dead flag
  // (+0x44 bit 8), so group counts, objectives, doors and waves move on as if the agent had killed it. A guard a script
  // made invulnerable: Rare's network kill's tail (die 82550B08, health 0).
  void KillTakenGuardStep(PPCContext &ctx, uint8_t *base)
  {
    const uint32_t c = g.kill_guard;
    const bool usable = Ptr(c) && Load32(base, c) == kGuardVtable &&
                        Load32(base, Load32(base, c) + 0x2E4) == 0x8252DE88 && g.scratch;
    if (!usable || LoadFloat(base, c + kHealth) <= 0.0f || (Load32(base, c + 0x44) & 8))
    {
      REXLOG_INFO("pdz_counterop: replaced guard {:08X}: {} (no kill needed or possible)", c,
                  !usable ? "not a guard with Rare's kill" : "already dead");
      g.taken.erase(c);
      g.kill_guard = 0;
      g.kill_stage = 0;
      return;
    }
    if (g.kill_stage == 0)
    {
      const bool hidden = (Load32(base, c + kHidden) & 0x10) != 0;
      if (hidden)
      {
        Call(ctx, base, sub_825818E0, c, 0); // Rare's restore (world, physics, team lists)
      }
      StoreFloat(base, g.scratch, g.kill_at[0]);
      StoreFloat(base, g.scratch + 4, g.kill_at[1]);
      StoreFloat(base, g.scratch + 8, g.kill_at[2]);
      Call(ctx, base, sub_825AD0E0, c, g.scratch);
      g.taken.erase(c); // no longer kept hidden
      g.kill_stage = 1;
      REXLOG_INFO("pdz_counterop: replaced guard {:08X} '{}': {} and moved to where player 2 died ({:.0f}, {:.0f}, "
                  "{:.0f}); Rare's kill next tick",
                  c, IdChars(SetupIdKey(base, Load32(base, c + kSetup))), hidden ? "restored (Rare's 825818E0(g, 0))" : "in the world",
                  g.kill_at[0], g.kill_at[1], g.kill_at[2]);
      return;
    }
    const float before = LoadFloat(base, c + kHealth);
    Call(ctx, base, sub_8252DE88, c, g.p1c, 5, 1);
    bool tail = false;
    if (LoadFloat(base, c + kHealth) > 0.0f)
    {
      tail = true;
      Call(ctx, base, sub_82550B08, c);
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = c;
      frame.ctx.f1.f64 = 0.0;
      frame.ctx.r5.u64 = 0;
      sub_8252D6A8(frame.ctx, base);
    }
    const uint32_t id = SetupIdKey(base, Load32(base, c + kSetup));
    if (id && g.ids.count(id))
    {
      g.dead_ids.insert(id);
    }
    REXLOG_INFO("pdz_counterop: replaced guard {:08X} '{}' killed through Rare's kill (8252DE88, credited to the agent): "
                "health {:.1f} -> {:.1f}, dead flag +0x44 {}{}; eligible guards left {}",
                c, IdChars(id), before, LoadFloat(base, c + kHealth), (Load32(base, c + 0x44) & 8) ? "set" : "NOT set",
                tail ? " (an invulnerable guard: die + health 0 as Rare's network kill)" : "", EligibleLeft());
    g.kill_guard = 0;
    g.kill_stage = 0;
  }

  void Tick(PPCContext &ctx, uint8_t *base)
  {
    if (!On())
    {
      if (g.started)
      {
        if (CoopLevel(base) && g.out)
        {
          ShowP2(ctx, base); // switched off in the level: player 2 back as Rare's co-op partner
        }
        Reset(base, "pdz_counterop off");
      }
      return;
    }
    const uint32_t so = CoopLevel(base);
    if (!so)
    {
      if (g.started)
      {
        Reset(base, "left the level");
      }
      return;
    }
    uint32_t p1p, p1c, p2p, p2c;
    FindPlayers(base, p1p, p1c, p2p, p2c);
    if (!p1c || !p2c)
    {
      return;
    }
    if (g.started && (so != g.state_object || p2c != g.p2c || p1c != g.p1c))
    {
      // A checkpoint reload or restart (new state object or new characters): start over with the new ones.
      REXLOG_INFO("pdz_counterop: the level was reloaded or a player re-created (state {:08X} -> {:08X}, player 2 {:08X} "
                  "-> {:08X}): re-applying",
                  g.state_object, so, g.p2c, p2c);
      Reset(base, "reload");
    }
    g.state_object = so;
    g.p1p = p1p;
    g.p1c = p1c;
    g.p2p = p2p;
    g.p2c = p2c;
    const bool cutscene = InCutscene(base);
    if (cutscene != g.in_cutscene)
    {
      g.in_cutscene = cutscene;
      if (g.started)
      {
        REXLOG_INFO("pdz_counterop: cutscene {}: player 2 at ({:.0f}, {:.0f}, {:.0f}), health {:.2f}, team bits {:04X}",
                    cutscene ? "starts" : "ends", LoadFloat(base, p2c + kPosition), LoadFloat(base, p2c + kPosition + 4),
                    LoadFloat(base, p2c + kPosition + 8), Health(base, p2c), Load16(base, p2c + kTeamBits));
      }
    }
    if (!g.started)
    {
      if (!cutscene && Alive(ctx, base, p1c) && Alive(ctx, base, p2c))
      {
        Start(ctx, base);
      }
      return;
    }
    if (g.unsupported)
    {
      return;
    }
    if (g.hide_pending)
    {
      if (!g.need_guard || g.out)
      {
        g.hide_pending = false; // a spot was taken first (or he is already out): nothing to hide
      }
      else if (Ptr(g.p2_mode) || std::chrono::steady_clock::now() - g.hide_pending_since >=
                                         std::chrono::milliseconds(int(kHideWaitSeconds * 1000)))
      {
        g.hide_pending = false;
        REXLOG_INFO("pdz_counterop: start: player 2's camera {} ({:.1f} s after the start)",
                    Ptr(g.p2_mode) ? fmt::format("{:08X} noted: his view follows the agent while out", g.p2_mode)
                                   : std::string("not seen: his view may stay black while out"),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - g.hide_pending_since).count());
        HideP2(ctx, base, "until his first spot");
      }
    }
    // Testing tools (pdz_counterop_test_kill_p2 / _agent): a killing hit through Rare's player damage handler (the
    // original, so pdz_test_god_mode doesn't stop it), so Rare's own death path runs. The counts live outside State,
    // so a reload doesn't repeat them.
    {
      static int32_t kills_done = 0;
      static bool agent_done = false;
      const int32_t want = REXCVAR_GET(pdz_counterop_test_kill_p2);
      if (!cutscene && want > kills_done && !g.spectating && !g.out && !g.revive_participant && !g.wait_getup &&
          Alive(ctx, base, p2c))
      {
        ++kills_done;
        REXLOG_INFO("pdz_counterop: test kill {} (value {}): player 2 health {:.2f}", kills_done, want,
                    Health(base, p2c));
        TestKill(ctx, base, p2c, p1c);
      }
      const bool kill_agent = REXCVAR_GET(pdz_counterop_test_kill_agent);
      if (!cutscene && kill_agent && !agent_done && Alive(ctx, base, p1c))
      {
        agent_done = true;
        REXLOG_INFO("pdz_counterop: test kill of the agent: health {:.2f}", Health(base, p1c));
        TestKill(ctx, base, p1c, p2c);
      }
      if (!kill_agent)
      {
        agent_done = false;
      }
    }
    // Player 2 down: the next spot after the delay (Rare's revive first: it brings him back).
    if (g.revive_participant && std::chrono::steady_clock::now() >= g.revive_at)
    {
      const uint32_t participant = g.revive_participant;
      g.revive_participant = 0;
      // Rare's revive brings him back alive; then the next spot, or out of the world while picks are retried.
      g.allow_revive = true;
      {
        rex::CallFrame frame(ctx);
        frame.ctx.r3.u64 = participant;
        __imp__sub_824573A8(frame.ctx, base);
      }
      g.allow_revive = false;
      g.tries = 0;
      g.diag_lines = 0;
      // Rare's revive is the only un-die (dead flag +64 bit 2, health, hands, animation state), but it also moves
      // him to his revive pad and starts Rare's get-up (animation, invulnerability). co_jungle_3: hidden at once, his
      // update didn't run and the get-up never ended (death state 2 at the 8 s cap). So he stays in the world and the
      // next spot is picked in this same frame: he gets up there (Rare's get-up finishes on his own updates). Only
      // when no spot is free is he hidden while the picks are retried.
      REXLOG_INFO("pdz_counterop: get-up: Rare's revive ran (death state {}, get-up flag {}, timer {:.2f}, "
                  "invulnerable {:.2f}, health {:.1f}, at ({:.0f}, {:.0f}, {:.0f}) = his revive pad); the next spot "
                  "now, where he gets up",
                  int32_t(Load32(base, participant + kPartDeathState)), int(base[p2c + kGetupFlag]),
                  LoadFloat(base, p2c + kGetupTimer), LoadFloat(base, participant + kPartInvulnerable),
                  LoadFloat(base, p2c + kHealth), LoadFloat(base, p2c + kPosition),
                  LoadFloat(base, p2c + kPosition + 4), LoadFloat(base, p2c + kPosition + 8));
      const auto now_revive = std::chrono::steady_clock::now();
      g.need_guard = true;
      g.after_death = true;
      g.need_since = now_revive;
      g.next_try = now_revive; // the retry block below runs in this tick
      g.hide_if_no_spot = true;
      g.kill_ready = g.kill_guard != 0; // the revive moved player 2 away: the replaced guard's kill can start
    }
    if (g.kill_guard && g.kill_ready && !cutscene)
    {
      KillTakenGuardStep(ctx, base);
      if (!g.kill_guard)
      {
        g.kill_ready = false;
      }
    }
    // Rare's get-up over (death state back to 0, flag clear, no invulnerability), or the cap: then the pick.
    if (g.wait_getup)
    {
      const auto now = std::chrono::steady_clock::now();
      const int32_t state = int32_t(Load32(base, p2p + kPartDeathState));
      const uint8_t flag = base[p2c + kGetupFlag];
      const float timer = LoadFloat(base, p2c + kGetupTimer), invulnerable = LoadFloat(base, p2p + kPartInvulnerable);
      const bool over = state == 0 && !flag && !(invulnerable > 0.0f);
      const bool cap = now - g.getup_since >= std::chrono::milliseconds(int(kGetupCapSeconds * 1000));
      if (over || cap)
      {
        if (!over)
        {
          // Rare's own countdowns end it on his next update once both are 0 (8254CD7C: timer <= 0 -> 82457B40; the
          // invulnerability is only ever counted down).
          if (flag)
          {
            StoreFloat(base, p2c + kGetupTimer, 0.0f);
          }
          StoreFloat(base, p2p + kPartInvulnerable, 0.0f);
        }
        REXLOG_INFO("pdz_counterop: get-up: {} after {:.1f} s (death state {}, flag {}, timer {:.2f}, invulnerable "
                    "{:.2f}){}; next spot",
                    over ? "over" : "NOT over at the cap", std::chrono::duration<double>(now - g.getup_since).count(),
                    state, int(flag), timer, invulnerable,
                    over ? "" : ": the get-up timer and the invulnerability set to 0 (Rare's update ends the get-up)");
        g.wait_getup = false;
        g.need_guard = true;
        g.after_death = true;
        g.need_since = now;
        g.next_try = now;
      }
    }
    // The retries: once a second outside cutscenes; after a death, out of guards after kOutOfGuardsSeconds.
    if (g.need_guard && !cutscene && std::chrono::steady_clock::now() >= g.next_try)
    {
      const auto now = std::chrono::steady_clock::now();
      g.next_try = now + std::chrono::milliseconds(int(kRetrySeconds * 1000));
      if (TakeGuard() && g.after_death && EligibleLeft() == 0)
      {
        // N64: no eligible guard left = player 2 is out for the rest of the mission (spectating the agent).
        g.need_guard = false;
        g.spectating = true;
        if (!g.out)
        {
          HideP2(ctx, base, "no eligible guard left: spectating the agent");
        }
        REXLOG_INFO("pdz_counterop: no eligible guard left (of {} listed: {} replaced, {} seen dead{}): player 2 is out "
                    "for the rest of the mission, spectating the agent",
                    g.ids.size(), g.taken_ids.size(), g.dead_ids.size(),
                    TestMaxLives() > 0 ? fmt::format("; test cap {} lives", TestMaxLives()) : std::string());
      }
      else if (TryPick(ctx, base, g.after_death ? "after his death" : "first spot"))
      {
        g.need_guard = false;
        g.diag_lines = 0;
        g.hide_if_no_spot = false;
      }
      else if (g.hide_if_no_spot)
      {
        // Revived but no free spot yet: out of sight (spectating) while the picks are retried.
        g.hide_if_no_spot = false;
        if (!g.out)
        {
          HideP2(ctx, base, "revived, no spot free yet: out of sight while the picks are retried");
        }
      }
      else if (g.after_death && !TakeGuard() &&
               now - g.need_since >= std::chrono::milliseconds(int(kOutOfGuardsSeconds * 1000)))
      {
        // (Take mode never gives up by time: while eligible guards are left, e.g. ones the level hasn't built yet,
        // player 2 spectates and the picks go on; EligibleLeft() == 0 ends it above.)
        g.need_guard = false;
        g.spectating = true;
        REXLOG_INFO("pdz_counterop: out of guards ({:.0f} s with no spot left): player 2 stays out of the world "
                    "(spectating); the mission goes on",
                    kOutOfGuardsSeconds);
      }
    }
    // The N64 share stays the most he has: Rare's co-op health regeneration took him back up to a human's full bar
    // (co_jungle_6: 0.5 -> 100.5); it may still refill him up to his share.
    if (g.set_health > 0.0f && !g.out && !g.revive_participant && Alive(ctx, base, p2c) && Ptr(p2p) &&
        Health(base, p2c) > g.set_health + 0.01f)
    {
      StoreFloat(base, p2p + kPartMaxHealth, g.set_health);
      if (++g.health_clamps == 1)
      {
        REXLOG_INFO("pdz_counterop: player 2's health held at his share {:.1f} (Rare's co-op regeneration raises a "
                    "human above it); logged once",
                    g.set_health);
      }
    }
    // Taken guards stay hidden.
    if (!g.taken.empty())
    {
      KeepTakenHidden(ctx, base);
    }
    // Health check 5 s after a spawn: what regen or a reset did to the health and max he was given.
    if (g.health_check && std::chrono::steady_clock::now() >= g.health_check_at)
    {
      g.health_check = false;
      const uint32_t participant = Load32(base, p2c + kParticipantOf);
      const float now_health = Health(base, p2c);
      const float now_max = Ptr(participant) ? LoadFloat(base, participant + kPartMaxHealth) : -1.0f;
      REXLOG_INFO("pdz_counterop: 5 s after the spawn: health {:.1f} (set {:.1f}), max {} (set {:.1f}){}; armour {:.1f}; "
                  "body {} head {}; at ({:.0f}, {:.0f}, {:.0f}); death state {}, get-up flag {}, invulnerable {:.2f}{}",
                  now_health, g.set_health, now_max >= 0.0f ? fmt::format("{:.1f}", now_max) : std::string("?"),
                  g.set_health,
                  now_health > g.set_health + 0.5f ? " (regenerated above the share: the max is not the regen cap)" : "",
                  LoadFloat(base, p2c + kArmour), Load32(base, p2c + kBodyId), Load32(base, p2c + kHeadId),
                  LoadFloat(base, p2c + kPosition), LoadFloat(base, p2c + kPosition + 4),
                  LoadFloat(base, p2c + kPosition + 8), int32_t(Load32(base, p2p + kPartDeathState)),
                  int(base[p2c + kGetupFlag]), LoadFloat(base, p2p + kPartInvulnerable),
                  (base[p2c + kGetupFlag] || LoadFloat(base, p2p + kPartInvulnerable) > 0.0f)
                      ? " (WARNING: Rare's get-up started after the spawn: he is invulnerable for a while)"
                      : "");
    }
    // Spectate camera (log 306): one line 3 s after he went out (outside cutscenes), whether it works.
    if (g.out && !g.spectate_reported && !cutscene &&
        std::chrono::steady_clock::now() - g.out_since >= std::chrono::seconds(3))
    {
      g.spectate_reported = true;
      if (g.spectate_writes)
      {
        REXLOG_INFO("pdz_counterop: spectate: player 2's camera updated {} times in his first 3 s out of the world; "
                    "it follows the agent ({:.1f} m behind, {:.1f} m above his eye, looking where he looks; the agent's "
                    "camera updated {} times){}",
                    g.spectate_writes, kSpectateBack / 100.0f, kSpectateUp / 100.0f, g.p1_cam_updates,
                    REXCVAR_GET(pdz_counterop_fix_out_freeze)
                        ? fmt::format("; pdz_counterop_fix_out_freeze: {} calls with Rare's step, {} leftover step "
                                      "values that were not a step (largest finite {:.3g}), {} angle resets",
                                      g.spectate_calls, g.bad_leftover, g.max_leftover, g.angle_resets)
                        : std::string("; pdz_counterop_fix_out_freeze OFF (the leftover step register is passed on)"));
      }
      else
      {
        REXLOG_WARN("pdz_counterop: spectate: player 2's camera was not updated by Rare's gameplay camera in his first "
                    "3 s out of the world (his updates {}, the agent's {}, agent camera seen {}): his view can't "
                    "follow the agent this way (likely still black)",
                    g.p2_cam_updates, g.p1_cam_updates, g.agent_cam ? "yes" : "no");
      }
    }
    // Player 2's masks: re-applied whenever something reset them (a revive, Rare's betrayal switch, a script).
    if (!g.spectating && !g.out && !g.revive_participant && !g.wait_getup && Alive(ctx, base, p2c) &&
        !TeamApplied(base, p2c))
    {
      if (++g.team_fixes <= 8 || g.team_fixes % 100 == 0)
      {
        REXLOG_INFO("pdz_counterop: player 2's team bits were {:04X} / allies {:04X} / enemies {:04X} (+6276 {:08X}): "
                    "re-applied ({} time(s))",
                    Load16(base, p2c + kTeamBits), Load16(base, p2c + kAllies), Load16(base, p2c + kEnemies),
                    Load32(base, p2c + kBetrayal), g.team_fixes);
      }
      ApplyTeam(base, p2c);
    }
    // Probe: what player 2 holds (a pick-up shows here; picking up and objectives are not blocked yet).
    const uint32_t weapon = HandWeaponType(base, p2c);
    if (weapon != g.last_weapon)
    {
      if (g.last_weapon != ~0u)
      {
        REXLOG_INFO("pdz_counterop: player 2's weapon in hand: type {} -> {}", g.last_weapon, weapon);
      }
      g.last_weapon = weapon;
    }
  }

  // ---- The look (at level load, in Rare's player factory) -----------------------------------------------------

  // A list entry (body or head) by id, found by walking the level's list (Rare's 82278788 / 822787E0 fall back to
  // body 86 / head 121, or worse, for a missing id).
  uint32_t ListEntry(const uint8_t *base, uint32_t which, uint32_t id)
  {
    const uint32_t lists = Load32(base, kGame + 8);
    const uint32_t list = Ptr(lists) ? Load32(base, lists + which) : 0;
    const int32_t count = Ptr(list) ? int32_t(Load32(base, list + kListCount)) : 0;
    const uint32_t entries = Ptr(list) ? Load32(base, list + kListEntries) : 0;
    for (int32_t i = 0; Ptr(entries) && i < count && i < 512; ++i)
    {
      const uint32_t e = Load32(base, entries + uint32_t(i) * 4);
      if (Ptr(e) && Load32(base, e) == id)
      {
        return e;
      }
    }
    return 0;
  }

  // The player hands 82414070 gives a body for a human (flag 1): its hands name (+16), else Rare's trooper hands;
  // or, when the body is already loaded (+68), what it holds at +20 (0 after a guard's flag-0 load).
  uint32_t LoaderHands(PPCContext &ctx, uint8_t *base, uint32_t entry, bool &loaded)
  {
    loaded = Load32(base, entry + kBodyAssets) != 0;
    if (loaded)
    {
      return Load32(base, entry + kBodyHands);
    }
    return Call(ctx, base, sub_82413FD8, Load32(base, entry + kBodyHandsRef), kHandsFallback, 0, 0);
  }

  // Can the level build a human with this body, without a loader crash? Nothing is requested: Rare's own lookup
  // (82413FD8, null when missing) only. Its list entry, an animation set humans are built with (+64 from the +12
  // name), the model (+4) and the ragdoll (+32, Jack's fallback) with their +8 (82414070 reads both unchecked), and
  // player hands: its own (the loader's way) or else the co-op body's (fill_hands, see Look).
  bool BodyUsable(PPCContext &ctx, uint8_t *base, uint32_t body, uint32_t coop_entry, std::string &why)
  {
    const uint32_t entry = ListEntry(base, kListsBody, body);
    if (!Ptr(entry))
    {
      why = "not in the level's body list";
      return false;
    }
    const uint32_t model_ref = Load32(base, entry + kBodyModelRef), third_ref = Load32(base, entry + kBodyThirdRef);
    if (!Ptr(model_ref) || !Ptr(third_ref))
    {
      why = fmt::format("entry {:08X} lacks a reference (model {:08X}, ragdoll {:08X})", entry, model_ref, third_ref);
      return false;
    }
    const uint32_t anim = Load32(base, entry + kBodyAnimSet);
    const std::string anim_name = GuestString(base, Load32(base, entry + kBodyAnimName));
    const bool coop_anim = Ptr(coop_entry) && anim == Load32(base, coop_entry + kBodyAnimSet);
    bool human_anim = false;
    for (const char *set : kHumanAnimSets)
    {
      human_anim = human_anim || anim_name == set;
    }
    if (!Ptr(anim) || !(coop_anim || human_anim))
    {
      why = fmt::format("entry {:08X}: animation set +64 {:08X} ('{}') is not one Rare builds humans with", entry, anim,
                        anim_name);
      return false;
    }
    const bool loaded = Load32(base, entry + kBodyAssets) != 0;
    uint32_t model = 0, third = 0;
    if (loaded)
    {
      model = Load32(base, entry + kBodyAssets);
      third = Load32(base, entry + kBodyThirdRef + 4); // +36, the loader's ragdoll
    }
    else
    {
      // Rare's constructor will call 82414070(entry, 1): the same lookups first (the model strictly, without the
      // placeholder, so player 2 never wears the placeholder body).
      model = Call(ctx, base, sub_82413FD8, model_ref, 0, 0, 0);
      third = Call(ctx, base, sub_82413FD8, third_ref, kRagdollFallback, 0, 0);
    }
    if (!Ptr(model) || !Ptr(Load32(base, model + 8)))
    {
      why = fmt::format("entry {:08X}: its model '{}' is not in the level's packages (or its +8 is empty)", entry,
                        GuestString(base, model_ref));
      return false;
    }
    if (!Ptr(third) || !Ptr(Load32(base, third + 8)))
    {
      why = fmt::format("entry {:08X}: its ragdoll '{}' (or Jack's fallback) is not in the level's packages, or its "
                        "+8 is empty (the loader reads it unchecked)",
                        entry, GuestString(base, third_ref));
      return false;
    }
    // Player hands: what the entry will hold at +20 once Rare's constructor ran (a loaded entry keeps its +20).
    bool unused = false;
    const uint32_t own = LoaderHands(ctx, base, entry, unused);
    uint32_t hands = own;
    std::string hands_why = Ptr(own) ? fmt::format("its own player hands {:08X}", own) : std::string();
    if (!Ptr(hands) && Ptr(coop_entry))
    {
      bool coop_loaded = false;
      hands = LoaderHands(ctx, base, coop_entry, coop_loaded);
      if (Ptr(hands))
      {
        hands_why = fmt::format("no player hands of its own (+16 '{}', no trooper hands in this level): the co-op "
                                "body's {:08X} ('{}', {}), written to +20 {} Rare's call",
                                GuestString(base, Load32(base, entry + kBodyHandsRef)), hands,
                                GuestString(base, Load32(base, coop_entry + kBodyHandsRef)),
                                coop_loaded ? "its +20" : "looked up", loaded ? "before" : "after");
      }
    }
    if (!Ptr(hands))
    {
      why = fmt::format("entry {:08X}: no player hands: neither its own ('{}'), the trooper hands nor the co-op "
                        "body's are in the level's packages (log 312: null first-person hands)",
                        entry, GuestString(base, Load32(base, entry + kBodyHandsRef)));
      return false;
    }
    if (!Ptr(own))
    {
      g_look.fill_entry = entry;
      g_look.fill_hands = hands;
    }
    g_look.hands_why = hands_why;
    why = fmt::format("entry {:08X} ('{}'): {}, animation '{}'{}, model {:08X}, ragdoll {:08X}; hands: {}", entry,
                      GuestString(base, model_ref), loaded ? "already loaded by Rare" : "loaded by Rare's constructor",
                      anim_name, coop_anim ? " (the co-op body's)" : " (not the co-op body's; a human set)", model,
                      third, hands_why);
    return true;
  }

  // Can the constructor attach this head? Its entry in the head list (822787E0 would give head 121 or 0 otherwise),
  // its model (+12 when loaded, else +4 looked up strictly: 82414618 would fall back to the placeholder head) and the
  // model's +8 (82411048 reads it unchecked).
  bool HeadUsable(PPCContext &ctx, uint8_t *base, uint32_t head, std::string &why)
  {
    const uint32_t entry = ListEntry(base, kListsHead, head);
    if (!Ptr(entry))
    {
      why = "not in the level's head list";
      return false;
    }
    uint32_t model = Load32(base, entry + kHeadAssets);
    const bool loaded = Ptr(model);
    if (!loaded)
    {
      model = Call(ctx, base, sub_82413FD8, Load32(base, entry + kHeadModelRef), 0, 0, 0);
    }
    if (!Ptr(model) || !Ptr(Load32(base, model + 8)))
    {
      why = fmt::format("entry {:08X}: its model '{}' is not in the level's packages (or its +8 is empty)", entry,
                        GuestString(base, Load32(base, entry + kHeadModelRef)));
      return false;
    }
    why = fmt::format("entry {:08X} ('{}'), model {:08X} {}", entry,
                      GuestString(base, Load32(base, entry + kHeadModelRef)), model,
                      loaded ? "already loaded" : "loaded by Rare's constructor");
    return true;
  }

  // The look for player 2 in this mission: the first (body, head) pair of kLook the level can build for a human.
  // Fills g_look; true when a body was chosen (the head then is the pair's, or the co-op head if it fails).
  bool ChooseLook(PPCContext &ctx, uint8_t *base, uint32_t setup)
  {
    g_look = Look{};
    g_look.decided = true;
    std::string raw;
    g_look.mission = pdz::pace::SpLevelName(base, &raw);
    g_look.coop_body = Load32(base, setup + kDescBody);
    g_look.coop_head = Load32(base, setup + kDescHead);
    g_look.head = g_look.coop_head;
    const MissionLook *row = nullptr;
    for (const MissionLook &m : kLook)
    {
      if (g_look.mission == m.mission)
      {
        row = &m;
      }
    }
    if (!row)
    {
      g_look.why = fmt::format("no guard look for '{}' (no placed guards: Datacore / Arena, or not a campaign "
                               "mission): the co-op body {} / head {} kept, nothing requested",
                               g_look.mission, g_look.coop_body, g_look.coop_head);
      return false;
    }
    const uint32_t coop_entry = ListEntry(base, kListsBody, g_look.coop_body);
    std::string tried;
    for (const LookPair &p : row->pairs)
    {
      if (!p.body)
      {
        break;
      }
      std::string why;
      if (p.body == kPlaceholderBody)
      {
        why = "the placeholder body";
      }
      else if (!BodyUsable(ctx, base, p.body, coop_entry, why))
      {
        g_look.fill_entry = g_look.fill_hands = 0;
      }
      else
      {
        g_look.chosen = true;
        g_look.body = p.body;
        std::string head_why;
        if (HeadUsable(ctx, base, p.head, head_why))
        {
          g_look.head = p.head;
        }
        else
        {
          head_why = fmt::format("the guard head {} can't be attached ({}): the co-op head {} kept", p.head, head_why,
                                 g_look.coop_head);
        }
        g_look.why = fmt::format("guard body {}: {}; head {}: {}{}", p.body, why, g_look.head, head_why,
                                 tried.empty() ? "" : "; rejected first: " + tried);
        return true;
      }
      tried += fmt::format("{}body {}: {}", tried.empty() ? "" : "; ", p.body, why);
    }
    g_look.why = fmt::format("no guard body of {} can be built for a human ({}): the co-op body {} / head {} kept, "
                             "nothing requested",
                             g_look.mission, tried.empty() ? "no candidates" : tried, g_look.coop_body,
                             g_look.coop_head);
    return false;
  }
} // namespace

// minigame_log.cpp's god mode (pdz_test_god_mode, testing): it spares the agent only, not player 2.
bool PdzCounterOpIsPlayer2(uint32_t character)
{
  return character && character == g.p2c && g.started && On();
}

// counter_op_menu.cpp's debriefing text: the level that just ended was played as counter-op.
bool PdzCounterOpPlayed()
{
  return g_played && On();
}

// counter_op_menu.cpp: clone missions (Datacore, Arena) are offered (pdz_counterop_clones).
bool PdzCounterOpClonesOn()
{
  return REXCVAR_GET(pdz_counterop_clones);
}

// Stage 1: Rare's setup filter (sub_82279928, hooked in cut_scenarios.cpp) with the counter-op kind bit. rare: Rare's
// answer (co-op kind 0x20). In a co-op game with pdz_counterop: the mask must hold the 2-player bit and 0x40.
bool PdzCounterOpFilter(const uint8_t *base, uint32_t setup, bool rare)
{
  if (!On() || !Ptr(setup))
  {
    return rare;
  }
  const uint32_t so = Load32(base, kGame);
  if (!Ptr(so) || Load32(base, so + 40) != 1)
  {
    return rare;
  }
  if (!g.started && !g_filter_seen)
  {
    g_filter_added = g_filter_removed = 0;
  }
  const uint32_t mask = Load32(base, kFilterTable + uint32_t(base[setup + kSetupFilter]) * 8 + 4);
  const bool counter = (mask & 0x2u) && (mask & 0x40u);
  ++g_filter_seen;
  g_filter_added += counter && !rare;
  g_filter_removed += !counter && rare;
  return counter;
}

// sub_825B3D58(setup): Rare's SP player factory. Player 2's call (player 1 exists with no next human, +6792: the
// only case Rare builds in co-op; a co-op game, pdz_counterop and pdz_counterop_guard_look on): the chosen guard
// body / head ids are written into his setup for the duration of Rare's call and restored, so Rare's own constructor
// builds him with them; the body's player hands (+20) are filled from the co-op body's when the guard body has none.
REX_HOOK_RAW(sub_825B3D58)
{
  const uint32_t setup = ctx.r3.u32;
  const uint32_t so = Load32(base, kGame);
  const uint32_t one = Load32(base, kPlayerOne);
  // Player 2's call: a co-op game, player 1 built, and not player 1's own setup (a rebuilt player 1 keeps her look).
  if (!On() || !REXCVAR_GET(pdz_counterop_guard_look) || !Ptr(setup) || !Ptr(so) || Load32(base, so + 40) != 1 ||
      !Ptr(one) || Load32(base, one + kSetup) == setup || Load32(base, one + kP1Partner) != 0)
  {
    __imp__sub_825B3D58(ctx, base);
    return;
  }
  bool chosen = false;
  {
    PPCContext saved = ctx;
    chosen = ChooseLook(saved, base, setup);
  }
  if (!chosen)
  {
    REXLOG_INFO("pdz_counterop: look: player 2 keeps the co-op look in {}: {}", g_look.mission, g_look.why);
    __imp__sub_825B3D58(ctx, base);
    return;
  }
  const uint32_t old_body = Load32(base, setup + kDescBody), old_head = Load32(base, setup + kDescHead);
  if (Ptr(g_look.fill_entry) && Ptr(g_look.fill_hands) && Load32(base, g_look.fill_entry + kBodyAssets) &&
      !Ptr(Load32(base, g_look.fill_entry + kBodyHands)))
  {
    // D15: log 312's cause (see Look::fill_hands). Rare's loader would store exactly this with flag 1.
    Store32(base, g_look.fill_entry + kBodyHands, g_look.fill_hands);
    REXLOG_INFO("pdz_counterop: look: hands: body {}'s list entry {:08X} is loaded without player hands; {:08X} "
                "written to +20 before Rare's player factory ({})",
                g_look.body, g_look.fill_entry, g_look.fill_hands, g_look.hands_why);
  }
  Store32(base, setup + kDescBody, g_look.body);
  Store32(base, setup + kDescHead, g_look.head);
  REXLOG_INFO("pdz_counterop: look: player 2 is built with body {} in {}: setup {:08X} body {} -> {}, head {} -> {} for "
              "Rare's player factory ({})",
              g_look.body, g_look.mission, setup, old_body, g_look.body, old_head, g_look.head, g_look.why);
  __imp__sub_825B3D58(ctx, base);
  Store32(base, setup + kDescBody, old_body);
  Store32(base, setup + kDescHead, old_head);
  const uint32_t human = ctx.r3.u32;
  if (Ptr(g_look.fill_entry) && Ptr(g_look.fill_hands) && !Ptr(Load32(base, g_look.fill_entry + kBodyHands)))
  {
    // The body was not loaded before the call: the constructor's 82414070(entry, 1) found no hands for it and stored
    // 0. The first-person hands are made from +20 later, when his participant attaches (825A99D8 -> 825B43E0).
    Store32(base, g_look.fill_entry + kBodyHands, g_look.fill_hands);
    REXLOG_INFO("pdz_counterop: look: hands: body {}'s list entry {:08X} got no player hands from Rare's loader; "
                "{:08X} written to +20 after the factory, before his participant attaches ({})",
                g_look.body, g_look.fill_entry, g_look.fill_hands, g_look.hands_why);
  }
  const uint32_t built_entry = Ptr(human) ? Load32(base, human + kBodyEntryOf) : 0;
  REXLOG_INFO("pdz_counterop: look: Rare's factory returned {:08X}{}; the setup's ids restored", human,
              Ptr(human) ? fmt::format(" (body {}, head {}, body instance {:08X}, head instance {:08X}, body entry "
                                       "{:08X} hands +20 {:08X}, hair bones {} / {})",
                                       Load32(base, human + kBodyId), Load32(base, human + kHeadId),
                                       Load32(base, human + 72), Load32(base, human + 4104), built_entry,
                                       Ptr(built_entry) ? Load32(base, built_entry + kBodyHands) : 0,
                                       int32_t(Load32(base, human + 4852)), int32_t(Load32(base, human + 4856)))
                         : std::string(" (none)"));
  if (Ptr(human) && (Load32(base, human + kBodyId) != g_look.body || Load32(base, human + kHeadId) != g_look.head ||
                     !Ptr(Load32(base, human + 72)) || !Ptr(Load32(base, human + 4104))))
  {
    REXLOG_WARN("pdz_counterop: look: player 2 is not built as asked (body {} / head {} asked; body {} / head {}, "
                "instances {:08X} / {:08X})",
                g_look.body, g_look.head, Load32(base, human + kBodyId), Load32(base, human + kHeadId),
                Load32(base, human + 72), Load32(base, human + 4104));
  }
}

// sub_8222F740: the co-op death states, every frame in a level. Rare's first, then the counter-op tick.
REX_HOOK_RAW(sub_8222F740)
{
  __imp__sub_8222F740(ctx, base);
  if (On())
  {
    PPCContext saved = ctx;
    Tick(saved, base);
  }
  else if (g.started)
  {
    PPCContext saved = ctx;
    Tick(saved, base); // puts player 2 back and resets
  }
}

// sub_8222FD98: co-op "game over?" (every human down). Counter-op: the agent down is enough.
REX_HOOK_RAW(sub_8222FD98)
{
  __imp__sub_8222FD98(ctx, base);
  if (!On() || !g.started || g.unsupported || (ctx.r3.u32 & 0xFF) || !Ptr(g.p1p) ||
      Load32(base, g.p1p + kPartDeathState) != 2)
  {
    return;
  }
  const uint8_t state_before = base[kLevelState];
  base[kLevelState] |= 2;
  PPCContext saved = ctx;
  Call(saved, base, sub_8222FF58, 0); // Rare's own all-down path calls it every frame too (8222FF20 from 8222FB48)
  if (!g.game_over_logged)
  {
    g.game_over_logged = true;
    REXLOG_INFO("pdz_counterop: the agent is down: mission failed (Rare's co-op game over: level state {:02X} -> "
                "{:02X}, Rare's own answer was 'not all down'; returning 'game over' so Rare's death states skip his "
                "revive, 8222FAD8); player 2 lives {}, his death state {}",
                int(state_before), int(base[kLevelState]), g.lives,
                Ptr(g.p2p) ? int32_t(Load32(base, g.p2p + kPartDeathState)) : -1);
  }
  ctx.r3.u64 = 1;
}

// sub_824573A8(participant): Rare's co-op revive, right after a human's death animation. Counter-op: player 2's is
// held 2.5 s (he watches where he died), then run by the tick and followed by the next spot.
REX_HOOK_RAW(sub_824573A8)
{
  const uint32_t participant = ctx.r3.u32;
  if (On() && g.started && !g.unsupported && !g.allow_revive && participant == g.p1p && Ptr(g.p1p) &&
      Load32(base, g.p1p + kPartDeathState) == 2)
  {
    // The agent is never revived (the game-over hook above already keeps Rare's death states from asking).
    if (++g.agent_revives_blocked <= 3)
    {
      REXLOG_INFO("pdz_counterop: a revive of the dead agent was asked (from {:08X}): not run (mission failed)",
                  uint32_t(ctx.lr));
    }
    return;
  }
  if (!On() || !g.started || g.unsupported || g.allow_revive || participant != g.p2p)
  {
    __imp__sub_824573A8(ctx, base);
    return;
  }
  if (g.revive_participant || g.wait_getup)
  {
    REXLOG_INFO("pdz_counterop: player 2's revive asked again (from {:08X}) while {}: ignored", uint32_t(ctx.lr),
                g.wait_getup ? "getting up" : "already held");
    return;
  }
  g.revive_participant = participant;
  g.revive_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(kNextGuardSeconds * 1000));
  // N64 take: the guard this life replaced dies now, where player 2 died (Rare's death path, next tick).
  if (TakeGuard() && Ptr(g.current_guard) && Ptr(g.p2c))
  {
    g.kill_guard = g.current_guard;
    g.current_guard = 0;
    for (uint32_t i = 0; i < 3; ++i)
    {
      g.kill_at[i] = LoadFloat(base, g.p2c + kPosition + i * 4);
    }
  }
  REXLOG_INFO("pdz_counterop: player 2 is down (life {}, revive asked from {:08X}; 8222FAE4 = Rare's death states): "
              "Rare's revive held {:.1f} s, then run out of sight",
              g.lives, uint32_t(ctx.lr), kNextGuardSeconds);
}

// sub_82586238(item, character, 4 out bytes): Rare's "can this character pick this item up?", shared by the weapon
// pick-up (virtual 82584740, which needs out 2) and the other pick-up checks (825845F8, 82554D90, bots 8253ADA8). Its
// own "no" path (825862A4..825862B8) stores 0 in all four bytes; for player 2 that answer is given without asking.
REX_HOOK_RAW(sub_82586238)
{
  const uint32_t character = ctx.r4.u32;
  // g_giving: our own give of the guard's gun at each spawn goes through this test too (log co_jungle: "give returned
  // none" while it was blocked).
  if (character != g.p2c || !g.started || g.unsupported || !Ptr(character) || !On() || g_giving ||
      !REXCVAR_GET(pdz_counterop_block_pickups))
  {
    __imp__sub_82586238(ctx, base);
    return;
  }
  for (const uint32_t out : {ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32})
  {
    if (Ptr(out))
    {
      base[out] = 0;
    }
  }
  const uint32_t item = ctx.r3.u32;
  if (++g.pickups_blocked <= 10 || g.pickups_blocked % 500 == 0)
  {
    REXLOG_INFO("pdz_counterop: pickup blocked for player 2 ({}): item {:08X} (+12 {}), asked from {:08X}",
                g.pickups_blocked, item, Ptr(item) ? int32_t(Load32(base, item + kItemType)) : -1, uint32_t(ctx.lr));
  }
}

// sub_82549040(victim, hit, attacker, ...): a character's hit handler (co-op: friend(victim, attacker) at 82549AB0
// cancels the damage). Counter-op probe, nothing changed: every hit between player 2 and anyone, and the agent's
// hits, with the health before / after, so a test shows each rule of the sides (rate-limited).
REX_HOOK_RAW(sub_82549040)
{
  const uint32_t victim = ctx.r3.u32, attacker = ctx.r5.u32;
  if (!g.started || g.unsupported || !Ptr(g.p2c) || (victim != g.p2c && attacker != g.p2c) || !Ptr(victim) || !On())
  {
    __imp__sub_82549040(ctx, base);
    return;
  }
  const float before = Health(base, victim), armour_before = LoadFloat(base, victim + kArmour);
  __imp__sub_82549040(ctx, base);
  const float after = Health(base, victim), armour_after = LoadFloat(base, victim + kArmour);
  if (++g.hit_lines > kHitLogFirst && g.hit_lines % kHitLogEvery != 0)
  {
    return;
  }
  auto who = [&](uint32_t c) -> std::string {
    if (!Ptr(c))
    {
      return "none";
    }
    if (c == g.p1c)
    {
      return "the agent";
    }
    if (c == g.p2c)
    {
      return "player 2";
    }
    const uint32_t vt = Load32(base, c);
    if (vt == kGuardVtable)
    {
      return fmt::format("guard {:08X} '{}' (masks {:04X}/{:04X}/{:04X})", c,
                         IdChars(SetupIdKey(base, Load32(base, c + kSetup))), Load16(base, c + kTeamBits),
                         Load16(base, c + kAllies), Load16(base, c + kEnemies));
    }
    return fmt::format("object {:08X} (vtable {:08X})", c, vt);
  };
  REXLOG_INFO("pdz_counterop: hit #{}: {} hit by {}: health {:.1f} -> {:.1f}, armour {:.1f} -> {:.1f}{}", g.hit_lines,
              who(victim), who(attacker), before, after, armour_before, armour_after,
              after < before || armour_after < armour_before ? "" : " (no damage)");
}

// sub_82214C48 (TU3 of 8220D9C8, third_person.cpp): the gameplay camera's update (r3 = camera mode, r4 = the target
// character). Counter-op, while player 2 is out of the world (log 306: his view was black): the agent's camera is
// noted, and player 2's is put 1.2 m behind and 0.3 m above the agent's eye, looking at the agent's look point. Rare's
// update runs first in both cases; nothing changes outside counter-op.
REX_HOOK_RAW(sub_82214C48)
{
  const uint32_t mode = ctx.r3.u32, target = ctx.r4.u32;
  const double step = ctx.f1.f64; // Rare's frame step for this update (f1: the rates below are multiplied by it)
  __imp__sub_82214C48(ctx, base);
  if (!On() || !Ptr(mode) || !Ptr(target))
  {
    return;
  }
  // Player 2's camera object, noted whenever Rare updates it (he is in the world then): co_jungle showed that while
  // he is out of the world Rare doesn't update his camera at all ("his updates 0"), so his view stayed black.
  if (target == g.p2c && !g.out)
  {
    if (!g.p2_mode_logged)
    {
      // Where the camera object sits (diagnostic: a field of his character or participant, for the start's lookup).
      g.p2_mode_logged = true;
      int32_t in_char = -1, in_part = -1;
      for (uint32_t off = 0; off < 0x2400 && in_char < 0; off += 4)
      {
        in_char = Load32(base, g.p2c + off) == mode ? int32_t(off) : -1;
      }
      for (uint32_t off = 0; Ptr(g.p2p) && off < 0x1000 && in_part < 0; off += 4)
      {
        in_part = Load32(base, g.p2p + off) == mode ? int32_t(off) : -1;
      }
      REXLOG_INFO("pdz_counterop: spectate: Rare updated player 2's camera {:08X} (in his character at +{}, in his "
                  "participant at +{}; -1 = not there)",
                  mode, in_char, in_part);
    }
    g.p2_mode = mode;
  }
  // co_jungle_3: at the start Rare hadn't updated player 2's camera before his first hide, so his view stayed black
  // until his first spot. The agent's camera object is found once in the agent's character (a pointer field); player
  // 2's is then read from the same field of his.
  if (target == g.p1c && g.started && !g.cam_field_tried && Ptr(g.p2c))
  {
    g.cam_field_tried = true;
    // The character first (9 KB), then his participant (4 KB; co_rooftops / co_trinity: not in the character).
    bool in_participant = false;
    for (uint32_t off = 0; off < 0x2400; off += 4)
    {
      if (Load32(base, target + off) == mode)
      {
        g.cam_field = off;
        break;
      }
    }
    if (!g.cam_field && Ptr(g.p1p) && Ptr(g.p2p))
    {
      for (uint32_t off = 0; off < 0x1000; off += 4)
      {
        if (Load32(base, g.p1p + off) == mode)
        {
          g.cam_field = off;
          in_participant = true;
          break;
        }
      }
    }
    const uint32_t p2_mode = !g.cam_field ? 0u : Load32(base, (in_participant ? g.p2p : g.p2c) + g.cam_field);
    REXLOG_INFO("pdz_counterop: spectate: the agent's camera {:08X} is {}; player 2's at the same field: {:08X}{}", mode,
                g.cam_field ? fmt::format("at his {} +{}", in_participant ? "participant" : "character", g.cam_field)
                            : std::string("not found in his first 9 KB or his participant's first 4 KB"),
                p2_mode, Ptr(g.p2_mode) ? fmt::format(" (Rare's own update gave {:08X})", g.p2_mode) : std::string());
    if (!Ptr(g.p2_mode) && Ptr(p2_mode) && p2_mode != mode)
    {
      g.p2_mode = p2_mode;
    }
  }
  if (!g.started || !g.out)
  {
    return;
  }
  if (target == g.p1c)
  {
    ++g.p1_cam_updates;
    for (uint32_t i = 0; i < 3; ++i)
    {
      g.agent_eye[i] = LoadFloat(base, mode + kCamEye + i * 4);
      g.agent_look[i] = LoadFloat(base, mode + kCamLook + i * 4);
    }
    g.agent_cam = std::isfinite(g.agent_eye[0]) && std::isfinite(g.agent_look[0]);
    // Player 2 out of the world: Rare's update for his camera, run here (then the spectate write below).
    if (Ptr(g.p2_mode) && Ptr(g.p2c) && !g.in_spectate_call)
    {
      g.in_spectate_call = true;
      {
        rex::CallFrame frame(ctx);
        frame.ctx.r3.u64 = g.p2_mode;
        frame.ctx.r4.u64 = g.p2c;
        SpectateStepFix(base, frame.ctx, step, REXCVAR_GET(pdz_counterop_fix_out_freeze));
        sub_82214C48(frame.ctx, base); // through this hook: it writes the spectate view
      }
      g.in_spectate_call = false;
    }
    return;
  }
  if (target != g.p2c)
  {
    return;
  }
  ++g.p2_cam_updates;
  if (!g.agent_cam)
  {
    return;
  }
  // Behind along the agent's look direction in the horizontal plane, then up.
  float fx = g.agent_look[0] - g.agent_eye[0], fz = g.agent_look[2] - g.agent_eye[2];
  const float len = std::sqrt(fx * fx + fz * fz);
  if (len < 1.0f)
  {
    fx = 0.0f;
    fz = 0.0f;
  }
  else
  {
    fx /= len;
    fz /= len;
  }
  const float cam[3] = {g.agent_eye[0] - fx * kSpectateBack, g.agent_eye[1] + kSpectateUp,
                        g.agent_eye[2] - fz * kSpectateBack};
  for (uint32_t i = 0; i < 3; ++i)
  {
    StoreFloat(base, mode + kCamPosition + i * 4, cam[i]);
    StoreFloat(base, mode + kCamLook + i * 4, g.agent_look[i]);
  }
  ++g.spectate_writes;
}

// pdz_test_keep_dropship (testing tool, off): the Jungle's "dropship destroyed" failure is a scripted message (type 8,
// subtype 6) to the Jungle level script's message handler sub_82311AB0 (vtable 820A21F4 slot 9; only the Jungle uses
// it), most likely an enemy radio operator calling it in (sender sub_825B4F88) when nobody stops him; it sets the
// trigger bits whose consequences (sub_82315BA0) play the Carrington line, fail objective 0 (Defend Dropship) and blow
// the dropship up. An unattended test whose agent stands still loses the mission to it ~12-20 s in (also solo, with
// counter-op off), so with this on that one message is dropped.
REXCVAR_DEFINE_BOOL(pdz_test_keep_dropship, false, "Testing",
                    "TESTING TOOL: in the Jungle, the scripted 'dropship destroyed' message (the failure an idle test "
                    "agent always gets ~15 s in) is ignored, so long unattended Jungle tests can run")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REX_EXTERN(__imp__sub_82311AB0);
REX_HOOK_RAW(sub_82311AB0)
{
  const uint32_t rec = ctx.r8.u32;
  if (REXCVAR_GET(pdz_test_keep_dropship) && rec >= 0x10000u && rec < 0xFFFF0000u && base[rec + 1] == 8 &&
      base[rec + 2] == 6)
  {
    static uint32_t dropped = 0;
    if (++dropped <= 3)
    {
      const uint32_t sender = ctx.r5.u32;
      REXLOG_INFO("pdz_test: Jungle dropship-destroyed message dropped (pdz_test_keep_dropship; param {}, sender {:08X} "
                  "vtable {:08X})",
                  int(base[rec + 3]), sender,
                  sender >= 0x10000u && sender < 0xFFFF0000u
                      ? (uint32_t(base[sender]) << 24 | uint32_t(base[sender + 1]) << 16 | uint32_t(base[sender + 2]) << 8 | base[sender + 3])
                      : 0u);
    }
    return;
  }
  __imp__sub_82311AB0(ctx, base);
}
