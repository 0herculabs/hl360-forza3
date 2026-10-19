# Native Metal renderer

The Metal backend imports the Xenia Alloy renderer through an ordered SDK patch
stack (`patches/sdk/0015`–`0024`). Patches 0001–0014 contain the shared FM3 and
initial macOS/Vulkan prerequisites. Upstream source notices and provenance stay
with the imported code. SDK changes must be reviewed separately from the game's
launcher, platform, and build-script changes; an uncommitted SDK checkout is
not a distributable dependency revision.

## Build and stage

First complete the baseline SDK and game build in [MACOS.md](MACOS.md).
`Build-Metal.py` obtains converter headers, libraries, and SPIRV-Cross sources
from a local Xenia Alloy checkout. Supply that checkout explicitly when it is
not a sibling named `xenia-alloy`. Converter dependencies are local build inputs,
not files to upload in the game PR; retain their upstream licensing notices.

```sh
python3 tools/Build-Metal.py --alloy-source /path/to/xenia-alloy \
  --game-backend --stage-dir out/macos/game-build-metal
```

The helper builds the Metal SDK/plugin, stages the game and launcher with
matching dependencies, adjusts packaged library search paths, signs the plugin
and launcher bundle, and writes `metal-backend.json` with file hashes. It does
not launch FM3. The installed Vulkan SDK remains separate.

Optional `--profile-fixture`, `--profile-xuid`, and `--cache-fixture` arguments
seed isolated launch commands. Use your own paths and offline XUID. Keep original
saves separate from writable runs.

## Current behavior

- Native Metal menus, car scenes, and racing have been manually exercised on
  Apple M3. The user reported smoother racing with occasional stutters; stable
  60 FPS across all tracks and game modes is not established.
- Cached pipeline descriptions are prepared before guest startup. Cold shaders
  may still require compilation. Prewarming only covers previously seen work.
- F3 toggles the guest FPS overlay. It measures the game's present-hook rate,
  not successful display presentations or simulation speed.
- Vibration can be toggled in the launcher and defaults off for this port.
- The 30/60 FPS host cap is experimental. Speed-correct 30 FPS simulation has
  not been validated; game-default timing remains the default.
- The global fake occlusion count of zero suppresses excessive sun glare in the
  tested race. This is a workaround, not real occlusion-query support.
- The performance preset skips some GPU-to-guest waits for destinations written
  repeatedly over 90 frames. Correctness across all gameplay is not established.
  The staged base configuration retains strict waits; launch scripts may override
  them. `readback_resolve` is consumed by Vulkan/D3D12, not the imported Metal
  path. Metal has separate synchronization controls.

## Launch modes

`FM3 Launcher.app` launches directly using saved launcher preferences. It does
not implicitly apply the diagnostic `.command` presets or isolated-run caches.

Generated development commands include ordinary play, profiling, a synchronization
comparison, a glare-workaround test, and an experimental 30 FPS test. These are
explicit test modes, not production quality presets. Temporary menu experiments
are no longer generated: repainting fixed the imported cached-preview issues.
Existing local run directories and commands are preserved for comparison.

The startup cache is selected from normally completed runs with matching game
and plugin hashes when `--warm-cache` is requested. A supplied cache fixture is
the fallback. The receipt records the actual selected cache. No cache guarantees
that unseen races or cars will be stutter-free.

## Remaining release work

Validate full startup through the Finder launcher with a persistent profile;
check save persistence and Disc 2 installation on macOS; verify Windows build
and launcher regressions in a Windows environment. Review SDK patches and
converter distribution requirements before publishing binaries. Standard macOS
data directories, notarization, real occlusion queries, and speed-correct frame
caps remain outside the validated support claim.
