// Laboratory | Rescue: Rare's remaining cut content (pdz_lab_* switches, all
// off by default), tu3-base, TU3 addresses. Research:
// the Laboratory research report (4.3, 5, 8), its audio / text notes,
// its follow-up notes (Q1, Q4: the conversation task
// and the animation survey). Companion of
// lab_restorations.cpp (pdz_lab_cut_lines and friends); deliberately a
// separate file with its own copies of the small helpers, so the two can be
// merged without conflicts. Every piece uses Rare's own recorded lines or
// Rare's own animation; where the moment had to be ours it says so.
//
// Hooks (none of them hooked elsewhere; all call Rare's code unchanged):
// - sub_821E1B48: the Jo-AI conversation task's update (vtable 82093470 +0x1C;
//   r3 task, f1 dt). Task +0x10 character, +0x38 state byte, +0x4C "target
//   holds a weapon", +0x40 float timer. State 1 plays the list's gun warning
//   (SCIENTIST_14 in the Laboratory's three lists), state 4 waits for it,
//   state 2 counts +0x40 up while Jo stays armed and fails silently at 5.0 s
//   (821E204C-821E207C, constant 82089540), state 3 = fail.
// - sub_82319B18: the Laboratory scientist task's update (vtable 820A3190
//   +0x1C; r3 task, +0x10 character, +0x38 state word, +0x4C cowering, +0x4E,
//   +0x6C type: 0 = s2s1 (SCIENTIST_30 mutters), 1 = s2s2 (SCIENTIST_29), 2 =
//   s4l4 / s4l5 (SCIENTIST_16); set at 8231C098 / 8231C19C / 8231C4DC). Its
//   idle mutter (8231A078-8231A164): every 5.0 s (double stamp +0x78 against
//   the AI clock 82E2F334, constant 82089540) a 1-in-5 roll (sub_827025F8(0,5)
//   == 0) plays the type's cue through the speech task when nothing else is
//   being said (char +0x327C == 0) and he is not cowering. This hook is also
//   the module's once-per-frame point in the Laboratory (the level has
//   scientists from start to end).
// - sub_821DF070: the alarm task's update (class 8209339C, ctor sub_821DEE58;
//   +0x10 character, +0xC result). Its start (821DF110) asks for the Run
//   animation through sub_821F0080([[char+0x30F0]+0x38], 0x6D) and sends the
//   guard to the nearest alarm; the update sets the result to 2 when he is
//   within reach of it (821DF0D0-821D0DC). That is the only "reached the
//   alarm" moment in the task.
//
// Pieces:
// (a) pdz_lab_gun_warning2: SCIENTIST_15 ("You heard me. Put your gun away." /
//     "What's the matter with you? Lose the gun." / "C'mon, lose the gun. I
//     don't wanna have to call security."), the recorded second warning that
//     no list has a slot for. Played on the scientist in state 2 of his
//     conversation once its armed timer passes 2.0 s (SCIENTIST_14 has ended;
//     the silent failure comes at 5.0 s, the longest take is 2.7 s).
// (b) pdz_lab_injured_warning: CHAN_35 ("You injured a scientist! Watch your
//     aim! Kill him and I'm scratching the mission." and three more), the
//     middle tier between CHAN_34 (flag 81, "you hit a scientist", latched
//     once by case 35 at 82322980) and CHAN_36 (flag 82, case 36, mission
//     failed). Rare left the tier without a moment; ours: after CHAN_34 has
//     played, the first time a scientist falls below half his health or a
//     second hit lands on a scientist (health drops read from the task's
//     update), once per run, not after flag 82.
// (c) pdz_lab_glyph_talk: the glyph team's recorded exchange SCIENTIST_31-36
//     (sci2 opens: "OK, so we have samples of the glyphs from all over the
//     world..."; sci1 answers; three rounds, ordered by the subtitle table
//     and the voice banks: 31/33/35 sci2, 32/34/36 sci1). Spoken by the two
//     hub scientists whose solo mutters are the only glyph lines that play:
//     s2s2 (type 1, SCIENTIST_29) opens, s2s1 (type 0, SCIENTIST_30) answers.
//     Moment: one of Rare's own 5 s mutter moments, with Rare's 1-in-5 odds,
//     while both are calm (state 0, not cowering, not alerted, nothing being
//     said) and within 15 m (their desks are 10.9 m apart), once per run. The
//     chosen moment's solo roll is skipped (the stamp is written before Rare's
//     update), and both stamps are kept fresh while the exchange runs, so no
//     solo mutter cuts across it.
// (d) pdz_lab_more_hints: Chandra's level hints 05 / 06 / 11 / 12, each once,
//     on Rare's own gadget-hint moments: the level messages 201 (Locktopus)
//     and 202 (demo kit) that volumes send every 30 s (cases 0xC9 / 0xCA at
//     82326034 / 82325FD0 toggle flags 175 / 176; the handler then plays the
//     generic CHAN_01_GEN_*_HINT). 4.5 s later: in the generator room box
//     (Rare's light-group 2 volume) with the generator objective (0) active,
//     175 -> HINT_05 (over-ride switch) and 176 -> HINT_06 (explosives);
//     elsewhere with the gas objective (1) active, 175 -> HINT_12 (the lockbot
//     closes the valves) and 176 -> HINT_11 (destroy the vent fans). The
//     pairing of hint to room and objective is ours; the moments are Rare's.
//     Skipped: 01 (mailbots: no model, setup or code), 03 (CHAN_29 says it,
//     and its only moment, the gold refusal, already prompts hint 02 in
//     pdz_lab_cut_lines), 04 (CHAN_33 says it), 13 (same content as hint 10,
//     which pdz_lab_cut_lines plays after CHAN_16).
// (e) pdz_lab_alarm_anim: Rare's PressTheAlarmButton (AnimType 0x91, the male
//     animation others_buttonpress_lfarm_front_high, in the set and asked for
//     by nothing) requested through Rare's wrapper sub_821F0080(owner, 0x91),
//     exactly as the alarm task asks for Run, when the alarm task's update
//     reports the guard within reach of the alarm (result +0xC -> 2). Nothing
//     in the task is delayed or changed. Female bodies (securityforcegirl) have
//     no entry of that type, so the wrapper plays Idle for them (821F01A0).
// (f) pdz_lab_emergency_flicker: Rare's unused light animation modes (prop
//     messages 112-115 in sub_825F3B68 set light +0x324 mode 0-3, +0x35C
//     frequency, +0x360 phase; the anim update sub_826060B0 blends colour to
//     colour 2, black on every Laboratory light, by the phase and multiplies
//     by the intensity, so only a lit light shows anything; nothing in the
//     game sends 112-115 and no Laboratory light has anim data). 1 s after
//     the blackout (flag 38) the two point lights of the main-hall light box
//     nearest the hub's middle level (the glyph team's desks) are switched
//     back on (sub_82602648, the setter Rare's toggle would reach without
//     LS7) with mode 3 Random (phase ping-pongs 0..1 at a random speed, up
//     to kFlickerStep per frame): two emergency lights flickering in the
//     dark hall. When the power returns (flag 40, pdz_lab_power_return) they
//     go back to mode 0. Which lights, their mode and speed are ours
//     (REPORT 8 D lists this as new design); the mechanism is Rare's.
//
// pdz_test_lab_cut (Testing, int): 1 SCIENTIST_15 on the scientist nearest Jo,
// 2 CHAN_35, 3 the glyph exchange now (both hub scientists alive), 4
// CHAN_HINT_05, 5 the alarm-button animation on the scientist nearest Jo (a
// male body, so the animation itself can be seen), 6 the emergency flicker
// now (no blackout needed: the two lights are set to Random where they are).
// Set it back to 0 before the next command.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <random>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include "lab_restorations.h"
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

