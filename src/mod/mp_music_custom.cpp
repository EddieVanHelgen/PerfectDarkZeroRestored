// Multiplayer match music: the player's own MP3s (pdz_mp_music_custom_dir, default empty = off), 2026-11-04.
//
// - List: the folder's .mp3 files, in number order of their names, scanned once at the first use. Each is shown in
//   the Music row after the game's tracks as "REMIX: <TITLE>": its ID3 title (TIT2), unless that is missing or a web
//   address (tags from download sites), else its file name without a leading track number; upper case, ASCII, a
//   trailing "REMIX" dropped (the prefix says it). Cut to fit the row (kMaxChars, the longest name of the game's
//   tracks there, LABORATORY CONTAINMENT); names that would come out the same lose the leading words they share.
// - Loudness: each file is decoded once on a worker thread (host_stream::Measure: BS.1770 gating, no K filter) and
//   its gain set so it plays as loud as the game's tracks: kRareLoudness is the median of the 31 level tracks of
//   Random measured the same way from their wave banks (16-bit PCM, -14.8 to -8.1 dB, 2026-11-04; the music plays
//   with Rare's category volume untouched). The gain stays within -24 / +6 dB and never lifts the
//   peak over -0.3 dBFS. Kept in mp_music_custom.txt in the user data folder (name, size, time), so a file is
//   measured only once.
// - Playback: host_stream (the SDK's FFmpeg, mixed into the host audio output after the game's mix, under the same
//   output gain and audio_mute): 2 s fade in, 3 s fade out before the end; a chosen remix loops through those
//   fades, Random plays it once. pdz_mp_music_custom_volume_db is followed live. A file that can't be read or
//   decoded is skipped with a log line.
// - The game has no music volume option of its own (none of its 89 DARKVALUE settings is a volume, and Rare never
//   sets the XACT music category's volume), so there is no game volume to follow beyond the host's.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/audio/host_stream.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include "../game_cvars.h"
#include "mp_music_custom.h"

namespace
{
  namespace hs = rex::audio::host_stream;
  namespace fs = std::filesystem;

  constexpr double kRareLoudness = -9.84;
  constexpr size_t kMaxChars = 22;
  constexpr const char *kPrefix = "REMIX: ";
  constexpr const char *kSavePrefix = "REMIX FILE:";

  struct Entry
  {
    fs::path file;
    std::string title;     // upper case, before the cut
    std::string menu;      // "REMIX: ..."
    uint64_t size = 0;
    int64_t time = 0;
    // Written by the measuring worker before `state`, read after it.
    double loudness = 0.0;
    double peak = 0.0;
    uint64_t frames = 0;
    double gain_db = 0.0;
    std::string error;
    std::atomic<int> state{0}; // 0 waiting, 1 measured, 2 failed
  };

  std::once_flag g_scan;
  std::vector<std::unique_ptr<Entry>> g_entries;
  const std::string g_empty;

  // Playback state (the game thread).
  int g_index = -1;
  bool g_loop = false;
  mp_music_custom::State g_state = mp_music_custom::State::Idle;
  double g_volume_db = 1e9;
  uint32_t g_logged_passes = 0;
  double g_logged_at = -1e9;

  fs::path CacheFile()
  {
    auto *rt = rex::Runtime::instance();
    return (rt ? rt->user_data_root() : fs::path()) / "mp_music_custom.txt";
  }

