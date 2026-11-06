// The official map packs on the launch executable (pdz_dlc_maps, default on,
// restart to apply; with no pack installed nothing below is active): Map Pack
// One (Trench, Rooftops, Plaza, GasPlant) and Map
// Pack Two (Facility, Ruin). The launch game has no DLC code at all; Title
// Update 3 added it. This ports what the maps need from TU3, alongside the
// launch code. See docs/MAP_PACKS.md for the evidence and how to install.
//
// TU3 addresses are T, launch addresses plain.
// - Mounting (T 8250FF08 UpdateDLCState): TU3 enumerates marketplace content
//   (XContentCreateEnumerator, type 2) and mounts each pack that has
//   \pdz\dlc_contents as CONTENT<user><n>:. We list the pack folders under
//   <user data>\0000000000000000\4D5307D3\00000002\ (shared by every local
//   profile) and mount them read-only as CONTENT0<n>: the way the SDK's
//   XamContentCreate does. The title update's own data lives on UPDATE:; we
//   mount ...\000B0000\<folder>\ there when it is installed (pack one's map
//   names are in its frontendTU1 strings).
// - File lookups (T 821F6040 replaces sub_821EEE60): each pack, then UPDATE:,
//   then the game, each as the plain path and the current / default language
//   variants (sub_82394750 / sub_82394810). A pack path is CONTENT0<n>:\ plus
//   the game path after its drive (pdz\ASSETS\...).
// - Paths (T 82225280 replaces sub_8221DFF0): a path that starts with
//   UPDATE:\ or CONTENT is absolute, like game:\ and X:\ (otherwise the game
//   puts its root in front).
// - Audio: TU3 sends wavebanks and cue lists through the same lookup (T
//   821F9888, 822027C8), replacing the launch wavebank loader's language
//   fallback. We keep the launch loaders as they are and add the pack lookup
//   underneath: a file under ASSETS\audio\ that CreateFileA (sub_82AC3DA8)
//   can't find in the game is taken from a pack that has it.
// - Manifest: \pdz\dlc_contents\default.bin, a compiled level list that TU3
//   loads as asset aid_dlc_info (a 'dlc' asset kind launch lacks). We read it
//   on the host. Records (0x24 bytes, offsets relative to the record): +0
//   level name, +4 id, +5 kind; the first also has +8 / +0x10 the general /
//   frontend string tables with their slots at +0xC / +0x14, +0x18 the
//   package base name and +0x1C the cue list.
// - Level list merge (T 8227A648): after the game builds its list
//   (sub_82272A28, global 82D6BAC8) we append each level (8-byte entry: name,
//   id, kind, like TU3's copies) with its leveldata and extra record, loading
//   <package>System first (T 82510300) and <package>Frontend / Frontend_loc
//   (T 82510468). The lockoffs (823377B8, which TU3 re-runs after its merge)
//   and the multiplayer menus read the list afterwards, so they pick the maps
//   up as they are. TU3 also keeps a per-user ownership bit; we don't.
// - Strings (T 8239BA70, 8239B6D8): each pack's general / frontend tables and
//   the title update's (aid_xml_dlc_locassets: generalTU1 slot 8, frontendTU1
//   slot 9) after the game's own (sub_82393540). A string id is
//   slot << 16 | index; launch has 7 slots (82D6F0C0), TU3 38, so slots 7+
//   live in our own array and the two lookups (sub_82393AD0, sub_82393B30)
//   read them from there.
// - Cue lists (T 822027C8): each pack's, right after the game's own
//   (sub_821F22D0, inside the audio init), with the launch loader (sub_821FA770).
// - Prop templates: TU3's master list (xml\proptemplate\masterlist, in its
//   1726605008.pkg) is launch's plus 9 entries: the packs' Facility doors ->
//   aid_xml_proptemplate_door_facupdown and teleporters ->
//   aid_xml_proptemplate_electrical_teleport (8, added here, sub_8251ADC0), and
//   one title update hit-fix prop (left out). Without them those props fall
//   back to no template (no door air hiss, no teleporting).
// - Map pictures: TU3 shows a level's info scene (leveldata +0x9C) in a new
//   DarkXUI control (T 823FFE58); here that scene becomes the launch map
//   tab scene's missing tab (see the sub_8277ADB8 hook).
// - Facility / Ruin rules (pdz_fix_tu_pack_map_rules, U22; T 823BA378,
//   823BE050): with Facility or Ruin chosen, the scenario, DarkOps scenario
//   and max players options offer TU3's lists ([0, 1], [0, 2], 2-8; the four
//   list readers sub_823B2C28 / 2D08 / 2D90 / 31A0 answer for options 61 / 62
//   / 5), and the Random map (sub_823B5288) picks from every map, except
//   Facility / Ruin when the scenario isn't one of theirs. Launch's picker
//   stops at position 8 (823B52BC), so it never chose GasPlant, Facility or
//   Ruin.
// - Title update data (pdz_fix_tu_township U12, pdz_fix_tu_mp_hitfix U18):
//   TU3 loads its package aid_darkpackage_titleupdate (1726605008.pkg) at
//   boot before every level package (T 821411A8), so its records win over the
//   level packages' (registration sub_821F0410 skips a name already
//   registered). Ported: the package is loaded from UPDATE: right after the
//   level list (in the sub_82272A28 hook), but only the hit-fix records are
//   registered (midasm hook before 821F04D4: anything else counts as already
//   registered and is skipped): Township's proplist, static and hit-fix prop
//   plus the 7 Township replacement textures, Chinatown's and Desert's
//   proplists, statics and hit-fix props, and Desert's hit-fix template entry
//   (thing -> example, the 9th TU3 master-list entry). The mine / ammo /
//   explosion volume / route records (U20, U27) are left out. The textures
//   are swapped into the background's two instances after the background
//   setup on level load and restart when the level is Township (T 82276D78;
//   midasm hooks before 8227142C / 82270308).
// Not ported: TU3's new control itself and everything else in the update.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/stack.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

#include "../game_cvars.h"
#include "../local_profiles.h"
#include "mp_music.h"


REX_EXTERN(__imp__sub_821EEE60);
REX_EXTERN(__imp__sub_82AC3DA8);
REX_EXTERN(__imp__sub_8221DFF0);
REX_EXTERN(__imp__sub_82272A28);
REX_EXTERN(__imp__sub_821F22D0);
REX_EXTERN(__imp__sub_821F0E58);
REX_EXTERN(__imp__sub_821F2050);
REX_EXTERN(__imp__sub_821F21A8);
REX_EXTERN(__imp__sub_821F1888);
REX_EXTERN(sub_8274DB40);
REX_EXTERN(__imp__sub_8251ADC0);
REX_EXTERN(__imp__sub_82623748);
REX_EXTERN(sub_826237E0);
REX_EXTERN(__imp__sub_82393540);
REX_EXTERN(__imp__sub_82393AD0);
REX_EXTERN(__imp__sub_82393B30);
REX_EXTERN(sub_82394750);
REX_EXTERN(sub_82394810);
REX_EXTERN(sub_821ED078);
REX_EXTERN(sub_826478B8);
REX_EXTERN(sub_82393938);
REX_EXTERN(sub_821FA770);
REX_EXTERN(sub_8275A400);
REX_EXTERN(sub_8275A870);
REX_EXTERN(sub_823962C0);
REX_EXTERN(__imp__sub_823B5288);
REX_EXTERN(__imp__sub_823B2C28);
REX_EXTERN(__imp__sub_823B2D08);
REX_EXTERN(__imp__sub_823B2D90);
REX_EXTERN(__imp__sub_823B31A0);
REX_EXTERN(sub_8242EC78);
REX_EXTERN(sub_8242EDC8);
REX_EXTERN(sub_8242ED60);
REX_EXTERN(sub_826F28C0);
REX_EXTERN(sub_823B0388);
REX_EXTERN(sub_823AFC00);
REX_EXTERN(sub_824BF828);

