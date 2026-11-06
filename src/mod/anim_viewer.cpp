// Animation viewer (pdz_debug_anim_viewer, debug, off by default). tu3-base
// only; TU3 addresses.
//
// PageDown plays the next animation of a list on the nearest non-player
// character (picked at the first press; it stays the target until End or until
// it's gone), PageUp the previous one, End resets (list back to the start, the
// character goes back to what its AI does). The list (pdz_debug_anim_viewer_list) loops; by
// default it is the character animations not yet in logs/anim_seen.csv
// (pdz_anim_log), the likely-unused ones first (docs/SP_INVENTORY.md,
// "Animations in depth"). The animation is replayed each time it ends until
// the next key, since the character's AI takes over after one play.
//
// Safety:
// - only character animations (aid_anim_chars_*, not the pigeon set), and
//   only on characters the game itself has played one on (anim_log tracks
//   them, with their vtable), so never on spiderbots, props or animals;
// - an animation the level hasn't loaded (another level's, e.g. Nightclub's
//   bar stool on Datacore) is skipped: sub_821E8A18 binds by name and fails;
// - viewer plays go straight to the original play routine, so pdz_anim_log
//   doesn't count them as seen; nothing is written to saves or settings.
//
// How a play is made: an entry of our own (64 bytes of guest memory per name)
// is bound by sub_821E8A18(name, entry) [+0 name, +4 speed 1, +8 type, +12
// blend, +20 handle] and played with sub_826946A8(character, entry, &position,
// zero vector, -, mode 2, 0, 0, f1 heading, f2 blend 0.2, stack +84 = 0): the
// call an animation task makes (sub_8219C1E0), mode 2 being the one-shot
// mode level scripts use for "out of" / talk animations. The heading is the
// character's own (sub_82519A90).
#include "anim_viewer.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/func.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include "../game_cvars.h"
#include "anim_log.h"

REX_EXTERN(__imp__sub_8269B040);
REX_EXTERN(__imp__sub_826946A8);
REX_EXTERN(sub_821E8A18);
REX_EXTERN(sub_82519A90);

namespace
{
  using Clock = std::chrono::steady_clock;

  struct InventoryEntry
  {
    const char *name;
    int rank; // 0 none, 1 can't play in shipped content ... 4 rare
  };

  constexpr InventoryEntry kInventory[] = {
#include "anim_inventory.inc"
  };

  constexpr uint32_t kPlayerOne = 0x82E3C3E4; // player 1's character (launch 82E3B624)
  constexpr uint32_t kPosition = 48;          // character: position (vec3, cm)
  constexpr uint32_t kEntrySize = 64;
  constexpr uint32_t kEntrySpeed = 4;
  constexpr uint32_t kEntryHandle = 20;
  constexpr uint32_t kHandleData = 8;
  constexpr uint32_t kDataDuration = 28;
  constexpr uint32_t kModeOnce = 2;
  constexpr double kBlend = 0.2;
  constexpr auto kTargetLost = std::chrono::seconds(1);
  constexpr auto kCandidateFresh = std::chrono::milliseconds(500);
  constexpr auto kReplayGap = std::chrono::milliseconds(600);
  constexpr auto kLabelTime = std::chrono::milliseconds(4000);
  constexpr auto kRepeatGap = std::chrono::milliseconds(300);

  enum Command : int
  {
    kNone = 0,
    kNext = 1,
    kPrev = -1,
    kReset = 2,
  };

  std::atomic<int> g_command{kNone};

  // Label (UI thread draws, guest thread sets).
  std::mutex g_label_mutex;
  std::string g_label;
  Clock::time_point g_label_until;

  void ShowLabel(std::string text)
  {
    REXLOG_INFO("pdz_anim_viewer: {}", text);
    std::lock_guard<std::mutex> lock(g_label_mutex);
    g_label = std::move(text);
    g_label_until = Clock::now() + kLabelTime;
  }

  class LabelDialog final : public rex::ui::ImGuiDialog
  {
  public:
    explicit LabelDialog(rex::ui::ImGuiDrawer *drawer) : ImGuiDialog(drawer) {}

