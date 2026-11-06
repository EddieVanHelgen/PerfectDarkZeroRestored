// Shadow-mode recorder: per-draw ring of plain structs (stage 0, native
// renderer plan, RENDER_FEASIBILITY.md 6.4 step 1 / 5.2 option C).
//
// FILES ONLY: this folder is not part of the PDZ build. Every offset below is
// a Title Update 3 device-struct offset recovered in work\PART2_D3D_USAGE.md
// (sections 3, 4, 10) and verified against the generated code of the flush
// sub_826FA8B8 and its run writers sub_826F9298 / sub_826F9450 / sub_826F9358
// (see shadow_assert.md for the exact bit layout of the dirty masks).
//
// Hot-path contract (every Draw* hook): O(1), no allocation, no clock reads,
// no logging; one fixed-size struct copy (memcpy of ~1.3 KB from the guest
// device struct plus ~30 scalar loads). Single producer: the game's render
// thread is the only caller of the D3D entry points that record.
#pragma once
#include <vector>

#include <cstdint>
#include <cstring>

namespace pdz::shadow
{
  // ---------------------------------------------------------------------------
  // Guest device struct (D3DDevice, 0x5080 bytes, pointer at [0x82E2B64C]).
  // ---------------------------------------------------------------------------
  namespace dev
  {
    constexpr uint32_t kDevicePointerGlobal = 0x82E2B64C;
    constexpr uint32_t kDeviceSize = 0x5080;

    constexpr uint32_t kRingWritePtr = 0;   // +0 command ring write pointer
    constexpr uint32_t kDirtyMasks = 16;    // five u64: +16 VS ALU, +24 PS ALU,
                                            // +32 fetch/bool/0x2000 regs,
                                            // +40 regs 0x2100-0x2294,
                                            // +48 regs 0x2300-0x2387 + streams + shaders
    constexpr uint32_t kDirtyMaskCount = 5;

    constexpr uint32_t kFetchConstants = 1152;     // 32 x 24 bytes (0x4800 + 6*i)
    constexpr uint32_t kFetchConstantCount = 32;
    constexpr uint32_t kFetchConstantBytes = 24;
    // Constant stores, each in register order (the flush emits register R from store + 4 * (R - first)):
    // checked statically (flush sub_826FA8B8, setters, state block capture sub_826ED690) and by the shadow
    // assert in Datacore (logs 045 / 053: 0 value mismatches); shadow_assert.md section 1.3.
    constexpr uint32_t kVsAluConstants = 1920;     // 256 x float4 (0x4000..): VS c n at +1920 + 16 n
    constexpr uint32_t kPsAluConstants = 6016;     // 256 x float4 (0x4400..): PS c n at +6016 + 16 n (hardware 256 + n)
    constexpr uint32_t kBoolConstants = 10112;     // 8 dwords (0x4900..0x4907): bool n = bit (n & 31) of dword n >> 5;
                                                   // VS b0..127 dwords 0..3 (+10112), PS b0..127 = 128..255 (+10128)
    constexpr uint32_t kBoolConstantCount = 8;
    constexpr uint32_t kLoopConstants = 10144;     // 32 dwords (0x4908..0x4927): loop i = +10144 + 4 i;
                                                   // VS i0..15 = 0..15 (+10144), PS i0..15 = 16..31 (+10208)
    constexpr uint32_t kLoopConstantCount = 32;

    constexpr uint32_t kFlagsByte = 10432;         // bit 2 = predicated tiling active
    constexpr uint32_t kVertexDeclaration = 11408; // current D3DVertexDeclaration*
    constexpr uint32_t kRegShadowBegin = 11456;    // 0x2000 run starts here
    constexpr uint32_t kRegShadowEnd = 11952;      // end of the 0x2380 run (+11920 + 8*4)
    constexpr uint32_t kRegShadowBytes = kRegShadowEnd - kRegShadowBegin; // 496
    constexpr uint32_t kRegShadowDwords = kRegShadowBytes / 4;             // 124

