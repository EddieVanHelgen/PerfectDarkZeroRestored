// Laboratory | Rescue restorations (pdz_lab_*), tu3-base, TU3 addresses.
// Research: the Laboratory research report (sections 1-4, 8) and its
// notes; the earlier facts are in docs/SP_INVENTORY.md ("Darkness in the
// Laboratory blackout", "Is the blackout ever reversed?") and
// docs/BUG_AUDIT.md LS6 / LS7. Each piece uses Rare's own lines and Rare's own
// logic where they exist, sits behind its own switch, is off by default and
// does nothing but observe while off.
//
// The Laboratory script (vtable 820A30EC): setup sub_8231B778, per-frame tick
// sub_8231F2E8 (f1 = dt; it runs the tannoy scheduler sub_8231B330 first),
// flag-change handler sub_823272E0 (hooked in restorations.cpp, which calls
// BeforeLabMessage / AfterLabMessage) and level-message receiver
// sub_823223F0. The level's 320-bit flag set is at [script+0x3EC] (ten
// words; flag n = word n/32, bit n%32); the handler gets the XOR against the
// previous frame in r5..r9, so a branch runs when its flag changes.
//
// - pdz_lab_blackout_lines (a): pdz_restore_light_speech's lines (R3,
//   restorations.cpp), spoken only by staff inside the four light-group
//   boxes (obbvolume1119000721 / 1006 / 1265, 1125326996; full sizes, traced
//   in the research), so nobody in the still-lit Lab One / Lab Two says
//   "lights out again". InDarkenedBox() below; the pick is in
//   restorations.cpp.
// - pdz_lab_power_return (b): the cut half of the set piece, on the switch
//   route only (flag 37, set by the switch case of the receiver and by
//   nothing else; the lockbot sets 13 + 39, the explosives 15 + 39). Rare
//   left: Chandra's recorded, never-named CHAN_07 "Jo! Generator is back
//   online."; flag 40, which the tannoy scheduler (8231B484) tests to stop
//   the "reconnecting the power" promises and which no code sets; the
//   uncalled sight-range reset sub_8252D728; the setup's ambient colour.
//   What is ours is the length (pdz_lab_power_return_s). While the lights
//   branch of the handler runs (flag 38, 8232A264: Lights_Toggle on groups
//   1-4, which only ever switches off, LS7) the per-light handler
//   sub_825F3B68 is observed, so exactly the lights Rare switched off are
//   known; a light shot out before or after is not touched. On expiry: those
//   lights fade back in (sub_82602648 state 1, the setter Rare's toggle
//   would have reached without the LS7 bug), the ambient colour goes back
//   to the values the blackout overwrote, every character carrying the dark
//   aim bit (0x2 of +6276, set only by the darkness) loses it and gets his
//   default sight range (sub_8252D728) and search floor switch (+0x49 = 1,
//   the registration value) back, flag 40 is set, and CHAN_07 plays. Guards
//   the receiver darkens afterwards (wake-ups and spawns test "objective 0
//   complete", not the power) are restored again after each level message.
// - pdz_lab_darkness_list (c): Rare's data slips in the 29-slot darkness
//   list the setup marks (8231CF54-8231D3A0, bit 0x4 of +6276). The record
//   ais/s03generator6 carries the id "s3g4" (a duplicate of s03generator4;
//   the lookup sub_821794A8 returns the first match, so one of the two is
//   never marked, and the slot for "s3g6" stays empty), and the walkway
//   guard's variants s3gb / s3gc (one of three survives the setup; script
//   slots +0x4BC / +0x4C4) are not on the list, so on variants 1 and 2 he
//   keeps full sight in the dark. After the setup: the surviving walkway
//   guard and every character whose setup id is "s3g4" get bit 0x4. The bit
//   is read only by the handler's darkness loop (823283F8).
// - pdz_lab_cut_lines (d): small lines Rare recorded and never wired
//   (REPORT.md section 4.3 and 8 A5-A7), each once per run:
//   - the gas payoff: the tick starts the Perfect / Dark Agent gas attack
//     (flag 31, CHAN_16) when (now - [script+0x768]) > [script+0x948] and
//     the gas objective (1) is not complete (82320A54-82320AC0); when that
//     moment comes with the objective already complete, nothing plays. Then
//     CHAN_24 "just as well you locked the valve down" (the valve route, flag
//     122) or CHAN_25 "just as well you destroyed the vent fans" (flag 141);
//   - the programmer's reply: his task sends Jo's threat JO_10 (flag 184,
//     8231A8C0 -> 8232A140) and later plays SCIENTIST_28 "I shut off the gas"
//     (8231AA94); SCIENTIST_27 "OK, OK. Gimme a chance." sits between and is
//     played on him (s5l1, the scientist the gas-start message goes to; the
//     scientist nearest Jo if that slot is empty) 4 s after the threat;
//   - Chandra's level hints whose moment Rare's own events give: 02 (the gold
//     keycard) after a gold door refuses Jo (MACHINE_01 / MACHINE_08, flags
//     268 / 269 / 164 / 165), 07 (where the tracking system is) after CHAN_15
//     (flag 26), 08 (the datathief virus) after CHAN_11 (flag 36), 10 (where
//     the gas is controlled) after CHAN_16 (flag 31). Skipped: 01 (the
//     mailbots don't exist), 03 / 04 (CHAN_29 / CHAN_33 say the same), 05 /
//     06 / 11 / 12 / 13 (no event of Rare's marks the moment; the choice of
//     route would be ours).
//   Chandra's lines go through her queue with the handler's arguments; the
//   programmer's is a positioned sound on him.
// - pdz_lab_blackout_coverage (e, OURS, not Rare's; default 1 = Rare's game):
//   how far the blackout reaches. Rare's four boxes leave 224 switchable
//   lights on (research note 3 2c: Lab One 52, the vents 38, strips down both
//   sides of the main hall 45, the far end of security 29, the prison 24, Lab
//   Two 18, the spawner end 16, singles 3) and his darkness list leaves out
//   the prison guards (s9*, dormant until Jo reaches the prison) and the ten
//   scientists. kExtraLights below is that set, compiled from
//   the research light table (lights_groups.tsv) with a level per light:
//   2 = the gaps inside the dark areas (the main-hall strips and the security
//   end, 71 lights), 3 = everything but the 6 red cell lights and the 17 amber
//   security lights (colour; 201 lights). The sun (type 1) can't be switched
//   off. In the frame of Rare's lights branch (flag 38) every still-lit light
//   of the chosen set is switched off with Rare's own setter (sub_82602648
//   state 0, fade 0 = instant, as Lights_Toggle value 0 does) and recorded
//   for the power return; shot-out lights (already off) are left alone, as
//   Rare's are. At 3, in the frame of Rare's darkness message (flag 132),
//   every living extra character with a perception profile gets Rare's
//   darkness writes: the prison guards the full set (range 1e6, dark aim
//   bit, a targetless SEARCH task, search floor switch off: Rare gives the
//   same to every guard he wakes after the blackout, 82326EB4), the
//   scientists sight only (range, bit, +0x49; a SEARCH task would replace
//   their scientist task and with it the cowering / surrender / warn
//   behaviour). Dormant characters have no profile and are skipped; after
//   each level message (the receiver wakes the prison, 82326A28) anyone of
//   the set whose profile shows the registration values is darkened then.
//   The level is latched at the blackout. pdz_lab_power_return restores the
//   extra lights with Rare's (same list) and every extra character through
//   UndarkenGuards (the dark aim bit marks them). pdz_lab_blackout_lines
//   drops its box filter at 3. pdz_test_lab_light_census logs the lights on
//   / off per area at the blackout and the power return.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/hook.h>
#include "guest_function.h" // our indirect calls: nullptr for no function (log 101)
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>

#include "../game_cvars.h"
#include "lab_restorations.h"
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

REX_EXTERN(sub_82202D20); // cue name -> cue reference (ref, name)
REX_EXTERN(sub_822014B8); // queue a 2D line (Chandra / tannoy): (ref, f1, f2, r6, r7 speaker, r8)
REX_EXTERN(sub_821FF3C8); // play a cue on a prop / character: (ref, pos or 0, r5, r6 attach, r7)
REX_EXTERN(sub_82527378); // game-state object iterator: next
REX_EXTERN(sub_825272F0); // game-state object iterator: unlink
REX_EXTERN(sub_82602648); // light state: (light, state 0 off / 1 on, f1 fade s; the sun ignores "on")
REX_EXTERN(sub_8252D728); // a character's sight range back to his default (uncalled by Rare)
REX_EXTERN(sub_8239D740); // allocate (r3 bytes, r4 tag); the darkness loop's fallback for a task (82328488)
REX_EXTERN(sub_821DF550); // build a SEARCH task in place (r3 memory, r4..r7 pad / target: 0); returns it
REX_EXTERN(sub_821712D8); // hand a task to a character's scheduler (r3 char + 0x18D4, r4 task)
REX_EXTERN(__imp__sub_825F3B68); // per-light level message (111 Lights_Toggle .. 115), from the light groups
REX_EXTERN(__imp__sub_8231B778); // the Laboratory script's setup (r3 script, r4 level, r5 full start)
REX_EXTERN(__imp__sub_8231F2E8); // the Laboratory script's per-frame tick (r3 script, f1 dt)
REX_EXTERN(__imp__sub_823223F0); // the Laboratory script's level-message receiver (r3 script)
REX_EXTERN(__imp__sub_8231B330); // the Laboratory tannoy scheduler (r3 script), called first in the tick

namespace
{
  // --- guest memory helpers ---
  uint32_t Load32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  void Store32(uint8_t *base, uint32_t address, uint32_t value)
  {
    uint8_t *p = base + address;
    p[0] = uint8_t(value >> 24);
    p[1] = uint8_t(value >> 16);
    p[2] = uint8_t(value >> 8);
    p[3] = uint8_t(value);
  }

  float LoadFloat(const uint8_t *base, uint32_t address)
  {
    const uint32_t bits = Load32(base, address);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
  }

  double LoadDouble(const uint8_t *base, uint32_t address)
  {
    const uint64_t bits = (uint64_t(Load32(base, address)) << 32) | Load32(base, address + 4);
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
  }

  bool IsGuestPointer(uint32_t address)
  {
    return address >= 0x00010000u && address < 0xFFFF0000u;
  }

