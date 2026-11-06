// Rare's Title Update 3 bug fixes ported to the launch exe (pdz_fix_tu_*),
// part of the game fixes group: each applies only while pdz_game_fixes is on
// (bind_game_fixes_ab, F11). See README.md next to this file and
// docs/BUG_AUDIT.md ("Title Update 3 changes", U-table) for the evidence.
// The title update's data (Township textures, hit-fix props: U12 / U18) and
// the Facility / Ruin rules (U22) live in dlc_maps.cpp with the rest of the
// title update plumbing. TU3 addresses are T, launch addresses plain.
//
// Most of these are checks TU3 added in the middle of a function. Those are
// mid-function hooks ([[midasm_hook]] in midasm_hooks.toml, next to the
// manifest): the recompiler calls the function below just before the named
// instruction and, when it returns true, jumps where TU3's new branch goes
// (the matching launch address). With the fix off they return false and the
// launch code runs unchanged.
//
// - pdz_fix_tu_datacore_null (U7, T 822FFDC8): the Datacore level script
//   (sub_822F8000) reads its tracked character (+2592) and then that
//   object's +92 with no null check (822F89A0, 822F8D64), a crash once the
//   character is gone. TU3 skips both blocks when +2592 is 0 (to 822F8CDC /
//   822F8F98, T 82300ABC / 82300D8C).
// - pdz_fix_tu_bot_task_null (U8, T 821C1A88 / 821C1D78 / 821C67A0 /
//   821C5160): the multiplayer bot scripts (Deathmatch, Team Deathmatch,
//   flag, King of the Hill, Protection; tick sub_821BCB80) use the mode
//   object at task +76 unchecked. TU3 leaves each of the four functions when
//   it is 0 (its DarkOps bots have none). In launch it is always set where
//   bots exist, so this never fires; a pure guard.
// - pdz_fix_tu_base_slot_null (U9, T 8217B2A0): sub_821766E8 looks an object
//   up in the 4-slot table of a map's bases (82D3D118) and reads each slot's
//   record without checking for an empty slot (821766FC), a crash on a map
//   with fewer than 4 bases. TU3 skips empty slots.
// - pdz_fix_tu_lv10_health (U11, T 8230E5B4): the LV10 (jungle) level script
//   (sub_82303A20) gives the character in its list entry 13 15000 health
//   (sub_825C5A78 at 823067B8: health and max health); TU3 also gives entry 14
//   the same, between entry 13 and entry 49 (500). The hook loads entry 14
//   and 15000 and runs the existing call again.
// - pdz_fix_tu_flag_drop (U13 + U17, T 82578F00 / 8241B048 / 8244C308): when a
//   capture-the-flag flag weapon is destroyed (sub_82569708) it tells the
//   flag mode the flag was dropped (vtable +16: sub_8240FFA0 / sub_82440190)
//   without saying who held it (r5 is left over). The mode then resets the
//   carrier's flag to "dropped" even when the weapon being stripped belonged
//   to someone who isn't the carrier (sub_82411468 strips it from any
//   non-carrier holder). TU3 passes the holder's player record and ignores
//   the drop unless it is the carrier (carrier id: record +932, + 4 x +940
//   for teams 2 / 3; T +944 / +952). Launch has no holder field, so the
//   holder is taken from the strip (sub_8253CB78: character, weapon) and the
//   check runs only for the destructor's call (lr 82569760).
// - pdz_fix_tu_teleport (U16, T 82436938): the multiplayer teleport
//   (sub_8242AB48: teleporter pads, the out-of-bounds return) moves the
//   character but leaves it in its old cell of the world's spatial lists and
//   leaves attached objects (its child list +88, next +96) behind. TU3 then
//   removes and re-inserts it (sub_82508F78 / sub_82508EA8) and moves each
//   child by the character's move (old and new world matrix, vtable +16;
//   child vtable +68). Done around the move calls (lr 8242ADC8 / 8242ADD8 /
//   8242ADE8).
// - pdz_fix_tu_pack_markers (U21, T 82435750): the out-of-bounds HUD markers
//   (sub_82429978's 8 x 16 marker loop) are skipped on Facility (map id 33)
//   and Ruin (35), the Map Pack Two maps (our pack ids are TU3's).
#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include "guest_function.h" // our indirect calls: nullptr for no function (log 101)
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/ppc/stack.h>
#include <rex/runtime.h>

