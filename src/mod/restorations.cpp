// Restorations (docs/TU3_BASE.md "Restoration build plan"): content Rare cut
// or never wired up, each behind its own switch, off by default. tu3-base
// only; TU3 addresses. Launch and TU3 behave the same here (TU3_BASE "TU3
// audio regressions"), so these are restorations, not fixes.
//
// - pdz_restore_mansion_door_greeting (R1): the main-door guard's
//   LV4_LET_JO_IN line. The speech builder (sub_82204718) turns event 169
//   into LV04_SPEECH_GUARD_N ("Enjoy the party." / "Miss." / "The
//   entertainment's about to begin."), but no code raises 169 (BUG_AUDIT
//   AI53). The Mansion script (sub_823326B0, the door state machine) lets Jo
//   in at 82332990 by setting bit 0x1000 in its door state word
//   ([script +1004] +12): at once on Agent and Secret Agent, after a
//   successful voice check on Perfect and Dark Agent. When that bit comes on
//   during a call, the main-door guard (script slot +1376 'end1', or +1384
//   'end2', the hard guard) says 169 through the voice (sub_8262AA40).
// - pdz_restore_light_speech (R3): the staff's lights-out lines in the
//   Laboratory blackout. Speech only: the darkness mechanic (sight range,
//   aim, search task; SP_INVENTORY "Darkness in the Laboratory blackout") is
//   Rare's and is left alone. The Laboratory handler (sub_823272E0) puts
//   the lights out when bit 0x40 of its first message mask (r5) is set
//   (8232A264): Lights_Toggle on groups 1-4, which always switches lights
//   off, and a black ambient colour. Nothing turns them back on
//   (SP_INVENTORY "Is the blackout ever reversed?"), so a repeat of the
//   message is ignored. Every voice recorded LIGHT_SHOT_OUT (147), and the
//   scientists recorded lights_out_next_solo / _question / _answer, but
//   nothing plays them (SP_INVENTORY, "A cut stealth layer?"). When the
//   lights go out (staff = the Laboratory script's living characters within
//   30 m of Jo):
//   - the nearest scientist says lights_out_next_solo, or, with a second
//     scientist within 10 m of him, one asks (_question) and the other
//     answers (_answer) 2.5 s later. These have no speech event, so they
//     play as positioned sounds on the speaker (sub_821FF3C8, as the level
//     scripts play sounds on characters);
//   - up to three of the other staff nearest Jo say 147.
#include <algorithm>
#include <atomic>
#include <chrono>
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
#include <rex/ui/keybinds.h>

#include "../game_cvars.h"
#include "restorations.h"
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

REX_EXTERN(__imp__sub_823326B0);
REX_EXTERN(__imp__sub_823272E0);
REX_EXTERN(sub_8262AA40);
REX_EXTERN(sub_82202D20);
REX_EXTERN(sub_821FF3C8);

namespace
{
  constexpr uint32_t kGameState = 0x82D6BFD8;   // -> game info; +4 = difficulty (0 Agent .. 3 Dark Agent)
  constexpr uint32_t kDoorState = 1004;         // script -> door state object
  constexpr uint32_t kDoorFlags = 12;           // word in it
  constexpr uint32_t kLetJoIn = 0x1000;         // set at 82332990
  constexpr uint32_t kGuardEnd1 = 1376;         // script slot 'end1'
  constexpr uint32_t kGuardEnd2 = 1384;         // script slot 'end2' (Perfect / Dark guard)
  constexpr uint32_t kVoice = 12688;            // character -> voice / speech component
  constexpr uint32_t kLetJoInEvent = 169;       // LV4_LET_JO_IN

  uint32_t Load32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  bool IsGuestPointer(uint32_t address)
  {
    return address >= 0x00010000u && address < 0xFFFF0000u;
  }

  bool LetJoIn(const uint8_t *base, uint32_t script)
  {
    const uint32_t door = Load32(base, script + kDoorState);
    return IsGuestPointer(door) && (Load32(base, door + kDoorFlags) & kLetJoIn) != 0;
  }
} // namespace