  // --- the game ---
  constexpr uint32_t kGameState = 0x82D6BFD8;      // -> game info; +4 = difficulty (0 Agent .. 3 Dark Agent)
  constexpr uint32_t kObjectList = 0x82D6BFF0;     // [kGameState + 0x18]: the game-state object list
  constexpr uint32_t kIteratorChain = 0x82E3C398;  // head of the chain of live object iterators
  constexpr uint32_t kAiClock = 0x82E2F334;        // float seconds, per level (the scripts' "now")
  constexpr uint32_t kCharacterPosition = 48;      // vec3, cm
  constexpr uint32_t kCharacterData = 0x54;        // -> setup data; +0x34 -> the setup id string ("s3g4")
  constexpr uint32_t kCharacterAiFlags = 0x1884;   // AI flag word (+6276): 0x2 dark aim, 0x4 darkness list
  constexpr uint32_t kCharacterProfileSlot = 0x354; // perception profile slot, -1 none
  constexpr uint32_t kHealthSlot = 332;            // character vtable: health (f1)
  constexpr uint32_t kScriptFlags = 0x3EC;         // script -> the level's flag words
  constexpr uint32_t kScriptObjectives = 0x3F0;    // script -> objectives, 0x34 bytes each, +0 state (2 = complete)
  constexpr uint32_t kProfileTable = 0x82E5D590;   // 0x50 bytes per slot; +0x1C squared sight range, +0x49 search floor switch
  constexpr uint32_t kAmbient = 0x82E09F60;        // two vec4 the ambient setter sub_82601D88 writes (r4 here, r3 at +0x10)
  constexpr uint32_t kAmbientSize = 32;
  constexpr uint32_t kRefSize = 1024;              // cue reference (the handler's are 544 bytes apart)
  constexpr uint32_t kNameSize = 128;
  constexpr uint32_t kIteratorSize = 32;           // Rare's iterator is 0x1C bytes
  constexpr unsigned kFlagLockbot = 13;            // the lockbot locked the override
  constexpr unsigned kFlagExplosives = 15;         // the generator was blown up
  constexpr unsigned kFlagSwitch = 37;             // the switch was pulled (-> 38 next frame)
  constexpr unsigned kFlagLights = 38;             // lights out (groups 1-4, ambient black)
  constexpr unsigned kFlagDarkness = 132;          // the darkness message (823282D4: r7 high word bit 0x10)
  constexpr uint32_t kProfileSlots = 171;          // the perception profile table's size
  constexpr uint32_t kCharacterScheduler = 0x18D4; // character + this = his task scheduler (823284B4)
  constexpr float kDarkRange2 = 1000000.0f;        // the darkness's squared sight range (820A06C0: 1,000 units)
  constexpr uint32_t kLightArray = 0x82E19AE0;     // the scene lights (pointers), walked by sub_82601F58
  constexpr uint32_t kLightCount = 0x82E3C5F4;     // how many
  constexpr uint32_t kLightStates = 0x82E1BA20;    // buffer offset: a light's on / off byte is at light + 0x5AC + [this] (sub_82602608)
  constexpr uint32_t kLightType = 0x2A8;           // light: record type (0 spot, 1 the sun, 2 point)
  constexpr uint32_t kLightPosition = 0x2D0;       // light: vec3, cm
  constexpr uint32_t kLightOwner = 0x338;          // light: owning prop (0 for a scene light)
  constexpr uint32_t kLightStateByte = 0x5AC;
  constexpr uint32_t kMaxLights = 2000;            // Rare's limit (note 3, 2a)
  constexpr float kLightMatch = 0.5f;              // cm per axis: a runtime light matches a table position
  constexpr unsigned kFlagPowerBack = 40;          // tested by the tannoy scheduler, never set by Rare
  constexpr uint32_t kScriptTannoyState = 0x46C;   // tannoy scheduler state: 0 idle, 1 generic, 2 power tannoy
  constexpr uint32_t kScriptTannoyInterval = 0x944; // float: the next interval; exactly 10.0 right after a blackout (TRIGGER02)
  constexpr uint32_t kMaxTrigger03 = 3;            // pdz_fix_lab_tannoy_repeat: TRIGGER03 at most this often (its 3 takes)
  constexpr uint32_t kDarkAim = 0x2;               // +6276: aim build-up x0.25 against a player (set by the darkness)
  constexpr uint32_t kDarknessList = 0x4;          // +6276: on the darkness list
  constexpr float kLightFadeS = 1.0f;              // the lights' fade back in (ours; Rare's shoot-out uses 0.01 s off)
  constexpr uint32_t kSlotWalkwayB = 0x4BC;        // script: s3gb (the variant-1 walkway guard; +0x4B8 link, +0x4BC pointer)
  constexpr uint32_t kSlotWalkwayC = 0x4C4;        // script: s3gc (variant 2)
  constexpr uint32_t kSlotGenerator4 = 0x4DC;      // script: s3g4 (the first of the two records found)
  constexpr uint32_t kSlotScientistFirst = 0x504;  // script: s4l1 .. s4l5, s5l1 .. s5l3 (pointers, 8 apart)
  constexpr uint32_t kSlotScientistLast = 0x53C;
  constexpr uint32_t kSlotProgrammer = 0x52C;      // script: s5l1, the gas programmer (the gas-start message goes to him)
  constexpr uint32_t kScriptGasStamp = 0x768;      // script: double, the gas attack's start stamp
  constexpr uint32_t kScriptGasDelay = 0x948;      // script: float, the gas attack's delay (s)
  constexpr uint32_t kPlayerOne = 0x82E3C3E4;      // player 1's character
  constexpr unsigned kFlagGasStart = 31;           // CHAN_16: the gas attack starts
  constexpr unsigned kFlagTracking = 26;           // CHAN_15: the tracking system is announced
  constexpr unsigned kFlagLockedOut = 36;          // CHAN_11: "controls are locked out, you need a datathief"
  constexpr unsigned kFlagValveLocked = 122;       // CHAN_23: the valve locked down (pre-emptive)
  constexpr unsigned kFlagFansDestroyed = 141;     // CHAN_22: the vent fans destroyed (pre-emptive)
  constexpr unsigned kFlagJoThreat = 184;          // JO_10: Jo threatens the gas programmer
  constexpr unsigned kFlagsGoldRefused[] = {268, 269, 164, 165}; // MACHINE_01 / MACHINE_08 on the gold doors
  constexpr float kProgrammerReplyDelayS = 4.0f;   // after Jo's threat (her takes run 2.5-4 s)
  constexpr float kHintGoldDelayS = 6.0f;          // after the door's refusal line
  constexpr float kHintTrackingDelayS = 8.0f;      // after CHAN_15
  constexpr float kHintVirusDelayS = 6.0f;         // after CHAN_11
  constexpr float kHintGasDelayS = 7.0f;           // after CHAN_16
  constexpr float kProgrammerSearch2 = 1500.0f * 1500.0f; // the fallback speaker: a scientist within 15 m of Jo

  std::mutex g_mutex;
  uint32_t g_ref = 0;      // guest: cue reference + name text
  uint32_t g_iterator = 0; // guest: Rare's object iterator
  std::atomic<bool> g_capture_lights{false}; // the handler's lights branch is running (power return)

  float Clock(const uint8_t *base)
  {
    return LoadFloat(base, kAiClock);
  }

  uint32_t Difficulty(const uint8_t *base)
  {
    const uint32_t info = Load32(base, kGameState);
    return IsGuestPointer(info) ? Load32(base, info + 4) : 0;
  }

  bool FlagSet(const uint8_t *base, uint32_t script, unsigned flag)
  {
    const uint32_t words = Load32(base, script + kScriptFlags);
    return IsGuestPointer(words) && (Load32(base, words + 4 * (flag / 32)) & (1u << (flag % 32))) != 0;
  }

  void SetFlag(uint8_t *base, uint32_t script, unsigned flag)
  {
    const uint32_t words = Load32(base, script + kScriptFlags);
    if (IsGuestPointer(words))
    {
      const uint32_t address = words + 4 * (flag / 32);
      Store32(base, address, Load32(base, address) | (1u << (flag % 32)));
    }
  }

  uint32_t ObjectiveState(const uint8_t *base, uint32_t script, unsigned objective)
  {
    const uint32_t objectives = Load32(base, script + kScriptObjectives);
    return IsGuestPointer(objectives) ? Load32(base, objectives + 0x34 * objective) : 0;
  }

  bool IsCharacter(const uint8_t *base, uint32_t character)
  {
    return IsGuestPointer(character) && IsGuestPointer(Load32(base, character));
  }

  bool Alive(PPCContext &ctx, uint8_t *base, uint32_t character)
  {
    if (!IsCharacter(base, character))
    {
      return false;
    }
    PPCFunc *fn = pdz::GuestFunction(Load32(base, Load32(base, character) + kHealthSlot), "lab_restorations health");
    if (!fn)
    {
      return false;
    }
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = character;
    frame.ctx.r4.u64 = 0;
    fn(frame.ctx, base);
    return frame.ctx.f1.f64 > 0.0;
  }

  void PositionOf(const uint8_t *base, uint32_t character, float out[3])
  {
    for (int i = 0; i < 3; ++i)
    {
      out[i] = LoadFloat(base, character + kCharacterPosition + 4 * i);
    }
  }

  // A setup id as the name lookup packs it (sub_82142B70: the first four
  // bytes, little-endian, stopping at the end of the string).
  uint32_t PackId(const char *text, size_t length)
  {
    uint32_t id = 0;
    for (size_t i = 0; i < 4 && i < length && text[i]; ++i)
    {
      id |= uint32_t(uint8_t(text[i])) << (8 * i);
    }
    return id;
  }

  // A character's setup id ([[character+0x54]+0x34], the string the lookup
  // callback 82179B38 compares), packed; 0 when there is none.
  uint32_t SetupId(const uint8_t *base, uint32_t character)
  {
    const uint32_t data = Load32(base, character + kCharacterData);
    if (!IsGuestPointer(data))
    {
      return 0;
    }
    const uint32_t text = Load32(base, data + 0x34);
    if (!IsGuestPointer(text))
    {
      return 0;
    }
    return PackId(reinterpret_cast<const char *>(base + text), 4);
  }

  // Every living or dead character in the game-state list, through Rare's
  // own iterator (sub_82527378 / sub_825272F0, type filter 0x21), as the
  // handler's darkness loop walks them (823283F4-82328538). The callback
  // gets each character; returns how many it saw.
  template <typename F>
  uint32_t ForEachCharacter(PPCContext &ctx, uint8_t *base, F &&fn)
  {
    const uint32_t list = Load32(base, kObjectList);
    if (!IsGuestPointer(list))
    {
      return 0;
    }
    if (!g_iterator)
    {
      g_iterator = REX_KERNEL_MEMORY()->SystemHeapAlloc(kIteratorSize);
      if (!g_iterator)
      {
        return 0;
      }
    }
    const uint32_t it = g_iterator;
    Store32(base, it + 0x00, 0);          // current node
    Store32(base, it + 0x04, 0);          // current bucket
    Store32(base, it + 0x08, 0xFFFFFFFFu); // bucket index, -1 before the first
    Store32(base, it + 0x0C, 0x21);       // type filter: characters
    base[it + 0x10] = 0;
    Store32(base, it + 0x14, list);
    Store32(base, it + 0x18, Load32(base, kIteratorChain)); // chain in (deletion safety)
    Store32(base, kIteratorChain, it);
    {
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = it;
      sub_82527378(frame.ctx, base);
    }
    uint32_t seen = 0;
    uint32_t node = Load32(base, it);
    for (int guard = 0; IsGuestPointer(node) && guard < 4096; ++guard)
    {
      const uint32_t object = Load32(base, node + 4);
      const uint32_t next = Load32(base, node + 0xC);
      Store32(base, it, next);
      const uint32_t bucket = Load32(base, it + 4);
      if (IsGuestPointer(bucket) && Load32(base, bucket + 4) == next)
      {
        rex::CallFrame frame(ctx); // this bucket is done: on to the next
        frame.ctx.r3.u64 = it;
        sub_82527378(frame.ctx, base);
      }
      if (IsCharacter(base, object))
      {
        ++seen;
        fn(object);
      }
      node = Load32(base, it);
    }
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = it;
    sub_825272F0(frame.ctx, base);
    return seen;
  }

