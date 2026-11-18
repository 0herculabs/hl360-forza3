// Offline inspection of Career > Go Race > Multiplayer.
// This changes only the menu-entry result, not XAM privileges or sign-in.
#include <bit>
#include <cstring>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_BOOL(multiplayer_menu_preview, false, "FM3/Research",
    "Allow viewing the stock Multiplayer menu offline; does not enable Xbox LIVE");

extern "C" void __imp__sub_8230D098(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_8230D098(PPCContext& __restrict ctx, uint8_t* base) {
  const auto caller = static_cast<uint32_t>(ctx.lr);
  const auto reason_address = ctx.r3.u32;
  const auto operation = ctx.r4.u32;
  __imp__sub_8230D098(ctx, base);
  if (caller != 0x823933EC || operation != 4) return;
  const auto result = ctx.r3.u32;
  uint32_t reason = 0;
  if (!result) {
    std::memcpy(&reason, base + reason_address, sizeof(reason));
    reason = std::byteswap(reason);
  }
  // Only these reasons lead to IDS_MatchmakingBlocked in 823933D0.
  if (REXCVAR_GET(multiplayer_menu_preview) && !result &&
      (reason == 1 || reason == 16)) {
    const uint32_t clear = 0;
    std::memcpy(base + reason_address, &clear, sizeof(clear));
    ctx.r3.u64 = 1;
  }
  REXLOG_WARN("MULTIPLAYER_MENU career gate caller={:08X} operation={} original_result={} reason={} effective_result={} preview={}",
      caller, operation, result, reason, ctx.r3.u32, REXCVAR_GET(multiplayer_menu_preview));
}

extern "C" void __imp__sub_823933D0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_823933D0(PPCContext& __restrict ctx, uint8_t* base) {
  const auto caller = static_cast<uint32_t>(ctx.lr);
  __imp__sub_823933D0(ctx, base);
  REXLOG_WARN("MULTIPLAYER_MENU career transition caller={:08X} result={}", caller, ctx.r3.u32);
}

extern "C" void __imp__sub_82363AE0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82363AE0(PPCContext& __restrict ctx, uint8_t* base) {
  REXLOG_WARN("MULTIPLAYER_MENU matchmaking blocked dialog caller={:08X}", static_cast<uint32_t>(ctx.lr));
  __imp__sub_82363AE0(ctx, base);
}

extern "C" void __imp__sub_828D90A8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_828D90A8(PPCContext& __restrict ctx, uint8_t* base) {
  const auto caller = static_cast<uint32_t>(ctx.lr);
  __imp__sub_828D90A8(ctx, base);
  if (caller != 0x82499908) return;
  const auto result = ctx.r3.u32;
  // PartyLobbyHostingTransition already created its local player. Its
  // transport-host failure otherwise leaves the "Creating session" modal
  // indefinitely. Permit only the UI preview to reach its idle state.
  // This does not provide a working transport or a playable private lobby.
  if (REXCVAR_GET(multiplayer_menu_preview) && (result & 0x80000000u))
    ctx.r3.u64 = 0;
  REXLOG_WARN("MULTIPLAYER_MENU party host caller={:08X} original_result={:08X} effective_result={:08X}; offline UI preview only",
      caller, result, ctx.r3.u32);
}
