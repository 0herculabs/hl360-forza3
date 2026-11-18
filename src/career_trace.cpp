// Research instrumentation for Ultimate Collection USA v0.0.0.8.
// Experimental Career split-screen hooks and lifecycle diagnostics.
#include <bit>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <rex/cvar.h>
#include <rex/audio/downmix.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include "player2_profile.h"

REXCVAR_DEFINE_BOOL(career_guest_retain, false, "FM3/Research",
    "EXPERIMENTAL: retain player two when leaving split-screen; isolated saves only");
REXCVAR_DEFINE_DOUBLE(opponent_engine_gain, 1.0, "FM3/Audio",
    "Opponent engine mix multiplier, 0 to 1; 1 restores original mix");
REXCVAR_DEFINE_BOOL(audio_single_listener, false, "FM3/Audio",
    "Experimental shared first listener for split-screen audio");
REXCVAR_DEFINE_BOOL(audio_front_channels_only, false, "FM3/Audio",
    "Diagnostic: omit center, surround and LFE from stereo output");
REXCVAR_DEFINE_BOOL(audio_diagnostics, false, "FM3/Debug",
    "Analyze submitted samples for repetition and audio delivery timing")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Measure delivery at the guest's actual 256-sample submissions. Keep the
// hot path allocation-free and emit only one summary per client per 5 seconds.
extern "C" void career_audio_submit_trace(unsigned int client, const uint8_t* samples, unsigned int source) {
  // Opt in before launch. Disabled builds skip sample scans, hashing, clock
  // reads, per-client history and logging; the caller still submits unchanged
  // samples through the original audio path.
  static const bool enabled=REXCVAR_GET(audio_diagnostics);
  if(!enabled) return;
  using Clock = std::chrono::steady_clock;
  struct Delivery {
    bool used = false;
    unsigned int client = 0, count = 0, gaps = 0;
    Clock::time_point start{}, previous{};
    double maximum_ms = 0;
    uint64_t last_hash = 0;
    bool have_hash = false;
    unsigned int repeated = 0, silent = 0, over_range = 0, invalid = 0;
    float peak = 0;
    uint64_t history[4][6]{};
    unsigned int history_count = 0, four_back[6]{};
  };
  static thread_local Delivery states[8];
  Delivery* state = nullptr;
  for (auto& entry : states) {
    if (entry.used && entry.client == client) { state = &entry; break; }
    if (!entry.used && !state) state = &entry;
  }
  if (!state) return;
  const auto now = Clock::now();
  if (!state->used) {
    state->used = true; state->client = client;
    state->start = state->previous = now;
    REXLOG_WARN("AUDIO_SOURCE client={} callback={:08X} synchronization=c94f5eb-comparison compiler={}", client, source, __clang_version__);
  } else {
    const double gap = std::chrono::duration<double, std::milli>(now-state->previous).count();
    if (gap > state->maximum_ms) state->maximum_ms = gap;
    if (gap > 10.667) ++state->gaps;
  }
  state->previous = now;
  uint64_t hash = 14695981039346656037ull;
  uint64_t channel_hash[6];
  bool channel_silent[6];
  for (unsigned int c=0;c<6;++c) { channel_hash[c]=14695981039346656037ull; channel_silent[c]=true; }
  bool silent = true;
  for (unsigned int i = 0; i < 256*6; ++i) {
    uint32_t bits;
    std::memcpy(&bits, samples + i*4, 4);
    bits = std::byteswap(bits);
    hash = (hash ^ bits) * 1099511628211ull;
    channel_hash[i/256] = (channel_hash[i/256] ^ bits) * 1099511628211ull;
    const float sample = std::bit_cast<float>(bits);
    if (!std::isfinite(sample)) { ++state->invalid; silent = false; continue; }
    const float magnitude = std::fabs(sample);
    if (magnitude > state->peak) state->peak = magnitude;
    if (magnitude > 1) ++state->over_range;
    if (magnitude > 0.000001f) silent = false;
    if (magnitude > 0.000001f) channel_silent[i/256] = false;
  }
  const auto history_slot = state->history_count % 4;
  for (unsigned int c=0;c<6;++c) {
    if (state->history_count>=4 && !channel_silent[c] && channel_hash[c]==state->history[history_slot][c]) ++state->four_back[c];
    state->history[history_slot][c]=channel_hash[c];
  }
  ++state->history_count;
  if (silent) ++state->silent;
  if (!silent && state->have_hash && hash == state->last_hash) ++state->repeated;
  state->have_hash = true;
  state->last_hash = hash;
  ++state->count;
  const double elapsed = std::chrono::duration<double>(now-state->start).count();
  if (elapsed >= 5.0) {
    REXLOG_WARN("AUDIO_DELIVERY client={} blocks_per_sec={:.1f} expected=187.5 max_gap_ms={:.2f} gaps_over_10ms={}",
        client, state->count/elapsed, state->maximum_ms, state->gaps);
    REXLOG_WARN("AUDIO_CONTENT client={} blocks={} repeated_non_silent={} silent={} peak={:.4f} samples_over_one={} nonfinite={}",
        client, state->count, state->repeated, state->silent, state->peak,
        state->over_range, state->invalid);
    state->start = now; state->count = state->gaps = 0; state->maximum_ms = 0;
    state->repeated = state->silent = state->over_range = state->invalid = 0;
    state->peak = 0;
    REXLOG_WARN("AUDIO_REPEAT_1024 client={} channel_counts FL={} FR={} C={} LFE={} SL={} SR={}",
        client,state->four_back[0],state->four_back[1],state->four_back[2],state->four_back[3],state->four_back[4],state->four_back[5]);
    for (auto& count:state->four_back) count=0;
  }
}