namespace
{
  // Launch globals.
  constexpr uint32_t kGameRoot = 0x82E67798;     // "game:\..." prefix of ASSETS\ paths
  constexpr uint32_t kLevelList = 0x82D6BAC8;    // count, entries, leveldata, extra, 3 x (size, id table)
  constexpr uint32_t kLoadPackages = 0x82E2E4C2; // byte: startup loads packages by name
  constexpr uint32_t kHeapContext = 0x82D6B2DC;  // -> +24 byte on, +28 heap (set around loads)
  constexpr uint32_t kGameStringSlots = 7;       // 82D6F0C0, 12 bytes each
  constexpr uint32_t kStringSlots = 38;          // as in TU3
  constexpr uint32_t kSlotSize = 12;
  constexpr uint32_t kLevelStringSlot = 7;       // TU3: the pack map's own strings
  constexpr uint32_t kGameState = 0x82D6B2B0;    // -> game state (+25 map id, +36 mode)
  constexpr uint32_t kMaxPacks = 10;             // TU3 enumerates 10 items
  constexpr uint32_t kAudioManager = 0x82D6B2D0;  // -> audio manager
  constexpr uint32_t kBankCount = 0x82D3351C;    // sound banks registered
  constexpr uint32_t kBankSlots = 84;            // 1020 bytes / 12, numbered from 1
  constexpr uint32_t kManagerSize = 1084;        // up to the next object in its block
  constexpr uint32_t kBankFields = 1076;         // fields after the table (+1076, +1080)
  constexpr uint32_t kFirstMovedBank = 86;       // bank 85 would overlap +1076..+1087
  constexpr uint32_t kMovedSize = 0x1000;        // relocated manager: 56 + 12 x 335 banks
  uint32_t g_bank_slots = kBankSlots;            // banks that fit (more once relocated)
  constexpr const char *kNoName = "_noname_";
  // Title Update 3's aid_xml_dlc_locassets (393969562.pkg).
  constexpr struct
  {
    const char *name;
    uint32_t slot;
  } kUpdateStrings[] = {{"aid_strings_text_generalTU1", 8}, {"aid_strings_text_frontendTU1", 9}};

  struct Level
  {
    std::string name;
    uint8_t id = 0;
    uint8_t kind = 0;
  };

  struct Pack
  {
    std::string root; // "CONTENT00"
    std::filesystem::path host;
    std::vector<Level> levels;
    std::string general, frontend, package, cuelist;
    uint32_t general_slot = 0, frontend_slot = 0;
  };

  // From TU3's prop template master list: the entries for the packs' props.
  constexpr struct
  {
    const char *model;
    const char *propTemplate;
  } kPackPropTemplates[] = {
      {"aid_darkcharacter_xenon_props_door_facility_labsingledoor_updown", "aid_xml_proptemplate_door_facupdown"},
      {"aid_darkcharacter_xenon_props_door_facility_singledoor_updown", "aid_xml_proptemplate_door_facupdown"},
      {"aid_darkcharacter_xenon_props_door_facility_doubledoor_updown", "aid_xml_proptemplate_door_facupdown"},
      {"aid_darkcharacter_xenon_props_door_facility_double", "aid_xml_proptemplate_door_facupdown"},
      {"aid_darkcharacter_xenon_props_multiplayer_props_teleport_gasplant", "aid_xml_proptemplate_electrical_teleport"},
      {"aid_darkcharacter_xenon_props_multiplayer_props_teleport_plaza", "aid_xml_proptemplate_electrical_teleport"},
      {"aid_darkcharacter_xenon_props_multiplayer_props_teleport_roof", "aid_xml_proptemplate_electrical_teleport"},
      {"aid_darkcharacter_xenon_props_multiplayer_props_teleport_trench", "aid_xml_proptemplate_electrical_teleport"},
  };
  constexpr uint32_t kTemplateMapInitReturn = 0x8251AE18; // sub_82623748 from sub_8251ADC0

  // TU3's 9th master-list entry: Desert's hit-fix prop (U18).
  constexpr const char *kThingModel = "aid_darkcharacter_xenon_props_multiplayer_props_thing";
  constexpr const char *kThingTemplate = "aid_xml_proptemplate_example";

  // Title update data (U12 / U18).
  constexpr const char *kTitleUpdatePackage = "aid_darkpackage_titleupdate";
  constexpr const char *kTitleUpdateFile = "PackageData\\1726605008.pkg";
  constexpr const char *kTownshipBackground = "aid_darkbackground_single_town";
  constexpr uint32_t kBackgroundManager = 0x82D6B2BC; // -> +20 / +24 background instances
  constexpr const char *kTownshipTextures[] = {
      "aid_texture_replacement_township1", "aid_texture_replacement_tonwship2", // Rare's spelling
      "aid_texture_replacement_township3", "aid_texture_replacement_township4",
      "aid_texture_replacement_township5", "aid_texture_replacement_township6",
      "aid_texture_replacement_township7"};
  // Instance fields T 82276D78 writes them to, in order.
  constexpr uint32_t kTownshipTextureFields[] = {1052, 1056, 1060, 1028, 920, 916, 1032};
  std::atomic<bool> g_tu_registering{false};
  bool g_tu_township = false;  // records taken at boot
  bool g_tu_mp_hitfix = false;
  bool g_tu_thing = false;     // Desert's hit-fix template is added
  uint32_t g_tu_taken = 0, g_tu_left = 0, g_tu_present = 0;
  uint32_t g_township_textures[std::size(kTownshipTextures)] = {};

  std::vector<Pack> g_packs;
  std::vector<std::string> g_roots; // packs, then UPDATE
  bool g_active = false;
  bool g_update_mounted = false;
  bool g_lookups = false; // pack / UPDATE: file lookups (packs, or title update data)
  uint32_t g_slots = 0;             // guest array of kStringSlots string slots
  uint32_t g_first_pack_level = 0;  // level list index of the first pack level

  bool Log()
  {
    return REXCVAR_GET(pdz_dlc_log);
  }

  uint32_t LoadBe32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  void StoreBe32(uint8_t *base, uint32_t address, uint32_t value)
  {
    uint8_t *p = base + address;
    p[0] = uint8_t(value >> 24);
    p[1] = uint8_t(value >> 16);
    p[2] = uint8_t(value >> 8);
    p[3] = uint8_t(value);
  }

  uint32_t Be32(const std::vector<uint8_t> &data, size_t offset)
  {
    if (offset + 4 > data.size())
    {
      return 0;
    }
    return (uint32_t(data[offset]) << 24) | (uint32_t(data[offset + 1]) << 16) |
           (uint32_t(data[offset + 2]) << 8) | data[offset + 3];
  }

  std::string ReadString(const uint8_t *base, uint32_t address, size_t max = 260)
  {
    std::string text;
    if (!address)
    {
      return text;
    }
    for (size_t i = 0; i < max && base[address + i]; ++i)
    {
      text += char(base[address + i]);
    }
    return text;
  }

  void WriteString(uint8_t *base, uint32_t address, const std::string &text, size_t max = 260)
  {
    const size_t count = std::min(text.size(), max - 1);
    std::memcpy(base + address, text.data(), count);
    base[address + count] = 0;
  }

  // Guest heap copy of a string, kept for good (the game may keep the pointer).
  uint32_t GuestString(uint8_t *base, const std::string &text)
  {
    const uint32_t address = REX_KERNEL_MEMORY()->SystemHeapAlloc(uint32_t(text.size() + 1));
    if (address)
    {
      WriteString(base, address, text, text.size() + 1);
    }
    return address;
  }

  uint32_t GuestAlloc(uint8_t *base, uint32_t size)
  {
    const uint32_t address = REX_KERNEL_MEMORY()->SystemHeapAlloc(size);
    if (address)
    {
      std::memset(base + address, 0, size);
    }
    return address;
  }

  bool Exists(const std::string &path)
  {
    return REX_KERNEL_FS()->ResolvePath(path) != nullptr;
  }

  // The part of a game path after its drive: "game:\pdz\ASSETS\x" -> "pdz\ASSETS\x".
  std::string Tail(const std::string &path)
  {
    const size_t slash = path.find('\\');
    return slash == std::string::npos ? path : path.substr(slash + 1);
  }

  std::string Lower(std::string text)
  {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
  }

  // The manifest strings at a record field (offset relative to the record).
  std::string FieldString(const std::vector<uint8_t> &data, size_t record, size_t field)
  {
    const size_t at = record + Be32(data, record + field);
    std::string text;
    for (size_t i = at; i < data.size() && data[i]; ++i)
    {
      text += char(data[i]);
    }
    return text;
  }

  // \pdz\dlc_contents\default.bin: 0x24-byte header (+0x20 record count), the
  // source path, then per record: 0xC, data size, total size, the data and its
  // strings, and a list of pointer fields ending in -1.
  bool ParseManifest(const std::vector<uint8_t> &data, Pack &pack)
  {
    const uint32_t count = Be32(data, 0x20);
    size_t at = 0x24;
    while (at < data.size() && data[at])
    {
      ++at;
    }
    at = (at + 4) & ~size_t(3);
    for (uint32_t i = 0; i < count; ++i)
    {
      while (at + 12 <= data.size() && Be32(data, at) != 0xC)
      {
        at += 4;
      }
      const uint32_t size = Be32(data, at + 4);
      const size_t record = at + 12;
      if (size < 0x24 || record + size > data.size())
      {
        return false;
      }
      Level level;
      level.name = FieldString(data, record, 0);
      level.id = data[record + 4];
      level.kind = data[record + 5];
      if (level.name.empty())
      {
        return false;
      }
      pack.levels.push_back(level);
      if (i == 0)
      {
        pack.general = FieldString(data, record, 0x08);
        pack.general_slot = data[record + 0x0C];
        pack.frontend = FieldString(data, record, 0x10);
        pack.frontend_slot = data[record + 0x14];
        pack.package = FieldString(data, record, 0x18);
        pack.cuelist = FieldString(data, record, 0x1C);
      }
      // Next record: after the -1 that ends this one's pointer list.
      at = record + size;
      while (at + 4 <= data.size() && Be32(data, at) != 0xFFFFFFFFu)
      {
        at += 4;
      }
      at += 4;
    }
    return !pack.levels.empty();
  }

