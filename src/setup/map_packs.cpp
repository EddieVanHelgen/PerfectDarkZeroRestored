// PDZ Restored: the official map packs (see map_packs.h).
#include "map_packs.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <system_error>

#include <fmt/format.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_JPEG
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_STDIO
#include <stb_image.h> // its zlib decoder only (the .pkg inflate check)

#pragma comment(lib, "bcrypt.lib")

namespace pdz::map_packs
{
  namespace fs = std::filesystem;

  const std::vector<PackInfo> &Packs()
  {
    static const std::vector<PackInfo> packs = {
        {"Map Pack One", "Plaza, Rooftops, Trench, Gas Plant", L"28A3533063A3643DD1AF810EE03BFEA893A7C9D74D"},
        {"Map Pack Two", "Facility, Ruin", L"7841AEFC8C33B6D0BECAF57E4DD4DBAECD104C734D"},
    };
    return packs;
  }

  fs::path TitleRoot(const fs::path &user_data_root) { return user_data_root / "0000000000000000" / "4D5307D3"; }

  namespace
  {
    constexpr size_t kBlock = 0x1000;
    constexpr uint32_t kPerLevel = 170;
    constexpr const char *kStamp = "pdz_install.txt";

    // Damage found in Map Pack One copies (tools/install_map_packs.py REPAIRS): file -> (offset, damaged, original).
    struct Repair
    {
      const char *file;
      size_t offset;
      uint8_t damaged, original;
    };
    constexpr Repair kRepairs[] = {
        {"1994253005.pkg", 6618007, 0x88, 0xAB}, // Plaza, block 8608
        {"454830383.pkg", 48589206, 0x00, 0x3A}, // Trench, block 44745
        {"454830383.pkg", 48589207, 0x00, 0x3E},
    };

