// Optional: swap which physical controller is Player 1 and which is Player 2.
//
// The title reads pads through three imported functions, each taking the
// controller index as its first argument. With swap_controllers on, index 0
// and 1 are exchanged before the runtime's own function runs. Everything else
// (profiles, saves, garages) stays with the same player number.
#include <cstdlib>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_BOOL(swap_controllers, false, "FM3/Input",
    "Swap the first two controllers (the second pad becomes Player 1)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
using GuestFn = void (*)(PPCContext&, uint8_t*);

GuestFn Original(const char* name) {
#ifdef _WIN32
  static const HMODULE runtime = GetModuleHandleW(L"rexruntime.dll");
  auto fn = runtime ? reinterpret_cast<GuestFn>(GetProcAddress(runtime, name)) : nullptr;
  if (fn) return fn;
#endif
  REXLOG_ERROR("CONTROLLER_SWAP runtime export {} not found", name);
  std::abort();
}

void Swap(PPCContext& ctx) {
  static const bool enabled = REXCVAR_GET(swap_controllers);
  if (enabled && ctx.r3.u32 < 2) ctx.r3.u64 = 1 - ctx.r3.u32;
}
}  // namespace

#define FM3_SWAPPED_INPUT(name)                                   \
  extern "C" void __imp__##name(PPCContext& ctx, uint8_t* base) { \
    static const GuestFn original = Original("__imp__" #name);    \
    Swap(ctx);                                                    \
    original(ctx, base);                                          \
  }

FM3_SWAPPED_INPUT(XamInputGetState)
FM3_SWAPPED_INPUT(XamInputSetState)
FM3_SWAPPED_INPUT(XamInputGetCapabilities)