  bool Mount(const std::string &root, const std::filesystem::path &host, uint32_t index)
  {
    auto *fs = REX_KERNEL_FS();
    const std::string device = "\\Device\\PdzContent\\" + std::to_string(index) + "\\";
    auto entry = std::make_unique<rex::filesystem::HostPathDevice>(device, host, true);
    if (!entry->Initialize() || !fs->RegisterDevice(std::move(entry)))
    {
      return false;
    }
    return fs->RegisterSymbolicLink(root + ":", device);
  }

  std::filesystem::path FindFolder(const std::filesystem::path &dir, const std::string &name)
  {
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
    {
      if (Lower(entry.path().filename().string()) == name)
      {
        return entry.path();
      }
    }
    return {};
  }

  // Lists and mounts the packs (with packs = false only the title update's
  // data), and the title update's data if installed.
  void MountPacks(bool packs)
  {
    const std::filesystem::path title = local_profiles::BaseDataRoot() / "0000000000000000" / "4D5307D3";
    std::vector<std::filesystem::path> folders;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(title / "00000002", ec))
    {
      if (packs && entry.is_directory(ec))
      {
        folders.push_back(entry.path());
      }
    }
    std::sort(folders.begin(), folders.end());
    for (const auto &folder : folders)
    {
      if (g_packs.size() >= kMaxPacks)
      {
        break;
      }
      const std::filesystem::path pdz = FindFolder(folder, "pdz");
      const std::filesystem::path contents = pdz.empty() ? pdz : FindFolder(pdz, "dlc_contents");
      const std::filesystem::path manifest = contents.empty() ? contents : contents / "default.bin";
      if (manifest.empty() || !std::filesystem::exists(manifest, ec))
      {
        continue;
      }
      std::ifstream file(manifest, std::ios::binary);
      std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      Pack pack;
      pack.host = folder;
      pack.root = "CONTENT0" + std::to_string(g_packs.size());
      if (!ParseManifest(data, pack))
      {
        REXLOG_WARN("pdz_dlc: {} has an unreadable dlc_contents\\default.bin, skipped", folder.string());
        continue;
      }
      if (!Mount(pack.root, folder, uint32_t(g_packs.size())))
      {
        REXLOG_WARN("pdz_dlc: couldn't mount {}", folder.string());
        continue;
      }
      if (Log())
      {
        std::string names;
        for (const Level &level : pack.levels)
        {
          names += fmt::format(" {}({:02X})", level.name, level.id);
        }
        REXLOG_INFO("pdz_dlc: {}: = {} ({}, strings {}@{} {}@{}, cues {}) levels:{}", pack.root,
                    folder.string(), pack.package, pack.general, pack.general_slot, pack.frontend,
                    pack.frontend_slot, pack.cuelist, names);
      }
      g_roots.push_back(pack.root);
      g_packs.push_back(std::move(pack));
    }
    for (const auto &entry : std::filesystem::directory_iterator(title / "000B0000", ec))
    {
      if (entry.is_directory(ec) && !FindFolder(entry.path(), "pdz").empty())
      {
        if (Mount("UPDATE", entry.path(), kMaxPacks))
        {
          g_roots.push_back("UPDATE");
          g_update_mounted = true;
          if (Log())
          {
            REXLOG_INFO("pdz_dlc: UPDATE: = {}", entry.path().string());
          }
        }
        break;
      }
    }
    if (packs && g_packs.empty())
    {
      REXLOG_INFO("pdz_dlc: no map pack installed in {}; the original map list is used",
                  (title / "00000002").string());
    }
  }

  // Asset by name (sub_821ED078) -> its data (sub_826478B8), as the level list
  // builder does it. name is a guest string.
  uint32_t LoadAsset(PPCContext &ctx, uint8_t *base, uint32_t name, uint32_t *handle_out = nullptr,
                     bool data = true)
  {
    uint32_t handle;
    {
      rex::CallFrame frame(ctx);
      const uint32_t out = rex::ppc::stack_push<uint32_t>(frame.ctx, base, 0);
      frame.ctx.r3.u64 = name;
      frame.ctx.r4.u64 = out;
      sub_821ED078(frame.ctx, base);
      handle = LoadBe32(base, out);
    }
    if (handle_out)
    {
      *handle_out = handle;
    }
    if (!handle || !data)
    {
      return 0;
    }
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = handle;
    sub_826478B8(frame.ctx, base);
    return frame.ctx.r3.u32;
  }

  // The thread's memory category (sub_823962C0(&old, category)); returns the
  // previous one.
  uint32_t SetCategory(PPCContext &ctx, uint8_t *base, uint32_t category)
  {
    rex::CallFrame frame(ctx);
    const uint32_t old = rex::ppc::stack_push<uint32_t>(frame.ctx, base, 0);
    frame.ctx.r3.u64 = old;
    frame.ctx.r4.u64 = category;
    sub_823962C0(frame.ctx, base);
    return LoadBe32(base, old);
  }

  // Loads a package by name, under the heap and memory category TU3 uses for
  // it (T 82510300: System 3 / 3, T 82510468: Frontend 8 / 4 with +32 clear).
  void LoadPackage(PPCContext &ctx, uint8_t *base, const std::string &name, uint32_t heap_id, uint32_t category)
  {
    const uint32_t heap = LoadBe32(base, kHeapContext);
    const uint32_t old_category = SetCategory(ctx, base, category);
    const uint32_t saved = LoadBe32(base, heap + 32);
    base[heap + 24] = 1;
    StoreBe32(base, heap + 28, heap_id);
    if (heap_id == 8)
    {
      StoreBe32(base, heap + 32, 0);
    }
    uint32_t handle = 0;
    LoadAsset(ctx, base, GuestString(base, name), &handle);
    StoreBe32(base, heap + 32, saved);
    base[heap + 24] = 0;
    SetCategory(ctx, base, old_category);
    if (Log())
    {
      REXLOG_INFO("pdz_dlc: package {}: {}", name, handle ? "loaded" : "not found");
    }
  }

  // A copy of a guest array of count words with room for extra more.
  uint32_t GrowArray(uint8_t *base, uint32_t array, uint32_t count, uint32_t extra)
  {
    const uint32_t grown = GuestAlloc(base, (count + extra) * 4);
    if (grown && array && count)
    {
      std::memcpy(base + grown, base + array, count * 4);
    }
    return grown;
  }

  // TU3's merge (T 8227A648), on the launch list after sub_82272A28.
  void RegisterLevels(PPCContext &ctx, uint8_t *base)
  {
    // As UpdateDLCState (T 8250FF08): memory category 5 and heap +32 clear
    // for the whole load.
    const uint32_t heap = LoadBe32(base, kHeapContext);
    const uint32_t saved_heap = LoadBe32(base, heap + 32);
    StoreBe32(base, heap + 32, 0);
    const uint32_t old_category = SetCategory(ctx, base, 5);
    const bool load_packages = base[kLoadPackages] != 0;

    std::vector<Level> levels;
    for (const Pack &pack : g_packs)
    {
      if (!pack.package.empty() && load_packages)
      {
        LoadPackage(ctx, base, pack.package + "System", 3, 3);
        LoadPackage(ctx, base, pack.package + "Frontend", 8, 4);
        LoadPackage(ctx, base, pack.package + "Frontend_loc", 8, 4);
      }
      for (const Level &level : pack.levels)
      {
        // Skip a level the list already has (same id and kind).
        const uint32_t count = LoadBe32(base, kLevelList);
        const uint32_t entries = LoadBe32(base, kLevelList + 4);
        bool known = false;
        for (uint32_t i = 0; i < count && !known; ++i)
        {
          const uint32_t entry = LoadBe32(base, entries + i * 4);
          known = base[entry + 4] == level.id && base[entry + 5] == level.kind;
        }
        if (!known)
        {
          levels.push_back(level);
        }
      }
    }
    if (!load_packages && Log())
    {
      REXLOG_INFO("pdz_dlc: packages are not loaded by name in this build (82E2E4C2 = 0)");
    }

    // The merge itself (T 8227A648): category 3, heap 3.
    SetCategory(ctx, base, 3);
    base[heap + 24] = 1;
    StoreBe32(base, heap + 28, 3);

    const uint32_t count = LoadBe32(base, kLevelList);
    const uint32_t extra = uint32_t(levels.size());
    g_first_pack_level = count;
    if (extra)
    {
      uint32_t arrays[3];
      for (uint32_t a = 0; a < 3; ++a)
      {
        arrays[a] = GrowArray(base, LoadBe32(base, kLevelList + 4 + a * 4), count, extra);
      }
      for (uint32_t j = 0; j < extra; ++j)
      {
        const Level &level = levels[j];
        const uint32_t index = count + j;
        const uint32_t entry = GuestAlloc(base, 8);
        const uint32_t name = GuestString(base, level.name);
        StoreBe32(base, entry, name);
        base[entry + 4] = level.id;
        base[entry + 5] = level.kind;
        StoreBe32(base, arrays[0] + index * 4, entry);

        const uint32_t data = LoadAsset(ctx, base, name);
        StoreBe32(base, arrays[1] + index * 4, data);
        uint32_t extra_data = 0;
        if (data)
        {
          const uint32_t extra_name = LoadBe32(base, data + 140);
          if (extra_name && ReadString(base, extra_name) != kNoName)
          {
            extra_data = LoadAsset(ctx, base, extra_name);
          }
        }
        StoreBe32(base, arrays[2] + index * 4, extra_data);

        // Per-kind id -> index table (kind 0 single player, 1 multiplayer, 2 other).
        if (level.kind < 3)
        {
          const uint32_t size_at = kLevelList + 16 + level.kind * 8;
          const uint32_t size = LoadBe32(base, size_at);
          uint32_t table = LoadBe32(base, size_at + 4);
          if (level.id >= size)
          {
            const uint32_t grown = GuestAlloc(base, (level.id + 1) * 4);
            for (uint32_t i = 0; i <= level.id; ++i)
            {
              StoreBe32(base, grown + i * 4, i < size ? LoadBe32(base, table + i * 4) : 0xFFFFFFFFu);
            }
            table = grown;
            StoreBe32(base, size_at, level.id + 1);
            StoreBe32(base, size_at + 4, table);
          }
          StoreBe32(base, table + level.id * 4, index);
        }
        if (Log())
        {
          REXLOG_INFO("pdz_dlc: level {} (id {:02X}, kind {}) -> index {}, leveldata {:08X}, extra {:08X}",
                      level.name, level.id, level.kind, index, data, extra_data);
        }
      }
      for (uint32_t a = 0; a < 3; ++a)
      {
        StoreBe32(base, kLevelList + 4 + a * 4, arrays[a]);
      }
      StoreBe32(base, kLevelList, count + extra);
    }
    base[heap + 24] = 0;
    StoreBe32(base, heap + 32, saved_heap);
    SetCategory(ctx, base, old_category);
    REXLOG_INFO("pdz_dlc: {} map pack(s), {} level(s) added ({} in the list)", g_packs.size(), extra,
                count + extra);
  }

  std::string FindInPacks(PPCContext &ctx, uint8_t *base, const std::string &rel);

  // A string table into one of our slots (sub_82393938(name, slot, 3)).
  void LoadStringTable(PPCContext &ctx, uint8_t *base, const std::string &name, uint32_t slot,
                       uint32_t heap = 3)
  {
    if (name.empty() || slot < kGameStringSlots || slot >= kStringSlots)
    {
      return;
    }
    const uint32_t entry = g_slots + slot * kSlotSize;
    if (LoadBe32(base, entry + 4))
    {
      return; // already loaded
    }
    // The launch loader has no check for a missing table (TU3 added one), so
    // only load tables a pack (or UPDATE:) has.
    // An asset name's underscores are its folders (multi_gasplant ->
    // multi\gasplant).
    const std::string prefix = "aid_strings_text_";
    std::string folder = name.rfind(prefix, 0) == 0 ? name.substr(prefix.size()) : std::string();
    std::replace(folder.begin(), folder.end(), '_', '\\');
    const bool found = !folder.empty() &&
                       !FindInPacks(ctx, base, "aid\\strings\\text\\" + folder + "\\default.str").empty();
    const uint32_t guest_name = GuestString(base, name);
    if (!found)
    {
      if (Log())
      {
        REXLOG_INFO("pdz_dlc: strings {} not found, slot {} left empty", name, slot);
      }
      return;
    }
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = guest_name;
    frame.ctx.r4.u64 = entry;
    frame.ctx.r5.u64 = heap; // 3 global tables, 8 per-level ones (as the game)
    sub_82393938(frame.ctx, base);
    if (Log())
    {
      REXLOG_INFO("pdz_dlc: strings {} -> slot {}: {}", name, slot, LoadBe32(base, entry) ? "loaded" : "not found");
    }
  }

  // The pack lookup for a game path, plain and in the two language folders:
  // returns the first pack (or UPDATE:) path that exists.
  std::string FindInPacks(PPCContext &ctx, uint8_t *base, const std::string &rel)
  {
    const std::string root = ReadString(base, kGameRoot);
    std::string prefixes[3] = {root + "ASSETS\\"};
    for (int variant = 1; variant < 3; ++variant)
    {
      rex::CallFrame frame(ctx);
      const uint32_t buffer = rex::ppc::stack_push(frame.ctx, base, std::string(260, '\0').data(), 260);
      frame.ctx.r3.u64 = buffer;
      frame.ctx.r4.u64 = 0;
      if (variant == 1)
      {
        sub_82394750(frame.ctx, base);
      }
      else
      {
        sub_82394810(frame.ctx, base);
      }
      prefixes[variant] = ReadString(base, buffer);
    }
    for (const std::string &pack : g_roots)
    {
      for (const std::string &prefix : prefixes)
      {
        const std::string candidate = pack + ":\\" + Tail(prefix + rel);
        if (Exists(candidate))
        {
          return candidate;
        }
      }
    }
    return {};
  }
} // namespace

