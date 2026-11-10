// Counter-Operative's menu entry, mission gate and end text (pdz_counterop_menu; built only with PDZ_COUNTEROP).
// See docs/COUNTEROP_STUDY.md and work\C_flow\FINDINGS.md (8 Nov 2026, untested).
//
// MISSION MODE (aid_darkxui_frontend_menu_storymode, the campaign's mode select) ships SOLO AGENT (Id Solo) and
// CO-OPERATIVE (Id CoOp): DarkXuiValueNavButtons (class 823FD2F8; properties GameValue, ButtonValue, InfoScene, and
// XuiNavButton's PressPath) on DARKVALUE_GAME_TYPE (id 79). Their press (823FF4C8, message 2021) calls Rare's value
// setter 823B8AD8(79, ButtonValue), whose case 79 stores the game type ([82D6BFD8] +40: 0 solo, 1 co-op, 2
// multiplayer); XuiNavButton then opens PressPath. Rare left no counter-op button in the scene (its only counter-op
// leftovers are frontend text 4 / 5 "COUNTER" / "OPERATIVE" of the old GAME MODE SELECT, 146-148, and the info card
// aid_darkxui_frontend_info_counteroperative with placeholder text), and no game type for it.
//
// Here, when MISSION MODE is created (XuiSceneCreate 8278F550, hooked in mp_music.cpp, which calls in), a third
// DarkXuiValueNavButton "CounterOp" is made the way mp_music.cpp makes its Music row (XuiCreateObject 827865B8, the
// CoOp button's own properties copied, rows below moved down one row, navigation relinked, 8278E870): the same
// game value (co-op, 1) and PressPath (CO-OP CONNECTION MODE), Rare's text 4 + 5 and Rare's counter-op card (its
// placeholder line replaced with Rare's text 146). Pressing it arms Counter-Op; SOLO AGENT, CO-OPERATIVE or COMBAT
// ARENA (any game-type press) disarms it, and so does SYSTEM LINK / XBOX LIVE on the connection screen (local only).
// The rest is Rare's co-op route: LOCAL, CO-OP MISSION SELECT (player 2 joins there, Rare's partner chooser),
// difficulty, briefing, start. The co-op screens' "CO-OP" header reads Rare's "COUNTER-OP" (147 -> 148) while armed.
// counter_op.cpp's On() is pdz_counterop || PdzCounterOpArmed(), so the mode runs for that game only.
//
// Missions: the MISSION SELECT list index is the level id (82402790 stores it with 82230470 as game +25). Datacore
// and Arena place no guards (study section 4), so while armed, opening the difficulty screen for one of them is
// refused: the difficulty scene is not created (XuiNavButton's handler 827A84B8 returns on a failed create, so the
// player stays on MISSION SELECT) and a message box says why.
//
// End: the agent down = Rare's co-op game over (counter_op.cpp: 82E426F8 |= 2), the agent's win = Rare's COMPLETED
// bit (82E426F8 bit 1); both end in Rare's debriefing aid_darkxui_ingame_large_solo_debriefing (every player count,
// table 820896D4), header "MISSION OVER". After a counter-op game its header shows "COUNTER-OP" (frontend 148, when
// that table is loaded) and Rare's "MISSION FAILED" / "MISSION COMPLETED" (general 66 / 67).
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/ppc/stack.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include "hook_profile.h" // pdz_hook_profile (must follow <rex/hook.h>)

REXCVAR_DEFINE_BOOL(pdz_counterop_menu, false, "Gameplay",
                    "Counter-Operative in the campaign menu (built only with PDZ_COUNTEROP; default off: not ready "
                    "yet): MISSION MODE gets a "
                    "COUNTER OPERATIVE entry beside SOLO AGENT and CO-OPERATIVE (Rare's own text and card); choosing "
                    "it plays the next local co-op games as Counter-Operative (player 2 on the guards' side) until "
                    "another mode is chosen. Datacore and Arena are refused (no guards). Off = no entry, plain menus "
                    "(pdz_counterop still forces the mode for every co-op game)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_counterop_menu_coop_card, true, "Gameplay",
                    "Counter-Operative menu entry (default on): its description card is built from Rare's "
                    "CO-OPERATIVE card (same place, bottom right, with Rare's COUNTER OPERATIVE text and line), "
                    "not from Rare's unused counter-op card, whose text sits on the menu column over LEADERBOARDS")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_8278F550); // XuiSceneCreate (base path, path, init data, &scene); hooked in mp_music.cpp
