// EXPERIMENT: a real second signed-in profile for local co-op.
//
// The runtime (rexruntime.dll) models exactly one signed-in user: every
// XamUser* query for user index > 0 answers "no such user", and the
// XamContent* functions ignore the user index and always use the single
// profile's XUID. FM3 therefore only ever sees Player 2 as a guest, whose
// profile carries no Career data.
//
// The title reaches those services through a handful of imported functions
// (__imp__Xam*). Defining functions with the same names here makes the linker
// use these instead of the DLL's; each one answers for user index 1 and passes
// everything else to the original DLL export, looked up by name at run time.
// rexruntime.dll itself is not modified.
//
// Saves: for user index 1 the single profile's XUID is switched to Player 2's
// for the duration of the original content call, under a lock, so Player 2's
// content lands in its own <user root>/<xuid> folder next to Player 1's.
#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <cstring>
#include <mutex>
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
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

REXCVAR_DEFINE_BOOL(player2_real_profile, false, "FM3/Research",
    "EXPERIMENT: report controller 2 as a second signed-in profile with its own saves")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(player2_profile_xuid, "", "FM3/Research",
    "Offline XUID of the second profile, 16 hex digits (empty = Player 1's XUID + 1)");
REXCVAR_DECLARE(std::string, player2_profile_name);

namespace {

using GuestFn = void (*)(PPCContext&, uint8_t*);
constexpr uint32_t kSecondUser = 1;

GuestFn Original(const char* name) {
#ifdef _WIN32
  static const HMODULE runtime = GetModuleHandleW(L"rexruntime.dll");
  auto fn = runtime ? reinterpret_cast<GuestFn>(GetProcAddress(runtime, name)) : nullptr;
  if (!fn) {
    REXLOG_ERROR("SECOND_PROFILE runtime export {} not found", name);
    std::abort();
  }
  return fn;
#else
  (void)name;
  std::abort();
#endif
}

bool Enabled() {
  static const bool enabled = REXCVAR_GET(player2_real_profile);
  return enabled;
}

rex::system::xam::UserProfile* Profile() { return REX_KERNEL_STATE()->user_profile(); }

// Player 1's XUID, captured before any temporary switch.
uint64_t HostXuid() {
  static const uint64_t host = Profile()->xuid();
  return host;
}

uint64_t SecondXuid() {
  static const uint64_t xuid = [] {
    const uint64_t host = HostXuid();
    const std::string text = REXCVAR_GET(player2_profile_xuid);
    uint64_t parsed = 0;
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed, 16);
    if (!text.empty() && ec == std::errc() && end == text.data() + text.size() &&
        (parsed & ~0x00C0000000000000ull) != host) {
      return parsed & ~0x00C0000000000000ull;
    }
    return host + 1;
  }();
  return xuid;
}

std::string SecondName() {
  std::string name = REXCVAR_GET(player2_profile_name);
  if (name.empty()) name = "Player 2";
  return name.substr(0, 15);
}

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = std::byteswap(value);
  std::memcpy(base + address, &value, 4);
}
void Store64(uint8_t* base, uint32_t address, uint64_t value) {
  value = std::byteswap(value);
  std::memcpy(base + address, &value, 8);
}

void Note(const char* what) {
  static std::atomic<unsigned> count{0};
  if (count.fetch_add(1) < 40) REXLOG_WARN("SECOND_PROFILE {}", what);
}

std::mutex content_lock;

// Run one XamContent* export. For Player 2 the profile's XUID is switched for
// the call (the runtime captures it by value when the call is made).
void ContentCall(const char* name, GuestFn original, PPCContext& ctx, uint8_t* base) {
  std::lock_guard lock(content_lock);
  if (!Enabled() || ctx.r3.u32 != kSecondUser) {
    original(ctx, base);
    return;
  }
  const uint64_t host = HostXuid();
  auto* profile = Profile();
  profile->set_xuid(SecondXuid());
  ctx.r3.u64 = 0;
  original(ctx, base);
  profile->set_xuid(host);
  static std::atomic<unsigned> count{0};
  if (count.fetch_add(1) < 40)
    REXLOG_WARN("SECOND_PROFILE {} for user 1 -> result {:08X} (XUID {:016X})", name, ctx.r3.u32, SecondXuid());
}

}  // namespace

// u32 XamUserGetSigninState(u32 user_index)
extern "C" void __imp__XamUserGetSigninState(PPCContext& ctx, uint8_t* base) {
  static const GuestFn original = Original("__imp__XamUserGetSigninState");
  if (Enabled()) HostXuid();
  if (Enabled() && ctx.r3.u32 == kSecondUser) {
    ctx.r3.u64 = 1;  // signed in locally
    Note("XamUserGetSigninState(1) -> signed in");
    return;
  }
  original(ctx, base);
}

// i32 XamUserGetXUID(u32 user_index, u32 type_mask, be<u64>* xuid)
extern "C" void __imp__XamUserGetXUID(PPCContext& ctx, uint8_t* base) {
  static const GuestFn original = Original("__imp__XamUserGetXUID");
  if (Enabled() && ctx.r3.u32 == kSecondUser && ctx.r5.u32) {
    Store64(base, ctx.r5.u32, SecondXuid());
    ctx.r3.u64 = 0;
    Note("XamUserGetXUID(1)");
    return;
  }
  original(ctx, base);
}