  bool WebAddress(const std::string &text)
  {
    std::string t = text;
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char *s : {".com", ".net", ".org", "www.", "http", ".ru", ".co."})
    {
      if (t.find(s) != std::string::npos)
      {
        return true;
      }
    }
    return false;
  }

  // TIT2 of an ID3v2.3 / 2.4 tag (ASCII part of the text).
  std::string Id3Title(const fs::path &file)
  {
    std::ifstream in(file, std::ios::binary);
    std::vector<uint8_t> d(65536);
    in.read(reinterpret_cast<char *>(d.data()), std::streamsize(d.size()));
    d.resize(size_t(in.gcount()));
    if (d.size() < 10 || std::memcmp(d.data(), "ID3", 3) != 0)
    {
      return {};
    }
    const int version = d[3];
    const size_t end = std::min(d.size(), 10 + ((size_t(d[6] & 0x7F) << 21) | (size_t(d[7] & 0x7F) << 14) |
                                                (size_t(d[8] & 0x7F) << 7) | size_t(d[9] & 0x7F)));
    size_t p = 10;
    while (p + 10 <= end && d[p])
    {
      size_t size = version >= 4 ? (size_t(d[p + 4] & 0x7F) << 21) | (size_t(d[p + 5] & 0x7F) << 14) |
                                       (size_t(d[p + 6] & 0x7F) << 7) | size_t(d[p + 7] & 0x7F)
                                 : (size_t(d[p + 4]) << 24) | (size_t(d[p + 5]) << 16) | (size_t(d[p + 6]) << 8) |
                                       size_t(d[p + 7]);
      if (std::memcmp(&d[p], "TIT2", 4) == 0 && size > 1 && p + 10 + size <= d.size())
      {
        const uint8_t encoding = d[p + 10];
        std::string text;
        const bool wide = encoding == 1 || encoding == 2;
        for (size_t i = p + 11; i + (wide ? 1 : 0) < p + 10 + size; i += wide ? 2 : 1)
        {
          uint16_t c = d[i];
          if (wide)
          {
            c = encoding == 2 ? uint16_t(d[i] << 8 | d[i + 1]) : uint16_t(d[i] | d[i + 1] << 8);
            if (c == 0xFEFF || c == 0xFFFE)
            {
              continue;
            }
          }
          if (!c)
          {
            break;
          }
          text += c >= 0x20 && c < 0x7F ? char(c) : ' ';
        }
        return text;
      }
      p += 10 + size;
    }
    return {};
  }

  std::vector<std::string> Words(const std::string &text)
  {
    std::vector<std::string> words;
    std::istringstream in(text);
    for (std::string w; in >> w;)
    {
      words.push_back(w);
    }
    return words;
  }

  // Upper case, ASCII letters / digits / a few marks, separators to spaces, a trailing "REMIX" dropped.
  std::string Clean(const std::string &text)
  {
    std::string out;
    for (unsigned char c : text)
    {
      if (std::isalnum(c) || c == '\'' || c == '&' || c == '!' || c == '.' || c == ',')
      {
        out += char(std::toupper(c));
      }
      else
      {
        out += ' ';
      }
    }
    std::vector<std::string> words = Words(out);
    if (words.size() > 1 && words.back() == "REMIX")
    {
      words.pop_back();
    }
    std::string joined;
    for (const std::string &w : words)
    {
      joined += (joined.empty() ? "" : " ") + w;
    }
    return joined;
  }

  std::string FileTitle(const fs::path &file)
  {
    std::string stem = file.stem().string();
    size_t i = 0;
    while (i < stem.size() && std::isdigit(static_cast<unsigned char>(stem[i])))
    {
      ++i;
    }
    if (i > 0 && i < stem.size() && (stem[i] == '-' || stem[i] == '_' || stem[i] == ' ' || stem[i] == '.'))
    {
      stem = stem.substr(i + 1);
    }
    return stem;
  }

  std::string Cut(const std::vector<std::string> &words, size_t from)
  {
    const size_t room = kMaxChars - std::strlen(kPrefix);
    std::string text;
    for (size_t i = from; i < words.size(); ++i)
    {
      const std::string next = text.empty() ? words[i] : text + " " + words[i];
      if (next.size() > room)
      {
        if (text.empty())
        {
          text = words[i].substr(0, room);
        }
        break;
      }
      text = next;
    }
    return kPrefix + text;
  }

  void MakeMenuNames()
  {
    std::vector<std::vector<std::string>> words;
    std::vector<size_t> from(g_entries.size(), 0);
    for (auto &e : g_entries)
    {
      words.push_back(Words(e->title));
    }
    for (size_t i = 0; i < g_entries.size(); ++i)
    {
      g_entries[i]->menu = Cut(words[i], 0);
    }
    // Same name after the cut: drop the leading words the group shares, then cut again.
    for (size_t i = 0; i < g_entries.size(); ++i)
    {
      std::vector<size_t> group{i};
      for (size_t j = i + 1; j < g_entries.size(); ++j)
      {
        if (g_entries[j]->menu == g_entries[i]->menu)
        {
          group.push_back(j);
        }
      }
      if (group.size() < 2)
      {
        continue;
      }
      size_t shared = 0;
      for (bool same = true; same; )
      {
        for (size_t g : group)
        {
          same = same && shared + 1 < words[g].size() && words[g][shared] == words[group[0]][shared];
        }
        shared += same ? 1 : 0;
      }
      for (size_t g : group)
      {
        g_entries[g]->menu = Cut(words[g], shared);
      }
    }
    for (size_t i = 0; i < g_entries.size(); ++i)
    {
      for (size_t j = 0; j < i; ++j)
      {
        if (g_entries[j]->menu == g_entries[i]->menu)
        {
          const std::string n = " " + std::to_string(i + 1);
          g_entries[i]->menu = g_entries[i]->menu.substr(0, kMaxChars - n.size()) + n;
        }
      }
    }
  }

  void SetGain(Entry &e)
  {
    double gain = std::clamp(kRareLoudness - e.loudness, -24.0, 6.0);
    if (gain > 0.0)
    {
      gain = std::max(0.0, std::min(gain, -0.3 - e.peak));
    }
    e.gain_db = gain;
  }

  // The worker: every file not in the cache is measured, one after the other, and appended to it.
  void MeasureAll()
  {
    struct Cached
    {
      std::string name;
      uint64_t size;
      int64_t time;
      double loudness, peak;
      uint64_t frames;
    };
    std::vector<Cached> cache;
    {
      std::ifstream in(CacheFile());
      for (std::string line; std::getline(in, line);)
      {
        std::istringstream f(line);
        Cached c{};
        std::string name;
        if (std::getline(f, name, '\t') && f >> c.size >> c.time >> c.loudness >> c.peak >> c.frames)
        {
          c.name = name;
          cache.push_back(c);
        }
      }
    }
    for (auto &e : g_entries)
    {
      const std::string name = e->file.filename().string();
      bool found = false;
      for (const Cached &c : cache)
      {
        if (c.name == name && c.size == e->size && c.time == e->time)
        {
          e->loudness = c.loudness;
          e->peak = c.peak;
          e->frames = c.frames;
          found = true;
        }
      }
      if (!found)
      {
        const hs::Measured m = hs::Measure(e->file);
        if (!m.ok)
        {
          e->error = m.error;
          REXLOG_WARN("pdz_mp_music: skipped {} (can't be played: {})", name, m.error);
          e->state.store(2, std::memory_order_release);
          continue;
        }
        e->loudness = m.loudness_db;
        e->peak = m.peak_db;
        e->frames = m.frames;
        std::ofstream out(CacheFile(), std::ios::app);
        out << name << '\t' << e->size << ' ' << e->time << ' ' << m.loudness_db << ' ' << m.peak_db << ' '
            << m.frames << '\n';
      }
      SetGain(*e);
      REXLOG_INFO("pdz_mp_music: {} = {}: {:.1f} s, loudness {:.2f} dB (the game's tracks {:.2f}), peak {:.1f} dBFS, "
                  "gain {:+.1f} dB ({})",
                  e->menu, name, double(e->frames) / hs::kRate, e->loudness, kRareLoudness, e->peak, e->gain_db,
                  found ? "measured before" : "measured now");
      e->state.store(1, std::memory_order_release);
    }
  }

  void Scan()
  {
    std::call_once(g_scan, [] {
      const std::string dir = REXCVAR_GET(pdz_mp_music_custom_dir);
      if (dir.empty())
      {
        return;
      }
      std::error_code ec;
      std::vector<fs::path> files;
      for (fs::directory_iterator it(fs::path(dir), ec), end; !ec && it != end; it.increment(ec))
      {
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext == ".mp3" && it->is_regular_file(ec))
        {
          files.push_back(it->path());
        }
      }
      if (ec && files.empty())
      {
        REXLOG_WARN("pdz_mp_music: pdz_mp_music_custom_dir '{}' can't be read ({})", dir, ec.message());
        return;
      }
      // Number order of the names ("2-..." before "10-..."), then by name.
      auto number = [](const fs::path &p) {
        const std::string s = p.filename().string();
        return s.empty() || !std::isdigit(static_cast<unsigned char>(s[0])) ? -1 : std::atoi(s.c_str());
      };
      std::sort(files.begin(), files.end(), [&](const fs::path &a, const fs::path &b) {
        return number(a) != number(b) ? number(a) < number(b) : a.filename() < b.filename();
      });
      for (const fs::path &f : files)
      {
        auto e = std::make_unique<Entry>();
        e->file = f;
        e->size = fs::file_size(f, ec);
        e->time = int64_t(fs::last_write_time(f, ec).time_since_epoch().count());
        std::string title = Id3Title(f);
        if (Clean(title).empty() || WebAddress(title))
        {
          title = FileTitle(f);
        }
        e->title = Clean(title);
        if (e->title.empty())
        {
          e->title = "TRACK " + std::to_string(g_entries.size() + 1);
        }
        g_entries.push_back(std::move(e));
      }
      MakeMenuNames();
      REXLOG_INFO("pdz_mp_music: {} file(s) of your own in {}", g_entries.size(), dir);
      if (!g_entries.empty())
      {
        std::thread(MeasureAll).detach();
      }
    });
  }

  void Follow()
  {
    const double db = REXCVAR_GET(pdz_mp_music_custom_volume_db);
    if (db != g_volume_db)
    {
      g_volume_db = db;
      hs::SetVolume(float(std::pow(10.0, std::clamp(db, -30.0, 12.0) / 20.0)));
    }
  }
} // namespace

