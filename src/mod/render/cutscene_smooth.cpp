// In-engine cutscenes at the frame rate (pdz_cutscene_smooth, on by default; off = Rare's look). TU3 addresses.
//
// 1. No interpolation. The cutscene's time moves every frame (game_pace.cpp, PdzCutsceneStep: the real-time step and
// the sound sync), but Rare samples every cutscene animation WITHOUT interpolation, so a frame between two keys
// repeats the earlier key's pose. The interpolation mode is the sampler's +164 (library 8271xxxx: 0 = none, key a;
// 1 = linear; 2 = cubic; the library's default is 1, sub_8271CFF0 at 8271D038, and gameplay keeps it). The cutscene
// sets 0 in two places:
//   (a) sub_821EA350, the init of the cutscene's own channel samplers (types 2-7 below), at 821EA3B4
//       (`stw r10,164(r31)`, r10 = 0; play mode +168 = 1, clamp, at 821EA3B0);
//   (b) the characters and other game objects (type 1) play the clip in slot 0 of their own object animation: the
//       block start sub_822214F8 calls sub_821EC2B8(anim object, 0, slot 0) at 82221644, which stores (flag ? 2 : 0)
//       to the slot's sampler +164 (slot +180) at 821EC2F0. Each frame the object's cutscene tick (vtable +320, e.g.
//       sub_8251D558, called at 8221EE78) runs sub_821E8F70 (slots' time and root: sub_821EB730 -> sub_8271D3E8 /
//       sub_8271DDC0, sampler = slot +16) and sub_821E9000 (bones: sub_821EB5A0 -> sub_8271F7B8).
// 2. Block timing. A cutscene is streamed in blocks of 8 frames of 30 fps data (camera clip: 8 keys, [anim+28] =
// 0.267 s = 8 / 30 s; the player's +88 counts blocks, the next is switched in when the first camera's t reaches 1,
// sub_822213E0 -> sub_82221368 / sub_822214F8, which re-inits every channel on the new block's clips). Blocks don't
// share frames. Rare's sampler puts key 0 at t = 0 and key 7 at t = 1 (sub_82720D90), so at the 360's 30 fps frame k
// of a block (t = k / 8) shows key k - 1: every key once. Between frames that mapping runs the content at 7/8 speed
// and skips a frame at every block switch. Here block clips are sampled at key position 8t - 1 - kLag (the 360's
// cadence, half a key later), from the frame shown at the previous block's switch (kept) to key 0 in the block's first
// part. The fraction never leaves [0, 1]: nothing is extrapolated.
//    The lag (log 288's "stops", a slight judder): the switch frame is the one where t reaches 1, clamped
// (sub_8271D3E8 8271D4B0); the time past 1 is lost there and the sound sync gives it back on the next frame
// (PdzCutsceneStep: the picture is behind by it), so with the switch frame showing the last key exactly, one frame
// in about three switches moved under a quarter of a frame and the next one more. The position is taken from t
// before the clamp (t + rate x the step); half a key of lag keeps it inside the block up to 60 frames per second.
// 3. Spaces. A block's root keys are relative to that clip: sub_821EA3F0 adds the root position it sampled (sampler
// +96..104) and the clip's own root offset [[anim+40]+0..8] to the channel's matrix (sampler +12 -> layer +432;
// 821EA4F4-821EA544; the block start sub_82213B38 resets it, 82213BD8-82213C14); its translation (+480) is the
// camera's eye in the world. So the camera keeps and blends its WORLD eye (and the look-at / lens tracks, world /
// scalar values the view uses as they are); objects blend only their bones' pose (local to the skeleton) at a block
// start and keep Rare's root (key 0).
// 4. The camera's curve, pdz_cutscene_camera_curve: catmull (default) = uniform Catmull-Rom on the eye and look-at
// point as 3D vectors and the other tracks as scalars, through Rare's keys, with guards: no control key across a cut,
// one end's tangent no longer than 1.5 x the segment when the neighbouring segment is over 3x or under 1/3 of it;
// monotone = a Fritsch-Butland cubic per component; linear. Rare's curve mode 2 isn't usable: the root only lerps in
// mode 2 (sub_8271DDC0 uses 82720438 for modes 1 and 2) and sub_82720D90 clamps the outer keys at a block's ends.
// 5. Cuts. Never blended across, never a curve through. The first camera's keys are read once per block (all its
// tracks, with Rare's decoder) and every interval judged by what's on screen: the eye's move, the view direction's
// turn (target - eye, the target being the look-at point while PDZ_UseLookAtPoint is on, layer +576 / 822134D8; else
// the root rotation's turn) and the look-at point's move, each against an absolute limit and against the neighbouring
// intervals (3x the larger or 4x the average, above a floor); interval 0's earlier neighbour is the previous block's
// last interval. PDZ_SceneID (layer +600, Rare's scene counter at 82213808) changing with a move also counts. A camera
// cut holds every channel on its key; an object's own root jump holds only that object. A block switch is judged the
// same way between the kept frame and key 0 (camera only).
// 6. The cutscene's end (log 288: the final frame in the next block, block 169 key 1 after 168 key 7, and Datacore's
// 100 key 3 after 99 key 7, the view swinging just before gameplay). The last block is shorter (2 / 4 frames); Rare's
// picture time sub_82221878 = (block +88 + t) x the CURRENT clip's length [anim+28] x 1000, so on the last block's
// first frame the picture drops to 2/8 or 4/8 of its real value, the update takes its "behind" branch (8221EDF4: the
// step is the whole difference, tens of seconds; PdzCutsceneStep's sync does the same through `reach`), t jumps to 1
// and 8221F0CC ends the cutscene: Rare plays the last block as one frame of its last key (the 360 too). Here a block
// that is passed in one frame (t from 0 to 1 at its first sample) keeps the previous block's last shown frame for every
// channel that kept one (the camera's world eye and tracks, objects' poses; flagged "past the end" in the end lines).
//
// The library samples a clip at its normalized time t (sampler +148, 0..1; +160 = rate = 1 / the clip's length;
// sub_8271D3E8 sets t: play mode +168 0 = wrap, 1 = clamp): sub_82720D90(sampler r3, &fraction r4, u32 index[4] r5)
// turns t into keys; the root track (sub_8271DDC0; plain floats, [[anim+40]+36], position flags +28..30, rotation
// +31..33, +35 = quaternion); the bones (sub_8271F7B8 -> the codec's decoder, table 82D1E910 +24, r8 = the mode, mode 0
// reads index[1]) into the pose buffer [sampler +116] (48 bytes a bone, sampler +46 bones); the float tracks
// (sub_8271FAE0). The cutscene (sub_8221ED68 state 3) advances, in this order, with f31 = the step: type 1 (entries
// +80, 112 bytes, type +64, object +72; emitter objects too, vtable +320 825A4918, left alone); type 2 (+28 / +32):
// camera controls (sub_8220C0D8; sub_8220C3F8 builds the view from the top layer's eye +4 / target +16, kept at
// control +948 / +972) with a cutscene layer (vtable 82095EB0, advance 822132B0, sampler = layer +128, tracks at layer
// +500); type 3 lights (tracks +356); type 4 emitters (left alone); types 6 / 7 props. The cues (sub_82221BD8) and the
// block switch read the time, not poses. Lip sync (sub_8268FD80) has its own clock.
//
// The ms clock and the step: PdzCutsceneStep's step is min(real-time step + lead, reach), reach = sound position +
// lead + 66 - the picture's time r27, which sub_82221878 truncates to whole ms: up to +-1 ms on 8.3 ms per step.
// Cost outside cutscenes: sub_821EA3F0 a compare of the return address; sub_821E8F70 / sub_821E9000 (every animated
// object) two guest loads; the library hooks (sub_82720D90, sub_8271F7B8, sub_8271FAE0) one relaxed atomic load;
// sub_8220F478 (once a frame) one flag test; sub_8220C3F8 (every camera view build) one compare.
// With pdz_cutscene_smooth off (the default since 30 Oct) every hook here tests that flag first and runs Rare's code
// unchanged: no BeginFrame, no final-frame view skip, no repeat frame, no dump.
// 7. The final frame: the frame Finished runs in renders the gameplay camera with a default rotation (log 291's
// dump): Finished pops the cutscene layer in the cutscene update, and the view builds of that frame
// (CameraControlUpdate's and the render's, sub_8220C3F8) read the gameplay layer before it is aimed. On that frame
// the active control's view build is skipped, so the control and the view keep what the frame before built (the
// cutscene's last view); nothing is written (log 294: a raw restore of 176 bytes crashed the shadow pass).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>

#include "cutscene_dump.h"
#include "game_pace.h" // LastCutsceneStep (the per-frame dump)

REXCVAR_DEFINE_BOOL(pdz_cutscene_smooth, false, "Render",
                    "In-engine cutscenes at the frame rate: Rare samples cutscene animation without interpolation "
                    "(each key held until the next; keys about 26-30 per second), so above 30 fps frames repeat poses. "
                    "On: the characters and objects, cameras, lights and props blend the two keys around the frame's "
                    "time with Rare's own interpolation (positions linear, rotations slerp / quaternion lerp), the "
                    "same time the sound sync uses; never past a key, never across a shot change; visibility, scene / "
                    "fog / light switches and the like stay stepped; emitters are unchanged. Off (the default since 30 "
                    "Oct: parked, black frames once per block with retime on, logs 291-297) = Rare's look, and every "
                    "hook of this file goes straight to Rare's code (the final-frame view, the repeat frame, retime, "
                    "the curve and the dump included). The sound sync (pdz_cutscene_audio_sync) is separate. One "
                    "'pdz_cutscene_smooth:' line per cutscene")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_cutscene_retime, false, "Render",
                    "With pdz_cutscene_smooth: sample the cutscene's 8-frame streaming blocks at the 360's cadence "
                    "(content at a steady 30 frames per second of picture time, half a key late; from the frame shown "
                    "at the previous block's switch to the next block's first key). Rare's mapping stretches each "
                    "block's keys over the block and skips one frame at every block switch (3.75 times a second). Off "
                    "(the default since 31 Oct) = Rare's key mapping. On, about once per block "
                    "the game runs an update without a Swap and then presents 1-4 frames with nothing drawn (the black "
                    "flashes), the view lunges at block starts, the sound sync spreads to 13-14 ms, and 3 of 6 test "
                    "runs crashed in render-side guest code right after the cutscene's end; off: none of these in 6 runs")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_cutscene_camera_spline, true, "Render",
                    "With pdz_cutscene_smooth: the cutscene camera follows a curve through its keys "
                    "(pdz_cutscene_camera_curve). Off = straight lines between keys")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(pdz_cutscene_camera_curve, "catmull", "Render",
                      "With pdz_cutscene_smooth and pdz_cutscene_camera_spline: the cutscene camera's curve through "
                      "its keys (eye, look-at point, lens, focus, viewport). catmull = Catmull-Rom (eye and look-at "
                      "as 3D vectors, never across a cut, a speed-change guard); monotone = a Fritsch-Butland cubic "
                      "per component (never past a key's value); linear = straight lines between keys")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_cutscene_frame_dump, false, "Render",
                    "Diagnostics, read only: for every cutscene a CSV in logs (cutscene_<log number>_<cutscene>_<n>.csv), "
                    "one row per game frame from its first frame to 30 frames after it ends: the frame / Swap / present "
                    "times, the cutscene's step and clocks, the key mapping, our eye and Rare's, the view (control "
                    "eye / target / up, the frame's camera matrix), the lens, the first characters' positions, view "
                    "repeats. Written when it ends; one log line per file. Changes nothing in the game")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pdz_cutscene_black_detect, false, "Render",
                    "Test aid (overnight 30-31 Oct; needs pdz_cutscene_frame_dump): every presented cutscene frame's "
                    "front buffer is sampled (an 8 x 8 grid of pixels read back a few frames later) and the dump gets "
                    "its brightness (bright_avg / bright_max columns), a 'dark' flag for a frame much darker than its "
                    "neighbours and a 'matrix_bad' flag for an invalid camera matrix; the dump's summary counts both. "
                    "Off by default: costs 64 small copies per cutscene frame")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pdz_cutscene_repeat_frame, true, "Render",
                    "A frame the game presents during a cutscene without having run the cutscene's update (log 292: "
                    "about one per block with retime on, undrawn, shown black by the native renderer) shows the "
                    "previous frame again. Never two in a row, and only after two updated frames. Counted on the "
                    "per-cutscene line (presents without a cutscene update). Also (night of 31 Oct): a cutscene frame "
                    "with no draws at all (retime's empty frames, 1-4 about once per block: the black flashes) right "
                    "after a drawn one shows that one again, at most 4 in a row. Off: present them as the game made "
                    "them")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_821EA3F0);
REX_EXTERN(__imp__sub_821E8F70);
REX_EXTERN(__imp__sub_821E9000);
REX_EXTERN(__imp__sub_82720D90);
REX_EXTERN(__imp__sub_8271F7B8);
REX_EXTERN(__imp__sub_8271FAE0);
REX_EXTERN(__imp__sub_82220AC8);
REX_EXTERN(__imp__sub_8220F478);
REX_EXTERN(__imp__sub_8220C3F8);

namespace
{
  // The game's frame clock block (game_pace.cpp): +68 the cutscene player, +136 the frame counter.
  constexpr uint32_t kClock = 0x82D6BFD8;
  constexpr uint32_t kClockCutscene = kClock + 68;
  constexpr uint32_t kClockFrames = kClock + 136;

  // The cutscene player (sub_8221ED68).
  constexpr uint32_t kCsState = 12;           // 3 = playing
  constexpr uint32_t kCsPlaying = 3;
  constexpr uint32_t kCsCtrlCount = 28;       // type 2: count, array of entries (entry +20 = the camera control)
  constexpr uint32_t kCsCtrlArray = 32;
  constexpr uint32_t kCsEntries = 80;         // all channels: 112 bytes each, +64 type, +72 the object (type 1)
  constexpr uint32_t kCsSetup = 84;           // +28 = the channel count
  constexpr uint32_t kCsBlock = 88;           // the block index (sub_82221368)
  constexpr uint32_t kEntrySize = 112;
  constexpr uint32_t kEntryType = 64;
  constexpr uint32_t kEntryObject = 72;
  constexpr uint32_t kCtrlEntryCtrl = 20;
  constexpr uint32_t kCtrlLayers = 20;        // control +20: the layer stack; +1092 its end (top = end - 4)
  constexpr uint32_t kCtrlLayersEnd = 1092;
  constexpr uint32_t kCtrlEye = 948;          // sub_8220C3F8: the view's eye / target (3 floats each)
  constexpr uint32_t kCtrlTarget = 972;
  // The active camera control (the camera render sub_8220D988 inside CameraControlUpdate reads it: lis -32041 /
  // addi -28600). sub_8220C3F8 (r3 = a control) builds its view: the top layer's eye / target / up into +948 / +972 /
  // +984 (verified: addi r30/r29/r27 = r31 + 948 / 972 / 984 go to sub_827D6470), the camera matrix into +1024, the
  // view matrix into the view (sub_82636638).
  constexpr uint32_t kActiveCamera = 0x82D69048;
  constexpr uint32_t kCtrlUpVector = 984;
  constexpr uint32_t kLayerActive = 124;      // 822132B0 samples only while this is set
  constexpr uint32_t kLayerSampler = 128;
  constexpr uint32_t kLayerTracks = 500;      // the float track buffer (822132B0's r5)
  constexpr uint32_t kLayerFocalLength = 504; // camera track pointers (0 = not bound)
  constexpr uint32_t kLayerFovY = 536;
  constexpr uint32_t kLayerUseLookAt = 576;   // > 0: the target is the look-at point (822134D8-82213A14)
  constexpr uint32_t kLayerSceneId = 600;
  constexpr uint32_t kLightSampler = 128;     // type 3 (sub_82222E20)
  constexpr uint32_t kLightTracks = 356;
  // Type 1 game objects: +72 the animation object; its +4 the slots' list (+12: +0 nodes of 8 bytes {+2 next index,
  // +4 slot}, +8 the head node); slot +0 = its id, +16 its sampler (sub_821EB640, sub_821EB730).
  constexpr uint32_t kObjAnim = 72;
  constexpr uint32_t kAnimSlots = 4;
  constexpr uint32_t kSlotList = 12;
  constexpr uint32_t kSlotSampler = 16;
  constexpr uint32_t kCutsceneSlot = 0;       // sub_822214F8: sub_821EC2B8(anim object, 0, 0)

  // The sampler (library, 8271xxxx).
  constexpr uint32_t kSmpAnim = 0;            // the clip
  constexpr uint32_t kSmpMatrix = 12;         // the channel's matrix (sub_821EA350; sub_821EA3F0 adds the root)
  constexpr uint32_t kSmpTrackOffset = 38;    // u16: the float tracks' slot in the pose buffer (sub_82747120)
  constexpr uint32_t kSmpBones = 46;          // u16
  constexpr uint32_t kSmpRootPos = 96;        // the sampled root position (sub_8271DDC0, 8271E390)
  constexpr uint32_t kSmpPose = 116;          // the pose buffer (48 bytes a bone)
  constexpr uint32_t kSmpTime = 148;          // float, 0..1
  constexpr uint32_t kSmpRate = 160;          // float, 1 / the clip's length (sub_8271D978)
  constexpr uint32_t kSmpInterp = 164;        // 0 none (step), 1 linear, 2 cubic
  constexpr uint32_t kSmpPlayMode = 168;      // 0 wrap, 1 clamp, 2 ping-pong
  constexpr uint32_t kSmpSubdiv = 204;        // u16
  constexpr uint32_t kSmpTrackCount = 212;    // u16: float tracks copied to the channel's buffer
  constexpr uint32_t kAnimLength = 28;        // float, s
  constexpr uint32_t kAnimKeys = 34;          // u16, frames
  constexpr uint32_t kAnimRoot = 40;          // the root track: +0..8 the clip's offset, +28..35 flags, +36 the keys
  constexpr uint32_t kAnimKeyStep = 64;       // float, stored keys per frame (1 = every frame)
  constexpr uint32_t kRootFlags = 28;
  constexpr uint32_t kRootKeys = 36;
  constexpr uint32_t kMatrixTranslation = 48;
  constexpr float kBlockRate = 30.0f;         // the blocks' frame rate (8 frames per 0.267 s)
  constexpr float kLag = 0.5f;                // keys (see 2)

