// EXPERIMENT: full-screen picture per player on two monitors.
//
// In a top/bottom split-screen race each player's view is a 1280x360 strip and
// the game gives its camera the matching 32:9 aspect ratio. The dual-monitor
// companion shows one strip per monitor, so each fills only half the height.
//
// With split_fullscreen_views on, a camera asked for a strip aspect (> 3:1)
// gets half of it (16:9) instead. The strip is then a vertically squashed
// 16:9 picture, which the companion stretches to fill each monitor.
// CPresentationCamera::SetAspectRatio(float) is vtable slot 0x8284DD08: it
// stores the value at +256 and marks the camera dirty.
#include <atomic>
#include <bit>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "player2_profile.h"

REXCVAR_DEFINE_BOOL(split_fullscreen_views, false, "FM3/Display",
    "EXPERIMENT: draw each split-screen view as a squashed 16:9 picture for the dual-monitor companion to stretch");

extern "C" void __imp__sub_8284DD08(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8284DD08(PPCContext& ctx, uint8_t* base) {
  if (REXCVAR_GET(split_fullscreen_views)) {
    const double asked = ctx.f1.f64;
    static std::atomic<unsigned> notes{0};
    static std::atomic<float> last{0};
    if (float(asked) != last.exchange(float(asked)) && notes.fetch_add(1) < 60)
      REXLOG_WARN("SPLIT_FULLSCREEN camera {:08X} aspect {:.4f}{}", ctx.r3.u32, asked,
                  asked > 3.0 ? " -> halved" : "");
    if (asked > 3.0) ctx.f1.f64 = asked * 0.5;
  }
  __imp__sub_8284DD08(ctx, base);
}

// Tell the dual-monitor companion whether the split-screen race HUD is the top
// screen, through a 4-byte named shared value. Called from the present hook;
// reads guest memory only.
void SquashHudIfShown(uint8_t* base);
namespace fm3 {
void PublishRaceView(uint8_t* base) {
#ifdef _WIN32
  if (!REXCVAR_GET(split_fullscreen_views)) return;
  SquashHudIfShown(base);
  static unsigned tick = 0;
  if (tick++ % 10) return;
  static volatile LONG* flag = [] {
    const auto name = L"Local\\FM3RaceView_" + std::to_wstring(GetCurrentProcessId());
    const HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LONG), name.c_str());
    return map ? static_cast<volatile LONG*>(MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, sizeof(LONG))) : nullptr;
  }();
  if (!flag) return;
  const std::string layers = UiLayersSummary(base);
  const auto first = layers.find(" | L1 ");
  const auto next = first == std::string::npos ? first : layers.find(" | L2 ", first);
  bool racing = false;
  if (next != std::string::npos) {
    const std::string top = layers.substr(first, next - first);
    const std::string hud = "CHudSplitscreen";
    racing = top.size() >= hud.size() && top.compare(top.size() - hud.size(), hud.size(), hud) == 0;
  }
  *flag = racing ? 1 : 0;
  // Player 2's result for the companion's banner: 8 LONGs
  // {show, place, credits, xp, level, wallet, reward cars, spare}. Shown while
  // a post-race screen is anywhere in UI layer 1.
  static volatile LONG* result = [] {
    const auto name = L"Local\\FM3Player2Result_" + std::to_wstring(GetCurrentProcessId());
    const HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 8 * sizeof(LONG), name.c_str());
    return map ? static_cast<volatile LONG*>(MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 8 * sizeof(LONG))) : nullptr;
  }();
  if (result) {
    int32_t r[7];
    Player2Result(r);
    for (int i = 0; i < 7; ++i) result[i + 1] = r[i];
    const bool post = next != std::string::npos && layers.substr(first, next - first).find("PostRace") != std::string::npos;
    // 2 = the results screen itself is on top (the companion replaces Player
    // 1's panels on Player 2's monitor), 1 = another post-race screen.
    const std::string section = next != std::string::npos ? layers.substr(first, next - first) : std::string();
    const std::string exact = " CPostRace";
    const bool on_top = section.size() >= exact.size() && section.compare(section.size() - exact.size(), exact.size(), exact) == 0;
    result[0] = post && r[0] > 0 ? (on_top ? 2 : 1) : 0;
  }