REX_EXTERN(__imp__sub_823FF4C8); // DarkXuiValueNavButton press (this): ButtonValue -> 823B8AD8(GameValue, value)
REX_EXTERN(__imp__sub_82402318); // MissionList message handler (this: +4 the list's handle)
REX_EXTERN(sub_823AB3C8);        // list: current item index (handle)
REX_EXTERN(sub_8227AD78);        // MISSION SELECT index -> level id
REX_EXTERN(sub_8239BCD8);        // a string by id ((table << 16) | index) -> UTF-16 pointer or 0 (table not loaded)
REX_EXTERN(sub_8262E740);        // Rare's string object: init empty (object)
REX_EXTERN(sub_8262F330);        // assign (object, UTF-16, length, 1)
REX_EXTERN(sub_8262ED18);        // free a string buffer
REX_EXTERN(sub_82400FB0);        // message box (user, text object, caption object, buttons) (82400DB0's tail)
// XUI (as mp_music.cpp uses them)
REX_EXTERN(sub_827865B8); // XuiCreateObject(class, &h)
REX_EXTERN(sub_8278A3B8); // XuiElementAddChild(parent, child)
REX_EXTERN(sub_8278A4C8); // first child (h, &out)
REX_EXTERN(sub_8278A5B8); // next sibling (h, &out)
REX_EXTERN(sub_8278E870); // init a created element like a loaded one (h, 0)
REX_EXTERN(sub_8277B7F8); // property id by name (h, name, &id)
REX_EXTERN(sub_8277F928); // get property (h, id, 0, &val)
REX_EXTERN(sub_8277FA28); // set property (h, id, 0, &val)
REX_EXTERN(sub_82794EC0); // XUIElementPropVal clear
REX_EXTERN(sub_827950E0); // XUIElementPropVal set string

bool PdzCounterOpPlayed();   // counter_op.cpp: the last level started as a supported counter-op game
bool PdzCounterOpClonesOn(); // counter_op.cpp: pdz_counterop_clones (Datacore / Arena offered: runtime clones)

namespace
{
  constexpr uint32_t kGame = 0x82D6BFD8;          // -> game state (+32 connection type, +40 game type)
  constexpr uint32_t kLevelState = 0x82E426F8;    // byte: 0x1 completed, 0x2 failed
  constexpr uint32_t kFrontendUser = 0x82E74070;  // +20: the frontend's user (255 = none)
  constexpr uint32_t kLevelList = 0x82D6C2B4;     // +8 entries (+0 name), +20 / +24 the SP id -> index table
  constexpr uint32_t kGameTypeValue = 79, kConnectionValue = 80; // DARKVALUE_GAME_TYPE / _CONNECTION_TYPE
  constexpr uint32_t kTextCounter = 0x20004, kTextOperative = 0x20005; // frontend: COUNTER / OPERATIVE
  constexpr uint32_t kTextCardLine = 0x20092;     // frontend 146: "for two-player counter-operative story missions"
  constexpr uint32_t kTextCoOp = 0x20093, kTextCounterOp = 0x20094; // frontend 147 / 148: CO-OP / COUNTER-OP
  constexpr uint32_t kTextInformation = 0x203E9;  // frontend 1001: Information
  constexpr uint32_t kTextFailed = 0x40042, kTextCompleted = 0x40043; // general 66 / 67
  constexpr const char *kAnchorId = "CoOp";
  constexpr const char *kButtonId = "CounterOp";
  constexpr const char *kCard = "aid_darkxui_frontend_info_counteroperative_xur";
  // Rare's co-op card (CO-OPERATIVE's InfoScene): a scene the size of the info area (891 x 720) whose group 'text3'
  // (516 x 124 at (345, 535)) holds 'level name' and 'location text', with two pictures 'level pic1' / 'level pic'.
  // Rare's unused counter-op card is a 541 x 564 scene with 'level name' at (94, 483) and 'location text' at (17,
  // 515) (515 x 99) directly in it, made for the old GAME MODE SELECT screen: in MISSION MODE that text lands on
  // the menu column (LEADERBOARDS, moved to y 512 by the new row). The card is made from the co-op card instead.
  constexpr const char *kCardFrom = "counteroperative", *kCardTo = "cooperative";
  constexpr uint32_t kPropValSize = 20, kPropString = 5, kPropVector = 7;
  constexpr const char *kCopiedProps[] = {"Visual",  "Width",     "Height",          "Anchor",   "Pivot",
                                          "Scale",   "Opacity",   "Show",            "ClipChildren",
                                          "BlendMode", "ColorWriteFlags", "Tab",     "PressKey",
                                          "PressPath", "StayVisible", "SrcTransIndex", "DestTransIndex",
                                          "GameValue", "ButtonValue"};

