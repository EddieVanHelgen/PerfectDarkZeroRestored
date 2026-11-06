// Native renderer (stage 1): per-pass dump of one frame (native_passdump.h).
//
// Native side: at every step (render-target change with draws since the last step, before a clear, around a
// resolve, and at the swap) the host surfaces involved are copied to a readback buffer (one mid-frame Flush per
// surface: one frame only) and written raw, rows at the footprint pitch, with one JSON line per file in
// native.jsonl (step, event, draws since the previous step, surface, DXGI format, pitch). Depth surfaces are the
// depth plane (D32 float or D24 in the low 24 bits).
//
// Emulated side: the SDK reads back every resolve the GPU processes in the same frame (resolve_readback::ArmDump,
// SDK patch sdk-05) and calls EmuObserver on the GPU thread with the 1x guest bytes it wrote; those go to
// emu<n>_<address>.bin plus emulated.jsonl. The emulator's EDRAM between resolves is not visible, so the emulated
// reference exists at resolves only; tools\passdump.py pairs the n-th emulated resolve with the n-th native one.
#include "native_passdump.h"

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/graphics/resolve_readback.h>
#include <rex/logging.h>

#include "native.h"
#include "native_device.h"
#include "native_recthread.h"

REXCVAR_DEFINE_INT32(pdz_native_pass_dump, 0, "Render",
                     "Native renderer: dump every pass of the frame of this auto-diff capture number (native host "
                     "surfaces and the emulator's resolves) to pdz_native_pass_dump_dir (0 = off).");
REXCVAR_DEFINE_STRING(pdz_native_pass_dump_dir, "", "Render", "Native renderer: output folder of the pass dump.");
REXCVAR_DEFINE_INT32(pdz_native_pass_dump_draws_from, 0, "Render",
                     "Native renderer: in the pass-dump frame, also dump the bound targets after every draw from this "
                     "frame-local draw number on (1 = the first draw; 0 = off).");

namespace pdz::native::passdump
{
  namespace
  {
    struct State
    {
      bool active = false;
      std::filesystem::path dir;
      FILE *index = nullptr;
      uint32_t step = 0;
      uint32_t files = 0;
      Surfaces::Bound pending; // targets drawn since the last step
      bool have_pending = false;
      uint32_t pending_draws = 0;
      uint32_t total_draws = 0;
      uint32_t resolves = 0;
      std::vector<const HostSurface *> fresh; // dumped since the last draw / clear (no need to dump again)
      ID3D12Resource *readback = nullptr;
      uint64_t readback_size = 0;
    };
    State g;

    // Emulated side: written on the GPU thread.
    std::mutex g_emu_mutex;
    std::filesystem::path g_emu_dir;
    FILE *g_emu_index = nullptr;
    uint32_t g_emu_count = 0;

    // Per-draw records (native_draws.bin / emulated_draws.bin): u32 index, u32 prim, u32 count, u32 pad,
    // u64 vs hash, then 256 u32 (VS c0..c63, host order).
    FILE *g_emu_draws = nullptr;
    uint32_t g_emu_draw_count = 0;
    FILE *g_native_draws = nullptr;

    void WriteDrawRecord(FILE *f, uint32_t index, uint32_t prim, uint32_t count, uint64_t vs_hash, const uint32_t *c,
                         uint32_t extra = 0)
    {
      const uint32_t head[4] = {index, prim, count, extra};
      std::fwrite(head, sizeof(head), 1, f);
      std::fwrite(&vs_hash, sizeof(vs_hash), 1, f);
      std::fwrite(c, sizeof(uint32_t), 256, f);
    }

    void EmuDrawObserver(uint32_t prim, uint32_t count, uint64_t vs_hash, const uint32_t *constants)
    {
      std::lock_guard<std::mutex> lock(g_emu_mutex);
      if (g_emu_draws)
      {
        WriteDrawRecord(g_emu_draws, g_emu_draw_count++, prim, count, vs_hash, constants,
                        rex::graphics::resolve_readback::CurrentIndirect());
        std::fflush(g_emu_draws);
      }
    }