#else
  (void)base;
#endif
}
}  // namespace fm3

// HUD for the stretched views. CHudSplitscreen is an Anark scene. Its runtime
// nodes are 0x100 bytes: +4 property node (name hash at +0), +0x3C local 4x4
// matrix (row vectors, translation in the last row), +0x7C world matrix
// (rebuilt from the parents every frame), +0xD8 next sibling, +0xDC first
// child. World y = 0 is the split line; Player 1's group (A0AA5307) sits at the
// origin and Player 2's (B6D6B446) at y = -H, each laying its pieces out in
// local y 0..H (H = 332).
//
// The companion stretches each strip to twice its height, so every piece must
// be drawn half as tall and stay against its own edge of the strip:
//   pieces in the upper part:  y' = H/2 + y/2      lower part:  y' = y/2
// Both are "scale the piece's local matrix by 0.5 in y, then shift", applied
// to the direct children of the two player groups (and the shared top-centre
// component 1CCBB455). The engine may rebuild a local matrix from its
// properties; a y scale back near 1 means that happened, and it is redone.
namespace {
uint32_t Rd(uint8_t* base, uint32_t a) { uint32_t v; std::memcpy(&v, base + a, 4); return std::byteswap(v); }
float RdF(uint8_t* base, uint32_t a) { return std::bit_cast<float>(Rd(base, a)); }
void WrF(uint8_t* base, uint32_t a, float f) { uint32_t v = std::byteswap(std::bit_cast<uint32_t>(f)); std::memcpy(base + a, &v, 4); }
bool Heap(uint32_t p) { return p >= 0x20000000 && p < 0x80000000 && !(p & 3); }
uint32_t NameOf(uint8_t* base, uint32_t runtime) { const auto node = Rd(base, runtime + 4); return Heap(node) ? Rd(base, node) : 0; }

constexpr uint32_t kLocal = 0x3C, kWorld = 0x7C, kSibling = 0xD8, kChild = 0xDC;
struct Mat { float m[16]; };
Mat RdM(uint8_t* base, uint32_t a) { Mat r; for (int i = 0; i < 16; ++i) r.m[i] = RdF(base, a + i * 4); return r; }
Mat Mul(const Mat& a, const Mat& b) {
  Mat r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float v = 0;
      for (int k = 0; k < 4; ++k) v += a.m[i * 4 + k] * b.m[k * 4 + j];
      r.m[i * 4 + j] = v;
    }
  return r;
}
// The engine rebuilds world matrices only for pieces that animate; static
// ones keep the matrix cached when the scene loaded. Bring every node under
// `first` in line with world = local x parent world.
void RefreshWorld(uint8_t* base, uint32_t first, const Mat& parent, int depth, unsigned& budget) {
  for (auto node = first; Heap(node) && budget; node = Rd(base, node + kSibling)) {
    --budget;
    const Mat want = Mul(RdM(base, node + kLocal), parent);
    for (int i = 0; i < 16; ++i) {
      const float have = RdF(base, node + kWorld + i * 4);
      if (!(have - want.m[i] < 0.01f && want.m[i] - have < 0.01f)) WrF(base, node + kWorld + i * 4, want.m[i]);
    }
    const auto child = Rd(base, node + kChild);
    if (depth < 16 && Heap(child)) RefreshWorld(base, child, want, depth + 1, budget);
  }
}
// Returns true when the piece was (re)squashed this call.
bool Squash(uint8_t* base, uint32_t runtime, float shift) {
  const float sy = RdF(base, runtime + kLocal + 5 * 4);
  if (!(sy > 0.75f && sy < 1.5f)) return false;  // already done, or not a plain piece
  for (int row = 0; row < 4; ++row) {
    const auto at = runtime + kLocal + (row * 4 + 1) * 4;
    WrF(base, at, RdF(base, at) * 0.5f);
  }
  const auto ty = runtime + kLocal + 13 * 4;
  WrF(base, ty, RdF(base, ty) + shift);
  return true;
}
// Pieces that live in the lower part of a player's strip (speed/gear cluster,
// minimap). Everything else is anchored to the top.
bool LowerPiece(uint32_t name) { return name == 0xEDC383AE || name == 0x5D5BFE9B; }

