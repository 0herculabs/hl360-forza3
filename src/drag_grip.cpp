// Drag strip rear grip boost (--drag_strip_grip=true, --drag_strip_grip_scale).
//
// FM3's tire force code 822009A8(car r3, wheel r4, ...) looks up the surface
// under the wheel with 82201168(surface manager, index) and multiplies the
// wheel's grip by the surface record's FrictionScale (record+4). The Autopark
// drag strips are plain Concrete (1.0), so a launch can never lift the front:
// a wheelie needs rear grip of about (CG to rear axle) / (CG height), 2.5-3x.
//
// With the option on, when that lookup comes from 822009A8 (return address
// 82200A20) for a rear wheel (CCarDynamics+5840 + k*1040, k = 2 or 3) and the
// loaded track is Autopark Ribbon_06/07/08 (the 1/8, 1/4 and 1/2 mile strips),
// the lookup returns a copy of the surface record with FrictionScale times the
// scale. The real surface table is not changed, so every other track, every
// other use of the surface (sound, effects, AI) and the front wheels stay as
// they were.
//
// The loaded track's names are in the track object held by the global
// 82C7AF44 (created once by 828BB6A8, vtable 821080F4): std::string track at
// +0x20 ("Autopark") and ribbon at +0x3C ("Ribbon_07").
#include <atomic>
#include <cctype>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(drag_strip_grip, false, "FM3",
                    "Multiply rear tire grip on the Autopark drag strips by drag_strip_grip_scale");
REXCVAR_DEFINE_DOUBLE(drag_strip_grip_scale, 3.0, "FM3",
                      "Rear grip multiplier on the drag strips when drag_strip_grip is on");

extern "C" void __imp__sub_82201168(PPCContext& __restrict ctx, uint8_t* base);

namespace {

constexpr uint32_t kTireForceReturn = 0x82200A20;  // after bl 82201168 in 822009A8
constexpr uint32_t kTrackGlobal = 0x82C7AF44;
constexpr uint32_t kTrackVtable = 0x821080F4;
constexpr uint32_t kRecordSize = 240;
constexpr uint32_t kSlots = 32;
constexpr uint32_t kSlotSize = 256;

uint32_t R(uint8_t* b, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, b + a, 4);
  return std::byteswap(v);
}

// Xbox 360 MSVC std::string: +4 inline buffer (or pointer when cap >= 16),
// +0x14 size, +0x18 capacity.
std::string GuestString(uint8_t* b, uint32_t s) {
  const uint32_t size = R(b, s + 0x14), cap = R(b, s + 0x18);
  if (size > 64 || cap < size) return {};
  const uint32_t data = cap >= 16 ? R(b, s + 4) : s + 4;
  if (!data) return {};
  return std::string(reinterpret_cast<const char*>(b + data), size);
}

bool IEquals(const std::string& a, const char* b) {
  const size_t n = std::strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}

// Track object pointer + ribbon name size identify a track load; re-read the
// names only when those change.
bool OnDragStrip(uint8_t* b) {
  static uint32_t last_obj = 0, last_key = ~0u;
  static bool last_result = false;
  const uint32_t obj = R(b, kTrackGlobal);
  if (!obj || R(b, obj) != kTrackVtable) return false;
  const uint32_t key = R(b, obj + 0x40) ^ R(b, obj + 0x44) ^ R(b, obj + 0x48) ^ R(b, obj + 0x50) ^
                       R(b, obj + 0x24) ^ R(b, obj + 0x28);
  if (obj == last_obj && key == last_key) return last_result;
  const std::string track = GuestString(b, obj + 0x20), ribbon = GuestString(b, obj + 0x3C);
  const bool drag = IEquals(track, "Autopark") &&
                    (IEquals(ribbon, "Ribbon_06") || IEquals(ribbon, "Ribbon_07") ||
                     IEquals(ribbon, "Ribbon_08"));
  if (drag != last_result || obj != last_obj) {
    if (drag)
      REXLOG_WARN("DRAG_GRIP on: {} {} rear grip x{:.2f}", track, ribbon,
                  REXCVAR_GET(drag_strip_grip_scale));
    else if (last_result)
      REXLOG_WARN("DRAG_GRIP off: {} {}", track, ribbon);
  }
  last_obj = obj;
  last_key = key;
  last_result = drag;
  return drag;
}

uint32_t SlotBase() {
  static const uint32_t slots =
      rex::Runtime::instance()->memory()->SystemHeapAlloc(kSlots * kSlotSize, 0x20);
  return slots;
}
std::atomic<uint32_t> next_slot{0};

}  // namespace

extern "C" void sub_82201168(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t ret = static_cast<uint32_t>(ctx.lr);
  __imp__sub_82201168(ctx, base);
  if (ret != kTireForceReturn || !REXCVAR_GET(drag_strip_grip)) return;
  const uint32_t record = ctx.r3.u32;
  if (!record) return;
  // In 822009A8 at this call: r28 = car dynamics, r31 = wheel.
  const uint32_t car = ctx.r28.u32, wheel = ctx.r31.u32;
  if (wheel < car + 5840) return;
  const uint32_t off = wheel - car - 5840;
  if (off % 1040 != 0 || off / 1040 < 2 || off / 1040 > 3) return;
  if (!OnDragStrip(base)) return;
  const uint32_t slots = SlotBase();
  if (!slots) return;

  const uint32_t copy = slots + (next_slot.fetch_add(1, std::memory_order_relaxed) % kSlots) * kSlotSize;
  std::memcpy(base + copy, base + record, kRecordSize);
  const float scaled =
      std::bit_cast<float>(R(base, record + 4)) * static_cast<float>(REXCVAR_GET(drag_strip_grip_scale));
  const uint32_t v = std::byteswap(std::bit_cast<uint32_t>(scaled));
  std::memcpy(base + copy + 4, &v, 4);
  ctx.r3.u64 = copy;
}