#include "../game_cvars.h"

REX_EXTERN(__imp__sub_8253CB78);
REX_EXTERN(__imp__sub_8240FFA0);
REX_EXTERN(__imp__sub_82440190);
REX_EXTERN(__imp__sub_8259D940);
REX_EXTERN(__imp__sub_8259DBA8);
REX_EXTERN(__imp__sub_8252A8E0);
REX_EXTERN(sub_82508F78);
REX_EXTERN(sub_82508EA8);

namespace
{
  uint8_t *Base()
  {
    auto *rt = rex::Runtime::instance();
    return rt ? rt->virtual_membase() : nullptr;
  }

  uint32_t LoadBe32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  // Guest heap objects can sit below 0x40000000 (the player does).
  bool IsGuestPointer(uint32_t address)
  {
    return address >= 0x00010000u && address < 0xFFFF0000u;
  }

  bool Fix(bool option)
  {
    return REXCVAR_GET(pdz_game_fixes) && option;
  }

  // Logs the first time a fix changes what the game does (then every 100th).
  void Note(const char *what, uint32_t &count)
  {
    if (count++ % 100 == 0)
    {
      REXLOG_INFO("pdz_fix_tu: {} ({} time(s))", what, count);
    }
  }

  // object->vtable[slot](object, r4, r5); returns r3.
  uint32_t CallVirtual(PPCContext &ctx, uint8_t *base, uint32_t object, uint32_t slot, uint32_t r4 = 0,
                       uint32_t r5 = 0)
  {
    PPCFunc *fn = pdz::GuestFunction(LoadBe32(base, LoadBe32(base, object) + slot));
    if (!fn)
    {
      return 0;
    }
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = object;
    frame.ctx.r4.u64 = r4;
    frame.ctx.r5.u64 = r5;
    fn(frame.ctx, base);
    return frame.ctx.r3.u32;
  }

  void CallObject(PPCContext &ctx, uint8_t *base, PPCFunc *fn, uint32_t object, uint32_t r4 = 0)
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = object;
    frame.ctx.r4.u64 = r4;
    fn(frame.ctx, base);
  }

  // U13 + U17: the character and weapon sub_8253CB78 is stripping.
  thread_local uint32_t g_strip_character = 0;
  thread_local uint32_t g_strip_weapon = 0;
  constexpr uint32_t kFlagWeaponDropReturn = 0x82569760; // sub_82569708's vtable +16 call
  constexpr uint32_t kPlayerRecord = 664;                // character vtable: its player record
  constexpr uint32_t kRecordCarrierId = 932;             // T +944
  constexpr uint32_t kRecordTeam = 940;                  // T +952

  // U16: the teleport (sub_8242AB48) around its move calls.
  constexpr uint32_t kTeleportMoveReturn = 0x8242ADC8;   // sub_8259D940 (type 2)
  constexpr uint32_t kTeleportPlaceReturn = 0x8242ADD8;  // sub_8259DBA8 (type 2)
  constexpr uint32_t kTeleportWarpReturn = 0x8242ADE8;   // sub_8252A8E0 (others)
  constexpr uint32_t kWorldMatrix = 16;                  // vtable: world matrix out (64 bytes)
  constexpr uint32_t kApplyMove = 68;                    // vtable: (old, new) world matrices
  struct Teleport
  {
    uint32_t character = 0;
    uint32_t child = 0; // first attached object, read before the move (as TU3)
    bool children = false;
    uint8_t old_matrix[64] = {};
  };
  thread_local Teleport g_teleport;

  void BeforeTeleportMove(PPCContext &ctx, uint8_t *base, uint32_t character)
  {
    g_teleport = {};
    g_teleport.character = character;
    g_teleport.child = LoadBe32(base, character + 88);
    if (!IsGuestPointer(g_teleport.child) || !LoadBe32(base, g_teleport.child + 72))
    {
      return;
    }
    rex::ppc::stack_guard guard(ctx);
    const uint8_t zero[64] = {};
    const uint32_t matrix = rex::ppc::stack_push(ctx, base, zero, 64);
    CallVirtual(ctx, base, character, kWorldMatrix, matrix);
    std::memcpy(g_teleport.old_matrix, base + matrix, 64);
    g_teleport.children = true;
  }

  void AfterTeleportMove(PPCContext &ctx, uint8_t *base, uint32_t character)
  {
    static uint32_t count = 0;
    if (g_teleport.character != character)
    {
      g_teleport = {};
      return;
    }
    CallObject(ctx, base, sub_82508F78, character);
    CallObject(ctx, base, sub_82508EA8, character, 0);
    uint32_t moved = 0;
    if (g_teleport.children && LoadBe32(base, g_teleport.child + 72))
    {
      rex::ppc::stack_guard guard(ctx);
      const uint8_t zero[64] = {};
      const uint32_t old_matrix = rex::ppc::stack_push(ctx, base, g_teleport.old_matrix, 64);
      const uint32_t new_matrix = rex::ppc::stack_push(ctx, base, zero, 64);
      CallVirtual(ctx, base, character, kWorldMatrix, new_matrix);
      for (uint32_t child = g_teleport.child; IsGuestPointer(child) && moved < 64;
           child = LoadBe32(base, child + 96), ++moved)
      {
        CallObject(ctx, base, sub_82508F78, child);
        CallVirtual(ctx, base, child, kApplyMove, old_matrix, new_matrix);
        CallObject(ctx, base, sub_82508EA8, child, 0);
      }
    }
    g_teleport = {};
    if (count++ % 100 == 0)
    {
      REXLOG_INFO("pdz_fix_tu: teleport: character re-inserted in the world lists, {} attached object(s) "
                  "moved (TU3) ({} time(s))",
                  moved, count);
    }
  }
} // namespace