// sub_823326B0: the Mansion script's message handler (r3 = the script).
REX_HOOK_RAW(sub_823326B0)
{
  const uint32_t script = ctx.r3.u32;
  const bool watch = REXCVAR_GET(pdz_restore_mansion_door_greeting) && IsGuestPointer(script);
  const bool before = watch && LetJoIn(base, script);
  __imp__sub_823326B0(ctx, base);
  if (!watch || before || !LetJoIn(base, script))
  {
    return;
  }
  const uint32_t info = Load32(base, kGameState);
  const uint32_t difficulty = IsGuestPointer(info) ? Load32(base, info + 4) : 0;
  uint32_t guard = Load32(base, script + (difficulty >= 2 ? kGuardEnd2 : kGuardEnd1));
  if (!IsGuestPointer(guard))
  {
    guard = Load32(base, script + (difficulty >= 2 ? kGuardEnd1 : kGuardEnd2));
  }
  if (!IsGuestPointer(guard))
  {
    REXLOG_INFO("pdz_restore: Mansion door greeting: no main-door guard in the script slots");
    return;
  }
  // say(voice, event, subject female = Jo, 1, delay 0), as the level scripts call it.
  rex::CallFrame frame(ctx);
  frame.ctx.r3.u64 = guard + kVoice;
  frame.ctx.r4.u64 = kLetJoInEvent;
  frame.ctx.r5.u64 = 1;
  frame.ctx.r6.u64 = 1;
  frame.ctx.f1.f64 = 0.0;
  sub_8262AA40(frame.ctx, base);
  REXLOG_INFO("pdz_restore: Mansion door greeting (LV4_LET_JO_IN) from guard {:08X}", guard);
}

// --- pdz_restore_light_speech (R3) ---
namespace
{
  using Clock = std::chrono::steady_clock;

  constexpr uint64_t kLightsMessage = 0x40;    // r5 bit: lights out, groups 1-4 (8232A264)
  constexpr uint32_t kSlotFirst = 1148;        // Laboratory script character slots: s1m1 ...
  constexpr uint32_t kSlotLast = 1700;         // ... s12b (zgli and the cutscene actors follow)
  constexpr uint32_t kSlotStep = 8;
  constexpr uint32_t kCharVoiceIndex = 12704;  // 15 SCI1, 16 SCI2
  constexpr uint32_t kCharPosition = 48;       // vec3, cm
  constexpr uint32_t kHealthSlot = 332;        // character vtable: health (f1)
  constexpr uint32_t kParticipantList = 0x82E34224; // -> first participant (+4 next)
  constexpr uint32_t kParticipantKind = 944;   // 2 player, 3 bot
  constexpr uint32_t kParticipantCharacter = 964; // participant -> its character
  constexpr uint32_t kLightShotOut = 147;      // LIGHT_SHOT_OUT
  constexpr float kHearing2 = 3000.0f * 3000.0f; // staff within 30 m of Jo
  constexpr float kPair2 = 1000.0f * 1000.0f;    // two scientists within 10 m make a pair
  constexpr int kFirstSpeakers = 3;
  constexpr auto kAnswerDelay = std::chrono::milliseconds(2500);
  constexpr uint32_t kRefSize = 1024;          // cue reference (the handler's are 544 bytes apart)
  constexpr uint32_t kNameSize = 128;

  std::mutex g_light_mutex;
  uint32_t g_light_script = 0;
  bool g_dark = false;
  uint32_t g_ref = 0;
  struct PendingLine
  {
    uint32_t character = 0;
    std::string cue;
    Clock::time_point when{};
  };
  PendingLine g_pending;

  float LoadFloat(const uint8_t *base, uint32_t address)
  {
    const uint32_t bits = Load32(base, address);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
  }

  // A character's position (+48, cm; as cut_scenarios.cpp / anim_viewer.cpp
  // read it in TU3).
  bool PositionOf(PPCContext &ctx, uint8_t *base, uint32_t character, float out[3])
  {
    (void)ctx;
    if (!IsGuestPointer(character))
    {
      return false;
    }
    for (int i = 0; i < 3; ++i)
    {
      out[i] = LoadFloat(base, character + kCharPosition + 4 * i);
    }
    return true;
  }

  bool Alive(PPCContext &ctx, uint8_t *base, uint32_t character)
  {
    const uint32_t vtable = Load32(base, character);
    if (!IsGuestPointer(vtable))
    {
      return false;
    }
    PPCFunc *fn = pdz::GuestFunction(Load32(base, vtable + kHealthSlot));
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

  // Jo's character: the first human participant's (+964).
  uint32_t Player(const uint8_t *base)
  {
    uint32_t p = Load32(base, kParticipantList);
    for (int guard = 0; IsGuestPointer(p) && guard < 64; ++guard, p = Load32(base, p + 4))
    {
      if (Load32(base, p + kParticipantKind) == 2)
      {
        const uint32_t character = Load32(base, p + kParticipantCharacter);
        return IsGuestPointer(character) ? character : 0;
      }
    }
    return 0;
  }

  struct Staff
  {
    uint32_t character;
    float pos[3];
    float d2; // to Jo
    bool scientist;
  };

  float Distance2(const float a[3], const float b[3])
  {
    float d2 = 0.0f;
    for (int i = 0; i < 3; ++i)
    {
      d2 += (a[i] - b[i]) * (a[i] - b[i]);
    }
    return d2;
  }

  // The living staff within earshot of Jo, nearest first.
  std::vector<Staff> NearbyStaff(PPCContext &ctx, uint8_t *base, uint32_t script)
  {
    std::vector<Staff> out;
    float jo[3] = {};
    const uint32_t player = Player(base);
    const bool have_jo = player && PositionOf(ctx, base, player, jo);
    for (uint32_t slot = kSlotFirst; slot <= kSlotLast; slot += kSlotStep)
    {
      const uint32_t c = Load32(base, script + slot);
      if (!IsGuestPointer(c) || c == player || !Alive(ctx, base, c))
      {
        continue;
      }
      Staff s{c, {}, 0.0f, false};
      if (!PositionOf(ctx, base, c, s.pos))
      {
        continue;
      }
      s.d2 = have_jo ? Distance2(s.pos, jo) : 0.0f;
      if (have_jo && s.d2 > kHearing2)
      {
        continue;
      }
      const uint32_t voice = Load32(base, c + kCharVoiceIndex);
      s.scientist = voice == 15 || voice == 16;
      out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const Staff &a, const Staff &b) { return a.d2 < b.d2; });
    return out;
  }

