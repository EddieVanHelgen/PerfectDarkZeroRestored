// Native renderer (stage 1): per-draw dump of chosen draws (native_drawdump.h).
//
// Selection: pdz_native_draw_dump = hex prefixes of pixel shader hashes ("c2bdc141", "c2bdc141,38b7d2f9", "*" = every
// draw). A dump frame starts at the first matching draw of a guest frame (DrawRecord::frame, the "f" of the
// pdz_render_shadow lines; the auto-diff's swapN frame is f = N - 1) at or after pdz_native_draw_dump_from and at least
// pdz_native_draw_dump_every frames after the previous dump frame; pdz_native_draw_dump_frames such frames are dumped.
// Every matching draw of a dump frame gets its "pdz_drawdump:" lines (at most pdz_native_draw_dump_lines a frame); the
// pdz_native_draw_dump_pick-th draw matching the first prefix also gets the files (pdz_native_draw_dump_dir):
//
//   f<frame>_d<seq>_<stage>t<slot>_<address>_<format>_<w>x<h>.raw / .pgm / .ppm   texture level 0 as the GPU holds it
//   f<frame>_d<seq>_rt0_before.* and f<frame>_d<seq>_rt0_after.*                 colour target 0 around the draw
//   f<frame>_d<seq>_draw.json                                                   constants, fetch constants, samplers,
//                                                                                vertices, state, the files above
//
// and pdz_native_draw_dump_rt - 1 further matching draws get their own _rt0_after files (which later draw changed
// what). .raw files are the rows at the readback pitch (the .json gives format, pitch and size); .ppm / .pgm exist for
// 8-bit UNORM / sRGB, 10:10:10:2 and 16-bit float formats (alpha of RGBA in <stem>_alpha.pgm).
#include "native_drawdump.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/graphics/registers.h>
#include <rex/hash.h> // xxhash.h with XXH_INLINE_ALL
#include <rex/logging.h>

#include "native.h"
#include "native_device.h"
#include "native_draw.h"
#include "native_recthread.h"
#include "native_surfaces.h"
#include "native_textures.h"

REXCVAR_DEFINE_STRING(pdz_native_draw_dump, "", "Render",
                      "Native renderer diagnostic: hex prefixes of pixel shader hashes (comma separated, '*' = every "
                      "draw) whose draws are dumped: 'pdz_drawdump:' lines (state, uploaded constants, textures, "
                      "samplers, UP vertices) and, for the picked draw, textures and colour target 0 read back to "
                      "pdz_native_draw_dump_dir. Empty = off (stage1/subagents/bink.md).");
REXCVAR_DEFINE_INT32(pdz_native_draw_dump_frames, 2, "Render",
                     "Native renderer diagnostic: number of frames pdz_native_draw_dump dumps.");
REXCVAR_DEFINE_INT32(pdz_native_draw_dump_from, 0, "Render",
                     "Native renderer diagnostic: first guest frame (the shadow's f; the auto-diff's swapN frame is "
                     "f = N - 1) pdz_native_draw_dump may dump.");
REXCVAR_DEFINE_INT32(pdz_native_draw_dump_every, 1, "Render",
                     "Native renderer diagnostic: minimum distance in frames between two pdz_native_draw_dump frames.");
REXCVAR_DEFINE_STRING(pdz_native_draw_dump_dir, "", "Render",
                      "Native renderer diagnostic: folder for the pdz_native_draw_dump files (empty = lines only).");
REXCVAR_DEFINE_INT32(pdz_native_draw_dump_pick, 0, "Render",
                     "Native renderer diagnostic: which draw matching the FIRST pdz_native_draw_dump prefix (0 = the "
                     "first) gets the files (textures read back, colour target 0 before / after, .json) in each frame.");
REXCVAR_DEFINE_INT32(pdz_native_draw_dump_rt, 1, "Render",
                     "Native renderer diagnostic: colour target 0 is read back before the picked draw and after it and "
                     "the next N - 1 matching draws (0 = no target readback).");
REXCVAR_DEFINE_INT32(pdz_native_draw_dump_lines, 800, "Render",
                     "Native renderer diagnostic: at most this many pdz_drawdump lines per dumped frame.");
REXCVAR_DEFINE_INT32(pdz_native_draw_dump_vs_consts, 12, "Render",
                     "Native renderer diagnostic: VS float constants c0.. logged per dumped draw (c252..c255 are added).");
REXCVAR_DEFINE_INT32(pdz_native_draw_dump_ps_consts, 4, "Render",
                     "Native renderer diagnostic: PS float constants c0.. logged per dumped draw.");

namespace pdz::native::drawdump
{
  bool g_armed = false;

  namespace
  {
    constexpr uint32_t kPsFloatCount = 224;   // as native_draw.cpp: PS c0..c223 are uploaded
    constexpr uint32_t kMaxVertexLog = 4;     // vertices per log line
    constexpr uint32_t kMaxVertexJson = 64;   // vertices in the picked draw's .json
    constexpr uint32_t kMaxRanges = 256;      // texture ranges re-hashed at the swap

    struct Prefix
    {
      uint64_t value = 0;
      uint32_t shift = 64; // match: (hash >> shift) == value; 64 = every hash ('*')
    };

    // Guest memory of a texture a logged draw sampled, hashed at the draw (re-hashed at the swap).
    struct Range
    {
      uint32_t address = 0;
      uint32_t bytes = 0;
      uint64_t hash = 0;
      uint32_t sequence = 0; // the last logged draw that sampled it
      uint32_t format = 0;
      uint32_t width = 0, height = 0;
    };

    struct State
    {
      std::string spec; // the cvar value the prefixes were parsed from
      std::vector<Prefix> prefixes;
      bool frame_active = false; // the current guest frame is a dump frame
      uint32_t frame = 0;        // its guest frame
      uint32_t frames_done = 0;
      int64_t last_frame = -1;   // guest frame of the previous dump frame
      uint32_t matches = 0;      // matching draws of the dump frame
      uint32_t first_matches = 0; // ... matching the first prefix
      uint32_t lines = 0;
      uint32_t lines_dropped = 0;
      bool picked_done = false;
      uint32_t picked_sequence = 0;
      uint32_t rt_after_left = 0;
      uint32_t files = 0;
      // Between BeforeDraw and AfterDraw.
      bool pending = false;
      uint32_t pending_sequence = 0;
      bool pending_picked = false;
      std::vector<std::string> picked_files; // files of the picked draw written by BeforeDraw (.json list)
      std::vector<Range> ranges;
      std::filesystem::path dir;
      bool dir_ready = false;
      ID3D12Resource *readback = nullptr;
      uint64_t readback_size = 0;
    };
    State g;

