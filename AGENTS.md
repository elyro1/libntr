# AGENTS.md

libntr: drop-in NitroSDK replacement that runs DS games on PC. This checkout is the `elyro1/libntr`
fork used by the Pokémon Platinum single-screen/widescreen port; it normally lives at `pokeplatinum/subprojects/libntr`
and is built as a meson subproject of that game.

## Repos and branches

- Remote: `origin` = `elyro1/libntr`. Work branch: `lib_libntr`.
- The game pins a commit in `pokeplatinum/subprojects/libntr.wrap` (`revision = <sha>`). After committing here, bump
  that revision in the game repo and remind the user that this commit has to be pushed to `origin` before a fresh
  checkout can fetch it.
- Changes go in as commits on this fork, never as diff files under `pokeplatinum/subprojects/packagefiles/`
  (the old `Apply pokeplatinum ... patch` commits folded those in).
- Commit or push only when asked. Commit messages: imperative one-line subject, like the history
  ("Add main-screen hold and crossfade").

## Ground rules

- **Stay a drop-in NitroSDK.** Public NitroSDK/TwlSDK APIs (`include/nitro`, `include/twl`) keep their signatures and
  DS semantics; games compiled against the real SDK must still work. Port extras are new, clearly named functions
  (`SIM_*`, or simulator-only bridges like `NNS_SndPlayerSetFastForward`), not changed behaviour of existing ones.
- **New behaviour sits behind a setting** with the original DS behaviour as default or reachable, the same rule as the
  game. Game-specific logic belongs in pokeplatinum; libntr provides the generic mechanism (e.g. wide 2D mode, master
  brightness scanline skip) that the game opts into.
- **Platform guards:** `SDK_BUILD_ARM` (real DS, Metrowerks), `SDK_BUILD_LINUX`, `SDK_BUILD_WIN`, `SDK_BUILD_NX`.
  Don't break the `arm` or `nx` builds even though we don't ship them; keep PC-only code in `libraries/sim/` or behind
  the guards.
- No Nintendo assets or ROM data in the repo.

## Layout

- `libraries/<module>/`: SDK modules (gx, fx, os, snd, card, fs, wm, ...). On PC many forward into the simulator.
- `libraries/sim/src/`: the PC simulator. Most port work happens here:
  - `sim_main.cpp`: SDL window, main loop, frame pacing, command line (`--print-config`), screen layouts.
  - `sim_g2.cpp`: 2D engine software rasterizer (BG/OBJ, windows, blending, wide 2D).
  - `g3_handler.cpp`, `g3_draw.cpp`, `sim_g3_CommandProcessor.cpp`: DS 3D commands translated to OpenGL 4.2.
  - `shaders/*.glsl`: G2/G3/bloom/field-coverage shaders.
  - `sim_gui*.cpp`: the Tab-menu ImGui settings windows. `sim_audio.cpp`: mixer, SoundTouch fast-forward.
  - `sim_autopilot.cpp`, `sim_frame_profile.cpp`: `PLATINUM_AUTOPILOT`, `PLATINUM_FRAME_TRACE` debug hooks.
  - `config/sim_config.c` (+ `include/simulator/config/`): `sim_config.ini` parsing and writing.
- `libraries/sim7/`: simulated ARM7 side. `include/simulator/`: simulator headers shared with games.
- `standalone/`: test EXE (`-Dstandalone=true`) and the CI build scripts; `.github/workflows/` runs them in Docker.
- `imgui`, `ImGuiFileDialog`, `glad`, `blip-buf`, `fireflower` under `libraries/sim/src/` are vendored; leave them
  alone unless the task is about them.

## Config keys

`sim_config.ini` keys are validated against what the writer emits: `SIM_ConfigCheck_Key` warns about any key the save
function doesn't write. A new key therefore needs all of:
1. a field in `SIM_config_type` (`include/simulator/config/sim_config.h`) with a default,
2. a `MATCH("<Section>", "<Key>")` branch in `IniHandler` (bad values via `SIM_ConfigCheck_BadValue`),
3. a `SIM_ConfigText_Printf` line in the writer, in the right section,
4. usually a control in `sim_gui_config.cpp`.
Check with `./main --print-config` from a game run dir.

## Build and test

There's no separate dev loop: build the game, which builds this checkout in place.

```bash
cd ../..  && ./drun.sh make linux     # or `make linux` with deps installed; output build_linux/pokeplatinum/main
```

- meson uses this directory as-is (it doesn't re-fetch while it exists), so uncommitted edits here are what gets built.
- `libraries/sim` builds with `-Wall`; keep it warning-free (the game's `make mingw` builds C++ with `-Werror`). The
  vendored code builds as `sim_vendor` and the SDK modules with `nitrosdk_quiet_args` (`-w`, which no later `-W` flag
  can undo). New SDK targets take `c_args: nitrosdk_quiet_args`; new vendored files go in `libsim_vendor_srcs`.
- Unit tests compile real libntr sources against the game build's `compile_commands.json`:
  - `python3 libraries/sim/tests/test_field_sprites.py ../../build_linux` (OBJ rasterization, native and wide)
  - in the game repo: `tools/scripts/test_libntr_{upstream_fixes,native_sdk,lifecycle}.py --build build_linux`
  Run the ones related to the change.
- To see a change in the game, follow the game's AGENTS.md "Running the game for checks": scratch run dir, never
  `build_linux/pokeplatinum` itself.
- Standalone CI builds: `./drun.sh ./standalone/scripts/ci_build_linux.sh` (and `_twl`, `win64` variants).

## Pitfalls

- **OAM lags VRAM by one frame** on the port (present, then VBlank transfers OAM). Changing when `GX_Load*`/OAM
  transfers happen here affects every screen in the game; see the game's AGENTS.md before touching that ordering.
- The dev GPU is NVIDIA, which accepts GL that Mesa rejects (e.g. two sampler types on one texture unit). Keep GL and
  GLSL strictly valid; "works here" is no proof.
- Widescreen/`top_only` code paths only run with a wide window (e.g. `WindowWidth=2160`, `WindowHeight=1000`) and
  `ScreenLayout=top_only`; the default 4:3 window won't exercise them.
- Keys in the wrong ini section are ignored with only a `[config]` warning on stderr.

## Style

- Follow the surrounding file: NitroSDK C style in `libraries/<module>`, the existing C++20 style in `libraries/sim`.
  No `.clang-format` here; don't reformat code you aren't changing.
- Keep comments short; explain why, not what.