  bool g_armed = false;
  bool g_card_from_coop = false; // the counter-op card being created is Rare's co-op card (pdz_counterop_menu_coop_card)
  uint32_t g_card_lines = 0;
  uint32_t g_mission_list = 0; // the last MissionList's handle

  bool Menu() { return REXCVAR_GET(pdz_counterop_menu); }

  uint32_t Load32(const uint8_t *base, uint32_t a)
  {
    const uint8_t *p = base + a;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }
  void Store32(uint8_t *base, uint32_t a, uint32_t v)
  {
    base[a] = uint8_t(v >> 24);
    base[a + 1] = uint8_t(v >> 16);
    base[a + 2] = uint8_t(v >> 8);
    base[a + 3] = uint8_t(v);
  }
  bool Ptr(uint32_t a) { return a >= 0x10000u; }

  // UTF-16 code units, kept as they are (Rare's localised text survives the round trip).
  std::u16string ReadWide(const uint8_t *base, uint32_t address, size_t max = 256)
  {
    std::u16string text;
    for (size_t i = 0; Ptr(address) && i < max; ++i)
    {
      const char16_t c = char16_t(base[address + i * 2] << 8 | base[address + i * 2 + 1]);
      if (!c)
      {
        break;
      }
      text += c;
    }
    return text;
  }
  std::u16string Wide(const std::string &ascii) { return std::u16string(ascii.begin(), ascii.end()); }
  std::string Ascii(const std::u16string &text)
  {
    std::string out;
    for (char16_t c : text)
    {
      out += c < 0x80 ? char(c) : '?';
    }
    return out;
  }
  uint32_t PushWide(PPCContext &frame, uint8_t *base, const std::u16string &text)
  {
    std::string bytes;
    for (char16_t c : text)
    {
      bytes += char(c >> 8);
      bytes += char(c & 0xFF);
    }
    bytes.append(2, '\0');
    return rex::ppc::stack_push(frame, base, bytes.data(), uint32_t(bytes.size()));
  }