    const char *KindName(shadow::Kind k)
    {
      switch (k)
      {
      case shadow::Kind::kDrawIndexedVertices: return "DrawIndexedVertices";
      case shadow::Kind::kDrawVertices: return "DrawVertices";
      case shadow::Kind::kDrawVerticesUP: return "DrawVerticesUP";
      case shadow::Kind::kBeginVertices: return "BeginVertices";
      case shadow::Kind::kDrawIndexedVerticesUP: return "DrawIndexedVerticesUP";
      default: return "other";
      }
    }

    void ParsePrefixes(const std::string &spec)
    {
      g.prefixes.clear();
      std::string token;
      auto flush = [&]() {
        std::string t = token;
        token.clear();
        if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X'))
        {
          t = t.substr(2);
        }
        if (t.empty())
        {
          return;
        }
        Prefix p;
        if (t == "*")
        {
          g.prefixes.push_back(p); // shift 64: every draw
          return;
        }
        if (t.size() > 16)
        {
          t.resize(16);
        }
        uint64_t v = 0;
        for (char c : t)
        {
          uint32_t d;
          if (c >= '0' && c <= '9') d = uint32_t(c - '0');
          else if (c >= 'a' && c <= 'f') d = uint32_t(c - 'a' + 10);
          else if (c >= 'A' && c <= 'F') d = uint32_t(c - 'A' + 10);
          else
          {
            REXLOG_WARN("pdz_drawdump: ignored prefix '{}' (hex digits or '*')", t);
            return;
          }
          v = (v << 4) | d;
        }
        p.value = v;
        p.shift = 64 - 4 * uint32_t(t.size());
        g.prefixes.push_back(p);
      };
      for (char c : spec)
      {
        if (c == ',' || c == ';' || c == ' ' || c == '\t')
        {
          flush();
        }
        else
        {
          token.push_back(c);
        }
      }
      flush();
    }

    bool PrefixMatches(const Prefix &p, uint64_t hash) { return p.shift >= 64 || (hash >> p.shift) == p.value; }

    bool AnyMatch(uint64_t hash)
    {
      for (const Prefix &p : g.prefixes)
      {
        if (PrefixMatches(p, hash))
        {
          return true;
        }
      }
      return false;
    }

    bool Line(const std::string &text)
    {
      if (g.lines >= uint32_t(std::max(0, REXCVAR_GET(pdz_native_draw_dump_lines))))
      {
        ++g.lines_dropped;
        return false;
      }
      ++g.lines;
      REXLOG_INFO("pdz_drawdump: {}", text);
      return true;
    }

    // A float for a log: %g, or the raw bits when it is NaN / infinite / denormal / huge (packed colours, indices).
    std::string Num(uint32_t bits)
    {
      const uint32_t exponent = (bits >> 23) & 0xFFu;
      float f;
      std::memcpy(&f, &bits, 4);
      if (exponent == 0xFFu || (exponent == 0 && (bits & 0x7FFFFFu) != 0) || std::fabs(f) > 1.0e7f)
      {
        return fmt::format("0x{:08X}", bits);
      }
      return fmt::format("{:.5g}", f);
    }

    std::string Float4(const uint32_t v[4])
    {
      return fmt::format("({} {} {} {})", Num(v[0]), Num(v[1]), Num(v[2]), Num(v[3]));
    }

    // Constant register `reg` from a host-order buffer (the uploaded constant buffer).
    void HostFloat4(const uint8_t *cb, uint32_t reg, uint32_t out[4]) { std::memcpy(out, cb + 16 * reg, 16); }

    // Constant register `reg` from the big-endian device store (`device` + `offset`).
    void DeviceFloat4(const uint8_t *device, uint32_t offset, uint32_t reg, uint32_t out[4])
    {
      for (uint32_t i = 0; i < 4; ++i)
      {
        uint32_t be;
        std::memcpy(&be, device + offset + 16 * reg + 4 * i, 4);
        out[i] = Swap32(be);
      }
    }

    uint64_t GuestHash(uint32_t address, uint32_t bytes)
    {
      // As native_textures.cpp FullHash (the same function and bounds), so it compares with HostTexture::full_hash.
      if (!bytes || uint64_t(address) + bytes > 0x20000000ull)
      {
        return 0;
      }
      const uint8_t *p = PhysPtr(address);
      return p ? XXH3_64bits(p, bytes) : 0;
    }

    void NoteRange(uint32_t address, uint32_t bytes, uint64_t hash, uint32_t sequence, const FetchDecoded &f)
    {
      for (Range &r : g.ranges)
      {
        if (r.address == address && r.bytes == bytes)
        {
          r.hash = hash;
          r.sequence = sequence;
          return;
        }
      }
      if (g.ranges.size() < kMaxRanges)
      {
        Range r;
        r.address = address;
        r.bytes = bytes;
        r.hash = hash;
        r.sequence = sequence;
        r.format = f.format;
        r.width = f.width;
        r.height = f.height;
        g.ranges.push_back(r);
      }
    }

    bool EnsureDir()
    {
      if (g.dir_ready)
      {
        return true;
      }
      const std::string dir = REXCVAR_GET(pdz_native_draw_dump_dir);
      if (dir.empty())
      {
        return false;
      }
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      g.dir = dir;
      g.dir_ready = true;
      return true;
    }

    bool WriteBytes(const std::string &name, const void *data, size_t bytes)
    {
      FILE *f = std::fopen((g.dir / name).string().c_str(), "wb");
      if (!f)
      {
        return false;
      }
      std::fwrite(data, 1, bytes, f);
      std::fclose(f);
      ++g.files;
      return true;
    }

