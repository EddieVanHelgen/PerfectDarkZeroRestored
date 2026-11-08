// PDZ Restored: the US Platinum Hits disc, translated at load (platinum.h).
#include "platinum.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <chrono>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xex_module.h>

REXCVAR_DEFINE_BOOL(pdz_platinum, true, "Setup",
                    "The US Platinum Hits disc (media ID 40C97944): setup accepts it without Title Update 3, and at load "
                    "its data is laid out as Title Update 3's (the code PDZ Restored is built from) and checked. Off = "
                    "refused as \"not supported\", as in 0.9.2.1");

namespace pdz::platinum
{
  namespace
  {
#include "platinum_translation.inc"

    constexpr uint32_t kBase = 0x82000000;
    constexpr uint32_t kSize = 0x13E0000;

    State g_state = State::kNone;
    std::string g_summary;

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

    uint32_t LoadBe(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
    void StoreBe(uint8_t *p, uint32_t v)
    {
      p[0] = uint8_t(v >> 24);
      p[1] = uint8_t(v >> 16);
      p[2] = uint8_t(v >> 8);
      p[3] = uint8_t(v);
    }

    // The tables as the generator hashed them: little-endian 32-bit words, in order (x86-64 is little-endian and the
    // structs are plain uint32_t fields).
    std::string TableSha1()
    {
      static_assert(sizeof(Run) == 12 && sizeof(Word) == 8);
      Sha1 h;
      h.Update(kRuns, sizeof(kRuns));
      h.Update(kPointerWords, sizeof(kPointerWords));
      h.Update(kShifts, sizeof(kShifts));
      h.Update(kTu3Words, sizeof(kTu3Words));
      return h.Hex();
    }

    // Every table entry inside the image and outside the code range (which is never written).
    bool TableInRange()
    {
      const auto outside_code = [](uint32_t a, uint32_t n) {
        return a >= kBase && a + n <= kBase + kSize && n <= kSize && (a + n <= kCodeLo || a >= kCodeHi);
      };
      for (const Run &r : kRuns)
      {
        if (!outside_code(r.tu3, r.size) || r.plat < kBase || r.plat + r.size > kBase + kSize)
        {
          return false;
        }
      }
      for (uint32_t w : kPointerWords)
      {
        if (!outside_code(kBase + (w >> 8) * 4, 4) || (w & 0xFF) >= std::size(kShifts))
        {
          return false;
        }
      }
      for (const Word &w : kTu3Words)
      {
        if (!outside_code(w.tu3, 4))
        {
          return false;
        }
      }
      return true;
    }

    bool Translate(uint8_t *image, uint32_t base, uint32_t size, uint32_t *entry_point)
    {
      // Cheap first: only an image with the Platinum disc's layout and entry point is hashed (TU3's own is 82ADB7A8).
      if (!REXCVAR_GET(pdz_platinum) || !image || base != kBase || size != kSize || !entry_point ||
          *entry_point != kPlatEntryPoint)
      {
        return false;
      }
      const auto t0 = std::chrono::steady_clock::now();
      Sha1 own;
      own.Update(image, size);
      const std::string own_sha1 = own.Hex();
      if (own_sha1 != kPlatSourceSha1)
      {
        g_summary = fmt::format("pdz_platinum: the image has the Platinum Hits disc's entry point but SHA-1 {} (the "
                                "disc's own code is {}): not translated",
                                own_sha1, kPlatSourceSha1);
        REXLOG_WARN("{}", g_summary);
        return false;
      }
      const std::string table_sha1 = TableSha1();
      if (table_sha1 != kTableSha1 || !TableInRange())
      {
        g_state = State::kFailed;
        g_summary = fmt::format("pdz_platinum: the Platinum Hits disc's image (SHA-1 {}), but the translation table "
                                "doesn't check out (SHA-1 {}, expected {}): not translated",
                                own_sha1, table_sha1, kTableSha1);
        REXLOG_ERROR("{}", g_summary);
        return false;
      }
      // 1. Moved runs, from a copy of the image as loaded (a run can overlap its own source).
      const std::vector<uint8_t> src(image, image + size);
      for (const Run &r : kRuns)
      {
        std::memcpy(image + (r.tu3 - base), src.data() + (r.plat - base), r.size);
      }
      // 2. Pointers: the Platinum address plus its shift = TU3's address.
      for (uint32_t w : kPointerWords)
      {
        uint8_t *p = image + (w >> 8) * 4;
        StoreBe(p, LoadBe(p) + uint32_t(kShifts[w & 0xFF]));
      }
      // 3. TU3's own words (build stamp, PE / debug header, its one extra log string, .pdata lengths).
      for (const Word &w : kTu3Words)
      {
        StoreBe(image + (w.tu3 - base), w.value);
      }
      Sha1 data;
      data.Update(image, kCodeLo - base);
      data.Update(image + (kCodeHi - base), size - (kCodeHi - base));
      const std::string data_sha1 = data.Hex();
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (data_sha1 != kTu3NonCodeSha1)
      {
        g_state = State::kFailed;
        g_summary = fmt::format("pdz_platinum: the Platinum Hits disc's image translated, but the result isn't Title "
                                "Update 3's outside the code (SHA-1 {}, expected {})",
                                data_sha1, kTu3NonCodeSha1);
        REXLOG_ERROR("{}", g_summary);
        return true; // changed; VerifyImage refuses it
      }
      g_state = State::kTranslated;
      *entry_point = kTu3EntryPoint;
      g_summary = fmt::format("pdz_platinum: the US Platinum Hits disc's code (SHA-1 {}): its data laid out as Title "
                              "Update 3's ({} moved runs, {} pointers, {} TU3 words; outside the code SHA-1 {} = TU3's), "
                              "entry point {:08X} -> {:08X}, in {:.0f} ms",
                              own_sha1, std::size(kRuns), std::size(kPointerWords), std::size(kTu3Words), data_sha1,
                              kPlatEntryPoint, kTu3EntryPoint, ms);
      REXLOG_INFO("{}", g_summary);
      return true;
    }
  } // namespace

  void Register() { rex::runtime::XexModule::SetImageTranslator(&Translate); }

  State GetState() { return g_state; }

  const std::string &Summary() { return g_summary; }

  bool Enabled() { return REXCVAR_GET(pdz_platinum); }
} // namespace pdz::platinum