    // Register runs inside the shadow (first register, device offset, count).
    constexpr uint32_t kRun2000 = 11456; // 16: RB_SURFACE_INFO .. PA_SC_SCREEN_SCISSOR_BR
    constexpr uint32_t kRun2100 = 11532; // 21: VGT_MAX_VTX_INDX .. PA_CL_VPORT_ZOFFSET
    constexpr uint32_t kRun2180 = 11616; //  5: SQ_PROGRAM_CNTL .. SQ_WRAPPING_1
    constexpr uint32_t kRun2200 = 11636; // 12: RB_DEPTHCONTROL .. RB_BLENDCONTROL3
    constexpr uint32_t kRun2280 = 11684; // 21: PA_SU_POINT_SIZE .. VGT_ENHANCE
    constexpr uint32_t kRun2300 = 11768; // 38: PA_SC_LINE_CNTL .. RB_SAMPLE_COUNT_ADDR
    constexpr uint32_t kRun2380 = 11920; //  8: PA_SU_POLY_OFFSET_FRONT_SCALE .. PA_CL_POINT_CULL_RAD

    // Not part of any dirty-mask run (emitted by sub_826EE1C0 instead):
    // PA_SC_WINDOW_OFFSET (0x2080): the dword before the window scissor. Not in a flush run (sub_826EE1C0 emits
    // 0x2080..0x2082 as its own packet); validated by the shadow assert's window-run check (shadow_assert.cpp).
    constexpr uint32_t kWindowOffset = 11520;
    constexpr uint32_t kWindowScissorTL = 11524;
    constexpr uint32_t kWindowScissorBR = 11528;

    // Key registers (dev offset = run base + 4 * (reg - first)).
    constexpr uint32_t kRB_COLOR_MASK = 11548;       // 0x2104
    constexpr uint32_t kRB_STENCILREFMASK = 11584;   // 0x210D
    constexpr uint32_t kRB_ALPHA_REF = 11588;        // 0x210E
    constexpr uint32_t kRB_DEPTHCONTROL = 11636;     // 0x2200
    constexpr uint32_t kRB_BLENDCONTROL0 = 11640;    // 0x2201
    constexpr uint32_t kRB_COLORCONTROL = 11644;     // 0x2202
    constexpr uint32_t kPA_SU_SC_MODE_CNTL = 11656;  // 0x2205

    constexpr uint32_t kIndexBuffer = 12532;       // current D3DIndexBuffer*
    constexpr uint32_t kRenderTargets = 12536;     // RT[0..3], 4 bytes each
    constexpr uint32_t kDepthStencil = 12552;      // current depth-stencil surface
    constexpr uint32_t kStreams = 12556;           // per stream {u32 offset, u32 vb}
    constexpr uint32_t kStreamCount = 16;          // 12556 + 8*16 = 12684 (< 12688 used by D3D)
    constexpr uint32_t kTextures = 12704;          // texture object per sampler, 4 bytes
    constexpr uint32_t kTextureCount = 26;         // 12704 + 4*26 = 12808 = viewport
    constexpr uint32_t kViewport = 12808;          // X, Y, W, H (u32), MinZ, MaxZ (f32)
    constexpr uint32_t kScissorRect = 12832;       // x1, y1, x2, y2
    constexpr uint32_t kClipPlanes = 12848;        // D3DDevice_SetClipPlane (sub_826F15F8): plane i = 4 floats at
                                                   // +12848 + 16 i, the values it also writes to PA_CL_UCP_i (0x2388 + 4 i)
    constexpr uint32_t kClipPlaneCount = 6;
    constexpr uint32_t kPixelShader = 12944;       // current D3DPixelShader*
    constexpr uint32_t kVertexShader = 20456;      // current D3DVertexShader*

    // D3D vertex buffer object fields used by the stream -> fetch builder
    // (sub_826F9578): +12 base address, +16 size in bytes.
    constexpr uint32_t kVbBase = 12;
    constexpr uint32_t kVbSize = 16;
    // D3D shader object: +4 refcount, +12 microcode copy (physical memory),
    // +52 copy of the container header (parsed by sub_82AD9B70).
    constexpr uint32_t kShaderUcode = 12;
    // D3D vertex declaration object: +8 element count, +12 max stream index,
    // +36 copy of the D3DVERTEXELEMENT9 table (12 bytes per element).
    constexpr uint32_t kDeclCount = 8;
    constexpr uint32_t kDeclMaxStream = 12;
    constexpr uint32_t kDeclElements = 36;
  } // namespace dev