  // A cue reference for `name` in our guest buffer (sub_82202D20 fills it),
  // or 0.
  uint32_t CueRef(PPCContext &ctx, uint8_t *base, const char *name)
  {
    const size_t length = std::strlen(name);
    if (length + 1 > kNameSize)
    {
      return 0;
    }
    if (!g_ref)
    {
      g_ref = REX_KERNEL_MEMORY()->SystemHeapAlloc(kRefSize + kNameSize);
      if (!g_ref)
      {
        return 0;
      }
    }
    const uint32_t text = g_ref + kRefSize;
    std::memcpy(base + text, name, length + 1);
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = g_ref;
    frame.ctx.r4.u64 = text;
    sub_82202D20(frame.ctx, base);
    return g_ref;
  }

  // A Chandra line, queued as the handler queues hers (sub_822014B8 with
  // f1 6.0, f2 0.0, r6 1, r7 0, r8 0; it copies the reference, 82201590).
  void SayChandra(PPCContext &ctx, uint8_t *base, const char *name)
  {
    const uint32_t ref = CueRef(ctx, base, name);
    if (!ref)
    {
      return;
    }
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = ref;
    frame.ctx.r4.u64 = 0;
    frame.ctx.r5.u64 = 0;
    frame.ctx.r6.u64 = 1;
    frame.ctx.r7.u64 = 0;
    frame.ctx.r8.u64 = 0;
    frame.ctx.f1.f64 = 6.0;
    frame.ctx.f2.f64 = 0.0;
    sub_822014B8(frame.ctx, base);
  }

  // A cue as a positioned sound attached to a character (sub_821FF3C8, as
  // the level scripts play sounds on characters; restorations.cpp PlayOn).
  void SayOn(PPCContext &ctx, uint8_t *base, uint32_t character, const char *name)
  {
    const uint32_t ref = CueRef(ctx, base, name);
    if (!ref)
    {
      return;
    }
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = ref;
    frame.ctx.r4.u64 = 0;
    frame.ctx.r5.u64 = 0;
    frame.ctx.r6.u64 = character; // attached to the speaker
    frame.ctx.r7.u64 = 1;
    frame.ctx.f1.f64 = 0.0;
    sub_821FF3C8(frame.ctx, base);
  }

  // --- per-run state (one Laboratory script instance) ---
  struct Pending
  {
    float due = 0.0f;        // AI clock
    uint32_t character = 0;  // 0: a Chandra line
    const char *cue = nullptr;
    const char *what = "";
  };

  struct State
  {
    uint32_t script = 0;
    std::vector<Pending> pending;
    // (b) the power return
    std::vector<uint32_t> lights; // the lights the blackout switched off
    uint8_t ambient[kAmbientSize] = {};
    bool have_ambient = false;
    bool armed = false;           // the switch route started the timer
    float arm_clock = 0.0f;
    bool restored = false;
    // (e) the blackout coverage
    int coverage = 0;                   // the level latched at the blackout (0: no blackout yet)
    bool dark = false;                  // the blackout happened (lights or darkness message seen)
    bool relit = false;                 // the power return brought the lights back
    std::vector<uint32_t> extra_lights; // ours (also in `lights`)
    std::vector<uint32_t> darkened;     // the extra characters we darkened
    // pdz_fix_lab_tannoy_repeat
    uint32_t trigger03_plays = 0;
    bool tannoy_stopped_logged = false;
    // (d) the cut lines, each once
    bool gas_payoff_done = false;
    bool programmer_done = false;
    bool hint_gold_done = false;
    bool hint_tracking_done = false;
    bool hint_virus_done = false;
    bool hint_gas_done = false;
  };
  State g_state;

  // The state for this script; a new script pointer is a new run of the level.
  State &StateFor(uint32_t script)
  {
    if (g_state.script != script)
    {
      g_state = State{};
      g_state.script = script;
    }
    return g_state;
  }

  void Queue(State &st, const uint8_t *base, float delay_s, uint32_t character, const char *cue, const char *what)
  {
    st.pending.push_back(Pending{Clock(base) + delay_s, character, cue, what});
  }

  // Due lines: Chandra's through her queue, a character's as a positioned
  // sound (skipped when he has died meanwhile).
  void PlayDue(PPCContext &ctx, uint8_t *base, State &st)
  {
    const float now = Clock(base);
    for (size_t i = 0; i < st.pending.size();)
    {
      if (now < st.pending[i].due)
      {
        ++i;
        continue;
      }
      const Pending line = st.pending[i];
      st.pending.erase(st.pending.begin() + long(i));
      if (!line.character)
      {
        SayChandra(ctx, base, line.cue);
        REXLOG_INFO("pdz_lab_cut_lines: {}: Chandra {}", line.what, line.cue + 15);
      }
      else if (Alive(ctx, base, line.character))
      {
        SayOn(ctx, base, line.character, line.cue);
        REXLOG_INFO("pdz_lab_cut_lines: {}: {} on {:08X}", line.what, line.cue + 15, line.character);
      }
      else
      {
        REXLOG_INFO("pdz_lab_cut_lines: {}: speaker {:08X} is dead, {} dropped", line.what, line.character,
                    line.cue + 15);
      }
    }
  }

  // --- (a) the four darkened light-group boxes ---
  // From the Laboratory package's obbvolume records (research note 3, 2b):
  // centre, full size, yaw about the vertical axis. The containment test is
  // strict on all three axes after the inverse of the box's matrix
  // (sub_825D76A8); the rotation of a record (0, 0, t) was decoded as
  // M = [[cos t, 0, -sin t], [0, 1, 0], [sin t, 0, cos t]] (row vectors).
  struct Box
  {
    int group;
    float centre[3];
    float size[3];
    float yaw_deg;
  };
  constexpr Box kBoxes[] = {
      {1, {248.1f, 31.3f, -1801.8f}, {3500.0f, 3000.0f, 8000.0f}, 0.0f},   // main hall
      {2, {1333.8f, -942.8f, 3586.1f}, {4000.0f, 4000.0f, 4000.0f}, 37.0f}, // generator room
      {3, {-3550.9f, 600.0f, -1876.4f}, {3000.0f, 1500.0f, 2000.0f}, 0.0f}, // cutscene room
      {4, {4230.3f, -214.3f, -6824.4f}, {9000.0f, 9000.0f, 7100.0f}, 0.0f}, // security, lower security, tower
  };

  bool InBox(const Box &b, const float pos[3])
  {
    const float d[3] = {pos[0] - b.centre[0], pos[1] - b.centre[1], pos[2] - b.centre[2]};
    const float t = b.yaw_deg * 3.14159265358979f / 180.0f;
    const float c = std::cos(t);
    const float s = std::sin(t);
    // local = d * M^T
    const float local[3] = {d[0] * c - d[2] * s, d[1], d[0] * s + d[2] * c};
    for (int i = 0; i < 3; ++i)
    {
      if (std::fabs(local[i]) >= b.size[i] * 0.5f)
      {
        return false;
      }
    }
    return true;
  }

  // --- (e) the blackout coverage: the lights Rare's boxes leave on ---
  // Census areas: a light in one of Rare's boxes takes the box's area (the
  // first box in kBoxes order for the 34 lights in two boxes); any other
  // light takes the area of its table entry (the research's room label, by
  // the nearest character); a light in neither is "other" (a lamp prop's).
  enum Area : uint8_t
  {
    kAreaMainHall,
    kAreaGenerator,
    kAreaCutscene,
    kAreaSecurity, // security, lower security, the tower
    kAreaLabOne,
    kAreaLabTwo,
    kAreaPrison,
    kAreaVents,      // the spiderbot / vent area
    kAreaSpawnerEnd, // the corner past the main hall's south end (13 amber, 3 white)
    kAreaOther,
    kAreaCount
  };
  constexpr const char *kAreaNames[kAreaCount] = {"main hall", "generator room", "cutscene room", "security",
                                                  "Lab One", "Lab Two", "prison", "vents", "spawner end", "other"};
  constexpr Area kBoxAreas[] = {kAreaMainHall, kAreaGenerator, kAreaCutscene, kAreaSecurity}; // per kBoxes entry