namespace
{
  bool TitleUpdateDataWanted()
  {
    return REXCVAR_GET(pdz_game_fixes) && (REXCVAR_GET(pdz_fix_tu_township) || REXCVAR_GET(pdz_fix_tu_mp_hitfix));
  }

  // The title update records registered: Township's (U12) and Chinatown's /
  // Desert's (U18) hit-fix data. Names are the asset names, lower case.
  bool TitleUpdateRecordWanted(const std::string &name)
  {
    auto has = [&](const char *part) { return name.find(part) != std::string::npos; };
    if (g_tu_township && (has("proplists_single_town") || has("static1142002086") || has("hitfix_town") ||
                          has("replacement_township") || has("replacement_tonwship")))
    {
      return true;
    }
    return g_tu_mp_hitfix && (has("proplists_multi_chinatown") || has("proplists_multi_desert") ||
                              has("static1141219760") || has("static1143197731") || has("static1139304392") ||
                              has("hitfix_urban") || has("hitfix_chinatown") || has("multiplayer_props_thing"));
  }

  // T 821411A8 / 82141260: the title update package (filtered) and the
  // Township textures. Runs once, at boot.
  void LoadTitleUpdateData(PPCContext &ctx, uint8_t *base)
  {
    if (!g_update_mounted || FindInPacks(ctx, base, kTitleUpdateFile).empty())
    {
      REXLOG_INFO("pdz_fix_tu: the title update's data package isn't installed (UPDATE:\\pdz\\ASSETS\\{}); "
                  "Township textures and the hit-fix props stay as launch. Install it with "
                  "tools/install_map_packs.py --update",
                  kTitleUpdateFile);
      return;
    }
    if (!base[kLoadPackages])
    {
      REXLOG_INFO("pdz_fix_tu: packages are not loaded by name in this build (82E2E4C2 = 0)");
      return;
    }
    g_tu_township = REXCVAR_GET(pdz_fix_tu_township);
    g_tu_mp_hitfix = REXCVAR_GET(pdz_fix_tu_mp_hitfix);
    g_tu_registering = true;
    LoadPackage(ctx, base, kTitleUpdatePackage, 3, 3);
    g_tu_registering = false;
    g_tu_thing = g_tu_mp_hitfix && g_tu_taken;
    if (!g_tu_taken)
    {
      REXLOG_WARN("pdz_fix_tu: no title update record was registered (package not loaded?)");
      g_tu_township = g_tu_mp_hitfix = g_tu_thing = false;
      return;
    }
    uint32_t textures = 0;
    for (size_t i = 0; g_tu_township && i < std::size(kTownshipTextures); ++i)
    {
      uint32_t handle = 0;
      LoadAsset(ctx, base, GuestString(base, kTownshipTextures[i]), &handle, false);
      g_township_textures[i] = handle;
      textures += handle != 0;
    }
    REXLOG_INFO("pdz_fix_tu: title update package: {} record(s) taken ({}{}), {} left to the game, {} "
                "already registered; Township textures {}/{}",
                g_tu_taken, g_tu_township ? "Township" : "", g_tu_mp_hitfix ? " Chinatown / Desert" : "", g_tu_left,
                g_tu_present, textures, std::size(kTownshipTextures));
  }

  bool PackMapRules()
  {
    return g_active && REXCVAR_GET(pdz_game_fixes) && REXCVAR_GET(pdz_fix_tu_pack_map_rules);
  }