  // Rare's string by id (empty when its table isn't loaded).
  std::u16string RareText(PPCContext &ctx, uint8_t *base, uint32_t id)
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = id;
    sub_8239BCD8(frame.ctx, base);
    return ReadWide(base, frame.ctx.r3.u32);
  }

  // XUI calls on one frame (as mp_music.cpp's Xui).
  struct Xui
  {
    rex::CallFrame frame;
    uint8_t *base;
    uint32_t val, out;
    Xui(PPCContext &ctx, uint8_t *b) : frame(ctx), base(b)
    {
      val = rex::ppc::stack_push(frame.ctx, base, std::string(kPropValSize, '\0').data(), kPropValSize);
      out = rex::ppc::stack_push<uint32_t>(frame.ctx, base, 0);
    }
    uint32_t PropId(uint32_t h, const std::string &name)
    {
      const uint32_t saved = frame.ctx.r1.u32;
      Store32(base, out, 0xFFFFFFFFu);
      frame.ctx.r3.u64 = h;
      frame.ctx.r4.u64 = PushWide(frame.ctx, base, Wide(name));
      frame.ctx.r5.u64 = out;
      sub_8277B7F8(frame.ctx, base);
      frame.ctx.r1.u32 = saved;
      return int32_t(frame.ctx.r3.u32) < 0 ? 0xFFFFFFFFu : Load32(base, out);
    }
    void Clear()
    {
      frame.ctx.r3.u64 = val;
      sub_82794EC0(frame.ctx, base);
    }
    bool Get(uint32_t h, const std::string &name)
    {
      std::memset(base + val, 0, kPropValSize);
      const uint32_t id = PropId(h, name);
      if (id == 0xFFFFFFFFu)
      {
        return false;
      }
      frame.ctx.r3.u64 = h;
      frame.ctx.r4.u64 = id;
      frame.ctx.r5.u64 = 0;
      frame.ctx.r6.u64 = val;
      sub_8277F928(frame.ctx, base);
      return int32_t(frame.ctx.r3.u32) >= 0;
    }
    bool Set(uint32_t h, const std::string &name)
    {
      const uint32_t id = PropId(h, name);
      if (id == 0xFFFFFFFFu)
      {
        return false;
      }
      frame.ctx.r3.u64 = h;
      frame.ctx.r4.u64 = id;
      frame.ctx.r5.u64 = 0;
      frame.ctx.r6.u64 = val;
      sub_8277FA28(frame.ctx, base);
      return int32_t(frame.ctx.r3.u32) >= 0;
    }
    std::u16string GetText(uint32_t h, const std::string &name)
    {
      std::u16string text;
      if (Get(h, name) && Load32(base, val) == kPropString)
      {
        text = ReadWide(base, Load32(base, val + 4));
      }
      Clear();
      return text;
    }
    std::string GetString(uint32_t h, const std::string &name) { return Ascii(GetText(h, name)); }
    bool SetText(uint32_t h, const std::string &name, const std::u16string &text)
    {
      const uint32_t saved = frame.ctx.r1.u32;
      frame.ctx.r3.u64 = val;
      frame.ctx.r4.u64 = PushWide(frame.ctx, base, text);
      sub_827950E0(frame.ctx, base);
      frame.ctx.r1.u32 = saved;
      const bool ok = Set(h, name);
      Clear();
      return ok;
    }
    bool SetString(uint32_t h, const std::string &name, const std::string &text) { return SetText(h, name, Wide(text)); }
    bool GetPosition(uint32_t h, float pos[3])
    {
      const bool ok = Get(h, "Position") && Load32(base, val) == kPropVector;
      for (int i = 0; ok && i < 3; ++i)
      {
        const uint32_t bits = Load32(base, val + 4 + i * 4);
        std::memcpy(&pos[i], &bits, 4);
      }
      Clear();
      return ok;
    }
    bool SetPosition(uint32_t h, const float pos[3])
    {
      std::memset(base + val, 0, kPropValSize);
      Store32(base, val, kPropVector);
      for (int i = 0; i < 3; ++i)
      {
        uint32_t bits;
        std::memcpy(&bits, &pos[i], 4);
        Store32(base, val + 4 + i * 4, bits);
      }
      return Set(h, "Position");
    }
    void Copy(uint32_t from, uint32_t to, const std::string &name)
    {
      if (Get(from, name))
      {
        Set(to, name);
      }
      Clear();
    }
    uint32_t Child(uint32_t h, bool first)
    {
      Store32(base, out, 0);
      frame.ctx.r3.u64 = h;
      frame.ctx.r4.u64 = out;
      if (first)
      {
        sub_8278A4C8(frame.ctx, base);
      }
      else
      {
        sub_8278A5B8(frame.ctx, base);
      }
      return int32_t(frame.ctx.r3.u32) < 0 ? 0 : Load32(base, out);
    }
    std::vector<uint32_t> Children(uint32_t h)
    {
      std::vector<uint32_t> list;
      for (uint32_t c = Child(h, true); c && list.size() < 256; c = Child(c, false))
      {
        list.push_back(c);
      }
      return list;
    }
    // The element with this Id in a tree (depth-first), and its parent.
    uint32_t Find(uint32_t h, const std::string &id, uint32_t &parent, int depth = 0)
    {
      for (uint32_t c : depth > 12 ? std::vector<uint32_t>{} : Children(h))
      {
        if (GetString(c, "Id") == id)
        {
          parent = h;
          return c;
        }
        if (const uint32_t found = Find(c, id, parent, depth + 1))
        {
          return found;
        }
      }
      return 0;
    }
    uint32_t Create(const std::string &cls)
    {
      const uint32_t saved = frame.ctx.r1.u32;
      Store32(base, out, 0);
      frame.ctx.r3.u64 = PushWide(frame.ctx, base, Wide(cls));
      frame.ctx.r4.u64 = out;
      sub_827865B8(frame.ctx, base);
      frame.ctx.r1.u32 = saved;
      return int32_t(frame.ctx.r3.u32) < 0 ? 0 : Load32(base, out);
    }
  };

  // MISSION MODE: the COUNTER OPERATIVE entry under CO-OPERATIVE.
  void AddButton(Xui &x, uint32_t scene)
  {
    uint32_t parent = 0;
    const uint32_t anchor = x.Find(scene, kAnchorId, parent);
    uint32_t unused = 0;
    if (!anchor || x.Find(scene, kButtonId, unused))
    {
      REXLOG_INFO("pdz_counterop: menu: MISSION MODE {}", anchor ? "already has the entry" : "has no CoOp button: no entry");
      return;
    }
    float at[3], above[3];
    const std::string up_id = x.GetString(anchor, "NavUp");
    const std::string down_id = x.GetString(anchor, "NavDown");
    uint32_t up_parent = 0;
    const uint32_t up = up_id.empty() ? 0 : x.Find(scene, up_id, up_parent);
    if (!x.GetPosition(anchor, at))
    {
      return;
    }
    float spacing = 45.0f;
    if (up && x.GetPosition(up, above) && at[1] - above[1] > 1.0f && at[1] - above[1] < 200.0f)
    {
      spacing = at[1] - above[1];
    }
    const uint32_t button = x.Create("DarkXuiValueNavButton");
    if (!button)
    {
      REXLOG_WARN("pdz_counterop: menu: couldn't create the COUNTER OPERATIVE button");
      return;
    }
    for (const char *name : kCopiedProps)
    {
      x.Copy(anchor, button, name);
    }
    std::u16string label = RareText(x.frame.ctx, x.base, kTextCounter);
    const std::u16string second = RareText(x.frame.ctx, x.base, kTextOperative);
    if (label.empty() || second.empty())
    {
      label = u"COUNTER OPERATIVE"; // only if the frontend table isn't loaded (it is in the menus)
    }
    else
    {
      label += u' ';
      label += second;
    }
    x.SetString(button, "Id", kButtonId);
    x.SetText(button, "Text", label);
    x.SetString(button, "InfoScene", kCard);
    // The column below CO-OPERATIVE moves down one row (its NavDown chain, so the footer stays put).
    std::vector<uint32_t> moved;
    std::string next = down_id;
    while (!next.empty() && next != kAnchorId && next != up_id && moved.size() < 8)
    {
      uint32_t p = 0;
      const uint32_t c = x.Find(scene, next, p);
      float pos[3];
      if (!c || std::find(moved.begin(), moved.end(), c) != moved.end() || !x.GetPosition(c, pos) || pos[1] <= at[1])
      {
        break; // the chain wrapped to the top
      }
      pos[1] += spacing;
      x.SetPosition(c, pos);
      moved.push_back(c);
      next = x.GetString(c, "NavDown");
    }
    const float pos[3] = {at[0], at[1] + spacing, at[2]};
    x.SetPosition(button, pos);
    x.SetString(button, "NavUp", kAnchorId);
    x.SetString(button, "NavDown", down_id);
    x.SetString(anchor, "NavDown", kButtonId);
    if (!down_id.empty())
    {
      uint32_t p = 0;
      const uint32_t below = x.Find(scene, down_id, p);
      if (below && x.GetString(below, "NavUp") == kAnchorId)
      {
        x.SetString(below, "NavUp", kButtonId);
      }
    }
    x.frame.ctx.r3.u64 = parent;
    x.frame.ctx.r4.u64 = button;
    sub_8278A3B8(x.frame.ctx, x.base);
    x.frame.ctx.r3.u64 = button;
    x.frame.ctx.r4.u64 = 0;
    sub_8278E870(x.frame.ctx, x.base);
    REXLOG_INFO("pdz_counterop: menu: '{}' added under CO-OPERATIVE at y {:.0f} (row {:.0f}, {} row(s) moved, game value "
                "'{}', press path '{}')",
                Ascii(label), pos[1], spacing, moved.size(), x.GetString(button, "GameValue"),
                x.GetString(button, "PressPath"));
  }

  // Every element whose Id starts with "header" and whose text starts with Rare's CO-OP: COUNTER-OP instead.
  int Headers(Xui &x, uint32_t h, const std::u16string &from, const std::u16string &to, int depth = 0)
  {
    int done = 0;
    for (uint32_t c : depth > 12 ? std::vector<uint32_t>{} : x.Children(h))
    {
      if (x.GetString(c, "Id").rfind("header", 0) == 0)
      {
        const std::u16string text = x.GetText(c, "Text");
        if (!from.empty() && text.rfind(from, 0) == 0)
        {
          done += x.SetText(c, "Text", to + text.substr(from.size()));
        }
      }
      done += Headers(x, c, from, to, depth + 1);
    }
    return done;
  }

  std::u16string Trim(std::u16string s)
  {
    while (!s.empty() && s.back() == u' ')
    {
      s.pop_back();
    }
    return s;
  }

  // The level name (Rare's entry name, e.g. "...holo...") for a level id.
  std::string LevelName(const uint8_t *base, int32_t id)
  {
    const uint32_t size = Load32(base, kLevelList + 20), table = Load32(base, kLevelList + 24);
    const uint32_t entries = Load32(base, kLevelList + 8);
    if (id < 0 || !Ptr(table) || !Ptr(entries) || uint32_t(id) >= size)
    {
      return {};
    }
    const int32_t index = int32_t(Load32(base, table + uint32_t(id) * 4));
    const uint32_t entry = index >= 0 && index < 4096 ? Load32(base, entries + uint32_t(index) * 4) : 0;
    const uint32_t name = Ptr(entry) ? Load32(base, entry) : 0;
    std::string text;
    for (uint32_t i = 0; Ptr(name) && i < 64 && base[name + i]; ++i)
    {
      text += char(std::tolower(base[name + i]));
    }
    return text;
  }

  // Datacore (holo) and Arena (cave) place no guards (study section 4): refused unless pdz_counterop_clones plays
  // them with their runtime clones (counter_op.cpp).
  bool Unsupported(const std::string &level)
  {
    return !PdzCounterOpClonesOn() &&
           (level.find("holo") != std::string::npos || level.find("cave") != std::string::npos);
  }

  // Rare's message box with our text and Rare's "Information" caption (82400DB0's way: two string objects).
  void MessageBox(PPCContext &ctx, uint8_t *base, const std::string &text)
  {
    rex::CallFrame frame(ctx);
    const uint32_t message = rex::ppc::stack_push(frame.ctx, base, std::string(12, '\0').data(), 12);
    const uint32_t caption = rex::ppc::stack_push(frame.ctx, base, std::string(12, '\0').data(), 12);
    const std::u16string wide = Wide(text);
    const uint32_t chars = PushWide(frame.ctx, base, wide);
    frame.ctx.r3.u64 = message;
    sub_8262E740(frame.ctx, base);
    frame.ctx.r3.u64 = message;
    frame.ctx.r4.u64 = chars;
    frame.ctx.r5.u64 = wide.size();
    frame.ctx.r6.u64 = 1;
    sub_8262F330(frame.ctx, base);
    const std::u16string info = RareText(frame.ctx, base, kTextInformation);
    const uint32_t info_chars = PushWide(frame.ctx, base, info);
    frame.ctx.r3.u64 = caption;
    sub_8262E740(frame.ctx, base);
    frame.ctx.r3.u64 = caption;
    frame.ctx.r4.u64 = info_chars;
    frame.ctx.r5.u64 = info.size();
    frame.ctx.r6.u64 = 1;
    sub_8262F330(frame.ctx, base);
    const uint8_t user = base[kFrontendUser + 20];
    frame.ctx.r3.u64 = user == 255 ? 0 : user;
    frame.ctx.r4.u64 = message;
    frame.ctx.r5.u64 = caption;
    frame.ctx.r6.u64 = 0;
    sub_82400FB0(frame.ctx, base);
    for (const uint32_t object : {message, caption})
    {
      const uint32_t buffer = Load32(base, object);
      if (buffer && Load32(base, object + 8) != 0xFFFFFFFFu)
      {
        frame.ctx.r3.u64 = buffer;
        sub_8262ED18(frame.ctx, base);
      }
    }
  }

  void Disarm(const char *why)
  {
    if (g_armed)
    {
      g_armed = false;
      REXLOG_INFO("pdz_counterop: menu: Counter-Operative off ({})", why);
    }
  }
} // namespace

