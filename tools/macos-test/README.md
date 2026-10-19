# macOS SDK checks

`python3 tools/Build-Mac.py --stage smoke` builds the actual FM3 app hooks with a stub image declaration and runs a game-independent Vulkan fill/fence/readback check against FM3's installed SDK. The stub only supports compilation; it cannot boot the game.

To exercise presentation and resize handling, run `python3 tools/Build-Mac.py --stage smoke --window-smoke`. The helper supplies the installed MoltenVK paths and runs the test app bundle. It checks known-color guest-output capture, requests three window size changes, and keeps drawing UI triangles for 60 seconds for visual inspection. The default window test without the hold option draws at least 120 frames. These tests need host Metal device access; a sandbox without GPU access reports an incompatible driver. This standalone test's hold duration does not limit game runs.

The harness comes from `DarthMDev/fm4-recomp` at `ed5f4090433120ced7fcaef559aaf2e5a8275010`, `tools/macos-test/main.cpp` (source SHA-256 `6a09c8aabbfc0ada434688eb2d75b7fd1f3e8af7a53e74e047f300ed51c634cb`). Changes here rename the title and test pattern and omit FM4-specific readback-slot readiness assertions. Those assertions depend on an SDK readback fix not yet adopted for FM3. The fenced buffer readback and presenter checks remain intact.

SDK patch 0007 adapts the file-clock assertion used by FM4's integration to avoid Catch2 stringifying libc++'s `__int128` timestamp representation. Patches 0005/0006 make the PPC assembly tests optional and fix the unit-test xxHash include dependency. Patch 0008 corrects escaped-string literals in the depfile test without changing the production escaping function.

Patch 0010 takes the macOS surface recreation and swapchain effect pipeline format fixes from FM4's integration. The resize smoke test passed with all three requested size changes and continued visible rendering.