namespace {
std::atomic<bool> shared_listener_active{false};
}
extern "C" void __imp__sub_827E7818(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_827E7818(PPCContext& __restrict ctx, uint8_t* base) {
  const auto requested = ctx.r4.u32;
  const bool shared = REXCVAR_GET(audio_single_listener) && requested == 2;
  if (shared) ctx.r4.u64 = 1;
  __imp__sub_827E7818(ctx, base);
  shared_listener_active.store(shared && ctx.r3.u32 == 0);
  REXLOG_WARN("AUDIO_LISTENER requested={} effective={} result={}",
      requested, shared ? 1u : requested, ctx.r3.u32);
}
extern "C" void __imp__sub_827E7870(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_827E7870(PPCContext& __restrict ctx, uint8_t* base) {
  if (shared_listener_active.load() && ctx.r4.u32 == 1) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
      REXLOG_WARN("AUDIO_LISTENER suppressing second listener transform; using listener zero");
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_827E7870(ctx, base);
}

namespace {
constexpr uint32_t kPlayerManager = 0x82C231F4;
uint32_t Read32(uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return std::byteswap(value);
}
void Write32(uint8_t* base, uint32_t address, uint32_t value) {
  value = std::byteswap(value);
  std::memcpy(base + address, &value, sizeof(value));
}
void Players(uint8_t* base, const char* event, uint64_t caller,
             uint32_t arg3, uint32_t arg4, uint32_t arg5) {
  REXLOG_WARN("CAREER_RESEARCH {} lr={:08X} args={:08X},{:08X},{:08X} manager_vt={:08X} local_count={} all_count={}",
      event, caller, arg3, arg4, arg5, Read32(base, kPlayerManager),
      Read32(base, kPlayerManager + 0x2C), Read32(base, kPlayerManager + 0x48));
  const auto count = Read32(base, kPlayerManager+0x2C);
  if (count >= 1 && count <= 2 && Read32(base, kPlayerManager) == 0x82001CDC) {
    const auto sentinel = Read32(base, kPlayerManager+0x28);
    auto node = Read32(base, sentinel);
    for (uint32_t i = 0; i < count && node != sentinel; ++i) {
      const auto player = Read32(base, node+8);
      REXLOG_WARN("CAREER_RESEARCH local[{}]={:08X} vt={:08X} field10={} profile={:08X} userdata={:08X} car={:08X}",
          i, player, Read32(base,player), Read32(base,player+0x10),
          Read32(base,player+0xAC), Read32(base,player+0xB4), Read32(base,player+0xB8));
      node = Read32(base,node);
    }
  }
}
}

// Career's opponent-position table contains AI entries, but no entry for the
// retained guest. Only omit that guest's missing entry, never a missing AI.
extern "C" bool career_skip_guest_result(uint8_t* base, uint32_t player, uint32_t component, uint32_t record) {
  if (!REXCVAR_GET(career_guest_retain) ||
      Read32(base, kPlayerManager) != 0x82001CDC ||
      Read32(base, kPlayerManager+0x2C) != 2) return false;
  const auto sentinel = Read32(base, kPlayerManager+0x28);
  const auto first = Read32(base, sentinel);
  const auto second = Read32(base, first);
  if (second == sentinel || Read32(base, second+8) != player) return false;
  const auto begin = Read32(base,component+0x28);
  const auto end = Read32(base,component+0x2C);
  bool freeSlot = begin && end >= begin && end-begin <= 64;
  for (auto entry=begin; freeSlot && entry<end; entry+=8)
    if (Read32(base,entry+4)==7) freeSlot=false;
  const auto points = Read32(base,player+0xC8);
  if (freeSlot && record && points <= 65535) {
    const uint16_t value = std::byteswap(static_cast<uint16_t>(points));
    std::memcpy(base+record+16,&value,2);
    fm3::SavePlayer2Points(Read32(base,component+8),points);
    REXLOG_WARN("CAREER_POINTS saved player2={} event={} slot=7",points,Read32(base,component+8));
  } else {
    REXLOG_WARN("CAREER_POINTS reserved slot unavailable; guest score not written");
  }
  return true;
}

static uint32_t SecondPlayer(uint8_t* base) {
  if (!REXCVAR_GET(career_guest_retain) || Read32(base,kPlayerManager)!=0x82001CDC ||
      Read32(base,kPlayerManager+0x2C)!=2) return 0;
  auto sentinel=Read32(base,kPlayerManager+0x28);
  auto node=Read32(base,Read32(base,sentinel));
  return node==sentinel ? 0 : Read32(base,node+8);
}

extern "C" void __imp__sub_827EC840(PPCContext& __restrict ctx,uint8_t* base);
extern "C" void sub_827EC840(PPCContext& __restrict ctx,uint8_t* base) {
  const auto config=ctx.r3.u32;
  __imp__sub_827EC840(ctx,base);
  if (REXCVAR_GET(audio_front_channels_only)) {
    auto fold = rex::audio::GetStereoFold();
    fold.center = fold.surround = fold.lfe = 0.0f;
    rex::audio::SetStereoFold(fold);
    REXLOG_WARN("AUDIO_MIX diagnostic front-left/right only; stereo scale={}", fold.scale);
  }
  const auto gain=REXCVAR_GET(opponent_engine_gain);
  // AudioEngineConfig.xml gameOpponentEngine is parsed at 827ED4D8
  // and clamped into +0x404. Keep player engines and music unchanged.
  const float original=std::bit_cast<float>(Read32(base,config+0x404));
  if (gain>=0.0 && gain<=1.0 && original>=0.0f && original<=1.0f) {
    const float adjusted=original*static_cast<float>(gain);
    Write32(base,config+0x404,std::bit_cast<uint32_t>(adjusted));
    REXLOG_WARN("AUDIO_MIX opponent engine gain {} -> {} (multiplier {})",original,adjusted,gain);
  }
}
extern "C" void __imp__sub_82231AD0(PPCContext& __restrict ctx,uint8_t* base);
extern "C" void sub_82231AD0(PPCContext& __restrict ctx,uint8_t* base) {
  const auto caller=ctx.lr;
  __imp__sub_82231AD0(ctx,base);
  const auto guest=SecondPlayer(base);
  if (caller==0x822E36D4 && guest && ctx.r3.u32) {
    uint16_t value;
    std::memcpy(&value,base+ctx.r3.u32+16,2);
    const auto points=std::byteswap(value);
    Write32(base,guest+0xC8,points);
    REXLOG_WARN("CAREER_POINTS restored player2={} from saved series",points);
  }
}

extern "C" void __imp__sub_8245A408(PPCContext& __restrict ctx,uint8_t* base);
extern "C" void __imp__sub_822E2D00(PPCContext& __restrict ctx,uint8_t* base);
extern "C" void sub_822E2D00(PPCContext& __restrict ctx,uint8_t* base) {
  const auto caller=ctx.lr, output=ctx.r5.u64;
  __imp__sub_822E2D00(ctx,base);
  const auto guest=SecondPlayer(base);
  if (caller!=0x823DCBC0 || !guest) return;
  const auto car=Read32(base,guest+0xB8);
  const auto begin=Read32(base,static_cast<uint32_t>(output)+0xC);
  const auto end=Read32(base,static_cast<uint32_t>(output)+0x10);
  if (!car || end<begin || end-begin>7*32 || (end-begin)%32) return;
  for (auto row=begin;row<end;row+=32) {
    if (Read32(base,row+4)!=7) continue;
    // The standings descriptor uses the same car ID, class and PI fields
    // that Career copies into an AI car config during setup. Here use P2's.
    Write32(base,row+8,Read32(base,car+8));
    Write32(base,row+0x10,Read32(base,car));
    Write32(base,row+0x14,Read32(base,car+4));
    REXLOG_WARN("CAREER_POINTS standings player2 car={} class={}",Read32(base,car+8),Read32(base,car));
  }
}
extern "C" void __imp__sub_8229F7F0(PPCContext& __restrict ctx,uint8_t* base);
extern "C" void __imp__sub_8227E4D0(PPCContext& __restrict ctx,uint8_t* base);
extern "C" void sub_8245A408(PPCContext& __restrict ctx,uint8_t* base) {
  unsigned player = 0;
  if (SecondPlayer(base)) {
    if (ctx.lr==0x823DCD48) player=1;
    if (ctx.lr==0x823DCF4C) {
      const auto begin=Read32(base,ctx.r1.u32+0x9C);
      const auto end=Read32(base,ctx.r1.u32+0xA0);
      if (end>=begin && ctx.r28.u32<end-begin &&
          Read32(base,begin+ctx.r28.u32+4)==7) player=2;
    }
  }
  const auto row=ctx.r3.u32;
  __imp__sub_8245A408(ctx,base);
  if (!player) return;
  // Use the game's owned Unicode string constructor and reference assignment.
  // Reserve scratch below the caller's stack and below it a separate call frame.
  PPCContext call=ctx;
  const auto scratch=ctx.r1.u32-0x80;
  call.r1.u64=ctx.r1.u32-0x100;
  Write32(base,scratch,0); Write32(base,scratch+4,0);
  const std::string label=player==2?fm3::Player2Name():"Player 1";
  for(unsigned i=0;i<=label.size();++i) {
    base[scratch+16+i*2]=0;
    base[scratch+17+i*2]=i<label.size()?label[i]:0;
  }
  call.r3.u64=scratch; call.r4.u64=scratch+16; call.r5.s64=-1;
  __imp__sub_8229F7F0(call,base);
  call.r3.u64=row+8; call.r4.u64=scratch;
  __imp__sub_8227E4D0(call,base);
  call.r3.u64=scratch; call.r4.u64=scratch+4;
  __imp__sub_8227E4D0(call,base);
  base[row+0x29]=1;
  REXLOG_WARN("CAREER_POINTS standings player={} points={}",player,Read32(base,row+0x2C));
}

#define TRACE_PLAYER_FUNCTION(address, label) \
extern "C" void __imp__sub_##address(PPCContext& __restrict ctx, uint8_t* base); \
extern "C" void sub_##address(PPCContext& __restrict ctx, uint8_t* base) { \
  const auto caller = ctx.lr; \
  const auto a = ctx.r3.u32, b = ctx.r4.u32, c = ctx.r5.u32; \
  Players(base, label " before", caller, a, b, c); \
  __imp__sub_##address(ctx, base); \
  Players(base, label " after", caller, a, b, c); \
}

TRACE_PLAYER_FUNCTION(822A1C80, "add_local")
TRACE_PLAYER_FUNCTION(8226A5E0, "join_controller")
TRACE_PLAYER_FUNCTION(822A1150, "create_nonlocal")
TRACE_PLAYER_FUNCTION(822E92B8, "career_initialize")

// CUserInterface4's scene stack ([ui+76]) holds the inputs whose events reach
// screens: +28 primary, +32 second. Stock split-screen sets the second one so
// CHudSplitscreen::OnInput (82369C98) hears Player 2 (Start = pause). Career
// co-op never set it, so Player 2 could not pause. Set it while the co-op race
// HUD exists and clear it on the next non-pause screen request, so Player 2's
// controller does not also drive Career menus and result screens.
extern "C" uint32_t fm3_coop_second_controller = 0xFF;
static uint32_t coop_second_input = 0;
// Stack +36 selects which inputs 827178B8 polls: 1 primary, 2 second, 3 both.
static uint32_t coop_input_mode_saved = 1;
static uint32_t UiSceneStack(uint8_t* base) {
  const auto ui = Read32(base, 0x82C76C1C);
  return ui >= 0x10000 ? Read32(base, ui+76) : 0;
}
static void SetCoopSecondInput(uint8_t* base) {
  const auto stack = UiSceneStack(base), inputs = Read32(base, 0x82C21118);
  if (stack < 0x10000 || inputs < 0x10000 || fm3_coop_second_controller >= 4) return;
  const auto input = inputs + 0x10 + fm3_coop_second_controller*0x1610;
  if (Read32(base, stack+28) == input || Read32(base, stack+32) != 0) return;
  Write32(base, stack+32, input);
  coop_second_input = input;
  coop_input_mode_saved = Read32(base, stack+36);
  Write32(base, stack+36, 3);
  REXLOG_WARN("CAREER_RESEARCH Player 2 controller {} added to race HUD input (input mode {} -> 3)", fm3_coop_second_controller, coop_input_mode_saved);
}
REXCVAR_DECLARE(bool, player2_real_profile);
static void ClearCoopSecondInput(uint8_t* base, uint32_t reason) {
  if (!coop_second_input) return;
  // With a real second profile the Career menus resolve the profile from the
  // last active controller, so Player 2's controller stays registered and can
  // open the garage and shop as Player 2. Removed only on return to solo.
  if (REXCVAR_GET(player2_real_profile) && reason != 0xFFFF) return;
  const auto stack = UiSceneStack(base);
  if (stack >= 0x10000 && Read32(base, stack+32) == coop_second_input) {
    Write32(base, stack+32, 0);
    if (Read32(base, stack+36) == 3) Write32(base, stack+36, coop_input_mode_saved == 3 ? 1 : coop_input_mode_saved);
  }
  coop_second_input = 0;
  REXLOG_WARN("CAREER_RESEARCH Player 2 HUD input removed (screen {:02X} requested)", reason);
}

extern "C" void fm3_set_coop_second_input(uint8_t* base) { SetCoopSecondInput(base); }
extern "C" void fm3_clear_coop_second_input(uint8_t* base) { ClearCoopSecondInput(base, 0xFFFF); }

// UI registry: normal HUD id 0x2C uses name +0x168 / factory +0x164;
// split HUD id 0x30 uses name +0x188 / factory +0x184.
// Retain the requested screen ID so Career's close/find operations still work.
static bool UseSplitHud(uint8_t* base, const PPCContext& ctx) {
  return REXCVAR_GET(career_guest_retain) && ctx.r4.u16 == 0x2C &&
      Read32(base, kPlayerManager) == 0x82001CDC &&
      Read32(base, kPlayerManager+0x2C) == 2;
}
// The split-screen HUD opens the split-screen pause menu, screen 0x60
// (CPauseSplitscreen). Its Restart path assumes the menu background scene is
// still loaded, which is true in stock split-screen but not in a Career race
// (crash_20261112_110543 and later). The Career pause menu, screen 0x5F
// (CPauseMenu), restarts without it. In Career co-op (two local players, game
// mode at state+768 not 2) create CPauseMenu when 0x60 is requested, keeping
// the requested screen ID as the HUD swap does. Registry entry for id N:
// factory at +4+N*8, layout name at +8+N*8.
REXCVAR_DEFINE_BOOL(coop_career_pause_menu, true, "FM3/Research",
    "Career co-op uses the Career pause menu instead of the split-screen one");
static bool UseCareerPause(uint8_t* base, const PPCContext& ctx) {
  if (!REXCVAR_GET(coop_career_pause_menu) || !REXCVAR_GET(career_guest_retain) ||
      ctx.r4.u16 != 0x60 || Read32(base, kPlayerManager) != 0x82001CDC ||
      Read32(base, kPlayerManager+0x2C) != 2) return false;
  const auto ui = Read32(base, 0x82C37720);
  return ui >= 0x10000 && Read32(base, ui+16+768) != 2;
}
extern "C" void __imp__sub_823264E8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_823264E8(PPCContext& __restrict ctx, uint8_t* base) {
  if (UseCareerPause(base, ctx)) {
    const auto registry = ctx.r3.u32;
    constexpr uint32_t kSplit = 4+0x60*8, kCareer = 4+0x5F*8;
    const auto factory = Read32(base,registry+kSplit), name = Read32(base,registry+kSplit+4);
    Write32(base,registry+kSplit,Read32(base,registry+kCareer));
    Write32(base,registry+kSplit+4,Read32(base,registry+kCareer+4));
    REXLOG_WARN("CAREER_RESEARCH selecting Career pause menu for two local players");
    __imp__sub_823264E8(ctx,base);
    Write32(base,registry+kSplit,factory);
    Write32(base,registry+kSplit+4,name);
    return;
  }
  if (!UseSplitHud(base, ctx)) { __imp__sub_823264E8(ctx,base); return; }
  const auto registry = ctx.r3.u32;
  const auto factory = Read32(base,registry+0x164);
  const auto name = Read32(base,registry+0x168);
  struct RestoreHud {
    uint8_t* base; uint32_t registry, factory, name;
    ~RestoreHud() {
      Write32(base,registry+0x164,factory);
      Write32(base,registry+0x168,name);
    }
  } restore{base,registry,factory,name};
  Write32(base,registry+0x164,Read32(base,registry+0x184));
  Write32(base,registry+0x168,Read32(base,registry+0x188));
  REXLOG_WARN("CAREER_RESEARCH selecting split-screen HUD for two local players");
  __imp__sub_823264E8(ctx,base);
  SetCoopSecondInput(base);
}
// CPauseMenu's message handler resolves the Career profile through
// 82362E10 -> 8226A510, which uses InputSystem::lastActiveController. When
// Player 2 operates the pause menu that is the guest profile, whose Career
// calendar lookup (822315E8) returns null and is dereferenced
// (crash_20261112_130720, Restart from Player 2's pause). Same class of fault
// as the iteration27 calendar crash; same remedy: while this handler runs in
// Career co-op, the last-active controller is Player 1's (manager+132).
extern "C" void __imp__sub_823D56E8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_823D56E8(PPCContext& __restrict ctx, uint8_t* base) {
  if (REXCVAR_GET(career_guest_retain) && Read32(base, kPlayerManager) == 0x82001CDC &&
      Read32(base, kPlayerManager+0x2C) == 2) {
    const auto input = Read32(base, 0x82C21118);
    const uint8_t host = base[kPlayerManager+132];
    if (input >= 0x10000 && host < 4 && base[input+0x6E78] != host) {
      static std::atomic<unsigned> logged{0};
      if (logged.fetch_add(1) < 6)
        REXLOG_WARN("CAREER_RESEARCH pause menu input from controller {}; Career profile resolved for controller {}", base[input+0x6E78], host);
      base[input+0x6E78] = host;
    }
  }
  __imp__sub_823D56E8(ctx,base);
}

// Diagnostic: who asks the UI for a pause menu (CUserInterface4 slot 64).
// The chain walk stops at the top-of-stack frame, whose back chain is page
// aligned and unmapped (an unbounded walk here caused crash_20261112_125115).
extern "C" void __imp__sub_827026D0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_827026D0(PPCContext& __restrict ctx, uint8_t* base) {
  if (ctx.r5.u16 != 0x60 && ctx.r5.u16 != 0x5F) ClearCoopSecondInput(base, ctx.r5.u16);
  if (ctx.r5.u16 == 0x60 || ctx.r5.u16 == 0x5F) {
    std::string chain = fmt::format("{:08X}", static_cast<uint32_t>(ctx.lr));
    uint32_t sp = ctx.r1.u32;
    for (int i = 0; i < 6; ++i) {
      const auto prev = Read32(base, sp);
      if (prev <= sp || prev - sp > 0x2000 || (prev & 0xFFF) == 0) break;
      chain += fmt::format(" {:08X}", Read32(base, prev-8));
      sp = prev;
    }
    const auto input = Read32(base, 0x82C21118);
    REXLOG_WARN("CAREER_RESEARCH pause menu requested id={:02X} layer={} r6={:08X} r7={:08X} last_active_controller={} manager_controller={} chain={}",
        ctx.r5.u16, ctx.r4.u32, ctx.r6.u32, ctx.r7.u32, input >= 0x10000 ? base[input+0x6E78] : 255, base[kPlayerManager+132], chain);
  }
  __imp__sub_827026D0(ctx,base);
}
extern "C" void __imp__sub_82328FC0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82328FC0(PPCContext& __restrict ctx, uint8_t* base) {
  if (UseSplitHud(base,ctx)) ctx.r4.u64 = 0x30;
  else if (UseCareerPause(base,ctx)) ctx.r4.u64 = 0x5F;
  __imp__sub_82328FC0(ctx,base);
}

extern "C" void __imp__sub_82210D10(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_822E3548(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_822E3548(PPCContext& __restrict ctx, uint8_t* base) {
  if (!REXCVAR_GET(career_guest_retain) || Read32(base, kPlayerManager+0x2C) != 2) {
    __imp__sub_822E3548(ctx, base);
    return;
  }
  // Career computes its AI budget twice as DebugOptions.maxCars - 1,
  // at 822E35F4 and 822E3994. Account for the extra human in both reads.
  // Scope this adjustment to Career setup, restoring the shared option before
  // the caller begins loading the race. Do not change the event database.
  PPCContext call = ctx;
  if (const auto guest=SecondPlayer(base)) Write32(base,guest+0xC8,0);
  __imp__sub_82210D10(call, base);
  const auto field = call.r3.u32 + 0x418;
  const auto original = Read32(base, field);
  if (original < 3 || original > 8) {
    REXLOG_WARN("CAREER_RESEARCH unexpected maxCars={}; leaving AI budget unchanged", original);
    __imp__sub_822E3548(ctx, base);
    return;
  }
  struct Restore {
    uint8_t* base;
    uint32_t field, value;
    ~Restore() { Write32(base, field, value); }
  } restore{base, field, original};
  Write32(base, field, original-1);
  REXLOG_WARN("CAREER_RESEARCH EXPERIMENT reducing Career AI budget for two local players");
  __imp__sub_822E3548(ctx, base);
  if(const auto guest=SecondPlayer(base)) fm3::PreparePlayer2Career(ctx,base,guest);
}

extern "C" void __imp__sub_822A0EB8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_822A0EB8(PPCContext& __restrict ctx, uint8_t* base) {
  const auto caller = ctx.lr;
  const auto manager = ctx.r3.u32, player = ctx.r4.u32, notify = ctx.r5.u32;
  Players(base, "remove_player before", caller, manager, player, notify);
  __imp__sub_822A0EB8(ctx, base);
  Players(base, "remove_player after", caller, manager, player, notify);
}

extern "C" void __imp__sub_822C23D0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_822C23D0(PPCContext& __restrict ctx, uint8_t* base) {
  // The enclosing split-screen UI cleanup also unloads the second profile.
  // Intercept that operation as a unit rather than keeping a player whose
  // profile has already been unloaded. Disconnect/destruction paths in the
  // player manager remain unchanged.
  if (REXCVAR_GET(career_guest_retain) &&
      (ctx.lr == 0x82363E94 || ctx.lr == 0x823E4EA8) &&
      Read32(base,kPlayerManager) == 0x82001CDC &&
      Read32(base,kPlayerManager+0x2C) == 2) {
    Players(base,"EXPERIMENT preserve second player and profile",ctx.lr,
            ctx.r3.u32,ctx.r4.u32,ctx.r5.u32);
    return;
  }
  __imp__sub_822C23D0(ctx,base);
}

extern "C" void __imp__sub_822E3DD0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_822E3DD0(PPCContext& __restrict ctx, uint8_t* base) {
  Players(base, "race_factory", ctx.lr, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  const uint32_t options = ctx.r5.u32;
  // This argument is dereferenced by the original function, but still guard
  // the diagnostics against an unexpected call rather than creating a fault.
  if (options >= 0x10000 && options <= 0xFFFFFDEF) {
    for (uint32_t offset = 0; offset < 0x210; offset += 16) {
      REXLOG_WARN("CAREER_RESEARCH race_options +{:03X} {:08X} {:08X} {:08X} {:08X}",
          offset, Read32(base, options+offset), Read32(base, options+offset+4),
          Read32(base, options+offset+8), Read32(base, options+offset+12));
    }
  }
  __imp__sub_822E3DD0(ctx, base);
}