// counter_op.cpp's On(): the mode chosen in the menu (and not left since).
bool PdzCounterOpArmed()
{
  return g_armed && Menu();
}

// mp_music.cpp's XuiSceneCreate hook: whether it should read the scene path for us.
bool PdzCounterOpSceneWanted()
{
  return Menu();
}

// Before Rare's XuiSceneCreate (path lower case): true = refuse (the scene is not created; *out = 0, E_FAIL).
bool PdzCounterOpRefuseScene(PPCContext &caller, uint8_t *base, const std::string &path, uint32_t out)
{
  PPCContext ctx = caller; // our calls never touch the caller's registers
  if (!PdzCounterOpArmed() || path.find("frontend_coop_local_difficulty") == std::string::npos ||
      !Ptr(g_mission_list))
  {
    return false;
  }
  int32_t level = -1;
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = g_mission_list;
    frame.ctx.r4.u64 = 0;
    sub_823AB3C8(frame.ctx, base);
    const int32_t index = int32_t(frame.ctx.r3.u32);
    frame.ctx.r3.u64 = uint32_t(index);
    sub_8227AD78(frame.ctx, base);
    level = index >= 0 ? int32_t(frame.ctx.r3.u32) : -1;
  }
  const std::string name = LevelName(base, level);
  if (!Unsupported(name))
  {
    REXLOG_INFO("pdz_counterop: menu: mission {} ('{}') chosen for Counter-Operative", level, name);
    return false;
  }
  if (Ptr(out))
  {
    Store32(base, out, 0);
  }
  REXLOG_INFO("pdz_counterop: menu: mission {} ('{}') refused: no guards to play (difficulty screen not opened)", level,
              name);
  MessageBox(ctx, base, "Counter-Operative needs a mission with guards. Datacore and Arena can be played in "
                        "Co-operative only.");
  return true;
}