  void Say(PPCContext &ctx, uint8_t *base, uint32_t character, uint32_t event)
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = character + kVoice;
    frame.ctx.r4.u64 = event;
    frame.ctx.r5.u64 = 1; // no _MALE take: the plain line
    frame.ctx.r6.u64 = 0;
    frame.ctx.f1.f64 = 0.0;
    sub_8262AA40(frame.ctx, base);
  }

  // XACT_SOUNDBANK_<SCI1|SCI2>_<line> as a positioned sound on the speaker.
  void PlayOn(PPCContext &ctx, uint8_t *base, uint32_t character, const std::string &line)
  {
    const uint32_t voice = Load32(base, character + kCharVoiceIndex);
    const std::string name = std::string("XACT_SOUNDBANK_") + (voice == 16 ? "SCI2_" : "SCI1_") + line;
    if (name.size() + 1 > kNameSize)
    {
      return;
    }
    if (!g_ref)
    {
      g_ref = REX_KERNEL_MEMORY()->SystemHeapAlloc(kRefSize + kNameSize);
      if (!g_ref)
      {
        return;
      }
    }
    const uint32_t text = g_ref + kRefSize;
    std::memcpy(base + text, name.c_str(), name.size() + 1);
    {
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = g_ref;
      frame.ctx.r4.u64 = text;
      sub_82202D20(frame.ctx, base);
    }
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = g_ref;
    frame.ctx.r4.u64 = 0;
    frame.ctx.r6.u64 = character; // attached to the speaker
    frame.ctx.r7.u64 = 1;
    frame.ctx.f1.f64 = 0.0;
    sub_821FF3C8(frame.ctx, base);
    REXLOG_INFO("pdz_restore: lights out: {} ({:08X})", name.substr(15), character);
  }

  void LightsOut(PPCContext &ctx, uint8_t *base, uint32_t script)
  {
    const std::vector<Staff> staff = NearbyStaff(ctx, base, script);
    const Staff *a = nullptr;
    const Staff *b = nullptr;
    for (const Staff &s : staff)
    {
      if (!s.scientist)
      {
        continue;
      }
      if (!a)
      {
        a = &s;
      }
      else if (Distance2(a->pos, s.pos) <= kPair2)
      {
        b = &s;
        break;
      }
    }
    if (b)
    {
      PlayOn(ctx, base, a->character, "LIGHTS_OUT_NEXT_QUESTION");
      g_pending = PendingLine{b->character, "LIGHTS_OUT_NEXT_ANSWER", Clock::now() + kAnswerDelay};
    }
    else if (a)
    {
      PlayOn(ctx, base, a->character, "LIGHTS_OUT_NEXT_SOLO");
    }
    size_t n = 0;
    for (const Staff &s : staff)
    {
      if (n >= size_t(kFirstSpeakers))
      {
        break;
      }
      if ((a && s.character == a->character) || (b && s.character == b->character))
      {
        continue;
      }
      Say(ctx, base, s.character, kLightShotOut);
      ++n;
    }
    REXLOG_INFO("pdz_restore: lights out: {} scientist line(s), LIGHT_SHOT_OUT from {} of {} staff within 30 m",
                b ? 2 : (a ? 1 : 0), n, staff.size());
  }
} // namespace

namespace restorations
{
  void Tick(PPCContext &ctx, uint8_t *base)
  {
    if (!REXCVAR_GET(pdz_restore_light_speech))
    {
      return;
    }
    std::lock_guard<std::mutex> lock(g_light_mutex);
    if (!g_pending.character || Clock::now() < g_pending.when)
    {
      return;
    }
    const PendingLine line = g_pending;
    g_pending = PendingLine{};
    if (Alive(ctx, base, line.character))
    {
      PlayOn(ctx, base, line.character, line.cue);
    }
  }
} // namespace restorations