    float HalfToFloat(uint16_t h)
    {
      const uint32_t sign = uint32_t(h >> 15) << 31;
      uint32_t exponent = (h >> 10) & 0x1Fu;
      uint32_t mantissa = h & 0x3FFu;
      uint32_t bits;
      if (exponent == 0)
      {
        if (mantissa == 0)
        {
          bits = sign;
        }
        else
        {
          exponent = 1;
          while (!(mantissa & 0x400u))
          {
            mantissa <<= 1;
            --exponent;
          }
          mantissa &= 0x3FFu;
          bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
        }
      }
      else if (exponent == 31)
      {
        bits = sign | 0x7F800000u | (mantissa << 13);
      }
      else
      {
        bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
      }
      float f;
      std::memcpy(&f, &bits, 4);
      return f;
    }

    uint8_t Unit8(float v)
    {
      if (!(v > 0.0f))
      {
        return 0;
      }
      return v >= 1.0f ? uint8_t(255) : uint8_t(v * 255.0f + 0.5f);
    }

    // .ppm / .pgm next to the .raw for the formats a viewer can show; false = raw only.
    bool WriteImage(const std::string &stem, DXGI_FORMAT format, uint32_t width, uint32_t height, const uint8_t *data,
                    uint32_t row_pitch)
    {
      enum class Kind
      {
        kNone,
        kR8,
        kRG8,
        kRGBA8,
        kBGRA8,
        kRGB10A2,
        kRGBA16F,
        kR16F,
        kR32F
      } kind = Kind::kNone;
      switch (format)
      {
      case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_A8_UNORM: kind = Kind::kR8; break;
      case DXGI_FORMAT_R8G8_UNORM: kind = Kind::kRG8; break;
      case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        kind = Kind::kRGBA8;
        break;
      case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM:
        kind = Kind::kBGRA8;
        break;
      case DXGI_FORMAT_R10G10B10A2_UNORM: kind = Kind::kRGB10A2; break;
      case DXGI_FORMAT_R16G16B16A16_FLOAT: kind = Kind::kRGBA16F; break;
      case DXGI_FORMAT_R16_FLOAT: kind = Kind::kR16F; break;
      case DXGI_FORMAT_R32_FLOAT: kind = Kind::kR32F; break;
      default: return false;
      }
      const bool grey = kind == Kind::kR8 || kind == Kind::kR16F || kind == Kind::kR32F;
      const bool alpha = kind == Kind::kRGBA8 || kind == Kind::kBGRA8 || kind == Kind::kRGB10A2 || kind == Kind::kRGBA16F;
      std::vector<uint8_t> rgb(size_t(width) * height * (grey ? 1 : 3));
      std::vector<uint8_t> a(alpha ? size_t(width) * height : 0);
      for (uint32_t y = 0; y < height; ++y)
      {
        const uint8_t *row = data + size_t(y) * row_pitch;
        for (uint32_t x = 0; x < width; ++x)
        {
          const size_t o = size_t(y) * width + x;
          uint8_t px[4] = {0, 0, 0, 255};
          switch (kind)
          {
          case Kind::kR8: px[0] = row[x]; break;
          case Kind::kRG8: px[0] = row[2 * x]; px[1] = row[2 * x + 1]; break;
          case Kind::kRGBA8: std::memcpy(px, row + 4 * x, 4); break;
          case Kind::kBGRA8:
            px[0] = row[4 * x + 2];
            px[1] = row[4 * x + 1];
            px[2] = row[4 * x];
            px[3] = format == DXGI_FORMAT_B8G8R8X8_UNORM ? uint8_t(255) : row[4 * x + 3];
            break;
          case Kind::kRGB10A2:
          {
            uint32_t v;
            std::memcpy(&v, row + 4 * x, 4);
            px[0] = uint8_t((v & 0x3FFu) >> 2);
            px[1] = uint8_t(((v >> 10) & 0x3FFu) >> 2);
            px[2] = uint8_t(((v >> 20) & 0x3FFu) >> 2);
            px[3] = uint8_t((v >> 30) * 85u);
            break;
          }
          case Kind::kRGBA16F:
            for (uint32_t c = 0; c < 4; ++c)
            {
              uint16_t h;
              std::memcpy(&h, row + 8 * x + 2 * c, 2);
              px[c] = Unit8(HalfToFloat(h));
            }
            break;
          case Kind::kR16F:
          {
            uint16_t h;
            std::memcpy(&h, row + 2 * x, 2);
            px[0] = Unit8(HalfToFloat(h));
            break;
          }
          case Kind::kR32F:
          {
            float f;
            std::memcpy(&f, row + 4 * x, 4);
            px[0] = Unit8(f);
            break;
          }
          default: break;
          }
          if (grey)
          {
            rgb[o] = px[0];
          }
          else
          {
            rgb[3 * o] = px[0];
            rgb[3 * o + 1] = px[1];
            rgb[3 * o + 2] = px[2];
          }
          if (alpha)
          {
            a[o] = px[3];
          }
        }
      }
      const std::string header = fmt::format("{}\n{} {}\n255\n", grey ? "P5" : "P6", width, height);
      std::string file = header;
      file.append(reinterpret_cast<const char *>(rgb.data()), rgb.size());
      WriteBytes(stem + (grey ? ".pgm" : ".ppm"), file.data(), file.size());
      if (alpha)
      {
        std::string af = fmt::format("P5\n{} {}\n255\n", width, height);
        af.append(reinterpret_cast<const char *>(a.data()), a.size());
        WriteBytes(stem + "_alpha.pgm", af.data(), af.size());
      }
      return true;
    }

