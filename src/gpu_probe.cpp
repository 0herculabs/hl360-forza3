// Diagnostic (gpu_probe = true): list every distinct render-to-texture
// resolve the title issues, to identify the followed car's shadow map.
//
// 824C3CF0 is the XDK's D3DDevice_Resolve(device, flags, srcRect*, destTex*,
// destPoint*, level, slice, clearColor*, ...): the routine that writes
// RB_COPY_CONTROL (0x2318). A D3D texture keeps its GPU fetch constant at
// +0x1C: dword1 bits 0-5 = texture format, dword2 = (width-1) | (height-1)<<13.
#include <atomic>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <bit>
#include <chrono>
#include <cstdio>
#include <string>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_BOOL(gpu_probe, false, "FM3/Debug", "Log each distinct render-to-texture resolve once");

namespace {
uint32_t Rd(uint8_t* base, uint32_t a) { uint32_t v; std::memcpy(&v, base + a, 4); return std::byteswap(v); }
bool Ok(uint32_t p) { return p >= 0x10000 && p < 0xF0000000; }
}  // namespace

// EXPERIMENT: the followed car's variance shadow map.
// ShadowMapGenerator 827B4E40(this, index) resolves render target 0 into a
// 512x512 k_16_16 texture with D3DRESOLVE_EXPONENTBIAS(10) (flags 0x28000000,
// return address 827B4EA0), or the depth buffer into a 256x256 k_24_8 texture
// (flags 4, return address 827B4EB8). Read back from guest memory, the
// 512x512 map is 0xFFFF in both channels at every texel: no car outline.
//
// While the file vsm_bias.txt exists next to the executable, its first number
// (-32..31) replaces that exponent bias. Re-read about once a second, so the
// value can be changed while a race is running.
namespace {
// Second number in vsm_bias.txt: exponent bias for every other resolve into a
// k_16_16 texture (the shadow blur passes, which the game issues with bias 0).
std::atomic<int> blur_bias_override{100};
int VsmBiasOverride() {
  static std::atomic<int> value{100};
  static std::atomic<long long> next_check{0};
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  if (now >= next_check.load(std::memory_order_relaxed)) {
    next_check = now + 1'000'000'000LL;
    int parsed = 100, blur = 100;
    if (FILE* f = std::fopen("vsm_bias.txt", "r")) {
      const int got = std::fscanf(f, "%d %d", &parsed, &blur);
      if (got < 1 || parsed < -32 || parsed > 31) parsed = 100;
      if (got < 2 || blur < -32 || blur > 31) blur = 100;
      std::fclose(f);
    }
    blur_bias_override = blur;
    if (parsed != value.exchange(parsed)) REXLOG_WARN("GPU_PROBE shadow map exponent bias override: {}", parsed == 100 ? std::string("off") : std::to_string(parsed));
  }
  return value.load(std::memory_order_relaxed);
}
}  // namespace

// FIX (focus_car_shadow_fix): the dark box under the followed car.
//
// Seen in RenderDoc captures of this build:
//  1. The followed car is drawn from the sun into a k_16_16 target as
//     (depth, depth^2) scaled to the format's full range; the background is
//     the maximum value. That picture is correct (a clean car silhouette).
//  2. The game resolves it into a signed k_16_16 texture with exponent bias 10.
//     Under this runtime that saturates every texel to 0xFFFF, which a signed
//     sampler reads as just below zero.
//  3. The next pass (pixel shader at 820DD6xx, 10 taps) turns each texel into
//     "is background" with value >= 1.0 (literal constants c0 = (0.99, 1, 0, 0)
//     and c1 = (0.25, 0.375, 0.0625, 1) stored with the shader at 820DD6E0).
//     Nothing passes, so the whole map counts as occluder: a box-shaped shadow.
// The runtime's resolve can only be steered by the bias: -6 is a bit-exact copy
// except that the maximum becomes 0x8000 (-1 when signed); -5 doubles that, so
// the background saturates to 0xFFFF and the car lands in 0x3560..0xFFxx.
// Changing the shader's literal thresholds at 820DD6E4/820DD6FC had no effect
// (fm3_110: the draw still received 0.99 / 1.0; the shader in use is loaded
// from elsewhere). So instead: bias -5, and mark the destination texture as
// unsigned (clear the four 2-bit sign fields, bits 2-9 of fetch-constant
// dword 0 at texture+0x1C). Unsigned 0xFFFF is exactly 1.0 and passes the
// "background" test; every car texel is below it.
REXCVAR_DEFINE_BOOL(focus_car_shadow_fix, false, "FM3/Research",
    "Fix the box-shaped shadow under the followed car (shadow-map resolve scale and thresholds)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
bool ShadowFixEnabled() {
  static const bool enabled = REXCVAR_GET(focus_car_shadow_fix);
  return enabled;
}
}  // namespace