  // Every switchable scene light (type 0 spot / 2 point) in no light group:
  // position (cm, the record's, light +0x2D0), census area, and the lowest
  // coverage level that switches it off (0: never; the 6 red cell lights and
  // the 17 amber security lights, by colour). Generated from
  // lights_groups.tsv by a table script;
  // the trailing number is the table's light index.
  struct ExtraLight
  {
    float x, y, z;
    Area area;
    uint8_t level;
  };
  constexpr ExtraLight kExtraLights[] = {
// 224 lights; per area and level: Generator L3: 1, LabOne L3: 52, LabTwo L3: 18, MainHall L2: 45, Prison L0: 6, Prison L3: 18, Security L0: 4, Security L2: 26, SpawnerEnd L0: 13, SpawnerEnd L3: 3, Vents L3: 38
      {7070.1f, 1655.8f, 628.1f, kAreaPrison, 0}, // 2 red
      {7318.5f, 1655.8f, 1323.6f, kAreaPrison, 0}, // 3 red
      {7153.9f, 1655.8f, 2024.5f, kAreaPrison, 0}, // 4 red
      {3986.8f, 1655.8f, 2024.5f, kAreaPrison, 0}, // 5 red
      {3787.7f, 1655.8f, 1323.6f, kAreaPrison, 0}, // 6 red
      {5506.5f, 1210.8f, 1678.4f, kAreaPrison, 3}, // 7 
      {4122.6f, -150.6f, 3803.6f, kAreaGenerator, 3}, // 9 
      {-37.9f, 49.6f, 6565.7f, kAreaVents, 3}, // 25 
      {-5454.1f, 922.3f, 588.0f, kAreaLabOne, 3}, // 31 
      {359.3f, 49.6f, 5724.1f, kAreaVents, 3}, // 34 
      {-48.8f, 49.6f, 5713.2f, kAreaVents, 3}, // 35 
      {4996.9f, 2175.4f, 2616.5f, kAreaPrison, 3}, // 37 
      {5530.1f, 2180.0f, -2635.3f, kAreaSecurity, 2}, // 38 
      {4391.3f, 1985.8f, -1580.1f, kAreaPrison, 3}, // 39 
      {6683.4f, 1985.8f, -1580.1f, kAreaPrison, 3}, // 40 
      {5520.0f, 1819.3f, -560.9f, kAreaPrison, 3}, // 41 
      {5495.6f, 1210.8f, 1072.8f, kAreaPrison, 3}, // 42 
      {3984.0f, 1655.8f, 628.1f, kAreaPrison, 0}, // 43 red
      {6081.9f, 1210.8f, 1930.5f, kAreaPrison, 3}, // 44 
      {6049.0f, 1210.8f, 785.1f, kAreaPrison, 3}, // 45 
      {4965.4f, 1210.8f, 785.1f, kAreaPrison, 3}, // 46 
      {4941.0f, 1210.8f, 1923.5f, kAreaPrison, 3}, // 47 
      {6060.6f, 2175.4f, 2616.5f, kAreaPrison, 3}, // 48 
      {-4844.4f, 616.6f, 1325.5f, kAreaLabOne, 3}, // 50 
      {-3401.9f, 1244.9f, 3518.1f, kAreaVents, 3}, // 55 
      {-59.7f, 49.6f, 6143.1f, kAreaVents, 3}, // 62 
      {-2710.2f, 616.6f, -149.6f, kAreaLabOne, 3}, // 67 
      {-2540.2f, 952.3f, 3380.3f, kAreaLabOne, 3}, // 74 
      {-5540.4f, 616.6f, 484.8f, kAreaLabOne, 3}, // 76 
      {-4277.0f, 616.6f, -511.3f, kAreaLabOne, 3}, // 77 
      {-4139.1f, 616.6f, -556.8f, kAreaLabOne, 3}, // 78 
      {-4546.6f, 1333.0f, 2420.2f, kAreaLabOne, 3}, // 79 
      {-4670.0f, 1193.1f, 1631.7f, kAreaLabOne, 3}, // 80 
      {-2533.5f, 616.6f, -93.9f, kAreaMainHall, 2}, // 82 
      {-2896.0f, 616.6f, -173.7f, kAreaLabOne, 3}, // 83 
      {-2394.3f, 616.6f, 14.9f, kAreaMainHall, 2}, // 84 
      {-4157.9f, 1383.0f, 2860.6f, kAreaLabOne, 3}, // 85 
      {-4770.2f, 1244.9f, 2019.0f, kAreaLabOne, 3}, // 86 
      {-4876.3f, 915.5f, -342.7f, kAreaLabOne, 3}, // 88 
      {-3450.0f, 1042.6f, 310.1f, kAreaLabOne, 3}, // 89 
      {-3470.1f, 922.3f, -588.5f, kAreaLabOne, 3}, // 90 
      {-2171.1f, 1003.4f, 400.0f, kAreaMainHall, 2}, // 91 
      {-2747.9f, 1383.0f, 2860.6f, kAreaLabOne, 3}, // 93 
      {-4600.1f, 1042.6f, 560.0f, kAreaLabOne, 3}, // 94 
      {-3977.8f, 853.9f, 1879.3f, kAreaLabOne, 3}, // 95 
      {-2878.9f, 853.9f, 1879.3f, kAreaLabOne, 3}, // 96 
      {-1281.7f, 727.7f, 4080.6f, kAreaLabOne, 3}, // 97 
      {-2635.7f, 952.3f, 3480.0f, kAreaLabOne, 3}, // 100 
      {-2791.0f, 952.3f, 3587.0f, kAreaVents, 3}, // 101 
      {-2915.9f, 952.3f, 3640.9f, kAreaVents, 3}, // 102 
      {-3096.5f, 952.3f, 3675.2f, kAreaVents, 3}, // 103 
      {-1941.1f, 638.0f, 2245.3f, kAreaLabOne, 3}, // 104 
      {-3434.7f, 952.3f, 3652.4f, kAreaVents, 3}, // 105 
      {-3559.4f, 952.3f, 3627.1f, kAreaVents, 3}, // 106 
      {-3755.3f, 952.3f, 3570.7f, kAreaVents, 3}, // 107 
      {-3877.8f, 952.3f, 3534.3f, kAreaVents, 3}, // 108 
      {-4079.7f, 952.3f, 3460.1f, kAreaVents, 3}, // 109 
      {-2441.3f, 952.3f, 3226.7f, kAreaLabOne, 3}, // 110 
      {-4272.7f, 952.3f, 3314.6f, kAreaLabOne, 3}, // 111 
      {-4260.8f, 952.3f, 3180.8f, kAreaLabOne, 3}, // 112 
      {-4193.2f, 952.3f, 3018.3f, kAreaLabOne, 3}, // 113 
      {-4441.9f, 952.3f, 2454.8f, kAreaLabOne, 3}, // 114 
      {-3191.2f, 717.5f, 2247.7f, kAreaLabOne, 3}, // 115 
      {-4123.2f, 714.0f, 1794.0f, kAreaLabOne, 3}, // 117 
      {-4123.2f, 714.0f, 1935.1f, kAreaLabOne, 3}, // 118 
      {-2774.3f, 717.8f, 1935.1f, kAreaLabOne, 3}, // 119 
      {-2774.3f, 717.8f, 1795.6f, kAreaLabOne, 3}, // 120 
      {-2462.7f, 616.6f, 976.3f, kAreaMainHall, 2}, // 121 
      {-2387.3f, 616.6f, 788.5f, kAreaMainHall, 2}, // 123 
      {-1799.3f, 638.0f, 3444.9f, kAreaLabOne, 3}, // 124 
      {-767.9f, 638.0f, 2277.7f, kAreaLabOne, 3}, // 128 
      {-3711.0f, 719.0f, 2251.4f, kAreaLabOne, 3}, // 130 
      {-1885.2f, 638.0f, 3292.5f, kAreaLabOne, 3}, // 137 
      {-1923.7f, 638.0f, 3163.3f, kAreaLabOne, 3}, // 138 
      {-1941.1f, 638.0f, 2790.1f, kAreaLabOne, 3}, // 139 
      {-1941.1f, 638.0f, 2637.8f, kAreaLabOne, 3}, // 140 
      {-3230.5f, 952.3f, 3675.2f, kAreaVents, 3}, // 141 
      {-1941.1f, 638.0f, 2113.2f, kAreaLabOne, 3}, // 142 
      {-2646.3f, 952.3f, 2180.3f, kAreaLabOne, 3}, // 143 
      {-2509.1f, 952.3f, 2163.5f, kAreaLabOne, 3}, // 144 
      {-2161.1f, 876.3f, 2537.7f, kAreaLabOne, 3}, // 145 
      {-2379.2f, 952.3f, 3097.7f, kAreaLabOne, 3}, // 146 
      {-2161.1f, 876.3f, 2331.1f, kAreaLabOne, 3}, // 159 
      {-1699.1f, 638.0f, 3528.8f, kAreaLabOne, 3}, // 160 
      {-1506.0f, 618.7f, -2819.1f, kAreaMainHall, 2}, // 162 
      {-1553.3f, 618.7f, -2694.9f, kAreaMainHall, 2}, // 163 
      {-1695.3f, 618.7f, -2521.0f, kAreaMainHall, 2}, // 164 
      {-2315.4f, 876.3f, 2001.9f, kAreaLabOne, 3}, // 166 
      {-2315.4f, 876.3f, 1864.8f, kAreaLabOne, 3}, // 167 
      {-2104.0f, 876.3f, 1784.2f, kAreaLabOne, 3}, // 168 
      {-2240.4f, 876.3f, 1784.2f, kAreaLabOne, 3}, // 169 
      {-2161.1f, 876.3f, 2199.2f, kAreaLabOne, 3}, // 170 
      {-4197.7f, 952.3f, 3409.6f, kAreaVents, 3}, // 173 
      {-1807.7f, 618.7f, -2457.3f, kAreaMainHall, 2}, // 179 
      {-4723.7f, 616.6f, 1484.5f, kAreaLabOne, 3}, // 196 
      {-1675.4f, 618.7f, -1930.5f, kAreaMainHall, 2}, // 203 
      {-1572.2f, 618.7f, -1744.0f, kAreaMainHall, 2}, // 204 
      {-1604.7f, 618.7f, -1599.5f, kAreaMainHall, 2}, // 205 
      {-1674.8f, 618.7f, 607.6f, kAreaMainHall, 2}, // 206 
      {-1526.1f, 618.7f, 607.6f, kAreaMainHall, 2}, // 207 
      {-1654.3f, -39.0f, -757.2f, kAreaMainHall, 2}, // 220 
      {-1805.5f, 618.7f, -1959.8f, kAreaMainHall, 2}, // 240 
      {-1544.6f, -39.0f, 100.0f, kAreaMainHall, 2}, // 252 
      {-1587.9f, -39.0f, -1761.0f, kAreaMainHall, 2}, // 264 
      {-1557.7f, -39.0f, -1894.6f, kAreaMainHall, 2}, // 277 
      {-1627.8f, -39.0f, -1568.8f, kAreaMainHall, 2}, // 279 
      {-1638.3f, -39.0f, -1434.7f, kAreaMainHall, 2}, // 280 
      {-1655.7f, -39.0f, -1234.7f, kAreaMainHall, 2}, // 281 
      {-1655.7f, -39.0f, -1094.9f, kAreaMainHall, 2}, // 282 
      {-1655.0f, -39.0f, -897.0f, kAreaMainHall, 2}, // 283 
      {-1650.7f, -39.0f, -563.7f, kAreaMainHall, 2}, // 285 
      {-1636.0f, -39.0f, -423.6f, kAreaMainHall, 2}, // 286 
      {-1609.6f, -39.0f, -210.8f, kAreaMainHall, 2}, // 287 
      {-1592.7f, -39.0f, -92.8f, kAreaMainHall, 2}, // 288 
      {-1508.9f, -39.0f, 232.4f, kAreaMainHall, 2}, // 290 
      {-1932.8f, 1003.4f, 400.0f, kAreaMainHall, 2}, // 385 
      {2083.1f, 307.5f, -3031.5f, kAreaMainHall, 2}, // 394 
      {3259.1f, 307.5f, -773.5f, kAreaLabTwo, 3}, // 395 
      {3617.3f, 307.5f, -746.2f, kAreaLabTwo, 3}, // 396 
      {3958.6f, 307.5f, -834.9f, kAreaLabTwo, 3}, // 397 
      {3586.0f, 592.4f, -1064.1f, kAreaLabTwo, 3}, // 398 
      {3496.4f, 617.4f, -2028.3f, kAreaLabTwo, 3}, // 399 
      {3496.4f, 617.4f, -3021.2f, kAreaLabTwo, 3}, // 400 
      {-2043.0f, 842.1f, -2200.8f, kAreaMainHall, 2}, // 401 
      {2969.0f, 307.5f, -981.6f, kAreaLabTwo, 3}, // 404 
      {2268.3f, 774.5f, -1320.0f, kAreaMainHall, 2}, // 406 
      {2214.3f, 788.8f, -2725.7f, kAreaMainHall, 2}, // 407 
      {2214.3f, 788.8f, -2220.1f, kAreaMainHall, 2}, // 408 
      {6219.5f, 263.1f, -1935.9f, kAreaVents, 3}, // 409 
      {4651.1f, 351.4f, 1649.1f, kAreaVents, 3}, // 414 
      {2083.1f, 307.5f, -1754.7f, kAreaMainHall, 2}, // 417 
      {2083.1f, 307.5f, -1537.4f, kAreaMainHall, 2}, // 418 
      {2083.1f, 307.5f, -1324.9f, kAreaMainHall, 2}, // 419 
      {2083.1f, 307.5f, -1115.7f, kAreaMainHall, 2}, // 420 
      {2214.3f, 788.8f, -1706.1f, kAreaMainHall, 2}, // 421 
      {6219.5f, 263.1f, -1564.9f, kAreaVents, 3}, // 422 
      {4705.2f, 257.1f, -1577.4f, kAreaLabTwo, 3}, // 423 
      {4705.2f, 257.1f, -1948.5f, kAreaLabTwo, 3}, // 424 
      {3863.0f, 1758.1f, -12938.3f, kAreaSecurity, 2}, // 425 
      {4391.0f, 220.6f, -3162.4f, kAreaLabTwo, 3}, // 431 
      {4252.0f, 307.5f, -1043.1f, kAreaLabTwo, 3}, // 432 
      {2083.1f, 307.5f, -2775.4f, kAreaMainHall, 2}, // 433 
      {2083.1f, 307.5f, -2573.2f, kAreaLabTwo, 3}, // 434 
      {2083.1f, 307.5f, -2355.8f, kAreaMainHall, 2}, // 435 
      {2083.1f, 307.5f, -2143.4f, kAreaMainHall, 2}, // 436 
      {2208.8f, 307.5f, -3233.8f, kAreaMainHall, 2}, // 437 
      {-144.1f, 1456.8f, -13684.9f, kAreaSecurity, 2}, // 438 
      {-144.1f, 1758.1f, -12938.3f, kAreaSecurity, 2}, // 439 
      {657.3f, 1456.8f, -13684.9f, kAreaSecurity, 2}, // 440 
      {657.3f, 1758.1f, -12938.3f, kAreaSecurity, 2}, // 441 
      {4537.3f, 1758.2f, -11011.9f, kAreaSecurity, 2}, // 442 
      {1456.6f, 1758.1f, -12938.3f, kAreaSecurity, 2}, // 443 
      {2269.7f, 1456.8f, -13684.9f, kAreaSecurity, 2}, // 444 
      {2269.7f, 1758.1f, -12938.3f, kAreaSecurity, 2}, // 445 
      {3061.3f, 1456.8f, -13684.9f, kAreaSecurity, 2}, // 446 
      {3061.3f, 1758.1f, -12938.3f, kAreaSecurity, 2}, // 447 
      {4667.4f, 1456.8f, -13684.9f, kAreaSecurity, 2}, // 450 
      {4667.4f, 1758.1f, -12938.3f, kAreaSecurity, 2}, // 451 
      {5735.3f, 1758.1f, -12938.3f, kAreaSecurity, 2}, // 452 
      {-4553.0f, 1052.8f, 5183.7f, kAreaVents, 3}, // 453 
      {3597.5f, 340.3f, -1513.8f, kAreaLabTwo, 3}, // 454 
      {2719.3f, 340.3f, -2239.3f, kAreaLabTwo, 3}, // 455 
      {3592.7f, 340.3f, -2530.4f, kAreaLabTwo, 3}, // 456 
      {3863.0f, 1456.8f, -13684.9f, kAreaSecurity, 2}, // 458 
      {-613.3f, 445.2f, 6223.0f, kAreaVents, 3}, // 462 
      {-1959.6f, 472.2f, 5834.5f, kAreaVents, 3}, // 463 
      {-3385.3f, 1032.9f, 5834.5f, kAreaVents, 3}, // 464 
      {-1125.0f, 1395.4f, -6930.7f, kAreaSpawnerEnd, 0}, // 465 amber
      {-3404.3f, 925.5f, 4540.2f, kAreaVents, 3}, // 466 
      {-1900.4f, 1063.9f, 4530.8f, kAreaVents, 3}, // 467 
      {5797.4f, 641.1f, -474.7f, kAreaVents, 3}, // 468 
      {4651.1f, 636.9f, 173.9f, kAreaVents, 3}, // 469 
      {4648.1f, 200.2f, 3137.7f, kAreaVents, 3}, // 471 
      {6934.3f, 616.1f, -1638.0f, kAreaVents, 3}, // 472 
      {6092.3f, 1758.2f, -11878.2f, kAreaSecurity, 2}, // 473 
      {6086.0f, 1758.2f, -11010.2f, kAreaSecurity, 2}, // 474 
      {1456.6f, 1456.8f, -13684.9f, kAreaSecurity, 2}, // 475 
      {4542.6f, 1758.2f, -11876.6f, kAreaSecurity, 2}, // 476 
      {-1750.6f, 1456.8f, -13684.9f, kAreaSecurity, 2}, // 477 
      {-1750.6f, 1758.1f, -12938.3f, kAreaSecurity, 2}, // 478 
      {-927.1f, 1456.8f, -13684.9f, kAreaSecurity, 2}, // 479 
      {-927.1f, 1758.1f, -12938.3f, kAreaSecurity, 2}, // 491 
      {5174.3f, 1341.8f, 1661.2f, kAreaPrison, 3}, // 495 
      {5899.1f, 1341.8f, 951.5f, kAreaPrison, 3}, // 497 
      {5170.1f, 1341.8f, 951.5f, kAreaPrison, 3}, // 498 
      {5899.1f, 1341.8f, 1661.2f, kAreaPrison, 3}, // 508 
      {2083.1f, 307.5f, -1934.1f, kAreaMainHall, 2}, // 525 
      {-635.2f, 517.8f, -7095.7f, kAreaSpawnerEnd, 3}, // 529 
      {5530.1f, 2180.0f, -2635.3f, kAreaSecurity, 2}, // 534 
      {-1516.4f, 1419.0f, -7120.9f, kAreaSpawnerEnd, 3}, // 565 
      {-438.4f, 1395.4f, -6704.7f, kAreaSpawnerEnd, 0}, // 649 amber
      {-459.7f, 318.2f, -7253.0f, kAreaSpawnerEnd, 0}, // 664 amber
      {-726.3f, 318.2f, -6904.9f, kAreaSpawnerEnd, 0}, // 666 amber
      {-767.4f, 318.2f, -7305.7f, kAreaSpawnerEnd, 0}, // 667 amber
      {-1102.6f, 318.2f, -7314.5f, kAreaSpawnerEnd, 0}, // 668 amber
      {-1102.6f, 318.2f, -6940.5f, kAreaSpawnerEnd, 0}, // 669 amber
      {-387.0f, 318.2f, -6770.1f, kAreaSpawnerEnd, 0}, // 671 amber
      {-816.0f, 1395.4f, -6922.2f, kAreaSpawnerEnd, 0}, // 695 amber
      {-613.0f, 1395.4f, -6839.0f, kAreaSpawnerEnd, 0}, // 696 amber
      {-304.2f, 1395.4f, -6532.2f, kAreaSpawnerEnd, 0}, // 698 amber
      {5735.3f, 1456.8f, -13684.9f, kAreaSecurity, 2}, // 706 
      {5191.6f, 2005.3f, -1654.0f, kAreaPrison, 3}, // 707 
      {-393.1f, 1395.4f, -7380.8f, kAreaSecurity, 0}, // 712 amber
      {-764.0f, 1395.4f, -7315.0f, kAreaSpawnerEnd, 0}, // 713 amber
      {5874.4f, 2005.3f, -1654.0f, kAreaPrison, 3}, // 723 
      {-1125.0f, 1395.4f, -7315.0f, kAreaSpawnerEnd, 0}, // 730 amber
      {5532.1f, 1951.5f, 353.5f, kAreaPrison, 3}, // 733 
      {5108.4f, 1858.9f, -3248.5f, kAreaSecurity, 0}, // 746 amber
      {5395.7f, 1858.9f, -2928.7f, kAreaSecurity, 0}, // 747 amber
      {5650.1f, 1858.9f, -2928.7f, kAreaSecurity, 0}, // 752 amber
      {3586.7f, 268.4f, -1416.8f, kAreaLabTwo, 3}, // 756 
      {4950.6f, 609.3f, -1382.0f, kAreaVents, 3}, // 759 
      {5354.5f, 606.3f, -1247.0f, kAreaVents, 3}, // 761 
      {5787.3f, 606.9f, -1286.0f, kAreaVents, 3}, // 762 
      {5972.3f, 601.9f, -1382.0f, kAreaVents, 3}, // 763 
      {4949.7f, 601.1f, -2118.3f, kAreaLabTwo, 3}, // 764 
      {5135.5f, 608.8f, -1284.7f, kAreaVents, 3}, // 769 
      {5133.9f, 608.7f, -2215.0f, kAreaLabTwo, 3}, // 773 
      {-1516.3f, 1852.8f, -7120.3f, kAreaSpawnerEnd, 3}, // 776 
      {5568.5f, 606.4f, -1246.7f, kAreaVents, 3}, // 777 
      {5970.2f, 609.9f, -2118.9f, kAreaVents, 3}, // 778 
      {5353.0f, 614.6f, -2253.3f, kAreaVents, 3}, // 783 
      {5566.7f, 614.7f, -2253.6f, kAreaVents, 3}, // 784 
      {5785.6f, 615.2f, -2215.0f, kAreaVents, 3}, // 785 
  };

