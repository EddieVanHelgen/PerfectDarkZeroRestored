// Shadow-mode recorder: ring storage, the hot-path snapshot, and the cold
// shader / vertex-declaration tables. See shadow_record.h for the layout and
// README.md for the cost estimate.
#include "shadow_record.h"
#include "shadow.h" // pdz_render_shadow_vs_ucode_erase
#include "frame_timing.h"
#include "native/native_view.h" // pdz_native_record_thread: the hook's copies of shader object fields

#include <unordered_map>

#include <rex/logging.h>
#include <rex/hash.h> // xxhash.h with XXH_INLINE_ALL (XXH3_64bits), the SDK's shader key

REXCVAR_DEFINE_INT32(pdz_native_record_ring, 1024, "Render",
                     "Native record thread (31 Oct, docs/INTERCEPTION_SPEEDUP.md): shadow-ring slots in use, rounded "
                     "up to a power of two from 256 to 4096 (0 = 4096, the size before 31 Oct). The render thread "
                     "runs at most that many draws ahead of the record thread; 1024 records (2 MB) stay in cache "
                     "between the two (Outpost: 117.6-118.7 -> 119.9 swaps/s). Nothing drawn changes. Read at "
                     "startup")
    .range(0, 4096)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace pdz::shadow
{
  namespace
  {
    // ~7.5 MB of static storage (ring 4096 x sizeof(DrawRecord) + tables).
    State g_state{};
    // Kept vertex microcode copies dropped by pdz_render_shadow_vs_ucode_erase (memory log).
    uint64_t g_vs_ucode_erased = 0;

    inline uint32_t Mix(uint32_t key)
    {
      // Objects are 128-byte aligned allocations; fold the pointer so that
      // neighbouring objects do not collide in the open-addressing tables.
      uint32_t h = key >> 4;
      h ^= h >> 16;
      h *= 0x7FEB352Du;
      h ^= h >> 15;
      h *= 0x846CA68Bu;
      h ^= h >> 16;
      return h;
    }

    constexpr uint32_t kMaxProbes = 64; // keeps every lookup O(1)
  } // namespace

  State &Get() { return g_state; }

  void LatchRingSize()
  {
    const int32_t want = REXCVAR_GET(pdz_native_record_ring);
    uint32_t slots = kRingSize;
    if (want > 0)
    {
      slots = 256;
      while (slots < uint32_t(want) && slots < kRingSize)
      {
        slots <<= 1;
      }
    }
    g_ring_mask = slots - 1;
    if (slots != kRingSize)
    {
      REXLOG_INFO("pdz_native_record_ring: {} of {} shadow-ring slots in use ({:.1f} MB)", slots, kRingSize,
                  double(slots) * sizeof(DrawRecord) / 1048576.0);
    }
  }

  const char *HookName(Hook h)
  {
    switch (h)
    {
    case Hook::kDrawIndexedVertices: return "DrawIndexedVertices";
    case Hook::kDrawVertices: return "DrawVertices";
    case Hook::kDrawVerticesUP: return "DrawVerticesUP";
    case Hook::kBeginVertices: return "BeginVertices";
    case Hook::kEndVertices: return "EndVertices";
    case Hook::kResolve: return "Resolve";
    case Hook::kSwap: return "Swap";
    case Hook::kSetTexture: return "SetTexture";
    case Hook::kSetRenderTarget: return "SetRenderTarget";
    case Hook::kSetDepthStencilSurface: return "SetDepthStencilSurface";
    case Hook::kSetVertexShader: return "SetVertexShader";
    case Hook::kSetPixelShader: return "SetPixelShader";
    case Hook::kSetVertexDeclaration: return "SetVertexDeclaration";
    case Hook::kSetViewport: return "SetViewport";
    case Hook::kSetScissorRect: return "SetScissorRect";
    case Hook::kClear: return "Clear";
    case Hook::kSetStreamSource: return "SetStreamSource";
    case Hook::kSetIndices: return "SetIndices";
    case Hook::kCreateVertexDeclaration: return "CreateVertexDeclaration";
    case Hook::kCreatePixelShader: return "CreatePixelShader";
    case Hook::kCreateVertexShader: return "CreateVertexShader";
    case Hook::kDrawIndexedVerticesUP: return "DrawIndexedVerticesUP";
    default: return "?";
    }
  }

  // ---------------------------------------------------------------------------
  // Hot path
  // ---------------------------------------------------------------------------
  DrawRecord &Record(uint8_t *base, uint32_t device, Kind kind, uint32_t caller,
                     uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t f0_bits)
  {
    const pdz::timing::Scope timing(pdz::timing::Bucket::kShadow); // pdz_frame_timing
    // pdz_frame_timing_phases (drawcost): shadow.record, outside native (the hooks call Record under the render mutex).
    const pdz::timing::PhaseScope phase(pdz::timing::Phase::kShadowRecord);
    State &s = g_state;
    DrawRecord &r = s.ring[s.head & RingMask()];
    r.sequence = s.head++;
    r.frame = s.frame;
    r.kind = kind;
    r.caller = caller;
    r.a[0] = a0;
    r.a[1] = a1;
    r.a[2] = a2;
    r.a[3] = a3;
    r.a[4] = a4;
    r.a[5] = 0; // a5..a7: set by the DrawIndexedVerticesUP hook after Record
    r.a[6] = 0;
    r.a[7] = 0;
    r.f0_bits = f0_bits;
    r.device = device;

    if (!IsGuestPointer(device))
    {
      // Create* paths never come here; a null device would mean the hook
      // register map is wrong. Leave the snapshot empty but keep the record.
      r.tiling_flag = 0xFF;
      return r;
    }

    const uint8_t *dev_ptr = GuestPtr(base, device);
    r.tiling_flag = (dev_ptr[dev::kFlagsByte] >> 2) & 1;

    r.vertex_shader = Load32(base, device + dev::kVertexShader);
    r.pixel_shader = Load32(base, device + dev::kPixelShader);
    const ShaderEntry *vs = r.vertex_shader ? FindShader(r.vertex_shader) : nullptr;
    const ShaderEntry *ps = r.pixel_shader ? FindShader(r.pixel_shader) : nullptr;
    r.vs_hash = vs ? vs->hash : 0;
    r.ps_hash = ps ? ps->hash : 0;
    r.vertex_decl = Load32(base, device + dev::kVertexDeclaration);
    r.index_buffer = Load32(base, device + dev::kIndexBuffer);

    for (uint32_t i = 0; i < dev::kStreamCount; ++i)
    {
      Stream &st = r.streams[i];
      const uint32_t slot = device + dev::kStreams + 8 * i;
      st.offset = Load32(base, slot);
      st.vb = Load32(base, slot + 4);
      st.stride = s.stream_stride[i];
      if (IsGuestPointer(st.vb))
      {
        st.base = Load32(base, st.vb + dev::kVbBase);
        st.size = Load32(base, st.vb + dev::kVbSize);
      }
      else
      {
        st.base = 0;
        st.size = 0;
      }
    }
    for (uint32_t i = 0; i < dev::kTextureCount; ++i)
    {
      r.textures[i] = Load32(base, device + dev::kTextures + 4 * i);
    }
    for (uint32_t i = 0; i < 4; ++i)
    {
      r.render_targets[i] = Load32(base, device + dev::kRenderTargets + 4 * i);
    }
    r.depth_stencil = Load32(base, device + dev::kDepthStencil);
    for (uint32_t i = 0; i < 6; ++i)
    {
      r.viewport[i] = Load32(base, device + dev::kViewport + 4 * i);
    }
    for (uint32_t i = 0; i < 4; ++i)
    {
      r.scissor[i] = Load32(base, device + dev::kScissorRect + 4 * i);
    }
    r.window_scissor[0] = Load32(base, device + dev::kWindowScissorTL);
    r.window_scissor[1] = Load32(base, device + dev::kWindowScissorBR);
    for (uint32_t i = 0; i < dev::kDirtyMaskCount; ++i)
    {
      r.dirty[i] = Load64(base, device + dev::kDirtyMasks + 8 * i);
    }

    // Raw copies (big-endian as in guest memory; decoded in the cold path).
    std::memcpy(r.regs_be, dev_ptr + dev::kRegShadowBegin, dev::kRegShadowBytes);
    std::memcpy(r.fetch_be, dev_ptr + dev::kFetchConstants, sizeof(r.fetch_be));
    std::memcpy(r.bools_be, dev_ptr + dev::kBoolConstants, sizeof(r.bools_be));
    std::memcpy(r.clip_planes_be, dev_ptr + dev::kClipPlanes, sizeof(r.clip_planes_be));
    return r;
  }

  // ---------------------------------------------------------------------------
  // Tables (cold inserts, O(1) lookups)
  // ---------------------------------------------------------------------------
  const ShaderEntry *FindShader(uint32_t object)
  {
    const State &s = g_state;
    uint32_t i = Mix(object) & (kShaderTableSize - 1);
    for (uint32_t probe = 0; probe < kMaxProbes; ++probe)
    {
      const ShaderEntry &e = s.shaders[i];
      if (e.object == object)
      {
        return &e;
      }
      if (e.object == 0)
      {
        return nullptr;
      }
      i = (i + 1) & (kShaderTableSize - 1);
    }
    return nullptr;
  }

  const DeclEntry *FindDeclaration(uint32_t object)
  {
    const State &s = g_state;
    uint32_t i = Mix(object) & (kDeclTableSize - 1);
    for (uint32_t probe = 0; probe < kMaxProbes; ++probe)
    {
      const DeclEntry &e = s.decls[i];
      if (e.object == object)
      {
        return &e;
      }
      if (e.object == 0)
      {
        return nullptr;
      }
      i = (i + 1) & (kDeclTableSize - 1);
    }
    return nullptr;
  }

  std::unordered_map<uint32_t, std::vector<uint8_t>> &VertexUcode()
  {
    static std::unordered_map<uint32_t, std::vector<uint8_t>> store;
    return store;
  }

  std::unordered_map<uint32_t, std::vector<uint8_t>> &PixelUcode()
  {
    static std::unordered_map<uint32_t, std::vector<uint8_t>> store;
    return store;
  }

  const std::vector<uint8_t> *FindPixelUcode(uint32_t object)
  {
    auto &store = PixelUcode();
    auto it = store.find(object);
    return it == store.end() ? nullptr : &it->second;
  }

  const std::vector<uint8_t> *FindVertexUcode(uint32_t object)
  {
    auto &store = VertexUcode();
    auto it = store.find(object);
    return it == store.end() ? nullptr : &it->second;
  }

  VertexUcodeStats GetVertexUcodeStats()
  {
    VertexUcodeStats s;
    for (const auto &entry : VertexUcode())
    {
      ++s.entries;
      s.bytes += entry.second.size();
    }
    s.erased = g_vs_ucode_erased;
    return s;
  }

  namespace
  {
    // `function` = the container header (magic, header size, microcode size, program info). `ucode_override`:
    // 0 = the microcode follows the header (a real container, Create* hooks); else the object's own microcode
    // copy (FindOrReadShader, where `function` is the header copy inside the D3D object).
    ShaderEntry *NoteShaderImpl(uint8_t *base, uint32_t object, uint32_t function, uint32_t type, uint32_t ucode_override);
  }

  void NoteShader(uint8_t *base, uint32_t object, uint32_t function, uint32_t type)
  {
    NoteShaderImpl(base, object, function, type, 0);
  }

  const ShaderEntry *FindOrReadShader(uint8_t *base, uint32_t object, bool vertex)
  {
    if (!IsGuestPointer(object))
    {
      return nullptr; // (no entry has such an object: NoteShaderImpl refuses them)
    }
    const ShaderEntry *found = FindShader(object);
    // pdz_native_record_thread: the object's fields as the hook copied them (native/native_view.h); guest memory
    // otherwise.
    const uint32_t object_ucode = pdz::native::view::Load32(base, object + (vertex ? 40u : 12u));
    const uint32_t object_bytes = pdz::native::view::Load32(base, object + (vertex ? 600u : 60u));
    if (found && object_ucode == found->object_ucode && object_bytes == found->object_bytes)
    {
      return found;
    }
    if (found && !found->from_object)
    {
      // A Create-hook entry whose object no longer holds the microcode pointer / size it had after Create: the
      // address was freed and reused (possibly by an object built without the hook), so the entry and its hash are
      // stale; read the object below like any unhooked one (review 15). Logged, as it should be rare.
      static uint32_t logged = 0;
      if (logged++ < 4)
      {
        REXLOG_INFO("shadow: hooked {} shader object {:08X} changed since its Create (microcode {:08X} -> {:08X}, "
                    "{} -> {} bytes); re-reading it from the object",
                    vertex ? "vertex" : "pixel", object, found->object_ucode, object_ucode, found->object_bytes,
                    object_bytes);
      }
    }
    // Shader objects that never passed the hooked Create* (built elsewhere, e.g. package resources): the D3D
    // object itself carries a copy of the container header (D3DPixelShader +52, D3DVertexShader +592; sub_826E3D60 /
    // sub_826E4108) and a pointer to its microcode copy (+12 / +40, the address the state flush IM_LOADs; the
    // shadow assert checks exactly these fields on every shader load).
    const uint32_t header = object + (vertex ? 592u : 52u);
    const uint32_t magic = Load32(base, header);
    const uint32_t header_bytes = Load32(base, header + 4);
    const uint32_t bytes = Load32(base, header + 8);
    const uint32_t ucode = pdz::native::view::Load32(base, object + (vertex ? 40u : 12u));
    if (magic != (vertex ? 0x102A0E01u : 0x102A0E00u) || header_bytes < 24 || header_bytes >= 16384 || bytes == 0 ||
        bytes > 64 * 1024 || (bytes & 3) || !IsGuestPointer(ucode))
    {
      return nullptr;
    }
    ShaderEntry *e = NoteShaderImpl(base, object, header, vertex ? 1 : 0, ucode);
    if (e && g_state.shaders_read_from_object++ < 4)
    {
      REXLOG_INFO("shadow: {} shader object {:08X} was not created through the hooked Create call; read from the "
                  "object (header +{}, microcode {:08X}, {} bytes, hash {:016x}, {} vfetch table entries)",
                  vertex ? "vertex" : "pixel", object, vertex ? 592 : 52, ucode, bytes, e->hash, e->vs_table_count);
    }
    return e;
  }

  namespace
  {
  ShaderEntry *NoteShaderImpl(uint8_t *base, uint32_t object, uint32_t function, uint32_t type, uint32_t ucode_override)
  {
    State &s = g_state;
    if (!IsGuestPointer(object) || !IsGuestPointer(function))
    {
      return nullptr;
    }
    uint32_t i = Mix(object) & (kShaderTableSize - 1);
    ShaderEntry *slot = nullptr;
    ShaderEntry *oldest = nullptr;
    for (uint32_t probe = 0; probe < kMaxProbes; ++probe)
    {
      ShaderEntry &e = s.shaders[i];
      if (e.object == object || e.object == 0)
      {
        slot = &e; // a reused pointer (object freed and re-created) overwrites
        break;
      }
      if (!oldest || e.created_frame < oldest->created_frame)
      {
        oldest = &e;
      }
      i = (i + 1) & (kShaderTableSize - 1);
    }
    if (!slot)
    {
      // Full probe window: overwrite its oldest entry in place (never a hole, so every chain stays intact). The
      // oldest object is the likeliest to be freed already; dropping the new one instead failed it for good.
      if (s.shader_table_overflow++ == 0)
      {
        REXLOG_INFO("shadow: shader table probe window full ({} entries); evicting the oldest entry from now on",
                    s.shader_count);
      }
      slot = oldest;
    }
    const bool erase_kept = REXCVAR_GET(pdz_render_shadow_vs_ucode_erase);
    if (slot->object == 0)
    {
      ++s.shader_count;
    }
    else if (slot->object != object && erase_kept)
    {
      // The probe window's oldest entry is overwritten by another object: the evicted object's kept microcode is never
      // looked up through the table again (it used to stay for the session).
      g_vs_ucode_erased += VertexUcode().erase(slot->object);
      PixelUcode().erase(slot->object);
    }
    // XDK native shader container (PART3_SHADERS.md 4.1): +0 magic 0x102A0E00
    // (pixel) / 0x102A0E01 (vertex), +4 header block size, +8 microcode size
    // in bytes; the microcode follows the header block. Hashed exactly as the
    // SDK's PipelineCache::LoadShader does: XXH3_64bits over the guest-order
    // dwords, so the value matches the .xsh / pipeline-cache keys.
    slot->object = object;
    slot->magic = Load32(base, function);
    const uint32_t header_bytes = Load32(base, function + 4);
    slot->ucode_bytes = Load32(base, function + 8);
    slot->ucode_src = ucode_override ? ucode_override : function + header_bytes;
    slot->type = type;
    slot->created_frame = s.frame;
    slot->vs_table_count = 0;
    slot->from_object = ucode_override ? 1u : 0u;
    // Create (sub_826E3D60 / sub_826E4108, hooked after the call) has set both: +12 / +40 = D3D's microcode copy,
    // +60 / +600 = the size in its header copy.
    slot->object_ucode = Load32(base, object + (type == 1 ? 40u : 12u));
    slot->object_bytes = Load32(base, object + (type == 1 ? 600u : 60u));
    if (ucode_override)
    {
      // D3D patches a vertex object's copy in place when it binds it; nothing unbound to keep here.
      VertexUcode().erase(object);
    }
    else if (slot->magic == 0x102A0E01u && slot->ucode_bytes && slot->ucode_bytes <= 64 * 1024 && IsGuestPointer(function + header_bytes))
    {
      const uint8_t *src = GuestPtr(base, function + header_bytes);
      VertexUcode()[object].assign(src, src + slot->ucode_bytes);
    }
    else if (erase_kept)
    {
      // Not a vertex shader we keep (a pixel shader created at a freed vertex shader's address, or an unusable
      // container): a copy kept for the address belongs to the earlier object.
      g_vs_ucode_erased += VertexUcode().erase(object);
    }
    // A pixel shader's microcode at create time (FindPixelUcode).
    if (!ucode_override && slot->magic == 0x102A0E00u && slot->ucode_bytes && slot->ucode_bytes <= 64 * 1024 &&
        IsGuestPointer(function + header_bytes))
    {
      const uint8_t *src = GuestPtr(base, function + header_bytes);
      PixelUcode()[object].assign(src, src + slot->ucode_bytes);
    }
    else
    {
      PixelUcode().erase(object);
    }
    if (slot->magic == 0x102A0E01u && header_bytes >= 24 && header_bytes < 16384)
    {
      const uint32_t program = Load32(base, function + 0x14);
      if (program >= 24 && program + 12 * 4 <= header_bytes)
      {
        const uint32_t count = Load32(base, function + program + 7 * 4);
        for (uint32_t e = 0; e < count && e < 24; ++e)
        {
          const uint32_t at = program + (11 + e) * 4;
          if (at + 4 > header_bytes)
          {
            break;
          }
          slot->vs_table[slot->vs_table_count++] = Load32(base, function + at);
        }
      }
    }
    if (slot->ucode_bytes != 0 && slot->ucode_bytes < (1u << 20) && IsGuestPointer(slot->ucode_src))
    {
      slot->hash = XXH3_64bits(GuestPtr(base, slot->ucode_src), slot->ucode_bytes);
    }
    else
    {
      slot->hash = 0;
    }
    return slot;
  }
  } // namespace

  void NoteDeclaration(uint8_t *base, uint32_t object, uint32_t elements)
  {
    State &s = g_state;
    if (!IsGuestPointer(object) || !IsGuestPointer(elements))
    {
      return;
    }
    uint32_t i = Mix(object) & (kDeclTableSize - 1);
    DeclEntry *slot = nullptr;
    DeclEntry *oldest = nullptr;
    for (uint32_t probe = 0; probe < kMaxProbes; ++probe)
    {
      DeclEntry &e = s.decls[i];
      if (e.object == object || e.object == 0)
      {
        slot = &e;
        break;
      }
      if (!oldest || e.created_frame < oldest->created_frame)
      {
        oldest = &e;
      }
      i = (i + 1) & (kDeclTableSize - 1);
    }
    if (!slot)
    {
      // Full probe window: overwrite the oldest entry in place. An evicted declaration that is still alive is
      // read back from its guest object on its next bind (native_shaders.cpp), so nothing is lost for good.
      if (s.decl_table_overflow++ == 0)
      {
        REXLOG_INFO("shadow: declaration table probe window full ({} entries); evicting the oldest entry from now on",
                    s.decl_count);
      }
      slot = oldest;
    }
    if (slot->object == 0)
    {
      ++s.decl_count;
    }
    slot->object = object;
    slot->source = elements;
    slot->created_frame = s.frame;
    // The D3D object keeps its own copy: +8 count, +12 max stream, +36 table.
    slot->count = Load32(base, object + dev::kDeclCount);
    slot->max_stream = Load32(base, object + dev::kDeclMaxStream);
    const uint32_t n = slot->count < 16 ? slot->count : 16;
    for (uint32_t e = 0; e < n; ++e)
    {
      const uint32_t at = object + dev::kDeclElements + 12 * e;
      // Keep the raw big-endian words so the dump can show the exact bytes.
      std::memcpy(slot->elements_be[e], GuestPtr(base, at), 12);
    }
    for (uint32_t e = n; e < 16; ++e)
    {
      slot->elements_be[e][0] = slot->elements_be[e][1] = slot->elements_be[e][2] = 0;
    }
  }
} // namespace pdz::shadow