  // ---------------------------------------------------------------------------
  // Ring record
  // ---------------------------------------------------------------------------
  enum class Kind : uint8_t
  {
    kNone = 0,
    kDrawIndexedVertices, // sub_826EA128: a0 prim, a1 base vertex, a2 start index, a3 index count
    kDrawVertices,        // sub_826E9EE8: a0 prim, a1 start vertex, a2 vertex count
    kDrawVerticesUP,      // sub_826E9A60: a0 prim, a1 vertex count, a2 data ptr, a3 stride
    kBeginVertices,       // sub_826E97B0: a0 prim, a1 vertex count, a2 stride, a3 ring data ptr (r3 on return)
    kClear,               // sub_826E0A48: a0 count, a1 rects, a2 flags, a3 colour; f0 = Z bits, a4 = stencil
    kResolve,             // sub_826F21D0: a0 flags, a1 src rect, a2 dest texture, a3 dest point
    kSwap,                // sub_826DF960: a0 front buffer, a1 parameters (frame marker)
    kDrawIndexedVerticesUP, // sub_826E9E18: a0 prim, a1 min vertex index, a2 vertex count, a3 index count, a4 index
                            // data ptr, a5 index format (1 = 32-bit, else 16-bit), a6 vertex data ptr, a7 stride
  };

  struct Stream
  {
    uint32_t vb;       // guest D3DVertexBuffer* (0 = unbound)
    uint32_t offset;   // SetStreamSource OffsetInBytes (device slot +0)
    uint32_t stride;   // SetStreamSource Stride, mirrored by the hook (not in the device slot)
    uint32_t base;     // vb+12 (physical address) when vb != 0
    uint32_t size;     // vb+16 when vb != 0
  };

  struct DrawRecord
  {
    uint32_t sequence;  // global record counter (wraps at 2^32)
    uint32_t frame;     // swap counter at record time
    Kind kind;
    uint8_t tiling_flag; // device+10432 bit 2 (predicated tiling active; never set by PDZ)
    uint16_t reserved;
    uint32_t caller;    // guest return address (ctx.lr) = call site
    uint32_t a[8];      // raw arguments, meaning per Kind (a5..a7: kDrawIndexedVerticesUP only, else 0)
    uint32_t f0_bits;   // f1 argument bits (Clear Z, Resolve Z)
    uint32_t device;    // guest device pointer (r3)

    uint32_t vertex_shader, pixel_shader; // guest object pointers
    uint64_t vs_hash, ps_hash;            // XXH3-64 of the microcode, cached at Create*Shader (0 = unknown)
    uint32_t vertex_decl;
    uint32_t index_buffer;
    Stream streams[dev::kStreamCount];
    uint32_t textures[dev::kTextureCount];
    uint32_t render_targets[4];
    uint32_t depth_stencil;
    uint32_t viewport[6];     // X, Y, W, H, MinZ bits, MaxZ bits
    uint32_t scissor[4];      // SetScissorRect copy at +12832
    uint32_t window_scissor[2]; // +11524 / +11528 (PA_SC_WINDOW_SCISSOR_TL/BR form)

    uint64_t dirty[dev::kDirtyMaskCount];       // +16..+55 as of hook entry (before the flush clears them)
    uint32_t regs_be[dev::kRegShadowDwords];    // raw big-endian copy of +11456..+11952
    uint8_t fetch_be[dev::kFetchConstantCount * dev::kFetchConstantBytes]; // raw copy of +1152..+1920
    uint32_t bools_be[dev::kBoolConstantCount]; // raw copy of +10112..+10144
    uint32_t clip_planes_be[dev::kClipPlaneCount * 4]; // raw copy of +12848..+12944 (PA_CL_UCP_0..5 X/Y/Z/W)
  };