  bool LightOn(const uint8_t *base, uint32_t light)
  {
    const uint32_t states = Load32(base, kLightStates); // 0 or 1 in practice
    return states < 16 && base[light + kLightStateByte + states] != 0;
  }

  // Every scene light in Rare's list (what sub_82601F58 walks: kLightArray,
  // kLightCount), owned prop lights (a lamp's own 11 cm light) skipped.
  template <typename F>
  void ForEachLight(const uint8_t *base, F &&fn)
  {
    const uint32_t count = std::min<uint32_t>(Load32(base, kLightCount), kMaxLights);
    for (uint32_t i = 0; i < count; ++i)
    {
      const uint32_t light = Load32(base, kLightArray + 4 * i);
      if (IsGuestPointer(light) && Load32(base, light + kLightOwner) == 0)
      {
        fn(light);
      }
    }
  }

  const ExtraLight *FindExtraLight(const uint8_t *base, uint32_t light)
  {
    const float x = LoadFloat(base, light + kLightPosition);
    const float y = LoadFloat(base, light + kLightPosition + 4);
    const float z = LoadFloat(base, light + kLightPosition + 8);
    for (const ExtraLight &e : kExtraLights)
    {
      if (std::fabs(e.x - x) <= kLightMatch && std::fabs(e.y - y) <= kLightMatch && std::fabs(e.z - z) <= kLightMatch)
      {
        return &e;
      }
    }
    return nullptr;
  }