  // sub_821EA3F0's callers, by return address: the channels smoothed through it.
  enum class Kind
  {
    kNone,
    kLayer, // 822132EC in 822132B0: camera control layer
    kLight, // 82222E54 in sub_82222E20
    kProp,  // 82222B64 in sub_82222B38, 822233B8 in sub_82223390
  };
  Kind KindOf(uint32_t ret)
  {
    switch (ret)
    {
    case 0x822132F0: return Kind::kLayer;
    case 0x82222E58: return Kind::kLight;
    case 0x82222B68:
    case 0x822233BC: return Kind::kProp;
    default: return Kind::kNone;
    }
  }

  // The continuous float tracks (the channel's track pointers; all others stay stepped). The camera's look-at point
  // (+564..572) is entries kLookAt..kLookAt + 2: a 3D vector for the curve and the cut test.
  constexpr uint32_t kCameraContinuous[] = {504, 508, 512, 516, 520, 524, 532, 536, 540, 544, 548, 552, 564, 568, 572};
  constexpr uint32_t kLookAt = 12;
  constexpr uint32_t kLightContinuous[] = {360, 364, 368, 372};
  constexpr uint32_t kMaxContinuous = 16;
  constexpr uint32_t kNoTrack = 0xFFFFFFFFu;

  // Cut tests (game units are cm; keys about 1/30 s apart). Each measure is a cut above its absolute limit, or above
  // its floor and kRatio x the larger / kRatioAvg x the average of the neighbouring intervals'.
  constexpr float kEyeAbs = 150.0f, kEyeFloor = 15.0f;  // the eye's move
  constexpr float kTurnAbs = 0.35f, kTurnFloor = 0.06f; // the view direction's turn, rad (20 / 3.4 degrees)
  constexpr float kLookAbs = 300.0f, kLookFloor = 20.0f; // the look-at point's move
  constexpr float kRotAbs = 0.6f, kRotFloor = 0.1f;     // objects' root turn (rad)
  constexpr float kRatio = 3.0f;
  constexpr float kRatioAvg = 4.0f;
  constexpr float kSceneMove = 0.5f; // a scene change counts as a cut only with a move over this (cm)
  // The catmull curve's guard.
  constexpr float kSpeedRatio = 3.0f;
  constexpr float kTangentCap = 1.5f;
  constexpr uint32_t kMaxControls = 64;
  constexpr uint32_t kMaxObjects = 48;
  constexpr uint32_t kMaxEntries = 512;
  constexpr uint32_t kMaxKeys = 64;
  constexpr uint32_t kMaxBones = 160;
  constexpr uint32_t kSnaps = 24;
  // The line's camera checks.
  constexpr uint32_t kStillRun = 8;
  constexpr float kOffKeys = 1.0f; // cm
  constexpr float kStop = 0.25f;
  constexpr float kTeleport = 4.0f;
  constexpr float kMoveEps = 0.05f; // cm
  constexpr uint32_t kSwitchDistances = 512;
  constexpr uint32_t kRing = 16;
  constexpr uint32_t kRingLogged = 12;
  constexpr uint32_t kAfterFrames = 3;
  constexpr float kInterimMs = 10000.0f;
  constexpr uint32_t kEvents = 8; // cuts / teleports listed in the line

  uint32_t L32(const uint8_t *base, uint32_t a)
  {
    return _byteswap_ulong(*reinterpret_cast<const uint32_t *>(base + a));
  }
  uint32_t L16(const uint8_t *base, uint32_t a)
  {
    return _byteswap_ushort(*reinterpret_cast<const uint16_t *>(base + a));
  }
  uint32_t L8(const uint8_t *base, uint32_t a) { return base[a]; }
  float LF(const uint8_t *base, uint32_t a)
  {
    const uint32_t v = L32(base, a);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }
  void S32(uint8_t *base, uint32_t a, uint32_t v) { *reinterpret_cast<uint32_t *>(base + a) = _byteswap_ulong(v); }
  void SF(uint8_t *base, uint32_t a, float f)
  {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    S32(base, a, v);
  }
  void LV(const uint8_t *base, uint32_t a, float (&v)[3])
  {
    for (uint32_t i = 0; i < 3; ++i)
    {
      v[i] = LF(base, a + 4 * i);
    }
  }
  float Dist(const float *a, const float *b)
  {
    float d2 = 0.0f;
    for (uint32_t i = 0; i < 3; ++i)
    {
      d2 += (b[i] - a[i]) * (b[i] - a[i]);
    }
    return std::sqrt(d2);
  }
  // The angle (rad) between the directions a1 -> b1 and a2 -> b2; -1 if either is too short.
  float Turn(const float *a1, const float *b1, const float *a2, const float *b2)
  {
    float d1[3], d2[3], n1 = 0.0f, n2 = 0.0f, dot = 0.0f;
    for (uint32_t i = 0; i < 3; ++i)
    {
      d1[i] = b1[i] - a1[i];
      d2[i] = b2[i] - a2[i];
      n1 += d1[i] * d1[i];
      n2 += d2[i] * d2[i];
      dot += d1[i] * d2[i];
    }
    if (n1 < 1.0f || n2 < 1.0f)
    {
      return -1.0f;
    }
    return std::acos(std::min(1.0f, std::max(-1.0f, dot / std::sqrt(n1 * n2))));
  }

  // The playing cutscene's player, or 0 (two guest loads).
  uint32_t PlayingCutscene(const uint8_t *base)
  {
    const uint32_t player = L32(base, kClockCutscene);
    return player && L32(base, player + kCsState) == kCsPlaying ? player : 0;
  }

  // ---- Per cutscene, for its line (game thread; atomics as the other hooks). ----
  struct Count
  {
    std::atomic<uint64_t> seen{0}, smoothed{0}, held{0}, irregular{0};
  };
  Count g_cam, g_cam0, g_obj, g_other; // camera controls (all, the first one), type 1 objects, lights / props
  enum Hold
  {
    kHoldObjectRoot,
    kHoldCamera,      // the first camera's interval (eye / view turn / look-at / scene with a move)
    kHoldCameraOther, // another camera control's root
    kHoldCameraCross,
    kSceneNoJump,     // counted, not held
    kHoldCameraLate,
    kHolds
  };
  std::atomic<uint64_t> g_holds[kHolds];
  std::atomic<uint64_t> g_frames{0}, g_cut_frames{0}, g_cuts{0};
  std::atomic<uint64_t> g_tracks{0}, g_stepped{0};
  std::atomic<uint32_t> g_cam_keys{0}, g_cam_controls{0}, g_cam_lens{0}, g_obj_max{0};
  std::atomic<float> g_cam_length{0.0f};
  std::atomic<uint64_t> g_retimed{0}, g_rare_mapping{0}, g_past_end{0};
  std::atomic<uint64_t> g_cam_cross_blend{0}, g_cam_cross_kept{0}, g_cam_cross_key0{0};
  std::atomic<uint64_t> g_obj_cross_blend{0}, g_obj_cross_kept{0}, g_obj_cross_key0{0};
  std::atomic<uint64_t> g_curve_eye{0}, g_curve_tracks{0}, g_linear_cam{0};
  std::atomic<uint64_t> g_eye_moved{0}, g_eye_same{0}, g_run_inside{0}, g_run_still{0}, g_same_time_still{0};
  std::atomic<uint32_t> g_run_inside_max{0}, g_run_still_max{0}, g_runs_inside{0};
  std::atomic<uint64_t> g_off_frames{0}, g_stops_switch{0}, g_stops_inside{0}, g_teleports{0}, g_space_mismatch{0};
  std::atomic<float> g_off_worst{0.0f}, g_off_worst_ms{0.0f};
  std::atomic<uint64_t> g_off_worst_frame{0};
  std::atomic<uint64_t> g_switch_blended{0}, g_switch_cut{0};
  float g_switch_distance[kSwitchDistances];
  uint32_t g_switch_n = 0;
  bool g_pending = false;
  uint32_t g_pending_player = 0;
  float g_next_interim_ms = kInterimMs;

  // The cuts and teleports of the line (the first kEvents of each), as text.
  std::string g_cut_events, g_teleport_events;
  uint32_t g_cut_events_n = 0, g_teleport_events_n = 0;

  // A smoothed sample is in progress (the library hooks' first test; gameplay animation calls them too).
  std::atomic<uint32_t> g_live{0};

  // The frame a sampler showed at its block's switch (t = 1), for the next block's first part.
  struct Snap
  {
    uint32_t sampler = 0;
    uint32_t player = 0;
    uint32_t block = 0;      // the block it was kept in
    float kept_at = -1.0f;   // its key position counted from the next block's key 0 (the shown position - frames)
    uint32_t bones = 0;      // objects: the pose (local to the skeleton), big-endian as in guest memory
    uint32_t pose[kMaxBones * 12] = {};
    bool world = false;      // the camera: its world eye (the matrix translation) and tracks as shown
    float eye[3] = {};
    uint32_t ntracks = 0;
    float tracks[kMaxContinuous] = {};
    bool use_look = false;
    bool scene_ok = false;
    int32_t scene = 0;
    bool switch_counted = false;
    bool switch_cut = false; // the switch into the next block was a cut (interval 0's curve doesn't reach back)
  };
  Snap g_snaps[kSnaps];
  uint32_t g_snap_next = 0;

  Snap *FindSnap(uint32_t sampler, bool create)
  {
    for (Snap &s : g_snaps)
    {
      if (s.sampler == sampler)
      {
        return &s;
      }
    }
    if (!create)
    {
      return nullptr;
    }
    Snap &s = g_snaps[g_snap_next++ % kSnaps];
    s.sampler = sampler;
    s.bones = 0;
    s.world = false;
    s.ntracks = 0;
    s.scene_ok = false;
    return &s;
  }

  // One interval of the first camera's clip, judged once per block.
  struct Interval
  {
    float eye = 0.0f, turn = -1.0f, look = -1.0f; // cm, rad (-1 = no direction), cm (-1 = no look-at)
    bool scene_change = false;
    uint32_t why = 0; // 1 eye, 2 eye vs neighbours, 4 turn, 8 turn vs neighbours, 16 look-at, 32 look-at vs
                      // neighbours, 64 scene with a move, 128 another root test
    bool cut = false;
  };
  // The first camera's keys of its current clip: every continuous track, the scene and use-look-at flags, read with
  // Rare's decoder at the block's first sample; then each interval judged.
  struct CamKeys
  {
    uint32_t anim = 0;
    uint32_t block = 0;
    uint32_t keys = 0;
    uint32_t n = 0;
    float vals[kMaxKeys][kMaxContinuous];
    int32_t scene[kMaxKeys];
    bool use_look[kMaxKeys];
    float eye[kMaxKeys][3]; // world
    Interval in[kMaxKeys];
    bool ready = false;
    uint32_t counted_scene = ~0u;
    // The previous block's last interval (interval 0's earlier neighbour).
    bool tail = false;
    uint32_t tail_block = 0;
    Interval tail_in;
  };
  CamKeys g_ck;

  // The camera's last frames (the end-of-cutscene log).
  struct RingEntry
  {
    uint64_t frame = 0;
    uint32_t block = 0, key = 0;
    float u = 0.0f;
    uint32_t flags = 0; // 1 hold, 2 block start, 4 kept across a cut, 8 late cut, 16 curve, 32 past the end, 64 switch
    float eye[3] = {};
    float view_eye[3] = {}, view_target[3] = {};
    bool view = false;
  };
  RingEntry g_ring[kRing];
  uint32_t g_ring_n = 0;
  uint32_t g_after = 0;
  uint32_t g_after_ctrl = 0;
  // The final frame (see 7): the frame Finished ran in (its game frame number, 0 = none pending) and whether its
  // line went out.
  uint32_t g_final_frame = 0;
  bool g_final_logged = false;
  // Presents without a cutscene update (csdump::BeforeSwap, every Swap): the counters for the line, the Swap marked
  // for the presenter to repeat the previous frame, and the previous Swap's cutscene step count / block / player.
  std::atomic<uint64_t> g_no_update_presents{0}, g_no_update_switch{0}, g_presents_repeated{0};
  std::atomic<uint64_t> g_skip_swap{0};
  // The cutscene Swaps (BeforeSwap, game thread) by number for the presenter's empty-frame test (record thread, a few
  // Swaps later), and the empty frames it showed as the previous one.
  constexpr uint32_t kCsSwapRing = 64;
  std::atomic<uint64_t> g_cs_swap_ring[kCsSwapRing] = {};
  std::atomic<uint64_t> g_empty_repeated{0};
  uint64_t g_swap_calls = 0;
  uint32_t g_swap_block = 0, g_swap_player = 0, g_swap_updated_run = 0;

  // ---- Game thread only (the cutscene update): this frame's decisions, built by the first hooked call. ----
  struct Object
  {
    uint32_t object = 0;
    uint32_t sampler = 0;
    bool regular = false;
    bool cut = false;  // its own root jumps in this interval: holds this object only
    float tu = 0.0f;   // its t before the clamp this frame
  };
  enum Curve
  {
    kCurveLinear,
    kCurveMonotone,
    kCurveCatmull
  };
  struct History
  {
    float eye[3] = {};
    bool cut = false, switch_frame = false;
    uint64_t frame = 0;
    float ms = 0.0f;
    uint32_t block = 0, k = 0;
  };
  struct Frame
  {
    bool valid = false;
    uint32_t frame = 0;
    uint32_t player = 0;
    uint32_t block = 0;
    bool cut = false;
    bool cross_cut = false;
    bool past_end = false;  // the block was passed in one frame: keep the previous block's last shown frame
    bool prev_cut = false;
    Curve curve = kCurveCatmull;
    uint32_t cam0_sampler = 0, cam0_ctrl = 0;
    float cam0_ms = 0.0f;
    uint32_t n_objects = 0;
    Object objects[kMaxObjects];
    float eye[6] = {};
    bool eye_valid = false;
    uint32_t cam_time = 0, cam_block = 0;
    uint32_t run = 0;
    bool moved_before = false;
    History hist[4];
    uint32_t hist_n = 0;
  };
  Frame g_f;

  // ---- The sample being smoothed (game thread): set by the channel hooks, read by the library hooks. ----
  struct Sample
  {
    bool on = false;
    uint32_t sampler = 0;
    bool hold = false;
    bool cross_cut = false;
    bool past_end = false;
    bool retime = false;
    bool camera = false;
    bool first_block = false;
    float tu = 0.0f;         // t before the clamp (the channel hook's prediction)
    Curve curve = kCurveLinear;
    bool mapped = false;
    bool armed = false;
    bool retimed = false;
    bool record = false;     // t = 1: the block's switch frame, kept for the next block
    bool cross = false;      // the block's first part: kept frame -> key 0 (weight w on key 0)
    float w = 0.0f;
    float pos = 0.0f;
    uint32_t frames = 0;
    uint32_t keys = 0;
    uint32_t k = 0;
    float u = 0.0f;
    uint32_t discrete = 0;
    uint32_t idx_at = 0;
    uint32_t idx[4] = {};
    Snap *snap = nullptr;
    bool late_cut = false;
    bool cam_cross_cut = false;
  };
  thread_local Sample t_s;
  thread_local uint32_t t_cont[kMaxContinuous];
  thread_local uint32_t t_cont_n = 0;
  thread_local int32_t t_scene_index = -1;
  thread_local int32_t t_use_index = -1;

  bool BlockClip(const uint8_t *base, uint32_t anim)
  {
    const uint32_t frames = L16(base, anim + kAnimKeys);
    const float length = LF(base, anim + kAnimLength);
    return frames >= 2 && length > 0.0f && std::fabs(float(frames) - kBlockRate * length) < 0.05f;
  }

  // The key position this frame samples: Rare's t x (frames - 1), or for block clips with retiming t x frames - 1 -
  // kLag from t before the clamp (below 0 in the block's first part: from the kept frame to key 0).
  struct Mapping
  {
    bool ok = false;
    bool retimed = false;
    uint32_t anim = 0;
    uint32_t keys = 0;
    float pos = 0.0f;
  };

  Mapping Map(const uint8_t *base, uint32_t sampler, float t, float tu, bool retime, bool first_block)
  {
    Mapping m;
    m.anim = L32(base, sampler + kSmpAnim);
    if (!m.anim || L16(base, sampler + kSmpSubdiv) != 0)
    {
      return m;
    }
    const uint32_t frames = L16(base, m.anim + kAnimKeys);
    const float s = LF(base, m.anim + kAnimKeyStep);
    if (frames < 2 || s != 1.0f)
    {
      return m; // a key layout Rare's own mapping handles (sub_82720D90's tail branch)
    }
    m.keys = frames;
    if (retime && BlockClip(base, m.anim))
    {
      m.retimed = true;
      m.pos = std::max(tu, t) * float(frames) - 1.0f - kLag;
      m.pos = first_block ? std::max(m.pos, 0.0f) : std::max(m.pos, -1.0f - kLag);
    }
    else
    {
      m.pos = std::max(t * float(frames - 1), 0.0f);
    }
    m.pos = std::min(m.pos, float(frames - 1));
    m.ok = true;
    return m;
  }

  uint32_t KeyAt(const Mapping &m) { return m.pos <= 0.0f ? 0u : std::min<uint32_t>(uint32_t(m.pos), m.keys - 1); }
  uint32_t IntervalAt(const Mapping &m) { return std::min<uint32_t>(KeyAt(m), m.keys - 2); }