#include "guest_function.h" // our indirect calls: nullptr for no function (log 101)

REXCVAR_DEFINE_BOOL(pdz_lab_gun_warning2, false, "Gameplay",
                    "Laboratory: the hub scientist's recorded second gun warning (LV05_SPEECH_SCIENTIST_15 \"You heard "
                    "me. Put your gun away.\"), which no conversation list has a slot for, 2 s into the armed wait "
                    "that follows his first warning (SCIENTIST_14) and ends in the silent failure at 5 s; off = "
                    "Rare's one warning")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_lab_injured_warning, false, "Gameplay",
                    "Laboratory: Chandra's recorded middle warning (LV05_SPEECH_CHAN_35 \"You injured a scientist! "
                    "Watch your aim! Kill him and I'm scratching the mission.\") between \"you hit a scientist\" "
                    "(CHAN_34) and the mission failure (CHAN_36): once per run, after CHAN_34, when a scientist "
                    "falls below half health or a second hit lands on a scientist (our moment; Rare left none)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_lab_glyph_talk, false, "Gameplay",
                    "Laboratory: the two hub scientists' recorded exchange about the glyphs (LV05_SPEECH_SCIENTIST_31 "
                    "to 36, six lines), which only their solo mutters (SCIENTIST_29 / 30) ever hint at: once per "
                    "run, at one of Rare's own mutter moments with Rare's odds, while both are calm and within 15 m")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_lab_more_hints, false, "Gameplay",
                    "Laboratory: Chandra's recorded level hints 05 / 06 (generator: over-ride switch, explosives) and "
                    "11 / 12 (gas: vent fans, valves), each once, 4.5 s after Rare's generic Locktopus / demo-kit hint "
                    "(the volume messages 201 / 202), picked by where Jo is (the generator room or not) and which "
                    "objective is active; hints 01, 03, 04 and 13 stay unwired (see lab_cut_content.cpp)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_lab_alarm_anim, false, "Gameplay",
                    "Laboratory: a guard who reaches an alarm plays Rare's unused PressTheAlarmButton animation "
                    "(AnimType 0x91, others_buttonpress_lfarm_front_high) through Rare's own request path when the "
                    "alarm task reports him within reach; the alarm itself is untouched (female guards have no such "
                    "animation and idle for the moment instead)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_lab_emergency_flicker, false, "Gameplay",
                    "Laboratory: 1 s after the blackout the two main-hall point lights nearest the hub scientists' "
                    "desks come back on and flicker with Rare's unused Random light animation (mode 3; the game never "
                    "sends it); back to normal when the power returns (pdz_lab_power_return). Which lights and how fast "
                    "are our choices")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(pdz_test_lab_cut, 0, "Testing",
                     "TESTING TOOL (Laboratory cut content): changing it runs one command once: 1 = SCIENTIST_15 on "
                     "the scientist nearest Jo, 2 = Chandra CHAN_35, 3 = the glyph exchange now (both hub scientists "
                     "alive), 4 = Chandra CHAN_HINT_05, 5 = the alarm-button animation on the scientist nearest Jo, "
                     "6 = the emergency flicker now (no blackout needed). Set it back to 0 before the next command")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(sub_82202D20); // cue name -> cue reference (ref, name)
REX_EXTERN(sub_822014B8); // queue a 2D line (Chandra): (ref, f1, f2, r6, r7, r8)
REX_EXTERN(sub_821FF3C8); // play a cue on a prop / character: (ref, pos or 0, r5, r6 attach, r7)
REX_EXTERN(sub_821F0080); // request an animation by type: (owner, AnimType, 0)
REX_EXTERN(sub_82602648); // light state: (light, state 0 off / 1 on, f1 fade s)
REX_EXTERN(__imp__sub_821E1B48); // the Jo-AI conversation task's update (r3 task, f1 dt)
REX_EXTERN(__imp__sub_82319B18); // the Laboratory scientist task's update (r3 task, f1 dt)
REX_EXTERN(__imp__sub_821DF070); // the alarm task's update (r3 task, f1 dt)

namespace
{
  // --- guest memory helpers (as lab_restorations.cpp) ---
  uint32_t Load32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
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

  void Store32(uint8_t *base, uint32_t address, uint32_t value)
  {
    uint8_t *p = base + address;
    p[0] = uint8_t(value >> 24);
    p[1] = uint8_t(value >> 16);
    p[2] = uint8_t(value >> 8);
    p[3] = uint8_t(value);
  }

  void StoreFloat(uint8_t *base, uint32_t address, float value)
  {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    Store32(base, address, bits);
  }

  void StoreDouble(uint8_t *base, uint32_t address, double value)
  {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    uint8_t *p = base + address;
    for (int i = 0; i < 8; ++i)
    {
      p[i] = uint8_t(bits >> (56 - 8 * i));
    }
  }

  bool IsGuestPointer(uint32_t address)
  {
    return address >= 0x00010000u && address < 0xFFFF0000u;
  }