  int32_t CallInt(PPCContext &ctx, uint8_t *base, PPCFunc *fn, uint32_t r3 = 0, uint32_t r4 = 0)
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = r3;
    frame.ctx.r4.u64 = r4;
    fn(frame.ctx, base);
    return int32_t(frame.ctx.r3.u32);
  }

  // Option ids (DARKVALUE name table order, 82D209A0).
  constexpr uint32_t kOptionMap = 1;
  constexpr uint32_t kOptionPlayersMax = 5;
  constexpr uint32_t kOptionMode = 60;
  constexpr uint32_t kOptionScenario = 61;
  constexpr uint32_t kOptionDarkOpsScenario = 62;
  constexpr uint32_t kFacilityId = 0x21;
  constexpr uint32_t kRuinId = 0x23;
  constexpr uint32_t kRandomLevelId = 0x82D28848; // -> the Random entry's level id
  constexpr uint32_t kMatchSettings = 0x82E2ED64; // -> settings (+652 the Random marker)

  // The map position of Facility or Ruin chosen in the setup (TU3: map
  // position == position of aid_xml_level_multi_alcatraz / _temple64).
  bool PackTwoMapChosen(PPCContext &ctx, uint8_t *base)
  {
    const int32_t map = CallInt(ctx, base, sub_823AFC00, kOptionMap);
    return map == CallInt(ctx, base, sub_8242EDC8, kFacilityId) || map == CallInt(ctx, base, sub_8242EDC8, kRuinId);
  }

  // TU3's value lists for Facility / Ruin (T lists 17, 19, 34).
  const std::vector<int32_t> *PackTwoList(PPCContext &ctx, uint8_t *base, uint32_t option)
  {
    static const std::vector<int32_t> players = {2, 3, 4, 5, 6, 7, 8};
    static const std::vector<int32_t> scenarios = {0, 1};
    static const std::vector<int32_t> darkops = {0, 2};
    if ((option != kOptionPlayersMax && option != kOptionScenario && option != kOptionDarkOpsScenario) ||
        !PackMapRules() || !PackTwoMapChosen(ctx, base))
    {
      return nullptr;
    }
    return option == kOptionPlayersMax ? &players : option == kOptionScenario ? &scenarios : &darkops;
  }
} // namespace

// Before 821F04D4 in sub_821F0410 (package registration; r3 = the name's
// existing record, r31 = the package entry): while the title update package
// registers, a record it shouldn't take counts as already registered, so the
// game skips it.
void PdzTuPackageEntry(PPCRegister &r3, PPCRegister &r31)
{
  if (!g_tu_registering)
  {
    return;
  }
  const uint8_t *base = rex::Runtime::instance()->virtual_membase();
  const std::string name = Lower(ReadString(base, LoadBe32(base, r31.u32)));
  if (r3.u32)
  {
    ++g_tu_present;
    return;
  }
  const bool take = TitleUpdateRecordWanted(name);
  ++(take ? g_tu_taken : g_tu_left);
  if (Log())
  {
    REXLOG_INFO("pdz_dlc: title update record {}: {}", name, take ? "taken" : "left to the game");
  }
  if (!take)
  {
    r3.u64 = 1;
  }
}

// After the background setup (sub_8260B9B0) on level load (before 8227142C,
// r29) and restart (before 82270308, r27); the register is the leveldata.
void PdzTuTownshipTextures(PPCRegister &leveldata)
{
  static uint32_t count = 0;
  if (!g_tu_township || !REXCVAR_GET(pdz_game_fixes) || !REXCVAR_GET(pdz_fix_tu_township))
  {
    return;
  }
  uint8_t *base = rex::Runtime::instance()->virtual_membase();
  for (uint32_t texture : g_township_textures)
  {
    if (!texture)
    {
      return;
    }
  }
  if (Lower(ReadString(base, LoadBe32(base, leveldata.u32 + 16))) != kTownshipBackground)
  {
    return;
  }
  const uint32_t manager = LoadBe32(base, kBackgroundManager);
  if (!manager)
  {
    return;
  }
  for (uint32_t slot : {20u, 24u})
  {
    const uint32_t instance = LoadBe32(base, manager + slot);
    const uint32_t object = instance ? LoadBe32(base, instance + 4) : 0;
    for (size_t i = 0; object && i < std::size(kTownshipTextures); ++i)
    {
      StoreBe32(base, object + kTownshipTextureFields[i], g_township_textures[i]);
    }
  }
  if (count++ % 100 == 0)
  {
    REXLOG_INFO("pdz_fix_tu: Township replacement textures set on the background (TU3) ({} time(s))", count);
  }
}

// sub_82272A28(): builds the level list at startup (DarkLevelInitConfigs).
REX_HOOK_RAW(sub_82272A28)
{
  __imp__sub_82272A28(ctx, base);
  const bool packs = REXCVAR_GET(pdz_dlc_maps);
  const bool title_update = TitleUpdateDataWanted();
  if (packs || title_update)
  {
    MountPacks(packs);
    g_lookups = (packs && !g_packs.empty()) || (title_update && g_update_mounted);
    if (packs && !g_packs.empty())
    {
      g_active = true; // the lookups below already need the packs
      RegisterLevels(ctx, base);
    }
    if (title_update)
    {
      LoadTitleUpdateData(ctx, base);
    }
  }
}

// sub_82393540(): string slots init, then the general / frontend tables.
REX_HOOK_RAW(sub_82393540)
{
  __imp__sub_82393540(ctx, base);
  if (!g_active)
  {
    return;
  }
  if (!g_slots)
  {
    g_slots = GuestAlloc(base, kStringSlots * kSlotSize);
  }
  if (!g_slots)
  {
    return;
  }
  if (std::find(g_roots.begin(), g_roots.end(), "UPDATE") != g_roots.end())
  {
    for (const auto &table : kUpdateStrings)
    {
      LoadStringTable(ctx, base, table.name, table.slot);
    }
  }
  for (const Pack &pack : g_packs)
  {
    LoadStringTable(ctx, base, pack.general, pack.general_slot);
    LoadStringTable(ctx, base, pack.frontend, pack.frontend_slot);
  }
}

// sub_82393780(text, subtitles): a level's string tables (slots 4-6), from
// the level load (82271C7C). TU3's version (8239B830, called with the level
// name in multiplayer modes 2 / 3: 822774D0 on the map id at +25) also loads
// "aid_strings_text_multi" + the level name's last "_..." part into slot 7:
// the pack maps' area names (the label under the pause map) are there.
// Launch maps have none (their names are in multi_all), so only tables a
// pack has are loaded.
REX_EXTERN(__imp__sub_82393780);
REX_EXTERN(__imp__sub_82393F08);
REX_EXTERN(sub_8226FDE0);
REX_EXTERN(sub_82393A48);
REX_HOOK_RAW(sub_82393780)
{
  __imp__sub_82393780(ctx, base);
  if (!g_active || !g_slots)
  {
    return;
  }
  const uint32_t game = LoadBe32(base, kGameState);
  const uint32_t mode = LoadBe32(base, game + 36);
  if (mode != 2 && mode != 3)
  {
    return;
  }
  rex::CallFrame frame(ctx);
  frame.ctx.r3.u64 = uint64_t(int64_t(int8_t(base[game + 25])));
  sub_8226FDE0(frame.ctx, base);
  const std::string level = ReadString(base, frame.ctx.r3.u32);
  const size_t underscore = level.rfind('_');
  if (underscore != std::string::npos)
  {
    LoadStringTable(ctx, base, "aid_strings_text_multi" + level.substr(underscore), kLevelStringSlot, 8);
  }
}

// sub_82393F08(): releases a level's string tables (slots 4-6, sub_82393A48
// each). TU3's (8239C110) releases slot 7 too.
REX_HOOK_RAW(sub_82393F08)
{
  __imp__sub_82393F08(ctx, base);
  if (g_slots && LoadBe32(base, g_slots + kLevelStringSlot * kSlotSize))
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = g_slots + kLevelStringSlot * kSlotSize;
    sub_82393A48(frame.ctx, base);
  }
}

// sub_821F0E58(): audio init. TU3 grows the audio manager so its sound bank
// table holds ~149 banks (the init clears 1788 bytes instead of 1020, the
// fields after the table move by 768). Launch code reaches the manager only
// through the pointer at 82D6B2D0 (set once by 82213160) and indexes banks as
// manager + 56 + 12 x n with n from the cue id (12 bits: 821FA770 writes,
// 821FAB60 / 821FD640 / 821FDAF8 read, no bounds checks); only the soundbank
// loader sub_821F2050, the unloader sub_821F21A8 and the idle check
// sub_821F1888 stop at entry 84 (all hooked below; TU3 changed exactly these
// plus the init's clear). So, before the init,
// the manager moves to a zeroed 4 KB guest block (its first 1084 bytes
// copied, every field at its offset); the packs' banks are numbered from 86,
// past the two fields after the table, and land in the new space. The
// game's own banks keep their numbers and slots.
REX_HOOK_RAW(sub_821F0E58)
{
  if (g_active)
  {
    const uint32_t old = LoadBe32(base, kAudioManager);
    // A pointer into the manager itself would go stale when it moves.
    bool self_pointer = false;
    for (uint32_t i = 0; i < kManagerSize && !self_pointer; i += 4)
    {
      const uint32_t v = LoadBe32(base, old + i);
      self_pointer = v >= old && v < old + kManagerSize;
    }
    const uint32_t moved = self_pointer ? 0 : GuestAlloc(base, kMovedSize);
    if (moved)
    {
      std::memcpy(base + moved, base + old, kManagerSize);
      StoreBe32(base, kAudioManager, moved);
      g_bank_slots = (kMovedSize - 56) / 12 - 1;
      if (Log())
      {
        REXLOG_INFO("pdz_dlc: audio manager {:08X} -> {:08X} ({} sound bank slots)", old, moved, g_bank_slots);
      }
    }
    else
    {
      REXLOG_WARN("pdz_dlc: audio manager not moved ({}); the packs' ambience banks may not fit",
                  self_pointer ? "it points into itself" : "no memory");
    }
  }
  __imp__sub_821F0E58(ctx, base);
}