  // t after this frame's step, as Rare computes it (fmadds at 821EA414 / 821EB9A4; sub_8271D3E8: clamped in play mode
  // 1, the fraction in play mode 0), and before the clamp.
  bool Advance(const uint8_t *base, uint32_t sampler, double step, float &t, float &tu)
  {
    const uint32_t play = L32(base, sampler + kSmpPlayMode);
    if (play > 1)
    {
      return false;
    }
    t = float(std::fma(double(LF(base, sampler + kSmpRate)), step, double(LF(base, sampler + kSmpTime))));
    tu = t;
    if (play == 1)
    {
      t = t > 1.0f ? 1.0f : (t < 0.0f ? 0.0f : t);
    }
    else if (t >= 1.0f || t < 0.0f)
    {
      const float whole = std::trunc(t);
      t = t >= 1.0f ? t - whole : t - (whole - 1.0f);
      tu = t;
    }
    return true;
  }

  bool Predict(const uint8_t *base, uint32_t sampler, double step, bool first_block, Mapping &m, float *tu_out,
               float *t_out = nullptr)
  {
    float t, tu;
    if (!Advance(base, sampler, step, t, tu))
    {
      return false;
    }
    if (tu_out)
    {
      *tu_out = tu;
    }
    if (t_out)
    {
      *t_out = t;
    }
    m = Map(base, sampler, t, tu, REXCVAR_GET(pdz_cutscene_retime), first_block);
    return m.ok;
  }

  // The root track's layout (sub_8271F630, sub_8271DDC0).
  struct Root
  {
    uint32_t desc = 0;
    uint32_t keys_at = 0;
    uint32_t stride = 0;
    uint32_t npos = 0;
    uint32_t nrot = 0;
    bool quat = false;
    bool has[3] = {};
  };

  bool ReadRoot(const uint8_t *base, uint32_t anim, Root &r)
  {
    r.desc = L32(base, anim + kAnimRoot);
    if (!r.desc)
    {
      return false;
    }
    r.keys_at = L32(base, r.desc + kRootKeys);
    if (!r.keys_at)
    {
      return false;
    }
    bool f[8];
    for (uint32_t i = 0; i < 8; ++i)
    {
      f[i] = L8(base, r.desc + kRootFlags + i) != 0;
    }
    r.has[0] = f[0];
    r.has[1] = f[1];
    r.has[2] = f[2];
    r.npos = uint32_t(f[0]) + f[1] + f[2];
    r.quat = f[7];
    r.nrot = r.quat ? ((f[3] || f[4] || f[5]) ? 4u : 0u) : uint32_t(f[3]) + f[4] + f[5];
    r.stride = r.npos + r.nrot;
    return r.stride != 0;
  }

  float Key(const uint8_t *base, const Root &r, uint32_t key, uint32_t c)
  {
    return LF(base, r.keys_at + 4 * (r.stride * key + c));
  }