// Instead of Rare's XuiSceneCreate (path lower case): the counter-op card's request is created from Rare's co-op card
// (the same file name with "cooperative" for "counteroperative"); true = done here (r3 = Rare's result).
bool PdzCounterOpCreateInstead(PPCContext &ctx, uint8_t *base, const std::string &path)
{
  g_card_from_coop = false;
  const size_t at = path.find(kCardFrom);
  if (!Menu() || !REXCVAR_GET(pdz_counterop_menu_coop_card) || path.find("info_counteroperative") == std::string::npos ||
      at == std::string::npos || !Ptr(ctx.r4.u32))
  {
    return false;
  }
  std::u16string wide = ReadWide(base, ctx.r4.u32, 260);
  if (wide.size() < at + std::strlen(kCardFrom))
  {
    return false;
  }
  wide.replace(at, std::strlen(kCardFrom), Wide(kCardTo));
  {
    // rex::CallFrame copies only r1 and r13 (not zero-initialised): XuiSceneCreate's other arguments must be carried
    // over by hand. Without r6 (the caller's &scene) its first store, *out = 0, went to garbage (the 8 Nov 23:36
    // scripted menu run: a write to guest 0x4FEC7370 the moment the cursor reached the entry).
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = ctx.r3.u64; // base path
    frame.ctx.r5.u64 = ctx.r5.u64; // init data
    frame.ctx.r6.u64 = ctx.r6.u64; // &scene
    frame.ctx.r4.u64 = PushWide(frame.ctx, base, wide);
    __imp__sub_8278F550(frame.ctx, base);
    ctx.r3.u64 = frame.ctx.r3.u64;
  }
  g_card_from_coop = int32_t(ctx.r3.u32) >= 0;
  if (++g_card_lines <= 3)
  {
    REXLOG_INFO("pdz_counterop: menu: card: '{}' asked; created from Rare's co-op card '{}' ({}: result {:08X})", path,
                Ascii(wide), g_card_from_coop ? "ok" : "FAILED", ctx.r3.u32);
  }
  if (!g_card_from_coop)
  {
    __imp__sub_8278F550(ctx, base); // Rare's own counter-op card, as before
  }
  return true;
}

