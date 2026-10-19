# macOS performance and validation

## Capture and interpret a run

```sh
python3 tools/Run-Mac.py --backend metal \
  --build-dir out/macos/game-build-metal --perf --warm-cache \
  --profile-fixture /path/to/copied-user-data --profile-xuid YOUR_OFFLINE_XUID
```

There is no time limit by default; quit normally when finished. Each run has a
receipt, logs, `frames.csv`, and process CPU/RSS samples under
`out/macos/test-runs/`. `--prepare-only` verifies an isolated launch without
starting the game. Fixtures are independently copied or APFS cloned, never
hard-linked to writable saves.

```sh
python3 tools/Summarize-Mac.py out/macos/test-runs/RUN \
  --start 120 --end 180 --label race
```

Choose a stable scene interval after loading. Guest FPS measures present-hook
calls, not successful display presentations. CPU stage wall times can nest and
overlap; do not sum them. Metal GPU timestamps cover command-processor buffers,
excluding separate presenter/texture submissions, and are not total utilization.
Report frame-time percentiles and stalls alongside average FPS. Compare the
same scene, cache state, configuration, game binary, and plugin hashes.

`--cvar log_async=true` opts into asynchronous logging to reduce blocking from
profiling output. The SDK default stays synchronous. The async queue preserves
messages and its explicit flush barrier waits for queued writes.

## Checks

```sh
python3 -m unittest discover -s tests -p 'test_*.py'
cmake --build out/macos/game-build --target fm3-launcher-tests \
  fm3-frame-stats-tests fm3-async-logging-tests fm3-host-path-tests
ctest --test-dir out/macos/game-build --output-on-failure
```

The async test skips with code 77 if the selected runtime lacks the optional
logging patch. To test a staged runtime explicitly, set `DYLD_LIBRARY_PATH` to
that package when invoking the test executable. The optional host-path test
requires owned Disc 1 data and the patched runtime; enable its CTest entry with
`FM3_DISC_PATH_TESTS=ON`, or run the executable with the disc directory argument.

Controlled GPU checks do not start FM3:

```sh
python3 tools/Check-Metal.py \
  --plugin out/macos/game-build-metal/librexgpu-xenos.dylib \
  --shader-cache /path/to/owned/cache/shaders/shareable/4D53084D.xsh
```

The harness validates drawn pixels, tiled compressed textures, presentation, and
GPU timing. Optional pipeline fixtures test disk-cache preparation. Native GPU
and guest-memory tools need normal host access outside restrictive sandboxes.

## Saves, caches, and disk space

`Clean-Mac.py` defaults to a dry run. Review it before `--apply`. Older run
payloads may be removed while retaining logs and receipts; profile compaction
removes only files hash-identical to the preserved fixture and keeps changed
saves plus a reconstruction manifest. A compacted run is not a complete launch
fixture. Keep a verified full save copy before deleting any run manually.

`--keep-run-payloads 1` opts into cleanup after a successful isolated run. Keep
original/imported saves and known working packages. Do not clean an active run.

For imported car-menu corruption, repaint and save in a private profile. The
optional `Inspect-FM3-Thumbnails.py` forensic helper requires Pillow and decodes
the observed XDC layout without modifying saves. Its limited format support is
not a general save editor. The legacy `--regenerate-thumbnails` option only backs
up copied images; it does not cause the game to rebuild missing references.