namespace mp_music_custom
{
  int Count()
  {
    Scan();
    return int(g_entries.size());
  }

  const std::string &MenuName(int index)
  {
    Scan();
    return index >= 0 && index < int(g_entries.size()) ? g_entries[index]->menu : g_empty;
  }

  std::string SaveName(int index)
  {
    Scan();
    return index >= 0 && index < int(g_entries.size()) ? kSavePrefix + g_entries[index]->file.filename().string()
                                                       : std::string();
  }

  int FromSaveName(const std::string &name)
  {
    if (name.rfind(kSavePrefix, 0) != 0)
    {
      return -1;
    }
    Scan();
    const std::string file = name.substr(std::strlen(kSavePrefix));
    for (size_t i = 0; i < g_entries.size(); ++i)
    {
      if (g_entries[i]->file.filename().string() == file)
      {
        return int(i);
      }
    }
    return -1;
  }

  bool Playable(int index)
  {
    Scan();
    return index >= 0 && index < int(g_entries.size()) && g_entries[index]->state.load() != 2;
  }

  void Start(int index, bool loop)
  {
    Scan();
    hs::Stop();
    g_index = index;
    g_loop = loop;
    g_state = index >= 0 && index < int(g_entries.size()) ? State::Waiting : State::Failed;
    if (g_state == State::Failed)
    {
      REXLOG_WARN("pdz_mp_music: no file {} of your own", index);
    }
    Tick();
  }

