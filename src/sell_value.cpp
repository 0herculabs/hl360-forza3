// Reward cars sell like bought cars (--reward_car_sell_value=true).
//
// FM3 values a garage car in 822882E8(garage, car): it reads the car's record
// into a local buffer (822868E8) and, when bit 1 of record+748 marks a reward
// car (driver-level gift), takes CarValueScales\RewardCarsValue (a flat 100 CR,
// tunables +40); otherwise it takes the car's price (8227E410) times
// SellValueScale (0.5, +32). It then adds the parts value (8227EE20) times
// RestockingValueScale (0.5, +36). Tunables = [[82C2111C]+504] (82258400).
//
// With the option on, this replacement runs the same steps but always takes
// the price-times-SellValueScale path, so a reward car sells for half its value
// like any other car. Same calls, same float maths as the original.
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_BOOL(reward_car_sell_value, false, "FM3",
                    "Reward cars sell for SellValueScale of their price like bought cars, "
                    "instead of the flat RewardCarsValue (100 CR)");

extern "C" void __imp__sub_822882E8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_822868E8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_8227E410(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_8227EE20(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82236A58(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_826EDA80(PPCContext& __restrict ctx, uint8_t* base);

namespace {
uint32_t R(uint8_t* b, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, b + a, 4);
  return __builtin_bswap32(v);
}
void W(uint8_t* b, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(b + a, &v, 4);
}
float F(uint8_t* b, uint32_t a) {
  const uint32_t v = R(b, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
// fctidz then low word, as the original stores and reloads it.
uint32_t ToInt(float f) { return static_cast<uint32_t>(static_cast<int64_t>(f)); }
}  // namespace

extern "C" void sub_822882E8(PPCContext& __restrict ctx, uint8_t* base) {
  if (!REXCVAR_GET(reward_car_sell_value)) {
    __imp__sub_822882E8(ctx, base);
    return;
  }
  const uint32_t garage = ctx.r3.u32, car = ctx.r4.u32;
  const uint64_t lr = ctx.lr;
  const uint32_t sp = ctx.r1.u32, frame = sp - 2112;
  W(base, frame, sp);  // stwu r1,-2112(r1)
  ctx.r1.u64 = frame;

  ctx.r3.u64 = frame + 96;
  ctx.r4.u64 = garage;
  ctx.r5.u64 = car;
  sub_822868E8(ctx, base);
  const bool reward = (R(base, frame + 844) >> 1) & 1;

  const uint32_t tunables = R(base, R(base, 0x82C2111C) + 504);
  ctx.r3.u64 = garage;
  ctx.r4.u64 = R(base, frame + 152);
  sub_8227E410(ctx, base);
  const uint32_t price = ctx.r3.u32;
  const uint32_t value = ToInt(static_cast<float>(static_cast<double>(price)) * F(base, tunables + 32));

  ctx.r3.u64 = frame + 96;
  sub_8227EE20(ctx, base);
  const uint32_t parts = ctx.r3.u32;
  const uint32_t total =
      ToInt(std::fmaf(static_cast<float>(static_cast<double>(parts)), F(base, tunables + 36),
                      static_cast<float>(static_cast<double>(value))));

  ctx.r3.u64 = frame + 152;
  sub_82236A58(ctx, base);
  ctx.r3.u64 = frame + 96;
  sub_826EDA80(ctx, base);

  if (reward) {
    REXLOG_WARN("SELL_VALUE reward car {:08X}: price {} x {} + parts {} x {} = {} CR (game rule: {} CR + parts)",
                car, price, F(base, tunables + 32), parts, F(base, tunables + 36), total,
                R(base, tunables + 40));
  }
  ctx.r1.u64 = sp;
  ctx.lr = lr;
  ctx.r3.u64 = total;
}