    void EmuObserver(uint32_t address, uint32_t length, const uint8_t *data, uint32_t dest_info)
    {
      std::lock_guard<std::mutex> lock(g_emu_mutex);
      if (!g_emu_index)
      {
        return;
      }
      const uint32_t n = g_emu_count++;
      const std::string name = fmt::format("emu{:03}_{:08X}.bin", n, address);
      if (FILE *f = std::fopen((g_emu_dir / name).string().c_str(), "wb"))
      {
        std::fwrite(data, 1, length, f);
        std::fclose(f);
      }
      const std::string line =
          fmt::format("{{\"emu_index\":{},\"address\":{},\"length\":{},\"dest_info\":{},\"file\":\"{}\"}}\n", n,
                      address, length, dest_info, name);
      std::fputs(line.c_str(), g_emu_index);
      std::fflush(g_emu_index);
    }

    bool SameTargets(const Surfaces::Bound &a, const Surfaces::Bound &b)
    {
      return a.rt[0] == b.rt[0] && a.rt[1] == b.rt[1] && a.rt[2] == b.rt[2] && a.rt[3] == b.rt[3] && a.ds == b.ds;
    }

    bool IsFresh(const HostSurface *s)
    {
      for (const HostSurface *f : g.fresh)
      {
        if (f == s)
        {
          return true;
        }
      }
      return false;
    }