// After Rare's XuiSceneCreate succeeded.
void PdzCounterOpSceneCreated(PPCContext &caller, uint8_t *base, const std::string &path, uint32_t scene)
{
  PPCContext ctx = caller;
  if (!Menu() || !Ptr(scene))
  {
    return;
  }
  if (path.find("frontend_menu_storymode") != std::string::npos)
  {
    Xui x(ctx, base);
    AddButton(x, scene);
    return;
  }
  if (path.find("info_counteroperative") != std::string::npos && g_card_from_coop)
  {
    // Rare's co-op card for the counter-op entry: Rare's COUNTER OPERATIVE title and line 146; its layout (the
    // description bottom right) and its pictures stay Rare's co-op card's.
    g_card_from_coop = false;
    Xui x(ctx, base);
    uint32_t parent = 0;
    std::u16string title = RareText(x.frame.ctx, base, kTextCounter);
    const std::u16string second = RareText(x.frame.ctx, base, kTextOperative);
    title = title.empty() || second.empty() ? std::u16string(u"COUNTER OPERATIVE") : title + u' ' + second;
    const std::u16string line = RareText(x.frame.ctx, base, kTextCardLine);
    const uint32_t name = x.Find(scene, "level name", parent);
    const uint32_t text = x.Find(scene, "location text", parent);
    const bool name_set = name && x.SetText(name, "Text", title);
    const bool line_set = text && !line.empty() && x.SetText(text, "Text", line);
    float group[3] = {}, at[3] = {};
    const uint32_t text3 = x.Find(scene, "text3", parent);
    const bool have = text3 && x.GetPosition(text3, group) && text && x.GetPosition(text, at);
    if (g_card_lines <= 3)
    {
      REXLOG_INFO("pdz_counterop: menu: card: title {}, line {}; description at {} (Rare's co-op layout)",
                  name_set ? "set" : "NOT set", line_set ? "set" : "NOT set",
                  have ? fmt::format("group 'text3' ({:.0f}, {:.0f}) + ({:.0f}, {:.0f})", group[0], group[1], at[0], at[1])
                       : std::string("?"));
    }
    return;
  }
  if (path.find("info_counteroperative") != std::string::npos)
  {
    // Rare's card: its "Explanation here" placeholder becomes Rare's own line 146; "pic goes here" goes.
    Xui x(ctx, base);
    uint32_t parent = 0;
    const std::u16string line = RareText(x.frame.ctx, base, kTextCardLine);
    if (const uint32_t text = x.Find(scene, "location text", parent); text && !line.empty())
    {
      x.SetText(text, "Text", line);
    }
    if (const uint32_t pic = x.Find(scene, "XuiText1", parent))
    {
      x.SetString(pic, "Text", "");
    }
    return;
  }
  if (PdzCounterOpArmed() && path.find("frontend_coop_") != std::string::npos)
  {
    Xui x(ctx, base);
    const std::u16string from = Trim(RareText(x.frame.ctx, base, kTextCoOp));
    const std::u16string to = Trim(RareText(x.frame.ctx, base, kTextCounterOp));
    if (!to.empty())
    {
      Headers(x, scene, from, to);
    }
    return;
  }
  if (path.find("ingame_large_solo_debriefing") != std::string::npos && PdzCounterOpPlayed())
  {
    // Who won: the agent's mission result, from the counter-op side's name.
    Xui x(ctx, base);
    uint32_t parent = 0;
    const uint32_t header = x.Find(scene, "header2", parent);
    const uint8_t state = base[kLevelState];
    const uint32_t result = (state & 0x1) ? kTextCompleted : (state & 0x2) ? kTextFailed : 0;
    std::u16string text = result ? RareText(x.frame.ctx, base, result) : std::u16string();
    const std::u16string side = Trim(RareText(x.frame.ctx, base, kTextCounterOp)); // empty in a level if unloaded
    if (header && !text.empty())
    {
      if (!side.empty())
      {
        text = side + u"\r\n" + text;
      }
      x.SetText(header, "Text", text);
    }
    REXLOG_INFO("pdz_counterop: end: level state {:02X}: the agent {} (debriefing header {})", state,
                (state & 0x1) ? "completed the mission" : (state & 0x2) ? "is down: the counter-op wins" : "left",
                header && !text.empty() ? "set" : "left as Rare's");
  }
}