extern "C" void __imp__sub_824C3CF0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_824C3CF0(PPCContext& ctx, uint8_t* base) {
  if (uint32_t(ctx.lr) == 0x827B4EA0) {
    int bias = VsmBiasOverride();
    if (bias == 100 && ShadowFixEnabled()) {
      bias = -5;
      const uint32_t tex = ctx.r6.u32;
      if (Ok(tex)) {
        const uint32_t fetch0 = Rd(base, tex + 0x1C);
        if (fetch0 & 0x3FC) {
          const uint32_t cleared = std::byteswap(fetch0 & ~0x3FCu);
          std::memcpy(base + tex + 0x1C, &cleared, 4);
          static std::atomic<unsigned> notes{0};
          if (notes.fetch_add(1) < 4) REXLOG_WARN("SHADOW_FIX texture {:08X} fetch0 {:08X} -> unsigned", tex, fetch0);
        }
      }
    }
    if (bias != 100) ctx.r4.u64 = (ctx.r4.u32 & 0x03FFFFFFu) | (uint32_t(bias & 0x3F) << 26);
  } else {
    const int blur = blur_bias_override.load(std::memory_order_relaxed);
    const uint32_t tex = ctx.r6.u32;
    if (blur != 100 && tex >= 0x10000 && tex < 0xF0000000 && (Rd(base, tex + 0x20) & 0x3F) == 25)
      ctx.r4.u64 = (ctx.r4.u32 & 0x03FFFFFFu) | (uint32_t(blur & 0x3F) << 26);
  }
  if (REXCVAR_GET(gpu_probe)) {
    const uint32_t flags = ctx.r4.u32, rect = ctx.r5.u32, tex = ctx.r6.u32, point = ctx.r7.u32;
    uint32_t d0 = 0, d1 = 0, d2 = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (Ok(tex)) { d0 = Rd(base, tex + 0x1C); d1 = Rd(base, tex + 0x20); d2 = Rd(base, tex + 0x24); }
    if (Ok(rect)) { x0 = Rd(base, rect); y0 = Rd(base, rect + 4); x1 = Rd(base, rect + 8); y1 = Rd(base, rect + 12); }
    const uint64_t key = (uint64_t(flags) << 32) ^ (uint64_t(d1 & 0x3F) << 26) ^ d2 ^ (uint64_t(x1 - x0) << 40) ^ (uint64_t(ctx.lr) << 8);
    static std::mutex lock;
    static std::unordered_map<uint64_t, unsigned> seen;
    std::lock_guard guard(lock);
    auto& count = seen[key];
    if (count++ == 0 && seen.size() <= 96)
      REXLOG_WARN("GPU_PROBE resolve caller={:08X} flags={:08X} src=({},{})-({},{}) destTex={:08X} format={} size={}x{} fetch0={:08X} fetch1={:08X} destPoint={}",
                  uint32_t(ctx.lr), flags, x0, y0, x1, y1, tex, d1 & 0x3F, (d2 & 0x1FFF) + 1, ((d2 >> 13) & 0x1FFF) + 1, d0, d1,
                  Ok(point) ? fmt::format("({},{})", Rd(base, point), Rd(base, point + 4)) : std::string("-"));
  }
  __imp__sub_824C3CF0(ctx, base);
}
