// EXPERIMENT: the box under the followed car.
//
// Each view names one "shadow focus car" every frame through
// IPresentationView::SetShadowFocusCarIndex (vtables 82001D4C / 82101B4C,
// function 827FB840, stores the index at view+464). That car gets a projected
// shadow-map shadow; every other car gets the simple drop shadow. Under this
// runtime the projected one comes out as a solid dark box (upstream issues
// 10 and 13), while the drop shadows are fine: the same car looks right on the
// other player's screen.
//
// focus_car_shadow_index replaces the index the game passes:
//   -2 = leave alone (default), -1 = no focus car, n = force car n.
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_INT32(focus_car_shadow_index, -2, "FM3/Research",
    "EXPERIMENT: override the per-view shadow focus car (-2 = game's choice, -1 = none)");

extern "C" void __imp__sub_827FB840(PPCContext& ctx, uint8_t* base);
extern "C" void sub_827FB840(PPCContext& ctx, uint8_t* base) {
  const int32_t wanted = REXCVAR_GET(focus_car_shadow_index);
  if (wanted != -2) {
    static std::atomic<unsigned> notes{0};
    if (notes.fetch_add(1) < 6)
      REXLOG_WARN("FOCUS_SHADOW view {:08X}: game asked for car {}, using {}", ctx.r3.u32, ctx.r4.s32, wanted);
    ctx.r4.s64 = wanted;
  }
  __imp__sub_827FB840(ctx, base);
}

// The title's developer switches (rendershadows, fastdropshadows, ...) live as
// bytes in the static CForzaCommandLineParameters object at 82C21C60
// (vtable 820080D8); 8225EF78 binds 256 option names to its fields. Offsets
// are listed in work/game_settings_offsets.json, e.g. rendershadows 2370,
// fastdropshadows 2389, fastcastshadows 2390, fastcarrender 2391,
// perfmode 2388, postproc 2348. Flipping them in a running race had no visible
// effect, so they are probably sampled when a race loads.
//
// game_settings_bytes = "offset=value,offset=value" writes those bytes every
// frame from startup, for experiments.
REXCVAR_DEFINE_STRING(game_settings_bytes, "", "FM3/Research",
    "EXPERIMENT: byte overrides in the game's developer-switch object, as offset=value pairs");

namespace fm3 {
void ApplyGameSettingBytes(uint8_t* base) {
  struct Pair { uint32_t offset; uint8_t value; };
  static const std::vector<Pair> pairs = [] {
    std::vector<Pair> out;
    const std::string text = REXCVAR_GET(game_settings_bytes);
    size_t at = 0;
    while (at < text.size()) {
      const size_t end = std::min(text.find(',', at), text.size());
      const size_t eq = text.find('=', at);
      if (eq != std::string::npos && eq < end) {
        const unsigned long offset = std::strtoul(text.substr(at, eq - at).c_str(), nullptr, 10);
        const unsigned long value = std::strtoul(text.substr(eq + 1, end - eq - 1).c_str(), nullptr, 10);
        if (offset >= 1024 && offset < 3000 && value < 256) out.push_back({uint32_t(offset), uint8_t(value)});
      }
      at = end + 1;
    }
    for (const auto& p : out) REXLOG_WARN("GAME_SETTINGS override byte +{} = {}", p.offset, p.value);
    return out;
  }();
  for (const auto& p : pairs) base[0x82C21C60 + p.offset] = p.value;
}
}  // namespace fm3