  constexpr uint32_t kRingSize = 4096; // power of two: the storage; the ring in use may be smaller (RingMask)
  static_assert((kRingSize & (kRingSize - 1)) == 0);
  // pdz_native_record_ring (31 Oct, docs/INTERCEPTION_SPEEDUP.md): the slots in use, a power of two <= kRingSize,
  // latched at startup (records are about 2 KB: 4096 slots = 8 MB, 1024 = 2 MB, inside a core's L2). Every index
  // into the ring goes through this mask.
  inline uint32_t g_ring_mask = kRingSize - 1;
  inline uint32_t RingMask() { return g_ring_mask; }
  inline uint32_t RingSlots() { return g_ring_mask + 1; }
  void LatchRingSize();

  // Bytes copied from the guest per record (for the cost note in README.md).
  constexpr uint32_t kBytesCopiedPerRecord =
      sizeof(uint64_t) * dev::kDirtyMaskCount + dev::kRegShadowBytes +
      dev::kFetchConstantCount * dev::kFetchConstantBytes + dev::kBoolConstantCount * 4 + dev::kClipPlaneCount * 16;

  // ---------------------------------------------------------------------------
  // Shader and declaration caches (filled by the Create* hooks, cold path)
  // ---------------------------------------------------------------------------
  struct ShaderEntry
  {
    uint32_t object;      // guest D3DPixelShader* / D3DVertexShader* (key, 0 = empty)
    uint32_t ucode_src;   // pFunction + header size: the microcode in the game's memory
    uint32_t ucode_bytes; // [pFunction + 8]
    uint32_t type;        // 0 = pixel, 1 = vertex (container magic low byte)
    uint64_t hash;        // XXH3-64 over ucode_bytes of guest-order dwords (SDK key)
    uint32_t magic;       // [pFunction + 0]
    uint32_t created_frame;
    // Vertex shaders: the container's vertex element table (program-info word 11.., count = word 7), copied at
    // create time because run-time-compiled containers (video player) live in a buffer reused afterwards.
    // Raw entries: bits 0..11 vfetch address, 12..15 usage, 16..19 usage index, 20..23 marker. Cold path.
    uint32_t vs_table[24];
    uint32_t vs_table_count;
    uint32_t from_object; // 1 = read from the D3D object (FindOrReadShader), not from a hooked Create call
    // The object's own microcode pointer (+40 VS / +12 PS: D3D's copy, not the container) and size (+600 / +60) when
    // the entry was made. FindOrReadShader trusts the entry only while the object still holds both.
    uint32_t object_ucode;
    uint32_t object_bytes;
  };
  constexpr uint32_t kShaderTableSize = 16384; // power of two; entries are never removed (no Release hook), so a full
                                               // probe window evicts its oldest entry (shadow_record.cpp)

  struct DeclEntry
  {
    uint32_t object;     // guest D3DVertexDeclaration* (key)
    uint32_t count;      // elements before the 0xFF terminator
    uint32_t max_stream;
    uint32_t elements_be[16][3]; // raw D3DVERTEXELEMENT9 (12 bytes each), first 16
    uint32_t created_frame;
    uint32_t source;     // the pVertexElements pointer passed in
  };
  constexpr uint32_t kDeclTableSize = 8192; // power of two; log 023: 1024 filled in the attract scene (one declaration
                                            // per model instance, never removed), every new one then failed to bind

  // ---------------------------------------------------------------------------
  // Per-frame counters for the state setters (no ring record needed: the draw
  // snapshot carries their effect).
  // ---------------------------------------------------------------------------
  enum class Hook : uint8_t
  {
    kDrawIndexedVertices, kDrawVertices, kDrawVerticesUP, kBeginVertices, kEndVertices,
    kResolve, kSwap, kSetTexture, kSetRenderTarget, kSetDepthStencilSurface,
    kSetVertexShader, kSetPixelShader, kSetVertexDeclaration, kSetViewport,
    kSetScissorRect, kClear, kSetStreamSource, kSetIndices, kCreateVertexDeclaration,
    kCreatePixelShader, kCreateVertexShader, kDrawIndexedVerticesUP,
    kCount
  };
  const char *HookName(Hook h);