// sub_8251ADC0(this): builds the prop template map (this +4: model name ->
// template name) from the master list. The map's node pool is sized for the
// list (sub_82623748 from 8251AE14), so it gets room for the packs' entries
// first; they are inserted after the list (sub_826237E0, as the game does).
REX_HOOK_RAW(sub_82623748)
{
  if (uint32_t(ctx.lr) == kTemplateMapInitReturn)
  {
    ctx.r4.u64 = ctx.r4.u32 + (g_active ? uint32_t(std::size(kPackPropTemplates)) : 0) + (g_tu_thing ? 1 : 0);
  }
  __imp__sub_82623748(ctx, base);
}

REX_HOOK_RAW(sub_8251ADC0)
{
  const uint32_t self = ctx.r3.u32;
  __imp__sub_8251ADC0(ctx, base);
  auto add = [&](const char *model, const char *prop_template)
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = self + 4;
    frame.ctx.r4.u64 = GuestString(base, model);
    frame.ctx.r5.u64 = GuestString(base, prop_template);
    sub_826237E0(frame.ctx, base);
  };
  if (g_active)
  {
    for (const auto &entry : kPackPropTemplates)
    {
      add(entry.model, entry.propTemplate);
    }
    if (Log())
    {
      REXLOG_INFO("pdz_dlc: {} prop templates added (Facility doors, teleporters)", std::size(kPackPropTemplates));
    }
  }
  if (g_tu_thing)
  {
    // pdz_fix_tu_mp_hitfix (U18): Desert's hit-fix prop, as TU3's master list.
    add(kThingModel, kThingTemplate);
    if (Log())
    {
      REXLOG_INFO("pdz_dlc: prop template added: {} -> {} (title update)", kThingModel, kThingTemplate);
    }
  }
}

// sub_821F1E48(path): loads a wavebank (diagnostic only, pdz_dlc_log).
REX_EXTERN(__imp__sub_821F1E48);
REX_HOOK_RAW(sub_821F1E48)
{
  const bool log = g_active && Log();
  const std::string path = log ? ReadString(base, ctx.r3.u32) : std::string();
  __imp__sub_821F1E48(ctx, base);
  if (log)
  {
    REXLOG_INFO("pdz_dlc: wavebank {} -> {:08X}", path, ctx.r3.u32);
  }
}

// sub_821F2050(path): loads a soundbank (from a global or level soundbank
// list) and stores its handle at +4 of the bank entry with the same name,
// searching entries 0-84 only (821F217C cmplwi 85; TU3's 821F9A00 searches
// its larger table). For a bank that moved past them (86+), do the same
// search there.
REX_HOOK_RAW(sub_821F2050)
{
  const bool moved = g_active && g_bank_slots > kBankSlots;
  const std::string path = moved ? ReadString(base, ctx.r3.u32) : std::string();
  __imp__sub_821F2050(ctx, base);
  const uint32_t handle = ctx.r3.u32;
  if (!moved || !handle)
  {
    return;
  }
  std::string name = path.substr(path.find_last_of("\\/") + 1);
  name = Lower(name.substr(0, name.rfind('.')));
  const uint32_t manager = LoadBe32(base, kAudioManager);
  const uint32_t count = LoadBe32(base, kBankCount);
  for (uint32_t i = 0; i <= kBankSlots; ++i)
  {
    if (Lower(ReadString(base, LoadBe32(base, manager + 56 + i * 12))) == name)
    {
      return; // the game's own search covered it
    }
  }
  for (uint32_t i = kFirstMovedBank; i <= count && i <= g_bank_slots; ++i)
  {
    const uint32_t entry = manager + 56 + i * 12;
    if (Lower(ReadString(base, LoadBe32(base, entry))) == name)
    {
      StoreBe32(base, entry + 4, handle);
      if (Log())
      {
        REXLOG_INFO("pdz_dlc: soundbank {} -> bank {}", name, i);
      }
      return;
    }
  }
}

// sub_821F21A8(&handle): unloads a soundbank, clearing +4 of every bank entry
// that holds it, in entries 0-84 only (li r10,85). Clear the moved ones too,
// or they would keep the destroyed soundbank.
REX_HOOK_RAW(sub_821F21A8)
{
  if (g_active && g_bank_slots > kBankSlots && ctx.r3.u32)
  {
    const uint32_t handle = LoadBe32(base, ctx.r3.u32);
    const uint32_t manager = LoadBe32(base, kAudioManager);
    const uint32_t count = LoadBe32(base, kBankCount);
    for (uint32_t i = kFirstMovedBank; handle && i <= count && i <= g_bank_slots; ++i)
    {
      const uint32_t entry = manager + 56 + i * 12;
      if (LoadBe32(base, entry + 4) == handle)
      {
        StoreBe32(base, entry + 4, 0);
        if (Log())
        {
          REXLOG_INFO("pdz_dlc: soundbank unloaded from bank {}", i);
        }
      }
    }
  }
  __imp__sub_821F21A8(ctx, base);
}

// sub_821F1888(): the audio idle check that leaving a level waits on (the game
// state machine sub_822261E0 polls it every frame before the level teardown):
// while any sound is active (the 25 counters at *manager +24) it stops every
// soundbank in entries 0-84 (cmpwi 1020; TU3's 821F9290 covers its larger
// table) and returns false. Stop the moved banks too, or a pack's ambience
// keeps playing and the exit waits forever.
REX_HOOK_RAW(sub_821F1888)
{
  // The multiplayer match music stops here too (mp_music.cpp).
  mp_music::LeaveLevel(ctx, base);
  if (g_active && g_bank_slots > kBankSlots)
  {
    const uint32_t manager = LoadBe32(base, kAudioManager);
    const uint32_t counters = LoadBe32(base, manager) + 24;
    int32_t active = 0;
    for (uint32_t i = 0; i < 25; ++i)
    {
      active += int32_t(LoadBe32(base, counters + i * 4));
    }
    const uint32_t count = LoadBe32(base, kBankCount);
    for (uint32_t i = kFirstMovedBank; active > 0 && i <= count && i <= g_bank_slots; ++i)
    {
      const uint32_t handle = LoadBe32(base, manager + 56 + i * 12 + 4);
      if (handle)
      {
        rex::CallFrame frame(ctx);
        frame.ctx.r3.u64 = handle;
        frame.ctx.r4.u64 = 1;
        sub_8274DB40(frame.ctx, base);
        if (Log())
        {
          REXLOG_INFO("pdz_dlc: soundbank in bank {} stopped (leaving the level)", i);
        }
      }
    }
  }
  __imp__sub_821F1888(ctx, base);
}

// sub_821F22D0(): loads the game's cue lists (aid_xml_audio_cuelistlists_global)
// during audio init (sub_821F0E58), before the rest of the init builds on them.
// The packs' lists go right after them (loading them after the whole init
// crashed the title menu's music). The packs' banks are ambience loops.
REX_HOOK_RAW(sub_821F22D0)
{
  __imp__sub_821F22D0(ctx, base);
  if (!g_active)
  {
    return;
  }
  // The sound bank table (manager +56, 12 bytes per bank, numbered from 1,
  // count at 82D3351C) has 84 slots in place and the game uses 82 of them;
  // with the manager moved (sub_821F0E58 above) the packs' banks start at 86.
  // A pack's list only loads if its new banks fit.
  const uint32_t manager = LoadBe32(base, kAudioManager);
  if (g_bank_slots > kBankSlots && LoadBe32(base, kBankCount) < kFirstMovedBank - 1)
  {
    StoreBe32(base, kBankCount, kFirstMovedBank - 1);
  }
  for (const Pack &pack : g_packs)
  {
    if (pack.cuelist.empty())
    {
      continue;
    }
    const uint32_t count = LoadBe32(base, kBankCount);
    std::vector<std::string> known;
    for (uint32_t i = 1; i <= count && i <= g_bank_slots; ++i)
    {
      if (i * 12 + 56 >= kBankFields && i < kFirstMovedBank)
      {
        continue; // skipped slots (their place holds other fields)
      }
      known.push_back(Lower(ReadString(base, LoadBe32(base, manager + 56 + i * 12))));
    }
    std::vector<std::string> added;
    std::filesystem::path audio = FindFolder(FindFolder(FindFolder(pack.host, "pdz"), "assets"), "audio");
    std::ifstream file(audio / pack.cuelist);
    for (std::string line; std::getline(file, line);)
    {
      const std::string bank = Lower(line.substr(0, line.find_first_of("\t\r")));
      if (!bank.empty() && std::find(known.begin(), known.end(), bank) == known.end() &&
          std::find(added.begin(), added.end(), bank) == added.end())
      {
        added.push_back(bank);
      }
    }
    if (count + added.size() > g_bank_slots)
    {
      REXLOG_WARN("pdz_dlc: cue list {} skipped: {} new sound bank(s), {} of {} slots used (the "
                  "launch sound bank table is full; its maps play without their own ambience)",
                  pack.cuelist, added.size(), count, g_bank_slots);
      continue;
    }
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = GuestString(base, pack.cuelist);
    frame.ctx.r4.u64 = 1;
    sub_821FA770(frame.ctx, base);
    if (Log())
    {
      REXLOG_INFO("pdz_dlc: cue list {} loaded ({} new bank(s), {} of {} slots used before)", pack.cuelist,
                  added.size(), count, g_bank_slots);
    }
  }
}