  protected:
    void OnDraw(ImGuiIO &io) override
    {
      std::string text;
      {
        std::lock_guard<std::mutex> lock(g_label_mutex);
        if (Clock::now() >= g_label_until)
        {
          return;
        }
        text = g_label;
      }
      ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.12f), ImGuiCond_Always,
                              ImVec2(0.5f, 0.0f));
      ImGui::SetNextWindowBgAlpha(0.75f);
      constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                          ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
                                          ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings;
      if (ImGui::Begin("##pdz_anim_viewer", nullptr, kFlags))
      {
        ImGui::TextUnformatted(text.c_str());
      }
      ImGui::End();
    }
  };

  std::unique_ptr<LabelDialog> g_dialog;

  // Guest-thread state (the update hook).
  struct Candidate
  {
    float position[3];
    Clock::time_point when;
  };

  std::vector<const InventoryEntry *> g_list;
  std::string g_list_mode;
  size_t g_index = 0;
  bool g_started = false;
  uint32_t g_target = 0;
  Clock::time_point g_target_seen;
  bool g_play_now = false;
  int g_direction = kNext;
  Clock::time_point g_next_play;
  bool g_replaying = false;
  std::unordered_map<uint32_t, Candidate> g_candidates;
  std::unordered_map<std::string, uint32_t> g_entries; // name -> our guest entry
  uint32_t g_zero = 0;                                  // 16 zero bytes (the offset vector)

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
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  bool IsGuestPointer(uint32_t address)
  {
    return address >= 0x00010000u && address < 0xFFFF0000u;
  }

  std::string Short(const char *name)
  {
    return std::strncmp(name, "aid_anim_chars_", 15) == 0 ? std::string(name + 15) : std::string(name);
  }

  bool ListMember(const InventoryEntry &e, const std::string &mode, const std::unordered_set<std::string> &seen)
  {
    if (std::strncmp(e.name, "aid_anim_chars_pigeon_", 22) == 0)
    {
      return false; // the pigeons' own skeleton
    }
    if (mode == "all")
    {
      return true;
    }
    if (mode == "suspects")
    {
      return e.rank > 0;
    }
    std::string lower = e.name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (mode == "unseen")
    {
      return seen.count(lower) == 0;
    }
    return lower.find(mode) != std::string::npos;
  }

  void BuildList()
  {
    g_list_mode = REXCVAR_GET(pdz_debug_anim_viewer_list);
    std::transform(g_list_mode.begin(), g_list_mode.end(), g_list_mode.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (g_list_mode.empty())
    {
      g_list_mode = "unseen";
    }
    const std::unordered_set<std::string> seen =
        g_list_mode == "unseen" ? anim_log::SeenNames() : std::unordered_set<std::string>();
    g_list.clear();
    for (const InventoryEntry &e : kInventory)
    {
      if (ListMember(e, g_list_mode, seen))
      {
        g_list.push_back(&e);
      }
    }
    // Likely-unused first (rank 1 .. 4), then the rest; by name within.
    std::stable_sort(g_list.begin(), g_list.end(), [](const InventoryEntry *a, const InventoryEntry *b)
                     {
      const int ra = a->rank ? a->rank : 99;
      const int rb = b->rank ? b->rank : 99;
      return ra != rb ? ra < rb : std::strcmp(a->name, b->name) < 0; });
    g_index = 0;
    g_started = false;
  }

  bool Position(const uint8_t *base, uint32_t character, float out[3])
  {
    for (int i = 0; i < 3; ++i)
    {
      out[i] = LoadFloat(base, character + kPosition + 4 * i);
    }
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
  }

  // The nearest human character updated in the last half second (0 if none).
  uint32_t Nearest(const uint8_t *base, float &distance)
  {
    const uint32_t player = Load32(base, kPlayerOne);
    float here[3];
    if (!IsGuestPointer(player) || !Position(base, player, here))
    {
      return 0;
    }
    const auto now = Clock::now();
    uint32_t best = 0;
    float best2 = 0.0f;
    for (auto it = g_candidates.begin(); it != g_candidates.end();)
    {
      if (now - it->second.when > kCandidateFresh)
      {
        it = g_candidates.erase(it);
        continue;
      }
      float d2 = 0.0f;
      for (int i = 0; i < 3; ++i)
      {
        const float d = it->second.position[i] - here[i];
        d2 += d * d;
      }
      if (!best || d2 < best2)
      {
        best = it->first;
        best2 = d2;
      }
      ++it;
    }
    distance = std::sqrt(best2) / 100.0f; // cm -> m
    return best;
  }

  // Our entry for name, bound for this level; 0 if the animation isn't loaded.
  uint32_t Bind(PPCContext &ctx, uint8_t *base, const char *name)
  {
    auto it = g_entries.find(name);
    if (it == g_entries.end())
    {
      const size_t length = std::strlen(name);
      const uint32_t entry = REX_KERNEL_MEMORY()->SystemHeapAlloc(uint32_t(kEntrySize + length + 1));
      if (!entry)
      {
        return 0;
      }
      std::memset(base + entry, 0, kEntrySize);
      std::memcpy(base + entry + kEntrySize, name, length + 1);
      it = g_entries.emplace(name, entry).first;
    }
    const uint32_t entry = it->second;
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = entry + kEntrySize; // name
    frame.ctx.r4.u64 = entry;
    sub_821E8A18(frame.ctx, base);
    return (frame.ctx.r3.u32 & 0xFF) && IsGuestPointer(Load32(base, entry + kEntryHandle)) ? entry : 0;
  }

  double Duration(const uint8_t *base, uint32_t entry)
  {
    const uint32_t handle = Load32(base, entry + kEntryHandle);
    const uint32_t data = IsGuestPointer(handle) ? Load32(base, handle + kHandleData) : 0;
    const float speed = LoadFloat(base, entry + kEntrySpeed);
    const float length = IsGuestPointer(data) ? LoadFloat(base, data + kDataDuration) : 0.0f;
    const double seconds = speed > 0.01f && std::isfinite(length) ? length / speed : 0.0;
    return seconds >= 0.1 && seconds <= 60.0 ? seconds : 3.0;
  }

  void Play(PPCContext &ctx, uint8_t *base, uint32_t character, uint32_t entry)
  {
    if (!g_zero)
    {
      g_zero = REX_KERNEL_MEMORY()->SystemHeapAlloc(16);
      if (!g_zero)
      {
        return;
      }
    }
    std::memset(base + g_zero, 0, 16);
    // Our frame below the hook's stack pointer; the stack argument at +84.
    const uint32_t sp = (ctx.r1.u32 - 0x200) & ~0xFu;
    Store32(base, sp + 84, 0);
    rex::CallFrame frame(ctx);
    frame.ctx.r1.u64 = sp;
    frame.ctx.r3.u64 = character;
    sub_82519A90(frame.ctx, base);
    const double heading = frame.ctx.f1.f64;
    frame.ctx.r1.u64 = sp;
    frame.ctx.r3.u64 = character;
    frame.ctx.r4.u64 = entry;
    // The base position (vec3) the animation is placed from, read without a
    // null check by sub_826947E0: the character's own (a null here crashed,
    // log 060).
    frame.ctx.r5.u64 = character + kPosition;
    frame.ctx.r6.u64 = g_zero;
    frame.ctx.r7.u64 = 0;
    frame.ctx.r8.u64 = kModeOnce;
    frame.ctx.r9.u64 = 0;
    frame.ctx.r10.u64 = 0;
    frame.ctx.f1.f64 = std::isfinite(heading) ? heading : 0.0;
    frame.ctx.f2.f64 = kBlend;
    __imp__sub_826946A8(frame.ctx, base); // not the hooked one: viewer plays aren't "seen"
  }

  void Advance(int direction)
  {
    if (g_list.empty())
    {
      return;
    }
    const size_t n = g_list.size();
    g_index = direction == kPrev ? (g_index + n - 1) % n : (g_index + 1) % n;
  }

  // Plays the current animation on the target; on a key press, skips the ones
  // this level hasn't loaded (in the key's direction).
  void PlayCurrent(PPCContext &ctx, uint8_t *base, uint32_t character, bool announce, float distance)
  {
    for (size_t tries = 0; tries < g_list.size(); ++tries)
    {
      const InventoryEntry &e = *g_list[g_index];
      const uint32_t entry = Bind(ctx, base, e.name);
      if (entry)
      {
        Play(ctx, base, character, entry);
        g_next_play = Clock::now() + std::chrono::milliseconds(int64_t(Duration(base, entry) * 1000.0)) + kReplayGap;
        g_replaying = true;
        if (announce)
        {
          ShowLabel(fmt::format("Anim {}/{}: {}{} ({:.1f} m, {} list){}", g_index + 1, g_list.size(), Short(e.name),
                                e.rank ? fmt::format(" [rank {}]", e.rank) : std::string(), distance, g_list_mode,
                                tries ? fmt::format(", {} not loaded here skipped", tries) : std::string()));
        }
        return;
      }
      if (!announce)
      {
        g_replaying = false; // a replay after a level change: stop
        return;
      }
      Advance(g_direction);
    }
    g_replaying = false;
    ShowLabel(fmt::format("Anim viewer: none of the {} animations of the {} list is loaded here", g_list.size(),
                          g_list_mode));
  }

  void Reset()
  {
    g_target = 0;
    g_replaying = false;
    g_play_now = false;
    BuildList();
    ShowLabel(fmt::format("Anim viewer reset: {} animations ({} list); PageDown next, PageUp back, End reset", g_list.size(),
                          g_list_mode));
  }

  float g_target_distance = 0.0f;

  void Update(PPCContext &ctx, uint8_t *base, uint32_t character)
  {
    const auto now = Clock::now();
    const uint32_t player = Load32(base, kPlayerOne);
    if (character != player && anim_log::IsHuman(base, character))
    {
      Candidate c;
      if (Position(base, character, c.position))
      {
        c.when = now;
        g_candidates[character] = c;
      }
    }
    if (character == g_target)
    {
      g_target_seen = now;
    }
    else if (g_target && now - g_target_seen > kTargetLost)
    {
      g_target = 0; // died, removed or the level changed
      g_replaying = false;
    }

    const int command = g_command.exchange(kNone);
    if (command == kReset)
    {
      Reset();
      return;
    }
    if (command == kNext || command == kPrev)
    {
      std::string mode = REXCVAR_GET(pdz_debug_anim_viewer_list);
      std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return char(std::tolower(c)); });
      if (g_list.empty() || g_list_mode != (mode.empty() ? std::string("unseen") : mode))
      {
        BuildList();
      }
      if (g_list.empty())
      {
        ShowLabel(fmt::format("Anim viewer: the {} list is empty", g_list_mode));
        return;
      }
      if (g_started)
      {
        Advance(command);
      }
      g_started = true;
      g_direction = command;
      if (!g_target || !anim_log::IsHuman(base, g_target))
      {
        float distance = 0.0f;
        g_target = Nearest(base, distance);
        g_target_distance = distance;
        g_target_seen = now;
        if (!g_target)
        {
          ShowLabel("Anim viewer: no character nearby (one that has moved or fought since the level loaded)");
          return;
        }
      }
      g_play_now = true;
    }

    if (character != g_target || !g_target)
    {
      return;
    }
    if (g_play_now)
    {
      g_play_now = false;
      PlayCurrent(ctx, base, character, true, g_target_distance);
    }
    else if (g_replaying && now >= g_next_play)
    {
      PlayCurrent(ctx, base, character, false, g_target_distance);
    }
  }

  bool FreshPress(Clock::time_point &last_event)
  {
    const auto now = Clock::now();
    const bool fresh = now - last_event > kRepeatGap;
    last_event = now;
    return fresh;
  }

  void Press(Clock::time_point &last_event, int command)
  {
    if (!FreshPress(last_event))
    {
      return;
    }
    if (!REXCVAR_GET(pdz_debug_anim_viewer))
    {
      ShowLabel("Anim viewer is off (--pdz_debug_anim_viewer=true)");
      return;
    }
    g_command.store(command);
  }
} // namespace

// sub_8269B040(character, mover, f1 = dt): a character's movement animation
// update (every walking character, each frame).
REX_HOOK_RAW(sub_8269B040)
{
  const uint32_t character = ctx.r3.u32;
  __imp__sub_8269B040(ctx, base);
  if (REXCVAR_GET(pdz_debug_anim_viewer) && IsGuestPointer(character))
  {
    Update(ctx, base, character);
  }
}

namespace anim_viewer
{
  void Init(rex::ui::ImGuiDrawer *drawer)
  {
    g_dialog = std::make_unique<LabelDialog>(drawer);
    rex::ui::RegisterBind("bind_anim_viewer_next", "PageDown", "Animation viewer: next animation on the nearest character", []
                          {
      static Clock::time_point last_event{};
      Press(last_event, kNext); });
    rex::ui::RegisterBind("bind_anim_viewer_prev", "PageUp", "Animation viewer: previous animation", []
                          {
      static Clock::time_point last_event{};
      Press(last_event, kPrev); });
    rex::ui::RegisterBind("bind_anim_viewer_reset", "End", "Animation viewer: reset (list start, character back to normal)", []
                          {
      static Clock::time_point last_event{};
      Press(last_event, kReset); });
  }
} // namespace anim_viewer
