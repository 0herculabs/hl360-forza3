// The XDK fence wait at 824B5008 repeatedly calls 824AAA68. The latter's
// cctpl/db16cyc/cctpm instructions are omitted by codegen, so its deliberate
// console backoff becomes a tight host loop. Retain the entire guest routine
// and its return value; add host backoff only at this verified fence caller.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <rex/cvar.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_BOOL(gpu_wait_backoff, true, "FM3/Performance",
    "Pause and yield during the XDK graphics fence polling loop")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Sleep through the predictable part of the fence wait instead of spinning.
// Measured in a two-player race: the frame thread spends 7-10 ms of every
// 16.7 ms frame in this loop and shows as one core at 100%. The wait begins
// in 824AA958 and ends in 824AA988; each thread keeps a running estimate of
// how long its waits last, sleeps in 0.5 ms steps (high-resolution waitable
// timer) until 2 ms before that estimate, then polls as before. A wait that
// turns out shorter than predicted finishes late by at most the difference,
// and pulls the estimate down at once.
REXCVAR_DEFINE_BOOL(gpu_wait_sleep, false, "FM3/Performance",
    "Sleep through most of the graphics fence wait instead of spinning (lower CPU use)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
using WaitClock = std::chrono::steady_clock;
struct WaitState {
  WaitClock::time_point start{};
  // One estimate per call site of the wait routine 824B4F78: the two sites
  // on the frame thread last very different times (about 1 ms and 9 ms).
  struct Site { uint32_t caller = 0; double estimate_ms = 0; } sites[6];
  Site* site = nullptr;
  bool active = false;
#ifdef _WIN32
  HANDLE timer = nullptr;
#endif
};
thread_local WaitState wait_state;
bool SleepEnabled() {
  static const bool enabled = REXCVAR_GET(gpu_wait_sleep);
  return enabled;
}
double ElapsedMs(const WaitState& w) {
  return std::chrono::duration<double, std::milli>(WaitClock::now() - w.start).count();
}
}  // namespace

extern "C" void __imp__sub_824AA958(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_824AA958(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_824AA958(ctx, base);
  if (!SleepEnabled()) return;
  wait_state.site = nullptr;
  wait_state.active = false;
  if (ctx.lr != 0x824B5004) return;  // only the fence loop inside 824B4F78
  // 824B4F78 saved its own return address at (its frame + 144) - 8.
  uint32_t caller;
  std::memcpy(&caller, base + ctx.r1.u32 + 136, 4);
  caller = _byteswap_ulong(caller);
  for (auto& site : wait_state.sites) {
    if (site.caller == 0) site.caller = caller;
    if (site.caller == caller) { wait_state.site = &site; break; }
  }
  if (!wait_state.site) return;
  wait_state.start = WaitClock::now();
  wait_state.active = true;
}
extern "C" void __imp__sub_824AA988(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_824AA988(PPCContext& __restrict ctx, uint8_t* base) {
  if (SleepEnabled() && wait_state.active) {
    const double took = ElapsedMs(wait_state);
    auto& estimate = wait_state.site->estimate_ms;
    // Drop to a shorter wait immediately; creep up slowly.
    estimate = took < estimate ? took : estimate + (took - estimate) * 0.1;
    wait_state.active = false;
  }
  __imp__sub_824AA988(ctx, base);
}

extern "C" void __imp__sub_824AAA68(PPCContext& __restrict ctx, uint8_t* base);

extern "C" void sub_824AAA68(PPCContext& __restrict ctx, uint8_t* base) {
  const auto caller = ctx.lr;
  __imp__sub_824AAA68(ctx, base);
  static const bool enabled = REXCVAR_GET(gpu_wait_backoff);
  if (!enabled || caller != 0x824B5010 || ctx.r3.u32 == 0) return;

#ifdef _WIN32
  if (SleepEnabled() && wait_state.active && wait_state.site->estimate_ms - ElapsedMs(wait_state) > 2.0) {
    auto& timer = wait_state.timer;
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    LARGE_INTEGER due; due.QuadPart = -5000;  // 0.5 ms
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer, 5);
      return;
    }
  }
#endif

  // Short pauses allow the SMT sibling to make progress without introducing
  // a millisecond sleep or changing guest timeouts, memory or register state.
  for (unsigned i = 0; i < 8; ++i) _mm_pause();
#ifdef _WIN32
  // Periodically offer runnable host threads this CPU. SwitchToThread does
  // not wait for a timer tick and returns immediately if nobody can run.
  static thread_local unsigned polls = 0;
  if ((++polls & 63u) == 0) SwitchToThread();
#endif
}