void SquashHud(uint8_t* base, uint32_t hud) {
  const auto root = Rd(base, hud + 84);
  if (!Heap(root)) return;
  // The layer hangs off the root as its sibling (+0x10); +0x14 (child) is empty.
  auto layer_node = Rd(base, root + 0x14);
  if (!Heap(layer_node)) layer_node = Rd(base, root + 0x10);
  const auto layer = Heap(layer_node) ? Rd(base, layer_node + 4) : 0;
  if (!Heap(layer)) return;
  // H from Player 2's group offset.
  float height = 0;
  for (auto g = Rd(base, layer + kChild), n = 0u; Heap(g) && n < 16; g = Rd(base, g + kSibling), ++n)
    if (NameOf(base, g) == 0xB6D6B446) height = -RdF(base, g + kLocal + 13 * 4);
  if (!(height > 200.0f && height < 500.0f)) return;
  unsigned changed = 0;
  for (auto g = Rd(base, layer + kChild), n = 0u; Heap(g) && n < 16; g = Rd(base, g + kSibling), ++n) {
    const auto name = NameOf(base, g);
    if (name == 0x1CCBB455) {
      changed += Squash(base, g, height * 0.5f);
    } else if (name == 0xA0AA5307 || name == 0xB6D6B446) {
      for (auto piece = Rd(base, g + kChild), m = 0u; Heap(piece) && m < 32; piece = Rd(base, piece + kSibling), ++m)
        changed += Squash(base, piece, LowerPiece(NameOf(base, piece)) ? 0.0f : height * 0.5f);
    }
  }
  // Squashed pieces carry a y scale of 0.5; nothing to refresh before the
  // first squash (keeps the stock HUD untouched if anything above bailed out).
  unsigned budget = 4000;
  RefreshWorld(base, Rd(base, layer + kChild), RdM(base, layer + kWorld), 0, budget);
  static std::atomic<unsigned> notes{0};
  if (changed && notes.fetch_add(1) < 12)
    REXLOG_WARN("SPLIT_FULLSCREEN HUD pieces squashed: {} (strip height {:.0f})", changed, height);
}
}  // namespace

// Called once per presented frame (fm3_089: hooking the scene's own update
// slot 82369C98 never fired). Finds CHudSplitscreen (vtable 8201BCDC) among
// the scenes of UI layer 1: CUserInterface4 [82C76C1C] -> scene stack +76 ->
// 148-byte layers at +12; each layer's list head at +20, scene at node+12.
void SquashHudIfShown(uint8_t* base) {
  const auto ui = Rd(base, 0x82C76C1C);
  const auto stack = Heap(ui) ? Rd(base, ui + 76) : 0;
  const auto layers = Heap(stack) ? Rd(base, stack + 12) : 0;
  if (!Heap(layers) || Rd(base, stack + 16) < layers + 2 * 148) return;
  const auto head = Rd(base, layers + 148 + 20);
  if (!Heap(head)) return;
  unsigned guard = 0;
  for (auto node = Rd(base, head); Heap(node) && node != head && guard < 16; node = Rd(base, node), ++guard) {
    const auto scene = Rd(base, node + 12);
    if (Heap(scene) && Rd(base, scene) == 0x8201BCDC) { SquashHud(base, scene); return; }
  }
}