  // --- the game ---
  constexpr uint32_t kGameState = 0x82D6BFD8;       // -> game info; +4 difficulty, +0x1C -> the level
  constexpr uint32_t kLevelScript = 0x2D8;          // level -> the message target L; the script is [L+0x74] (8214E7B8)
  constexpr uint32_t kLabScriptVtable = 0x820A30EC; // the Laboratory script's vtable
  constexpr uint32_t kAiClock = 0x82E2F334;         // float seconds, per level (the scripts' "now")
  constexpr uint32_t kPlayerOne = 0x82E3C3E4;       // player 1's character
  constexpr uint32_t kCharacterPosition = 48;       // vec3, cm
  constexpr uint32_t kCharacterAlertFlags = 0x186C; // bit 0x10 alerted
  constexpr uint32_t kCharacterSpeaking = 0x327C;   // the speech component's current line (0 = none)
  constexpr uint32_t kCharacterAnimHolder = 0x30F0; // -> holder; +0x38 the animation owner (821DF13C-821DF14C)
  constexpr uint32_t kHealthSlot = 332;             // character vtable: health (f1)
  constexpr uint32_t kScriptFlags = 0x3EC;          // script -> the level's flag words
  constexpr uint32_t kScriptObjectives = 0x3F0;     // script -> objectives, 0x34 bytes each, +0 state (1 active, 2 complete)
  constexpr uint32_t kRefSize = 1024;               // cue reference
  constexpr uint32_t kNameSize = 128;
  // the conversation task (82093470)
  constexpr uint32_t kConvCharacter = 0x10;
  constexpr uint32_t kConvState = 0x38;  // byte
  constexpr uint32_t kConvTimer = 0x40;  // float: the armed wait (state 2), 0 .. 5.0
  constexpr uint32_t kConvArmed = 0x4C;  // byte: the target holds a weapon
  constexpr float kGunWarning2At = 2.0f; // s into the armed wait (SCIENTIST_15's longest take is 2.7 s; the fail is at 5.0)
  // the scientist task (820A3190)
  constexpr uint32_t kSciCharacter = 0x10;
  constexpr uint32_t kSciState = 0x38;    // word: 0 = working
  constexpr uint32_t kSciCowering = 0x4C; // byte
  constexpr uint32_t kSciBusy = 0x4E;     // byte, tested before the mutters (82319FB0)
  constexpr uint32_t kSciType = 0x6C;     // word: 0 s2s1 (sci1 lines), 1 s2s2 (sci2 lines), 2 computer room
  constexpr uint32_t kSciMutterStamp = 0x78; // double: the AI clock at the last mutter moment
  constexpr float kMutterPeriodS = 5.0f;     // 82089540
  constexpr int kMutterOdds = 5;             // sub_827025F8(0, 5) == 0
  constexpr float kGlyphRange2 = 1500.0f * 1500.0f; // 15 m (desks 10.9 m apart)
  // the alarm task (8209339C)
  constexpr uint32_t kAlarmCharacter = 0x10;
  constexpr uint32_t kAlarmResult = 0xC; // 2 = within reach of the alarm
  constexpr uint32_t kAnimPressTheAlarmButton = 0x91;
  // flags
  constexpr unsigned kFlagScientistHit = 81;    // CHAN_34, latched by case 35
  constexpr unsigned kFlagScientistKilled = 82; // CHAN_36 + mission failed, case 36
  constexpr unsigned kFlagHintLocktopus = 175;  // toggled by level message 201 (case 0xC9)
  constexpr unsigned kFlagHintDemoKit = 176;    // toggled by level message 202 (case 0xCA)
  constexpr float kInjuredAfterHitS = 5.5f;     // CHAN_35 waits for CHAN_34 (longest take 3.9 s) to end
  constexpr float kHintAfterGenericS = 4.5f;    // after the generic gadget hint (takes 3-4 s)

  // The generator room box (Rare's light-group 2 obbvolume; research 3 2b,
  // lab_restorations.cpp kBoxes[1]).
  constexpr float kGeneratorCentre[3] = {1333.8f, -942.8f, 3586.1f};
  constexpr float kGeneratorHalf = 2000.0f;
  constexpr float kGeneratorYawDeg = 37.0f;
  // The main-hall box (light group 1; no yaw) and the lights (research 3 1a,
  // 2a, 4).
  constexpr float kHallCentre[3] = {248.1f, 31.3f, -1801.8f};
  constexpr float kHallHalf[3] = {1750.0f, 1500.0f, 4000.0f};
  constexpr float kHubDesks[3] = {-140.0f, 292.0f, -1228.0f}; // between s2s1 (-248, 300, -1761) and s2s2 (-32, 285, -694)
  constexpr uint32_t kLightList = 0x82E19AE0;      // array of light pointers (the iterator sub_82601F58)
  constexpr uint32_t kLightCount = 0x82E3C5F4;     // their count (limit 2000)
  constexpr uint32_t kLightStateBase = 0x82E1BA20; // [this] + light + 0x5AC = the on/off byte (sub_82602608)
  constexpr uint32_t kLightType = 0x2A8;           // 0 spot, 1 directional, 2 point
  constexpr uint32_t kLightPosition = 0x2D0;
  constexpr uint32_t kLightAnimMode = 0x324;       // 0 normal, 1 OnOff, 2 Undulate, 3 Random (messages 112-115)
  constexpr uint32_t kLightAnimFrequency = 0x35C;  // Random: the phase step per frame is rand() * this (82606100-8260610C)
  constexpr uint32_t kLightAnimPhase = 0x360;      // 0..1, colour -> colour 2 (black)
  constexpr uint32_t kLightAnimRandom = 3;
  constexpr float kFlickerStep = 0.15f;            // ours: a full swing in about 13 frames on average (value 15 in Rare's message units)
  constexpr float kFlickerFadeS = 0.5f;            // the switch-back-on fade (ours)
  constexpr float kFlickerAfterBlackoutS = 1.0f;   // after flag 38: Rare's Lights_Toggle has run by then
  constexpr unsigned kFlagLights = 38;             // lights out (the blackout)
  constexpr unsigned kFlagPowerBack = 40;          // set by pdz_lab_power_return when the power returns
  constexpr size_t kFlickerLights = 2;

  std::mutex g_mutex;
  uint32_t g_ref = 0; // guest: cue reference + name text
  std::mt19937 g_rng{0x6C7962u};

  float Clock(const uint8_t *base)
  {
    return LoadFloat(base, kAiClock);
  }

  // The Laboratory script, or 0 in any other level.
  uint32_t LabScript(const uint8_t *base)
  {
    // The Laboratory script as Rare's own setup / tick hand it to lab_restorations (r3); 0 outside the Laboratory.
    // (The earlier chain [[gs+0x1C]+0x2D8](+0x74) found nothing in the first scripted run.)
    const uint32_t script = lab_restorations::CurrentScript();
    return IsGuestPointer(script) && Load32(base, script) == kLabScriptVtable ? script : 0;
  }