    // Copies subresource 0 of `resource` (level 0; slice 0 of an array, every slice of a volume) to the readback
    // buffer, flushes, and returns the rows at the footprint pitch. The resource is transitioned to COPY_SOURCE and
    // back to the state it was in, so the draw's own bindings stay valid. One GPU round trip.
    bool Readback(NativeDevice &dev, ID3D12Resource *resource, D3D12_RESOURCE_STATES &state, std::vector<uint8_t> &out,
                  D3D12_PLACED_SUBRESOURCE_FOOTPRINT &fp, UINT &rows)
    {
      if (!resource || !dev.in_frame())
      {
        return false;
      }
      D3D12_RESOURCE_DESC desc = resource->GetDesc();
      UINT64 row_size = 0, total = 0;
      dev.device()->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &row_size, &total);
      if (!total || total > (256ull << 20))
      {
        return false;
      }
      if (!g.readback || g.readback_size < total)
      {
        if (g.readback)
        {
          dev.ReleaseLater(g.readback);
        }
        g.readback = dev.CreateBuffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                                      D3D12_RESOURCE_FLAG_NONE, "pdz_native draw dump readback");
        g.readback_size = g.readback ? total : 0;
        if (!g.readback)
        {
          return false;
        }
      }
      const D3D12_RESOURCE_STATES before = state;
      dev.Transition(resource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION dst{};
      dst.pResource = g.readback;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint = fp;
      D3D12_TEXTURE_COPY_LOCATION src{};
      src.pResource = resource;
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.SubresourceIndex = 0;
      dev.list()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      dev.Transition(resource, state, before);
      dev.Flush();
      uint8_t *mapped = nullptr;
      const D3D12_RANGE range{0, SIZE_T(total)};
      if (FAILED(g.readback->Map(0, &range, reinterpret_cast<void **>(&mapped))) || !mapped)
      {
        return false;
      }
      const size_t bytes =
          size_t(fp.Footprint.RowPitch) * rows * std::max<UINT>(fp.Footprint.Depth, 1u);
      out.assign(mapped + fp.Offset, mapped + fp.Offset + std::min<size_t>(bytes, size_t(total - fp.Offset)));
      const D3D12_RANGE none{0, 0};
      g.readback->Unmap(0, &none);
      return true;
    }

    // Reads back `resource` and writes <stem>.raw (+ image); returns the file stem's .json fragment or "".
    std::string DumpResource(NativeDevice &dev, ID3D12Resource *resource, D3D12_RESOURCE_STATES &state,
                             const std::string &stem, const char *what)
    {
      std::vector<uint8_t> data;
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
      UINT rows = 0;
      if (!Readback(dev, resource, state, data, fp, rows))
      {
        Line(fmt::format("  {} readback failed ({})", what, stem));
        return {};
      }
      WriteBytes(stem + ".raw", data.data(), data.size());
      const bool image = WriteImage(stem, fp.Footprint.Format, fp.Footprint.Width, fp.Footprint.Height, data.data(),
                                    fp.Footprint.RowPitch);
      g.picked_files.push_back(stem);
      return fmt::format("{{\"what\":\"{}\",\"stem\":\"{}\",\"dxgi\":{},\"width\":{},\"height\":{},\"depth\":{},"
                         "\"row_pitch\":{},\"rows\":{},\"image\":{}}}",
                         what, stem, uint32_t(fp.Footprint.Format), fp.Footprint.Width, fp.Footprint.Height,
                         fp.Footprint.Depth, fp.Footprint.RowPitch, rows, image ? "true" : "false");
    }

    std::string g_picked_json_files; // .json fragments of the picked draw's files (BeforeDraw + AfterDraw)

    void AppendFileJson(const std::string &fragment)
    {
      if (fragment.empty())
      {
        return;
      }
      if (!g_picked_json_files.empty())
      {
        g_picked_json_files += ",";
      }
      g_picked_json_files += fragment;
    }

    std::string Stem(uint32_t frame, uint32_t sequence) { return fmt::format("f{}_d{}", frame, sequence); }

    // Before the picked draw: its textures as the GPU holds them and colour target 0.
    void DumpPickedBefore(uint8_t *base, const shadow::DrawRecord &r, const NativeShader *vs, const NativeShader *ps)
    {
      NativeDevice *dev = NativeDevice::Get();
      if (!dev || !dev->in_frame() || !EnsureDir())
      {
        return;
      }
      const std::string stem = Stem(r.frame, r.sequence);
      for (const NativeShader *s : {vs, ps})
      {
        for (const ShaderTexture &t : s->textures)
        {
          const FetchDecoded f = DecodeFetch(r.fetch_be + t.slot * shadow::dev::kFetchConstantBytes);
          const TextureView view = Textures::Get().Lookup(f);
          if (!view.texture)
          {
            continue;
          }
          const std::string name = fmt::format("{}_{}t{}_{:08X}_{}_{}x{}", stem, s == vs ? "vs" : "ps", t.slot,
                                               f.base_address, TextureFormatName(f.format), f.width, f.height);
          AppendFileJson(DumpResource(*dev, view.texture->resource, view.texture->state, name,
                                      s == vs ? "vs texture" : "ps texture"));
        }
      }
      if (REXCVAR_GET(pdz_native_draw_dump_rt) > 0)
      {
        const Surfaces::Bound b = Surfaces::Get().GetBound(base, r.device);
        if (b.rt[0] && b.rt[0]->resource)
        {
          AppendFileJson(DumpResource(*dev, b.rt[0]->resource, b.rt[0]->state, stem + "_rt0_before", "rt0 before"));
        }
      }
    }

    void EndPending(const char *why)
    {
      if (g.pending)
      {
        Line(fmt::format("f{} #{} matched but was not drawn ({})", g.frame, g.pending_sequence, why));
        g.pending = false;
      }
    }

