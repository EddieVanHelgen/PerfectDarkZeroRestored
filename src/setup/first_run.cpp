// PDZ Restored: first-run setup (first_run.h).
#include "first_run.h"

#include "map_packs.h"

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
REXCVAR_DEFINE_STRING(pdz_setup_test_media_id, "", "Testing",
                      "TESTING TOOL: setup treats the disc's default.xex as if its header said this media ID (8 hex "
                      "digits, e.g. 40C97944) and doesn't match it by its SHA-1, so a known disc goes the way of a disc "
                      "setup doesn't know (accepted by its header, its own code checked at the first load). Empty = off");
REXCVAR_DEFINE_BOOL(pdz_setup_test_image_mismatch, false, "Testing",
                    "TESTING TOOL: the image check at load reports a mismatch whatever the image is, so the message "
                    "for a disc with different game code is shown (and setup.json set aside, as for a real one)");
REXCVAR_DEFINE_BOOL(pdz_setup_restarted, false, "Setup",
                    "Set by setup itself when it restarts PDZ Restored once after adding Title Update 3 to a disc it "
                    "didn't know (the settings window isn't shown again). Not for players");
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
    // 0.9.2.1: a disc that isn't in this list (or is listed without a TU3 of its own) is still accepted when its
    // default.xex is a valid XEX for Perfect Dark Zero (title 4D5307D3). Its own code is then checked at the first
    // load (VerifyImage): the same as kImageSha1 = no title update needed; otherwise a Title Update 3 (the disc's own
    // if it carries one, else any) is added and PDZ Restored restarts once, and the final code is checked again.
    struct Pressing
    {
      uint32_t media_id;
      const char *version;
      const char *xex_sha1;  // nullptr: no disc of it seen (accepted by its header, then VerifyImage)
      const char *xexp_sha1; // nullptr: no Title Update 3 of its own known
      const wchar_t *tu_file; // nullptr: the same (its code is checked at the first load)
      bool not_supported = false; // a known disc whose code PDZ Restored can't run: refused before anything is copied
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
        // The US "Platinum Hits" reprint (7 Nov 2026, a dump of a real disc; docs/RELEASE_FIRST_RUN.md "The US
        // Platinum Hits"): known, NOT supported. Its own code is a full-size image like TU3's (SHA-1 e3048eb7...) but
        // a different build (linked 12 Sep 2006, TU3's 19 Sep; the code moved from 8214030E on), and TU3's patch
        // can't be applied to it (the SDK's patcher crashed). The map packs are merged into its game files.
        {0x40C97944, "0.0.0.15", "29470e582ff8497f7465d475ed9b819389c83f12", nullptr, nullptr, true},
    };
    // The message for a disc whose game code PDZ Restored can't run (setup's log path follows on its own line).
    constexpr const wchar_t *kNotSupported = L"This version of the game is not supported.";
    // setup.json's image_check after TU3 was added at the first load, and once the restart's image check passed. A
    // stamp still at kTu3Added at a normal start means the restart never reached the check (Prepare sets it aside).
    constexpr const char *kTu3Added = "with Title Update 3 (added at the first load)";
    constexpr const char *kTu3Checked = "with Title Update 3 (added at the first load, checked)";
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

    // setup.log is copied to logs\setup.log next to PDZRestored.exe (easy to find); messages name that copy.
    fs::path LogCopyPath() { return rex::filesystem::GetExecutableFolder() / "logs" / "setup.log"; }
    void CopyTraceToLogs()
    {
      if (g_trace_file.empty())
      {
        return;
      }
      std::error_code ec;
      fs::create_directories(LogCopyPath().parent_path(), ec);
      fs::copy_file(g_trace_file, LogCopyPath(), fs::copy_options::overwrite_existing, ec);
    }
    std::wstring LogWhere()
    {
      return L"\n\nYour setup log is here: " + LogCopyPath().wstring() + L" (and " + g_trace_file.wstring() + L").";
    }
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
      // The first `n` bytes of an item (a package's header).
      std::vector<uint8_t> ReadHead(const Item &item, size_t n) const
      {
        n = size_t(std::min<uint64_t>(n, item.size));
        std::vector<uint8_t> data(n);
        if (!item.entry)
        {
          FILE *f = OpenFile(item.host.wstring().c_str(), L"rb");
          const size_t got = f ? std::fread(data.data(), 1, n, f) : 0;
          if (f)
          {
            std::fclose(f);
          }
          data.resize(got);
          return data;
        }
        rex::filesystem::File *file = nullptr;
        if (item.entry->Open(rex::filesystem::FileAccess::kFileReadData, &file) != 0 || !file)
        {
          return {};
        }
        size_t got = 0;
        file->ReadSync(std::span<uint8_t>(data.data(), data.size()), 0, &got);
        file->Destroy();
        data.resize(got);
        return data;
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
        out += fmt::format(L"  media ID {:08X} (version {}), with {}\n", p.media_id, Widen(p.version),
                           p.tu_file ? p.tu_file : L"(no title update of its own known)");
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

    bool Usable(const Paths &p, bool need_xexp = true)
    {
      std::error_code ec;
      return fs::exists(p.game_root / "default.xex", ec) && (!need_xexp || fs::exists(p.game_root / "default.xexp", ec)) &&
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
      const std::string want_xexp = StampField(json, "xexp_sha1"); // empty: the disc's own code, no title update
      if (!xex || (!want_xexp.empty() && !xexp) || !PackageThere(data_dir / "update"))
      {
        why = "prepared files missing";
        return false;
      }
      if (StampField(json, "xex_sha1") != Sha1Of(*xex) || (!want_xexp.empty() && want_xexp != Sha1Of(*xexp)) ||
          StampField(json, "setup_version") != std::to_string(kStampVersion))
      {
        why = "the files changed since setup";
        return false;
      }
      return true;
    }

    [[noreturn]] void Fail(const std::wstring &text_in)
    {
      const std::wstring text = text_in + LogWhere();
      Trace("stopped: " + fs::path(text).string());
      CopyTraceToLogs();
      const int shown = REXCVAR_GET(pdz_setup_quiet) ? -1 : setup_ui::Error(kTitle, text);
      Trace("message box result " + std::to_string(shown) + " (last error " + std::to_string(GetLastError()) + ")");
      CopyTraceToLogs();
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
    // The top-level names of the known discs (the original US disc's ISO and the extracted 750FF1D9 / 41EEAFCB
    // discs): anything else at the top of a disc is logged by name and size.
    bool KnownTopLevel(const std::string &name)
    {
      static const char *kNames[] = {"default.xex", "fonts", "pdz", "savethumbnail.png", "xetvconfig.ini",
                                     "$systemupdate", "nxeart", "avatarawards"};
      for (const char *n : kNames)
      {
        if (_stricmp(name.c_str(), n) == 0)
        {
          return true;
        }
      }
      return false;
    }

    // An Xbox 360 content package (STFS: CON / LIVE / PIRS) inside a disc: its content type and title.
    struct DiscPackage
    {
      const Item *item = nullptr;
      uint32_t content_type = 0, title_id = 0;
    };

    Paths RunSetup(const fs::path &exe_dir, const fs::path &data_dir, const fs::path &user_data_root,
                   const std::string &reason)
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
      XexId id = ReadXexId(*xex);
      const std::string test_media = REXCVAR_GET(pdz_setup_test_media_id);
      if (!test_media.empty() && id.ok)
      {
        id.media_id = uint32_t(std::strtoul(test_media.c_str(), nullptr, 16));
        Trace(fmt::format("TESTING (pdz_setup_test_media_id): the disc is treated as media ID {:08X}, not matched by "
                          "its SHA-1",
                          id.media_id));
      }
      const Pressing *pressing = nullptr;
      for (const Pressing &p : kPressings)
      {
        const bool by_hash = test_media.empty() && p.xex_sha1 && xex_sha1 == p.xex_sha1;
        const bool by_header = !p.xex_sha1 && id.ok && id.title_id == kTitleId && id.media_id == p.media_id &&
                               VersionText(id.version) == p.version;
        if (by_hash || by_header)
        {
          pressing = &p;
        }
      }
      if (!id.ok || id.title_id != kTitleId)
      {
        Trace(fmt::format("not Perfect Dark Zero: {}: header {}, title {:08X}, media ID {:08X}, version {}, default.xex "
                          "SHA-1 {}",
                          disc_path.string(), id.ok ? "read" : "not a valid XEX2 header", id.title_id, id.media_id,
                          VersionText(id.version), xex_sha1));
        Fail(L"This disc isn't Perfect Dark Zero, or it has been changed. Use a Perfect Dark Zero disc, as it came off "
             L"the disc.");
      }
      // A disc setup doesn't know (or knows only from a player's report, with no title update of its own): its own
      // code is checked at the first load (VerifyImage), before any title update is applied.
      const bool check_at_load = !pressing || !pressing->tu_file;
      if (!pressing)
      {
        Trace(fmt::format("UNKNOWN DISC, accepted by its header: title {:08X}, media ID {:08X}, version {}, "
                          "default.xex SHA-1 {} ({})",
                          id.title_id, id.media_id, VersionText(id.version), xex_sha1, disc_path.string()));
      }
      else if (pressing->not_supported)
      {
        // Known, and known not to run: stopped before anything is copied (no title update is added to it).
        Trace(fmt::format("disc: media ID {:08X}, version {}, default.xex SHA-1 {}: a known disc that PDZ Restored "
                          "doesn't support (its game code is a different build from the one PDZ Restored is made "
                          "from; Title Update 3 can't be applied to it)",
                          pressing->media_id, pressing->version, xex_sha1));
        Fail(kNotSupported);
      }
      else
      {
        Trace(fmt::format("disc: media ID {:08X}, version {}{}", pressing->media_id, pressing->version,
                          !pressing->tu_file  ? " (known only from a player's report, not verified)"
                          : pressing->xex_sha1 ? ""
                                               : " (by its header; the image is checked at load)"));
      }
      Trace(check_at_load ? std::string("its own code is checked at the first load, before any title update: the same "
                                        "as the code PDZ Restored is built from = no title update needed")
                          : std::string("a known disc: its own code needs Title Update 3 (checked for 6A78E206, 750FF1D9 "
                                        "and 41EEAFCB on 3 Nov 2026)"));
      const uint32_t media_id = pressing ? pressing->media_id : id.media_id;
      const std::string version_text = pressing ? std::string(pressing->version) : VersionText(id.version);

      // What the disc has that the known discs don't: top-level names (with sizes) and content packages.
      {
        std::vector<std::pair<std::string, std::pair<uint64_t, size_t>>> tops; // name -> bytes, files
        for (const Item &i : disc.items)
        {
          const std::string top = i.rel.substr(0, i.rel.find('\\'));
          auto it = std::find_if(tops.begin(), tops.end(), [&](const auto &t) { return _stricmp(t.first.c_str(), top.c_str()) == 0; });
          if (it == tops.end())
          {
            tops.push_back({top, {0, 0}});
            it = tops.end() - 1;
          }
          it->second.first += i.size;
          ++it->second.second;
        }
        std::string all;
        for (const auto &t : tops)
        {
          all += fmt::format("{}{} ({} files, {} bytes)", all.empty() ? "" : ", ", t.first, t.second.second,
                             t.second.first);
          if (!KnownTopLevel(t.first))
          {
            Trace(fmt::format("disc extra (not on the known discs): top-level '{}': {} files, {} bytes", t.first,
                              t.second.second, t.second.first));
          }
        }
        Trace("disc top level: " + all);
      }
      std::vector<DiscPackage> packages;
      for (const Item &i : disc.items)
      {
        if (i.size < 0x1000)
        {
          continue;
        }
        const std::vector<uint8_t> head = disc.ReadHead(i, 0x400);
        if (head.size() < 0x364 || (std::memcmp(head.data(), "CON ", 4) != 0 && std::memcmp(head.data(), "LIVE", 4) != 0 &&
                                    std::memcmp(head.data(), "PIRS", 4) != 0))
        {
          continue;
        }
        DiscPackage pk;
        pk.item = &i;
        pk.content_type = Be32(head, 0x344);
        pk.title_id = Be32(head, 0x360);
        packages.push_back(pk);
        Trace(fmt::format("disc package: {} ({} bytes, {}, content type {:08X}{}, title {:08X})", i.rel, i.size,
                          std::string(reinterpret_cast<const char *>(head.data()), 4), pk.content_type,
                          pk.content_type == 0x000B0000   ? " = a title update"
                          : pk.content_type == 0x00000002 ? " = downloadable content (a map pack?)"
                                                          : "",
                          pk.title_id));
      }
      if (packages.empty())
      {
        Trace("disc packages: none (no CON / LIVE / PIRS package on the disc)");
      }

      // A title update the disc carries itself (a package of content type 000B0000 for this title, or a TU_ file):
      // extracted next to the prepared files and preferred over the game folder's.
      std::vector<fs::path> disc_tus;
      for (const Item &i : disc.items)
      {
        const DiscPackage *pk = nullptr;
        for (const DiscPackage &d : packages)
        {
          pk = d.item == &i ? &d : pk;
        }
        const bool tu_package = pk && pk->content_type == 0x000B0000 && pk->title_id == kTitleId;
        if (!tu_package && !(pk && IsTuPackage(fs::path(i.rel))))
        {
          continue;
        }
        const fs::path to = data_dir / "disc_tu" / fs::path(i.rel).filename();
        std::atomic<uint64_t> done{0};
        std::atomic<bool> cancel{false};
        std::string error;
        if (disc.Copy(i, to, done, cancel, error))
        {
          Trace("the disc's own title update: " + i.rel + " -> " + to.string());
          disc_tus.push_back(to);
        }
        else
        {
          Trace("the disc's own title update " + i.rel + " couldn't be extracted: " + error);
        }
      }
      tus.insert(tus.begin(), disc_tus.begin(), disc_tus.end());
      const wchar_t *kTuFile = pressing ? pressing->tu_file : nullptr;
      int best_rank = -1;
      for (const fs::path &t : tus)
      {
        // The disc's own, then the one named for this disc, then another disc's TU3 (any works), then whatever else.
        int rank = std::find(disc_tus.begin(), disc_tus.end(), t) != disc_tus.end() ? 3
                   : kTuFile && NameIs(t, kTuFile)                                ? 2
                                                                                  : 0;
        for (const Pressing &p : kPressings)
        {
          rank = std::max(rank, p.tu_file && NameIs(t, p.tu_file) ? 1 : 0);
        }
        if (rank > best_rank)
        {
          best_rank = rank;
          tu_path = t;
        }
      }
      // Title Update 3's data package (the file TU3's code reads at boot) on the disc itself: then a disc whose own code
      // turns out to need no title update can do without one.
      const Item *disc_tu_data = nullptr;
      for (const Item &i : disc.items)
      {
        if (_stricmp(fs::path(i.rel).filename().string().c_str(), "1726605008.pkg") == 0)
        {
          disc_tu_data = &i;
          Trace("the disc has Title Update 3's data package itself: " + i.rel + " (" + std::to_string(i.size) +
                " bytes)");
        }
      }
      if (tu_path.empty() && check_at_load && disc_tu_data)
      {
        Trace("no title update found: going on without one (the disc has TU3's data package; its own code is checked "
              "at the first load)");
      }
      else if (tu_path.empty())
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


      std::string tu_kind = "none";
      const Item *xexp_item = nullptr;
      std::optional<std::vector<uint8_t>> xexp;
      std::string xexp_sha1;
      const Pressing *tu_of = nullptr; // the disc this TU3 was made for (any of them works)
      if (tu_path.empty())
      {
        Trace("no title update");
      }
      else
      {
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
      xexp_item = tu.Find("default.xexp");
      xexp = xexp_item ? tu.Read(*xexp_item) : std::nullopt;
      xexp_sha1 = xexp ? Sha1Of(*xexp) : std::string();
      for (const Pressing &p : kPressings)
      {
        if (p.xexp_sha1 && xexp_sha1 == p.xexp_sha1)
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
                                             tu_of->media_id)) +
            (check_at_load ? " - held back in pending\\ until the disc's own code has been checked at the first load"
                           : ""));
      } // a title update

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
        jobs.push_back({&tu, &i,
                        xexp_file ? data_dir / (check_at_load ? "pending" : "game") / "default.xexp"
                                  : data_dir / "update" / i.rel});
        total += i.size;
        update_files += xexp_file ? 0 : 1;
      }
      if (tu_path.empty() && disc_tu_data)
      {
        jobs.push_back({&disc, disc_tu_data, data_dir / "update" / "Pdz" / "Assets" / "PackageData" / "1726605008.pkg"});
        total += disc_tu_data->size;
        update_files = 1;
      }
      // Map packs on the disc (downloadable-content packages for this title): installed after the copy, as packs
      // copied from a console are (map_packs.h). Those in a $ folder aren't copied with the disc's files, so they go
      // to disc_content\.
      std::vector<fs::path> disc_packs;
      for (const DiscPackage &pk : packages)
      {
        if (pk.content_type != 0x00000002 || pk.title_id != kTitleId)
        {
          continue;
        }
        if (pk.item->rel.rfind("$", 0) == 0)
        {
          const fs::path to = data_dir / "disc_content" / fs::path(pk.item->rel).filename();
          jobs.push_back({&disc, pk.item, to});
          total += pk.item->size;
          disc_packs.push_back(to);
        }
        else
        {
          disc_packs.push_back(data_dir / "game" / pk.item->rel);
        }
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
      // An earlier setup's default.xexp must not stay: a disc checked at the first load is loaded without one.
      fs::remove(data_dir / "game" / "default.xexp", ec);
      fs::remove(data_dir / "pending" / "default.xexp", ec);
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
      for (const fs::path &pack : disc_packs)
      {
        int index = -1;
        std::string log;
        const bool ok = map_packs::Install(pack, map_packs::TitleRoot(user_data_root), index, log);
        Trace(fmt::format("map pack from the disc: {}: {} ({})", pack.string(), ok ? "installed" : "not installed", log));
      }
      if (!Usable(out, !check_at_load))
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
            << "  \"xexp_sha1\": \"" << (check_at_load ? std::string() : xexp_sha1) << "\",\n"
            << "  \"pending_xexp_sha1\": \"" << (check_at_load ? xexp_sha1 : std::string()) << "\",\n"
            << "  \"image_check\": \"" << (check_at_load ? "pending" : "known disc") << "\",\n"
            << "  \"media_id\": \"" << fmt::format("{:08X}", media_id) << "\",\n"
            << "  \"version\": \"" << version_text << "\",\n"
            << "  \"disc\": \"" << disc_path.generic_string() << "\",\n"
            << "  \"title_update\": \"" << tu_path.generic_string() << "\",\n"
            << "  \"files\": \"" << jobs.size() << "\",\n"
            << "  \"update_files\": \"" << update_files << "\",\n"
            << "  \"bytes\": \"" << total << "\",\n"
            << "  \"seconds\": \"" << fmt::format("{:.1f}", secs) << "\"\n"
            << "}\n";
      stamp.close();
      Trace(fmt::format("done in {:.1f} s", secs));
      CopyTraceToLogs();
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
    // Setup's own restart (after adding TU3 at the first load) never runs setup again, even under pdz_setup=force.
    const bool force = mode == "force" && !REXCVAR_GET(pdz_setup_restarted);
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
    std::error_code ec;
    // A setup.json from a start that added TU3 to a disc's own code and restarted, where the restart never reached its
    // image check (0.9.2.1 part 1: the patcher crashed on the US Platinum Hits): set aside, so setup runs again and
    // stops with its message instead of crashing at every start.
    if (!REXCVAR_GET(pdz_setup_restarted))
    {
      const auto old = ReadHostFile(data_dir / "setup.json");
      if (old && StampField(std::string(old->begin(), old->end()), "image_check") == kTu3Added)
      {
        g_trace_file = data_dir / "setup.log";
        fs::rename(data_dir / "setup.json", data_dir / "setup.json.rejected", ec);
        Trace("setup.json says Title Update 3 was added at the first load, but the restart never reached its image "
              "check (it stopped or crashed): set aside as setup.json.rejected" +
              (ec ? " (rename failed: " + ec.message() + ")" : std::string()) + "; setup runs again");
        why = "the last start stopped before its image check";
      }
    }
    if (!force && why.empty() && StampValid(data_dir, why))
    {
      g_summary = fmt::format("pdz_setup: prepared game files in {} (setup.json checked)", data_dir.string());
      return {data_dir / "game", data_dir / "update"};
    }
    fs::create_directories(data_dir, ec);
    g_trace_file = data_dir / "setup.log";
    return RunSetup(exe_dir, data_dir, user_data_root, force ? std::string("forced, pdz_setup") : why);
  }

  void VerifyImage(const uint8_t *image, size_t size, const fs::path &game_root)
  {
    // Only the part of the range that is mapped: a disc's own code without Title Update 3 is a smaller image (TU3's
    // ends at 833E0000), so the rest of the range isn't there and reading it would fault.
    size_t mapped = 0;
    while (mapped < size)
    {
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(image + mapped, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
          (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
      {
        break;
      }
      const size_t region_end = size_t(static_cast<const uint8_t *>(mbi.BaseAddress) + mbi.RegionSize - image);
      mapped = std::min(size, region_end);
    }
    Sha1 h;
    h.Update(image, mapped);
    const std::string got = h.Hex() + (mapped < size ? fmt::format(" (only 0x{:X} of 0x{:X} bytes mapped)", mapped, size)
                                                       : std::string());
    const bool test_mismatch = REXCVAR_GET(pdz_setup_test_image_mismatch);
    const fs::path data_dir = game_root.parent_path();
    const fs::path stamp_path = data_dir / "setup.json";
    std::error_code ec;
    const bool prepared = fs::exists(stamp_path, ec);
    if (prepared)
    {
      g_trace_file = data_dir / "setup.log"; // this start's lines go to setup's own log too
    }
    const auto stamp_bytes = prepared ? ReadHostFile(stamp_path) : std::nullopt;
    std::string stamp = stamp_bytes ? std::string(stamp_bytes->begin(), stamp_bytes->end()) : std::string();
    const std::string image_check = StampField(stamp, "image_check");
    const bool has_xexp = fs::exists(game_root / "default.xexp", ec);
    if (image_check == "pending" && has_xexp)
    {
      Trace("the disc's own code was to be checked, but a default.xexp is in " + game_root.string() +
            " (not from this setup): this check is of the code with it");
    }
    const auto set_field = [&](const std::string &key, const std::string &value) {
      const std::string k = "\"" + key + "\": \"";
      const size_t at = stamp.find(k);
      if (at == std::string::npos)
      {
        return;
      }
      const size_t end = stamp.find('"', at + k.size());
      stamp.replace(at + k.size(), end - at - k.size(), value);
    };
    const auto write_stamp = [&] { std::ofstream(stamp_path, std::ios::binary | std::ios::trunc) << stamp; };
    Trace(fmt::format("image check at load: SHA-1 {} ({}; expected {}){}", got,
                      has_xexp ? "with Title Update 3's default.xexp" : "the disc's own code, no title update",
                      kImageSha1, test_mismatch ? " - TESTING: reported as a mismatch (pdz_setup_test_image_mismatch)"
                                                : ""));
    if (mapped == size && got == kImageSha1 && !test_mismatch)
    {
      REXLOG_INFO("pdz_setup: executable image checked (SHA-1 {}): the one PDZ Restored is made from{}", got,
                  has_xexp ? "" : " (the disc's own code: no title update needed)");
      if (image_check == "pending")
      {
        set_field("image_check", "the disc's own code matches: no title update needed");
        write_stamp();
        Trace("the disc's own code is the one PDZ Restored is built from: no title update needed (none is used)");
      }
      else if (image_check == kTu3Added)
      {
        // Recorded, so a start that finds kTu3Added still there knows the restart never got this far (Prepare).
        set_field("image_check", kTu3Checked);
        write_stamp();
        Trace("final image with Title Update 3: the one PDZ Restored is built from");
      }
      CopyTraceToLogs();
      return;
    }
    // A disc setup didn't know, whose own code is a full-size image like TU3's but not the build's: an updated build of
    // the game, not the original code TU3's patch is made for (the US Platinum Hits: the SDK's patcher crashed on it).
    // No title update is added; it stops below with the message.
    const bool wrong_build = image_check == "pending" && !has_xexp && mapped == size;
    if (wrong_build)
    {
      Trace("the disc's own code is SHA-1 " + got + ": a full-size image like Title Update 3's, but not the one PDZ "
            "Restored is built from (a different build of the game): Title Update 3 can't be applied to it, so none is "
            "added");
    }
    // A disc setup didn't know: its own code (the original, smaller image) isn't the build's, so Title Update 3's
    // default.xexp (held back in pending\) is added and PDZ Restored starts again once.
    const fs::path pending = data_dir / "pending" / "default.xexp";
    if (image_check == "pending" && !has_xexp && !wrong_build && fs::exists(pending, ec) && !test_mismatch)
    {
      Trace("the disc's own code is SHA-1 " + got + ", not the one PDZ Restored is built from: adding Title Update 3 "
            "(its default.xexp from pending\\) and starting again");
      fs::rename(pending, game_root / "default.xexp", ec);
      if (ec)
      {
        fs::copy_file(pending, game_root / "default.xexp", fs::copy_options::overwrite_existing, ec);
      }
      set_field("xexp_sha1", StampField(stamp, "pending_xexp_sha1"));
      set_field("image_check", kTu3Added);
      write_stamp();
      CopyTraceToLogs();
      REXLOG_INFO("pdz_setup: the disc's own code (SHA-1 {}) needs Title Update 3: added, PDZ Restored starts again",
                  got);
      rex::FlushLogging();
      std::wstring cmd = std::wstring(GetCommandLineW()) + L" --pdz_setup_restarted=true";
      STARTUPINFOW si{};
      si.cb = sizeof(si);
      PROCESS_INFORMATION pi{};
      wchar_t exe[MAX_PATH] = {};
      GetModuleFileNameW(nullptr, exe, MAX_PATH);
      if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
      {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        std::exit(0);
      }
      Trace("starting again failed (error " + std::to_string(GetLastError()) + "): start PDZ Restored again by hand");
    }
    REXLOG_ERROR("pdz_setup: the loaded executable image is not the one PDZ Restored is made from (SHA-1 {}, expected "
                 "{}; game files {})",
                 got, kImageSha1, game_root.string());
    const std::string media = StampField(stamp, "media_id"), version = StampField(stamp, "version");
    Trace("final image doesn't match: SHA-1 " + got + ", expected " + kImageSha1 + " (media ID " + media + ", version " +
          version + (has_xexp ? ", with Title Update 3" : ", the disc's own code") + ")");
    // A prepared folder with this result is not used again: setup runs again at the next start.
    if (prepared && fs::exists(data_dir / "update", ec))
    {
      fs::rename(stamp_path, data_dir / "setup.json.rejected", ec);
    }
    const std::wstring text = std::wstring(kNotSupported) + (g_trace_file.empty() ? std::wstring() : LogWhere());
    Trace("stopped: " + fs::path(text).string());
    CopyTraceToLogs();
    if (!REXCVAR_GET(pdz_setup_quiet))
    {
      setup_ui::Error(kTitle, text);
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