  Area AreaOf(const uint8_t *base, uint32_t light)
  {
    float pos[3];
    for (int i = 0; i < 3; ++i)
    {
      pos[i] = LoadFloat(base, light + kLightPosition + 4 * uint32_t(i));
    }
    for (size_t i = 0; i < sizeof(kBoxes) / sizeof(kBoxes[0]); ++i)
    {
      if (InBox(kBoxes[i], pos))
      {
        return kBoxAreas[i];
      }
    }
    const ExtraLight *e = FindExtraLight(base, light);
    return e ? e->area : kAreaOther;
  }

  // Rare's light state setter (sub_82602648): on / off with a fade in seconds
  // (0 = this frame; the sun ignores "off").
  void SetLight(PPCContext &ctx, uint8_t *base, uint32_t light, bool on, float fade_s)
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = light;
    frame.ctx.r4.u64 = on ? 1 : 0;
    frame.ctx.f1.f64 = double(fade_s);
    sub_82602648(frame.ctx, base);
  }

  std::string AreaCounts(const uint32_t counts[kAreaCount])
  {
    std::string out;
    for (uint32_t a = 0; a < kAreaCount; ++a)
    {
      if (counts[a])
      {
        out += (out.empty() ? "" : ", ") + std::string(kAreaNames[a]) + " " + std::to_string(counts[a]);
      }
    }
    return out.empty() ? "none" : out;
  }

  // pdz_test_lab_light_census: every scene light's state, in total and per area.
  void LightCensus(const uint8_t *base, const char *when)
  {
    uint32_t on[kAreaCount] = {};
    uint32_t off[kAreaCount] = {};
    uint32_t total_on = 0;
    uint32_t total_off = 0;
    uint32_t sun = 0;
    ForEachLight(base, [&](uint32_t light) {
      const uint32_t type = Load32(base, light + kLightType);
      if (type == 1)
      {
        ++sun;
        return;
      }
      if (type != 0 && type != 2)
      {
        return;
      }
      const Area a = AreaOf(base, light);
      if (LightOn(base, light))
      {
        ++on[a];
        ++total_on;
      }
      else
      {
        ++off[a];
        ++total_off;
      }
    });
    std::string areas;
    for (uint32_t a = 0; a < kAreaCount; ++a)
    {
      if (on[a] || off[a])
      {
        areas += (areas.empty() ? "" : ", ") + std::string(kAreaNames[a]) + " " + std::to_string(on[a]) + "/" +
                 std::to_string(off[a]);
      }
    }
    REXLOG_INFO("pdz_test_lab_light_census: {}: {} scene lights ({} sun apart): {} on, {} off; per area on/off: {}", when,
                total_on + total_off, sun, total_on, total_off, areas);
  }

  // The coverage level for this run: latched from the cvar at the blackout.
  int CoverageLevel(State &st)
  {
    if (st.coverage == 0)
    {
      st.coverage = std::clamp<int>(REXCVAR_GET(pdz_lab_blackout_coverage), 1, 3);
    }
    return st.coverage;
  }

  // Level 2 / 3: the table's lights up to `level` that are still on go off
  // this frame, as Rare's toggle switched his (value 0 -> fade 0), and are
  // recorded for the power return. Lights already off (shot out) are left as
  // Rare leaves his.
  void SwitchOffExtraLights(PPCContext &ctx, uint8_t *base, State &st, int level)
  {
    uint32_t per_area[kAreaCount] = {};
    uint32_t n = 0;
    uint32_t already_off = 0;
    ForEachLight(base, [&](uint32_t light) {
      const uint32_t type = Load32(base, light + kLightType);
      if (type != 0 && type != 2)
      {
        return;
      }
      const ExtraLight *e = FindExtraLight(base, light);
      if (!e || e->level == 0 || int(e->level) > level)
      {
        return;
      }
      if (!LightOn(base, light))
      {
        ++already_off;
        return;
      }
      SetLight(ctx, base, light, false, 0.0f);
      st.lights.push_back(light);
      st.extra_lights.push_back(light);
      ++per_area[e->area];
      ++n;
    });
    REXLOG_INFO("pdz_lab_blackout_coverage: level {}: {} extra lights switched off this frame with Rare's ({}); {} of the set "
                "were already off",
                level, n, AreaCounts(per_area), already_off);
  }

  // The characters Rare's darkness list leaves out (script slots: pointer
  // words, note 1b 2.2). The prison guards get Rare's full darkness, the
  // scientists sight only (see the header).
  struct ExtraCharacter
  {
    uint32_t slot;
    const char *name;
    bool guard;
  };
  constexpr ExtraCharacter kExtraCharacters[] = {
      {0x4A4, "s2s1", false}, {0x4AC, "s2s2", false}, {0x504, "s4l1", false}, {0x50C, "s4l2", false},
      {0x514, "s4l3", false}, {0x51C, "s4l4", false}, {0x524, "s4l5", false}, {0x52C, "s5l1", false},
      {0x534, "s5l2", false}, {0x53C, "s5l3", false}, {0x5A4, "s9p1", true},  {0x5AC, "s9p2", true},
      {0x5B4, "s9p3", true},  {0x5BC, "s9g1", true},  {0x5C4, "s9g2", true},  {0x5CC, "s9g3", true},
      {0x5D4, "s9g4", true},  {0x5DC, "s9g5", true},  {0x5E4, "s9g6", true},  {0x5EC, "s9g7", true},
      {0x5F4, "s9g8", true},
  };

  // His perception profile, or 0 (dormant / unregistered: slot -1).
  uint32_t Profile(const uint8_t *base, uint32_t character)
  {
    const int32_t slot = int32_t(Load32(base, character + kCharacterProfileSlot));
    return (slot >= 0 && slot < int32_t(kProfileSlots)) ? kProfileTable + 0x50 * uint32_t(slot) : 0;
  }

  // Not yet dark: the profile shows registration values (a wake re-registers
  // it, 82581974, which resets the range and sets +0x49 = 1).
  bool NeedsDarkness(const uint8_t *base, uint32_t profile)
  {
    return LoadFloat(base, profile + 0x1C) != kDarkRange2 || base[profile + 0x49] != 0;
  }

  // Rare's targetless SEARCH task, built as the darkness loop builds it
  // (82328480-823284B8: allocate 0xA8 bytes, sub_821DF550 with no pad and no
  // target, hand it to the scheduler).
  bool GiveSearchTask(PPCContext &ctx, uint8_t *base, uint32_t character)
  {
    uint32_t memory = 0;
    {
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = 0xA8;
      frame.ctx.r4.u64 = 10;
      sub_8239D740(frame.ctx, base);
      memory = frame.ctx.r3.u32;
    }
    if (!IsGuestPointer(memory))
    {
      return false;
    }
    uint32_t task = 0;
    {
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = memory;
      frame.ctx.r4.u64 = 0;
      frame.ctx.r5.u64 = 0;
      frame.ctx.r6.u64 = 0;
      frame.ctx.r7.u64 = 0;
      sub_821DF550(frame.ctx, base);
      task = frame.ctx.r3.u32;
    }
    if (!IsGuestPointer(task))
    {
      return false;
    }
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = character + kCharacterScheduler;
    frame.ctx.r4.u64 = task;
    sub_821712D8(frame.ctx, base);
    return true;
  }

  // Rare's darkness writes for one character (823283F4-823284D0): squared
  // sight range 1e6, the dark aim bit, the search floor switch off, and
  // (guards) the SEARCH task. Logged with id, position and ranges.
  void Darken(PPCContext &ctx, uint8_t *base, State &st, uint32_t character, uint32_t profile, const ExtraCharacter &who,
              const char *why)
  {
    const float old2 = LoadFloat(base, profile + 0x1C);
    uint32_t bits;
    std::memcpy(&bits, &kDarkRange2, sizeof(bits));
    Store32(base, profile + 0x1C, bits);
    base[profile + 0x49] = 0;
    Store32(base, character + kCharacterAiFlags, Load32(base, character + kCharacterAiFlags) | kDarkAim);
    const bool task = who.guard && GiveSearchTask(ctx, base, character);
    st.darkened.push_back(character);
    float pos[3];
    PositionOf(base, character, pos);
    const uint32_t data = Load32(base, character + kCharacterData);
    const float def = IsGuestPointer(data) ? LoadFloat(base, data + 0x40) : 0.0f;
    REXLOG_INFO("pdz_lab_blackout_coverage: darkened {} ({:08X}) at ({:.0f} {:.0f} {:.0f}), {}: squared sight range {:.0f} -> "
                "{:.0f} (default {:.0f} units, now 1,000), search floor switch off, dark aim bit{}",
                who.name, character, pos[0], pos[1], pos[2], why, old2, kDarkRange2, def,
                task ? ", SEARCH task" : (who.guard ? ", no SEARCH task (allocation failed)" : ", no SEARCH task (scientist)"));
  }

  // Level 3: every living extra character with a profile that still shows
  // the light values. Returns how many; `names` lists them.
  uint32_t DarkenExtraCharacters(PPCContext &ctx, uint8_t *base, State &st, const char *why, std::string &names)
  {
    uint32_t n = 0;
    for (const ExtraCharacter &e : kExtraCharacters)
    {
      const uint32_t c = Load32(base, st.script + e.slot);
      if (!IsCharacter(base, c))
      {
        continue;
      }
      if (Load32(base, c + kCharacterAiFlags) & kDarknessList)
      {
        continue; // Rare's list has him: his loop and his wake-ups do the work
      }
      const uint32_t profile = Profile(base, c);
      if (!profile || !NeedsDarkness(base, profile) || !Alive(ctx, base, c))
      {
        continue;
      }
      Darken(ctx, base, st, c, profile, e, why);
      names += (names.empty() ? "" : " ") + std::string(e.name);
      ++n;
    }
    return n;
  }

  // --- (b) the power return ---
  // Every character carrying the dark aim bit: the bit off, his default
  // sight range back (Rare's uncalled sub_8252D728) and the search floor
  // switch back to its registration value. Returns how many.
  uint32_t UndarkenGuards(PPCContext &ctx, uint8_t *base)
  {
    uint32_t n = 0;
    ForEachCharacter(ctx, base, [&](uint32_t c) {
      const uint32_t flags = Load32(base, c + kCharacterAiFlags);
      if ((flags & kDarkAim) == 0)
      {
        return;
      }
      Store32(base, c + kCharacterAiFlags, flags & ~kDarkAim);
      const int32_t slot = int32_t(Load32(base, c + kCharacterProfileSlot));
      if (slot >= 0 && slot < 171)
      {
        rex::CallFrame frame(ctx);
        frame.ctx.r3.u64 = c;
        sub_8252D728(frame.ctx, base);
        base[kProfileTable + 0x50 * uint32_t(slot) + 0x49] = 1;
      }
      ++n;
    });
    return n;
  }

  void RestorePower(PPCContext &ctx, uint8_t *base, State &st)
  {
    st.restored = true;
    if (FlagSet(base, st.script, kFlagExplosives))
    {
      REXLOG_INFO("pdz_lab_power_return: generator blown up since the switch: the power stays off");
      return;
    }
    std::sort(st.lights.begin(), st.lights.end());
    st.lights.erase(std::unique(st.lights.begin(), st.lights.end()), st.lights.end());
    uint32_t lights = 0;
    for (const uint32_t light : st.lights)
    {
      if (!IsGuestPointer(light))
      {
        continue;
      }
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = light;
      frame.ctx.r4.u64 = 1;
      frame.ctx.f1.f64 = double(kLightFadeS);
      sub_82602648(frame.ctx, base);
      ++lights;
    }
    if (st.have_ambient)
    {
      std::memcpy(base + kAmbient, st.ambient, kAmbientSize);
    }
    else
    {
      // The setup's values (8231E2B0-8231E30C; the scene record's own):
      // r4 (0.0567971, 0.0659179, 0.06612, 0) at +0, r3 (0.168024, 0.22314, 0.219548, 0) at +0x10.
      const float values[8] = {0.0567971f, 0.0659179f, 0.06612f, 0.0f, 0.168024f, 0.22314f, 0.219548f, 0.0f};
      for (int i = 0; i < 8; ++i)
      {
        uint32_t bits;
        std::memcpy(&bits, &values[i], sizeof(bits));
        Store32(base, kAmbient + 4 * uint32_t(i), bits);
      }
    }
    // (e) the extra characters still carrying the dark aim bit: UndarkenGuards
    // restores them with Rare's (bit off, default range, +0x49 = 1).
    uint32_t extra_characters = 0;
    for (const uint32_t c : st.darkened)
    {
      if (IsCharacter(base, c) && (Load32(base, c + kCharacterAiFlags) & kDarkAim))
      {
        ++extra_characters;
      }
    }
    const uint32_t guards = UndarkenGuards(ctx, base);
    st.relit = true;
    SetFlag(base, st.script, kFlagPowerBack);
    SayChandra(ctx, base, "XACT_SOUNDBANK_LV05_SPEECH_CHAN_07");
    REXLOG_INFO("pdz_lab_power_return: power back {:.1f} s after the switch: {} lights fading in over {:.1f} s, ambient {}, "
                "{} guards undarkened, flag 40 set (tannoy promises stop), Chandra LV05_SPEECH_CHAN_07",
                Clock(base) - st.arm_clock, lights, kLightFadeS, st.have_ambient ? "restored" : "set to the setup values",
                guards);
    if (st.coverage > 1)
    {
      REXLOG_INFO("pdz_lab_blackout_coverage: power back: {} extra lights (level {}) restored with Rare's; {} of the {} extra "
                  "character(s) darkened were still dark and are restored (the rest were dead or already restored)",
                  st.extra_lights.size(), st.coverage, extra_characters, st.darkened.size());
    }
    if (REXCVAR_GET(pdz_test_lab_light_census))
    {
      LightCensus(base, "power return (a light fading in counts as on)");
    }
  }

  // --- (c) the darkness list ---
  bool MarkDarkness(uint8_t *base, uint32_t character)
  {
    const uint32_t flags = Load32(base, character + kCharacterAiFlags);
    if (flags & kDarknessList)
    {
      return false;
    }
    Store32(base, character + kCharacterAiFlags, flags | kDarknessList);
    return true;
  }

  void FixDarknessList(PPCContext &ctx, uint8_t *base, uint32_t script)
  {
    // The surviving walkway guard (the setup deleted the other two variants).
    const struct
    {
      uint32_t slot;
      const char *name;
    } walkway[] = {{kSlotWalkwayB, "s3gb"}, {kSlotWalkwayC, "s3gc"}};
    for (const auto &w : walkway)
    {
      const uint32_t c = Load32(base, script + w.slot);
      if (!IsCharacter(base, c) || !Alive(ctx, base, c))
      {
        continue;
      }
      float pos[3];
      PositionOf(base, c, pos);
      REXLOG_INFO("pdz_lab_darkness_list: walkway guard {} ({:08X}) at ({:.0f} {:.0f} {:.0f}) {}", w.name, c, pos[0], pos[1],
                  pos[2], MarkDarkness(base, c) ? "put on the darkness list" : "already on the list");
    }
    // Every character tagged "s3g4": the slot holds the first found; the
    // other (ais/s03generator6, meant to be s3g6) is unreferenced.
    const uint32_t key = PackId("s3g4", 4);
    const uint32_t found = Load32(base, script + kSlotGenerator4);
    uint32_t seen = 0;
    ForEachCharacter(ctx, base, [&](uint32_t c) {
      if (SetupId(base, c) != key)
      {
        return;
      }
      ++seen;
      float pos[3];
      PositionOf(base, c, pos);
      REXLOG_INFO("pdz_lab_darkness_list: \"s3g4\" record {} ({:08X}) at ({:.0f} {:.0f} {:.0f}) {}",
                  c == found ? "in the script slot" : "not in the script slot (s03generator6, meant to be s3g6)", c, pos[0],
                  pos[1], pos[2], MarkDarkness(base, c) ? "put on the darkness list" : "already on the list");
    });
    REXLOG_INFO("pdz_lab_darkness_list: setup done: {} character(s) tagged \"s3g4\" (Rare's data has 2)", seen);
  }
} // namespace