// u32 XamUserGetName(u32 user_index, char* buffer, u32 buffer_len)
extern "C" void __imp__XamUserGetName(PPCContext& ctx, uint8_t* base) {
  static const GuestFn original = Original("__imp__XamUserGetName");
  if (Enabled() && ctx.r3.u32 == kSecondUser && ctx.r4.u32 && ctx.r5.u32) {
    const auto name = SecondName();
    const size_t room = std::min<size_t>(ctx.r5.u32, 16);
    const size_t length = std::min(name.size(), room - 1);
    std::memcpy(base + ctx.r4.u32, name.data(), length);
    base[ctx.r4.u32 + length] = 0;
    ctx.r3.u64 = 0;
    Note("XamUserGetName(1)");
    return;
  }
  original(ctx, base);
}

// i32 XamUserGetSigninInfo(u32 user_index, u32 flags, X_USER_SIGNIN_INFO* info)
// { be64 xuid; be32 unk; be32 signin_state; be32 unk; be32 unk; char name[16]; }
extern "C" void __imp__XamUserGetSigninInfo(PPCContext& ctx, uint8_t* base) {
  static const GuestFn original = Original("__imp__XamUserGetSigninInfo");
  if (Enabled() && ctx.r3.u32 == kSecondUser && ctx.r5.u32) {
    const auto info = ctx.r5.u32;
    std::memset(base + info, 0, 40);
    Store64(base, info, SecondXuid());
    Store32(base, info + 12, 1);
    const auto name = SecondName();
    std::memcpy(base + info + 24, name.data(), std::min<size_t>(name.size(), 15));
    ctx.r3.u64 = 0;
    Note("XamUserGetSigninInfo(1)");
    return;
  }
  original(ctx, base);
}

// Settings, privileges and friends have no per-user data in this runtime;
// answer for Player 2 as it answers for Player 1.
// u32 XamUserReadProfileSettings(u32 title_id, u32 user_index, ...)
extern "C" void __imp__XamUserReadProfileSettings(PPCContext& ctx, uint8_t* base) {
  static const GuestFn original = Original("__imp__XamUserReadProfileSettings");
  if (Enabled() && ctx.r4.u32 == kSecondUser) ctx.r4.u64 = 0;
  original(ctx, base);
}
// u32 XamUserCheckPrivilege(u32 user_index, u32 mask, be<u32>* out)
extern "C" void __imp__XamUserCheckPrivilege(PPCContext& ctx, uint8_t* base) {
  static const GuestFn original = Original("__imp__XamUserCheckPrivilege");
  if (Enabled() && ctx.r3.u32 == kSecondUser) ctx.r3.u64 = 0;
  original(ctx, base);
}

#define FM3_SECOND_PROFILE_CONTENT(name)                                  \
  extern "C" void __imp__##name(PPCContext& ctx, uint8_t* base) {         \
    static const GuestFn original = Original("__imp__" #name);            \
    ContentCall(#name, original, ctx, base);                              \
  }

// Each takes the user index as its first argument.
FM3_SECOND_PROFILE_CONTENT(XamContentCreateEx)
FM3_SECOND_PROFILE_CONTENT(XamContentCreateEnumerator)
FM3_SECOND_PROFILE_CONTENT(XamContentDelete)
FM3_SECOND_PROFILE_CONTENT(XamContentGetCreator)
FM3_SECOND_PROFILE_CONTENT(XamContentResolve)

// The per-user profile loader. 8226A5E0 (split-screen join) calls it through
// the user's vtable with device -1, which makes 82269790 build an in-memory
// default profile and never mount a save volume. Player 1's Career entry
// passes a real storage device. With the experiment on, give the second
// user's load the device Player 1's load used, so 82269790 creates a
// CProfileSaveGameVolume for Player 2's XUID.
// u32 Load(user* this, i32 device, u32 user_index)
extern "C" void __imp__sub_8229CE20(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8229CE20(PPCContext& ctx, uint8_t* base) {
  static std::atomic<uint32_t> host_device{0xFFFFFFFF};
  const uint32_t user = ctx.r3.u32, device = ctx.r4.u32, index = ctx.r5.u32;
  bool redirected = false;
  if (Enabled()) {
    if (device != 0xFFFFFFFF) {
      if ((index & 0xFF) != kSecondUser) host_device = device;
    } else if ((index & 0xFF) == kSecondUser && host_device != 0xFFFFFFFF) {
      ctx.r4.u64 = host_device.load();
      redirected = true;
    }
  }
  __imp__sub_8229CE20(ctx, base);
  if (Enabled())
    REXLOG_WARN("SECOND_PROFILE profile load user={:08X} device={:08X} index={} -> result {}{}", user, device,
        index, ctx.r3.u32, redirected ? fmt::format(" (device redirected to {:08X})", host_device.load()) : std::string());
}