    // Copies subresource 0 (the depth plane of a depth-stencil) to the readback buffer, waits, and returns the
    // rows at the footprint pitch.
    bool ReadResource(NativeDevice &dev, ID3D12Resource *res, D3D12_RESOURCE_STATES &state, std::vector<uint8_t> &out,
                      D3D12_PLACED_SUBRESOURCE_FOOTPRINT &fp, UINT &rows)
    {
      D3D12_RESOURCE_DESC desc = res->GetDesc();
      UINT64 row_size = 0, total = 0;
      dev.device()->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &row_size, &total);
      if (!total)
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
                                      D3D12_RESOURCE_FLAG_NONE, "pdz_native pass dump readback");
        g.readback_size = g.readback ? total : 0;
        if (!g.readback)
        {
          return false;
        }
      }
      dev.Transition(res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION dst{};
      dst.pResource = g.readback;
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint = fp;
      D3D12_TEXTURE_COPY_LOCATION src{};
      src.pResource = res;
      src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      src.SubresourceIndex = 0;
      dev.list()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      dev.Flush();
      uint8_t *mapped = nullptr;
      D3D12_RANGE range{0, SIZE_T(total)};
      if (FAILED(g.readback->Map(0, &range, reinterpret_cast<void **>(&mapped))) || !mapped)
      {
        return false;
      }
      const size_t bytes = size_t(fp.Footprint.RowPitch) * rows;
      out.assign(mapped + fp.Offset, mapped + fp.Offset + bytes);
      D3D12_RANGE none{0, 0};
      g.readback->Unmap(0, &none);
      return true;
    }

    bool WriteFile(const std::string &name, const std::vector<uint8_t> &data)
    {
      FILE *f = std::fopen((g.dir / name).string().c_str(), "wb");
      if (!f)
      {
        return false;
      }
      std::fwrite(data.data(), 1, data.size(), f);
      std::fclose(f);
      ++g.files;
      return true;
    }

    void IndexLine(const std::string &line)
    {
      if (g.index)
      {
        std::fputs(line.c_str(), g.index);
        std::fputc('\n', g.index);
      }
    }

    void DumpSurface(HostSurface &s, const char *event, const char *what, uint32_t draws)
    {
      NativeDevice *dev = NativeDevice::Get();
      if (!dev || !dev->in_frame() || !s.resource)
      {
        return;
      }
      std::vector<uint8_t> data;
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
      UINT rows = 0;
      if (!ReadResource(*dev, s.resource, s.state, data, fp, rows))
      {
        return;
      }
      const std::string name = fmt::format("s{:04}_{}_{}_{:08X}.bin", g.step, event, what, s.guest);
      WriteFile(name, data);
      IndexLine(fmt::format(
          "{{\"step\":{},\"event\":\"{}\",\"what\":\"{}\",\"draws\":{},\"guest\":\"{:08X}\",\"width\":{},\"height\":{},"
          "\"host_width\":{},\"host_height\":{},\"dxgi\":{},\"resource_dxgi\":{},\"row_pitch\":{},\"rows\":{},"
          "\"depth\":{},\"depth_format\":{},\"color_format\":{},\"edram\":{},\"file\":\"{}\"}}",
          g.step, event, what, draws, s.guest, s.width, s.height, s.host_width, s.host_height,
          // resource_dxgi: the typed RTV / DSV format (45 / 20 for depth), as before the depth resources became
          // TYPELESS; the footprint (dxgi, row_pitch) is the same for both, it follows the format family.
          uint32_t(fp.Footprint.Format), uint32_t(s.view_format), fp.Footprint.RowPitch, rows,
          s.depth ? "true" : "false", s.depth_format, s.color_format, s.edram_base, name));
      g.fresh.push_back(&s);
    }

    void DumpPending(const char *event)
    {
      if (!g.have_pending || !g.pending_draws)
      {
        return;
      }
      static const char *const kRt[4] = {"rt0", "rt1", "rt2", "rt3"};
      for (uint32_t i = 0; i < 4; ++i)
      {
        if (g.pending.rt[i])
        {
          DumpSurface(*g.pending.rt[i], event, kRt[i], g.pending_draws);
        }
      }
      if (g.pending.ds)
      {
        DumpSurface(*g.pending.ds, event, "ds", g.pending_draws);
      }
      g.pending_draws = 0;
      ++g.step;
    }
  } // namespace

  bool Active() { return g.active; }

  void MaybeBegin(uint64_t swap, uint32_t capture)
  {
    const int32_t chosen = REXCVAR_GET(pdz_native_pass_dump);
    const std::string dir = REXCVAR_GET(pdz_native_pass_dump_dir);
    // graphics_backend null: refused, the emulated side is the plugin's resolve readback (logged once by
    // native_present.cpp; the auto-diff that calls this is refused too).
    // pdz_native_record_thread: refused too (it reads the live device around each draw; logged by native_present.cpp).
    if (g.active || chosen <= 0 || uint32_t(chosen) != capture || dir.empty() || pdz::gpusync::NullBackend() ||
        recthread::Active())
    {
      return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    g = State();
    g.dir = dir;
    g.index = std::fopen((g.dir / "native.jsonl").string().c_str(), "w");
    if (!g.index)
    {
      REXLOG_WARN("pdz_native: pass dump: cannot write {}", (g.dir / "native.jsonl").string());
      return;
    }
    {
      std::lock_guard<std::mutex> lock(g_emu_mutex);
      g_emu_dir = g.dir;
      if (!g_emu_index)
      {
        g_emu_index = std::fopen((g.dir / "emulated.jsonl").string().c_str(), "w");
      }
      if (!g_emu_draws)
      {
        g_emu_draws = std::fopen((g.dir / "emulated_draws.bin").string().c_str(), "wb");
      }
    }
    if (!g_native_draws)
    {
      g_native_draws = std::fopen((g.dir / "native_draws.bin").string().c_str(), "wb");
    }
    rex::graphics::resolve_readback::SetDrawObserver(&EmuDrawObserver);
    rex::graphics::resolve_readback::ArmDump(swap, &EmuObserver);
    g.active = true;
    IndexLine(fmt::format("{{\"event\":\"begin\",\"swap\":{},\"capture\":{}}}", swap, capture));
    REXLOG_INFO("pdz_native: pass dump of the frame of swap {} (auto-diff capture #{}) to {}", swap, capture, dir);
  }

  void End()
  {
    if (!g.active)
    {
      return;
    }
    DumpPending("end");
    IndexLine(fmt::format("{{\"event\":\"end\",\"draws\":{},\"resolves\":{},\"files\":{}}}", g.total_draws,
                          g.resolves, g.files));
    std::fclose(g.index);
    g.index = nullptr;
    g.active = false;
    // The readback buffer (the size of the largest surface dumped: 30 MB for a 64bpp target at scale 2) was kept for
    // the rest of the session (and MaybeBegin's State() reset dropped the pointer without releasing it).
    if (g.readback)
    {
      if (NativeDevice *dev = NativeDevice::Existing())
      {
        dev->ReleaseLater(g.readback);
      }
      g.readback = nullptr;
      g.readback_size = 0;
    }
    REXLOG_INFO("pdz_native: pass dump done: {} steps, {} draws, {} resolves, {} files", g.step, g.total_draws,
                g.resolves, g.files);
  }

  uint64_t ReadbackBytes() { return g.readback ? g.readback_size : 0; }

  void BeforeDraw(uint8_t *base, uint32_t device)
  {
    if (!g.active)
    {
      return;
    }
    const Surfaces::Bound bound = Surfaces::Get().GetBound(base, device);
    if (!g.have_pending || !SameTargets(bound, g.pending))
    {
      DumpPending("targets");
      g.pending = bound;
      g.have_pending = true;
      g.pending_draws = 0;
    }
  }

  void RecordDraw(uint32_t sequence, uint32_t prim, uint32_t count, uint64_t vs_hash, const uint8_t *c_be)
  {
    if (!g.active || !g_native_draws)
    {
      return;
    }
    uint32_t c[256];
    for (uint32_t i = 0; i < 256; ++i)
    {
      c[i] = (uint32_t(c_be[4 * i]) << 24) | (uint32_t(c_be[4 * i + 1]) << 16) | (uint32_t(c_be[4 * i + 2]) << 8) |
             c_be[4 * i + 3];
    }
    WriteDrawRecord(g_native_draws, sequence, prim, count, vs_hash, c);
    std::fflush(g_native_draws);
  }

  void CountDraw()
  {
    if (g.active)
    {
      ++g.pending_draws;
      ++g.total_draws;
      g.fresh.clear();
      const int32_t from = REXCVAR_GET(pdz_native_pass_dump_draws_from);
      if (from > 0 && g.total_draws >= uint32_t(from))
      {
        IndexLine(fmt::format("{{\"step\":{},\"event\":\"draw-marker\",\"draw\":{}}}", g.step, g.total_draws));
        DumpPending("draw");
        g.pending_draws = 0;
      }
    }
  }

  void BeforeClear(const Surfaces::Bound &bound, uint32_t flags, uint32_t color, float z, uint32_t stencil)
  {
    if (!g.active)
    {
      return;
    }
    DumpPending("before-clear");
    IndexLine(fmt::format("{{\"step\":{},\"event\":\"clear\",\"flags\":{},\"color\":\"{:08X}\",\"z\":{},\"stencil\":{},"
                          "\"rt0\":\"{:08X}\",\"ds\":\"{:08X}\"}}",
                          g.step, flags, color, z, stencil, bound.rt[0] ? bound.rt[0]->guest : 0,
                          bound.ds ? bound.ds->guest : 0));
    g.fresh.clear();
  }

  void BeforeResolve(HostSurface &source)
  {
    if (!g.active)
    {
      return;
    }
    DumpPending("before-resolve");
    if (!IsFresh(&source))
    {
      DumpSurface(source, "resolve-src", source.depth ? "ds" : "rt", 0);
      ++g.step;
    }
  }

  void AfterResolve(HostSurface &source, HostTexture &dest, const FetchDecoded &f, uint32_t flags, int32_t dst_x,
                    int32_t dst_y, int32_t width, int32_t height)
  {
    if (!g.active)
    {
      return;
    }
    NativeDevice *dev = NativeDevice::Get();
    std::vector<uint8_t> data;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    if (dev && dev->in_frame() && dest.resource && ReadResource(*dev, dest.resource, dest.state, data, fp, rows))
    {
      const std::string name = fmt::format("s{:04}_resolve{:03}_dest_{:08X}.bin", g.step, g.resolves, f.base_address);
      WriteFile(name, data);
      IndexLine(fmt::format(
          "{{\"step\":{},\"event\":\"resolve\",\"resolve_index\":{},\"flags\":{},\"source\":\"{:08X}\","
          "\"base_address\":{},\"format\":{},\"width\":{},\"height\":{},\"pitch\":{},\"tiled\":{},\"endian\":{},"
          "\"swizzle\":{},\"dst_x\":{},\"dst_y\":{},\"copy_width\":{},\"copy_height\":{},\"dxgi\":{},\"row_pitch\":{},"
          "\"rows\":{},\"host_width\":{},\"host_height\":{},\"scale\":{},\"file\":\"{}\"}}",
          g.step, g.resolves, flags, source.guest, f.base_address, f.format, f.width, f.height, f.pitch,
          f.tiled ? "true" : "false", f.endian, f.swizzle, dst_x, dst_y, width, height, uint32_t(fp.Footprint.Format),
          fp.Footprint.RowPitch, rows, dest.width, dest.height, dest.scale, name));
    }
    ++g.resolves;
    ++g.step;
    if (flags & 0x200)
    {
      g.fresh.clear(); // the source was cleared after the copy
    }
  }
} // namespace pdz::native::passdump