// DarkXuiValueNavButton press: CounterOp arms the mode, any other game-type press disarms it; a network connection
// press (SYSTEM LINK / XBOX LIVE) disarms it too (counter-op is local split screen only).
REX_HOOK_RAW(sub_823FF4C8)
{
  const uint32_t self = ctx.r3.u32;
  const uint32_t value_id = Ptr(self) ? Load32(base, self + 8) : 0;
  if (Menu() && Ptr(self) && (value_id == kGameTypeValue || value_id == kConnectionValue))
  {
    std::string id;
    {
      PPCContext copy = ctx; // Rare's call below gets its registers untouched
      Xui x(copy, base);
      id = x.GetString(Load32(base, self + 4), "Id");
    }
    if (value_id == kGameTypeValue)
    {
      if (id == kButtonId)
      {
        if (!g_armed)
        {
          REXLOG_INFO("pdz_counterop: menu: Counter-Operative chosen (MISSION MODE): the next local co-op games are "
                      "counter-op");
        }
        g_armed = true;
      }
      else
      {
        Disarm(id.empty() ? "another game type" : id.c_str());
      }
    }
  }
  __imp__sub_823FF4C8(ctx, base);
  if (g_armed && value_id == kConnectionValue)
  {
    const uint32_t so = Load32(base, kGame);
    if (Ptr(so) && Load32(base, so + 32) != 0)
    {
      Disarm("a network connection was chosen");
    }
  }
}

// MissionList: its handle, for the difficulty gate above (82402790 reads the choice the same way).
REX_HOOK_RAW(sub_82402318)
{
  if (Ptr(ctx.r3.u32))
  {
    g_mission_list = Load32(base, ctx.r3.u32 + 4);
  }
  __imp__sub_82402318(ctx, base);
}
