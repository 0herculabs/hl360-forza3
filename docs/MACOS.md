# macOS support

FM3 supports experimental native Apple Silicon builds with Metal. Windows keeps
its Direct3D 12 default; MoltenVK remains available as a diagnostic baseline.
Provide your own supported USA Ultimate Collection game discs (see the main
README). Game data, saves, generated code, SDK binaries, and test captures are
not distributed with this repository.

## Requirements and build

Use a native Apple Silicon terminal, Xcode command-line tools, Python 3,
CMake 3.25 or newer, and Ninja. Initialize recursive Git submodules first.
The tested configuration is Apple M3, ARM64 macOS 27.0.1, Apple Clang 21.
Other Macs and Intel macOS have not been gameplay validated.

```sh
python3 tools/Build-Mac.py --stage preflight
python3 tools/Build-Mac.py --stage sdk
python3 tools/Build-Mac.py --stage smoke
python3 tools/Build-Mac.py --stage tools
```

The tools stage builds the launcher, installation tool, and regression tests
without discs or game code generation. SDK builds validate the pinned source
revision and ordered patches before applying missing patches. Use a dedicated
SDK checkout; the scripts do not reset local changes.

For the game, place extracted Disc 1 at `game/disc1/`, then run:

```sh
python3 tools/Build-Mac.py --stage game
```

This builds the MoltenVK baseline and generates the translated game sources.
Then follow [the Metal build instructions](MACOS_METAL.md) to stage the native
renderer with matching libraries. Do not mix libraries between SDK builds.
CMake build directories are not relocatable; configure fresh directories after
moving a checkout.

## Launcher and saves

The Metal staging script creates `FM3 Launcher.app` beside the game executable.
Keep the bundle inside that package folder. It resolves the sibling game,
configuration, manifest, and tools when opened from Finder. Its settings include
Metal/Vulkan selection, display options, controller vibration, and an experimental
frame cap. Vibration is disabled in FM3's default configuration.

Settings (`fm3.user.toml`, `launcher.toml`) and the default persistent `user/`
folder live beside the package. This development layout requires a writable
folder; it does not yet use Application Support. The bundle is locally signed,
not notarized. Launcher UI and child-process handoff have been checked on macOS;
full game startup through the launcher still requires a manual acceptance check.

For isolated testing, use `tools/Run-Mac.py` instead. It copies supplied saves
and caches into a private run directory and records hashes, settings, and logs.
It does not update the source fixture. Retain or import changed saves from the
run explicitly before choosing a different source fixture. Never point two
running games at the same writable save directory.

## Troubleshooting

If imported Xenia saves show missing car bodies or unrelated images in the car
menus, repaint the affected car and save. The user confirmed this refreshed all
reported corrupt previews. Offline inspection also found the broken Fiesta
image already stored in the imported thumbnail. Deleting thumbnail files alone
does not make FM3 regenerate them and can leave black placeholders. Preserve
player data and back up saves before making changes.

The current glare workaround and synchronization tradeoffs are described in
[Metal support](MACOS_METAL.md). Performance measurements and test commands are
in [performance and validation](MACOS_PERFORMANCE.md).
