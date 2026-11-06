// Offline matches up to 32 players (you + 31 bots, or split screen + the
// rest), the size Rare built the maps for on Xbox Live. TU3 addresses
// (launch in [brackets]); docs/BUG_AUDIT.md "Bot limit (MB)".
//
// The offline 16 is settings policy (MB1, MB2): the player tables and pools
// already hold 32 or more (MB3, MB4). Three places set it:
// - The max players option (5) reads its value list from a router: offline
//   (base config 16) it answers list 15 = [2..16]; online it answers list
//   13 = [2..32]. Offline now answers list 13 too (T 823BAA94 [823B1F7C]).
// - The base config forces max players back to 16 when it is above (T
//   823B92A4 [823B0AD0]); skipped, so the value chosen stays. TU3's Facility /
//   Ruin branch (8 players) is a separate test and stays.
// - The bot limit option (77 [76], BOT_MAXCOUNT) is set from sub_82479720
//   [8246CBD0], which returns 16 except on Live; it returns 31 now. The bot
//   count setter still clamps bots to max players - humans.
// - The bot limit's value list: the list count reader (sub_823BB748, hooked
//   in cut_scenarios.cpp) answers a fixed 17 for option 77 (T 823BB77C ->
//   823BB7B4), so its values were 0-16 and 31 was refused: the bot count was
//   clamped to nothing (in play testing the bot count couldn't be chosen). It answers 32
//   now; the values are the bot count's own list (99, 0-63), and the first /
//   last / value readers take the count from the same reader.
// - Max players itself is set to 16 whenever setup opens: TU3's defaults (T
//   823BD8E8) and the saved setup loaded option by option (T 823BD338), and
//   offline it has no row of its own (log 246: the bot count asked 63, kept
//   15, with max players 16 and the bot limit 31). Offline it is only a cap
//   (nobody else can join), so a 16 is stored as 32 (the option setter, hooked
//   in cut_scenarios.cpp). Other values (TU3's Facility / Ruin 8) stay.
// - A brand-new profile has no saved setup with a 16 in it, and Rare's cap
//   only lowers max players, so an 8 stayed on every map (bots clamped to 7;
//   2026-11-06 new-player test). After each setup option set, a map other
//   than Facility / Ruin now raises max players to 32 (cut_scenarios.cpp
//   KeepOfflineMaxPlayers, before the DEFAULT bot count), with one
//   'pdz_players: setup:' line whenever max players / the bot limit change.
// The bot count is still chosen in the menu as before; nothing changes in a
// match with 16 or fewer.
// Infection stays at 16 players (crash, logs 301-303, 2026-10-25): Rare's
// team brain keeps 16 per-player-slot records (+520); Infection fills one
// per participant, and the 17th overran the fields after them. The bot limit
// answers Rare's 16 there (cut_scenarios.cpp PdzInfectionChosen), and the
// bot count is clamped at match start. pdz_infection_32 (off) lifts that and
// caps the list's fill instead (cut_scenarios.cpp PdzTeamBrainBotCap).
//
// Watched, not changed: the perception / noise slot table (T 82E5D590
// [82E5C7C8], 170 slots of 80 bytes) has no overflow check (MB5): with every
// slot taken the registration (T 82189918 [82184D58]) uses slot -1 and
// writes before the table. More players make that likelier, so the moment
// it happens is logged (a warning, once a second at most).
#include <chrono>
#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

REX_EXTERN(__imp__sub_82479720);
bool PdzInfectionChosen(PPCContext &ctx, uint8_t *base); // cut_scenarios.cpp: the setup options are DarkOps Infection
bool PdzInfection32();                                   // cut_scenarios.cpp: pdz_infection_32 (Infection up to 32)

namespace
{
  constexpr uint32_t kListPlayers2To32 = 13;
  constexpr uint32_t kOfflineBotLimit = 31;
  constexpr uint32_t kOptionBotLimit = 77; // BOT_MAXCOUNT (launch 76)
} // namespace

// The value the max players option (5) is set to: 16 -> 32.
int32_t PdzPlayerLimitMaxPlayers(int32_t value)
{
  return value == 16 ? 32 : value;
}

// The list count for option 77 (the bot limit): 0-31; -1 = the game's own.
int32_t PdzPlayerLimitListCount(uint32_t option)
{
  return option == kOptionBotLimit ? int32_t(kOfflineBotLimit + 1) : -1;
}

// Before T 823BAA94 (li r3, 15: the offline list) in the option list router:
// list 13 [2..32] instead; true jumps to T 823BAA98 (the router's exit).
bool PdzPlayersMaxList(PPCRegister &r3)
{
  r3.u64 = kListPlayers2To32;
  return true;
}

// Before T 823B92A4 (the base config's force-to-16 of max players): true
// jumps past it to T 823B92E0.
bool PdzKeepPlayersMax()
{
  return true;
}

// Before T 821899B4 (mr r10, r28: no free perception slot, slot -1).
void PdzPerceptionSlotsFull()
{
  static std::chrono::steady_clock::time_point last{};
  static uint32_t count = 0;
  ++count;
  const auto now = std::chrono::steady_clock::now();
  if (now - last >= std::chrono::seconds(1))
  {
    last = now;
    REXLOG_WARN("pdz_players: the perception slot table is full (170 slots): slot -1 used ({} time(s) so far)", count);
  }
}

// sub_82479720 [8246CBD0](settings): the bot limit for the setup defaults.
REX_HOOK_RAW(sub_82479720)
{
  __imp__sub_82479720(ctx, base);
  PPCContext saved = ctx;
  if (ctx.r3.u32 == 16 && (!PdzInfectionChosen(saved, base) || PdzInfection32()))
  {
    // Infection keeps 16 (its team brain's bot list holds 16, logs 301-303)
    // unless pdz_infection_32 keeps that list inside its size. Log 097: read
    // from the setup options; the match settings object still held the last
    // match's mode here.
    ctx.r3.u64 = kOfflineBotLimit;
  }
}