// sub_8231B778: the Laboratory script's setup (r3 = the script). A new run
// of the level: our state starts over; then the darkness list fix.
REX_HOOK_RAW(sub_8231B778)
{
  const uint32_t script = ctx.r3.u32;
  __imp__sub_8231B778(ctx, base);
  if (!IsGuestPointer(script))
  {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state = State{};
  g_state.script = script;
  if (REXCVAR_GET(pdz_lab_darkness_list))
  {
    FixDarknessList(ctx, base, script);
  }
}

namespace
{
  // --- (d) the cut lines ---
  // The gas programmer: s5l1, or the living scientist nearest Jo.
  uint32_t Programmer(PPCContext &ctx, uint8_t *base, uint32_t script)
  {
    const uint32_t s5l1 = Load32(base, script + kSlotProgrammer);
    if (IsCharacter(base, s5l1) && Alive(ctx, base, s5l1))
    {
      return s5l1;
    }
    const uint32_t player = Load32(base, kPlayerOne);
    if (!IsCharacter(base, player))
    {
      return 0;
    }
    float jo[3];
    PositionOf(base, player, jo);
    uint32_t best = 0;
    float best_d2 = kProgrammerSearch2;
    for (uint32_t slot = kSlotScientistFirst; slot <= kSlotScientistLast; slot += 8)
    {
      const uint32_t c = Load32(base, script + slot);
      if (!IsCharacter(base, c) || !Alive(ctx, base, c))
      {
        continue;
      }
      float pos[3];
      PositionOf(base, c, pos);
      const float d2 = (pos[0] - jo[0]) * (pos[0] - jo[0]) + (pos[1] - jo[1]) * (pos[1] - jo[1]) +
                       (pos[2] - jo[2]) * (pos[2] - jo[2]);
      if (d2 < best_d2)
      {
        best_d2 = d2;
        best = c;
      }
    }
    return best;
  }

  // The tick's gas-start test (82320A54-82320AC0) with the objective already
  // complete: the attack Rare skips, explained by Chandra.
  void GasPayoff(PPCContext &ctx, uint8_t *base, State &st)
  {
    if (st.gas_payoff_done || Difficulty(base) < 2 || FlagSet(base, st.script, kFlagGasStart) ||
        ObjectiveState(base, st.script, 1) != 2)
    {
      return;
    }
    const double stamp = LoadDouble(base, st.script + kScriptGasStamp);
    const float delay = LoadFloat(base, st.script + kScriptGasDelay);
    if (float(double(Clock(base)) - stamp) <= delay)
    {
      return;
    }
    st.gas_payoff_done = true;
    if (FlagSet(base, st.script, kFlagValveLocked))
    {
      SayChandra(ctx, base, "XACT_SOUNDBANK_LV05_SPEECH_CHAN_24");
      REXLOG_INFO("pdz_lab_cut_lines: gas attack pre-empted (valve locked, flag 122): Chandra LV05_SPEECH_CHAN_24");
    }
    else if (FlagSet(base, st.script, kFlagFansDestroyed))
    {
      SayChandra(ctx, base, "XACT_SOUNDBANK_LV05_SPEECH_CHAN_25");
      REXLOG_INFO("pdz_lab_cut_lines: gas attack pre-empted (vent fans destroyed, flag 141): Chandra LV05_SPEECH_CHAN_25");
    }
    else
    {
      REXLOG_INFO("pdz_lab_cut_lines: gas attack pre-empted, but neither the valve (122) nor the fans (141) flag is set: no payoff line");
    }
  }

  void CutLinesOnMessage(PPCContext &ctx, uint8_t *base, State &st, const lab_restorations::Message &m)
  {
    using lab_restorations::Changed;
    if (!st.programmer_done && Changed(m, kFlagJoThreat))
    {
      st.programmer_done = true;
      const uint32_t who = Programmer(ctx, base, st.script);
      if (who)
      {
        Queue(st, base, kProgrammerReplyDelayS, who, "XACT_SOUNDBANK_LV05_SPEECH_SCIENTIST_27", "programmer's reply");
        REXLOG_INFO("pdz_lab_cut_lines: Jo's threat (JO_10, flag 184): SCIENTIST_27 queued on {:08X} ({}) in {:.1f} s", who,
                    who == Load32(base, st.script + kSlotProgrammer) ? "s5l1" : "the scientist nearest Jo",
                    kProgrammerReplyDelayS);
      }
      else
      {
        REXLOG_INFO("pdz_lab_cut_lines: Jo's threat (JO_10, flag 184): no living programmer found, SCIENTIST_27 dropped");
      }
    }
    bool gold = false;
    for (const unsigned flag : kFlagsGoldRefused)
    {
      gold = gold || Changed(m, flag);
    }
    if (!st.hint_gold_done && gold)
    {
      st.hint_gold_done = true;
      Queue(st, base, kHintGoldDelayS, 0, "XACT_SOUNDBANK_LV05_SPEECH_CHAN_HINT_02", "gold door refused Jo (hint 02)");
      REXLOG_INFO("pdz_lab_cut_lines: gold door refused Jo (MACHINE_01 / 08): CHAN_HINT_02 queued in {:.1f} s", kHintGoldDelayS);
    }
    if (!st.hint_tracking_done && Changed(m, kFlagTracking))
    {
      st.hint_tracking_done = true;
      Queue(st, base, kHintTrackingDelayS, 0, "XACT_SOUNDBANK_LV05_SPEECH_CHAN_HINT_07", "tracking system announced (hint 07)");
      REXLOG_INFO("pdz_lab_cut_lines: tracking system announced (CHAN_15, flag 26): CHAN_HINT_07 queued in {:.1f} s",
                  kHintTrackingDelayS);
    }
    if (!st.hint_virus_done && Changed(m, kFlagLockedOut))
    {
      st.hint_virus_done = true;
      Queue(st, base, kHintVirusDelayS, 0, "XACT_SOUNDBANK_LV05_SPEECH_CHAN_HINT_08", "controls locked out (hint 08)");
      REXLOG_INFO("pdz_lab_cut_lines: controls locked out (CHAN_11, flag 36): CHAN_HINT_08 queued in {:.1f} s", kHintVirusDelayS);
    }
    if (!st.hint_gas_done && Changed(m, kFlagGasStart))
    {
      st.hint_gas_done = true;
      Queue(st, base, kHintGasDelayS, 0, "XACT_SOUNDBANK_LV05_SPEECH_CHAN_HINT_10", "gas attack started (hint 10)");
      REXLOG_INFO("pdz_lab_cut_lines: gas attack started (CHAN_16, flag 31): CHAN_HINT_10 queued in {:.1f} s", kHintGasDelayS);
    }
  }
} // namespace

// sub_825F3B68: one light's level message (r3 light, r4 type). Observed
// only while the Laboratory handler's lights branch runs with
// pdz_lab_power_return on: the lights its Lights_Toggle (111) reaches are
// the ones the power return brings back.
REX_HOOK_RAW(sub_825F3B68)
{
  if (g_capture_lights.load(std::memory_order_relaxed) && (ctx.r4.u32 & 0xFF) == 111 && IsGuestPointer(ctx.r3.u32))
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state.lights.push_back(ctx.r3.u32);
  }
  __imp__sub_825F3B68(ctx, base);
}