// sub_821EEE60(rel, out, ?, alt): "game:\pdz\ASSETS\" + rel, or its language
// variants; true if the file exists (out gets the path).
REX_HOOK_RAW(sub_821EEE60)
{
  if (!g_lookups || (ctx.r6.u32 & 0xFF))
  {
    __imp__sub_821EEE60(ctx, base);
    return;
  }
  const std::string rel = ReadString(base, ctx.r3.u32);
  const std::string found = FindInPacks(ctx, base, rel);
  if (found.empty())
  {
    __imp__sub_821EEE60(ctx, base);
    return;
  }
  if (ctx.r4.u32)
  {
    WriteString(base, ctx.r4.u32, found);
  }
  if (Log() && rel.find('.') != std::string::npos)
  {
    REXLOG_INFO("pdz_dlc: {} -> {}", rel, found);
  }
  ctx.r3.u64 = 1;
}

// sub_8221DFF0(path, out): the full path of a file (root in front unless it
// is already absolute); returns out.
REX_HOOK_RAW(sub_8221DFF0)
{
  if (g_lookups)
  {
    const std::string path = ReadString(base, ctx.r3.u32);
    const std::string lower = Lower(path.substr(0, 8));
    if (lower.rfind("update:\\", 0) == 0 || lower.rfind("content", 0) == 0)
    {
      WriteString(base, ctx.r4.u32, path);
      ctx.r3.u64 = ctx.r4.u32;
      return;
    }
  }
  __imp__sub_8221DFF0(ctx, base);
}

// sub_82AC3DA8(path, access, share, security, disposition, flags, template):
// CreateFileA. A missing file under ASSETS\audio\ is taken from a pack.
REX_HOOK_RAW(sub_82AC3DA8)
{
  if (g_active)
  {
    const std::string path = ReadString(base, ctx.r3.u32);
    const std::string lower = Lower(path);
    if (Log() && (lower.find(".xwb") != std::string::npos || lower.find(".xsb") != std::string::npos))
    {
      REXLOG_INFO("pdz_dlc: open {} ({})", path, Exists(path) ? "in the game" : "not in the game");
    }
    if (lower.find("\\assets\\audio\\") != std::string::npos && !Exists(path))
    {
      for (const std::string &pack : g_roots)
      {
        const std::string candidate = pack + ":\\" + Tail(path);
        if (Exists(candidate))
        {
          if (Log())
          {
            REXLOG_INFO("pdz_dlc: {} -> {}", path, candidate);
          }
          const uint32_t saved = ctx.r1.u32;
          ctx.r3.u64 = rex::ppc::stack_push_string(ctx, base, candidate.c_str());
          __imp__sub_82AC3DA8(ctx, base);
          ctx.r1.u32 = saved;
          return;
        }
      }
    }
  }
  __imp__sub_82AC3DA8(ctx, base);
}

// sub_82393AD0(id): string id (slot << 16 | index) -> text.
REX_HOOK_RAW(sub_82393AD0)
{
  const uint32_t slot = ctx.r3.u32 >> 16;
  if (slot < kGameStringSlots || !g_slots)
  {
    __imp__sub_82393AD0(ctx, base);
    return;
  }
  uint32_t result = 0;
  const uint32_t entry = g_slots + slot * kSlotSize;
  if (slot < kStringSlots && LoadBe32(base, entry))
  {
    rex::CallFrame frame(ctx);
    const uint32_t out = rex::ppc::stack_push<uint32_t>(frame.ctx, base, 0);
    frame.ctx.r3.u64 = LoadBe32(base, entry);
    frame.ctx.r4.u64 = LoadBe32(base, entry + 4);
    frame.ctx.r5.u64 = ctx.r3.u32 & 0xFFFF;
    frame.ctx.r6.u64 = out;
    sub_8275A400(frame.ctx, base);
    result = LoadBe32(base, out);
  }
  ctx.r3.u64 = result;
}

// sub_82393B30(id): string id -> length (or -1).
REX_HOOK_RAW(sub_82393B30)
{
  const uint32_t slot = ctx.r3.u32 >> 16;
  if (slot < kGameStringSlots || !g_slots)
  {
    __imp__sub_82393B30(ctx, base);
    return;
  }
  uint32_t result = 0xFFFFFFFFu;
  const uint32_t entry = g_slots + slot * kSlotSize;
  if (slot < kStringSlots && LoadBe32(base, entry))
  {
    rex::CallFrame frame(ctx);
    const uint32_t out = rex::ppc::stack_push<uint32_t>(frame.ctx, base, 0);
    frame.ctx.r3.u64 = LoadBe32(base, entry + 4);
    frame.ctx.r4.u64 = ctx.r3.u32 & 0xFFFF;
    frame.ctx.r5.u64 = out;
    frame.ctx.r6.u64 = 0;
    sub_8275A870(frame.ctx, base);
    result = frame.ctx.r3.u32 - 1;
  }
  ctx.r3.u64 = result;
}

// Map pictures. The multiplayer map picture is an XUI tab scene
// (DarkXuiGameContextualTabscene on DARKVALUE_LEVEL / DARKVALUE_OPTIMATCH_LEVEL,
// launch frontend package 1365415862.pkg) with one child scene per map, named
// Tab<level id + 1>: Tab1 subway, Tab5-Tab9, Tab63 random. The XUI tab switch
// (sub_82792BB8) looks the child up by that id (sub_8277ADB8, returning to
// 82792C8C) and shows nothing when it is missing, as for the pack maps. TU3
// shows a level's info scene instead (leveldata +0x9C, e.g.
// aid_darkxui_frontend_info_level_trench in the pack's Frontend package: one
// tab with the map's image and name) in a new control (T 823FFE58: scene
// create 82399048 with the element's base path 823A2C00, add child 8277A658,
// the previous one destroyed with 827768C8). Here, for a pack level only, that
// scene becomes the missing tab: one per tab scene, replaced when another pack
// map is shown. The launch maps' tabs are found as before and untouched.
REX_EXTERN(__imp__sub_8277ADB8);
REX_EXTERN(sub_82779A50);
REX_EXTERN(sub_823A2C00);
REX_EXTERN(sub_82399048);
REX_EXTERN(sub_8277A658);
REX_EXTERN(sub_827768C8);

namespace
{
  constexpr uint32_t kTabLookupReturn = 0x82792C8C; // sub_8277ADB8 from the tab switch
  constexpr uint32_t kElementClass = 0x82D356C4;    // XUI element class (handle -> object)
  constexpr uint32_t kRandomTab = 63;               // Tab63: only the map tab scenes have it
  constexpr uint32_t kSceneCategory = 7;            // memory category for scenes (823A11A0)

  struct PackTab
  {
    uint32_t scene; // tab scene handle
    uint32_t child; // the pack scene shown as its tab
  };
  std::vector<PackTab> g_pack_tabs;

  std::string ReadWide(const uint8_t *base, uint32_t address, size_t max = 64)
  {
    std::string text;
    for (size_t i = 0; address && i < max; ++i)
    {
      const uint16_t c = uint16_t(base[address + i * 2] << 8 | base[address + i * 2 + 1]);
      if (!c)
      {
        break;
      }
      text += c < 0x80 ? char(c) : '?';
    }
    return text;
  }

  // A guest UTF-16 copy of an ASCII string on the frame's stack.
  uint32_t PushWide(PPCContext &frame, uint8_t *base, const std::string &text)
  {
    std::string wide;
    for (char c : text)
    {
      wide += '\0';
      wide += c;
    }
    wide.append(2, '\0');
    return rex::ppc::stack_push(frame, base, wide.data(), uint32_t(wide.size()));
  }