// U7: before 822F89A0 / 822F8D64 in sub_822F8000 (r31 = the script).
bool PdzTuDatacoreNull(PPCRegister &r31)
{
  static uint32_t count = 0;
  const uint8_t *base = Base();
  if (!base || !Fix(REXCVAR_GET(pdz_fix_tu_datacore_null)) || LoadBe32(base, r31.u32 + 2592) != 0)
  {
    return false;
  }
  Note("Datacore script: tracked character gone, block skipped (TU3)", count);
  return true;
}

// U8: at the start of the four bot task functions (r31 = the task).
bool PdzTuBotTaskNull(PPCRegister &r31)
{
  static uint32_t count = 0;
  const uint8_t *base = Base();
  if (!base || !Fix(REXCVAR_GET(pdz_fix_tu_bot_task_null)) || LoadBe32(base, r31.u32 + 76) != 0)
  {
    return false;
  }
  Note("bot task without its mode object, skipped (TU3)", count);
  return true;
}

// U9: before 821766FC in sub_821766E8 (r8 = the slot's record).
bool PdzTuBaseSlotNull(PPCRegister &r8)
{
  static uint32_t count = 0;
  if (r8.u32 != 0 || !Fix(REXCVAR_GET(pdz_fix_tu_base_slot_null)))
  {
    return false;
  }
  Note("base table: empty slot skipped (TU3)", count);
  return true;
}

// U11: before 823067BC in sub_82303A20 (r17 = the list). Returning true
// jumps to the entry-13 call (bl sub_825C5A78 at 823067B8) with entry 14 and
// 15000, which comes back here; the second visit goes on to entry 49.
bool PdzTuLv10Health(PPCRegister &r17, PPCRegister &r3, PPCRegister &f1)
{
  static bool injected = false;
  static uint32_t count = 0;
  if (injected)
  {
    injected = false;
    return false;
  }
  const uint8_t *base = Base();
  if (!base || !Fix(REXCVAR_GET(pdz_fix_tu_lv10_health)) || LoadBe32(base, r17.u32 + 4) < 15)
  {
    return false;
  }
  const uint32_t entries = LoadBe32(base, r17.u32 + 8);
  const uint32_t character = IsGuestPointer(entries) ? LoadBe32(base, entries + 14 * 8 + 4) : 0;
  if (!IsGuestPointer(character))
  {
    return false;
  }
  if (count++ == 0)
  {
    REXLOG_INFO("pdz_fix_tu: LV10 script: list entry 14 ({:08X}, vtable {:08X}) gets 15000 health like "
                "entry 13 (TU3)",
                character, LoadBe32(base, character));
  }
  r3.u64 = character;
  f1.f64 = 15000.0;
  injected = true;
  return true;
}

