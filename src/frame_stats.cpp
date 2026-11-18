// Frame pacing statistics (opt-in: --frame_stats=true), as in the FM2
// recompilation.
//
// Every frame the title ends in 0x824AF280, the XDK present path that calls
// VdSwap (at 0x824AF4F4). Wrapping it gives the real frame interval as the game
// sees it. Every 5 seconds a summary goes to the log: fps, average / 99th
// percentile / worst frame time, and how many frames missed 1/60 s and 1/30 s.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_BOOL(frame_stats, false, "FM3/Debug", "Log frame pacing statistics every 5 seconds");

extern "C" void __imp__sub_824AF280(PPCContext& __restrict ctx, uint8_t* base);

namespace {

using Clock = std::chrono::steady_clock;

struct Stats {
  Clock::time_point window_start{};
  Clock::time_point last{};
  std::vector<float> frame_ms;
};

// Per-caller time spent in the XDK graphics wait 824B4F78 (the loop that polls
// 824AAA68 until the emulated GPU has caught up). Any guest thread may wait,
// so the totals are atomics; the frame thread drains them with each summary.
struct WaitSlot {
  std::atomic<uint32_t> caller{0};
  std::atomic<uint64_t> ns{0};
  std::atomic<uint32_t> calls{0};
};
constexpr size_t kWaitSlots = 24;  // 15 call sites in the title
WaitSlot wait_slots[kWaitSlots];

void RecordWait(uint32_t caller, uint64_t ns) {
  for (auto& slot : wait_slots) {
    uint32_t seen = slot.caller.load(std::memory_order_relaxed);
    if (seen == 0 && slot.caller.compare_exchange_strong(seen, caller)) seen = caller;
    if (seen != caller) continue;
    slot.ns.fetch_add(ns, std::memory_order_relaxed);
    slot.calls.fetch_add(1, std::memory_order_relaxed);
    return;
  }
}

void LogWaits(size_t frames) {
  struct Row { uint32_t caller; double ms; uint32_t calls; };
  Row rows[kWaitSlots];
  size_t count = 0;
  double total_ms = 0;
  for (auto& slot : wait_slots) {
    const uint32_t caller = slot.caller.load(std::memory_order_relaxed);
    const uint32_t calls = slot.calls.exchange(0, std::memory_order_relaxed);
    const double ms = double(slot.ns.exchange(0, std::memory_order_relaxed)) / 1e6;
    if (!caller || !calls) continue;
    rows[count++] = {caller, ms, calls};
    total_ms += ms;
  }
  if (!count || !frames) return;
  std::sort(rows, rows + count, [](const Row& a, const Row& b) { return a.ms > b.ms; });
  std::string detail;
  for (size_t i = 0; i < count && i < 6; ++i) {
    detail += fmt::format(" | {:08X}: {:.2f} ms/frame, {:.1f} calls/frame", rows[i].caller,
                          rows[i].ms / double(frames), double(rows[i].calls) / double(frames));
  }
  REXLOG_WARN("gpu wait: {:.2f} ms/frame in 824B4F78 by caller{}", total_ms / double(frames),
              detail);
}

}  // namespace

extern "C" void __imp__sub_824B4F78(PPCContext& __restrict ctx, uint8_t* base);

// Diagnostic only: the original routine runs unchanged; with frame_stats off
// this adds nothing but the flag test.
extern "C" void sub_824B4F78(PPCContext& __restrict ctx, uint8_t* base) {
  if (!REXCVAR_GET(frame_stats)) {
    __imp__sub_824B4F78(ctx, base);
    return;
  }
  const auto caller = static_cast<uint32_t>(ctx.lr);
  const auto start = Clock::now();
  __imp__sub_824B4F78(ctx, base);
  RecordWait(caller, uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                         Clock::now() - start).count()));
}

// Overrides the weak alias the generated code calls.
extern "C" void sub_824AF280(PPCContext& __restrict ctx, uint8_t* base) {
  if (!REXCVAR_GET(frame_stats)) {
    __imp__sub_824AF280(ctx, base);
    return;
  }
  static Stats stats;
  const auto now = Clock::now();
  if (stats.last.time_since_epoch().count()) {
    stats.frame_ms.push_back(std::chrono::duration<float, std::milli>(now - stats.last).count());
  } else {
    stats.window_start = now;
  }
  stats.last = now;

  const float window_s = std::chrono::duration<float>(now - stats.window_start).count();
  if (window_s >= 5.0f && !stats.frame_ms.empty()) {
    auto sorted = stats.frame_ms;
    std::sort(sorted.begin(), sorted.end());
    float sum = 0;
    int over60 = 0, over30 = 0;
    for (float ms : sorted) {
      sum += ms;
      over60 += ms > 17.5f;
      over30 += ms > 34.0f;
    }
    const size_t n = sorted.size();
    // Before the frame line, so tools that read the last "frame stats" entry
    // are unaffected.
    LogWaits(n);
    REXLOG_WARN(
        "frame stats: {} frames in {:.1f}s = {:.1f} fps | avg {:.2f} ms, p99 {:.2f} ms, max {:.2f} ms | "
        ">1/60s: {} | >1/30s: {}",
        n, window_s, float(n) / window_s, sum / float(n), sorted[std::min(n - 1, n * 99 / 100)],
        sorted.back(), over60, over30);
    stats.frame_ms.clear();
    stats.window_start = now;
  }
  __imp__sub_824AF280(ctx, base);
}
