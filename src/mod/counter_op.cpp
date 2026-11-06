// Counter-Operative (pdz_counterop, default off): docs/COUNTEROP_STUDY.md stages 0-2 and the minimum of stage 3.
//
// A normal split-screen co-op mission (game type 1 kept, so every co-op path of Rare's keeps working). Player 1 is
// the agent. Player 2 is the counter-operative: Rare's co-op human, SPAWNED at a guard's spot (log 312 redesign: he
// no longer takes a live guard's body), with that guard's gun and on the guards' side through Rare's own team masks
// (study section 5, option b): player 2's team bits (+128) = 0x0001 (ALPHA, which every SP guard counts among its
// allies, 0xFFFF) and his enemy mask (+132) = 0x0002 (BETA, the humans' bit). Rare's friend test (8251EBD0) then
// makes the guards ignore him, hits between him and the guards do nothing (82549040), and he and the agent can hurt
// each other. The guard whose spot he took is removed (and kept removed). When player 2 dies, Rare's co-op revive
// (824573A8) is held 2.5 s, then run and followed by the next spot; with none left player 2 stays down (spectating)
// and the mission goes on. The agent down = Rare's co-op game over (82E426F8 |= 2, 8222FF58), as Rare does when every
// human is down. Stage 1: Rare's setup filter answers with the counter-op kind bit 0x40 instead of co-op's 0x20
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
// 20 s with none = out of guards (player 2 spectates, the mission goes on).
//
// Look (log 312: an SP guard body on the live human crashed Rare's weapon / animation update; log 311 before it was
// the give's ammo arguments): player 2 is BUILT with a soldier body the way multiplayer builds a player: the SP
// player factory 825B3D58 builds the co-op partner from his setup (+52 body, +56 head) and is called for player 2
// once player 1 exists ([82E3C3E4]); the hook on it (pdz_counterop_guard_look) writes a chosen body id into the
// setup for the duration of Rare's call and restores it, and Rare's own constructor wires everything (model, hands,
// bones, face set, armour pieces through its update). Log 313: body ids are the level's body list's numbering (=
// the setup "type": 56 the Jungle trooper, 64 the co-op partner), NOT the multiplayer roster's (its tables
// 82D27580 / 82D27610 index the multiplayer list: id 91 is Jonathan in the Jungle's list), and Rare's body loader
// (82414070) dereferences a missing asset (entry +32 -> +8) without a check, so a body whose assets are not in the
// level's packages crashes the load. Candidates are therefore taken from THIS level's body list only (the
// mission's own guard bodies first, then the generic enemy range in list order), with the co-op body's animation
// set (+64) and a hands reference (+16); every asset the loader needs is looked up first with Rare's own lookup
// (82413FD8, which returns null instead of crashing): the model (+4), the hands (+16) and the +32 asset the loader
// dereferences; only when all three resolve is the body used. The head stays the co-op one (no head requests). No
// candidate = the co-op look with no requests at all, logged with every candidate tried and why it was rejected.
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
// Not done (known gaps): his facing, level triggers / objectives / key items still accept him, no menu / HUD / end
// text.
#include <algorithm>
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

#include "objective_bots.h"
#include "render/game_pace.h" // SpLevelName