    uint32_t Be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
    uint32_t Le24(const uint8_t *p) { return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16); }
    uint32_t Be24(const uint8_t *p) { return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]; }

    bool Sha1(const uint8_t *data, size_t size, uint8_t out[20])
    {
      return BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, const_cast<uint8_t *>(data), ULONG(size),
                                       out, 20));
    }

    std::string Hex(const uint8_t *d, size_t n)
    {
      std::string s;
      for (size_t i = 0; i < n; ++i)
      {
        s += fmt::format("{:02x}", d[i]);
      }
      return s;
    }

    // tools/stfs.py's Stfs: a read-only STFS (LIVE / PIRS / CON) package.
    struct Stfs
    {
      std::ifstream f;
      uint32_t header_size = 0, ft_block_count = 0, ft_block = 0, tables_per_level = 1;
      uint64_t base = 0;
      std::wstring display_name;
      bool ok = false;

      explicit Stfs(const fs::path &path) : f(path, std::ios::binary)
      {
        std::vector<uint8_t> h(0xA000);
        if (!f || !f.read(reinterpret_cast<char *>(h.data()), std::streamsize(h.size())))
        {
          return;
        }
        if (std::memcmp(h.data(), "LIVE", 4) != 0 && std::memcmp(h.data(), "PIRS", 4) != 0 &&
            std::memcmp(h.data(), "CON ", 4) != 0)
        {
          return;
        }
        header_size = Be32(&h[0x340]);
        const size_t vd = 0x379;
        const uint8_t separation = h[vd + 2];
        ft_block_count = h[vd + 3] | (uint32_t(h[vd + 4]) << 8);
        ft_block = Le24(&h[vd + 5]);
        tables_per_level = (separation & 1) ? 1 : 2;
        base = (uint64_t(header_size) + kBlock - 1) & ~uint64_t(kBlock - 1);
        for (size_t i = 0; i < 0x80; i += 2)
        {
          const wchar_t c = wchar_t((h[0x411 + i] << 8) | h[0x411 + i + 1]);
          if (!c)
          {
            break;
          }
          display_name += c;
        }
        ok = true;
      }

      uint32_t Backing(uint32_t block) const
      {
        uint32_t result = block, b = kPerLevel;
        for (int i = 0; i < 3; ++i)
        {
          result += ((block + b) / b) * tables_per_level;
          if (block < b)
          {
            break;
          }
          b *= kPerLevel;
        }
        return result;
      }

      bool Read(uint64_t at, uint8_t *out, size_t n)
      {
        f.clear();
        f.seekg(std::streamoff(at));
        return bool(f.read(reinterpret_cast<char *>(out), std::streamsize(n)));
      }

      bool ReadBlock(uint32_t block, uint8_t *out) { return Read(base + uint64_t(Backing(block)) * kBlock, out, kBlock); }

      // The level-0 hash entry of a data block: 20-byte SHA-1, status, next block.
      bool HashEntry(uint32_t block, uint8_t entry[0x18])
      {
        const uint32_t group_first = (block / kPerLevel) * kPerLevel;
        const uint32_t table = Backing(group_first) - tables_per_level;
        return Read(base + uint64_t(table) * kBlock + (block % kPerLevel) * 0x18, entry, 0x18);
      }

      std::vector<uint32_t> Chain(uint32_t start, uint32_t count, bool consecutive)
      {
        std::vector<uint32_t> blocks;
        uint32_t b = start;
        for (uint32_t i = 0; i < count; ++i)
        {
          blocks.push_back(b);
          if (consecutive)
          {
            ++b;
          }
          else
          {
            uint8_t e[0x18];
            if (!HashEntry(b, e))
            {
              break;
            }
            b = Be24(&e[0x15]);
          }
        }
        return blocks;
      }

      struct Entry
      {
        int index = 0, parent = -1;
        std::string name;
        bool directory = false, consecutive = false;
        uint32_t blocks = 0, start = 0, size = 0;
      };

      std::vector<Entry> FileTable()
      {
        std::vector<Entry> out;
        std::vector<uint8_t> blk(kBlock);
        int index = 0;
        for (uint32_t b : Chain(ft_block, ft_block_count, false))
        {
          if (!ReadBlock(b, blk.data()))
          {
            break;
          }
          for (size_t i = 0; i < kBlock; i += 0x40, ++index)
          {
            const uint8_t *e = &blk[i];
            const int name_len = e[0x28] & 0x3F;
            if (name_len == 0 || e[0] == 0)
            {
              continue;
            }
            Entry en;
            en.index = index;
            en.name.assign(reinterpret_cast<const char *>(e), size_t(name_len));
            en.directory = (e[0x28] & 0x80) != 0;
            en.consecutive = (e[0x28] & 0x40) != 0;
            en.blocks = Le24(&e[0x29]);
            en.start = Le24(&e[0x2F]);
            en.parent = int16_t((e[0x32] << 8) | e[0x33]);
            en.size = Be32(&e[0x34]);
            out.push_back(en);
          }
        }
        return out;
      }

      static std::string Path(const std::vector<Entry> &entries, const Entry &e)
      {
        std::map<int, const Entry *> by_index;
        for (const Entry &x : entries)
        {
          by_index[x.index] = &x;
        }
        std::string path = e.name;
        int p = e.parent;
        for (int guard = 0; p != -1 && by_index.count(p) && guard < 64; ++guard)
        {
          path = by_index[p]->name + "/" + path;
          p = by_index[p]->parent;
        }
        return path;
      }

      // The file's bytes, and the blocks that don't match the package's hashes (after `repairs`).
      bool ReadFile(const Entry &e, std::vector<uint8_t> &data, std::vector<uint32_t> &chain)
      {
        chain = Chain(e.start, e.blocks, e.consecutive);
        data.assign(size_t(chain.size()) * kBlock, 0);
        for (size_t i = 0; i < chain.size(); ++i)
        {
          if (!ReadBlock(chain[i], &data[i * kBlock]))
          {
            return false;
          }
        }
        // The last block keeps the package's own bytes past the end for the hash check below.
        return data.size() >= e.size;
      }

      size_t BadBlocks(const std::vector<uint32_t> &chain, const std::vector<uint8_t> &padded, std::vector<uint32_t> &bad)
      {
        for (size_t i = 0; i < chain.size(); ++i)
        {
          uint8_t entry[0x18], digest[20];
          if (!HashEntry(chain[i], entry) || !Sha1(&padded[i * kBlock], kBlock, digest) ||
              std::memcmp(digest, entry, 20) != 0)
          {
            bad.push_back(chain[i]);
          }
        }
        return bad.size();
      }
    };

    uint32_t Adler32(const uint8_t *d, size_t n)
    {
      uint32_t a = 1, b = 0;
      while (n)
      {
        const size_t chunk = std::min<size_t>(n, 5552);
        n -= chunk;
        for (size_t i = 0; i < chunk; ++i)
        {
          a += d[i];
          b += a;
        }
        d += chunk;
        a %= 65521;
        b %= 65521;
      }
      return (b << 16) | a;
    }

    // A zlib stream that inflates completely and matches its Adler-32 trailer.
    bool InflatesOk(const std::vector<uint8_t> &z, size_t size, std::string &why)
    {
      if (size < 6)
      {
        why = "too short";
        return false;
      }
      int out_len = 0;
      char *out = stbi_zlib_decode_malloc_guesssize_headerflag(reinterpret_cast<const char *>(z.data()), int(size),
                                                               int(std::min<size_t>(size * 4, 512u << 20)), &out_len, 1);
      if (!out)
      {
        why = "does not inflate";
        return false;
      }
      const uint32_t want = Be32(&z[size - 4]);
      const uint32_t got = Adler32(reinterpret_cast<const uint8_t *>(out), size_t(out_len));
      STBI_FREE(out);
      if (want != got)
      {
        why = fmt::format("inflates, but its Adler-32 is {:08x}, the trailer says {:08x}", got, want);
        return false;
      }
      return true;
    }

    int IdentifyPack(const fs::path &package, const std::wstring &display_name)
    {
      const std::wstring file = package.filename().wstring();
      for (size_t i = 0; i < Packs().size(); ++i)
      {
        if (_wcsicmp(file.c_str(), Packs()[i].file_name) == 0)
        {
          return int(i);
        }
      }
      std::wstring lower = display_name;
      std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
      if (lower.find(L"2") != std::wstring::npos || lower.find(L"two") != std::wstring::npos)
      {
        return 1;
      }
      if (lower.find(L"1") != std::wstring::npos || lower.find(L"one") != std::wstring::npos)
      {
        return 0;
      }
      return -1;
    }
  } // namespace

  State StateOf(const fs::path &title_root, size_t index, bool deep)
  {
    State st;
    if (index >= Packs().size())
    {
      return st;
    }
    const fs::path folder = title_root / "00000002" / Packs()[index].file_name;
    std::error_code ec;
    if (fs::is_directory(fs::path(folder.wstring() + L".failed"), ec) && !fs::is_directory(folder, ec))
    {
      st.status = Status::kDidNotVerify;
      std::ifstream why(fs::path(folder.wstring() + L".failed") / "pdz_failed.txt");
      std::getline(why, st.reason);
      return st;
    }
    if (!fs::is_directory(folder, ec))
    {
      return st;
    }
    std::ifstream stamp(folder / kStamp);
    std::string line;
    bool repaired = false;
    std::vector<std::pair<std::string, std::string>> files;
    while (std::getline(stamp, line))
    {
      if (line.rfind("repaired ", 0) == 0)
      {
        repaired = line == "repaired 1";
      }
      else if (line.size() > 41 && line[40] == ' ')
      {
        files.emplace_back(line.substr(0, 40), line.substr(41));
      }
    }
    if (files.empty())
    {
      // Installed by tools/install_map_packs.py (no stamp): it verified it then; the default.bin check stands in.
      st.status = fs::exists(folder / "Pdz" / "dlc_contents" / "default.bin", ec) ? Status::kInstalled : Status::kDidNotVerify;
      st.reason = st.status == Status::kDidNotVerify ? "Pdz\\dlc_contents\\default.bin is missing" : "";
      return st;
    }
    if (deep)
    {
      for (const auto &[sha, rel] : files)
      {
        std::ifstream f(folder / fs::u8path(rel), std::ios::binary);
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        uint8_t digest[20];
        if (!f.good() && !f.eof())
        {
          st.status = Status::kDidNotVerify;
          st.reason = rel + " can't be read";
          return st;
        }
        if (!Sha1(d.data(), d.size(), digest) || Hex(digest, 20) != sha)
        {
          st.status = Status::kDidNotVerify;
          st.reason = rel + " changed since it was installed";
          return st;
        }
      }
    }
    st.status = repaired ? Status::kInstalledRepaired : Status::kInstalled;
    return st;
  }

  bool Install(const fs::path &package, const fs::path &title_root, int &index_out, std::string &log)
  {
    index_out = -1;
    Stfs s(package);
    if (!s.ok)
    {
      log = "not a package PDZ Restored can read (no LIVE / PIRS / CON header)";
      return false;
    }
    const std::vector<Stfs::Entry> entries = s.FileTable();
    bool has_manifest = false;
    for (const auto &e : entries)
    {
      std::string p = Stfs::Path(entries, e);
      std::transform(p.begin(), p.end(), p.begin(), ::tolower);
      has_manifest |= !e.directory && p == "pdz/dlc_contents/default.bin";
    }
    if (!has_manifest)
    {
      log = "not a Perfect Dark Zero map pack (no Pdz/dlc_contents/default.bin)";
      return false;
    }
    const int index = IdentifyPack(package, s.display_name);
    if (index < 0)
    {
      log = "a map pack, but not one of the two official ones";
      return false;
    }
    index_out = index;
    const fs::path target = title_root / "00000002" / Packs()[size_t(index)].file_name;
    const fs::path work = fs::path(target.wstring() + L".new"), failed = fs::path(target.wstring() + L".failed");
    std::error_code ec;
    fs::remove_all(work, ec);
    bool ok = true, repaired = false;
    std::string stamp, problems;
    for (const auto &e : entries)
    {
      if (e.directory)
      {
        continue;
      }
      const std::string rel = Stfs::Path(entries, e);
      std::vector<uint8_t> data;
      std::vector<uint32_t> chain;
      if (!s.ReadFile(e, data, chain))
      {
        problems += rel + ": can't be read; ";
        ok = false;
        continue;
      }
      const std::string base_name = fs::u8path(rel).filename().string();
      for (const Repair &r : kRepairs)
      {
        if (_stricmp(base_name.c_str(), r.file) == 0 && r.offset < e.size && data[r.offset] == r.damaged)
        {
          data[r.offset] = r.original;
          repaired = true;
        }
      }
      std::vector<uint32_t> bad;
      if (s.BadBlocks(chain, data, bad))
      {
        problems += fmt::format("{}: {} block(s) don't match the package's hashes; ", rel, bad.size());
        ok = false;
      }
      std::string lower = base_name;
      std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
      std::string why;
      if (lower.size() > 4 && lower.substr(lower.size() - 4) == ".pkg" && !InflatesOk(data, e.size, why))
      {
        problems += rel + ": " + why + "; ";
        ok = false;
      }
      const fs::path out = work / fs::u8path(rel);
      fs::create_directories(out.parent_path(), ec);
      std::ofstream f(out, std::ios::binary);
      f.write(reinterpret_cast<const char *>(data.data()), std::streamsize(e.size));
      uint8_t digest[20];
      Sha1(data.data(), e.size, digest);
      stamp += Hex(digest, 20) + " " + rel + "\n";
    }
    if (!ok)
    {
      fs::remove_all(failed, ec);
      fs::rename(work, failed, ec);
      std::ofstream(failed / "pdz_failed.txt") << problems << "\n";
      log = "didn't verify: " + problems;
      return false;
    }
    std::ofstream(work / kStamp) << "repaired " << (repaired ? 1 : 0) << "\n" << stamp;
    fs::remove_all(target, ec);
    fs::remove_all(failed, ec);
    fs::rename(work, target, ec);
    if (ec)
    {
      log = "couldn't move it into place: " + ec.message();
      return false;
    }
    log = repaired ? "installed (repaired: a known fault in this copy was fixed)" : "installed, every file verified";
    return true;
  }

  bool Remove(const fs::path &title_root, size_t index)
  {
    if (index >= Packs().size())
    {
      return false;
    }
    const fs::path folder = title_root / "00000002" / Packs()[index].file_name;
    std::error_code ec;
    fs::remove_all(folder, ec);
    fs::remove_all(fs::path(folder.wstring() + L".failed"), ec);
    return !fs::exists(folder, ec);
  }
} // namespace pdz::map_packs