    void EndFrame(uint8_t *base, const char *why)
    {
      (void)base;
      EndPending(why);
      if (!g.frame_active)
      {
        return;
      }
      // Texture memory of the logged draws, hashed again: a change means the CPU rewrote it after the draw in this
      // frame; the emulator's GPU samples it after the CPU's writes, the native at draw time.
      uint32_t changed = 0;
      for (const Range &rg : g.ranges)
      {
        const uint64_t now = GuestHash(rg.address, rg.bytes);
        if (now != rg.hash)
        {
          ++changed;
          Line(fmt::format("f{} texture {:08X} {} {}x{} ({} bytes) CHANGED after its last logged draw #{} (written "
                           "later in the frame; the emulator samples the later data)",
                           g.frame, rg.address, TextureFormatName(rg.format), rg.width, rg.height, rg.bytes, rg.sequence));
        }
      }
      REXLOG_INFO("pdz_drawdump: f{} done ({}): {} matching draws ({} with the first prefix), picked #{}{}, {} files, "
                  "{} textures re-hashed at the swap ({} changed after their draw), {} lines dropped (cap "
                  "pdz_native_draw_dump_lines)",
                  g.frame, why, g.matches, g.first_matches, g.picked_sequence, g.picked_done ? "" : " (none)", g.files,
                  g.ranges.size(), changed, g.lines_dropped);
      ++g.frames_done;
      g.last_frame = int64_t(g.frame);
      g.frame_active = false;
      g.ranges.clear();
      g.picked_done = false;
      g.rt_after_left = 0;
      g.files = 0;
      if (g.frames_done >= uint32_t(std::max(0, REXCVAR_GET(pdz_native_draw_dump_frames))))
      {
        g_armed = false;
        REXLOG_INFO("pdz_drawdump: all {} frames dumped", g.frames_done);
        // The readback buffer (up to the largest texture / target read back) is not needed any more; it used to be
        // kept for the session. A re-armed dump (new prefix list) makes a new one.
        if (g.readback)
        {
          if (NativeDevice *dev = NativeDevice::Existing())
          {
            dev->ReleaseLater(g.readback);
          }
          g.readback = nullptr;
          g.readback_size = 0;
        }
      }
    }
  } // namespace

  uint64_t ReadbackBytes() { return g.readback ? g.readback_size : 0; }

  void NewFrame()
  {
    if (pdz::gpusync::NullBackend() || recthread::Active())
    {
      // graphics_backend null, or pdz_native_record_thread (the dump reads guest memory around the draw): refused (stage 2 M1: no stage-1 diagnostics without the plugin; logged once by
      // native_present.cpp).
      g_armed = false;
      return;
    }
    const std::string spec = REXCVAR_GET(pdz_native_draw_dump);
    if (spec != g.spec)
    {
      g.spec = spec;
      ParsePrefixes(spec);
      g.frames_done = 0;
      g.last_frame = -1;
      if (!g.prefixes.empty())
      {
        REXLOG_INFO("pdz_drawdump: armed for PS prefixes '{}' ({} frames from f{} every {}, pick {}, rt {}, dir '{}')",
                    spec, REXCVAR_GET(pdz_native_draw_dump_frames), REXCVAR_GET(pdz_native_draw_dump_from),
                    REXCVAR_GET(pdz_native_draw_dump_every), REXCVAR_GET(pdz_native_draw_dump_pick),
                    REXCVAR_GET(pdz_native_draw_dump_rt), REXCVAR_GET(pdz_native_draw_dump_dir));
      }
    }
    g_armed = !g.prefixes.empty() &&
              (g.frame_active || g.frames_done < uint32_t(std::max(0, REXCVAR_GET(pdz_native_draw_dump_frames))));
  }

  bool BeforeDraw(uint8_t *base, const shadow::DrawRecord &r, const NativeShader *vs, const NativeShader *ps)
  {
    if (!g_armed || !vs || !ps)
    {
      return false;
    }
    EndPending("skipped after the shader lookup");
    if (!AnyMatch(ps->hash))
    {
      return false;
    }
    if (g.frame_active && r.frame != g.frame)
    {
      EndFrame(base, "frame changed without a swap");
    }
    if (!g.frame_active)
    {
      const int32_t from = REXCVAR_GET(pdz_native_draw_dump_from);
      const int32_t every = std::max(1, REXCVAR_GET(pdz_native_draw_dump_every));
      if (int64_t(r.frame) < int64_t(from) || (g.last_frame >= 0 && int64_t(r.frame) - g.last_frame < every) ||
          g.frames_done >= uint32_t(std::max(0, REXCVAR_GET(pdz_native_draw_dump_frames))))
      {
        return false;
      }
      g.frame_active = true;
      g.frame = r.frame;
      g.matches = g.first_matches = g.lines = g.lines_dropped = 0;
      g.picked_done = false;
      g.picked_sequence = 0;
      g.rt_after_left = 0;
      g.files = 0;
      g.ranges.clear();
      REXLOG_INFO("pdz_drawdump: f{} dump frame {} of {} begins", r.frame, g.frames_done + 1,
                  REXCVAR_GET(pdz_native_draw_dump_frames));
    }
    ++g.matches;
    bool picked = false;
    if (!g.prefixes.empty() && PrefixMatches(g.prefixes[0], ps->hash))
    {
      const uint32_t index = g.first_matches++;
      picked = !g.picked_done && int64_t(index) == int64_t(REXCVAR_GET(pdz_native_draw_dump_pick));
    }
    g.pending = true;
    g.pending_sequence = r.sequence;
    g.pending_picked = picked;
    if (picked)
    {
      g.picked_done = true;
      g.picked_sequence = r.sequence;
      g.picked_files.clear();
      g_picked_json_files.clear();
      g.rt_after_left = uint32_t(std::max(0, REXCVAR_GET(pdz_native_draw_dump_rt)));
      DumpPickedBefore(base, r, vs, ps);
    }
    return true;
  }

  void AfterDraw(uint8_t *base, const shadow::DrawRecord &r, const DrawInfo &info)
  {
    if (!g.pending || g.pending_sequence != r.sequence || !info.vs || !info.ps)
    {
      return;
    }
    g.pending = false;
    const bool picked = g.pending_picked;
    NativeDevice *dev = NativeDevice::Get();
    const RenderState *rs = info.rs;
    const HostViewport *hv = info.hv;

    // Draw line: what the native drew it with.
    {
      std::string rt0 = "-";
      if (info.rt0)
      {
        const HostSurface *s = info.rt0;
        rt0 = TraceSurface(s->guest, s->width, s->height, s->color_format, s->edram_base, false);
      }
      std::string line = fmt::format("f{} #{} {} caller {:08X} prim {} count {} (host {}) | vs {:016x} ps {:016x}{}",
                                     r.frame, r.sequence, KindName(r.kind), r.caller, info.prim, info.guest_count,
                                     info.host_count, info.vs->hash, info.ps->hash, picked ? " | PICKED" : "");
      if (rs)
      {
        line += fmt::format(" | blend0 {:08X} colormask {:04X} depthcontrol {:08X} colorcontrol {:08X} alpha test {} "
                            "ref {:.4f}",
                            rs->blendcontrol[0], rs->color_mask, rs->depthcontrol, rs->colorcontrol, info.alpha_func,
                            info.alpha_ref);
      }
      if (hv)
      {
        line += fmt::format(" | vp {:.1f},{:.1f} {:.1f}x{:.1f} z {:.3f}..{:.3f} scissor {},{}..{},{} ndc scale {:.5f} "
                            "{:.5f} {:.5f} offset {:.5f} {:.5f} {:.5f} vte {} clip {:08X}",
                            hv->viewport.TopLeftX, hv->viewport.TopLeftY, hv->viewport.Width, hv->viewport.Height,
                            hv->viewport.MinDepth, hv->viewport.MaxDepth, info.scissor.left, info.scissor.top,
                            info.scissor.right, info.scissor.bottom, hv->ndc_scale[0], hv->ndc_scale[1],
                            hv->ndc_scale[2], hv->ndc_offset[0], hv->ndc_offset[1], hv->ndc_offset[2], hv->vte_flags,
                            rs ? rs->clip_cntl : 0u);
      }
      line += fmt::format(" | rt0 {} | pso {}", rt0, info.pso);
      Line(line);
      // The raw registers the host viewport / NDC transform came from, and D3D's own viewport struct.
      namespace gr = rex::graphics;
      Line(fmt::format("  regs vte {:08X} vport x {:.4f} {:.4f} y {:.4f} {:.4f} z {:.6f} {:.6f} (scale offset) clip {:08X} "
                       "su_vtx {:08X} su_sc_mode {:08X} window_offset {:08X} | d3d viewport {},{} {}x{} z {}..{}",
                       ReadReg(r, gr::XE_GPU_REG_PA_CL_VTE_CNTL), ReadRegFloat(r, gr::XE_GPU_REG_PA_CL_VPORT_XSCALE),
                       ReadRegFloat(r, gr::XE_GPU_REG_PA_CL_VPORT_XOFFSET),
                       ReadRegFloat(r, gr::XE_GPU_REG_PA_CL_VPORT_YSCALE),
                       ReadRegFloat(r, gr::XE_GPU_REG_PA_CL_VPORT_YOFFSET),
                       ReadRegFloat(r, gr::XE_GPU_REG_PA_CL_VPORT_ZSCALE),
                       ReadRegFloat(r, gr::XE_GPU_REG_PA_CL_VPORT_ZOFFSET), ReadReg(r, gr::XE_GPU_REG_PA_CL_CLIP_CNTL),
                       ReadReg(r, gr::XE_GPU_REG_PA_SU_VTX_CNTL), ReadReg(r, gr::XE_GPU_REG_PA_SU_SC_MODE_CNTL),
                       ReadReg(r, gr::XE_GPU_REG_PA_SC_WINDOW_OFFSET), r.viewport[0], r.viewport[1], r.viewport[2],
                       r.viewport[3], Num(r.viewport[4]), Num(r.viewport[5])));
    }

    // Constants: the buffers the GPU read for this draw, against the device store (a difference = constants the
    // native did not upload, e.g. a reused buffer the game changed without a dirty bit, or a literal).
    auto constants = [&](const char *stage, const uint8_t *cb, uint32_t device_offset, uint32_t count, bool reused,
                         bool add_top) {
      std::string text = fmt::format("  {} [{}{}]", stage, reused ? "buffer reused from an earlier draw" : "uploaded",
                                     info.literals ? ", literals on" : "");
      std::vector<uint32_t> regs;
      for (uint32_t i = 0; i < count; ++i)
      {
        regs.push_back(i);
      }
      if (add_top)
      {
        for (uint32_t i = 252; i < 256; ++i)
        {
          if (i >= count)
          {
            regs.push_back(i);
          }
        }
      }
      uint32_t differ = 0;
      std::string first_differ;
      for (uint32_t reg : regs)
      {
        uint32_t c[4] = {}, d[4] = {};
        if (cb)
        {
          HostFloat4(cb, reg, c);
        }
        if (info.device)
        {
          DeviceFloat4(info.device, device_offset, reg, d);
        }
        text += fmt::format(" c{} {}", reg, Float4(cb ? c : d));
        if (cb && info.device && std::memcmp(c, d, 16) != 0)
        {
          if (!differ++)
          {
            first_differ = fmt::format(" (c{}: device {})", reg, Float4(d));
          }
        }
      }
      if (!cb)
      {
        text += " [no uploaded buffer: device values]";
      }
      else if (differ)
      {
        text += fmt::format(" | {} register(s) DIFFER from the device store{}", differ, first_differ);
      }
      else
      {
        text += " | = device store";
      }
      Line(text);
    };
    const uint32_t ps_count = uint32_t(std::clamp(REXCVAR_GET(pdz_native_draw_dump_ps_consts), 0, int32_t(kPsFloatCount)));
    const uint32_t vs_count = uint32_t(std::clamp(REXCVAR_GET(pdz_native_draw_dump_vs_consts), 0, 256));
    constants("ps", info.ps_cb, shadow::dev::kPsAluConstants, ps_count, info.ps_cb_reused, false);
    constants("vs", info.vs_cb, shadow::dev::kVsAluConstants, vs_count, info.vs_cb_reused, true);
    // Bool constants as the draw uploaded them (UploadBoolLoop: the device store, host order; PDZ has no bool
    // literals). Shader index 128 + n = PS b<n> = bit n of the first ps dword (the water shaders' light source:
    // b0 2D projector, b1 3D projector, b2 constant colour c27).
    Line(fmt::format("  bools vs b0-127 {:08X} {:08X} {:08X} {:08X} | ps b0-127 {:08X} {:08X} {:08X} {:08X}",
                     Swap32(r.bools_be[0]), Swap32(r.bools_be[1]), Swap32(r.bools_be[2]), Swap32(r.bools_be[3]),
                     Swap32(r.bools_be[4]), Swap32(r.bools_be[5]), Swap32(r.bools_be[6]), Swap32(r.bools_be[7])));

    // Textures: fetch constant, host view, freshness of the upload.
    std::string tex_json;
    for (const NativeShader *s : {info.vs, info.ps})
    {
      for (const ShaderTexture &t : s->textures)
      {
        const FetchDecoded f = DecodeFetch(r.fetch_be + t.slot * shadow::dev::kFetchConstantBytes);
        std::string host = "no host texture (unsupported / no fetch)";
        std::string freshness;
        bool stale = false;
        uint64_t now_hash = 0;
        const TextureView view = (f.type == 2 && f.base_address) ? Textures::Get().Lookup(f) : TextureView{};
        if (view.texture)
        {
          const HostTexture &h = *view.texture;
          host = fmt::format("host dxgi {} {}x{}x{} mips {}", uint32_t(h.resource_format), h.width, h.height,
                             std::max(h.depth, h.array_size), h.mips);
          if (h.mirror)
          {
            freshness = fmt::format(" resolve mirror (scale {}, generation {})", h.scale, h.generation);
          }
          else
          {
            now_hash = GuestHash(f.base_address, h.full_bytes);
            stale = now_hash != h.full_hash;
            const uint64_t age = dev ? dev->frame_index() - h.upload_frame : 0;
            freshness = fmt::format(" upload of {} frame(s) ago{}, {} bytes {}", age,
                                    h.volatile_content ? " (volatile: checked every frame)" : "", h.full_bytes,
                                    stale ? "STALE (guest memory changed since the upload)" : "= guest memory");
            NoteRange(f.base_address, h.full_bytes, now_hash, r.sequence, f);
          }
        }
        // What a tfetch3D slot really got (BindSrvTable's rule): the Texture3D itself, the volume view of a 2D /
        // stacked texture, or a null SRV (reads 0).
        std::string binding;
        if (t.dim == TexDim::k3D && view.texture)
        {
          if (view.texture->dimension == 2)
          {
            binding = " | tfetch3D: Texture3D";
          }
          else if (view.texture->dimension == 1)
          {
            if ((s->array_slots >> t.slot) & 1u)
            {
              binding = " | tfetch3D: Texture2DArray (pdz_native_stacked_array)";
            }
            else if (view.texture->mirror)
            {
              binding = " | tfetch3D: NULL (resolve mirror, no volume view)";
            }
            else if (VolumeViewFor(f))
            {
              binding = fmt::format(" | tfetch3D: volume view (vol filter mag {} min {})", f.vol_mag_filter,
                                    f.vol_min_filter);
            }
            else
            {
              binding = " | tfetch3D: NULL (kind mismatch: volume view off)";
            }
          }
        }
        Line(fmt::format("  {} tex slot {} dim {} {:08X} {} {}x{}x{} fetch-dim {}{} pitch {} {} endian {} swizzle {:04o} "
                         "signs {:02X} num_format {} exp_adjust {} mips {}..{} mip {:08X} clamp {}/{}/{} filter mag {} "
                         "min {} mip {} aniso {} border {} lod_bias {} | {}{}{} | fetch {:08X} {:08X} {:08X} {:08X} "
                         "{:08X} {:08X}",
                         s == info.vs ? "vs" : "ps", t.slot, uint32_t(t.dim), f.base_address, TextureFormatName(f.format),
                         f.width, f.height, f.depth, f.dimension, f.stacked ? " stacked" : "", f.pitch,
                         f.tiled ? "tiled" : "linear", f.endian, f.swizzle, f.signs, f.num_format, f.exp_adjust,
                         f.mip_min_level, f.mip_max_level, f.mip_address, f.clamp_x, f.clamp_y, f.clamp_z, f.mag_filter,
                         f.min_filter, f.mip_filter, f.aniso_filter, f.border_color, f.lod_bias, host, freshness, binding,
                         f.dwords[0], f.dwords[1], f.dwords[2], f.dwords[3], f.dwords[4], f.dwords[5]));
        if (picked)
        {
          tex_json += fmt::format(
              "{}{{\"stage\":\"{}\",\"slot\":{},\"dim\":{},\"fetch\":[{},{},{},{},{},{}],\"address\":{},\"format\":{},"
              "\"format_name\":\"{}\",\"width\":{},\"height\":{},\"depth\":{},\"pitch\":{},\"tiled\":{},\"endian\":{},"
              "\"swizzle\":{},\"signs\":{},\"num_format\":{},\"exp_adjust\":{},\"host_dxgi\":{},\"mirror\":{},"
              "\"stale\":{}}}",
              tex_json.empty() ? "" : ",", s == info.vs ? "vs" : "ps", t.slot, uint32_t(t.dim), f.dwords[0], f.dwords[1],
              f.dwords[2], f.dwords[3], f.dwords[4], f.dwords[5], f.base_address, f.format, TextureFormatName(f.format),
              f.width, f.height, f.depth, f.pitch, f.tiled ? "true" : "false", f.endian, f.swizzle, f.signs,
              f.num_format, f.exp_adjust, view.texture ? uint32_t(view.texture->resource_format) : 0u,
              view.texture && view.texture->mirror ? "true" : "false", stale ? "true" : "false");
        }
      }
    }

    // Samplers (the descriptors the draw's tables were built from).
    std::string sampler_json;
    for (const NativeShader *s : {info.vs, info.ps})
    {
      for (const ShaderSampler &sm : s->samplers)
      {
        const D3D12_SAMPLER_DESC d = GetSampler(r.fetch_be + sm.slot * shadow::dev::kFetchConstantBytes, sm.code);
        Line(fmt::format("  {} sampler {} slot {} code {:#x}: filter {:#x} address {}/{}/{} lod {:.2f}..{:.2f} bias "
                         "{:.3f} aniso {} border {:.0f},{:.0f},{:.0f},{:.0f}",
                         s == info.vs ? "vs" : "ps", sm.index, sm.slot, sm.code, uint32_t(d.Filter), uint32_t(d.AddressU),
                         uint32_t(d.AddressV), uint32_t(d.AddressW), d.MinLOD, d.MaxLOD, d.MipLODBias, d.MaxAnisotropy,
                         d.BorderColor[0], d.BorderColor[1], d.BorderColor[2], d.BorderColor[3]));
        if (picked)
        {
          sampler_json += fmt::format("{}{{\"stage\":\"{}\",\"index\":{},\"slot\":{},\"code\":{},\"filter\":{},"
                                      "\"address\":[{},{},{}],\"min_lod\":{},\"max_lod\":{},\"bias\":{},\"border\":{}}}",
                                      sampler_json.empty() ? "" : ",", s == info.vs ? "vs" : "ps", sm.index, sm.slot,
                                      sm.code, uint32_t(d.Filter), uint32_t(d.AddressU), uint32_t(d.AddressV),
                                      uint32_t(d.AddressW), d.MinLOD, d.MaxLOD, d.MipLODBias, d.BorderColor[0]);
        }
      }
    }

    // Vertices of a UP / BeginVertices draw, as uploaded (8in32 swap of the guest dwords).
    std::string vertex_json;
    if (info.user_data && shadow::IsGuestPointer(info.data) && info.stride >= 4)
    {
      const uint32_t dwords = info.stride / 4;
      const uint32_t shown = std::min(info.vertex_count, kMaxVertexLog);
      std::string text = fmt::format("  vertices at {:08X} stride {} count {}:", info.data, info.stride, info.vertex_count);
      for (uint32_t v = 0; v < shown; ++v)
      {
        text += fmt::format(" v{} (", v);
        for (uint32_t i = 0; i < std::min(dwords, 8u); ++i)
        {
          text += fmt::format("{}{}", i ? " " : "", Num(Load32(base, info.data + v * info.stride + 4 * i)));
        }
        text += ")";
      }
      Line(text);
      if (picked)
      {
        const uint32_t n = std::min(info.vertex_count, kMaxVertexJson);
        for (uint32_t v = 0; v < n; ++v)
        {
          vertex_json += v ? ",[" : "[";
          for (uint32_t i = 0; i < dwords; ++i)
          {
            vertex_json += fmt::format("{}{}", i ? "," : "", Load32(base, info.data + v * info.stride + 4 * i));
          }
          vertex_json += "]";
        }
      }
    }

    // Colour target 0 after the draw: the picked draw and the next matching ones (pdz_native_draw_dump_rt).
    if (g.rt_after_left > 0 && dev && dev->in_frame() && EnsureDir())
    {
      --g.rt_after_left;
      const Surfaces::Bound b = Surfaces::Get().GetBound(base, r.device);
      if (b.rt[0] && b.rt[0]->resource)
      {
        const std::string frag =
            DumpResource(*dev, b.rt[0]->resource, b.rt[0]->state, Stem(r.frame, r.sequence) + "_rt0_after", "rt0 after");
        if (!frag.empty())
        {
          Line(fmt::format("  rt0 after #{} -> {}_rt0_after (picked #{})", r.sequence, Stem(r.frame, r.sequence),
                           g.picked_sequence));
        }
        if (picked)
        {
          AppendFileJson(frag);
        }
      }
    }

    // The picked draw's .json: every constant of both buffers, fetch constants, samplers, vertices, state, files.
    if (picked && EnsureDir())
    {
      auto consts_json = [&](const uint8_t *cb, uint32_t device_offset, uint32_t count) {
        std::string out;
        for (uint32_t reg = 0; reg < count; ++reg)
        {
          uint32_t c[4] = {};
          if (cb)
          {
            HostFloat4(cb, reg, c);
          }
          else if (info.device)
          {
            DeviceFloat4(info.device, device_offset, reg, c);
          }
          float f[4];
          std::memcpy(f, c, 16);
          out += fmt::format("{}[{},{},{},{}]", reg ? "," : "", std::isfinite(f[0]) ? f[0] : 0.0f,
                             std::isfinite(f[1]) ? f[1] : 0.0f, std::isfinite(f[2]) ? f[2] : 0.0f,
                             std::isfinite(f[3]) ? f[3] : 0.0f);
        }
        return out;
      };
      std::string json = fmt::format(
          "{{\"frame\":{},\"sequence\":{},\"kind\":\"{}\",\"caller\":{},\"prim\":{},\"guest_count\":{},\"host_count\":{},"
          "\"vs\":\"{:016x}\",\"ps\":\"{:016x}\",\"vs_cb_reused\":{},\"ps_cb_reused\":{},\"literals\":{},",
          r.frame, r.sequence, KindName(r.kind), r.caller, info.prim, info.guest_count, info.host_count, info.vs->hash,
          info.ps->hash, info.vs_cb_reused ? "true" : "false", info.ps_cb_reused ? "true" : "false",
          info.literals ? "true" : "false");
      if (rs)
      {
        json += fmt::format("\"blendcontrol\":[{},{},{},{}],\"color_mask\":{},\"depthcontrol\":{},\"colorcontrol\":{},"
                            "\"su_sc_mode_cntl\":{},\"clip_cntl\":{},\"alpha_func\":{},\"alpha_ref\":{},",
                            rs->blendcontrol[0], rs->blendcontrol[1], rs->blendcontrol[2], rs->blendcontrol[3],
                            rs->color_mask, rs->depthcontrol, rs->colorcontrol, rs->su_sc_mode_cntl, rs->clip_cntl,
                            info.alpha_func, info.alpha_ref);
      }
      if (hv)
      {
        json += fmt::format("\"viewport\":[{},{},{},{},{},{}],\"scissor\":[{},{},{},{}],\"ndc_scale\":[{},{},{}],"
                            "\"ndc_offset\":[{},{},{}],\"vte_flags\":{},",
                            hv->viewport.TopLeftX, hv->viewport.TopLeftY, hv->viewport.Width, hv->viewport.Height,
                            hv->viewport.MinDepth, hv->viewport.MaxDepth, info.scissor.left, info.scissor.top,
                            info.scissor.right, info.scissor.bottom, hv->ndc_scale[0], hv->ndc_scale[1],
                            hv->ndc_scale[2], hv->ndc_offset[0], hv->ndc_offset[1], hv->ndc_offset[2], hv->vte_flags);
      }
      json += fmt::format("\"ps_constants\":[{}],\"vs_constants\":[{}],\"textures\":[{}],\"samplers\":[{}],"
                          "\"vertex_stride\":{},\"vertex_dwords_be\":[{}],\"files\":[{}]}}\n",
                          consts_json(info.ps_cb, shadow::dev::kPsAluConstants, kPsFloatCount),
                          consts_json(info.vs_cb, shadow::dev::kVsAluConstants, 256), tex_json, sampler_json,
                          info.user_data ? info.stride : 0u, vertex_json, g_picked_json_files);
      WriteBytes(Stem(r.frame, r.sequence) + "_draw.json", json.data(), json.size());
      Line(fmt::format("  picked draw #{}: {} file(s) in {} ({}_*)", r.sequence, g.files, g.dir.string(),
                       Stem(r.frame, r.sequence)));
    }
  }

  void OnSwap(uint8_t *base)
  {
    if (!g.frame_active && !g.pending)
    {
      return;
    }
    EndFrame(base, "swap");
  }
} // namespace pdz::native::drawdump