  bool FlagSet(const uint8_t *base, uint32_t script, unsigned flag)
  {
    const uint32_t words = Load32(base, script + kScriptFlags);
    return IsGuestPointer(words) && (Load32(base, words + 4 * (flag / 32)) & (1u << (flag % 32))) != 0;
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

  float Health(PPCContext &ctx, uint8_t *base, uint32_t character)
  {
    if (!IsCharacter(base, character))
    {
      return 0.0f;
    }
    PPCFunc *fn = pdz::GuestFunction(Load32(base, Load32(base, character) + kHealthSlot), "lab_cut_content health");
    if (!fn)
    {
      return 0.0f;
    }
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = character;
    frame.ctx.r4.u64 = 0;
    fn(frame.ctx, base);
    return float(frame.ctx.f1.f64);
  }

  bool Alive(PPCContext &ctx, uint8_t *base, uint32_t character)
  {
    return Health(ctx, base, character) > 0.0f;
  }

  void PositionOf(const uint8_t *base, uint32_t character, float out[3])
  {
    for (int i = 0; i < 3; ++i)
    {
      out[i] = LoadFloat(base, character + kCharacterPosition + 4 * i);
    }
  }

  float Distance2(const uint8_t *base, uint32_t a, uint32_t b)
  {
    float pa[3], pb[3];
    PositionOf(base, a, pa);
    PositionOf(base, b, pb);
    const float d[3] = {pa[0] - pb[0], pa[1] - pb[1], pa[2] - pb[2]};
    return d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
  }

  bool InGeneratorRoom(const float pos[3])
  {
    const float d[3] = {pos[0] - kGeneratorCentre[0], pos[1] - kGeneratorCentre[1], pos[2] - kGeneratorCentre[2]};
    const float t = kGeneratorYawDeg * 3.14159265358979f / 180.0f;
    const float c = std::cos(t);
    const float s = std::sin(t);
    const float local[3] = {d[0] * c - d[2] * s, d[1], d[0] * s + d[2] * c};
    return std::fabs(local[0]) < kGeneratorHalf && std::fabs(local[1]) < kGeneratorHalf && std::fabs(local[2]) < kGeneratorHalf;
  }

  // A cue reference for `name` in our guest buffer (sub_82202D20 fills it), or 0.
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

  // A Chandra line, queued as the handler queues hers (sub_822014B8 with f1
  // 6.0, f2 0.0, r6 1, r7 0, r8 0; it copies the reference).
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

  // A cue as a positioned sound attached to a character (sub_821FF3C8).
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
    frame.ctx.r6.u64 = character;
    frame.ctx.r7.u64 = 1;
    frame.ctx.f1.f64 = 0.0;
    sub_821FF3C8(frame.ctx, base);
  }

  // An animation by type on a character, as the alarm task's start asks for
  // Run (821DF13C-821DF14C: owner = [[char+0x30F0]+0x38], r5 = 0).
  bool PlayAnimType(PPCContext &ctx, uint8_t *base, uint32_t character, uint32_t type)
  {
    if (!IsCharacter(base, character))
    {
      return false;
    }
    const uint32_t holder = Load32(base, character + kCharacterAnimHolder);
    if (!IsGuestPointer(holder))
    {
      return false;
    }
    const uint32_t owner = Load32(base, holder + 0x38);
    if (!IsGuestPointer(owner))
    {
      return false;
    }
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = owner;
    frame.ctx.r4.u64 = type;
    frame.ctx.r5.u64 = 0;
    sub_821F0080(frame.ctx, base);
    return true;
  }

  // --- per-run state ---
  struct Pending
  {
    float due = 0.0f;
    uint32_t character = 0; // 0: Chandra
    const char *cue = nullptr;
    const char *tag = "";  // log prefix
    const char *what = "";
  };

  struct Scientist
  {
    uint32_t task = 0;
    uint32_t type = 0;
    float last_seen = 0.0f;
    bool calm = false;
    float max_health = 0.0f;
    float last_health = 0.0f;
    bool have_health = false;
    uint32_t hits = 0;
  };

  struct Talk
  {
    bool running = false;
    uint32_t a = 0; // sci2 lines (31 / 33 / 35)
    uint32_t b = 0; // sci1 lines (32 / 34 / 36)
    uint32_t task_a = 0, task_b = 0;
    int step = 0;
    float next = 0.0f;
  };

  struct State
  {
    uint32_t script = 0;
    float last_clock = -1.0f;
    float frame_clock = -1.0f;
    std::vector<Pending> pending;
    std::unordered_map<uint32_t, Scientist> scientists; // by character
    // (a)
    uint32_t warning2_task = 0; // the conversation task already warned twice
    // (b)
    bool injured_done = false;
    bool saw_flag81 = false;
    float flag81_clock = -1.0f;
    uint32_t hit_events = 0;
    // (c)
    Talk talk;
    bool talk_done = false;
    int talk_attempts = 0;
    // (d)
    bool have_hint_words = false;
    bool hint_locktopus_last = false, hint_demokit_last = false;
    bool hint05_done = false, hint06_done = false, hint11_done = false, hint12_done = false;
    // (e)
    uint32_t alarm_anims = 0;
    // (f)
    bool flicker_armed = false; // flag 38 seen
    float flicker_due = 0.0f;
    bool flicker_done = false;
    bool flicker_reset = false;
    std::vector<uint32_t> flicker_lights;
    // test tool
    int32_t test_last = 0;
  };
  State g_state;

  // The state for this run: a new script pointer or a clock that went back
  // (a restart) starts over.
  State &StateFor(const uint8_t *base, uint32_t script)
  {
    const float now = Clock(base);
    if (g_state.script != script || now + 1.0f < g_state.last_clock)
    {
      g_state = State{};
      g_state.script = script;
    }
    g_state.last_clock = now;
    return g_state;
  }