  void FadeOut()
  {
    if (g_state == State::Playing)
    {
      hs::FadeOut(3.0);
    }
  }

  void Stop()
  {
    if (g_state != State::Idle && REXCVAR_GET(pdz_music_log))
    {
      REXLOG_INFO("pdz_mp_music: stopped {}", MenuName(g_index));
    }
    hs::Stop();
    g_state = State::Idle;
    g_index = -1;
  }

  State Tick()
  {
    if (g_state == State::Waiting)
    {
      Entry &e = *g_entries[g_index];
      const int measured = e.state.load(std::memory_order_acquire);
      if (measured == 2)
      {
        REXLOG_WARN("pdz_mp_music: skipped {} ({})", e.file.filename().string(), e.error);
        g_state = State::Failed;
      }
      else if (measured == 1)
      {
        hs::Options o;
        o.gain = float(std::pow(10.0, e.gain_db / 20.0));
        o.fade_in = 2.0;
        o.fade_out = 3.0;
        o.loop = g_loop;
        o.frames = e.frames;
        g_volume_db = 1e9;
        if (hs::Play(e.file, o))
        {
          g_logged_passes = 0;
          g_logged_at = 0.0;
          g_state = State::Playing;
          Follow();
          REXLOG_INFO("pdz_mp_music: playing {} ({}), gain {:+.1f} dB{}", e.menu, e.file.filename().string(),
                      e.gain_db, g_loop ? ", looping" : "");
        }
        else
        {
          REXLOG_WARN("pdz_mp_music: skipped {} (the file is gone)", e.file.filename().string());
          g_state = State::Failed;
        }
      }
    }
    else if (g_state == State::Playing)
    {
      Follow();
      const hs::Status s = hs::GetStatus();
      if (s.failed)
      {
        REXLOG_WARN("pdz_mp_music: skipped {} ({})", g_entries[g_index]->file.filename().string(), s.error);
        g_state = State::Failed;
      }
      else if (s.finished)
      {
        g_state = State::Finished;
        if (REXCVAR_GET(pdz_music_log))
        {
          REXLOG_INFO("pdz_mp_music: {} finished after {} pass(es), {} output blocks not ready", MenuName(g_index),
                      s.passes, s.underruns);
        }
      }
      else if (REXCVAR_GET(pdz_music_log) && (s.passes != g_logged_passes || s.seconds >= g_logged_at + 30.0))
      {
        g_logged_passes = s.passes;
        g_logged_at = s.seconds;
        REXLOG_INFO("pdz_mp_music: {} at {:.1f} s of pass {}, {} output blocks not ready", MenuName(g_index), s.seconds,
                    s.passes + 1, s.underruns);
      }
    }
    return g_state;
  }

  int Playing()
  {
    return g_state == State::Idle ? -1 : g_index;
  }
} // namespace mp_music_custom
