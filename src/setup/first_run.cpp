// PDZ Restored: first-run setup (first_run.h).
#include "first_run.h"

#include "setup_window.h"
#include "theme.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <fmt/xchar.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/logging.h>

#pragma comment(lib, "bcrypt.lib")

REXCVAR_DEFINE_STRING(pdz_setup, "auto", "Setup",
                      "First-run setup of the game files (docs/RELEASE_FIRST_RUN.md): auto = only when no usable game "
                      "data is found; force = run it again now; off = never (the paths as given).");
REXCVAR_DEFINE_STRING(pdz_setup_data_dir, "", "Setup",
                      "Where setup puts the prepared game files (game\\ and update\\ and setup.json). Empty = data\\ "
                      "next to the exe, or <user data root>\\data when the exe's folder can't be written.");
REXCVAR_DEFINE_BOOL(pdz_setup_quiet, false, "Setup",
                    "TESTING TOOL: setup shows no message box or file picker; a problem is written to setup.log and "
                    "the game exits (code 2), as it would after the box.");
REXCVAR_DECLARE(int32_t, pdz_settings_test_dpi);
REXCVAR_DEFINE_STRING(pdz_setup_test_capture, "", "Testing",
                      "TESTING TOOL: at start-up, before anything else, the setup progress window with a sample step "
                      "and one of setup's questions are shown and written as setup_progress.bmp / setup_message.bmp "
                      "into this folder (the question is answered OK); then the start goes on as usual, and each later "
                      "progress window (the disc copy, the shader translation) writes setup_progress_<n>.bmp there");
REXCVAR_DEFINE_BOOL(pdz_setup_link, true, "Setup",
                    "First-run setup: a disc dump given as extracted files on the same drive as the prepared folder is "
                    "hard-linked there instead of copied (instant, no second copy on disk; the game only reads them). "
                    "Another drive, an ISO or a link that fails: copied as before");
REXCVAR_DEFINE_STRING(pdz_setup_source, "", "Setup",
                      "Where setup looks for the disc dump and Title Update 3 first. Empty = game\\ next to the exe; "
                      "a file picker asks when nothing is found there.");

namespace pdz::first_run
{
  namespace fs = std::filesystem;

  namespace
  {
    // What this build was made from: the retail disc's default.xex (media ID 6A78E206, version 0.0.0.4) and Title
    // Update 3's default.xexp (version 0.0.3.4). README.md "What you need".
    // The disc pressings and their own Title Update 3 (docs/RELEASE_FIRST_RUN.md "pressings", 2026-11-03). Every
    // disc's default.xex holds the same encrypted image (only its header differs: media ID, version, signature), and
    // each disc's own TU3 turns it into the same final image (kImageSha1; checked for 6A78E206, 750FF1D9 and
    // 41EEAFCB by loading each). 509DBB6B and 2B35C136 are known only by their TU3 file; the image check at load
    // (VerifyImage) decides for them. The discs differ in their languages (other loc / audio folders).
    // Any TU3 works with any disc (2026-11-04, docs/RELEASE_FIRST_RUN.md "Any Title Update 3"): the 9 pairings of the
    // three discs and their three TU3 packages all give kImageSha1 (the SDK's patcher only warns that the patch was
    // made for another disc's signature), and the five TU3 packages' 29 data files are byte-identical; only their
    // default.xexp differs (its header: media ID and version). Setup takes the disc's own TU3 when several are there.
    struct Pressing
    {
      uint32_t media_id;
      const char *version;
      const char *xex_sha1; // nullptr: no disc of it seen (accepted by its header, then VerifyImage)
      const char *xexp_sha1;
      const wchar_t *tu_file;
    };
    constexpr Pressing kPressings[] = {
        {0x6A78E206, "0.0.0.4", "c24ae39fb18a22de1f18e61dc498393d25eadcaf", "b148ab898498a4dbdd494ddaf0e7d826612a6126",
         L"TU_16L61UJ_000000G000000.00000000000O4"},
        {0x750FF1D9, "0.0.0.10", "deeb2c2578fbd158fdfd0f13b14c7f3bdcf8d7f2", "465e2344fe2200b6f5639bd81056ce08fa15e4ad",
         L"TU_16L61UJ_0000018000000.00000000000OA"},
        {0x41EEAFCB, "0.0.0.1", "b510e9b67edea54680cd9e7e572387077561e99f", "c721f4ee0df4900cda8cb7186b8cfb4217d78a6f",
         L"TU_16L61UJ_0000004000000.00000000000O1"},
        {0x509DBB6B, "0.0.0.3", nullptr, "5978a7a0c72f4d9ac18d263d6a647fe1d6f3d9ff", L"TU_16L61UJ_000000C000000.00000000000O3"},
        {0x2B35C136, "0.0.0.2", nullptr, "e43476e35d3ba5b3cfda881c94b60fe0be0f6662", L"TU_16L61UJ_0000008000000.00000000000O2"},
    };
    // SHA-1 of the loaded executable image (0x82000000, 0x13E0000 bytes) the build's code is recompiled from.
    constexpr const char *kImageSha1 = "4e57d5890f8b00803aa707a7453fbd14127ebd50";
    constexpr uint32_t kTitleId = 0x4D5307D3;
    constexpr const wchar_t *kTitle = L"PDZ Restored setup";
    constexpr int kStampVersion = 1;