  void Queue(State &st, const uint8_t *base, float delay_s, uint32_t character, const char *cue, const char *tag,
             const char *what)
  {
    st.pending.push_back(Pending{Clock(base) + delay_s, character, cue, tag, what});
  }

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
        REXLOG_INFO("{}: {}: Chandra {}", line.tag, line.what, line.cue + 15);
      }
      else if (Alive(ctx, base, line.character))
      {
        SayOn(ctx, base, line.character, line.cue);
        REXLOG_INFO("{}: {}: {} on {:08X}", line.tag, line.what, line.cue + 15, line.character);
      }
      else
      {
        REXLOG_INFO("{}: {}: speaker {:08X} is dead, {} dropped", line.tag, line.what, line.character, line.cue + 15);
      }
    }
  }

  // The living scientist nearest Jo (of those the task hook has seen), or 0.
  uint32_t NearestScientist(PPCContext &ctx, uint8_t *base, State &st)
  {
    const uint32_t jo = Load32(base, kPlayerOne);
    if (!IsCharacter(base, jo))
    {
      return 0;
    }
    uint32_t best = 0;
    float best_d2 = 0.0f;
    for (const auto &entry : st.scientists)
    {
      if (!IsCharacter(base, entry.first) || !Alive(ctx, base, entry.first))
      {
        continue;
      }
      const float d2 = Distance2(base, entry.first, jo);
      if (!best || d2 < best_d2)
      {
        best = entry.first;
        best_d2 = d2;
      }
    }
    return best;
  }

  // --- (c) the glyph exchange ---
  struct Line
  {
    bool opener; // true: a (sci2), false: b (sci1)
    const char *cue;
    float gap_s; // to the next line (the longest take + 0.6 s)
  };
  constexpr Line kGlyphLines[] = {
      {true, "XACT_SOUNDBANK_LV05_SPEECH_SCIENTIST_31", 9.4f},  // "OK, so we have samples of the glyphs..." (8.8 s)
      {false, "XACT_SOUNDBANK_LV05_SPEECH_SCIENTIST_32", 5.6f}, // "Some civilization..." / "...I'm with you so far." (5.0 s)
      {true, "XACT_SOUNDBANK_LV05_SPEECH_SCIENTIST_33", 6.2f},  // "...must form some sort of alphabet." (5.6 s)
      {false, "XACT_SOUNDBANK_LV05_SPEECH_SCIENTIST_34", 4.3f}, // "It's like no code I've seen before." (3.7 s)
      {true, "XACT_SOUNDBANK_LV05_SPEECH_SCIENTIST_35", 3.3f},  // "This whole thing's like one giant jigsaw." (2.7 s)
      {false, "XACT_SOUNDBANK_LV05_SPEECH_SCIENTIST_36", 3.8f}, // "I always preferred monopoly..." (3.8 s)
  };

  bool SciCalm(const uint8_t *base, uint32_t task, uint32_t character)
  {
    return Load32(base, task + kSciState) == 0 && base[task + kSciCowering] == 0 && base[task + kSciBusy] == 0 &&
           (Load32(base, character + kCharacterAlertFlags) & 0x10) == 0 && Load32(base, character + kCharacterSpeaking) == 0;
  }

  void StartTalk(uint8_t *base, State &st, uint32_t a, uint32_t task_a, uint32_t b, uint32_t task_b, const char *why)
  {
    st.talk = Talk{};
    st.talk.running = true;
    st.talk.a = a;
    st.talk.b = b;
    st.talk.task_a = task_a;
    st.talk.task_b = task_b;
    st.talk.step = 0;
    st.talk.next = Clock(base) + 0.3f;
    ++st.talk_attempts;
    REXLOG_INFO("pdz_lab_glyph_talk: {}: the glyph exchange starts: s2s2 {:08X} (SCIENTIST_31 / 33 / 35) and s2s1 {:08X} "
                "(SCIENTIST_32 / 34 / 36), {:.1f} m apart",
                why, a, b, std::sqrt(Distance2(base, a, b)) / 100.0f);
  }

  void AbortTalk(State &st, const char *why)
  {
    REXLOG_INFO("pdz_lab_glyph_talk: exchange stopped after line {} of 6: {}{}", st.talk.step, why,
                st.talk_attempts >= 2 ? " (no further attempt this run)" : " (may start again later)");
    st.talk.running = false;
    if (st.talk_attempts >= 2)
    {
      st.talk_done = true;
    }
  }

  // Before Rare's scientist update: Rare's mutter moment for this scientist,
  // our 1-in-5 roll, and the partner check. A hit skips Rare's own roll for
  // this moment by stamping the task first.
  void GlyphBefore(PPCContext &ctx, uint8_t *base, State &st, uint32_t task, uint32_t character)
  {
    const double now = double(Clock(base));
    if (st.talk.running)
    {
      // keep both solo mutters away while the exchange runs
      if (task == st.talk.task_a || task == st.talk.task_b)
      {
        StoreDouble(base, task + kSciMutterStamp, now);
      }
      return;
    }
    if (st.talk_done)
    {
      return;
    }
    const uint32_t type = Load32(base, task + kSciType);
    if (type > 1 || !SciCalm(base, task, character))
    {
      return;
    }
    if (float(now - LoadDouble(base, task + kSciMutterStamp)) <= kMutterPeriodS)
    {
      return; // not one of Rare's moments
    }
    // the partner: the other glyph type, seen this second, calm, near
    uint32_t partner = 0, partner_task = 0;
    for (const auto &entry : st.scientists)
    {
      const Scientist &s = entry.second;
      if (entry.first == character || s.type != (1u - type) || !s.calm || Clock(base) - s.last_seen > 1.0f)
      {
        continue;
      }
      if (!IsCharacter(base, entry.first) || Distance2(base, entry.first, character) > kGlyphRange2)
      {
        continue;
      }
      partner = entry.first;
      partner_task = s.task;
    }
    if (!partner)
    {
      return;
    }
    if (std::uniform_int_distribution<int>(0, kMutterOdds - 1)(g_rng) != 0)
    {
      return; // Rare rolls his solo mutter as usual
    }
    if (!Alive(ctx, base, character) || !Alive(ctx, base, partner))
    {
      return;
    }
    StoreDouble(base, task + kSciMutterStamp, now);
    StoreDouble(base, partner_task + kSciMutterStamp, now);
    if (type == 1)
    {
      StartTalk(base, st, character, task, partner, partner_task, "mutter moment");
    }
    else
    {
      StartTalk(base, st, partner, partner_task, character, task, "mutter moment");
    }
  }

  void GlyphTick(PPCContext &ctx, uint8_t *base, State &st)
  {
    if (!st.talk.running)
    {
      return;
    }
    const float now = Clock(base);
    if (now < st.talk.next)
    {
      return;
    }
    if (st.talk.step >= int(sizeof(kGlyphLines) / sizeof(kGlyphLines[0])))
    {
      st.talk.running = false;
      st.talk_done = true;
      REXLOG_INFO("pdz_lab_glyph_talk: the glyph exchange is over (6 lines)");
      return;
    }
    if (!IsCharacter(base, st.talk.a) || !IsCharacter(base, st.talk.b) || !Alive(ctx, base, st.talk.a) ||
        !Alive(ctx, base, st.talk.b))
    {
      AbortTalk(st, "a speaker is gone");
      return;
    }
    if (!SciCalm(base, st.talk.task_a, st.talk.a) || !SciCalm(base, st.talk.task_b, st.talk.b))
    {
      // +0x327C is set while our own positioned line plays? It is the speech
      // component's own line, which we do not use, so a non-zero value here
      // means Rare's speech (a reaction) started: stop.
      AbortTalk(st, "a speaker is no longer calm");
      return;
    }
    const Line &line = kGlyphLines[st.talk.step];
    const uint32_t speaker = line.opener ? st.talk.a : st.talk.b;
    SayOn(ctx, base, speaker, line.cue);
    REXLOG_INFO("pdz_lab_glyph_talk: line {} of 6: {} on {:08X} ({})", st.talk.step + 1, line.cue + 15, speaker,
                line.opener ? "s2s2" : "s2s1");
    st.talk.next = now + line.gap_s;
    ++st.talk.step;
  }

  void ForceTalk(PPCContext &ctx, uint8_t *base, State &st)
  {
    uint32_t a = 0, task_a = 0, b = 0, task_b = 0;
    for (const auto &entry : st.scientists)
    {
      if (!IsCharacter(base, entry.first) || !Alive(ctx, base, entry.first))
      {
        continue;
      }
      if (entry.second.type == 1)
      {
        a = entry.first;
        task_a = entry.second.task;
      }
      else if (entry.second.type == 0)
      {
        b = entry.first;
        task_b = entry.second.task;
      }
    }
    if (!a || !b)
    {
      REXLOG_INFO("pdz_test_lab_cut 3: both hub scientists are needed alive (type 1 {:08X}, type 0 {:08X}): nothing played", a, b);
      return;
    }
    st.talk_attempts = 0;
    st.talk_done = false;
    StartTalk(base, st, a, task_a, b, task_b, "pdz_test_lab_cut 3");
  }

  // --- (b) the injured warning ---
  void InjuredCheck(PPCContext &ctx, uint8_t *base, State &st, uint32_t character, Scientist &s, float health)
  {
    (void)ctx;
    if (!s.have_health)
    {
      s.have_health = true;
      s.max_health = health;
      s.last_health = health;
      return;
    }
    if (health > s.max_health)
    {
      s.max_health = health; // state 7 raises the hub scientist's health to a tuning value
    }
    bool hit = false;
    if (health < s.last_health - 0.01f)
    {
      hit = true;
      ++s.hits;
      ++st.hit_events;
    }
    s.last_health = health;
    if (st.injured_done || health <= 0.0f)
    {
      return;
    }
    const bool below_half = health < 0.5f * s.max_health;
    if (!hit && !below_half)
    {
      return;
    }
    if (!FlagSet(base, st.script, kFlagScientistHit) || FlagSet(base, st.script, kFlagScientistKilled))
    {
      return; // CHAN_34 not yet (the first tier), or already the last tier
    }
    if (!below_half && st.hit_events < 2)
    {
      return;
    }
    st.injured_done = true;
    const float since34 = st.flag81_clock >= 0.0f ? Clock(base) - st.flag81_clock : kInjuredAfterHitS;
    const float delay = std::max(0.5f, kInjuredAfterHitS - since34);
    Queue(st, base, delay, 0, "XACT_SOUNDBANK_LV05_SPEECH_CHAN_35", "pdz_lab_injured_warning", "scientist injured (tier 2)");
    REXLOG_INFO("pdz_lab_injured_warning: scientist {:08X} {} (health {:.1f} of {:.1f}, {} hit(s) on him, {} on scientists; "
                "CHAN_34 {:.1f} s ago): CHAN_35 queued in {:.1f} s",
                character, below_half ? "below half health" : "hit again", health, s.max_health, s.hits, st.hit_events,
                since34, delay);
  }

  // --- (d) the hints on Rare's gadget-hint moments ---
  void HintsTick(PPCContext &ctx, uint8_t *base, State &st)
  {
    (void)ctx;
    const bool locktopus = FlagSet(base, st.script, kFlagHintLocktopus);
    const bool demokit = FlagSet(base, st.script, kFlagHintDemoKit);
    if (!st.have_hint_words)
    {
      st.have_hint_words = true;
      st.hint_locktopus_last = locktopus;
      st.hint_demokit_last = demokit;
      return;
    }
    const bool locktopus_now = locktopus != st.hint_locktopus_last;
    const bool demokit_now = demokit != st.hint_demokit_last;
    st.hint_locktopus_last = locktopus;
    st.hint_demokit_last = demokit;
    if (!locktopus_now && !demokit_now)
    {
      return;
    }
    const char *gadget = locktopus_now ? "Locktopus (flag 175)" : "demo kit (flag 176)";
    const uint32_t jo = Load32(base, kPlayerOne);
    float pos[3] = {0.0f, 0.0f, 0.0f};
    if (IsCharacter(base, jo))
    {
      PositionOf(base, jo, pos);
    }
    const bool generator_room = InGeneratorRoom(pos);
    const uint32_t generator = ObjectiveState(base, st.script, 0);
    const uint32_t gas = ObjectiveState(base, st.script, 1);
    const char *cue = nullptr;
    const char *what = nullptr;
    bool *done = nullptr;
    if (generator_room && generator == 1)
    {
      if (locktopus_now)
      {
        cue = "XACT_SOUNDBANK_LV05_SPEECH_CHAN_HINT_05";
        what = "generator room, Locktopus hint (hint 05: the over-ride switch)";
        done = &st.hint05_done;
      }
      else
      {
        cue = "XACT_SOUNDBANK_LV05_SPEECH_CHAN_HINT_06";
        what = "generator room, demo-kit hint (hint 06: explosives)";
        done = &st.hint06_done;
      }
    }
    else if (!generator_room && gas == 1)
    {
      if (locktopus_now)
      {
        cue = "XACT_SOUNDBANK_LV05_SPEECH_CHAN_HINT_12";
        what = "gas objective, Locktopus hint (hint 12: the lockbot closes the valves)";
        done = &st.hint12_done;
      }
      else
      {
        cue = "XACT_SOUNDBANK_LV05_SPEECH_CHAN_HINT_11";
        what = "gas objective, demo-kit hint (hint 11: destroy the vent fans)";
        done = &st.hint11_done;
      }
    }
    if (!cue)
    {
      REXLOG_INFO("pdz_lab_more_hints: generic {} hint at ({:.0f} {:.0f} {:.0f}), {}in the generator room, generator objective "
                  "state {}, gas objective state {}: no level hint fits",
                  gadget, pos[0], pos[1], pos[2], generator_room ? "" : "not ", generator, gas);
      return;
    }
    if (*done)
    {
      return;
    }
    *done = true;
    Queue(st, base, kHintAfterGenericS, 0, cue, "pdz_lab_more_hints", what);
    REXLOG_INFO("pdz_lab_more_hints: generic {} hint, {}: {} queued in {:.1f} s", gadget, what, cue + 15, kHintAfterGenericS);
  }

  // --- (f) the emergency flicker ---
  bool InMainHall(const float pos[3])
  {
    return std::fabs(pos[0] - kHallCentre[0]) < kHallHalf[0] && std::fabs(pos[1] - kHallCentre[1]) < kHallHalf[1] &&
           std::fabs(pos[2] - kHallCentre[2]) < kHallHalf[2];
  }

  bool LightOn(const uint8_t *base, uint32_t light)
  {
    const uint32_t offset = Load32(base, kLightStateBase);
    return base[light + offset + 0x5AC] != 0;
  }

  // The kFlickerLights point lights of the main hall nearest the hub desks:
  // switched on and set to Random.
  void StartFlicker(PPCContext &ctx, uint8_t *base, State &st, const char *why)
  {
    const uint32_t count = Load32(base, kLightCount);
    struct Candidate
    {
      uint32_t light;
      float d2;
      float pos[3];
    };
    std::vector<Candidate> found;
    for (uint32_t i = 0; i < count && i < 2000; ++i)
    {
      const uint32_t light = Load32(base, kLightList + 4 * i);
      if (!IsGuestPointer(light) || Load32(base, light + kLightType) != 2)
      {
        continue;
      }
      Candidate c;
      c.light = light;
      for (int k = 0; k < 3; ++k)
      {
        c.pos[k] = LoadFloat(base, light + kLightPosition + 4 * k);
      }
      if (!InMainHall(c.pos))
      {
        continue;
      }
      const float d[3] = {c.pos[0] - kHubDesks[0], c.pos[1] - kHubDesks[1], c.pos[2] - kHubDesks[2]};
      c.d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
      found.push_back(c);
    }
    std::sort(found.begin(), found.end(), [](const Candidate &a, const Candidate &b) { return a.d2 < b.d2; });
    st.flicker_done = true;
    st.flicker_lights.clear();
    if (found.empty())
    {
      REXLOG_INFO("pdz_lab_emergency_flicker: {}: no point light of {} lies in the main-hall box: nothing to flicker", why, count);
      return;
    }
    for (size_t i = 0; i < found.size() && i < kFlickerLights; ++i)
    {
      const Candidate &c = found[i];
      const bool was_on = LightOn(base, c.light);
      {
        rex::CallFrame frame(ctx);
        frame.ctx.r3.u64 = c.light;
        frame.ctx.r4.u64 = 1;
        frame.ctx.f1.f64 = double(kFlickerFadeS);
        sub_82602648(frame.ctx, base);
      }
      Store32(base, c.light + kLightAnimMode, kLightAnimRandom);
      StoreFloat(base, c.light + kLightAnimFrequency, kFlickerStep);
      StoreFloat(base, c.light + kLightAnimPhase, 0.0f);
      st.flicker_lights.push_back(c.light);
      REXLOG_INFO("pdz_lab_emergency_flicker: {}: light {:08X} at ({:.0f} {:.0f} {:.0f}), {:.1f} m from the hub desks, was {}: "
                  "switched on over {:.1f} s, mode 3 Random, step {:.2f}",
                  why, c.light, c.pos[0], c.pos[1], c.pos[2], std::sqrt(c.d2) / 100.0f, was_on ? "on" : "off", kFlickerFadeS,
                  kFlickerStep);
    }
  }

  void StopFlicker(uint8_t *base, State &st, const char *why)
  {
    st.flicker_reset = true;
    for (const uint32_t light : st.flicker_lights)
    {
      if (IsGuestPointer(light))
      {
        Store32(base, light + kLightAnimMode, 0);
        StoreFloat(base, light + kLightAnimPhase, 0.0f);
      }
    }
    REXLOG_INFO("pdz_lab_emergency_flicker: {}: {} light(s) back to mode 0", why, st.flicker_lights.size());
  }

  void FlickerTick(PPCContext &ctx, uint8_t *base, State &st)
  {
    const float now = Clock(base);
    if (!st.flicker_armed && !st.flicker_done && FlagSet(base, st.script, kFlagLights))
    {
      st.flicker_armed = true;
      st.flicker_due = now + kFlickerAfterBlackoutS;
      REXLOG_INFO("pdz_lab_emergency_flicker: blackout (flag 38): the emergency lights come on in {:.1f} s", kFlickerAfterBlackoutS);
    }
    if (st.flicker_armed && !st.flicker_done && now >= st.flicker_due)
    {
      StartFlicker(ctx, base, st, "blackout");
    }
    if (st.flicker_done && !st.flicker_reset && !st.flicker_lights.empty() && FlagSet(base, st.script, kFlagPowerBack))
    {
      StopFlicker(base, st, "power back (flag 40)");
    }
  }

  // --- the test tool ---
  void TestTool(PPCContext &ctx, uint8_t *base, State &st)
  {
    const int32_t value = REXCVAR_GET(pdz_test_lab_cut);
    if (value == st.test_last)
    {
      return;
    }
    st.test_last = value;
    if (value == 0)
    {
      return;
    }
    const uint32_t nearest = NearestScientist(ctx, base, st);
    switch (value)
    {
    case 1:
      if (nearest)
      {
        SayOn(ctx, base, nearest, "XACT_SOUNDBANK_LV05_SPEECH_SCIENTIST_15");
      }
      REXLOG_INFO("pdz_test_lab_cut 1: SCIENTIST_15 on the scientist nearest Jo ({:08X})", nearest);
      break;
    case 2:
      SayChandra(ctx, base, "XACT_SOUNDBANK_LV05_SPEECH_CHAN_35");
      REXLOG_INFO("pdz_test_lab_cut 2: Chandra CHAN_35");
      break;
    case 3:
      ForceTalk(ctx, base, st);
      break;
    case 4:
      SayChandra(ctx, base, "XACT_SOUNDBANK_LV05_SPEECH_CHAN_HINT_05");
      REXLOG_INFO("pdz_test_lab_cut 4: Chandra CHAN_HINT_05");
      break;
    case 5:
      REXLOG_INFO("pdz_test_lab_cut 5: PressTheAlarmButton (0x91) on the scientist nearest Jo ({:08X}): {}", nearest,
                  nearest && PlayAnimType(ctx, base, nearest, kAnimPressTheAlarmButton) ? "requested" : "no animation owner");
      break;
    case 6:
      st.flicker_reset = false;
      StartFlicker(ctx, base, st, "pdz_test_lab_cut 6");
      break;
    default:
      REXLOG_INFO("pdz_test_lab_cut {}: unknown command (1-6)", value);
      break;
    }
  }

  // Once per frame (the first scientist task update of the frame).
  void FrameTick(PPCContext &ctx, uint8_t *base, State &st)
  {
    if (REXCVAR_GET(pdz_lab_injured_warning) && !st.saw_flag81 && FlagSet(base, st.script, kFlagScientistHit))
    {
      st.saw_flag81 = true;
      st.flag81_clock = st.frame_clock >= 0.0f ? Clock(base) : -1.0f; // -1: set before we looked (long ago)
      REXLOG_INFO("pdz_lab_injured_warning: CHAN_34 played (flag 81): the injured tier is armed");
    }
    if (REXCVAR_GET(pdz_lab_glyph_talk))
    {
      GlyphTick(ctx, base, st);
    }
    if (REXCVAR_GET(pdz_lab_more_hints))
    {
      HintsTick(ctx, base, st);
    }
    if (REXCVAR_GET(pdz_lab_emergency_flicker))
    {
      FlickerTick(ctx, base, st);
    }
    TestTool(ctx, base, st);
    PlayDue(ctx, base, st);
  }
} // namespace

