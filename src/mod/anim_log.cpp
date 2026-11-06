// Animation log (pdz_anim_log, diagnostics, off by default). tu3-base only;
// TU3 addresses, launch addresses in brackets where known.
//
// Appends every animation the game starts, once, to logs/anim_seen.csv, kept
// across sessions (names already in the file aren't added again), so
// tools/anim_report.py can list the animations never seen in play. See
// docs/SP_INVENTORY.md, "Animations in depth".
//
// Animation entries (the group tables' rows, rebuilt at load by sub_821E8A18
// [name, entry]): +0 name (char *), +4 speed, +8 AnimType, +12 blend,
// +20 animation handle. Three places hand one out:
// - sub_821F00C8(character, AnimType, flags) picks a character's animation of
//   a type from its set (joanims / henchmananims: walks, misc, hits, deaths,
//   shooting, close-range attacks) and returns the entry ("type");
// - sub_8214E100(table, name) looks a named animation up among the
//   pre-loaded ones (scripts, cutscene actors, hardpoints) and returns its
//   record, name at +0 ("name");
// - sub_826946A8(character, entry, ...) plays an entry on a character
//   ("play"; anim tasks, gun raise / lower, level scripts).
// Weapon first-person, weapon overlay and prop animations play through other
// paths and aren't recorded.
#include "anim_log.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/ppc/func.h>
#include <rex/logging.h>

#include "../game_cvars.h"
#include "ai_behaviour_debug.h"
#include "bot_watch.h"
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

REX_EXTERN(__imp__sub_821F00C8);
REX_EXTERN(__imp__sub_8214E100);
REX_EXTERN(__imp__sub_826946A8);

namespace
{
  constexpr uint32_t kGameInfo = 0x82D6BFD8; // -> +40 game type (0 single player, 1 co-op, 2 / 3 multiplayer)
  constexpr uint32_t kEntryName = 0;
  constexpr uint32_t kEntryType = 8;
  constexpr size_t kNameMax = 128;
  const char *const kPath = "logs/anim_seen.csv";

  uint32_t Load32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  bool IsGuestPointer(uint32_t address)
  {
    return address >= 0x00010000u && address < 0xFFFF0000u;
  }

  bool LogEnabled()
  {
    return REXCVAR_GET(pdz_anim_log);
  }

  std::mutex g_mutex;
  bool g_loaded = false;
  std::unordered_set<std::string> g_names;     // lower case, every session
  struct EntryInfo
  {
    bool named = false;  // +0 points at an asset name
    bool human = false;  // a character animation (not the pigeons')
    bool logged = false; // checked against the log already
  };
  std::unordered_map<uint64_t, EntryInfo> g_entries; // by (name pointer, entry)
  std::unordered_map<uint32_t, uint32_t> g_humans; // character -> its vtable when seen starting a character animation
  FILE *g_csv = nullptr;
  uint32_t g_new_this_session = 0;

  std::string Lower(std::string s)
  {
    for (char &c : s)
    {
      if (c >= 'A' && c <= 'Z')
      {
        c = char(c - 'A' + 'a');
      }
    }
    return s;
  }

  // Under g_mutex.
  void LoadSeen()
  {
    if (g_loaded)
    {
      return;
    }
    g_loaded = true;
    if (FILE *f = std::fopen(kPath, "r"))
    {
      char line[512];
      bool header = true;
      while (std::fgets(line, sizeof(line), f))
      {
        if (header)
        {
          header = false;
          continue;
        }
        const char *comma = std::strchr(line, ',');
        std::string name = comma ? std::string(line, size_t(comma - line)) : std::string(line);
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r'))
        {
          name.pop_back();
        }
        if (!name.empty())
        {
          g_names.insert(Lower(name));
        }
      }
      std::fclose(f);
    }
    REXLOG_INFO("pdz_anim_log: {} animations already in {}", g_names.size(), kPath);
  }

  // The guest C string at address (empty unless it looks like an asset name).
  std::string GuestName(const uint8_t *base, uint32_t address)
  {
    if (!IsGuestPointer(address))
    {
      return {};
    }
    std::string s;
    for (size_t i = 0; i < kNameMax; ++i)
    {
      const char c = char(base[address + i]);
      if (c == 0)
      {
        break;
      }
      if (c < 0x20 || c > 0x7E)
      {
        return {};
      }
      s.push_back(c);
    }
    return s.rfind("aid_", 0) == 0 ? s : std::string();
  }

  std::string Group(const std::string &name)
  {
    // aid_anim_chars_<group>_..., aid_anim_<kind>_...
    const std::string rest = name.size() > 9 ? name.substr(9) : name;
    const size_t a = rest.find('_');
    if (a == std::string::npos)
    {
      return rest;
    }
    if (rest.compare(0, a, "chars") == 0)
    {
      const size_t b = rest.find('_', a + 1);
      return "chars/" + rest.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
    }
    return rest.substr(0, a);
  }

