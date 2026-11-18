// Launch timer (diagnostic, --launch_timer=true): logs 0-60 mph, 0-100 mph and
// quarter-mile times for each local player's car, to compare this build with
// the times FM3's own developers recorded (gamedb.slt Data_Car SimTimeTo60MPH,
// SimTimeTo100MPH, SimTimeQuarterMile, SimSpeedQuarterMile).
//
// The game's automated stats tool (CAutomatedCarStatsImp, 8274B858) reads a
// car's speed as |velocity| at CCarDynamics+80 (m/s, x/y/z floats) and uses
// 26.8224 m/s, 44.704 m/s and 402.336 m. Live cars are
// TRefCountedObject<CCarDynamics> (vtable 82001438, ICarDynamics interface at
// +2400). Cars are tracked through their constructor (8274A6D0, sets both
// vtables) and destructor (8274A780). A pointer chain from the player object
// traced in fm3_149 did not hold in fm3_150, so every live car is timed and
// logged by its slot (creation order); with one car on track it is the
// player's.
//
// Sampled once per presented frame (60 Hz) with crossing times interpolated
// between frames, on the real-time clock. A frame whose velocity is exactly
// unchanged is taken as paused and not counted.
#include <algorithm>
#include <bit>
#include <chrono>
#include <string>

#include <fmt/format.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <rex/ppc/context.h>

REXCVAR_DEFINE_BOOL(launch_timer, false, "FM3/Debug",
                    "Log 0-60 mph, 0-100 mph and quarter-mile times from standing starts");

namespace {

using Clock = std::chrono::steady_clock;

uint32_t R(uint8_t* b, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, b + a, 4);
  return std::byteswap(v);
}
float F(uint8_t* b, uint32_t a) { return std::bit_cast<float>(R(b, a)); }

constexpr float kStill = 0.3f;        // m/s: below this the car is standing
constexpr float k60 = 26.8224f;       // 60 mph
constexpr float k100 = 44.704f;       // 100 mph
constexpr float kQuarter = 402.336f;  // m

struct Run {
  uint32_t dyn = 0;
  bool armed = false;    // seen standing still
  bool running = false;  // launched, measuring
  double t = 0;          // seconds since launch
  double dist = 0;       // metres since launch
  float speed = 0;
  float vel[3]{};
  float max_speed = 0;
  double t60 = -1, t100 = -1, tq = -1;
  float vq = 0;
  Clock::time_point last{};
};
constexpr unsigned kSlots = 16;
Run runs[kSlots];
uint32_t live[kSlots];  // car dynamics objects, by slot
std::mutex mutex;

unsigned LiveCount() {
  unsigned n = 0;
  for (auto a : live) n += a != 0;
  return n;
}

void Report(unsigned p, const Run& r, const char* why) {
  auto s = [](double v) { return v < 0 ? std::string("-") : fmt::format("{:.2f} s", v); };
  REXLOG_WARN(
      "LAUNCH_TIMER car {} of {}: 0-60 mph {} | 0-100 mph {} | quarter mile {} at {:.1f} mph | "
      "top {:.1f} mph over {:.0f} m in {:.2f} s ({})",
      p + 1, LiveCount(), s(r.t60), s(r.t100), s(r.tq), r.vq * 2.2369363f, r.max_speed * 2.2369363f, r.dist, r.t,
      why);
}

void Sample(unsigned p, uint8_t* b, Clock::time_point now) {
  Run& r = runs[p];
  const auto dyn = live[p];
  if (dyn != r.dyn) {
    if (r.running && r.t60 >= 0) Report(p, r, "car changed");
    r = {};
    r.dyn = dyn;
    r.last = now;
  }
  if (!dyn) return;
  float v[3] = {F(b, dyn + 80), F(b, dyn + 84), F(b, dyn + 88)};
  const double dt = std::chrono::duration<double>(now - r.last).count();
  r.last = now;
  const bool frozen = std::memcmp(v, r.vel, sizeof v) == 0;
  std::memcpy(r.vel, v, sizeof v);
  if (frozen || dt <= 0 || dt > 0.25) return;  // paused, or a hitch/loading
  const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  const float prev = r.speed;
  r.speed = speed;
  if (!std::isfinite(speed)) return;

  if (!r.running) {
    if (speed < kStill) {
      r.armed = true;
      return;
    }
    if (!r.armed) return;
    // Launch: start the clock where speed crossed kStill.
    r.running = true;
    const double frac = (speed - kStill) / std::max(1e-3f, speed - prev);
    r.t = std::min(dt, dt * frac);
    r.dist = 0.5 * speed * r.t;
    r.max_speed = speed;
    return;
  }

  const double t0 = r.t;
  r.t += dt;
  const double d0 = r.dist;
  r.dist += 0.5 * (prev + speed) * dt;
  auto cross = [&](float target) {
    const double f = (target - prev) / std::max(1e-4f, speed - prev);
    return t0 + dt * std::clamp(f, 0.0, 1.0);
  };
  if (r.t60 < 0 && prev < k60 && speed >= k60) r.t60 = cross(k60);
  if (r.t100 < 0 && prev < k100 && speed >= k100) r.t100 = cross(k100);
  if (r.tq < 0 && r.dist >= kQuarter) {
    const double f = (kQuarter - d0) / std::max(1e-6, r.dist - d0);
    r.tq = t0 + dt * f;
    r.vq = prev + float(f) * (speed - prev);
  }
  r.max_speed = std::max(r.max_speed, speed);

  if (r.tq >= 0) {
    Report(p, r, "complete");
    r.running = false;
    r.armed = false;
  } else if (speed < r.max_speed - 2.0f || speed < kStill) {
    // Lifted or braked before the quarter mile.
    if (r.t60 >= 0) Report(p, r, "lifted before the quarter mile");
    r.running = false;
    r.armed = speed < kStill;
  }
}

}  // namespace

namespace fm3 {
void LaunchTimerFrame(uint8_t* base) {
  if (!REXCVAR_GET(launch_timer)) return;
  const auto now = Clock::now();
  std::lock_guard lock(mutex);
  for (unsigned p = 0; p < kSlots; ++p) Sample(p, base, now);
}
}  // namespace fm3

extern "C" void __imp__sub_8274A6D0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8274A780(PPCContext& __restrict ctx, uint8_t* base);

// TRefCountedObject<CCarDynamics> constructor (this in r3).
extern "C" void sub_8274A6D0(PPCContext& __restrict ctx, uint8_t* base) {
  const auto self = ctx.r3.u32;
  __imp__sub_8274A6D0(ctx, base);
  std::lock_guard lock(mutex);
  for (auto& a : live) if (a == self) return;
  for (auto& a : live) if (!a) { a = self; return; }
}
// Its destructor (this in r3).
extern "C" void sub_8274A780(PPCContext& __restrict ctx, uint8_t* base) {
  {
    std::lock_guard lock(mutex);
    for (auto& a : live) if (a == ctx.r3.u32) a = 0;
  }
  __imp__sub_8274A780(ctx, base);
}