// --- pdz_test_lab_blackout_key (testing tool) ---
// The Home key sends the generator blackout to the Laboratory script as the
// generator switch did in log 114: in one message the lights (r5 bit 0x40,
// 8232A264), Chandra's CHAN_06 with the mast sound (r7 high word bit 0x8,
// 823280F0) and the darkness (r7 high word bit 0x10, 823282D4); then
// TANNOY_TRIGGER02 (r6 bit 0x2) 10 s later and TANNOY_TRIGGER03 (r6 bit 0x4)
// 52 s later. The generator alarm (r7 high word bit 0x4: CHAN_04 and
// TANNOY_TRIGGER01) wasn't heard in log 114 and isn't sent. The bits are
// merged into the next call of Rare's handler (only the Laboratory script has
// it, so the key does nothing elsewhere); with the switch on, every message
// the handler gets is logged, so a real switch use can be compared.
namespace
{
  struct TimedBits
  {
    Clock::time_point when;
    uint64_t r5 = 0, r6 = 0, r7 = 0;
    const char *what = "";
  };
  std::atomic<bool> g_blackout_key{false};
  std::mutex g_key_mutex;
  std::vector<TimedBits> g_key_bits;
} // namespace

namespace restorations
{
  void RegisterBinds()
  {
    rex::ui::RegisterBind("bind_test_lab_blackout", "Home",
                          "Testing tool: Laboratory generator blackout (pdz_test_lab_blackout_key)", []
                          {
      static Clock::time_point last_event{};
      const auto now = Clock::now();
      const bool fresh = now - last_event > std::chrono::milliseconds(300);
      last_event = now;
      if (fresh && REXCVAR_GET(pdz_test_lab_blackout_key))
      {
        g_blackout_key.store(true);
      } });
  }
} // namespace restorations

// sub_823272E0: the Laboratory script's message handler (r3 = the script;
// r5-r9 = 64-bit message masks).
REX_HOOK_RAW(sub_823272E0)
{
  if (REXCVAR_GET(pdz_test_lab_blackout_key))
  {
    std::lock_guard<std::mutex> lock(g_key_mutex);
    const auto now = Clock::now();
    if (g_blackout_key.exchange(false))
    {
      g_key_bits.clear();
      g_key_bits.push_back({now, 0x40, 0, uint64_t(0x8 | 0x10) << 32, "lights out, CHAN_06, darkness"});
      g_key_bits.push_back({now + std::chrono::seconds(10), 0, 0x2, 0, "TANNOY_TRIGGER02"});
      g_key_bits.push_back({now + std::chrono::seconds(52), 0, 0x4, 0, "TANNOY_TRIGGER03"});
      REXLOG_INFO("pdz_test: blackout key pressed in Laboratory (script {:08X})", ctx.r3.u32);
    }
    for (auto it = g_key_bits.begin(); it != g_key_bits.end();)
    {
      if (now < it->when)
      {
        ++it;
        continue;
      }
      ctx.r5.u64 |= it->r5;
      ctx.r6.u64 |= it->r6;
      ctx.r7.u64 |= it->r7;
      REXLOG_INFO("pdz_test: blackout key: sent {} (r5 {:X}, r6 {:X}, r7 {:X})", it->what, it->r5, it->r6, it->r7);
      it = g_key_bits.erase(it);
    }
    if (ctx.r5.u64 | ctx.r6.u64 | ctx.r7.u64 | ctx.r8.u64 | ctx.r9.u64)
    {
      REXLOG_INFO("pdz_test: Laboratory message: r5 {:016X} r6 {:016X} r7 {:016X} r8 {:016X} r9 {:016X}", ctx.r5.u64,
                  ctx.r6.u64, ctx.r7.u64, ctx.r8.u64, ctx.r9.u64);
    }
  }
  const uint32_t script = ctx.r3.u32;
  const bool lights = (ctx.r5.u64 & kLightsMessage) != 0;
  __imp__sub_823272E0(ctx, base);
  if (!lights || !REXCVAR_GET(pdz_restore_light_speech) || !IsGuestPointer(script))
  {
    return;
  }
  std::lock_guard<std::mutex> lock(g_light_mutex);
  if (script != g_light_script)
  {
    g_light_script = script; // a new run of the level
    g_dark = false;
    g_pending = PendingLine{};
  }
  if (g_dark)
  {
    REXLOG_INFO("pdz_restore: lights message again, already dark");
    return;
  }
  g_dark = true;
  LightsOut(ctx, base, script);
}
