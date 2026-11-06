// PDZ Restored: the official map packs, installed from the player's own package files (2026-11-04). The exe's port of
// tools/install_map_packs.py + tools/stfs.py (docs/MAP_PACKS.md "Install"): each pack's STFS package is only read;
// every file is extracted, the known damage in some copies of Map Pack One is repaired (Plaza 1994253005.pkg: 1 byte,
// Trench 454830383.pkg: 2 bytes), every file is checked against the package's SHA-1 block hashes and every .pkg must
// inflate with a valid Adler-32 trailer. Only a pack that verifies completely is installed, into
// <user data>\0000000000000000\4D5307D3\00000002\<package file name>\, where TU3 lists it (XamContentCreateEnumerator).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace pdz::map_packs
{
  struct PackInfo
  {
    const char *name;         // "Map Pack One"
    const char *maps;         // "Plaza, Rooftops, Trench, Gas Plant"
    const wchar_t *file_name; // the package's own file name (content id), the install folder's name
  };
  const std::vector<PackInfo> &Packs();

  enum class Status
  {
    kNotInstalled,
    kInstalled,
    kInstalledRepaired,
    kDidNotVerify,
  };

  struct State
  {
    Status status = Status::kNotInstalled;
    std::string reason; // kDidNotVerify: why (also the last install's failure)
  };

  // <user data>\0000000000000000\4D5307D3 (the title's content root).
  std::filesystem::path TitleRoot(const std::filesystem::path &user_data_root);

  // The installed state of pack `index` (Packs()), from the folder and its install stamp; `deep` re-checks every
  // file against the stamp's SHA-1 (Check installed packs).
  State StateOf(const std::filesystem::path &title_root, size_t index, bool deep);

  // Installs a pack package (identified by its file table: Pdz/dlc_contents/default.bin, and by its file name or
  // display name). `index_out`: which pack (or -1). False with `log` saying why. Replaces an installed copy.
  bool Install(const std::filesystem::path &package, const std::filesystem::path &title_root, int &index_out,
               std::string &log);

  // Removes PDZ Restored's copy of pack `index` (the player's own package file isn't touched).
  bool Remove(const std::filesystem::path &title_root, size_t index);
} // namespace pdz::map_packs