// sub_821E1B48: the Jo-AI conversation task's update (r3 = the task). After
// Rare's code: in state 2 (the armed wait after the first gun warning) past
// kGunWarning2At, the recorded second warning, once per conversation.
REX_HOOK_RAW(sub_821E1B48)
{
  const uint32_t task = ctx.r3.u32;
  __imp__sub_821E1B48(ctx, base);
  if (!REXCVAR_GET(pdz_lab_gun_warning2) || !IsGuestPointer(task))
  {
    return;
  }
  const uint32_t script = LabScript(base);
  if (!script)
  {
    return; // another level's conversation (the Mansion's radio guards)
  }
  if (base[task + kConvState] != 2 || base[task + kConvArmed] == 0 || LoadFloat(base, task + kConvTimer) < kGunWarning2At)
  {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  State &st = StateFor(base, script);
  if (st.warning2_task == task)
  {
    return;
  }
  st.warning2_task = task;
  const uint32_t character = Load32(base, task + kConvCharacter);
  if (!IsCharacter(base, character))
  {
    return;
  }
  SayOn(ctx, base, character, "XACT_SOUNDBANK_LV05_SPEECH_SCIENTIST_15");
  REXLOG_INFO("pdz_lab_gun_warning2: conversation task {:08X}, Jo still armed {:.1f} s after SCIENTIST_14 (fail at 5.0 s): "
              "SCIENTIST_15 on {:08X}",
              task, LoadFloat(base, task + kConvTimer), character);
}

// sub_82319B18: the Laboratory scientist task's update (r3 = the task, +0x10
// the scientist). Before Rare's code: the glyph exchange's moment (c). After:
// the scientist register, his health (b), and the module's frame tick.
REX_HOOK_RAW(sub_82319B18)
{
  const uint32_t task = ctx.r3.u32;
  const bool glyph = REXCVAR_GET(pdz_lab_glyph_talk);
  const bool injured = REXCVAR_GET(pdz_lab_injured_warning);
  const bool hints = REXCVAR_GET(pdz_lab_more_hints);
  const bool test = REXCVAR_GET(pdz_test_lab_cut) != 0;
  const bool any = glyph || injured || hints || test || REXCVAR_GET(pdz_lab_gun_warning2) ||
                   REXCVAR_GET(pdz_lab_alarm_anim) || REXCVAR_GET(pdz_lab_emergency_flicker);
  if (!any || !IsGuestPointer(task))
  {
    __imp__sub_82319B18(ctx, base);
    return;
  }
  const uint32_t script = LabScript(base);
  const uint32_t character = Load32(base, task + kSciCharacter);
  if (!script || !IsCharacter(base, character))
  {
    __imp__sub_82319B18(ctx, base);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    State &st = StateFor(base, script);
    if (glyph)
    {
      GlyphBefore(ctx, base, st, task, character);
    }
  }
  __imp__sub_82319B18(ctx, base);
  std::lock_guard<std::mutex> lock(g_mutex);
  State &st = StateFor(base, script);
  Scientist &s = st.scientists[character];
  s.task = task;
  s.type = Load32(base, task + kSciType);
  s.last_seen = Clock(base);
  s.calm = SciCalm(base, task, character);
  if (injured)
  {
    InjuredCheck(ctx, base, st, character, s, Health(ctx, base, character));
  }
  const float now = Clock(base);
  if (now != st.frame_clock)
  {
    FrameTick(ctx, base, st);
    st.frame_clock = now;
  }
}

// sub_821DF070: the alarm task's update (r3 = the task, +0x10 the guard).
// After Rare's code: the first frame the result says "within reach of the
// alarm" (2), Rare's PressTheAlarmButton animation by type.
REX_HOOK_RAW(sub_821DF070)
{
  const uint32_t task = ctx.r3.u32;
  const bool watch = REXCVAR_GET(pdz_lab_alarm_anim) && IsGuestPointer(task);
  const uint32_t before = watch ? Load32(base, task + kAlarmResult) : 0;
  __imp__sub_821DF070(ctx, base);
  if (!watch || before == 2 || Load32(base, task + kAlarmResult) != 2)
  {
    return;
  }
  const uint32_t script = LabScript(base);
  if (!script)
  {
    return; // Laboratory only (the switch's name); the request would work in any level
  }
  const uint32_t character = Load32(base, task + kAlarmCharacter);
  const bool requested = PlayAnimType(ctx, base, character, kAnimPressTheAlarmButton);
  std::lock_guard<std::mutex> lock(g_mutex);
  State &st = StateFor(base, script);
  ++st.alarm_anims;
  REXLOG_INFO("pdz_lab_alarm_anim: guard {:08X} within reach of the alarm (alarm task {:08X} result 2): PressTheAlarmButton "
              "(0x91) {} (no. {} this run)",
              character, task, requested ? "requested through sub_821F0080" : "not requested (no animation owner)",
              st.alarm_anims);
}