    std::string g_summary; // logged once logging is up
    std::atomic<uint64_t> g_linked{0}; // pdz_setup_link: files hard-linked instead of copied
    bool g_ran = false;                // setup prepared the files at this start
    fs::path g_trace_file;  // <data dir>/setup.log while setup runs (before the game's own log exists)

    // One line to setup.log (and kept for the game log).
    void Trace(const std::string &line)
    {
      if (g_trace_file.empty())
      {
        return;
      }
      std::ofstream(g_trace_file, std::ios::binary | std::ios::app) << line << '\n';
    }

    std::wstring Widen(const std::string &s) { return fs::path(s).wstring(); }
    FILE *OpenFile(const wchar_t *path, const wchar_t *mode)
    {
      FILE *f = nullptr;
      return _wfopen_s(&f, path, mode) == 0 ? f : nullptr;
    }
    std::string Narrow(const fs::path &p) { return p.string(); }

    // ---- SHA-1 (Windows' own CNG) ----
    class Sha1
    {
    public:
      Sha1()
      {
        if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA1_ALGORITHM, nullptr, 0) == 0)
        {
          BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0);
        }
      }
      ~Sha1()
      {
        if (hash_)
        {
          BCryptDestroyHash(hash_);
        }
        if (alg_)
        {
          BCryptCloseAlgorithmProvider(alg_, 0);
        }
      }
      void Update(const void *data, size_t size)
      {
        if (hash_)
        {
          BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void *>(data)), ULONG(size), 0);
        }
      }
      std::string Hex()
      {
        uint8_t digest[20] = {};
        if (hash_)
        {
          BCryptFinishHash(hash_, digest, sizeof(digest), 0);
        }
        std::string s;
        for (uint8_t b : digest)
        {
          s += fmt::format("{:02x}", b);
        }
        return s;
      }

    private:
      BCRYPT_ALG_HANDLE alg_ = nullptr;
      BCRYPT_HASH_HANDLE hash_ = nullptr;
    };

    std::string Sha1Of(const std::vector<uint8_t> &data)
    {
      Sha1 h;
      h.Update(data.data(), data.size());
      return h.Hex();
    }

    std::optional<std::vector<uint8_t>> ReadHostFile(const fs::path &p)
    {
      std::ifstream f(p, std::ios::binary);
      if (!f)
      {
        return std::nullopt;
      }
      return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    // ---- XEX header: the media ID and version a default.xex was built for ----
    struct XexId
    {
      bool ok = false;
      uint32_t media_id = 0, version = 0, title_id = 0;
    };

    uint32_t Be32(const std::vector<uint8_t> &d, size_t at)
    {
      return at + 4 <= d.size() ? (uint32_t(d[at]) << 24) | (uint32_t(d[at + 1]) << 16) | (uint32_t(d[at + 2]) << 8) | d[at + 3]
                                : 0;
    }

    XexId ReadXexId(const std::vector<uint8_t> &d)
    {
      XexId id;
      if (d.size() < 0x18 || std::memcmp(d.data(), "XEX2", 4) != 0)
      {
        return id;
      }
      const uint32_t count = Be32(d, 0x14);
      for (uint32_t i = 0; i < count && i < 64; ++i)
      {
        if (Be32(d, 0x18 + i * 8) == 0x00040006) // XEX_HEADER_EXECUTION_INFO
        {
          const uint32_t at = Be32(d, 0x18 + i * 8 + 4);
          id.media_id = Be32(d, at);
          id.version = Be32(d, at + 4);
          id.title_id = Be32(d, at + 12);
          id.ok = true;
        }
      }
      return id;
    }

    std::string VersionText(uint32_t v)
    {
      return fmt::format("{}.{}.{}.{}", v >> 28, (v >> 24) & 0xF, (v >> 8) & 0xFFFF, v & 0xFF);
    }

    // ---- Sources: a disc (ISO or folder) and Title Update 3 (STFS package or folder) ----
    struct Item
    {
      std::string rel;   // path inside the source, backslashes
      uint64_t size = 0;
      rex::filesystem::Entry *entry = nullptr; // an ISO / STFS entry (else a host file)
      fs::path host;
    };

    class Source
    {
    public:
      fs::path path;
      std::unique_ptr<rex::filesystem::DiscImageDevice> disc;
      std::unique_ptr<rex::filesystem::StfsContainerDevice> stfs;
      fs::path folder;
      std::vector<Item> items;

      bool OpenIso(const fs::path &p)
      {
        path = p;
        disc = std::make_unique<rex::filesystem::DiscImageDevice>("\\Device\\PdzSetupDisc", p);
        if (!disc->Initialize())
        {
          disc.reset();
          return false;
        }
        Walk(const_cast<rex::filesystem::Entry *>(disc->root()), "");
        return true;
      }
      bool OpenStfs(const fs::path &p)
      {
        path = p;
        stfs = std::make_unique<rex::filesystem::StfsContainerDevice>("\\Device\\PdzSetupTu", p);
        if (!stfs->Initialize())
        {
          stfs.reset();
          return false;
        }
        rex::filesystem::Entry *root = stfs->ResolvePath("");
        if (!root)
        {
          return false;
        }
        Walk(root, "");
        return true;
      }
      void OpenFolder(const fs::path &dir)
      {
        path = folder = dir;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec))
        {
          if (it->is_regular_file(ec))
          {
            Item item;
            item.host = it->path();
            item.rel = fs::relative(it->path(), dir, ec).string();
            item.size = it->file_size(ec);
            items.push_back(item);
          }
        }
      }
      const Item *Find(const std::string &rel) const
      {
        for (const Item &i : items)
        {
          if (_stricmp(i.rel.c_str(), rel.c_str()) == 0)
          {
            return &i;
          }
        }
        return nullptr;
      }
      // Reads a whole item (small files: default.xex / default.xexp).
      std::optional<std::vector<uint8_t>> Read(const Item &item) const
      {
        if (!item.entry)
        {
          return ReadHostFile(item.host);
        }
        std::vector<uint8_t> data(item.size);
        rex::filesystem::File *file = nullptr;
        if (item.entry->Open(rex::filesystem::FileAccess::kFileReadData, &file) != 0 || !file)
        {
          return std::nullopt;
        }
        size_t got = 0;
        const bool ok = file->ReadSync(std::span<uint8_t>(data.data(), data.size()), 0, &got) == 0 && got == data.size();
        file->Destroy();
        return ok ? std::optional<std::vector<uint8_t>>(std::move(data)) : std::nullopt;
      }
      // Copies an item to a host file, in chunks, adding to `done` (bytes). False on an error or a cancel.
      bool Copy(const Item &item, const fs::path &to, std::atomic<uint64_t> &done, const std::atomic<bool> &cancel,
                std::string &error) const
      {
        std::error_code ec;
        fs::create_directories(to.parent_path(), ec);
        if (!item.entry)
        {
          if (fs::exists(to, ec) && fs::equivalent(item.host, to, ec))
          {
            done += item.size;
            return true; // already in place
          }
          if (REXCVAR_GET(pdz_setup_link))
          {
            fs::remove(to, ec); // a partial copy from an earlier, cancelled setup
            if (CreateHardLinkW(to.wstring().c_str(), item.host.wstring().c_str(), nullptr))
            {
              done += item.size;
              ++g_linked;
              return true;
            }
          }
        }
        FILE *out = OpenFile(to.wstring().c_str(), L"wb");
        if (!out)
        {
          error = "can't write " + to.string();
          return false;
        }
        std::vector<uint8_t> buf(8 << 20);
        bool ok = true;
        if (item.entry)
        {
          rex::filesystem::File *file = nullptr;
          if (item.entry->Open(rex::filesystem::FileAccess::kFileReadData, &file) != 0 || !file)
          {
            error = "can't open " + item.rel + " in " + path.string();
            ok = false;
          }
          for (uint64_t at = 0; ok && at < item.size;)
          {
            if (cancel)
            {
              ok = false;
              error = "cancelled";
              break;
            }
            const size_t n = size_t(std::min<uint64_t>(buf.size(), item.size - at));
            size_t got = 0;
            if (file->ReadSync(std::span<uint8_t>(buf.data(), n), at, &got) != 0 || got != n ||
                std::fwrite(buf.data(), 1, n, out) != n)
            {
              error = "read or write failed at " + item.rel;
              ok = false;
              break;
            }
            at += n;
            done += n;
          }
          if (file)
          {
            file->Destroy();
          }
        }
        else
        {
          FILE *in = OpenFile(item.host.wstring().c_str(), L"rb");
          if (!in)
          {
            error = "can't read " + item.host.string();
            ok = false;
          }
          while (ok)
          {
            if (cancel)
            {
              ok = false;
              error = "cancelled";
              break;
            }
            const size_t n = std::fread(buf.data(), 1, buf.size(), in);
            if (n == 0)
            {
              break;
            }
            if (std::fwrite(buf.data(), 1, n, out) != n)
            {
              error = "write failed: " + to.string();
              ok = false;
            }
            done += n;
          }
          if (in)
          {
            std::fclose(in);
          }
        }
        if (std::fclose(out) != 0 && ok)
        {
          error = "write failed: " + to.string();
          ok = false;
        }
        return ok;
      }

    private:
      void Walk(rex::filesystem::Entry *e, const std::string &prefix)
      {
        if (!e)
        {
          return;
        }
        for (const auto &child : e->children())
        {
          const std::string rel = prefix.empty() ? child->name() : prefix + "\\" + child->name();
          if (child->attributes() & rex::filesystem::kFileAttributeDirectory)
          {
            Walk(child.get(), rel);
          }
          else
          {
            Item item;
            item.rel = rel;
            item.size = child->size();
            item.entry = child.get();
            items.push_back(item);
          }
        }
      }
    };

    bool IsIso(const fs::path &p)
    {
      std::wstring e = p.extension().wstring();
      std::transform(e.begin(), e.end(), e.begin(), ::towlower);
      return e == L".iso" || e == L".xiso";
    }
    bool IsTuPackage(const fs::path &p)
    {
      std::wstring n = p.filename().wstring();
      std::transform(n.begin(), n.end(), n.begin(), ::towupper);
      return n.rfind(L"TU_", 0) == 0;
    }
    bool NameIs(const fs::path &p, const wchar_t *name) { return _wcsicmp(p.filename().wstring().c_str(), name) == 0; }

    // Looks in `dir` (and one level below) for a disc (ISO, or a folder with default.xex) and Title Update 3 (the
    // package as copied from an Xbox 360, or an extracted default.xexp).
    void Discover(const fs::path &dir, fs::path &disc, std::vector<fs::path> &tus)
    {
      std::error_code ec;
      if (!fs::is_directory(dir, ec))
      {
        return;
      }
      std::vector<fs::path> files;
      for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
           it.increment(ec))
      {
        if (it.depth() > 2)
        {
          it.disable_recursion_pending();
          continue;
        }
        if (it->is_regular_file(ec))
        {
          files.push_back(it->path());
        }
      }
      uint64_t best_iso = 0;
      for (const fs::path &f : files)
      {
        if (IsIso(f) && fs::file_size(f, ec) > best_iso)
        {
          best_iso = fs::file_size(f, ec);
          disc = f;
        }
      }
      if (disc.empty())
      {
        for (const fs::path &f : files)
        {
          if (NameIs(f, L"default.xex"))
          {
            disc = f;
            break;
          }
        }
      }
      for (const fs::path &f : files)
      {
        if (IsTuPackage(f) || NameIs(f, L"default.xexp"))
        {
          tus.push_back(f);
        }
      }
    }

    std::wstring PressingList()
    {
      std::wstring out;
      for (const Pressing &p : kPressings)
      {
        out += fmt::format(L"  media ID {:08X} (version {}), with {}\n", p.media_id, Widen(p.version), p.tu_file);
      }
      return out;
    }

    bool Writable(const fs::path &dir)
    {
      std::error_code ec;
      fs::create_directories(dir, ec);
      const fs::path probe = dir / ".pdz_write_probe";
      FILE *f = OpenFile(probe.wstring().c_str(), L"wb");
      if (!f)
      {
        return false;
      }
      std::fclose(f);
      fs::remove(probe, ec);
      return true;
    }

    bool PackageThere(const fs::path &update_root)
    {
      std::error_code ec;
      return fs::exists(update_root / "Pdz" / "Assets" / "PackageData" / "1726605008.pkg", ec);
    }

    bool Usable(const Paths &p)
    {
      std::error_code ec;
      return fs::exists(p.game_root / "default.xex", ec) && fs::exists(p.game_root / "default.xexp", ec) &&
             PackageThere(p.update_root);
    }

    // setup.json: the hashes and counts of what setup made.
    std::string StampField(const std::string &json, const std::string &key)
    {
      const std::string k = "\"" + key + "\": \"";
      const size_t at = json.find(k);
      if (at == std::string::npos)
      {
        return {};
      }
      const size_t end = json.find('"', at + k.size());
      return end == std::string::npos ? std::string() : json.substr(at + k.size(), end - at - k.size());
    }

    // The prepared data is there and unchanged: the stamp's hashes match the files now.
    bool StampValid(const fs::path &data_dir, std::string &why)
    {
      const auto stamp = ReadHostFile(data_dir / "setup.json");
      if (!stamp)
      {
        why = "no setup.json";
        return false;
      }
      const std::string json(stamp->begin(), stamp->end());
      const auto xex = ReadHostFile(data_dir / "game" / "default.xex");
      const auto xexp = ReadHostFile(data_dir / "game" / "default.xexp");
      if (!xex || !xexp || !PackageThere(data_dir / "update"))
      {
        why = "prepared files missing";
        return false;
      }
      if (StampField(json, "xex_sha1") != Sha1Of(*xex) || StampField(json, "xexp_sha1") != Sha1Of(*xexp) ||
          StampField(json, "setup_version") != std::to_string(kStampVersion))
      {
        why = "the files changed since setup";
        return false;
      }
      return true;
    }

    [[noreturn]] void Fail(const std::wstring &text)
    {
      Trace("stopped: " + fs::path(text).string());
      const int shown = REXCVAR_GET(pdz_setup_quiet) ? -1 : setup_ui::Error(kTitle, text);
      Trace("message box result " + std::to_string(shown) + " (last error " + std::to_string(GetLastError()) + ")");
      std::exit(2);
    }

    bool Ask(const std::wstring &text)
    {
      const bool ok = !REXCVAR_GET(pdz_setup_quiet) && setup_ui::AskOkCancel(kTitle, text);
      Trace(std::string("asked: ") + (ok ? "OK" : "Cancel") + ": " + fs::path(text.substr(0, 60)).string());
      return ok;
    }

    std::wstring Mb(uint64_t bytes) { return fmt::format(L"{:.1f} GB", double(bytes) / (1024.0 * 1024.0 * 1024.0)); }

    // The whole setup: find, check, extract, stamp. Exits on a problem the player must fix.
    Paths RunSetup(const fs::path &exe_dir, const fs::path &data_dir, const std::string &reason)
    {
      const auto t0 = std::chrono::steady_clock::now();
      // 1. Find the disc and the title update.
      fs::path disc_path, tu_path;
      std::vector<fs::path> tus;
      const std::string src = REXCVAR_GET(pdz_setup_source);
      const fs::path source_dir = src.empty() ? exe_dir / "game" : fs::path(src);
      Discover(source_dir, disc_path, tus);
      Trace("setup started (" + reason + "); looked in " + source_dir.string() + ": disc '" + disc_path.string() +
            "', " + std::to_string(tus.size()) + " title update candidate(s)");
      if (disc_path.empty())
      {
        if (!Ask(L"PDZ Restored needs your own copy of Perfect Dark Zero for Xbox 360.\n\nPut your disc image and "
                 L"Title Update 3 in the game folder next to PDZRestored.exe, then start again.\n\nOr press OK to pick "
                 L"the disc image now. An .iso file works, or the default.xex of an extracted disc."))
        {
          std::exit(2);
        }
        if (!setup_ui::PickFile(L"Choose your Perfect Dark Zero disc image",
                                {{L"Disc image or default.xex", L"*.iso;*.xiso;default.xex"}, {L"All files", L"*.*"}},
                                disc_path))
        {
          std::exit(2);
        }
      }
      // 2. Open and check them.
      Source disc, tu;
      if (IsIso(disc_path))
      {
        if (!disc.OpenIso(disc_path))
        {
          Trace("not a readable disc image: " + disc_path.string());
          Fail(L"PDZ Restored can't read this file as an Xbox 360 disc image. Pick the .iso of your disc, or the "
               L"default.xex of an extracted disc.");
        }
      }
      else
      {
        disc.OpenFolder(disc_path.parent_path());
      }
      Trace("disc opened: " + std::to_string(disc.items.size()) + " files" + (disc.disc ? " (disc image)" : " (folder)"));
      const Item *xex_item = disc.Find("default.xex");
      const auto xex = xex_item ? disc.Read(*xex_item) : std::nullopt;
      if (!xex)
      {
        Trace("no default.xex in " + disc_path.string());
        Fail(L"There's no default.xex in what you picked. Is it a Perfect Dark Zero disc?");
      }
      const std::string xex_sha1 = Sha1Of(*xex);
      Trace("default.xex SHA-1 " + xex_sha1);
      const XexId id = ReadXexId(*xex);
      const Pressing *pressing = nullptr;
      for (const Pressing &p : kPressings)
      {
        const bool by_hash = p.xex_sha1 && xex_sha1 == p.xex_sha1;
        const bool by_header = !p.xex_sha1 && id.ok && id.title_id == kTitleId && id.media_id == p.media_id &&
                               VersionText(id.version) == p.version;
        if (by_hash || by_header)
        {
          pressing = &p;
        }
      }
      if (!pressing)
      {
        Trace(fmt::format("unknown disc {}: title {:08X}, media ID {:08X}, version {}, default.xex SHA-1 {}",
                          disc_path.string(), id.title_id, id.media_id, VersionText(id.version), xex_sha1));
        Fail(id.ok && id.title_id == kTitleId
                 ? std::wstring(L"This is Perfect Dark Zero, but not a disc PDZ Restored knows. Use one of the five "
                                L"retail discs, as it came off the disc.")
                 : std::wstring(L"This disc isn't Perfect Dark Zero, or it has been changed. Use one of the five retail "
                                L"Perfect Dark Zero discs."));
      }
      Trace(fmt::format("disc: media ID {:08X}, version {}{}", pressing->media_id, pressing->version,
                        pressing->xex_sha1 ? "" : " (by its header; the image is checked at load)"));
      const wchar_t *kTuFile = pressing->tu_file;
      int best_rank = -1;
      for (const fs::path &t : tus)
      {
        // The one named for this disc, then another disc's TU3 (any works), then whatever else is there.
        int rank = NameIs(t, kTuFile) ? 2 : 0;
        for (const Pressing &p : kPressings)
        {
          rank = std::max(rank, NameIs(t, p.tu_file) ? 1 : 0);
        }
        if (rank > best_rank)
        {
          best_rank = rank;
          tu_path = t;
        }
      }
      if (tu_path.empty())
      {
        if (!Ask(L"Now pick Title Update 3, copied from your own Xbox 360. Any disc's Title Update 3 works: its file "
                 L"name usually starts with TU_16L61UJ_. You can also pick the default.xexp of an extracted one."))
        {
          std::exit(2);
        }
        if (!setup_ui::PickFile(L"Choose Title Update 3",
                                {{L"Title Update 3", std::wstring(L"TU_*;default.xexp")}, {L"All files", L"*.*"}},
                                tu_path))
        {
          std::exit(2);
        }
      }


      std::string tu_kind;
      if (NameIs(tu_path, L"default.xexp"))
      {
        tu.OpenFolder(tu_path.parent_path());
        tu_kind = "extracted folder";
      }
      else if (tu.OpenStfs(tu_path))
      {
        tu_kind = "package";
        // (A title update package's header names the title; its media ID is 0 there. The xexp below has it.)
        const auto &info = tu.stfs->header().metadata.execution_info;
        if (uint32_t(info.title_id) != kTitleId)
        {
          Trace(fmt::format("title update for another game: title {:08X} ({})", uint32_t(info.title_id),
                            tu_path.string()));
          Fail(L"This title update is for another game. PDZ Restored needs Perfect Dark Zero's Title Update 3, copied "
               L"from your own Xbox 360. Any disc's Title Update 3 works: its file name usually starts with TU_16L61UJ_.");
        }
      }
      else
      {
        Trace("not a readable title update: " + tu_path.string());
        Fail(L"PDZ Restored can't read this file as a title update. Pick your Title Update 3 file, copied from your "
             L"own Xbox 360, or the default.xexp of an extracted one.");
      }
      Trace("title update opened (" + tu_kind + "): " + std::to_string(tu.items.size()) + " files");
      const Item *xexp_item = tu.Find("default.xexp");
      const auto xexp = xexp_item ? tu.Read(*xexp_item) : std::nullopt;
      const std::string xexp_sha1 = xexp ? Sha1Of(*xexp) : std::string();
      const Pressing *tu_of = nullptr; // the disc this TU3 was made for (any of the five works)
      for (const Pressing &p : kPressings)
      {
        if (xexp_sha1 == p.xexp_sha1)
        {
          tu_of = &p;
        }
      }
      if (!tu_of)
      {
        const XexId id = xexp ? ReadXexId(*xexp) : XexId{};
        const uint32_t update = id.ok ? (id.version >> 8) & 0xFFFF : 0;
        Trace(fmt::format("not a known Title Update 3: {} (title {:08X}, media ID {:08X}, version {}, default.xexp "
                          "SHA-1 {})",
                          tu_path.string(), id.title_id, id.media_id, VersionText(id.version), xexp_sha1));
        const std::wstring what =
            !xexp                        ? std::wstring(L"This one has no default.xexp in it.")
            : !id.ok                     ? std::wstring(L"This one isn't a title update PDZ Restored knows.")
            : id.title_id != kTitleId    ? std::wstring(L"This one is for another game.")
            : update != 3 && update != 0 ? fmt::format(L"This one is Title Update {}.", update)
                                         : std::wstring(L"This one isn't a known Title Update 3.");
        Fail(fmt::format(L"PDZ Restored needs Perfect Dark Zero's Title Update 3, copied from your own Xbox 360. {} "
                         L"Any disc's Title Update 3 works: its file name usually starts with TU_16L61UJ_.",
                         what));
      }
      Trace("default.xexp SHA-1 " + xexp_sha1 +
            (tu_of == pressing ? std::string(" (this disc's own Title Update 3)")
                               : fmt::format(" (the Title Update 3 of media ID {:08X}; any TU3 gives the same image)",
                                             tu_of->media_id)));

      // 3. What to copy where. The disc's own system update folder ($SystemUpdate) isn't needed.
      struct Job
      {
        const Source *from;
        const Item *item;
        fs::path to;
      };
      std::vector<Job> jobs;
      uint64_t total = 0;
      for (const Item &i : disc.items)
      {
        if (i.rel.rfind("$", 0) == 0 || _stricmp(i.rel.c_str(), "default.xexp") == 0)
        {
          continue;
        }
        jobs.push_back({&disc, &i, data_dir / "game" / i.rel});
        total += i.size;
      }
      size_t update_files = 0;
      for (const Item &i : tu.items)
      {
        const bool xexp_file = &i == xexp_item;
        if (!xexp_file && (tu_kind == "extracted folder" && !(i.rel.size() > 4 && _strnicmp(i.rel.c_str(), "Pdz\\", 4) == 0)))
        {
          continue; // an extracted folder: only default.xexp and Pdz\...
        }
        jobs.push_back({&tu, &i, xexp_file ? data_dir / "game" / "default.xexp" : data_dir / "update" / i.rel});
        total += i.size;
        update_files += xexp_file ? 0 : 1;
      }
      ULARGE_INTEGER free_bytes{};
      std::error_code ec;
      fs::create_directories(data_dir, ec);
      if (GetDiskFreeSpaceExW(data_dir.wstring().c_str(), &free_bytes, nullptr, nullptr) &&
          free_bytes.QuadPart < total + (256ull << 20))
      {
        Trace("not enough space in " + data_dir.string());
        Fail(L"There isn't enough free space for the game files. They need " + Mb(total + (256ull << 20)) +
             L", and the drive has " + Mb(free_bytes.QuadPart) + L" free. Free up some space, then start again.");
      }

      // 4. Extract, with progress.
      fs::remove(data_dir / "setup.json", ec); // a stamp only after everything is in place
      Trace("extracting " + std::to_string(jobs.size()) + " files, " + std::to_string(total) + " bytes into " +
            data_dir.string());
      setup_ui::State state;
      std::atomic<uint64_t> done{0};
      std::string error;
      size_t files_done = 0;
      // One ticker for the whole job moves the bar (within large files too); the worker names the file.
      std::atomic<size_t> files_atomic{0};
      setup_ui::RunWithProgress(kTitle, state, [&](setup_ui::State &s) {
        std::atomic<bool> copying{true};
        std::thread ticker([&] {
          while (copying)
          {
            {
              std::lock_guard<std::mutex> lock(s.mutex);
              s.progress = total ? double(done) / double(total) : 0.0;
              s.line1 = fmt::format(L"Preparing the game files ({} of {}): {} of {}",
                                    std::min(files_atomic.load() + 1, jobs.size()), jobs.size(), Mb(done), Mb(total));
            }
            Sleep(100);
          }
        });
        for (const Job &j : jobs)
        {
          {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.line2 = Widen(j.item->rel);
          }
          if (!j.from->Copy(*j.item, j.to, done, s.cancel, error))
          {
            break;
          }
          files_atomic = ++files_done;
        }
        copying = false;
        ticker.join();
      });
      Trace("extraction ended: " + std::to_string(files_done) + " files, " + std::to_string(uint64_t(done)) + " bytes" +
            (error.empty() ? std::string() : ", error: " + error));
      if (!error.empty())
      {
        Fail(error == "cancelled" ? std::wstring(L"Setup was cancelled. It'll start again next time you open PDZ "
                                                 L"Restored.")
                                  : std::wstring(L"Setup couldn't prepare the game files. Check that they aren't damaged, "
                                                 L"then start again. The details are in the setup log."));
      }
      Paths out{data_dir / "game", data_dir / "update"};
      if (!Usable(out))
      {
        Trace("the Title Update 3 data package is missing from " + tu_path.string());
        Fail(L"Setup finished, but your Title Update 3 is missing its data package. Pick your complete Title Update 3 "
             L"file, copied from your own Xbox 360.");
      }
      const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      g_ran = true;
      // (Paths with forward slashes: valid JSON without escaping.)
      std::ofstream stamp(data_dir / "setup.json", std::ios::binary);
      stamp << "{\n"
            << "  \"setup_version\": \"" << kStampVersion << "\",\n"
            << "  \"xex_sha1\": \"" << xex_sha1 << "\",\n"
            << "  \"xexp_sha1\": \"" << xexp_sha1 << "\",\n"
            << "  \"media_id\": \"" << fmt::format("{:08X}", pressing->media_id) << "\",\n"
            << "  \"disc\": \"" << disc_path.generic_string() << "\",\n"
            << "  \"title_update\": \"" << tu_path.generic_string() << "\",\n"
            << "  \"files\": \"" << jobs.size() << "\",\n"
            << "  \"update_files\": \"" << update_files << "\",\n"
            << "  \"bytes\": \"" << total << "\",\n"
            << "  \"seconds\": \"" << fmt::format("{:.1f}", secs) << "\"\n"
            << "}\n";
      stamp.close();
      Trace(fmt::format("done in {:.1f} s", secs));
      g_summary = fmt::format("pdz_setup: first-run setup ({}): disc {} ({}), title update {} ({}): {} files, {:.2f} GB "
                              "into {} in {:.1f} s ({} hard-linked, the rest copied)",
                              reason, disc_path.string(), disc.disc ? "disc image" : "extracted folder", tu_path.string(),
                              tu_kind, jobs.size(), double(total) / (1024.0 * 1024.0 * 1024.0), data_dir.string(), secs,
                              g_linked.load());
      return out;
    }
  } // namespace

  Paths Prepare(const fs::path &exe_dir, const fs::path &user_data_root, const Paths &current, bool explicit_paths)
  {
    if (REXCVAR_GET(pdz_settings_test_dpi) > 0)
    {
      theme::SetTestDpi(REXCVAR_GET(pdz_settings_test_dpi)); // the setup windows and message boxes too
    }
    if (!REXCVAR_GET(pdz_setup_test_capture).empty())
    {
      setup_ui::TestCapture(fs::u8path(REXCVAR_GET(pdz_setup_test_capture)), kTitle, kTitle,
                            L"Now pick Title Update 3, copied from your own Xbox 360. Any disc's Title Update 3 "
                            L"works: its file name usually starts with TU_16L61UJ_. You can also pick the default.xexp "
                            L"of an extracted one.");
    }
    const std::string mode = REXCVAR_GET(pdz_setup);
    if (mode == "off" || explicit_paths)
    {
      g_summary = fmt::format("pdz_setup: setup {} (game {}, update {})",
                              mode == "off" ? "off (pdz_setup)" : "skipped: paths given on the command line",
                              current.game_root.string(), current.update_root.string());
      return current;
    }
    const bool force = mode == "force";
    if (!force && Usable(current))
    {
      g_summary = fmt::format("pdz_setup: game files found (game {}, update {}): no setup needed",
                              current.game_root.string(), current.update_root.string());
      return current;
    }
    const std::string dir_cvar = REXCVAR_GET(pdz_setup_data_dir);
    const fs::path data_dir = !dir_cvar.empty()           ? fs::path(dir_cvar)
                              : Writable(exe_dir / "data") ? exe_dir / "data"
                                                           : user_data_root / "data";
    std::string why;
    if (!force && StampValid(data_dir, why))
    {
      g_summary = fmt::format("pdz_setup: prepared game files in {} (setup.json checked)", data_dir.string());
      return {data_dir / "game", data_dir / "update"};
    }
    std::error_code ec;
    fs::create_directories(data_dir, ec);
    g_trace_file = data_dir / "setup.log";
    return RunSetup(exe_dir, data_dir, force ? std::string("forced, pdz_setup") : why);
  }

  void VerifyImage(const uint8_t *image, size_t size, const fs::path &game_root)
  {
    Sha1 h;
    h.Update(image, size);
    const std::string got = h.Hex();
    if (got == kImageSha1)
    {
      REXLOG_INFO("pdz_setup: executable image checked (SHA-1 {}): the one PDZ Restored is made from", got);
      return;
    }
    REXLOG_ERROR("pdz_setup: the loaded executable image is not the one PDZ Restored is made from (SHA-1 {}, expected "
                 "{}; game files {})",
                 got, kImageSha1, game_root.string());
    // A prepared folder with this result is not used again: setup runs again at the next start.
    std::error_code ec;
    if (fs::exists(game_root.parent_path() / "setup.json", ec) && fs::exists(game_root.parent_path() / "update", ec))
    {
      fs::rename(game_root.parent_path() / "setup.json", game_root.parent_path() / "setup.json.rejected", ec);
    }
    if (!REXCVAR_GET(pdz_setup_quiet))
    {
      setup_ui::Error(kTitle, L"This disc and title update don't make the game PDZ Restored was built from, so it can't "
                              L"run. Use a retail Perfect Dark Zero disc with Title Update 3.");
    }
    std::exit(2);
  }

  bool SetupRanThisStart() { return g_ran; }

  void LogSummary()
  {
    if (!g_summary.empty())
    {
      REXLOG_INFO("{}", g_summary);
    }
  }
} // namespace pdz::first_run