// sub_8231F2E8: the Laboratory script's per-frame tick (r3 = the script,
// f1 = dt). Our timers run after Rare's.
namespace
{
  std::atomic<uint32_t> g_tick_script{0};
  std::atomic<int64_t> g_tick_ms{0};
} // namespace

REX_HOOK_RAW(sub_8231F2E8)
{
  const uint32_t script = ctx.r3.u32;
  g_tick_script.store(script, std::memory_order_relaxed);
  g_tick_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count(),
                  std::memory_order_relaxed);
  __imp__sub_8231F2E8(ctx, base);
  if (!IsGuestPointer(script))
  {
    return;
  }
  const bool power = REXCVAR_GET(pdz_lab_power_return);
  const bool cut = REXCVAR_GET(pdz_lab_cut_lines);
  if (!power && !cut)
  {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  State &st = StateFor(script);
  if (power && st.armed && !st.restored && Clock(base) - st.arm_clock >= float(REXCVAR_GET(pdz_lab_power_return_s)))
  {
    RestorePower(ctx, base, st);
  }
  if (cut)
  {
    GasPayoff(ctx, base, st);
    PlayDue(ctx, base, st);
  }
}

// sub_823223F0: the Laboratory script's level-message receiver (r3 = the
// script). Its wake-ups and spawns darken guards while objective 0 is
// complete, power or no power (823265EC, 82326BA4, 82326EB4): once the power
// is back, any guard it darkened is restored again. Its prison wake-up
// (type 7 byte 6, 82326A28) registers the prison guards' perception with the
// light values and applies no darkness: with pdz_lab_blackout_coverage 3 and
// the blackout on, they are darkened here, in the same call.
REX_HOOK_RAW(sub_823223F0)
{
  const uint32_t script = ctx.r3.u32;
  __imp__sub_823223F0(ctx, base);
  if (!IsGuestPointer(script))
  {
    return;
  }
  const bool power = REXCVAR_GET(pdz_lab_power_return);
  if (!power && REXCVAR_GET(pdz_lab_blackout_coverage) < 3)
  {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  State &st = StateFor(script);
  if (power && st.restored && !FlagSet(base, st.script, kFlagExplosives))
  {
    const uint32_t n = UndarkenGuards(ctx, base);
    if (n)
    {
      REXLOG_INFO("pdz_lab_power_return: {} guard(s) darkened by a level message after the power came back: undarkened", n);
    }
  }
  if (st.dark && !st.relit && st.coverage >= 3)
  {
    std::string names;
    const uint32_t n = DarkenExtraCharacters(ctx, base, st, "woken by a level message after the blackout", names);
    if (n)
    {
      REXLOG_INFO("pdz_lab_blackout_coverage: {} extra character(s) registered by a level message after the blackout (the "
                  "prison wake-up) darkened: {}",
                  n, names);
    }
  }
}

// sub_8231B330: the Laboratory tannoy scheduler (r3 = the script; research
// 1a 3.2, notes 7_followup Q6c). Idle: when its timer runs out it picks a
// power tannoy (state 2) while objective 0 (the generator) is complete and
// flag 40 is clear (8231B464-8231B488), else a generic one (state 1) unless
// the alarm (flag 27) is on. State 2: interval exactly 10.0 (the blackout
// cases write it) -> flag 97 TRIGGER02 ("temporary power failure"), else
// flag 98 TRIGGER03 ("apologize for the delay in reconnecting the power"),
// then idle with 30-60 s (8231B394-8231B43C). Flag 40 has no writer, so
// TRIGGER03 repeats for the rest of the mission, over the alarm too.
// pdz_fix_lab_tannoy_repeat: after kMaxTrigger03 TRIGGER03 passes the
// scheduler is skipped while the blackout lasts (objective 0 complete, flag 40
// clear); nothing else is scheduled then anyway (generic tannoys need state
// 1, which idle never picks in the blackout). Flag 40 itself is not set, so
// the joke tannoys don't return in the dark; pdz_lab_power_return sets it
// when the power comes back and the scheduler runs as Rare wrote it again.
REX_HOOK_RAW(sub_8231B330)
{
  const uint32_t script = ctx.r3.u32;
  if (!REXCVAR_GET(pdz_fix_lab_tannoy_repeat) || !IsGuestPointer(script))
  {
    __imp__sub_8231B330(ctx, base);
    return;
  }
  bool trigger03_pass = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    State &st = StateFor(script);
    const bool blackout = ObjectiveState(base, script, 0) == 2 && !FlagSet(base, script, kFlagPowerBack);
    if (blackout && st.trigger03_plays >= kMaxTrigger03)
    {
      if (!st.tannoy_stopped_logged)
      {
        st.tannoy_stopped_logged = true;
        REXLOG_INFO("pdz_fix_lab_tannoy_repeat: TANNOY_TRIGGER03 played {} times: power announcements stop (flag 40 left "
                    "clear)", st.trigger03_plays);
      }
      return; // Rare's scheduler is not run
    }
    trigger03_pass = blackout && Load32(base, script + kScriptTannoyState) == 2 &&
                     LoadFloat(base, script + kScriptTannoyInterval) != 10.0f;
  }
  __imp__sub_8231B330(ctx, base);
  if (trigger03_pass && Load32(base, script + kScriptTannoyState) == 0)
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    State &st = StateFor(script);
    ++st.trigger03_plays;
    REXLOG_INFO("pdz_fix_lab_tannoy_repeat: TANNOY_TRIGGER03 scheduled ({} of {})", st.trigger03_plays, kMaxTrigger03);
  }
}

namespace lab_restorations
{
  uint32_t CurrentScript()
  {
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    return now - g_tick_ms.load(std::memory_order_relaxed) < 500 ? g_tick_script.load(std::memory_order_relaxed) : 0;
  }

  bool Changed(const Message &m, unsigned flag)
  {
    const unsigned word = flag / 32;
    const uint64_t regs[5] = {m.r5, m.r6, m.r7, m.r8, m.r9};
    if (word >= 10)
    {
      return false;
    }
    const uint64_t bit = (word % 2 == 0) ? (uint64_t(1) << (32 + flag % 32)) : (uint64_t(1) << (flag % 32));
    return (regs[word / 2] & bit) != 0;
  }

  bool InDarkenedBox(const float pos[3])
  {
    for (const Box &b : kBoxes)
    {
      if (InBox(b, pos))
      {
        return true;
      }
    }
    return false;
  }

  void BeforeLabMessage(PPCContext &ctx, uint8_t *base, uint32_t script, const Message &m)
  {
    (void)ctx;
    if (!REXCVAR_GET(pdz_lab_power_return) || !Changed(m, kFlagLights))
    {
      return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    State &st = StateFor(script);
    if (!st.have_ambient)
    {
      std::memcpy(st.ambient, base + kAmbient, kAmbientSize); // before the handler blacks it out
      st.have_ambient = true;
    }
    st.lights.clear();
    g_capture_lights.store(true, std::memory_order_relaxed);
  }

  int BlackoutCoverage()
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.coverage ? g_state.coverage : std::clamp<int>(REXCVAR_GET(pdz_lab_blackout_coverage), 1, 3);
  }

  void AfterLabMessage(PPCContext &ctx, uint8_t *base, uint32_t script, const Message &m)
  {
    g_capture_lights.store(false, std::memory_order_relaxed);
    // (e) in the frame of Rare's lights branch / darkness loop, right after them.
    if (Changed(m, kFlagLights) || Changed(m, kFlagDarkness))
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      State &st = StateFor(script);
      const bool first = !st.dark;
      st.dark = true;
      const int level = CoverageLevel(st);
      if (Changed(m, kFlagLights))
      {
        if (level > 1 && !st.relit)
        {
          SwitchOffExtraLights(ctx, base, st, level);
        }
        else if (level > 1)
        {
          REXLOG_INFO("pdz_lab_blackout_coverage: lights message after the power return: the extra lights are left alone");
        }
        if (REXCVAR_GET(pdz_test_lab_light_census))
        {
          LightCensus(base, level > 1 ? "blackout (Rare's and the extra lights off)" : "blackout (Rare's lights off)");
        }
      }
      if (Changed(m, kFlagDarkness) && level >= 3)
      {
        std::string names;
        const uint32_t n = DarkenExtraCharacters(ctx, base, st, "blackout", names);
        REXLOG_INFO("pdz_lab_blackout_coverage: level 3: {} extra character(s) darkened with Rare's: {}{}", n,
                    names.empty() ? "none" : names,
                    first ? " (dormant prison guards follow when the receiver wakes them)" : "");
      }
    }
    if (REXCVAR_GET(pdz_lab_power_return))
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      State &st = StateFor(script);
      if (Changed(m, kFlagLights))
      {
        REXLOG_INFO("pdz_lab_power_return: blackout: {} lights switched off by Rare's Lights_Toggle noted", st.lights.size());
      }
      if (Changed(m, kFlagSwitch) && !st.armed)
      {
        if (FlagSet(base, script, kFlagLockbot) || FlagSet(base, script, kFlagExplosives))
        {
          REXLOG_INFO("pdz_lab_power_return: switch used after the {}: the power stays off",
                      FlagSet(base, script, kFlagExplosives) ? "explosives" : "lockbot");
        }
        else
        {
          st.armed = true;
          st.arm_clock = Clock(base);
          REXLOG_INFO("pdz_lab_power_return: switch route (flag 37): power back in {} s of game time",
                      REXCVAR_GET(pdz_lab_power_return_s));
        }
      }
    }
    if (REXCVAR_GET(pdz_lab_cut_lines))
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      CutLinesOnMessage(ctx, base, StateFor(script), m);
    }
  }
} // namespace lab_restorations