REXCVAR_DEFINE_BOOL(pdz_counterop, false, "Gameplay",
                    "Counter-Operative (docs/COUNTEROP_STUDY.md; off by default): in a split-screen co-op mission, "
                    "player 2 plays a guard against the agent (player 1). He spawns at an unused guard's spot (a "
                    "dormant one first, away from the agent and off his screen when possible), with its gun, and "
                    "the guards treat him as a friend; when he dies he spawns at another spot after 2.5 s; with "
                    "none left he stays down and the mission goes on. The agent down = mission failed. Datacore and "
                    "Arena are not supported (no placed guards). Set it before starting the mission (Rare's "
                    "counter-op spawn set is chosen at level load). One 'pdz_counterop:' line per event")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_counterop_guard_look, true, "Gameplay",
                    "Counter-Operative: build player 2 as the mission's closest multiplayer soldier character "
                    "(chosen when the level loads, through Rare's own player factory); off = the co-op partner's look")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(pdz_counterop_health_scale, 0.5, "Gameplay",
                      "Counter-Operative: player 2's health and max health on each spawn = (the guard's health / its "
                      "spawn health) x the player's max x this, floor 25. Default 0.5 = N64 Perfect Dark's rule "
                      "(0.125 per damage point of a 4-point guard: a full guard gives half a bar)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_8222F740); // co-op per-participant death states (every frame in a level, from 8222D4A8)
REX_EXTERN(__imp__sub_8222FD98); // co-op: game over? (every human down)
REX_EXTERN(__imp__sub_824573A8); // co-op revive (participant)
REX_EXTERN(__imp__sub_825B3D58); // the SP player factory: (setup) -> human (player 2's call once player 1 exists)
REX_EXTERN(sub_8222FF58);        // game over
REX_EXTERN(sub_825AD148);        // move a character (character, &position, 0)
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
REX_EXTERN(sub_82413FD8); // an asset by id: (id, type string, 0, 0) -> asset or 0 (82414070's own lookup)

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
  // 82414070's type strings for its lookups (82413FD8): the model (+4 -> +68), the hands (+16 -> +20, humans only)
  // and the +32 asset (-> +36), whose +8 it reads without a check (log 313's crash).
  constexpr uint32_t kModelAssetType = 0x820B601C, kHandsAssetType = 0x820B604C, kThirdAssetType = 0x820B6078;
  constexpr uint32_t kPlaceholderBody = 86; // Rare's fallback body (82278788): never a candidate
  constexpr uint32_t kMaxLookProbes = 8;
  // The gameplay camera mode (third_person.cpp, same in TU3): after its update, the camera position, the look point
  // and the eye.
  constexpr uint32_t kCamPosition = 4, kCamLook = 16, kCamEye = 188;
  constexpr float kSpectateBack = 120.0f, kSpectateUp = 30.0f; // cm: behind and above the agent's eye
  constexpr uint16_t kP2Team = 0x0001, kP2Enemies = 0x0002; // study section 5 (b)
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
  constexpr MissionIds kEligible[] = {
      {"nightclub", "s20a,s19b,s19a,s20b,s20c,s01a,s01b,s20d,s21d,s21b,s21c,s05d,s21a,s22d,s02c,s22a,s22c,s22b,s11b,s11c,s10c,s03b,s11d,s11e,s08c,s08b,s08a,s09a,s09d,s09c,s09b,s05a,s05b,s06a,s05c,s03a,s12b,s12c,s12d,s16a,s16b"}, // 41 ids
      {"subway", "lop2,saf5,arm4,arp2,arp3,saf1,armp,arp4,saf3,saf4,war1,saf2,arm3,lorp,lor1,zsb4,fac1,bik3,bik4,bik2,bik5,bik1,fac2,arm2,arm1,zsb5,zsb7,zsb6,zsb3,fac3,zsb2,zsb1,zsb8,s16a,s16b,16-3,s18b,18-1,out3,out4,out1,out2,inn2,inn3,inn4,inn1,inn6,sin2,sin3,inn5,zsba,sin4,zsb9"}, // 53 ids
      {"rooftops", "s141,s142,s133,s20a,s15b,s143,s15a,top2,s131,s123,gf32,gf33,top1,s112,s111,top3,s122,s121,s113,gf31,mff2,tff1,tff2,s132,bfb4,mff1,gft1,gft2,gft3,ggo1,ggo2,ggo3,s194,s193,bff2,bff1,17-3,17-2,s17a,s192,s191,bff4,bfb1,bfm3,bfb2,bfb3,bfm2,bfm1"}, // 48 ids
      {"mansion", "mgg1,tow2,tow1,tow4,tow3,pat1,pat2,fre4,fre5,pat3,fre2,fre1,fre3,end1,rsp1,lsp1,cpt1,s06c,wpt1,mgp1,ocp1,ocg1,wpt2"}, // 23 ids
      {"laboratory", "s3g7,s3g4,s3g5,s3g8,s1m3,s1m2,s1m4,s1m5,s3g3,s3g1,s1m1,s3gb,s3g2,s3gc,s11b,s11c,s11a,s11d,s9g2,s9g5,s9g6,s9g3,s9g4,s9g7,s9g8,s6s3,s6s5,s6s4,s6s2,s6s1,s9p2,s9p1,s9g1,s9p3,s7s4,s7s2,s7s1,s7s3"}, // 38 ids
      {"river", "s3g5,s3g4,s5g1,s3g1,s2g5,s3g3,s3g2,s4g5,s4g1,s4g3,s4g4,s4g2,s2g4,s1g1,s20b,s20a,s19b,s19a,s19d,s19c,s20c,s2g1,s1g5,s2g3,s2g2,s1g2,s20d,s1g4,s1g3,s16g,s16h,s16e,s16f,s16i,s17b,s17c,s16j,s17a,s16d,s15g,s15h,s15e,s15f,s15i,s16b,s16c,s15j,s16a,s17f,s17g,s17d,s17e,s17h,s17i,s17j,s8g3,s8g4,s8g2,s9g1,s10a,s10b,s9g2,s9g3,s6g3,s6g4,s6g1,s6g2,s6g5,s6kc,s6g6,s6g7,s10c,s13a,s13b,s12f,s12g,s15c,s15d,s15a,s15b,s12e,s11c,s11d,s11b,s12c,s12d,s12a,s12b"}, // 88 ids
      {"trinity1", "s03e,s07l,s03d,s14c,s14d,s14a,s14b,s14e,s14h,s14i,s14f,s14g,s14j,s01d,s01b,s01c,s03b,s03c,s03a,s06e,s06d,s14k,s14l,s06c,s06b,s18a,s18b,s18c,s08a,s07j,s08b,s17c,s15a,s15b,s16c,s17a,s17b,s16a,s16b,s07i,s06a,s07z,s07k,s07a,s07h,s07n,s07m,s07f,s07g,s07c,s07b,s07e,s07d"}, // 53 ids
      {"trinity2", "s07g,s07h,s08a,s01c,s01d,s01e,s07f,s07b,s07a,s06h,s07e,s07d,s07c,s08e,s08f,s09a,s08b,s08c,s08d,s09b,s01a,s01b,s09c,s03f,s04a,s03d,s03e,s04d,s04e,s04b,s04c,s01f,s03b,s03c,s03a,s04f,s06b,s06c,s05h,s06a,s06f,s06g,s06d,s06e,s05b,s05c,s05a,s05f,s05g,s05d,s05e,s09d,s12d,s12c,s12b,s12e,s17f,s17g,s17h,s12a,s18a,s18b,s17j,s17i,s11f,s11e,s17e,s14b,s14c,s12h,s12i,s14a,s13a,s12j,s14d,s17b,s17c,s17d,s17a,s12g,s12f,s10f,s10e,s10d,s11a,s18d,s18c,s11b,s10c,s09g,s09f,s09e,s09h,s10b,s10a,s09i,s18e,s11c,s11d"}, // 99 ids
      {"jungle", "s44c,s10e,s10f,s10b,s10a,s10c,s09d,s10d,s11g,s11f,s11h,s11j,s11i,s11b,s11a,s11c,s11e,s11d,s09c,s6e1,s5l6,s6e2,s6e4,s6e3,s5l2,s5l1,s5l3,s5l5,s5l4,s7c1,s7c8,s7c7,s09b,s09a,s7c3,s7c2,s7c4,s7c6,s7c5,s02j,s02a,s02i,s02g,s02h,s03d,s03e,s03c,s03a,s03b,sta1,s02e,s02f,s02d,s02b,s02c,s44b,s11p,s44a,s43b,s43c,s11l,s11k,s11m,s11o,s11n,s41a,s41b,s41c,s03f,s03g,s03h,s42c,s43a,s42a,s42b"}, // 75 ids
      {"temple", "s2b1,s1a1,s1a2,s2b4,s2b3,s2b2,s08c,s08e,s08d,s06g,s06f,s06h,s08f,s10a,s09f,s10c,s10b,s09e,s09b,s09a,s09d,s09c,s4p3,s4p4,s4b2,s4p2,s3b1,s4b1,s4p1,s5c1,s06c,s06d,s06e,s06b,s5c2,s5c3"}, // 36 ids
      {"outpost", "s14e,s14f,s14g,s14b,s14c,s14d,s16a,s16b,s17a,s14h,s15a,s15b,s14a,s11a,s11b,s12a,s10a,s10b,s10c,s13c,s13d,s13e,s12b,s13a,s13b,s03a,s03b,s02b,s02c,s03c,s03d,s01c,s01d,s01a,s01b,s01e,s01h,s02a,s01f,s01g,s08f,s08g,s08d,s08e,s08h,s09c,s09d,s09a,s09b,s07a,s07b,s08b,s08c,s07d,s08a,s17b,s27k,s27l,s34d,s34e,s34f,s27a,s27b,s27c,s26a,s34c,s34b,s27h,s27i,s27g,s27d,s27e,s27f,s19d,s19c,s19b,s20a,s21c,s21b,s21a,s17e,s17d,s17c,s18a,s19a,s18c,s18b,s24b,s24a,s23e,s34a,s24c,s22c,s22b,s22a,s23a,s23d,s23c,s23b"}, // 99 ids
      {"bridge", "s22b,s22c,s20b,s21a,s22a,s40b,s40a,s40d,s40c,s20a,s14a,s12b,s15b,s15a,s10a,s12a,s11a,s16a,s18c,s18b,s19a,s18d,s17a,s16b,s18a,s17b,s26d,s26c,s26b,s27a,s27c,s27b,s26a,s25a,s23d,s23c,s22d,s23a,s23b,s30a,s09b,s44k,s44j,s44i,s44l,s45c,s45b,s45a,s44h,s44c,s44b,s44a,s44d,s44g,s44f,s44e,s45f,s45e,s45d,s45g,s45j,s45i,s45h,s04b,s05a,s05b,s04a,s03b,s03c,s03d,s06a,s08a,s08b,s09a,s07b,s06b,s06c,s07a,s02c,s02d,s03a,s02a,s02b"}, // 83 ids
  };
  // Denied ids per mission (the study's excluded.md: named / boss / scripted / hostage / non-combatant / vehicle and
  // jetpack crews / spawner templates / third parties): the no-id rule never takes these.
  constexpr MissionIds kDenied[] = {
      {"datacore", "Grd1,L02S,Al01,tut0,tut1,jack"},
      {"nightclub", "rin1,s18c,s25a,s25b,sc31,s18b,sc38,sc33,sc36,s02d,sc32,sc30,s11a,s10d,rin2,s18a,s10a,s10b,s04a,r2bu,rin3,sc10,sc11,sc14,sc09,sc06,sc07,sc08,sc21,sc22,sc23,sc20,sc15,sc17,sc19,s12a,s02b,s02a,s02e,sc03,sc04,sc05,sc02,sc01"},
      {"subway", "phon,sub1,sub4,sub5,sub2,sub3,was1,sub6,zieg,jack,s19b,s20a,sak2,s20c,s20b,s17c,s17b,s19a,s17a,sak1,dspa"},
      {"rooftops", "s16a,zip1,krad,sni2,sni1,sni3"},
      {"mansion", "ter1,ter2,ter3,civ2,civ3,mai1,ter4,ter8,chan,ter5,ter6,ter7,s4s2,s5s2,rdo1,s5s1,bik1,s4s1,civ1,end2,rdo2"},
      {"laboratory", "s2s2,s2s1,s4l4,s4l5,s4l3,s4l1,s4l2,s101,jack,s12a,s12b,s5l1,s5l3,s5l2"},
      {"river", "s22a,s21f,s23a,s22b,s21e,s21c,s21d,s21a,s21b,s23b,s24b,s24a,s8g1,s7g1,s14a,s11a"},
      {"trinity1", "s02a,s11b,s11c,s11a,s10h,s11d,s11f,s11g,s10g,s11e,s02b,s02c,s01a,clay,drca,virg,radd,s10a,s09h,radr,s09b,s09a,s10c,s10d,s10e,s10b,s10f,s09g,s09f,s09c,s09d,s09e,rada"},
      {"trinity2", "s30b,s30a,s02a,s02b,s16a,s21a,s15a,s21c,s31c,s31b,s21b,s31a"},
      {"jungle", "s31p,s08a,s01a,s12a,s12b"},
      {"temple", "s08b,s08a,s07a,s06a"},
      {"outpost", "s04b,s04c,s04a,s04d,s04e,s07c,s31c,s32a,s32b,s31b,s31a,s30p,bff1,s30b,s25a,s27j,s28a,s28b"},
      {"bridge", "s30m,s35i,s30c,s10b,s28a,s24a,s29a,s35d,s35c,s35b,s35e,s35h,s35g,s35f,s35a,s31a,s29b,s32a,s33b,s33a,s32b,s47b,s47a,s46a,s47c,s48b,s48a,s47d,s01d,s01e,s01f,s01c,s01j,s01a,s01b,s01g"},
      {"arena", "Grd1,zlsp"},
  };

  // The mission's own guard bodies (the setup "type" = body id of its placed guards, from the decoded setups), tried
  // first; then every generic enemy body in the level's list (CombatType ids, list order). Missions without a row
  // use the list only.
  struct MissionLook
  {
    const char *mission;
    uint32_t bodies[3];
  };
  constexpr MissionLook kLook[] = {
      {"jungle", {56, 52, 64}},    // troopers (heads 29-34), 52, the co-op partner's own 64
      {"nightclub", {48, 62, 83}}, // gangsters, 62, staff 83
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
    float x = 0.0f, y = 0.0f, z = 0.0f; // the spot: a dormant guard's setup position, a live guard's own
    float distance = 0.0f;
    bool on_screen = false;
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
    // Health: the co-op max health, restored on a stop; what the last spawn set, checked once 5 s later.
    float coop_max_health = 0.0f;
    bool max_health_set = false;
    float set_health = 0.0f;
    std::chrono::steady_clock::time_point health_check_at{};
    bool health_check = false;
    bool registry_warned = false;
  } g;

  // Setup filter counters (PdzCounterOpFilter, at level load; logged at the start).
  uint32_t g_filter_added = 0, g_filter_removed = 0, g_filter_seen = 0;
  std::mt19937 g_rng{0xC0DE0Bu};

  bool On() { return REXCVAR_GET(pdz_counterop); }

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

  void ApplyTeam(uint8_t *base, uint32_t c)
  {
    Store16(base, c + kTeamBits, kP2Team);
    Store16(base, c + kEnemies, kP2Enemies);
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

  // The eligible spots (not taken, alive), nearest the agent first, and the counts.
  std::vector<Spot> Spots(PPCContext &ctx, uint8_t *base, PickDiag &d)
  {
    std::vector<Spot> out;
    if (!Ptr(g.p1c))
    {
      return out;
    }
    std::vector<uint32_t> objects;
    CollectObjects(base, objects, d);
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
      const uint32_t setup = Load32(base, c + kSetup);
      const uint8_t team = Ptr(setup) ? base[setup + kSetupTeam] : 0;
      uint32_t id_ptr = 0;
      const uint32_t id = SetupIdKey(base, setup, &id_ptr);
      if (id)
      {
        seen_ids.insert(id);
      }
      if (g.taken.count(c))
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
      if (!Alive(ctx, base, c))
      {
        ++d.dead;
        continue;
      }
      e.dormant = (Load32(base, c + kHidden) & 0x10) != 0;
      e.character = c;
      e.setup = setup;
      e.id = id;
      e.id_ptr = id_ptr;
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
    std::sort(out.begin(), out.end(), [](const Spot &a, const Spot &b) { return a.distance < b.distance; });
    return out;
  }

  // The pick: dormant spots first, at least kMinSpawnDistance from the agent and off his screen; relaxed step by
  // step (dormant far; live far off-screen; live far; dormant any; live any); one of the three nearest qualifying at
  // random. Returns the step's name.
  const char *Pick(const std::vector<Spot> &spots, Spot &out)
  {
    struct Step
    {
      const char *name;
      bool dormant, far, off_screen;
    };
    static constexpr Step kSteps[] = {
        {"a dormant spot, far and off-screen", true, true, true},
        {"a dormant spot, far", true, true, false},
        {"a live guard, far and off-screen", false, true, true},
        {"a live guard, far", false, true, false},
        {"a dormant spot (none far)", true, false, false},
        {"a live guard (none far)", false, false, false},
    };
    for (const Step &s : kSteps)
    {
      std::vector<const Spot *> list;
      for (const Spot &e : spots)
      {
        if (e.dormant == s.dormant && (!s.far || e.distance >= kMinSpawnDistance) && (!s.off_screen || !e.on_screen))
        {
          list.push_back(&e); // spots are sorted nearest first
        }
      }
      if (!list.empty())
      {
        const size_t n = std::min<size_t>(list.size(), 3);
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
    const float guard_health = LoadFloat(base, e.character + kHealth);
    const float spawn_health = Ptr(e.setup) ? LoadFloat(base, e.setup + kSetupHealth) : 0.0f;
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
    std::string no_gun;
    const uint32_t gun = GiveableWeaponType(base, guard, no_gun);
    g.taken.insert(guard);
    ++g.lives;
    REXLOG_INFO("pdz_counterop: {}: player 2 (life {}) spawns at {}: guard {:08X} (setup {:08X}, id {}; {}; body {}, "
                "head {}) at ({:.0f}, {:.0f}, {:.0f}), {:.1f} m from the agent, {} the agent's screen; {}",
                why, g.lives, step, guard, e.setup, e.id ? IdLog(e.id, e.id_ptr) : std::string("none"),
                e.by_list ? fmt::format("rule: the {} id list", g.mission)
                          : fmt::format("rule: no id ({}): type 48+, not a non-combatant, armed, not denied", e.why),
                Load32(base, guard + kBodyId), Load32(base, guard + kHeadId), e.x, e.y, e.z, e.distance / 100.0f,
                e.on_screen ? "on" : "off", CountsText(d));
    // (a)
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
                  e.x, e.y, e.z, e.dormant ? "the dormant guard's setup position" : "the live guard's position",
                  LoadFloat(base, g.p2c + kPosition), LoadFloat(base, g.p2c + kPosition + 4),
                  LoadFloat(base, g.p2c + kPosition + 8));
    }
    else
    {
      REXLOG_WARN("pdz_counterop: spawn (b): no guest scratch memory for the move: player 2 not moved");
    }
    // (c) the side and the gun, as in log 310 (no ammo amounts in the give: log 311 crashed in its secondary-ammo
    // block, which reads the entry's +4 definition, null for a one-ammo gun); then full reserves through Rare's adders.
    ApplyTeam(base, g.p2c);
    if (gun)
    {
      const uint32_t item = Call(ctx, base, sub_8254C378, g.p2c, gun, 0, 0, 0, 0);
      std::string ammo = "no item: no ammo added";
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
      REXLOG_INFO("pdz_counterop: spawn (c): guards' side (team bits {:04X}, enemies {:04X}); gun type {} given ({}); {}",
                  Load16(base, g.p2c + kTeamBits), Load16(base, g.p2c + kEnemies), gun,
                  item ? fmt::format("item {:08X}", item) : std::string("give returned none"), ammo);
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
    const std::string removed = RemoveGuard(ctx, base, guard);
    REXLOG_INFO("pdz_counterop: spawn (e): guard {:08X} {}", guard, removed);
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
      REXLOG_INFO("pdz_counterop: stopped ({}): player 2 lives {}, team bits re-applied {} time(s){}", why, g.lives,
                  g.team_fixes, restored);
    }
    const uint32_t scratch = g.scratch;
    g = State{};
    g.scratch = scratch;
  }

  void Start(PPCContext &ctx, uint8_t *base)
  {
    g.started = true;
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
    if (g.ids.empty())
    {
      g.unsupported = true;
      REXLOG_INFO("pdz_counterop: {}: no eligible guards (Datacore and Arena have only runtime clones; unsupported for "
                  "now): plain co-op",
                  g.mission.empty() ? "this level" : g.mission);
      return;
    }
    // Player 2 is never Joanna's partner: out of the world until his first spot. Picks are tried once a second,
    // from kFirstPickSeconds on and never during a cutscene (log 304: the one pick ran before the opening cutscene).
    HideP2(ctx, base, "until his first spot");
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
      --diag.eligible;
      ++diag.taken;
      --(e.dormant ? diag.dormant : diag.live);
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
                    LoadFloat(base, p2c + kPosition + 8), LoadFloat(base, p2c + kHealth), Load16(base, p2c + kTeamBits));
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
      if (!TryPick(ctx, base, "after his death"))
      {
        HideP2(ctx, base, "between spots");
        g.need_guard = true;
        g.after_death = true;
        g.need_since = std::chrono::steady_clock::now();
        g.next_try = g.need_since + std::chrono::milliseconds(int(kRetrySeconds * 1000));
      }
    }
    // The retries: once a second outside cutscenes; after a death, out of guards after kOutOfGuardsSeconds.
    if (g.need_guard && !cutscene && std::chrono::steady_clock::now() >= g.next_try)
    {
      const auto now = std::chrono::steady_clock::now();
      g.next_try = now + std::chrono::milliseconds(int(kRetrySeconds * 1000));
      if (TryPick(ctx, base, g.after_death ? "after his death" : "first spot"))
      {
        g.need_guard = false;
        g.diag_lines = 0;
      }
      else if (g.after_death && now - g.need_since >= std::chrono::milliseconds(int(kOutOfGuardsSeconds * 1000)))
      {
        g.need_guard = false;
        g.spectating = true;
        REXLOG_INFO("pdz_counterop: out of guards ({:.0f} s with no spot left): player 2 stays out of the world "
                    "(spectating); the mission goes on",
                    kOutOfGuardsSeconds);
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
      const float now_health = LoadFloat(base, p2c + kHealth);
      const float now_max = Ptr(participant) ? LoadFloat(base, participant + kPartMaxHealth) : -1.0f;
      REXLOG_INFO("pdz_counterop: 5 s after the spawn: health {:.1f} (set {:.1f}), max {} (set {:.1f}){}; armour {:.1f}; "
                  "body {} head {}; at ({:.0f}, {:.0f}, {:.0f})",
                  now_health, g.set_health, now_max >= 0.0f ? fmt::format("{:.1f}", now_max) : std::string("?"),
                  g.set_health,
                  now_health > g.set_health + 0.5f ? " (regenerated above the share: the max is not the regen cap)" : "",
                  LoadFloat(base, p2c + kArmour), Load32(base, p2c + kBodyId), Load32(base, p2c + kHeadId),
                  LoadFloat(base, p2c + kPosition), LoadFloat(base, p2c + kPosition + 4),
                  LoadFloat(base, p2c + kPosition + 8));
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
                    "camera updated {} times)",
                    g.spectate_writes, kSpectateBack / 100.0f, kSpectateUp / 100.0f, g.p1_cam_updates);
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
    if (!g.spectating && !g.out && !g.revive_participant && Alive(ctx, base, p2c) &&
        (Load16(base, p2c + kTeamBits) != kP2Team || Load16(base, p2c + kEnemies) != kP2Enemies))
    {
      if (++g.team_fixes <= 8 || g.team_fixes % 100 == 0)
      {
        REXLOG_INFO("pdz_counterop: player 2's team bits were {:04X} / enemies {:04X}: re-applied ({} time(s))",
                    Load16(base, p2c + kTeamBits), Load16(base, p2c + kEnemies), g.team_fixes);
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

  // Can the level build a human with this body, without a loader crash? Its list entry must have a hands reference
  // (+16) and the co-op body's animation set (+64), and the three assets the loader dereferences must resolve
  // through Rare's own lookup: the model (+4), the hands (+16) and the +32 asset. Nothing else is requested.
  bool BodyUsable(PPCContext &ctx, uint8_t *base, uint32_t body, uint32_t coop_entry, std::string &why)
  {
    const uint32_t entry = ListEntry(base, kListsBody, body);
    if (!Ptr(entry))
    {
      why = "not in the level's body list";
      return false;
    }
    const uint32_t model_ref = Load32(base, entry + kBodyModelRef), hands_ref = Load32(base, entry + kBodyHandsRef),
                   third_ref = Load32(base, entry + kBodyThirdRef);
    if (!model_ref || !hands_ref || !third_ref)
    {
      why = fmt::format("entry {:08X} lacks a reference (model {:08X}, hands {:08X}, +32 {:08X})", entry, model_ref,
                        hands_ref, third_ref);
      return false;
    }
    if (Ptr(coop_entry) && Load32(base, entry + kBodyAnimSet) != Load32(base, coop_entry + kBodyAnimSet))
    {
      why = fmt::format("another animation set than the co-op body's (+64 {:08X} vs {:08X})",
                        Load32(base, entry + kBodyAnimSet), Load32(base, coop_entry + kBodyAnimSet));
      return false;
    }
    if (Load32(base, entry + kBodyAssets))
    {
      // Already loaded by Rare for this level (a guard wears it): the model is there; the hands still need checking.
      const uint32_t hands = Load32(base, entry + kBodyHands)
                                 ? Load32(base, entry + kBodyHands)
                                 : Call(ctx, base, sub_82413FD8, hands_ref, kHandsAssetType, 0, 0);
      if (!Ptr(hands))
      {
        why = fmt::format("entry {:08X} is loaded (a guard wears it) but its player hands {:08X} are not in the level's "
                          "packages",
                          entry, hands_ref);
        return false;
      }
      why = fmt::format("entry {:08X} already loaded by Rare (assets {:08X}), hands {:08X} resolve", entry,
                        Load32(base, entry + kBodyAssets), hands);
      return true;
    }
    const uint32_t model = Call(ctx, base, sub_82413FD8, model_ref, kModelAssetType, 0, 0);
    if (!Ptr(model))
    {
      why = fmt::format("entry {:08X}: its model {:08X} is not in the level's packages", entry, model_ref);
      return false;
    }
    const uint32_t hands = Call(ctx, base, sub_82413FD8, hands_ref, kHandsAssetType, 0, 0);
    if (!Ptr(hands))
    {
      why = fmt::format("entry {:08X}: its player hands {:08X} are not in the level's packages", entry, hands_ref);
      return false;
    }
    const uint32_t third = Call(ctx, base, sub_82413FD8, third_ref, kThirdAssetType, 0, 0);
    if (!Ptr(third))
    {
      why = fmt::format("entry {:08X}: its +32 asset {:08X} (the one the loader dereferences) is not in the level's "
                        "packages",
                        entry, third_ref);
      return false;
    }
    why = fmt::format("entry {:08X}: model {:08X}, hands {:08X}, +32 asset {:08X} all resolve", entry, model, hands,
                      third);
    return true;
  }

  // The look for player 2 in this mission: the first candidate body the level can build (the mission's guard bodies,
  // then the generic enemy bodies of the level's list), with the co-op head. Fills g_look; true when a body was chosen.
  bool ChooseLook(PPCContext &ctx, uint8_t *base, uint32_t setup)
  {
    g_look = Look{};
    g_look.decided = true;
    std::string raw;
    g_look.mission = pdz::pace::SpLevelName(base, &raw);
    g_look.coop_body = Load32(base, setup + kDescBody);
    g_look.coop_head = Load32(base, setup + kDescHead);
    g_look.head = g_look.coop_head;
    std::vector<uint32_t> candidates;
    for (const MissionLook &m : kLook)
    {
      if (g_look.mission == m.mission)
      {
        for (const uint32_t b : m.bodies)
        {
          candidates.push_back(b);
        }
      }
    }
    {
      const uint32_t lists = Load32(base, kGame + 8);
      const uint32_t list = Ptr(lists) ? Load32(base, lists + kListsBody) : 0;
      const int32_t count = Ptr(list) ? int32_t(Load32(base, list + kListCount)) : 0;
      const uint32_t entries = Ptr(list) ? Load32(base, list + kListEntries) : 0;
      for (int32_t i = 0; Ptr(entries) && i < count && i < 512; ++i)
      {
        const uint32_t e = Load32(base, entries + uint32_t(i) * 4);
        const uint32_t id = Ptr(e) ? Load32(base, e) : 0;
        if (CombatType(id))
        {
          candidates.push_back(id);
        }
      }
    }
    const uint32_t coop_entry = ListEntry(base, kListsBody, g_look.coop_body);
    std::string tried;
    std::unordered_set<uint32_t> seen;
    uint32_t probes = 0;
    for (const uint32_t body : candidates)
    {
      if (body == g_look.coop_body || body == kPlaceholderBody || !seen.insert(body).second)
      {
        continue;
      }
      if (++probes > kMaxLookProbes)
      {
        tried += fmt::format("; {} more candidates not tried (probe limit)", candidates.size());
        break;
      }
      std::string why;
      if (!BodyUsable(ctx, base, body, coop_entry, why))
      {
        tried += fmt::format("{}body {}: {}", tried.empty() ? "" : "; ", body, why);
        continue;
      }
      g_look.chosen = true;
      g_look.body = body;
      g_look.why = fmt::format("body {} ({}), the co-op head {} kept{}", body, why, g_look.coop_head,
                               tried.empty() ? "" : "; rejected first: " + tried);
      return true;
    }
    g_look.why = fmt::format("no body of this level's list can be built for a human ({}): the co-op body {} / head {} "
                             "kept, nothing requested",
                             tried.empty() ? "no candidates" : tried, g_look.coop_body, g_look.coop_head);
    return false;
  }
} // namespace

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

// sub_825B3D58(setup): Rare's SP player factory. Player 2's call (player 1 exists, a co-op game, pdz_counterop and
// pdz_counterop_guard_look on): the chosen multiplayer soldier's body / head ids are written into his setup for the
// duration of Rare's call and restored, so Rare's own constructor builds him with them (the multiplayer way).
REX_HOOK_RAW(sub_825B3D58)
{
  const uint32_t setup = ctx.r3.u32;
  const uint32_t so = Load32(base, kGame);
  const uint32_t one = Load32(base, kPlayerOne);
  // Player 2's call: a co-op game, player 1 built, and not player 1's own setup (a rebuilt player 1 keeps her look).
  if (!On() || !REXCVAR_GET(pdz_counterop_guard_look) || !Ptr(setup) || !Ptr(so) || Load32(base, so + 40) != 1 ||
      !Ptr(one) || Load32(base, one + kSetup) == setup)
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
  Store32(base, setup + kDescBody, g_look.body);
  Store32(base, setup + kDescHead, g_look.head);
  REXLOG_INFO("pdz_counterop: look: player 2 is built with body {} in {}: setup {:08X} body {} -> {}, head {} -> {} for "
              "Rare's player factory ({})",
              g_look.body, g_look.mission, setup, old_body, g_look.body, old_head, g_look.head, g_look.why);
  __imp__sub_825B3D58(ctx, base);
  Store32(base, setup + kDescBody, old_body);
  Store32(base, setup + kDescHead, old_head);
  const uint32_t human = ctx.r3.u32;
  REXLOG_INFO("pdz_counterop: look: Rare's factory returned {:08X}{}; the setup's ids restored", human,
              Ptr(human) ? fmt::format(" (body {}, head {}, model {:08X}, head model {:08X})", Load32(base, human + kBodyId),
                                       Load32(base, human + kHeadId), Load32(base, human + 72),
                                       Load32(base, human + 4104))
                         : std::string(" (none)"));
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
  base[kLevelState] |= 2;
  PPCContext saved = ctx;
  Call(saved, base, sub_8222FF58, 0);
  if (!g.game_over_logged)
  {
    g.game_over_logged = true;
    REXLOG_INFO("pdz_counterop: the agent is down: mission failed (Rare's co-op game over); player 2 lives {}", g.lives);
  }
  ctx.r3.u64 = 1;
}

// sub_824573A8(participant): Rare's co-op revive, right after a human's death animation. Counter-op: player 2's is
// held 2.5 s (he watches where he died), then run by the tick and followed by the next spot.
REX_HOOK_RAW(sub_824573A8)
{
  const uint32_t participant = ctx.r3.u32;
  if (!On() || !g.started || g.unsupported || g.allow_revive || participant != g.p2p)
  {
    __imp__sub_824573A8(ctx, base);
    return;
  }
  g.revive_participant = participant;
  g.revive_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(kNextGuardSeconds * 1000));
  REXLOG_INFO("pdz_counterop: player 2 is down (life {}): the next spot in {:.1f} s", g.lives, kNextGuardSeconds);
}

// sub_82214C48 (TU3 of 8220D9C8, third_person.cpp): the gameplay camera's update (r3 = camera mode, r4 = the target
// character). Counter-op, while player 2 is out of the world (log 306: his view was black): the agent's camera is
// noted, and player 2's is put 1.2 m behind and 0.3 m above the agent's eye, looking at the agent's look point. Rare's
// update runs first in both cases; nothing changes outside counter-op.
REX_HOOK_RAW(sub_82214C48)
{
  const uint32_t mode = ctx.r3.u32, target = ctx.r4.u32;
  __imp__sub_82214C48(ctx, base);
  if (!g.started || !g.out || !On() || !Ptr(mode) || !Ptr(target))
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