  uint32_t ElementObject(PPCContext &ctx, uint8_t *base, uint32_t handle)
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = handle;
    frame.ctx.r4.u64 = LoadBe32(base, kElementClass);
    sub_82779A50(frame.ctx, base);
    return frame.ctx.r3.u32;
  }

  // Whether child is still a child of parent (children: object +24, next +32).
  bool HasChild(PPCContext &ctx, uint8_t *base, uint32_t parent, uint32_t child)
  {
    const uint32_t parent_object = ElementObject(ctx, base, parent);
    const uint32_t child_object = ElementObject(ctx, base, child);
    for (uint32_t node = parent_object ? LoadBe32(base, parent_object + 24) : 0; node && child_object;
         node = LoadBe32(base, node + 32))
    {
      if (node == child_object)
      {
        return true;
      }
    }
    return false;
  }

  uint32_t FindChild(PPCContext &ctx, uint8_t *base, uint32_t parent, const std::string &id)
  {
    rex::CallFrame frame(ctx);
    const uint32_t out = rex::ppc::stack_push<uint32_t>(frame.ctx, base, 0);
    frame.ctx.r3.u64 = parent;
    frame.ctx.r4.u64 = PushWide(frame.ctx, base, id);
    frame.ctx.r5.u64 = out;
    __imp__sub_8277ADB8(frame.ctx, base);
    return LoadBe32(base, out);
  }

  // A pack level's info scene name (leveldata +0x9C), by level id.
  std::string PackInfoScene(uint8_t *base, uint32_t id)
  {
    const uint32_t size = LoadBe32(base, kLevelList + 16 + 1 * 8); // kind 1 (multiplayer)
    const uint32_t table = LoadBe32(base, kLevelList + 16 + 1 * 8 + 4);
    if (id >= size || !table)
    {
      return {};
    }
    const uint32_t index = LoadBe32(base, table + id * 4);
    if (index == 0xFFFFFFFFu || index < g_first_pack_level || index >= LoadBe32(base, kLevelList))
    {
      return {};
    }
    const uint32_t data = LoadBe32(base, LoadBe32(base, kLevelList + 8) + index * 4);
    const std::string scene = data ? ReadString(base, LoadBe32(base, data + 0x9C)) : std::string();
    return scene.rfind("aid_darkxui", 0) == 0 ? scene : std::string();
  }
} // namespace

// sub_8277ADB8(element, id, &child): XUI child by id.
REX_HOOK_RAW(sub_8277ADB8)
{
  const uint32_t lr = uint32_t(ctx.lr);
  const uint32_t parent = ctx.r3.u32;
  const uint32_t id = ctx.r4.u32;
  const uint32_t out = ctx.r5.u32;
  __imp__sub_8277ADB8(ctx, base);
  if (!g_active || lr != kTabLookupReturn || !out || LoadBe32(base, out))
  {
    return;
  }
  const std::string tab = ReadWide(base, id);
  if (tab.size() < 4 || tab.rfind("Tab", 0) != 0 ||
      tab.find_first_not_of("0123456789", 3) != std::string::npos)
  {
    return;
  }
  const uint32_t number = uint32_t(std::stoul(tab.substr(3)));
  const std::string scene = number ? PackInfoScene(base, number - 1) : std::string();
  if (scene.empty() || !FindChild(ctx, base, parent, "Tab" + std::to_string(kRandomTab)))
  {
    return;
  }

  // The pack scene this tab scene showed before goes (as TU3's control does).
  for (auto it = g_pack_tabs.begin(); it != g_pack_tabs.end();)
  {
    if (it->scene != parent)
    {
      ++it;
      continue;
    }
    if (HasChild(ctx, base, parent, it->child))
    {
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = it->child;
      sub_827768C8(frame.ctx, base);
    }
    it = g_pack_tabs.erase(it);
  }

  const uint32_t old_category = SetCategory(ctx, base, kSceneCategory);
  uint32_t child = 0;
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = parent;
    sub_823A2C00(frame.ctx, base);
    const uint32_t base_path = frame.ctx.r3.u32;
    const uint32_t result = rex::ppc::stack_push<uint32_t>(frame.ctx, base, 0);
    frame.ctx.r4.u64 = PushWide(frame.ctx, base, scene);
    frame.ctx.r3.u64 = base_path;
    frame.ctx.r5.u64 = 0;
    frame.ctx.r6.u64 = result;
    sub_82399048(frame.ctx, base);
    child = LoadBe32(base, result);
  }
  if (child)
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = parent;
    frame.ctx.r4.u64 = child;
    sub_8277A658(frame.ctx, base);
    g_pack_tabs.push_back({parent, child});
    StoreBe32(base, out, child);
    ctx.r3.u64 = 0;
  }
  SetCategory(ctx, base, old_category);
  if (Log())
  {
    REXLOG_INFO("pdz_dlc: map picture {} -> {} ({:08X})", tab, scene, child);
  }
}


// --- Facility / Ruin rules (pdz_fix_tu_pack_map_rules, U22) ---

// sub_823B5288(): the Random map: a random map position other than Random's
// (launch: positions 0-8 only). TU3 (T 823BE050) picks from every map and
// leaves out Facility / Ruin unless the scenario is one they allow.
REX_HOOK_RAW(sub_823B5288)
{
  if (!PackMapRules())
  {
    __imp__sub_823B5288(ctx, base);
    return;
  }
  const uint32_t random_id = LoadBe32(base, kRandomLevelId);
  const int32_t count = CallInt(ctx, base, sub_8242EC78);
  const int32_t random = CallInt(ctx, base, sub_8242EDC8, random_id);
  const int32_t facility = CallInt(ctx, base, sub_8242EDC8, kFacilityId);
  const int32_t ruin = CallInt(ctx, base, sub_8242EDC8, kRuinId);
  const int32_t mode = CallInt(ctx, base, sub_823AFC00, kOptionMode);
  const int32_t scenario = CallInt(ctx, base, sub_823AFC00, kOptionScenario);
  const int32_t darkops = CallInt(ctx, base, sub_823AFC00, kOptionDarkOpsScenario);
  const bool pack_two_allowed = mode == 0 ? (scenario == 0 || scenario == 1 || scenario == -1)
                                          : (darkops == 0 || darkops == 2 || darkops == -1);
  int32_t position = -1;
  for (int tries = 0; tries < 1000 && count > 1; ++tries)
  {
    const int32_t pick = CallInt(ctx, base, sub_826F28C0, 0, uint32_t(count - 1));
    if (pick == random || ((pick == facility || pick == ruin) && !pack_two_allowed))
    {
      continue;
    }
    position = pick;
    break;
  }
  if (position < 0)
  {
    __imp__sub_823B5288(ctx, base);
    return;
  }
  CallInt(ctx, base, sub_823B0388, kOptionMap, uint32_t(position));
  // Launch's tail (823B52DC-823B52FC): settings +652 = Random's level index.
  const int32_t index = CallInt(ctx, base, sub_8242ED60, uint32_t(CallInt(ctx, base, sub_8242EDC8, random_id)));
  CallInt(ctx, base, sub_824BF828, LoadBe32(base, kMatchSettings) + 652, uint32_t(index) & 0xFF);
  if (Log())
  {
    REXLOG_INFO("pdz_dlc: Random map -> position {} of {} (Facility / Ruin {})", position, count,
                pack_two_allowed ? "allowed" : "left out for this scenario");
  }
}

// The option value list readers (count / first / last / value at index).
REX_HOOK_RAW(sub_823B2C28)
{
  if (const auto *list = PackTwoList(ctx, base, ctx.r3.u32))
  {
    ctx.r3.u64 = uint32_t(list->size());
    return;
  }
  __imp__sub_823B2C28(ctx, base);
}

REX_HOOK_RAW(sub_823B2D08)
{
  if (const auto *list = PackTwoList(ctx, base, ctx.r3.u32))
  {
    ctx.r3.u64 = uint32_t(list->front());
    return;
  }
  __imp__sub_823B2D08(ctx, base);
}

REX_HOOK_RAW(sub_823B2D90)
{
  if (const auto *list = PackTwoList(ctx, base, ctx.r3.u32))
  {
    ctx.r3.u64 = uint32_t(list->back());
    return;
  }
  __imp__sub_823B2D90(ctx, base);
}

// sub_823B31A0(option, index, kind): kinds 6 / 12 / 39 / 45 return the index.
REX_HOOK_RAW(sub_823B31A0)
{
  const int32_t kind = int32_t(ctx.r5.u32);
  if (kind != 6 && kind != 12 && kind != 39 && kind != 45)
  {
    if (const auto *list = PackTwoList(ctx, base, ctx.r3.u32))
    {
      const int32_t index = int32_t(ctx.r4.u32);
      ctx.r3.u64 = index >= 0 && index < int32_t(list->size()) ? uint32_t((*list)[index]) : 0xFFFFFFFFu;
      return;
    }
  }
  __imp__sub_823B31A0(ctx, base);
}