  // ---------------------------------------------------------------------------
  // Recorder state (one instance, static storage)
  // ---------------------------------------------------------------------------
  struct State
  {
    DrawRecord ring[kRingSize];
    uint32_t head;             // next slot = head & (kRingSize - 1)
    uint32_t frame;            // swap counter
    uint32_t stream_stride[dev::kStreamCount];
    uint32_t counters[uint32_t(Hook::kCount)];      // this frame
    uint32_t last_counters[uint32_t(Hook::kCount)]; // previous frame (for the dump)
    ShaderEntry shaders[kShaderTableSize];
    DeclEntry decls[kDeclTableSize];
    uint32_t shader_count;
    uint32_t decl_count;
    uint32_t shader_table_overflow; // inserts that evicted the oldest entry of a full probe window
    uint32_t decl_table_overflow;
    uint32_t shaders_read_from_object; // FindOrReadShader fallbacks
  };
  State &Get();

  // Hot path. Fills a record from the device struct and the arguments and
  // advances the ring. Returns the slot written (BeginVertices patches a[3]).
  DrawRecord &Record(uint8_t *base, uint32_t device, Kind kind, uint32_t caller,
                     uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t f0_bits);

  // Cold path (Create* hooks).
  void NoteShader(uint8_t *base, uint32_t object, uint32_t function, uint32_t type);
  void NoteDeclaration(uint8_t *base, uint32_t object, uint32_t elements);
  const ShaderEntry *FindShader(uint32_t object); // O(1) expected, also used hot
  // FindShader, else reads the shader from the D3D object itself (objects the game built without the hooked
  // Create* call) and records it. nullptr when the object does not look like a D3D shader of that kind.
  const ShaderEntry *FindOrReadShader(uint8_t *base, uint32_t object, bool vertex);
  const DeclEntry *FindDeclaration(uint32_t object);
  // Vertex shaders: the microcode as it was at create time (the container of a run-time-compiled shader is
  // freed / reused after CreateVertexShader). nullptr when not kept. Cold path; not thread-safe against
  // concurrent creates (the game creates shaders on its render thread).
  const std::vector<uint8_t> *FindVertexUcode(uint32_t object);
  // A pixel shader's microcode as it was when the shader was created (2026-11-03: the video player rebuilds its
  // shaders in a reused buffer, so a miss read later from the game's memory got other bytes; log 597).
  const std::vector<uint8_t> *FindPixelUcode(uint32_t object);
  // Memory log: the kept vertex microcode copies (entries, bytes) and those dropped by pdz_render_shadow_vs_ucode_erase
  // (an object whose table entry another object overwrote, or whose address became a pixel shader). Walks the map:
  // call with the native render lock held and only while that cvar is on (the Create hooks then write under the lock).
  struct VertexUcodeStats
  {
    uint64_t entries = 0, bytes = 0, erased = 0;
  };
  VertexUcodeStats GetVertexUcodeStats();

  inline void Count(Hook h) { ++Get().counters[uint32_t(h)]; }

  // ---------------------------------------------------------------------------
  // Guest memory helpers (big-endian; physical window mirrors REX_PHYS_HOST_OFFSET).
  // ---------------------------------------------------------------------------
  inline const uint8_t *GuestPtr(const uint8_t *base, uint32_t address)
  {
    return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
  }
  inline bool IsGuestPointer(uint32_t address)
  {
    return address >= 0x00010000u && address < 0xFFFF0000u;
  }
  inline uint32_t Load32(const uint8_t *base, uint32_t address)
  {
    const uint8_t *p = GuestPtr(base, address);
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }
  inline uint64_t Load64(const uint8_t *base, uint32_t address)
  {
    return (uint64_t(Load32(base, address)) << 32) | Load32(base, address + 4);
  }
  inline uint8_t Load8(const uint8_t *base, uint32_t address) { return *GuestPtr(base, address); }
  inline float BitsToFloat(uint32_t bits)
  {
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
  }
  inline uint32_t Swap32(uint32_t v)
  {
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
  }
} // namespace pdz::shadow