  bool HumanAnimation(const std::string &lower)
  {
    return lower.rfind("aid_anim_chars_", 0) == 0 && lower.rfind("aid_anim_chars_pigeon_", 0) != 0;
  }

  const char *Mode(const uint8_t *base)
  {
    const uint32_t info = Load32(base, kGameInfo);
    switch (IsGuestPointer(info) ? Load32(base, info + 40) : 0xFFFFFFFFu)
    {
    case 0:
      return "single player";
    case 1:
      return "co-op";
    case 2:
    case 3:
      return "multiplayer";
    default:
      return "-";
    }
  }

  // An entry (or named record) handed out for character (0 if none).
  void Observe(const uint8_t *base, uint32_t entry, uint32_t character, const char *source, bool typed)
  {
    if (!IsGuestPointer(entry))
    {
      return;
    }
    const uint32_t name_address = Load32(base, entry + kEntryName);
    const uint64_t key = (uint64_t(name_address) << 32) | entry;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_entries.find(key);
    if (it == g_entries.end())
    {
      const std::string name = GuestName(base, name_address);
      EntryInfo info;
      info.named = !name.empty();
      info.human = info.named && HumanAnimation(Lower(name));
      it = g_entries.emplace(key, info).first;
    }
    EntryInfo &info = it->second;
    if (character && info.human)
    {
      g_humans[character] = Load32(base, character);
    }
    if (!info.named || info.logged || !LogEnabled())
    {
      return;
    }
    info.logged = true;
    const std::string lower = Lower(GuestName(base, name_address));
    LoadSeen();
    if (!g_names.insert(lower).second)
    {
      return;
    }
    if (!g_csv)
    {
      FILE *probe = std::fopen(kPath, "r");
      const bool fresh = probe == nullptr;
      if (probe)
      {
        std::fclose(probe);
      }
      g_csv = std::fopen(kPath, "a");
      if (g_csv && fresh)
      {
        std::fprintf(g_csv, "name,group,source,anim_type,mode,first_seen\n");
      }
    }
    if (!g_csv)
    {
      return;
    }
    const std::time_t now = std::time(nullptr);
    char date[32] = {};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    const std::string type = typed ? fmt::format("{:#x}", Load32(base, entry + kEntryType)) : std::string("-");
    std::fprintf(g_csv, "%s,%s,%s,%s,%s,%s\n", lower.c_str(), Group(lower).c_str(), source, type.c_str(),
                 Mode(base), date);
    std::fflush(g_csv);
    ++g_new_this_session;
    REXLOG_INFO("pdz_anim_log: new #{} {} ({}, {})", g_new_this_session, lower, source, type);
  }
} // namespace

namespace anim_log
{
  bool Tracking()
  {
    return REXCVAR_GET(pdz_anim_log) || REXCVAR_GET(pdz_debug_anim_viewer);
  }

  bool IsHuman(const uint8_t *base, uint32_t character)
  {
    if (!IsGuestPointer(character))
    {
      return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_humans.find(character);
    // Same vtable as when seen: not a freed slot reused by another kind of object.
    return it != g_humans.end() && it->second == Load32(base, character);
  }

  std::unordered_set<std::string> SeenNames()
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadSeen();
    return g_names;
  }
} // namespace anim_log

// sub_821F00C8(character, AnimType, flags): a character's animation of a type.
// Also counts enemies' dodges for pdz_ai_behaviour_log (ai_behaviour_debug).
REX_HOOK_RAW(sub_821F00C8)
{
  const uint32_t character = ctx.r3.u32;
  const uint32_t anim_type = ctx.r4.u32;
  __imp__sub_821F00C8(ctx, base);
  ai_behaviour_debug::OnAnimType(character, anim_type);
  if (anim_log::Tracking())
  {
    Observe(base, ctx.r3.u32, character, "type", true);
  }
}

// sub_8214E100(table, name): a pre-loaded named animation's record (name at +0).
REX_HOOK_RAW(sub_8214E100)
{
  __imp__sub_8214E100(ctx, base);
  if (anim_log::Tracking())
  {
    Observe(base, ctx.r3.u32, 0, "name", false);
  }
}

// sub_826946A8(character, entry, ...): plays an animation entry on a character.
REX_HOOK_RAW(sub_826946A8)
{
  if (const uint32_t watched = bot_watch::Watched(); watched && watched == ctx.r3.u32)
  {
    const uint32_t entry = ctx.r4.u32;
    const uint32_t name = entry >= 0x10000u ? (uint32_t(base[entry]) << 24) | (uint32_t(base[entry + 1]) << 16) |
                                                  (uint32_t(base[entry + 2]) << 8) | base[entry + 3]
                                            : 0;
    if (name >= 0x10000u && name < 0xFFFF0000u)
    {
      const char *text = reinterpret_cast<const char *>(base + name);
      bot_watch::Note(watched, "anim " + std::string(text, strnlen(text, 64)));
    }
  }
  if (anim_log::Tracking())
  {
    Observe(base, ctx.r4.u32, ctx.r3.u32, "play", true);
  }
  __imp__sub_826946A8(ctx, base);
}