  void KeyPos(const uint8_t *base, const Root &r, uint32_t key, float (&p)[3])
  {
    uint32_t c = 0;
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
      p[axis] = r.has[axis] ? Key(base, r, key, c++) : 0.0f;
    }
  }

  // The clip's root offset sub_821EA3F0 adds with the sampled position (821EA50C-821EA52C).
  void ClipOffset(const uint8_t *base, const Root &r, float (&o)[3]) { LV(base, r.desc, o); }

  // How far the root moves (cm) and turns (rad) from key i to key i + 1.
  void Jump(const uint8_t *base, const Root &r, uint32_t i, float &dp, float &da)
  {
    float d2 = 0.0f;
    for (uint32_t c = 0; c < r.npos; ++c)
    {
      const float d = Key(base, r, i + 1, c) - Key(base, r, i, c);
      d2 += d * d;
    }
    dp = std::sqrt(d2);
    da = 0.0f;
    if (r.quat && r.nrot == 4)
    {
      float dot = 0.0f, na = 0.0f, nb = 0.0f;
      for (uint32_t c = r.npos; c < r.npos + 4; ++c)
      {
        const float a = Key(base, r, i, c), b = Key(base, r, i + 1, c);
        dot += a * b;
        na += a * a;
        nb += b * b;
      }
      const float n = std::sqrt(na * nb);
      const float half_cos = n > 0.0f ? std::min(1.0f, std::fabs(dot) / n) : 1.0f;
      da = 2.0f * std::acos(half_cos);
    }
    else
    {
      for (uint32_t c = r.npos; c < r.npos + r.nrot; ++c)
      {
        da = std::max(da, std::fabs(Key(base, r, i + 1, c) - Key(base, r, i, c)));
      }
    }
  }

  // A measure against its absolute limit and its neighbours (< 0 = absent): 1 = absolute, 2 = against the neighbours.
  uint32_t CutBy(float d, float prev, float next, float abs_limit, float floor)
  {
    if (d < 0.0f)
    {
      return 0;
    }
    if (d > abs_limit)
    {
      return 1;
    }
    if (d <= floor || (prev < 0.0f && next < 0.0f))
    {
      return 0;
    }
    const float mx = std::max(prev, next);
    const float avg = prev >= 0.0f && next >= 0.0f ? 0.5f * (prev + next) : mx;
    return d > kRatio * mx || d > kRatioAvg * avg ? 2 : 0;
  }

  // An object's (or a non-first camera's) root interval [i, i + 1] jumps (same clip: one space).
  bool RootCut(const uint8_t *base, uint32_t anim, uint32_t keys, uint32_t i)
  {
    Root r;
    if (i + 1 >= keys || !ReadRoot(base, anim, r))
    {
      return false;
    }
    float dp, da, pp = -1.0f, pa = -1.0f, np = -1.0f, na = -1.0f;
    Jump(base, r, i, dp, da);
    if (i > 0)
    {
      Jump(base, r, i - 1, pp, pa);
    }
    if (i + 2 < keys)
    {
      Jump(base, r, i + 1, np, na);
    }
    return CutBy(dp, pp, np, kEyeAbs, kEyeFloor) || CutBy(da, pa, na, kRotAbs, kRotFloor);
  }

  // The first camera's interval i is a cut (its block's keys judged), else the root test.
  bool CameraCut(const uint8_t *base, uint32_t anim, uint32_t keys, uint32_t i)
  {
    if (g_ck.ready && g_ck.anim == anim && i + 1 < g_ck.keys)
    {
      return g_ck.in[i].cut;
    }
    return RootCut(base, anim, keys, i);
  }

  uint32_t ControlLayer(const uint8_t *base, uint32_t ctrl)
  {
    const uint32_t end = L32(base, ctrl + kCtrlLayersEnd);
    if (end == ctrl + kCtrlLayers)
    {
      return 0;
    }
    const uint32_t layer = L32(base, end - 4);
    return layer && L32(base, layer + kLayerActive) ? layer : 0;
  }

  uint32_t FindSlot(const uint8_t *base, uint32_t anim_object, uint32_t id)
  {
    const uint32_t slots = anim_object ? L32(base, anim_object + kAnimSlots) : 0;
    if (!slots)
    {
      return 0;
    }
    const uint32_t list = slots + kSlotList;
    const uint32_t head = L32(base, list + 8), nodes = L32(base, list + 0);
    if (!head)
    {
      return 0;
    }
    int32_t index = int16_t(int32_t(head - nodes) >> 3);
    uint32_t found = 0;
    for (uint32_t guard = 0; index != -1 && guard < 256; ++guard)
    {
      const uint32_t node = nodes + uint32_t(index) * 8;
      const uint32_t slot = L32(base, node + 4);
      if (slot && L8(base, slot) == id)
      {
        found = slot;
      }
      index = int16_t(L16(base, node + 2));
    }
    return found;
  }

  void UpdateMax(std::atomic<uint32_t> &m, uint32_t v)
  {
    if (v > m.load(std::memory_order_relaxed))
    {
      m.store(v, std::memory_order_relaxed);
    }
  }

  void EndRun(bool by_motion)
  {
    if (!g_f.run)
    {
      return;
    }
    if (by_motion && g_f.moved_before && g_f.run < kStillRun)
    {
      g_run_inside.fetch_add(g_f.run, std::memory_order_relaxed);
      g_runs_inside.fetch_add(1, std::memory_order_relaxed);
      UpdateMax(g_run_inside_max, g_f.run);
    }
    else
    {
      g_run_still.fetch_add(g_f.run, std::memory_order_relaxed);
      UpdateMax(g_run_still_max, g_f.run);
    }
    g_f.run = 0;
  }

  void CheckView(const uint8_t *base, uint32_t ctrl, uint32_t sampler, uint32_t block)
  {
    float v[6];
    for (uint32_t c = 0; c < 3; ++c)
    {
      v[c] = LF(base, ctrl + kCtrlEye + 4 * c);
      v[3 + c] = LF(base, ctrl + kCtrlTarget + 4 * c);
    }
    if (g_ring_n)
    {
      RingEntry &e = g_ring[(g_ring_n - 1) % kRing];
      if (!e.view)
      {
        std::memcpy(e.view_eye, v, 12);
        std::memcpy(e.view_target, v + 3, 12);
        e.view = true;
      }
    }
    const uint32_t time = sampler ? L32(base, sampler + kSmpTime) : 0;
    if (g_f.eye_valid)
    {
      if (std::memcmp(v, g_f.eye, sizeof(v)) != 0)
      {
        g_eye_moved.fetch_add(1, std::memory_order_relaxed);
        EndRun(true);
        g_f.moved_before = true;
      }
      else
      {
        g_eye_same.fetch_add(1, std::memory_order_relaxed);
        ++g_f.run;
        if (time == g_f.cam_time && block == g_f.cam_block)
        {
          g_same_time_still.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
    std::memcpy(g_f.eye, v, sizeof(v));
    g_f.eye_valid = true;
    g_f.cam_time = time;
    g_f.cam_block = block;
  }

  Curve CurveMode()
  {
    if (!REXCVAR_GET(pdz_cutscene_camera_spline))
    {
      return kCurveLinear;
    }
    const std::string &s = REXCVAR_GET(pdz_cutscene_camera_curve);
    return s == "linear" ? kCurveLinear : s == "monotone" ? kCurveMonotone : kCurveCatmull;
  }

  void Line(const uint8_t *base, uint32_t player, const char *when);
  void ResetStats();
  void NoteDumpMapping(uint32_t sampler, uint32_t frame, uint32_t block, float pos, uint32_t k, float u, uint32_t key,
                       uint32_t flags);

  std::string WhyText(uint32_t why)
  {
    std::string s;
    const char *names[] = {"eye", "eye vs neighbours", "view turn", "view turn vs neighbours", "look-at",
                           "look-at vs neighbours", "scene with a move", "root"};
    for (uint32_t b = 0; b < 8; ++b)
    {
      if (why & (1u << b))
      {
        s += s.empty() ? "" : "+";
        s += names[b];
      }
    }
    return s.empty() ? "none" : s;
  }

  // A cut entry of the line: the interval's measures and their neighbours'.
  void NoteCut(uint32_t k, bool cross, float cross_eye, const char *by)
  {
    if (g_cut_events_n >= kEvents)
    {
      return;
    }
    ++g_cut_events_n;
    const auto f = [](float x) { return x < 0.0f ? std::string("-") : fmt::format("{:.1f}", x); };
    const auto deg = [](float x) { return x < 0.0f ? std::string("-") : fmt::format("{:.1f}", x * 57.2958f); };
    if (cross)
    {
      g_cut_events += fmt::format("[frame {}, {:.0f} ms, block {} switch: eye {} cm, by {}] ",
                                  g_frames.load(std::memory_order_relaxed), g_f.cam0_ms, g_f.block, f(cross_eye), by);
      return;
    }
    const Interval *in = g_ck.ready && k + 1 < g_ck.keys ? &g_ck.in[k] : nullptr;
    const Interval *p = in && k > 0 ? &g_ck.in[k - 1] : nullptr;
    const Interval *n = in && k + 2 < g_ck.keys ? &g_ck.in[k + 1] : nullptr;
    g_cut_events += fmt::format(
        "[frame {}, {:.0f} ms, block {}, keys {}-{}: eye {} cm (neighbours {} / {}), turn {} deg ({} / {}), look-at {} "
        "cm, by {}] ",
        g_frames.load(std::memory_order_relaxed), g_f.cam0_ms, g_f.block, k, k + 1, in ? f(in->eye) : "?",
        p ? f(p->eye) : "-", n ? f(n->eye) : "-", in ? deg(in->turn) : "?", p ? deg(p->turn) : "-",
        n ? deg(n->turn) : "-", in ? f(in->look) : "?", in ? WhyText(in->why) : std::string(by));
  }

  // Judge the first camera's intervals of its clip (all keys known): the measures, then the rules against the
  // neighbours (interval 0's earlier neighbour: the previous block's last interval).
  void JudgeCamera(const uint8_t *base, uint32_t anim)
  {
    CamKeys &c = g_ck;
    Root r;
    const bool root = ReadRoot(base, anim, r);
    for (uint32_t i = 0; i + 1 < c.keys; ++i)
    {
      Interval &in = c.in[i];
      in = Interval{};
      in.eye = Dist(c.eye[i], c.eye[i + 1]);
      const bool look = c.use_look[i] && c.use_look[i + 1] && c.n > kLookAt + 2 && t_cont[kLookAt] != kNoTrack;
      if (look)
      {
        in.look = Dist(c.vals[i] + kLookAt, c.vals[i + 1] + kLookAt);
        in.turn = Turn(c.eye[i], c.vals[i] + kLookAt, c.eye[i + 1], c.vals[i + 1] + kLookAt);
      }
      if (in.turn < 0.0f && root && r.nrot)
      {
        // No look-at direction: the root rotation sets the view (82213A14); its turn (one space within the clip).
        float dp;
        Jump(base, r, i, dp, in.turn);
      }
      in.scene_change = c.scene[i] != c.scene[i + 1];
    }
    for (uint32_t i = 0; i + 1 < c.keys; ++i)
    {
      Interval &in = c.in[i];
      const bool tail = i == 0 && c.tail && c.tail_block + 1 == c.block;
      const Interval *p = i > 0 ? &c.in[i - 1] : (tail ? &c.tail_in : nullptr);
      const Interval *n = i + 2 < c.keys ? &c.in[i + 1] : nullptr;
      const uint32_t e = CutBy(in.eye, p ? p->eye : -1.0f, n ? n->eye : -1.0f, kEyeAbs, kEyeFloor);
      const uint32_t t = CutBy(in.turn, p ? p->turn : -1.0f, n ? n->turn : -1.0f, kTurnAbs, kTurnFloor);
      const uint32_t l = CutBy(in.look, p ? p->look : -1.0f, n ? n->look : -1.0f, kLookAbs, kLookFloor);
      in.why = (e == 1 ? 1u : e == 2 ? 2u : 0u) | (t == 1 ? 4u : t == 2 ? 8u : 0u) | (l == 1 ? 16u : l == 2 ? 32u : 0u);
      if (in.scene_change && (in.eye > kSceneMove || std::max(in.look, 0.0f) > kSceneMove))
      {
        in.why |= 64;
      }
      in.cut = in.why != 0;
    }
  }

  // A block switch of the first camera across a cut, before it samples: the kept world eye against key 0's (its key +
  // the clip's offset, as sub_821EA3F0 will place it); the references: the previous block's last interval (scaled to
  // the kept frame's distance from key 0) and the new block's first.
  bool CameraCrossCut(const uint8_t *base, uint32_t sampler, const Mapping &m, float *distance)
  {
    const Snap *s = FindSnap(sampler, false);
    Root r;
    if (!s || !s->world || s->player != g_f.player || s->block + 1 != g_f.block || !ReadRoot(base, m.anim, r))
    {
      return false;
    }
    float p0[3], o[3];
    KeyPos(base, r, 0, p0);
    ClipOffset(base, r, o);
    for (uint32_t a = 0; a < 3; ++a)
    {
      p0[a] += o[a];
    }
    const float d = Dist(s->eye, p0);
    if (distance)
    {
      *distance = d;
    }
    // The kept block's last interval: still the current clip judgement before the new block's first camera sample.
    const float span = std::max(1.0f, -s->kept_at);
    const Interval *last = g_ck.ready && g_ck.block == s->block && g_ck.keys >= 2 ? &g_ck.in[g_ck.keys - 2]
                           : g_ck.tail && g_ck.tail_block == s->block              ? &g_ck.tail_in
                                                                                   : nullptr;
    const float prev = last ? last->eye * span : -1.0f;
    float next = -1.0f, na;
    if (m.keys >= 2)
    {
      Jump(base, r, 0, next, na);
    }
    return CutBy(d, prev, next, kEyeAbs, kEyeFloor) != 0;
  }

  // Once per frame, before the first animation of the cutscene samples.
  void BeginFrame(const uint8_t *base, uint32_t player, double step)
  {
    const uint32_t frame = L32(base, kClockFrames);
    if (g_f.valid && g_f.frame == frame && g_f.player == player)
    {
      return;
    }
    if (g_pending && g_pending_player != player)
    {
      Line(base, g_pending_player, "ended early");
      ResetStats();
    }
    g_pending = true;
    g_pending_player = player;
    if (g_f.player != player)
    {
      g_f.prev_cut = false;
      g_f.eye_valid = false;
      g_f.run = 0;
      g_f.moved_before = false;
      g_f.hist_n = 0;
    }
    g_f.valid = true;
    g_f.frame = frame;
    g_f.player = player;
    g_f.block = L32(base, player + kCsBlock);
    g_f.curve = CurveMode();
    const bool first_block = g_f.block == 0;
    int cause = -1;
    uint32_t cut_k = 0;
    bool cross_cut = false, past_end = false;
    float cross_eye = -1.0f;

    // Type 1 objects: their own cuts only.
    g_f.n_objects = 0;
    const uint32_t setup = L32(base, player + kCsSetup);
    const uint32_t entries = L32(base, player + kCsEntries);
    const uint32_t n = setup && entries ? std::min<uint32_t>(L32(base, setup + 28), kMaxEntries) : 0;
    for (uint32_t i = 0; i < n && g_f.n_objects < kMaxObjects; ++i)
    {
      const uint32_t e = entries + i * kEntrySize;
      if (int32_t(L32(base, e + kEntryType)) != 1)
      {
        continue;
      }
      const uint32_t object = L32(base, e + kEntryObject);
      if (!object)
      {
        continue;
      }
      Object &o = g_f.objects[g_f.n_objects++];
      o = Object{};
      o.object = object;
      const uint32_t slot = FindSlot(base, L32(base, object + kObjAnim), kCutsceneSlot);
      if (!slot || L32(base, slot + kSlotSampler + kSmpInterp) != 0 || !L32(base, slot + kSlotSampler + kSmpAnim))
      {
        continue;
      }
      o.sampler = slot + kSlotSampler;
      Mapping m;
      o.regular = Predict(base, o.sampler, step, first_block, m, &o.tu);
      o.cut = o.regular && m.pos >= 0.0f && RootCut(base, m.anim, m.keys, IntervalAt(m));
    }
    UpdateMax(g_obj_max, g_f.n_objects);

    // Camera controls (type 2).
    const uint32_t count = std::min<uint32_t>(L32(base, player + kCsCtrlCount), kMaxControls);
    const uint32_t array = L32(base, player + kCsCtrlArray);
    g_f.cam0_sampler = 0;
    g_f.cam0_ctrl = 0;
    uint32_t lens = 0;
    for (uint32_t i = 0; array && i < count; ++i)
    {
      const uint32_t entry = L32(base, array + 4 * i);
      const uint32_t ctrl = entry ? L32(base, entry + kCtrlEntryCtrl) : 0;
      if (!ctrl)
      {
        continue;
      }
      const uint32_t layer = ControlLayer(base, ctrl);
      const uint32_t sampler = layer ? layer + kLayerSampler : 0;
      if (i == 0)
      {
        g_f.cam0_ctrl = ctrl;
        CheckView(base, ctrl, sampler, g_f.block);
      }
      if (!layer)
      {
        continue;
      }
      if (L32(base, layer + kLayerFocalLength) || L32(base, layer + kLayerFovY))
      {
        ++lens;
      }
      if (L32(base, sampler + kSmpInterp) != 0)
      {
        continue;
      }
      Mapping m;
      float tu = 0.0f, t = 0.0f;
      const float t_before = LF(base, sampler + kSmpTime);
      if (!Predict(base, sampler, step, first_block, m, &tu, &t))
      {
        continue;
      }
      if (i == 0)
      {
        g_f.cam0_sampler = sampler;
        g_f.cam0_ms = (float(g_f.block) + t) * LF(base, m.anim + kAnimLength) * 1000.0f;
        if (g_cam_keys.load(std::memory_order_relaxed) == 0)
        {
          g_cam_keys.store(m.keys, std::memory_order_relaxed);
          g_cam_length.store(LF(base, m.anim + kAnimLength), std::memory_order_relaxed);
        }
        if (g_f.cam0_ms >= g_next_interim_ms)
        {
          Line(base, player, "interim");
          g_next_interim_ms = (std::floor(g_f.cam0_ms / kInterimMs) + 1.0f) * kInterimMs;
        }
        // A block passed in one frame (Rare's picture time on a shorter last block, see 6): keep the last shown frame.
        if (!first_block && t_before == 0.0f && t >= 1.0f)
        {
          past_end = true;
        }
      }
      if (m.retimed && m.pos < 0.0f)
      {
        if (CameraCrossCut(base, sampler, m, i == 0 ? &cross_eye : nullptr))
        {
          cross_cut = true;
        }
      }
      else
      {
        const uint32_t k = IntervalAt(m);
        if (i == 0)
        {
          if (CameraCut(base, m.anim, m.keys, k))
          {
            cause = kHoldCamera;
            cut_k = k;
          }
          else if (g_ck.ready && g_ck.anim == m.anim && k + 1 < g_ck.keys && g_ck.in[k].scene_change &&
                   g_ck.counted_scene != k)
          {
            g_ck.counted_scene = k;
            g_holds[kSceneNoJump].fetch_add(1, std::memory_order_relaxed);
          }
        }
        else if (RootCut(base, m.anim, m.keys, k) && cause < 0)
        {
          cause = kHoldCameraOther;
        }
      }
    }
    UpdateMax(g_cam_controls, count);
    UpdateMax(g_cam_lens, lens);

    g_f.cut = cause >= 0;
    g_f.cross_cut = cross_cut;
    g_f.past_end = past_end;
    if (cause >= 0)
    {
      g_holds[cause].fetch_add(1, std::memory_order_relaxed);
    }
    if (cross_cut)
    {
      g_holds[kHoldCameraCross].fetch_add(1, std::memory_order_relaxed);
    }
    if (past_end)
    {
      g_past_end.fetch_add(1, std::memory_order_relaxed);
    }
    g_frames.fetch_add(1, std::memory_order_relaxed);
    const bool any = g_f.cut || cross_cut;
    if (any)
    {
      g_cut_frames.fetch_add(1, std::memory_order_relaxed);
      if (!g_f.prev_cut)
      {
        g_cuts.fetch_add(1, std::memory_order_relaxed);
        if (cross_cut)
        {
          NoteCut(0, true, cross_eye, "eye at the block switch");
        }
        else
        {
          NoteCut(cut_k, false, 0.0f, cause == kHoldCameraOther ? "another camera's root" : "root");
        }
      }
    }
    g_f.prev_cut = any;
  }

  // The continuous tracks' indices in the channel's float track buffer (kNoTrack = absent); the camera's scene ID and
  // use-look-at flag.
  void ContinuousTracks(const uint8_t *base, Kind kind, uint32_t sampler)
  {
    t_cont_n = 0;
    t_scene_index = -1;
    t_use_index = -1;
    const uint32_t *offsets = nullptr;
    uint32_t n = 0, owner = 0, buffer_at = 0;
    if (kind == Kind::kLayer)
    {
      offsets = kCameraContinuous;
      n = uint32_t(std::size(kCameraContinuous));
      owner = sampler - kLayerSampler;
      buffer_at = kLayerTracks;
    }
    else if (kind == Kind::kLight)
    {
      offsets = kLightContinuous;
      n = uint32_t(std::size(kLightContinuous));
      owner = sampler - kLightSampler;
      buffer_at = kLightTracks;
    }
    if (!n)
    {
      return;
    }
    const uint32_t buffer = L32(base, owner + buffer_at);
    const uint32_t tracks = L16(base, sampler + kSmpTrackCount);
    const auto index = [&](uint32_t offset) -> int32_t {
      const uint32_t p = L32(base, owner + offset);
      return buffer && p >= buffer && ((p - buffer) & 3) == 0 && (p - buffer) / 4 < tracks ? int32_t((p - buffer) / 4)
                                                                                         : -1;
    };
    for (uint32_t i = 0; i < n && i < kMaxContinuous; ++i)
    {
      const int32_t x = index(offsets[i]);
      t_cont[i] = x < 0 ? kNoTrack : uint32_t(x);
    }
    t_cont_n = std::min(n, kMaxContinuous);
    if (kind == Kind::kLayer)
    {
      t_scene_index = index(kLayerSceneId);
      t_use_index = index(kLayerUseLookAt);
    }
  }

  Object *CutsceneObject(const uint8_t *base, uint32_t object, double step)
  {
    const uint32_t player = PlayingCutscene(base);
    if (!player)
    {
      return nullptr;
    }
    BeginFrame(base, player, step);
    for (uint32_t i = 0; i < g_f.n_objects; ++i)
    {
      if (g_f.objects[i].object == object)
      {
        Object *o = &g_f.objects[i];
        return o->sampler && L32(base, o->sampler + kSmpInterp) == 0 ? o : nullptr;
      }
    }
    return nullptr;
  }

  void BeginSample(uint8_t *base, uint32_t sampler, bool camera, bool hold, float tu)
  {
    t_s = Sample{};
    t_s.on = true;
    t_s.sampler = sampler;
    t_s.hold = hold;
    t_s.cross_cut = g_f.cross_cut;
    t_s.past_end = g_f.past_end;
    t_s.retime = REXCVAR_GET(pdz_cutscene_retime);
    t_s.first_block = g_f.block == 0;
    t_s.tu = tu;
    t_s.camera = camera;
    t_s.curve = camera ? g_f.curve : kCurveLinear;
    t_cont_n = 0;
    t_scene_index = -1;
    t_use_index = -1;
    S32(base, sampler + kSmpInterp, 1);
    g_live.fetch_add(1, std::memory_order_relaxed);
  }

  void EndSample(uint8_t *base)
  {
    g_live.fetch_sub(1, std::memory_order_relaxed);
    S32(base, t_s.sampler + kSmpInterp, 0);
    if (t_s.mapped)
    {
      (t_s.retimed ? g_retimed : g_rare_mapping).fetch_add(1, std::memory_order_relaxed);
    }
    t_s.on = false;
  }

  // ---- Curves. ----
  float Tangent(float a, float b) { return a * b <= 0.0f ? 0.0f : 2.0f * a * b / (a + b); }

  float Hermite(float p1, float p2, float m1, float m2, float u)
  {
    const float u2 = u * u, u3 = u2 * u;
    return (2.0f * u3 - 3.0f * u2 + 1.0f) * p1 + (u3 - 2.0f * u2 + u) * m1 + (-2.0f * u3 + 3.0f * u2) * p2 +
           (u3 - u2) * m2;
  }

  float Monotone(float p0, float p1, float p2, float p3, bool has0, bool has3, float u)
  {
    const float d = p2 - p1;
    return Hermite(p1, p2, has0 ? Tangent(p1 - p0, d) : d, has3 ? Tangent(d, p3 - p2) : d, u);
  }

  void CatmullVector(const float *p0, const float *p1, const float *p2, const float *p3, bool has0, bool has3,
                     uint32_t dim, float u, float *out)
  {
    float m1[3], m2[3], seg = 0.0f, l0 = 0.0f, l3 = 0.0f, n1 = 0.0f, n2 = 0.0f;
    for (uint32_t i = 0; i < dim; ++i)
    {
      const float d = p2[i] - p1[i];
      seg += d * d;
      m1[i] = has0 ? 0.5f * (p2[i] - p0[i]) : d;
      m2[i] = has3 ? 0.5f * (p3[i] - p1[i]) : d;
      if (has0)
      {
        l0 += (p1[i] - p0[i]) * (p1[i] - p0[i]);
      }
      if (has3)
      {
        l3 += (p3[i] - p2[i]) * (p3[i] - p2[i]);
      }
      n1 += m1[i] * m1[i];
      n2 += m2[i] * m2[i];
    }
    seg = std::sqrt(seg);
    l0 = std::sqrt(l0);
    l3 = std::sqrt(l3);
    n1 = std::sqrt(n1);
    n2 = std::sqrt(n2);
    const float cap = kTangentCap * seg;
    const auto guard = [&](float neighbour, float norm, float *m) {
      if ((neighbour > kSpeedRatio * seg || neighbour * kSpeedRatio < seg) && norm > cap && norm > 0.0f)
      {
        for (uint32_t i = 0; i < dim; ++i)
        {
          m[i] *= cap / norm;
        }
      }
    };
    if (has0)
    {
      guard(l0, n1, m1);
    }
    if (has3)
    {
      guard(l3, n2, m2);
    }
    for (uint32_t i = 0; i < dim; ++i)
    {
      out[i] = Hermite(p1[i], p2[i], m1[i], m2[i], u);
    }
  }

  void CurveVector(Curve c, const float *p0, const float *p1, const float *p2, const float *p3, bool has0, bool has3,
                   uint32_t dim, float u, float *out)
  {
    if (c == kCurveCatmull)
    {
      CatmullVector(p0, p1, p2, p3, has0, has3, dim, u, out);
      return;
    }
    for (uint32_t i = 0; i < dim; ++i)
    {
      out[i] = c == kCurveMonotone ? Monotone(p0[i], p1[i], p2[i], p3[i], has0, has3, u)
                                   : p1[i] + (p2[i] - p1[i]) * u;
    }
  }

  // ---- The camera after sub_821EA3F0 (the world eye: the matrix translation it just set). ----
  void PushHistory(const float (&e)[3], bool cut, bool switch_frame, uint32_t k)
  {
    for (uint32_t i = 3; i > 0; --i)
    {
      g_f.hist[i] = g_f.hist[i - 1];
    }
    History &h = g_f.hist[0];
    std::memcpy(h.eye, e, 12);
    h.cut = cut;
    h.switch_frame = switch_frame;
    h.frame = g_frames.load(std::memory_order_relaxed);
    h.ms = g_f.cam0_ms;
    h.block = g_f.block;
    h.k = k;
    if (++g_f.hist_n < 4)
    {
      return;
    }
    // The middle frame [1]: its move against the frames either side.
    const float before = Dist(g_f.hist[3].eye, g_f.hist[2].eye), mid = Dist(g_f.hist[2].eye, g_f.hist[1].eye),
                after = Dist(g_f.hist[1].eye, g_f.hist[0].eye);
    const float avg = 0.5f * (before + after);
    if (before <= kMoveEps || after <= kMoveEps)
    {
      return;
    }
    const History &m = g_f.hist[1];
    if (mid < kStop * avg)
    {
      (m.switch_frame || g_f.hist[2].switch_frame ? g_stops_switch : g_stops_inside)
          .fetch_add(1, std::memory_order_relaxed);
    }
    else if (mid > kTeleport * avg && !m.cut && !g_f.hist[2].cut)
    {
      g_teleports.fetch_add(1, std::memory_order_relaxed);
      if (g_teleport_events_n < kEvents)
      {
        ++g_teleport_events_n;
        const Interval *in = g_ck.ready && g_ck.block == m.block && m.k + 1 < g_ck.keys ? &g_ck.in[m.k] : nullptr;
        g_teleport_events += fmt::format(
            "[frame {}, {:.0f} ms, block {}, keys {}-{}{}: moved {:.1f} cm, the frames either side {:.1f} / {:.1f}; the "
            "interval: eye {} cm, turn {} deg, look-at {} cm, judged {}] ",
            m.frame, m.ms, m.block, m.k, m.k + 1, m.switch_frame ? " (switch)" : "", mid, before, after,
            in ? fmt::format("{:.1f}", in->eye) : "?",
            in && in->turn >= 0.0f ? fmt::format("{:.1f}", in->turn * 57.2958f) : "-",
            in && in->look >= 0.0f ? fmt::format("{:.1f}", in->look) : "-", in ? WhyText(in->why) : "?");
      }
    }
  }

  void OffKeys(float d, const uint8_t *base, uint32_t sampler)
  {
    if (d <= kOffKeys)
    {
      return;
    }
    g_off_frames.fetch_add(1, std::memory_order_relaxed);
    if (d > g_off_worst.load(std::memory_order_relaxed))
    {
      (void)base;
      (void)sampler;
      g_off_worst.store(d, std::memory_order_relaxed);
      g_off_worst_frame.store(g_frames.load(std::memory_order_relaxed), std::memory_order_relaxed);
      g_off_worst_ms.store(g_f.cam0_ms, std::memory_order_relaxed);
    }
  }

  float BoxDistance(const float (&e)[3], const float *a, const float *b)
  {
    float d2 = 0.0f;
    for (uint32_t i = 0; i < 3; ++i)
    {
      const float lo = std::min(a[i], b[i]), hi = std::max(a[i], b[i]);
      const float o = e[i] < lo ? lo - e[i] : (e[i] > hi ? e[i] - hi : 0.0f);
      d2 += o * o;
    }
    return std::sqrt(d2);
  }

  // The previous block's kept frame (world) for interval 0's curve, when the switch into this block wasn't a cut.
  const Snap *ReachBack(uint32_t sampler)
  {
    const Snap *s = FindSnap(sampler, false);
    return t_s.retimed && s && s->world && s->player == g_f.player && s->block + 1 == g_f.block && !s->switch_cut
               ? s
               : nullptr;
  }

  void CameraAfter(uint8_t *base, uint32_t sampler, bool cam0)
  {
    const uint32_t matrix = L32(base, sampler + kSmpMatrix);
    Root r;
    const uint32_t anim = L32(base, sampler + kSmpAnim);
    if (!matrix || !t_s.mapped || !ReadRoot(base, anim, r))
    {
      g_linear_cam.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    float world[3], slot[3], clip[3], offset[3];
    LV(base, matrix + kMatrixTranslation, world);
    LV(base, sampler + kSmpRootPos, slot);
    ClipOffset(base, r, clip);
    float mismatch = 0.0f;
    for (uint32_t a = 0; a < 3; ++a)
    {
      offset[a] = world[a] - slot[a];
      mismatch = std::max(mismatch, std::fabs(offset[a] - clip[a]));
    }
    if (mismatch > kOffKeys)
    {
      g_space_mismatch.fetch_add(1, std::memory_order_relaxed);
    }
    const auto world_key = [&](uint32_t key, float (&p)[3]) {
      KeyPos(base, r, key, p);
      for (uint32_t a = 0; a < 3; ++a)
      {
        p[a] += offset[a];
      }
    };
    float want[3];
    std::memcpy(want, world, 12);
    uint32_t flags = t_s.record ? 64u : 0u;
    bool curved = false, box = false, all_axes = false;
    float box_a[3], box_b[3];
    Snap *s = t_s.snap;
    if (t_s.cross)
    {
      // The block's first part: the kept world eye -> key 0's (sampled now), or the kept one across a camera cut /
      // past the end; nothing kept: key 0.
      flags |= t_s.past_end ? 32u : 2u;
      const Snap *kept = s && s->world ? s : nullptr;
      if (kept)
      {
        const bool cut = t_s.cross_cut || t_s.cam_cross_cut || t_s.past_end;
        const float w = cut ? 0.0f : t_s.w;
        s->switch_cut = cut; // for interval 0's curve (ReachBack)
        for (uint32_t a = 0; a < 3; ++a)
        {
          want[a] = kept->eye[a] + (world[a] - kept->eye[a]) * w;
        }
        all_axes = true;
        if (cut)
        {
          flags |= 4;
        }
        if (cam0 && !t_s.past_end)
        {
          (cut ? g_cam_cross_kept : g_cam_cross_blend).fetch_add(1, std::memory_order_relaxed);
          Snap *ms = FindSnap(sampler, false);
          if (ms && !ms->switch_counted)
          {
            ms->switch_counted = true;
            (cut ? g_switch_cut : g_switch_blended).fetch_add(1, std::memory_order_relaxed);
            if (g_switch_n < kSwitchDistances)
            {
              g_switch_distance[g_switch_n++] = Dist(kept->eye, world);
            }
          }
        }
        std::memcpy(box_a, kept->eye, 12);
        std::memcpy(box_b, world, 12);
        box = true;
      }
      else if (cam0)
      {
        g_cam_cross_key0.fetch_add(1, std::memory_order_relaxed);
      }
    }
    else if (t_s.hold || t_s.late_cut)
    {
      world_key(t_s.discrete, want);
      flags |= t_s.late_cut ? 8u : 1u;
    }
    else
    {
      float p[4][3] = {};
      world_key(t_s.k, p[1]);
      world_key(t_s.k + 1, p[2]);
      if (t_s.curve != kCurveLinear)
      {
        bool has0 = false, has3 = false;
        if (t_s.k > 0 && !CameraCut(base, anim, t_s.keys, t_s.k - 1))
        {
          world_key(t_s.k - 1, p[0]);
          has0 = true;
        }
        else if (t_s.k == 0)
        {
          // Interval 0: the previous block's kept frame (world), when the switch wasn't a cut, brought to one key's
          // spacing before key 0 (it was shown span keys before it), so the curve leaves key 0 along the motion that
          // reached it instead of with a one-sided tangent.
          if (const Snap *b = ReachBack(sampler))
          {
            const float span = std::max(1.0f, -b->kept_at);
            for (uint32_t a = 0; a < 3; ++a)
            {
              p[0][a] = p[1][a] - (p[1][a] - b->eye[a]) / span;
            }
            has0 = true;
          }
        }
        if (t_s.k + 2 < t_s.keys && !CameraCut(base, anim, t_s.keys, t_s.k + 1))
        {
          world_key(t_s.k + 2, p[3]);
          has3 = true;
        }
        CurveVector(t_s.curve, p[0], p[1], p[2], p[3], has0, has3, 3, t_s.u, want);
        curved = true;
        flags |= 16;
      }
      std::memcpy(box_a, p[1], 12);
      std::memcpy(box_b, p[2], 12);
      box = true;
    }
    for (uint32_t a = 0; a < 3; ++a)
    {
      if (r.has[a] || all_axes)
      {
        SF(base, matrix + kMatrixTranslation + 4 * a, want[a]);
      }
      else
      {
        want[a] = world[a];
      }
    }
    // The frame shown at the block's switch, in world space, for the next block.
    if (t_s.record && !t_s.past_end)
    {
      Snap *k = FindSnap(sampler, true);
      k->player = g_f.player;
      k->block = g_f.block;
      k->kept_at = t_s.pos - float(t_s.frames);
      k->world = true;
      std::memcpy(k->eye, want, 12);
      k->switch_counted = false;
      k->switch_cut = false;
    }
    (curved ? g_curve_eye : g_linear_cam).fetch_add(1, std::memory_order_relaxed);
    if (!cam0)
    {
      return;
    }
    if (box)
    {
      OffKeys(BoxDistance(want, box_a, box_b), base, sampler);
    }
    PushHistory(want, g_f.cut || g_f.cross_cut || t_s.late_cut || t_s.cam_cross_cut, t_s.record || t_s.cross,
                t_s.k);
    RingEntry &e = g_ring[g_ring_n++ % kRing];
    e = RingEntry{};
    e.frame = g_frames.load(std::memory_order_relaxed);
    e.block = g_f.block;
    e.key = t_s.discrete;
    e.u = t_s.cross ? t_s.pos : t_s.u;
    e.flags = flags;
    std::memcpy(e.eye, want, 12);
    NoteDumpMapping(sampler, L32(base, kClockFrames), g_f.block, t_s.pos, t_s.k, t_s.u, t_s.discrete, flags);
  }
} // namespace

// ---- The per-frame cutscene dump (pdz_cutscene_frame_dump; cutscene_dump.h). Read only: it changes nothing in the
// game or in the smoothing above. One row per game frame (closed at the frame's Swap, shadow_hooks.cpp) from the
// cutscene's first frame to kDumpAfter frames after it stopped, buffered and written as logs\cutscene_<log number>_
// <cutscene player>_<n>.csv on a writer thread when it ends (or at a new cutscene, the row cap, or process exit). ----
namespace
{
  constexpr uint32_t kDumpAfter = 30;
  constexpr uint32_t kDumpRows = 20000;
  constexpr uint32_t kDumpPresents = 8192; // the present ring (native Swap number -> time)
  constexpr uint32_t kVblankCount = 0x82D3DB00; // the game's vblank callback block +0 (game_pace.cpp)
  constexpr uint32_t kPlayerOne = 0x82E3C3E4;   // player 1 (Jo in single player; native_present.cpp's mark line)
  constexpr uint32_t kObjPosition = 48;         // vec3, cm (the player's position, native_present.cpp; objects alike)
  constexpr uint32_t kCtrlUp = 984;             // sub_8220C3F8: the view's up
  constexpr uint32_t kDumpObjects = 3;
  constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

  struct DumpRow
  {
    uint64_t frame = 0, swap = 0;
    double end_ms = 0.0, present_ms = -1.0;
    uint32_t vblank = 0;
    bool after = false;
    double step_s = -1.0, clock_ms = -1.0, picture_ms = -1.0, sound_ms = -1.0;
    bool mapped = false;
    uint32_t block = 0, k = 0, key = 0, flags = 0;
    float pos = 0.0f, u = 0.0f;
    float our_eye[3] = {kNaN, kNaN, kNaN}, rare_eye[3] = {kNaN, kNaN, kNaN};
    float view_eye[3] = {kNaN, kNaN, kNaN}, view_target[3] = {kNaN, kNaN, kNaN}, view_up[3] = {kNaN, kNaN, kNaN};
    bool matrix = false;
    float m[16] = {};
    float focal = kNaN, fov_y = kNaN;
    float obj[kDumpObjects][3] = {{kNaN, kNaN, kNaN}, {kNaN, kNaN, kNaN}, {kNaN, kNaN, kNaN}};
    float jo[3] = {kNaN, kNaN, kNaN};
    float bright_avg = kNaN, bright_max = kNaN; // pdz_cutscene_black_detect (filled by WriteDump)
    int64_t native_draws = -1;                  // ... and the native renderer's draws that frame
  };

  struct DumpPresent
  {
    std::atomic<uint64_t> swap{0};
    std::atomic<int64_t> us{0};
    // pdz_cutscene_black_detect: the front buffer's brightness for this Swap (luma 0-255), valid when bright_swap ==
    // the row's Swap.
    std::atomic<uint64_t> bright_swap{0};
    std::atomic<float> bright_avg{0.0f}, bright_max{0.0f};
    std::atomic<uint32_t> draws{0};
  };

  std::atomic<bool> g_dump_collecting{false}; // the taps' test
  uint64_t g_dump_swaps = 0;                  // every Swap since start (game thread)
  DumpPresent g_dump_presents[kDumpPresents];
  std::vector<DumpRow> g_dump_rows;
  uint32_t g_dump_player = 0, g_dump_n = 0, g_dump_after_left = 0;
  bool g_dump_in_after = false;
  uint64_t g_dump_step_calls = 0;
  // Within the frame (game thread): the camera's mapping (CameraAfter) and the shadow setup's camera matrix.
  uint32_t g_dump_map_frame = ~0u, g_dump_cam_frame = ~0u, g_dump_map_sampler = 0;
  // Presents are noted up to this Swap number (all while collecting; the next ones' presents after it ends).
  std::atomic<uint64_t> g_dump_present_until{0};
  DumpRow g_dump_map;
  float g_dump_cam[16] = {};
  // What the after-frames still read: the first camera control, the first type 1 objects.
  uint32_t g_dump_ctrl = 0;
  uint32_t g_dump_objects[kDumpObjects] = {};
  const auto g_dump_epoch = std::chrono::steady_clock::now();

  double DumpNowMs()
  {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_dump_epoch).count();
  }

  void NoteDumpMapping(uint32_t sampler, uint32_t frame, uint32_t block, float pos, uint32_t k, float u, uint32_t key,
                       uint32_t flags)
  {
    if (!g_dump_collecting.load(std::memory_order_relaxed))
    {
      return;
    }
    g_dump_map_sampler = sampler;
    g_dump_map_frame = frame;
    g_dump_map.mapped = true;
    g_dump_map.block = block;
    g_dump_map.pos = pos;
    g_dump_map.k = k;
    g_dump_map.u = u;
    g_dump_map.key = key;
    g_dump_map.flags = flags;
  }

  std::string DumpFlags(uint32_t f)
  {
    std::string s;
    const std::pair<uint32_t, const char *> names[] = {{2, "cross"},    {4, "cross_cut"}, {1, "hold"},
                                                       {64, "record"},  {8, "late_cut"},  {16, "curve"},
                                                       {32, "past_end"}};
    for (const auto &[bit, name] : names)
    {
      if (f & bit)
      {
        s += s.empty() ? "" : "+";
        s += name;
      }
    }
    return s;
  }

  // Where the dump goes (log 290: the relative "logs/" missed: the working directory isn't the build folder): the
  // folder of the run's log file, from the SDK's file sink (rex::CurrentLogFilePath), else the exe's folder + logs
  // (rex_app.cpp's log_dir). The run tag: that log's number (perfectdarkzerorecomp_290.log -> "290"), else the run's
  // start date-time ("20261030_112335"), never 0.
  std::filesystem::path DumpFolder()
  {
    const std::filesystem::path log = rex::CurrentLogFilePath();
    if (!log.empty() && log.has_parent_path())
    {
      return log.parent_path();
    }
    wchar_t exe[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (len > 0 && len < MAX_PATH)
    {
      return std::filesystem::path(exe).parent_path() / L"logs";
    }
    std::error_code ec;
    return std::filesystem::current_path(ec) / L"logs";
  }

  std::string RunTag()
  {
    static const std::string tag = [] {
      const std::string stem = rex::CurrentLogFilePath().stem().string();
      const size_t underscore = stem.rfind('_');
      if (underscore != std::string::npos && underscore + 1 < stem.size() &&
          stem.find_first_not_of("0123456789", underscore + 1) == std::string::npos)
      {
        const int number = std::atoi(stem.c_str() + underscore + 1);
        if (number > 0)
        {
          return std::to_string(number);
        }
      }
      const std::time_t now = std::time(nullptr);
      std::tm local{};
      localtime_s(&local, &now);
      char buf[32];
      std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &local);
      return std::string(buf);
    }();
    return tag;
  }

  std::string Num(double v, int decimals)
  {
    return std::isfinite(v) ? fmt::format("{:.{}f}", v, decimals) : std::string();
  }

  // The CSV and its summary line (writer thread, or the caller at process exit).
  void WriteDump(std::vector<DumpRow> rows, uint32_t player, uint32_t n, bool log)
  {
    if (rows.empty())
    {
      return;
    }
    // The presents of the rows' Swaps (the ring holds the latest kDumpPresents).
    for (DumpRow &r : rows)
    {
      const DumpPresent &p = g_dump_presents[r.swap % kDumpPresents];
      if (p.swap.load(std::memory_order_acquire) == r.swap)
      {
        r.present_ms = double(p.us.load(std::memory_order_relaxed)) * 1e-3;
      }
      if (p.bright_swap.load(std::memory_order_acquire) == r.swap)
      {
        r.bright_avg = p.bright_avg.load(std::memory_order_relaxed);
        r.bright_max = p.bright_max.load(std::memory_order_relaxed);
        r.native_draws = p.draws.load(std::memory_order_relaxed);
      }
    }
    // pdz_cutscene_black_detect: a dark flash = a frame whose average brightness is under a quarter of the median of
    // its 8 neighbours either side (which average at least 12 of 255), or an all-but-black frame (brightest under 8)
    // between brighter ones; the camera matrix is valid when finite with unit-length, orthogonal rows 0-2.
    std::vector<uint8_t> dark(rows.size(), 0), matrix_bad(rows.size(), 0);
    std::string dark_rows;
    uint32_t dark_count = 0, sampled = 0, bad_matrices = 0;
    for (uint32_t i = 0; i < rows.size(); ++i)
    {
      const DumpRow &r = rows[i];
      if (r.matrix)
      {
        bool ok = true;
        for (uint32_t m = 0; m < 16; ++m)
        {
          ok = ok && std::isfinite(r.m[m]);
        }
        for (uint32_t a = 0; ok && a < 3; ++a)
        {
          const float *ra = r.m + 4 * a;
          const float len = std::sqrt(ra[0] * ra[0] + ra[1] * ra[1] + ra[2] * ra[2]);
          ok = len > 0.9f && len < 1.1f;
          for (uint32_t b = a + 1; ok && b < 3; ++b)
          {
            const float *rb = r.m + 4 * b;
            ok = std::fabs(ra[0] * rb[0] + ra[1] * rb[1] + ra[2] * rb[2]) < 0.1f;
          }
        }
        if (!ok)
        {
          matrix_bad[i] = 1;
          ++bad_matrices;
        }
      }
      if (!std::isfinite(r.bright_avg))
      {
        continue;
      }
      ++sampled;
      std::vector<float> around;
      for (uint32_t j = i >= 8 ? i - 8 : 0; j < std::min<uint32_t>(uint32_t(rows.size()), i + 9); ++j)
      {
        if (j != i && std::isfinite(rows[j].bright_avg))
        {
          around.push_back(rows[j].bright_avg);
        }
      }
      if (around.size() < 4)
      {
        continue;
      }
      std::nth_element(around.begin(), around.begin() + around.size() / 2, around.end());
      const float median = around[around.size() / 2];
      if ((median >= 12.0f && r.bright_avg < 0.25f * median) || (r.bright_max < 8.0f && median >= 12.0f))
      {
        dark[i] = 1;
        ++dark_count;
        if (dark_rows.size() < 400)
        {
          dark_rows += fmt::format("{}row {} (frame {}, swap {}, block {} pos {:.2f}, avg {:.1f} vs {:.1f})",
                                   dark_rows.empty() ? "" : "; ", i, r.frame, r.swap, r.block, r.pos, r.bright_avg,
                                   median);
        }
      }
    }
    const std::filesystem::path folder = DumpFolder();
    const std::filesystem::path file = folder / fmt::format("cutscene_{}_{:08X}_{}.csv", RunTag(), player, n);
    const std::string path = file.string();
    std::error_code dir_ec;
    std::filesystem::create_directories(folder, dir_ec);
    FILE *f = _wfopen(file.c_str(), L"w");
    if (!f)
    {
      const int err = errno;
      if (log)
      {
        char text[128] = {};
        strerror_s(text, sizeof(text), err);
        REXLOG_WARN("pdz_cutscene_smooth dump: couldn't write {} (errno {}: {}{})", path, err, text,
                    dir_ec ? fmt::format("; creating the folder failed: {}", dir_ec.message()) : std::string());
      }
      return;
    }
    std::string head = "row,frame,swap,present_ms,frame_end_ms,vblank,phase,step_s,clock_ms,picture_ms,sound_ms,block,"
                       "pos,k,u,key,flags,our_eye_x,our_eye_y,our_eye_z,rare_eye_x,rare_eye_y,rare_eye_z,view_eye_x,"
                       "view_eye_y,view_eye_z,view_target_x,view_target_y,view_target_z,view_up_x,view_up_y,view_up_z";
    for (uint32_t i = 0; i < 16; ++i)
    {
      head += fmt::format(",m{}{}", i / 4, i % 4);
    }
    head += ",focal_length,fov_y";
    for (uint32_t o = 0; o < kDumpObjects; ++o)
    {
      head += fmt::format(",obj{0}_x,obj{0}_y,obj{0}_z", o + 1);
    }
    head += ",jo_x,jo_y,jo_z,view_repeat,game_frames_this_present,bright_avg,bright_max,dark,matrix_bad,native_draws\n";
    std::fputs(head.c_str(), f);
    uint32_t repeats = 0, repeat_run = 0, repeat_longest = 0, never = 0, presents = 0;
    float worst_step = 0.0f;
    uint64_t worst_frame = 0;
    uint32_t worst_row = 0;
    uint64_t last_presented_swap = 0;
    for (uint32_t i = 0; i < rows.size(); ++i)
    {
      const DumpRow &r = rows[i];
      // The view this frame was rendered with: the shadow setup's camera matrix, else the control's eye / target.
      bool repeat = false;
      if (i > 0)
      {
        const DumpRow &q = rows[i - 1];
        if (r.matrix && q.matrix)
        {
          repeat = std::memcmp(r.m, q.m, sizeof(r.m)) == 0;
        }
        else
        {
          repeat = std::memcmp(r.view_eye, q.view_eye, 12) == 0 && std::memcmp(r.view_target, q.view_target, 12) == 0;
        }
        if (!r.after && !q.after && std::isfinite(r.our_eye[0]) && std::isfinite(q.our_eye[0]))
        {
          const float step = Dist(q.our_eye, r.our_eye);
          if (step > worst_step)
          {
            worst_step = step;
            worst_frame = r.frame;
            worst_row = i;
          }
        }
      }
      repeats += repeat ? 1 : 0;
      repeat_run = repeat ? repeat_run + 1 : 0;
      repeat_longest = std::max(repeat_longest, repeat_run);
      std::string frames_this_present;
      if (r.present_ms >= 0.0)
      {
        ++presents;
        frames_this_present = last_presented_swap ? fmt::format("{}", r.swap - last_presented_swap) : "1";
        last_presented_swap = r.swap;
      }
      else
      {
        ++never;
      }
      std::string line = fmt::format("{},{},{},{},{},{},{},{},{},{},{},", i, r.frame, r.swap, Num(r.present_ms, 3),
                                     Num(r.end_ms, 3), r.vblank, r.after ? "after" : "cutscene",
                                     r.step_s >= 0.0 ? Num(r.step_s, 6) : "", r.clock_ms >= 0.0 ? Num(r.clock_ms, 0) : "",
                                     r.picture_ms >= 0.0 ? Num(r.picture_ms, 0) : "",
                                     r.sound_ms >= 0.0 ? Num(r.sound_ms, 2) : "");
      line += r.mapped ? fmt::format("{},{:.4f},{},{:.4f},{},{}", r.block, r.pos, r.k, r.u, r.key, DumpFlags(r.flags))
                       : std::string(",,,,,");
      const auto v3 = [&](const float *v) { return fmt::format(",{},{},{}", Num(v[0], 2), Num(v[1], 2), Num(v[2], 2)); };
      line += v3(r.our_eye) + v3(r.rare_eye) + v3(r.view_eye) + v3(r.view_target) + v3(r.view_up);
      for (uint32_t m = 0; m < 16; ++m)
      {
        line += "," + (r.matrix ? Num(r.m[m], 4) : std::string());
      }
      line += "," + Num(r.focal, 3) + "," + Num(r.fov_y, 3);
      for (uint32_t o = 0; o < kDumpObjects; ++o)
      {
        line += v3(r.obj[o]);
      }
      line += v3(r.jo);
      line += fmt::format(",{},{},{},{},{},{},{}\n", i > 0 ? (repeat ? 1 : 0) : 0, frames_this_present,
                          Num(r.bright_avg, 1), Num(r.bright_max, 1), dark[i], matrix_bad[i],
                          r.native_draws >= 0 ? fmt::format("{}", r.native_draws) : std::string());
      std::fputs(line.c_str(), f);
    }
    std::fclose(f);
    if (log)
    {
      REXLOG_INFO("pdz_cutscene_smooth dump written: {} ({} rows)", path, rows.size());
      REXLOG_INFO("pdz_cutscene_smooth dump: {}: {} rows (cutscene {:08X}, frames {}-{}); view repeats {} (longest run "
                  "{}); presents with 0 game frames: not observable at the presenter hand-off (one per Swap; the "
                  "display's own repeats / drops are in the SDK's frame_pacing_log); game frames never handed to the "
                  "presenter {} ({} handed); largest frame-to-frame eye step {:.1f} cm at frame {} (row {})",
                  path, rows.size(), player, rows.front().frame, rows.back().frame, repeats, repeat_longest, never,
                  presents, double(worst_step), worst_frame, worst_row);
      REXLOG_INFO("pdz_cutscene_smooth dump: {}: dark flashes {} of {} sampled frames{}{}; camera matrices invalid {}",
                  path, dark_count, sampled, sampled ? "" : " (pdz_cutscene_black_detect off: not sampled)",
                  dark_rows.empty() ? std::string() : ": " + dark_rows, bad_matrices);
    }
  }

  void FinishDump(bool at_exit)
  {
    g_dump_collecting.store(false, std::memory_order_relaxed);
    g_dump_present_until.store(g_dump_swaps + 120, std::memory_order_relaxed); // the last rows' presents still land
    if (g_dump_rows.empty())
    {
      return;
    }
    std::vector<DumpRow> rows = std::move(g_dump_rows);
    g_dump_rows = {};
    const uint32_t player = g_dump_player, n = g_dump_n;
    if (at_exit)
    {
      WriteDump(std::move(rows), player, n, false);
      return;
    }
    // A worker writes it: no disk I/O on the game thread. The presents of the last after-frames land meanwhile.
    std::thread([rows = std::move(rows), player, n]() mutable {
      std::this_thread::sleep_for(std::chrono::milliseconds(500)); // the frames in flight reach the presenter
      WriteDump(std::move(rows), player, n, true);
    }).detach();
  }

  void StartDump(uint32_t player)
  {
    g_dump_rows.clear();
    g_dump_rows.reserve(4096);
    g_dump_player = player;
    ++g_dump_n;
    g_dump_after_left = 0;
    g_dump_in_after = false;
    g_dump_map_frame = ~0u;
    g_dump_cam_frame = ~0u;
    g_dump_ctrl = 0;
    std::fill(std::begin(g_dump_objects), std::end(g_dump_objects), 0u);
    g_dump_present_until.store(~uint64_t(0), std::memory_order_relaxed);
    g_dump_collecting.store(true, std::memory_order_relaxed);
  }

  // Process exit (static destruction): a cutscene still being collected is written (no log line then).
  struct DumpAtExit
  {
    ~DumpAtExit()
    {
      if (g_dump_collecting.load(std::memory_order_relaxed))
      {
        FinishDump(true);
      }
    }
  } g_dump_at_exit;

  // The row of the frame the Swap closes (game thread).
  void DumpRowNow(const uint8_t *base, uint32_t player)
  {
    DumpRow r;
    r.frame = L32(base, kClockFrames);
    r.swap = g_dump_swaps;
    r.end_ms = DumpNowMs();
    r.vblank = L32(base, kVblankCount);
    // The frame Finished ran in (state no longer 3 at its Swap) still sampled the camera: a cutscene row.
    const bool sampled_now = g_dump_map_frame == r.frame;
    r.after = player == 0 && !sampled_now;
    uint64_t calls = 0;
    uint32_t picture = 0, clock = 0;
    double step = 0.0, sound = 0.0;
    if (!r.after && pdz::pace::LastCutsceneStep(calls, picture, clock, step, sound) && calls != g_dump_step_calls)
    {
      g_dump_step_calls = calls;
      r.step_s = step;
      r.clock_ms = clock;
      r.picture_ms = picture;
      r.sound_ms = sound;
    }
    if (g_dump_map_frame == r.frame)
    {
      r.mapped = true;
      r.block = g_dump_map.block;
      r.pos = g_dump_map.pos;
      r.k = g_dump_map.k;
      r.u = g_dump_map.u;
      r.key = g_dump_map.key;
      r.flags = g_dump_map.flags;
    }
    if (g_dump_cam_frame == r.frame)
    {
      r.matrix = true;
      std::memcpy(r.m, g_dump_cam, sizeof(r.m));
    }
    if (!r.after)
    {
      // The cutscene's first camera and objects, as this frame's BeginFrame found them.
      const bool live = player && g_f.player == player;
      g_dump_ctrl = live && g_f.cam0_ctrl ? g_f.cam0_ctrl : g_dump_ctrl;
      for (uint32_t o = 0; o < kDumpObjects && o < g_f.n_objects && live; ++o)
      {
        g_dump_objects[o] = g_f.objects[o].object;
      }
      const uint32_t sampler = sampled_now ? g_dump_map_sampler : live ? g_f.cam0_sampler : 0;
      Root rt;
      const uint32_t anim = sampler ? L32(base, sampler + kSmpAnim) : 0;
      if (anim && ReadRoot(base, anim, rt))
      {
        // Our eye: the matrix translation the view is built from (sub_821EA3F0, then CameraAfter when smoothing).
        if (const uint32_t matrix = L32(base, sampler + kSmpMatrix))
        {
          float e[3];
          LV(base, matrix + kMatrixTranslation, e);
          std::memcpy(r.our_eye, e, 12);
        }
        // Rare's eye without smoothing: its own key (sub_82720D90: key = t x (frames - 1), truncated) + the clip's
        // root offset, as sub_821EA3F0 places it.
        const uint32_t frames = L16(base, anim + kAnimKeys);
        if (frames >= 1)
        {
          const float t = LF(base, sampler + kSmpTime);
          const uint32_t key = std::min<uint32_t>(uint32_t(std::max(0.0f, t * float(frames - 1))), frames - 1);
          float p[3], o[3];
          KeyPos(base, rt, key, p);
          ClipOffset(base, rt, o);
          for (uint32_t a = 0; a < 3; ++a)
          {
            r.rare_eye[a] = p[a] + o[a];
          }
        }
        // The lens as used: the layer's focal length / FovY track values (822132B0 reads them after the sample).
        const uint32_t layer = sampler - kLayerSampler;
        if (const uint32_t p = L32(base, layer + kLayerFocalLength))
        {
          r.focal = LF(base, p);
        }
        if (const uint32_t p = L32(base, layer + kLayerFovY))
        {
          r.fov_y = LF(base, p);
        }
      }
    }
    if (g_dump_ctrl)
    {
      float v[3];
      LV(base, g_dump_ctrl + kCtrlEye, v);
      std::memcpy(r.view_eye, v, 12);
      LV(base, g_dump_ctrl + kCtrlTarget, v);
      std::memcpy(r.view_target, v, 12);
      LV(base, g_dump_ctrl + kCtrlUp, v);
      std::memcpy(r.view_up, v, 12);
    }
    for (uint32_t o = 0; o < kDumpObjects; ++o)
    {
      if (g_dump_objects[o])
      {
        float v[3];
        LV(base, g_dump_objects[o] + kObjPosition, v);
        std::memcpy(r.obj[o], v, 12);
      }
    }
    if (const uint32_t jo = L32(base, kPlayerOne))
    {
      float v[3];
      LV(base, jo + kObjPosition, v);
      std::memcpy(r.jo, v, 12);
    }
    g_dump_rows.push_back(r);
  }
} // namespace

namespace pdz::csdump
{
  void OnSwap(uint8_t *base)
  {
    ++g_dump_swaps; // the native renderer's g_present.swaps counts the same Swaps (native_present.cpp OnSwap)
    const bool collecting = g_dump_collecting.load(std::memory_order_relaxed);
    if (!REXCVAR_GET(pdz_cutscene_smooth))
    {
      if (collecting)
      {
        FinishDump(false); // switched off during a cutscene: write what was collected
      }
      return; // off: no dump
    }
    if (!collecting && !REXCVAR_GET(pdz_cutscene_frame_dump))
    {
      return;
    }
    const uint32_t player = PlayingCutscene(base);
    if (collecting && player && player != g_dump_player)
    {
      FinishDump(false); // a new cutscene within the after-frames
    }
    if (!g_dump_collecting.load(std::memory_order_relaxed))
    {
      if (!player || !REXCVAR_GET(pdz_cutscene_frame_dump))
      {
        return;
      }
      StartDump(player);
    }
    if (!player)
    {
      if (!g_dump_in_after)
      {
        g_dump_in_after = true;
        g_dump_after_left = kDumpAfter;
      }
      if (g_dump_after_left == 0)
      {
        FinishDump(false);
        return;
      }
      --g_dump_after_left;
    }
    else if (g_dump_in_after)
    {
      g_dump_in_after = false; // the same cutscene again (it paused / resumed state 3)
    }
    DumpRowNow(base, player);
    if (g_dump_rows.size() >= kDumpRows)
    {
      FinishDump(false);
    }
  }

  void OnPresent(uint64_t swap)
  {
    if (swap > g_dump_present_until.load(std::memory_order_relaxed))
    {
      return; // the dump isn't collecting (0 while it never ran)
    }
    DumpPresent &p = g_dump_presents[swap % kDumpPresents];
    p.us.store(int64_t(DumpNowMs() * 1000.0), std::memory_order_relaxed);
    p.swap.store(swap, std::memory_order_release);
  }

  void OnShadowCamera(uint32_t matrix)
  {
    if (!g_dump_collecting.load(std::memory_order_relaxed) || !matrix)
    {
      return;
    }
    const uint8_t *base = rex::Runtime::instance()->virtual_membase();
    for (uint32_t i = 0; i < 16; ++i)
    {
      g_dump_cam[i] = LF(base, matrix + 4 * i);
    }
    g_dump_cam_frame = L32(base, kClockFrames);
  }

  void BeforeSwap(uint8_t *base)
  {
    if (!REXCVAR_GET(pdz_cutscene_smooth))
    {
      g_swap_player = 0;
      g_skip_swap.store(0, std::memory_order_relaxed); // off: no frame is repeated
      return;
    }
    const uint32_t player = g_pending ? PlayingCutscene(base) : 0;
    if (!player)
    {
      g_swap_player = 0;
      return;
    }
    const uint64_t swap = g_dump_swaps + 1; // this Swap's number (OnSwap counts it after the guest's Swap)
    g_cs_swap_ring[swap % kCsSwapRing].store(swap, std::memory_order_release);
    uint64_t calls = 0;
    uint32_t picture = 0, clock = 0;
    double step = 0.0, sound = 0.0;
    pdz::pace::LastCutsceneStep(calls, picture, clock, step, sound);
    const uint32_t block = L32(base, player + kCsBlock);
    const bool same = g_swap_player == player;
    if (same && calls == g_swap_calls)
    {
      // No cutscene update since the previous Swap: the game didn't draw this frame (log 292).
      g_no_update_presents.fetch_add(1, std::memory_order_relaxed);
      if (block != g_swap_block)
      {
        g_no_update_switch.fetch_add(1, std::memory_order_relaxed);
      }
      if (REXCVAR_GET(pdz_cutscene_repeat_frame) && g_swap_updated_run >= 2)
      {
        g_skip_swap.store(swap, std::memory_order_release);
        g_presents_repeated.fetch_add(1, std::memory_order_relaxed);
      }
      g_swap_updated_run = 0;
    }
    else
    {
      g_swap_updated_run = same ? g_swap_updated_run + 1 : 0;
    }
    g_swap_calls = calls;
    g_swap_block = block;
    g_swap_player = player;
  }

  bool SkipPresent(uint64_t swap) { return swap != 0 && g_skip_swap.load(std::memory_order_acquire) == swap; }

  bool CutsceneSwap(uint64_t swap)
  {
    return swap != 0 && REXCVAR_GET(pdz_cutscene_smooth) && REXCVAR_GET(pdz_cutscene_repeat_frame) &&
           g_cs_swap_ring[swap % kCsSwapRing].load(std::memory_order_acquire) == swap;
  }

  void NoteEmptyRepeat() { g_empty_repeated.fetch_add(1, std::memory_order_relaxed); }

  bool WantBrightness(uint64_t swap)
  {
    return REXCVAR_GET(pdz_cutscene_black_detect) && swap != 0 &&
           swap <= g_dump_present_until.load(std::memory_order_relaxed);
  }

  void OnBrightness(uint64_t swap, float average, float brightest, uint32_t draws)
  {
    DumpPresent &p = g_dump_presents[swap % kDumpPresents];
    p.draws.store(draws, std::memory_order_relaxed);
    p.bright_avg.store(average, std::memory_order_relaxed);
    p.bright_max.store(brightest, std::memory_order_relaxed);
    p.bright_swap.store(swap, std::memory_order_release);
  }
} // namespace pdz::csdump

// The cutscene channels' sampler advance (sampler r3, step f1, float track buffer r5).
REX_HOOK_RAW(sub_821EA3F0)
{
  if (!REXCVAR_GET(pdz_cutscene_smooth))
  {
    __imp__sub_821EA3F0(ctx, base); // off: Rare's code, nothing else
    return;
  }
  const Kind kind = KindOf(uint32_t(ctx.lr));
  if (kind == Kind::kNone)
  {
    __imp__sub_821EA3F0(ctx, base);
    return;
  }
  const uint32_t sampler = ctx.r3.u32;
  const uint32_t player = PlayingCutscene(base);
  if (!player || L32(base, sampler + kSmpInterp) != 0 || L32(base, sampler + kSmpPlayMode) != 1)
  {
    __imp__sub_821EA3F0(ctx, base);
    return;
  }
  Count &count = kind == Kind::kLayer ? g_cam : g_other;
  count.seen.fetch_add(1, std::memory_order_relaxed);
  if (!REXCVAR_GET(pdz_cutscene_smooth))
  {
    __imp__sub_821EA3F0(ctx, base);
    return;
  }
  const double step = ctx.f1.f64;
  BeginFrame(base, player, step);
  const bool cam0 = kind == Kind::kLayer && sampler == g_f.cam0_sampler;
  if (cam0)
  {
    g_cam0.seen.fetch_add(1, std::memory_order_relaxed);
  }
  Mapping m;
  float tu = 0.0f;
  if (!Predict(base, sampler, step, g_f.block == 0, m, &tu))
  {
    count.irregular.fetch_add(1, std::memory_order_relaxed);
    if (cam0)
    {
      g_cam0.irregular.fetch_add(1, std::memory_order_relaxed);
    }
    __imp__sub_821EA3F0(ctx, base);
    return;
  }
  const bool held = g_f.cut;
  (held ? count.held : count.smoothed).fetch_add(1, std::memory_order_relaxed);
  if (cam0)
  {
    (held ? g_cam0.held : g_cam0.smoothed).fetch_add(1, std::memory_order_relaxed);
  }
  BeginSample(base, sampler, kind == Kind::kLayer, held, tu);
  ContinuousTracks(base, kind, sampler);
  __imp__sub_821EA3F0(ctx, base);
  if (kind == Kind::kLayer)
  {
    CameraAfter(base, sampler, cam0);
  }
  EndSample(base);
}

// Type 1 objects, slots' time and root (r3 = the object, f1 = the step). The root stays Rare's at a block's first part
// (key 0): its values are relative to the block's clip.
REX_HOOK_RAW(sub_821E8F70)
{
  if (!REXCVAR_GET(pdz_cutscene_smooth))
  {
    __imp__sub_821E8F70(ctx, base); // off: Rare's code (no BeginFrame: it armed the final-frame skip, log 294)
    return;
  }
  Object *o = CutsceneObject(base, ctx.r3.u32, ctx.f1.f64);
  if (!o)
  {
    __imp__sub_821E8F70(ctx, base);
    return;
  }
  g_obj.seen.fetch_add(1, std::memory_order_relaxed);
  if (!REXCVAR_GET(pdz_cutscene_smooth) || !o->regular)
  {
    if (REXCVAR_GET(pdz_cutscene_smooth))
    {
      g_obj.irregular.fetch_add(1, std::memory_order_relaxed);
    }
    __imp__sub_821E8F70(ctx, base);
    return;
  }
  const bool hold = g_f.cut || o->cut;
  (hold ? g_obj.held : g_obj.smoothed).fetch_add(1, std::memory_order_relaxed);
  if (o->cut && !g_f.cut)
  {
    g_holds[kHoldObjectRoot].fetch_add(1, std::memory_order_relaxed);
  }
  BeginSample(base, o->sampler, false, hold, o->tu);
  __imp__sub_821E8F70(ctx, base);
  EndSample(base);
}

// Type 1 objects, bones (r3 = the object): slot 0 at the frame's mapping, its pose blended / kept at a block's first
// part (sub_8271F7B8 below); its float tracks stay stepped.
REX_HOOK_RAW(sub_821E9000)
{
  if (!REXCVAR_GET(pdz_cutscene_smooth))
  {
    __imp__sub_821E9000(ctx, base);
    return;
  }
  Object *o = CutsceneObject(base, ctx.r3.u32, 0.0);
  if (!o || !o->regular || !REXCVAR_GET(pdz_cutscene_smooth))
  {
    __imp__sub_821E9000(ctx, base);
    return;
  }
  BeginSample(base, o->sampler, false, g_f.cut || o->cut, o->tu);
  __imp__sub_821E9000(ctx, base);
  EndSample(base);
}

// The library's key lookup (sampler r3, &fraction r4, u32 index[4] r5): called for every animation sample (gameplay
// too): one relaxed load when nothing is smoothed. For the sampler being smoothed: Rare's result, then the frame's key
// position for the library's linear mode (index[1] = k, index[2] = k + 1, fraction u in [0, 1]); a cut: the key the
// position has reached; the block's first part: key 0 exactly (blended with the kept frame after it). Any other
// sampler disarms the float track hook.
REX_HOOK_RAW(sub_82720D90)
{
  if (g_live.load(std::memory_order_relaxed) == 0 || !t_s.on)
  {
    __imp__sub_82720D90(ctx, base);
    return;
  }
  const uint32_t sampler = ctx.r3.u32, frac_at = ctx.r4.u32, idx_at = ctx.r5.u32;
  __imp__sub_82720D90(ctx, base);
  if (sampler != t_s.sampler)
  {
    t_s.armed = false;
    return;
  }
  t_s.armed = true;
  t_s.idx_at = idx_at;
  const float t = LF(base, sampler + kSmpTime);
  // t before the clamp: the channel hook's prediction when the library clamped it to 1.
  const float tu = t >= 1.0f ? std::max(t_s.tu, t) : t;
  const Mapping m = Map(base, sampler, t, tu, t_s.retime, t_s.first_block);
  if (!m.ok)
  {
    t_s.mapped = false;
    t_s.keys = 0;
    t_s.discrete = L32(base, idx_at + 4);
    for (uint32_t i = 0; i < 4; ++i)
    {
      t_s.idx[i] = L32(base, idx_at + 4 * i);
    }
    if (t_s.hold)
    {
      SF(base, frac_at, 0.0f);
    }
    return;
  }
  uint32_t key = KeyAt(m);
  uint32_t k = IntervalAt(m);
  float u = std::min(std::max(m.pos - float(k), 0.0f), 1.0f);
  t_s.cross = m.retimed && (m.pos < 0.0f || t_s.past_end);
  if (t_s.past_end)
  {
    key = 0;
    k = 0;
  }
  if (t_s.cross || t_s.hold)
  {
    u = float(key) - float(k);
  }
  t_s.mapped = true;
  t_s.retimed = m.retimed;
  t_s.pos = m.pos;
  t_s.frames = m.keys;
  t_s.keys = m.keys;
  t_s.k = k;
  t_s.u = u;
  t_s.discrete = key;
  t_s.record = m.retimed ? t >= 1.0f && !t_s.past_end : m.pos >= float(m.keys - 1);
  if (t_s.cross)
  {
    Snap *s = FindSnap(sampler, false);
    t_s.snap = s && s->player == g_f.player && s->block + 1 == g_f.block ? s : nullptr;
    const float from = t_s.snap ? t_s.snap->kept_at : -1.0f - kLag;
    t_s.w = t_s.cross_cut || t_s.past_end || from >= 0.0f ? 0.0f
                                                          : std::min(std::max((m.pos - from) / -from, 0.0f), 1.0f);
  }
  else if (t_s.record)
  {
    t_s.snap = FindSnap(sampler, true);
  }
  t_s.idx[0] = k > 0 ? k - 1 : 0u;
  t_s.idx[1] = k;
  t_s.idx[2] = k + 1;
  t_s.idx[3] = std::min<uint32_t>(k + 2, m.keys - 1);
  for (uint32_t i = 0; i < 4; ++i)
  {
    S32(base, idx_at + 4 * i, t_s.idx[i]);
  }
  SF(base, frac_at, u);
}

// The library's bones (sampler r3; the pose buffer [sampler +116], 48 bytes a bone, local to the skeleton). For a
// smoothed object (not the camera): at its block's switch frame keep the pose shown; in the next block's first part
// blend it with key 0's (each bone's first vec4, the rotation quaternion: same hemisphere, lerp, normalized; the other
// two lerped); across a camera cut or past the end the kept pose stays.
REX_HOOK_RAW(sub_8271F7B8)
{
  if (g_live.load(std::memory_order_relaxed) == 0 || !t_s.on || t_s.camera || ctx.r3.u32 != t_s.sampler)
  {
    __imp__sub_8271F7B8(ctx, base);
    return;
  }
  const uint32_t sampler = ctx.r3.u32;
  __imp__sub_8271F7B8(ctx, base);
  if (!t_s.mapped || !t_s.snap)
  {
    if (t_s.mapped && t_s.cross)
    {
      g_obj_cross_key0.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }
  const uint32_t pose = L32(base, sampler + kSmpPose);
  const uint32_t bones = L16(base, sampler + kSmpBones);
  if (!pose || bones == 0 || bones > kMaxBones)
  {
    return;
  }
  Snap &s = *t_s.snap;
  if (t_s.record)
  {
    std::memcpy(s.pose, base + pose, bones * 48);
    s.bones = bones;
    s.player = g_f.player;
    s.block = g_f.block;
    s.kept_at = t_s.pos - float(t_s.frames);
    return;
  }
  if (!t_s.cross || s.bones != bones)
  {
    g_obj_cross_key0.fetch_add(t_s.cross ? 1 : 0, std::memory_order_relaxed);
    return;
  }
  const float w = t_s.w;
  (w > 0.0f ? g_obj_cross_blend : g_obj_cross_kept).fetch_add(1, std::memory_order_relaxed);
  for (uint32_t b = 0; b < bones; ++b)
  {
    for (uint32_t v = 0; v < 3; ++v)
    {
      const uint32_t at = pose + b * 48 + v * 16;
      float p[4], c[4];
      for (uint32_t i = 0; i < 4; ++i)
      {
        const uint32_t raw = _byteswap_ulong(s.pose[b * 12 + v * 4 + i]);
        std::memcpy(&p[i], &raw, 4);
        c[i] = LF(base, at + 4 * i);
      }
      float np = 0.0f, nc = 0.0f, dot = 0.0f;
      for (uint32_t i = 0; i < 4; ++i)
      {
        np += p[i] * p[i];
        nc += c[i] * c[i];
        dot += p[i] * c[i];
      }
      const bool quat = v == 0 && std::fabs(np - 1.0f) < 0.05f && std::fabs(nc - 1.0f) < 0.05f;
      if (quat && dot < 0.0f)
      {
        for (float &x : p)
        {
          x = -x;
        }
      }
      float r[4], nr = 0.0f;
      for (uint32_t i = 0; i < 4; ++i)
      {
        r[i] = p[i] + (c[i] - p[i]) * w;
        nr += r[i] * r[i];
      }
      if (quat && nr > 1e-6f)
      {
        const float inv = 1.0f / std::sqrt(nr);
        for (float &x : r)
        {
          x *= inv;
        }
      }
      for (uint32_t i = 0; i < 4; ++i)
      {
        SF(base, at + 4 * i, r[i]);
      }
    }
  }
}

namespace
{
  void SaveArgs(const PPCContext &ctx, PPCRegister (&r)[8], PPCRegister &f1)
  {
    r[0] = ctx.r3;
    r[1] = ctx.r4;
    r[2] = ctx.r5;
    r[3] = ctx.r6;
    r[4] = ctx.r7;
    r[5] = ctx.r8;
    r[6] = ctx.r9;
    r[7] = ctx.r10;
    f1 = ctx.f1;
  }
  void RestoreArgs(PPCContext &ctx, const PPCRegister (&r)[8], const PPCRegister &f1)
  {
    ctx.r3 = r[0];
    ctx.r4 = r[1];
    ctx.r5 = r[2];
    ctx.r6 = r[3];
    ctx.r7 = r[4];
    ctx.r8 = r[5];
    ctx.r9 = r[6];
    ctx.r10 = r[7];
    ctx.f1 = f1;
  }
  bool HasLook() { return t_cont_n >= kLookAt + 3 && t_cont[kLookAt] != kNoTrack; }
} // namespace

// The float tracks' decode (sub_8271F7B8 -> here: r3 the track set, r4 the output, r5 the key indices, r8 the mode,
// f1 the fraction). Called for every animation sample: one relaxed load when nothing is smoothed. For the sample being
// smoothed (armed by its key lookup):
//   - the first camera, at its clip's first sample: every key decoded in mode 0 (which reads index[1]) and kept (all
//     continuous tracks, scene ID, use-look-at), each interval judged (5); any camera: its continuous tracks on the
//     curve (look-at as a 3D vector), straight, the key on a cut, or between the kept frame's values and key 0's at a
//     block's first part (judging the switch: eye, view turn, look-at, scene with a move);
//   - a light: the continuous tracks in the linear mode at the frame's position;
//   - then every track stepped (mode 0) on the key the position has reached, the decode the channel keeps;
//   - the continuous values written over it (track i at r4 + 4 x (sampler +38 + i), the slot sub_821EA3F0 copies to
//     the channel's buffer: sub_82747120, the memcpy at 821EA56C).
REX_HOOK_RAW(sub_8271FAE0)
{
  if (g_live.load(std::memory_order_relaxed) == 0 || !t_s.on || !t_s.armed || ctx.r5.u32 != t_s.idx_at)
  {
    __imp__sub_8271FAE0(ctx, base);
    return;
  }
  t_s.armed = false;
  const uint32_t out = ctx.r4.u32, idx_at = ctx.r5.u32;
  const uint32_t slot = L16(base, t_s.sampler + kSmpTrackOffset);
  PPCRegister args[8], f1;
  SaveArgs(ctx, args, f1);
  const uint32_t n = t_s.mapped ? t_cont_n : 0u;
  const auto value = [&](uint32_t i) {
    return t_cont[i] == kNoTrack ? 0.0f : LF(base, out + 4 * (slot + t_cont[i]));
  };
  const auto decode = [&](uint32_t key, float *dst, int32_t *scene, bool *use) {
    RestoreArgs(ctx, args, f1);
    S32(base, idx_at + 4, key);
    ctx.r8.u64 = 0;
    __imp__sub_8271FAE0(ctx, base);
    for (uint32_t i = 0; i < n; ++i)
    {
      dst[i] = value(i);
    }
    if (scene)
    {
      // Rare: scene = (int)(value + 0.5) when >= 0 (82213808).
      const float x = t_scene_index >= 0 ? LF(base, out + 4 * (slot + uint32_t(t_scene_index))) : -1.0f;
      *scene = x >= 0.0f ? int32_t(x + 0.5f) : -1;
    }
    if (use)
    {
      *use = t_use_index >= 0 && LF(base, out + 4 * (slot + uint32_t(t_use_index))) > 0.0f;
    }
  };
  float result[kMaxContinuous] = {};
  bool write = false;
  if (n && t_s.camera)
  {
    const uint32_t anim = L32(base, t_s.sampler + kSmpAnim);
    const uint32_t k = t_s.k;
    const bool first_camera = t_s.sampler == g_f.cam0_sampler;
    float v[4][kMaxContinuous] = {};
    int32_t sc[4] = {-1, -1, -1, -1};
    bool use[4] = {};
    const bool have[4] = {k > 0, true, k + 1 < t_s.keys, k + 2 < t_s.keys};
    if (first_camera && (g_ck.anim != anim || !g_ck.ready) && t_s.keys <= kMaxKeys)
    {
      // The clip's first sample: every key (5), and the previous clip's last interval as interval 0's neighbour.
      if (g_ck.ready && g_ck.keys >= 2)
      {
        g_ck.tail = true;
        g_ck.tail_block = g_ck.block;
        g_ck.tail_in = g_ck.in[g_ck.keys - 2];
      }
      g_ck.anim = anim;
      g_ck.block = g_f.block;
      g_ck.keys = t_s.keys;
      g_ck.n = n;
      g_ck.counted_scene = ~0u;
      Root r;
      float clip[3] = {};
      const bool root = ReadRoot(base, anim, r);
      if (root)
      {
        ClipOffset(base, r, clip);
      }
      for (uint32_t j = 0; j < t_s.keys; ++j)
      {
        decode(j, g_ck.vals[j], &g_ck.scene[j], &g_ck.use_look[j]);
        float p[3] = {};
        if (root)
        {
          KeyPos(base, r, j, p);
        }
        for (uint32_t a = 0; a < 3; ++a)
        {
          g_ck.eye[j][a] = p[a] + clip[a];
        }
      }
      JudgeCamera(base, anim);
      g_ck.ready = true;
    }
    if (first_camera && g_ck.ready && g_ck.anim == anim)
    {
      for (uint32_t j = 0; j < 4; ++j)
      {
        if (have[j])
        {
          const uint32_t key = k + j - 1;
          std::memcpy(v[j], g_ck.vals[key], sizeof(float) * n);
          sc[j] = g_ck.scene[key];
          use[j] = g_ck.use_look[key];
        }
      }
    }
    else
    {
      for (uint32_t j = 0; j < 4; ++j)
      {
        if (have[j])
        {
          decode(k + j - 1, v[j], &sc[j], &use[j]);
        }
      }
    }
    Snap *s = t_s.snap;
    if (t_s.cross)
    {
      // The block's first part: kept frame -> key 0, or kept across a camera cut / past the end. The switch judged in
      // world terms: eye (key 0's sampled root + the clip's offset, as sub_821EA3F0 will place it), view turn and
      // look-at against the kept frame, scene with a move.
      const Snap *kept = s && s->world && s->ntracks == n ? s : nullptr;
      bool cut = t_s.cross_cut || t_s.past_end;
      if (kept && !cut)
      {
        Root r;
        float e0[3] = {}, clip[3] = {};
        if (ReadRoot(base, anim, r))
        {
          LV(base, t_s.sampler + kSmpRootPos, e0);
          ClipOffset(base, r, clip);
          for (uint32_t a = 0; a < 3; ++a)
          {
            e0[a] += clip[a];
          }
        }
        const float span = std::max(1.0f, -kept->kept_at);
        const bool tail = g_ck.tail && first_camera && g_ck.tail_block == kept->block;
        const float eye_move = Dist(kept->eye, e0);
        const float first = t_s.keys >= 2 && first_camera && g_ck.ready ? g_ck.in[0].eye : -1.0f;
        uint32_t why = CutBy(eye_move, tail ? g_ck.tail_in.eye * span : -1.0f, first, kEyeAbs, kEyeFloor) ? 1u : 0u;
        float look_move = -1.0f;
        if (HasLook() && kept->use_look && use[1])
        {
          look_move = Dist(kept->tracks + kLookAt, v[1] + kLookAt);
          const float turn = Turn(kept->eye, kept->tracks + kLookAt, e0, v[1] + kLookAt);
          if (CutBy(turn, tail ? g_ck.tail_in.turn * span : -1.0f, first_camera && g_ck.ready ? g_ck.in[0].turn : -1.0f,
                    kTurnAbs, kTurnFloor))
          {
            why |= 4;
          }
          if (CutBy(look_move, tail ? g_ck.tail_in.look * span : -1.0f,
                    first_camera && g_ck.ready ? g_ck.in[0].look : -1.0f, kLookAbs, kLookFloor))
          {
            why |= 16;
          }
        }
        if (kept->scene_ok && t_scene_index >= 0 && sc[1] != kept->scene &&
            (eye_move > kSceneMove || look_move > kSceneMove))
        {
          why |= 64;
        }
        if (why)
        {
          cut = true;
          t_s.cam_cross_cut = true;
          g_holds[kHoldCameraLate].fetch_add(1, std::memory_order_relaxed);
          if (first_camera)
          {
            NoteCut(0, true, eye_move, WhyText(why).c_str());
          }
        }
      }
      const float w = cut ? 0.0f : t_s.w;
      for (uint32_t i = 0; i < n; ++i)
      {
        result[i] = kept ? kept->tracks[i] + (v[1][i] - kept->tracks[i]) * w : v[1][i];
      }
      write = true;
    }
    else if (t_s.hold)
    {
      const uint32_t j = t_s.discrete == k ? 1u : 2u;
      std::memcpy(result, v[j], sizeof(float) * n);
      write = true;
    }
    else
    {
      const bool cut_now = CameraCut(base, anim, t_s.keys, k);
      if (cut_now)
      {
        // A cut the frame's decision didn't have (the clip judged in this sample): the key.
        t_s.late_cut = true;
        g_holds[kHoldCameraLate].fetch_add(1, std::memory_order_relaxed);
        const uint32_t j = t_s.discrete == k ? 1u : 2u;
        std::memcpy(result, v[j], sizeof(float) * n);
      }
      else
      {
        bool has0 = have[0] && !CameraCut(base, anim, t_s.keys, k - 1);
        const bool has3 = have[3] && !CameraCut(base, anim, t_s.keys, k + 1);
        if (!have[0] && k == 0)
        {
          // Interval 0: the previous block's kept values (no cut at the switch), at one key's spacing before key 0
          // (CameraAfter does the same for the eye).
          if (const Snap *b = ReachBack(t_s.sampler); b && b->ntracks == n)
          {
            const float span = std::max(1.0f, -b->kept_at);
            for (uint32_t i = 0; i < n; ++i)
            {
              v[0][i] = v[1][i] - (v[1][i] - b->tracks[i]) / span;
            }
            has0 = true;
          }
        }
        for (uint32_t i = 0; i < n; ++i)
        {
          if (HasLook() && i == kLookAt)
          {
            CurveVector(t_s.curve, v[0] + i, v[1] + i, v[2] + i, v[3] + i, has0, has3, 3, t_s.u, result + i);
            i += 2;
            continue;
          }
          CurveVector(t_s.curve, v[0] + i, v[1] + i, v[2] + i, v[3] + i, has0, has3, 1, t_s.u, result + i);
        }
        if (t_s.curve != kCurveLinear)
        {
          g_curve_tracks.fetch_add(1, std::memory_order_relaxed);
        }
      }
      write = true;
    }
    if (t_s.record && !t_s.past_end)
    {
      Snap *rec = FindSnap(t_s.sampler, true);
      std::memcpy(rec->tracks, result, sizeof(float) * n);
      rec->ntracks = n;
      rec->scene_ok = t_scene_index >= 0;
      rec->scene = sc[t_s.discrete == k ? 1 : 2];
      rec->use_look = use[t_s.discrete == k ? 1 : 2];
    }
  }
  else if (n && !t_s.hold && !t_s.cross)
  {
    RestoreArgs(ctx, args, f1);
    ctx.r8.u64 = 1;
    __imp__sub_8271FAE0(ctx, base);
    for (uint32_t i = 0; i < n; ++i)
    {
      result[i] = value(i);
    }
    write = true;
  }
  // Every track stepped on the key the position has reached.
  RestoreArgs(ctx, args, f1);
  S32(base, idx_at + 4, t_s.discrete);
  ctx.r8.u64 = 0;
  __imp__sub_8271FAE0(ctx, base);
  S32(base, idx_at + 4, t_s.idx[1]);
  if (write)
  {
    for (uint32_t i = 0; i < n; ++i)
    {
      if (t_cont[i] != kNoTrack)
      {
        SF(base, out + 4 * (slot + t_cont[i]), result[i]);
      }
    }
    g_tracks.fetch_add(n, std::memory_order_relaxed);
  }
  g_stepped.fetch_add(1, std::memory_order_relaxed);
}

namespace
{
  const char *CurveName(Curve c) { return c == kCurveCatmull ? "catmull" : c == kCurveMonotone ? "monotone" : "linear"; }

  // The per-cutscene line (at Finished, "ended early", every 10 s "interim").
  void Line(const uint8_t *base, uint32_t player, const char *when)
  {
    const auto v = [](const auto &a) { return a.load(std::memory_order_relaxed); };
    const uint64_t seen = v(g_cam.seen) + v(g_obj.seen) + v(g_other.seen);
    if (!seen)
    {
      return;
    }
    (void)base;
    const uint32_t keys = v(g_cam_keys);
    const float length = v(g_cam_length);
    const double rate = keys > 1 && length > 0.0f ? double(keys - 1) / double(length) : 0.0;
    const uint64_t frames = v(g_frames);
    const bool on = REXCVAR_GET(pdz_cutscene_smooth);
    float dmin = 0.0f, dmed = 0.0f, dmax = 0.0f;
    if (g_switch_n)
    {
      float sorted[kSwitchDistances];
      std::memcpy(sorted, g_switch_distance, sizeof(float) * g_switch_n);
      std::sort(sorted, sorted + g_switch_n);
      dmin = sorted[0];
      dmed = sorted[g_switch_n / 2];
      dmax = sorted[g_switch_n - 1];
    }
    REXLOG_INFO(
        "pdz_cutscene_smooth {} ({}; retime {}, camera curve {}): cutscene {:08X}, {} frames. Mapping: {} samples at the "
        "360's block cadence, {} at Rare's. Holds by cause: the first camera's interval {} frames, another camera's "
        "root {}, camera block switch {} (+{} found while sampling), object root {} samples; scene changes without a "
        "move (not held) {}; blocks passed in one frame (kept the last shown frame) {}. Cuts {} ({} frames): {}. "
        "Camera block switches (world eye, kept frame -> key 0): {} blended, {} cut, distance min {:.1f} / median "
        "{:.1f} / max {:.1f} cm; block-start frames: {} blended, {} kept, {} on key 0. Objects (type 1, slot 0; up to "
        "{}): {} samples, {} blended, {} held, {} on Rare's code; block-start poses {} blended, {} kept, {} on key 0. "
        "Cameras ({} controls, {} with lens tracks): {} samples, {} blended, {} held, {} on Rare's code; eye on the "
        "curve {}, other {}, tracks on the curve {}; the first control: clip {} keys per {:.3f} s = {:.1f} keys/s, {} "
        "samples ({:.2f} per frame), {} blended, {} held. Its world eye: off its two keys by more than {:.0f} cm on {} "
        "frames (worst {:.1f} cm at frame {}, {:.0f} ms), stops {} at block switches / {} inside a block, teleports {} "
        "(outside cuts): {}; matrix != root + clip offset on {} frames. Its view moved on {} frames, unchanged on {} ({} "
        "with its time unchanged): {} inside motion ({} runs under {} frames, longest {}), {} in still shots (longest "
        "{}). Presents without a cutscene update: {} (at a block switch: {}), {} shown as the previous frame "
        "(pdz_cutscene_repeat_frame {}); frames with no draws shown as the previous frame {}. Lights / props: {} samples, {} blended, {} held, {} on Rare's code; {} "
        "continuous track values written, {} stepped decodes. Emitters unchanged",
        on ? "on" : "off", when, REXCVAR_GET(pdz_cutscene_retime) ? "on" : "off", CurveName(CurveMode()), player,
        frames, v(g_retimed), v(g_rare_mapping), v(g_holds[kHoldCamera]), v(g_holds[kHoldCameraOther]),
        v(g_holds[kHoldCameraCross]), v(g_holds[kHoldCameraLate]), v(g_holds[kHoldObjectRoot]),
        v(g_holds[kSceneNoJump]), v(g_past_end), v(g_cuts), v(g_cut_frames),
        g_cut_events.empty() ? std::string("none listed") : g_cut_events, v(g_switch_blended), v(g_switch_cut),
        double(dmin), double(dmed), double(dmax), v(g_cam_cross_blend), v(g_cam_cross_kept), v(g_cam_cross_key0),
        v(g_obj_max), v(g_obj.seen), v(g_obj.smoothed), v(g_obj.held), v(g_obj.irregular), v(g_obj_cross_blend),
        v(g_obj_cross_kept), v(g_obj_cross_key0), v(g_cam_controls), v(g_cam_lens), v(g_cam.seen), v(g_cam.smoothed),
        v(g_cam.held), v(g_cam.irregular), v(g_curve_eye), v(g_linear_cam), v(g_curve_tracks), keys, double(length),
        rate, v(g_cam0.seen), frames ? double(v(g_cam0.seen)) / double(frames) : 0.0, v(g_cam0.smoothed),
        v(g_cam0.held), double(kOffKeys), v(g_off_frames), double(v(g_off_worst)), v(g_off_worst_frame),
        double(v(g_off_worst_ms)), v(g_stops_switch), v(g_stops_inside), v(g_teleports),
        g_teleport_events.empty() ? std::string("none listed") : g_teleport_events, v(g_space_mismatch),
        v(g_eye_moved), v(g_eye_same), v(g_same_time_still), v(g_run_inside), v(g_runs_inside), kStillRun,
        v(g_run_inside_max), v(g_run_still), v(g_run_still_max), v(g_no_update_presents), v(g_no_update_switch),
        v(g_presents_repeated), REXCVAR_GET(pdz_cutscene_repeat_frame) ? "on" : "off", v(g_empty_repeated),
        v(g_other.seen),
        v(g_other.smoothed), v(g_other.held), v(g_other.irregular), v(g_tracks), v(g_stepped));
  }

  void RingLines(uint32_t player)
  {
    const uint32_t n = std::min(g_ring_n, kRingLogged);
    for (uint32_t i = 0; i < n; ++i)
    {
      const RingEntry &e = g_ring[(g_ring_n - n + i) % kRing];
      REXLOG_INFO("pdz_cutscene_smooth end {:08X} [{}]: frame {}, block {}, key {}, u {:.2f}, {}{}{}{}{}{}{}world eye "
                  "({:.1f}, {:.1f}, {:.1f}); view eye ({:.1f}, {:.1f}, {:.1f}) target ({:.1f}, {:.1f}, {:.1f}){}",
                  player, int(i) - int(n), e.frame, e.block, e.key, double(e.u), e.flags & 1 ? "hold, " : "",
                  e.flags & 2 ? "block start, " : "", e.flags & 4 ? "kept across a cut, " : "",
                  e.flags & 8 ? "cut found while sampling, " : "", e.flags & 16 ? "curve, " : "",
                  e.flags & 32 ? "past the end: kept the previous block's last shown frame, " : "",
                  e.flags & 64 ? "block switch, " : "", double(e.eye[0]), double(e.eye[1]), double(e.eye[2]),
                  double(e.view_eye[0]), double(e.view_eye[1]), double(e.view_eye[2]), double(e.view_target[0]),
                  double(e.view_target[1]), double(e.view_target[2]), e.view ? "" : " (view not yet built)");
    }
  }

  void ResetStats()
  {
    for (Count *c : {&g_cam, &g_cam0, &g_obj, &g_other})
    {
      for (auto *a : {&c->seen, &c->smoothed, &c->held, &c->irregular})
      {
        a->store(0, std::memory_order_relaxed);
      }
    }
    for (auto &h : g_holds)
    {
      h.store(0, std::memory_order_relaxed);
    }
    for (auto *c : {&g_frames, &g_cut_frames, &g_cuts, &g_tracks, &g_stepped, &g_retimed, &g_rare_mapping, &g_past_end,
                    &g_cam_cross_blend, &g_cam_cross_kept, &g_cam_cross_key0, &g_obj_cross_blend, &g_obj_cross_kept,
                    &g_obj_cross_key0, &g_curve_eye, &g_curve_tracks, &g_linear_cam, &g_eye_moved, &g_eye_same,
                    &g_run_inside, &g_run_still, &g_same_time_still, &g_off_frames, &g_stops_switch,
                    &g_stops_inside, &g_teleports, &g_space_mismatch, &g_off_worst_frame, &g_switch_blended,
                    &g_switch_cut, &g_no_update_presents, &g_no_update_switch, &g_presents_repeated, &g_empty_repeated})
    {
      c->store(0, std::memory_order_relaxed);
    }
    for (auto *c : {&g_cam_keys, &g_cam_controls, &g_cam_lens, &g_obj_max, &g_run_inside_max, &g_run_still_max,
                    &g_runs_inside})
    {
      c->store(0, std::memory_order_relaxed);
    }
    for (auto *c : {&g_cam_length, &g_off_worst, &g_off_worst_ms})
    {
      c->store(0.0f, std::memory_order_relaxed);
    }
    g_swap_calls = 0;
    g_swap_block = 0;
    g_swap_player = 0;
    g_swap_updated_run = 0;
    g_cut_events.clear();
    g_teleport_events.clear();
    g_cut_events_n = 0;
    g_teleport_events_n = 0;
    g_switch_n = 0;
    g_pending = false;
    g_pending_player = 0;
    g_next_interim_ms = kInterimMs;
    g_f.valid = false;
    g_f.player = 0;
    g_f.prev_cut = false;
    g_f.eye_valid = false;
    g_f.run = 0;
    g_f.moved_before = false;
    g_f.hist_n = 0;
    g_ck.anim = 0;
    g_ck.ready = false;
    g_ck.tail = false;
    g_ring_n = 0;
    for (Snap &s : g_snaps)
    {
      s.sampler = 0;
    }
  }
} // namespace

// DarkCutscene::Finished (sub_82220AC8, r3 = the cutscene player; a skip comes through here too, sub_822208F0): the
// line, the camera's last frames, and the first gameplay frames after it (sub_8220F478 below).
REX_HOOK_RAW(sub_82220AC8)
{
  if (!REXCVAR_GET(pdz_cutscene_smooth))
  {
    g_final_frame = 0;
    if (g_pending)
    {
      ResetStats(); // switched off during a cutscene
    }
    __imp__sub_82220AC8(ctx, base);
    return;
  }
  const uint32_t player = ctx.r3.u32;
  EndRun(false);
  if (g_pending)
  {
    if (g_ring_n && g_f.cam0_ctrl)
    {
      RingEntry &e = g_ring[(g_ring_n - 1) % kRing];
      if (!e.view)
      {
        LV(base, g_f.cam0_ctrl + kCtrlEye, e.view_eye);
        LV(base, g_f.cam0_ctrl + kCtrlTarget, e.view_target);
        e.view = true;
      }
    }
    Line(base, player, "finished");
    RingLines(player);
    g_after = g_f.cam0_ctrl ? kAfterFrames : 0;
    g_after_ctrl = g_f.cam0_ctrl;
    g_final_frame = g_f.cam0_ctrl ? L32(base, kClockFrames) : 0;
    g_final_logged = false;
  }
  ResetStats();
  __imp__sub_82220AC8(ctx, base);
}

// CameraControlUpdate (sub_8220F478, once a frame): after a cutscene, the first camera control's view for the first
// gameplay frames; and a cutscene that stopped without Finished: its line "(ended early)". One flag test otherwise.
REX_HOOK_RAW(sub_8220F478)
{
  __imp__sub_8220F478(ctx, base);
  if (!REXCVAR_GET(pdz_cutscene_smooth))
  {
    g_after = 0;
    return;
  }
  if (g_after)
  {
    float eye[3], target[3];
    LV(base, g_after_ctrl + kCtrlEye, eye);
    LV(base, g_after_ctrl + kCtrlTarget, target);
    REXLOG_INFO("pdz_cutscene_smooth after [{}]: the cutscene's camera control {:08X}: view eye ({:.1f}, {:.1f}, "
                "{:.1f}) target ({:.1f}, {:.1f}, {:.1f})",
                kAfterFrames - g_after + 1, g_after_ctrl, double(eye[0]), double(eye[1]), double(eye[2]),
                double(target[0]), double(target[1]), double(target[2]));
    --g_after;
  }
  if (g_pending && !PlayingCutscene(base))
  {
    Line(base, g_pending_player, "ended early");
    RingLines(g_pending_player);
    ResetStats();
  }
}

// The camera control's view build (sub_8220C3F8, r3 = the control; called by the control's advance in
// CameraControlUpdate and by the render, 8261BD78 / 8261BDD0): on the frame Finished ran in, the active control's top
// layer is the gameplay layer before its first aim, so the build gives an axis-aligned default rotation (log 291's
// lantern frame, dump row 5294). On that frame the build is skipped for the active control when the values it would
// overwrite are a sound view (eye +948, target +972, up +984: finite, eye to target over 1 cm, up near unit length),
// so the control and the view keep the frame before's (the cutscene's last view); otherwise Rare's build runs
// (the lantern frame is better than a bad view). Nothing is written. One compare on every other frame.
REX_HOOK_RAW(sub_8220C3F8)
{
  if (g_final_frame && REXCVAR_GET(pdz_cutscene_smooth))
  {
    const uint32_t frame = L32(base, kClockFrames);
    if (frame != g_final_frame)
    {
      if (frame > g_final_frame + 1)
      {
        g_final_frame = 0; // the frame passed
      }
    }
    else if (ctx.r3.u32 && ctx.r3.u32 == L32(base, kActiveCamera) && !PlayingCutscene(base))
    {
      const uint32_t ctrl = ctx.r3.u32;
      float eye[3], target[3], up[3];
      LV(base, ctrl + kCtrlEye, eye);
      LV(base, ctrl + kCtrlTarget, target);
      LV(base, ctrl + kCtrlUpVector, up);
      const char *why = nullptr;
      for (uint32_t i = 0; i < 3 && !why; ++i)
      {
        if (!std::isfinite(eye[i]) || !std::isfinite(target[i]) || !std::isfinite(up[i]))
        {
          why = "a value is not finite";
        }
      }
      const float d = Dist(eye, target);
      const float un = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
      if (!why && d <= 1.0f)
      {
        why = "eye and target within 1 cm";
      }
      if (!why && (un < 0.5f || un > 2.0f))
      {
        why = "up not near unit length";
      }
      if (!g_final_logged)
      {
        g_final_logged = true;
        if (why)
        {
          REXLOG_INFO("pdz_cutscene_smooth final frame view restored: skipped ({}; control {:08X}, the cutscene's "
                      "{:08X}): eye ({:.1f}, {:.1f}, {:.1f}) target ({:.1f}, {:.1f}, {:.1f}) up ({:.2f}, {:.2f}, {:.2f})",
                      why, ctrl, g_after_ctrl, double(eye[0]), double(eye[1]), double(eye[2]), double(target[0]),
                      double(target[1]), double(target[2]), double(up[0]), double(up[1]), double(up[2]));
        }
        else
        {
          REXLOG_INFO("pdz_cutscene_smooth final frame view restored: yes (the view build skipped on frame {}, the "
                      "frame before's kept; control {:08X}, the cutscene's {:08X}): eye ({:.1f}, {:.1f}, {:.1f}) target "
                      "({:.1f}, {:.1f}, {:.1f}) up ({:.2f}, {:.2f}, {:.2f})",
                      frame, ctrl, g_after_ctrl, double(eye[0]), double(eye[1]), double(eye[2]), double(target[0]),
                      double(target[1]), double(target[2]), double(up[0]), double(up[1]), double(up[2]));
        }
      }
      if (!why)
      {
        return; // keep the frame before's view
      }
    }
  }
  __imp__sub_8220C3F8(ctx, base);
}