// U21: before 82429DF0 in sub_82429978 (the marker loop's skip test).
bool PdzTuPackMarkerSkip()
{
  const uint8_t *base = Base();
  if (!base || !Fix(REXCVAR_GET(pdz_fix_tu_pack_markers)))
  {
    return false;
  }
  const uint32_t game = LoadBe32(base, 0x82D6B2B0);
  const int8_t map = IsGuestPointer(game) ? int8_t(base[game + 25]) : 0;
  return map == 33 || map == 35;
}

// U13 + U17: sub_8253CB78(character, weapon) strips a weapon (and destroys it).
REX_HOOK_RAW(sub_8253CB78)
{
  const uint32_t character = g_strip_character;
  const uint32_t weapon = g_strip_weapon;
  g_strip_character = ctx.r3.u32;
  g_strip_weapon = ctx.r4.u32;
  __imp__sub_8253CB78(ctx, base);
  g_strip_character = character;
  g_strip_weapon = weapon;
}

namespace
{
  // T 8241B088 / 8244C348: a drop from a holder who isn't the carrier is ignored.
  bool IgnoreFlagDrop(PPCContext &ctx, uint8_t *base)
  {
    static uint32_t count = 0;
    if (uint32_t(ctx.lr) != kFlagWeaponDropReturn || !Fix(REXCVAR_GET(pdz_fix_tu_flag_drop)))
    {
      return false;
    }
    // sub_82569708 keeps the weapon in r31.
    if (!IsGuestPointer(g_strip_character) || g_strip_weapon != ctx.r31.u32)
    {
      return false;
    }
    const uint32_t record = CallVirtual(ctx, base, g_strip_character, kPlayerRecord);
    if (!IsGuestPointer(record))
    {
      return false; // TU3: no record, no check
    }
    uint32_t id = LoadBe32(base, record + kRecordCarrierId);
    if (id == 2 || id == 3)
    {
      id += LoadBe32(base, record + kRecordTeam) << 2;
    }
    const uint32_t team = ctx.r4.u32;
    if (id == LoadBe32(base, ctx.r3.u32 + (team + 25) * 4))
    {
      return false;
    }
    Note("flag weapon of a non-carrier destroyed: the carrier's flag isn't dropped (TU3)", count);
    return true;
  }
} // namespace

// sub_8240FFA0 / sub_82440190(mode, team, holder record): "flag dropped" of
// the two flag modes.
REX_HOOK_RAW(sub_8240FFA0)
{
  if (IgnoreFlagDrop(ctx, base))
  {
    return;
  }
  __imp__sub_8240FFA0(ctx, base);
}

REX_HOOK_RAW(sub_82440190)
{
  if (IgnoreFlagDrop(ctx, base))
  {
    return;
  }
  __imp__sub_82440190(ctx, base);
}

// U16: the teleport's move calls (r3 = the character).
REX_HOOK_RAW(sub_8259D940)
{
  if (uint32_t(ctx.lr) == kTeleportMoveReturn && Fix(REXCVAR_GET(pdz_fix_tu_teleport)))
  {
    BeforeTeleportMove(ctx, base, ctx.r3.u32);
  }
  __imp__sub_8259D940(ctx, base);
}

REX_HOOK_RAW(sub_8259DBA8)
{
  const bool teleport = uint32_t(ctx.lr) == kTeleportPlaceReturn && Fix(REXCVAR_GET(pdz_fix_tu_teleport));
  const uint32_t character = ctx.r3.u32;
  __imp__sub_8259DBA8(ctx, base);
  if (teleport)
  {
    AfterTeleportMove(ctx, base, character);
  }
}

REX_HOOK_RAW(sub_8252A8E0)
{
  const bool teleport = uint32_t(ctx.lr) == kTeleportWarpReturn && Fix(REXCVAR_GET(pdz_fix_tu_teleport));
  const uint32_t character = ctx.r3.u32;
  if (teleport)
  {
    BeforeTeleportMove(ctx, base, character);
  }
  __imp__sub_8252A8E0(ctx, base);
  if (teleport)
  {
    AfterTeleportMove(ctx, base, character);
  }
}
