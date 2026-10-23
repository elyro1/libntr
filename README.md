# libntr
![logo](images/libntr.png)
Drop-in replacements for NitroSDK, NitroSystem, NitroWiFi, NitroDWC, and VCT
that target Nintendo DS/DSi, PC, and other platforms.

libntr makes it easier to port Nintendo DS/DSi games to PC and other platforms by handling the logic for common libraries used by nearly all Nintendo DS games.


## Features
 * Fully compatible with NitroSDK
 * Translate DS 3D graphics APIs to OpenGL 4.2 with support for 3D upscaling
 * Simulate DS 2D graphics
 * Includes support for adding GUIs via Dear ImGui
 * Simulate local wireless multiplayer over LAN or the internet

## Available Build Targets
Select the build target with the 'build_target' meson option.
 * arm (Nintendo DS hardware)
 * linux (x86_64 Linux PC)
 * win64 (x86_64 Windows PC)
 * nx (Switch via libnx)

## DSi Support (BETA)
libntr supports building TWL applications. Set the meson options:
 * sdk_version_major=5
 * hardware_type=(twl_hyb) or (twl_ltd)
 * * twl_hyb: DSi enhanced application, supports running on original NDS and DSi
 * * twl_ltd: DSiWare, DSi only

## Simulator fast-forward

Hold Space in the game window to use the speed configured in Config →
Fast-forward speed. `[General] FastForwardMultiplier` in `sim_config.ini`
accepts 1–10 (including fractional values); the default is 5. Missing or invalid
values use the default, and numeric values outside the range are clamped.
Keyboard capture, focus loss, and game-logic pause return playback to 1×.

Games opt sound players into accelerated timing with the simulator-only
`NNS_SndPlayerSetFastForward(logicalPlayerNo, enabled)` bridge in libntrsystem.
It transfers the policy to each allocated runtime player through queued libntr
sound commands. Direct wave playback can use
`SIM_Audio_SetChannelFastForward(channelNo, enabled)` before its start command.
Unclassified players and channels retain normal speed. Ownership persists on
released notes and changes when a channel is allocated to another player.
Accelerated sounds keep their original pitch. A separate effects stream uses
SoundTouch to shorten playback before mixing with music. Normal-speed effects
bypass processing; accelerated effects have a small buffering delay. Returning
to normal speed discards the buffered tail with a short transition. Music stays
at normal speed by leaving its logical player unclassified. The multiplier is a
target rate; reaching it depends on rendering and audio workload.

Simulator builds require SoundTouch 2.3 or later (`soundtouch` via pkg-config).
For Windows, include the SoundTouch DLL alongside the executable. DS builds do
not use this dependency.

## Integrated NitroSystem

`system/` contains NitroSystem's foundation, 2D/3D graphics, VRAM management,
MCS, and sound libraries. Meson provides both `NitroSDK` and `NitroSystem`
from this project; consumers do not need a separate libntrsystem checkout.
The existing `<nnsys.h>` and `<nnsys/...>` include paths remain available
through the `NitroSystem` dependency.

The sources were imported from cybervisi0n/libntrsystem at
`9c2309a2c995c919a67093858677f0f43459d7fc`, with pokeplatinum's
fast-forward players, extended field textures, simulator feature detection,
and BW battle multi-cell bank patches applied. The simulator feature probes
are replaced by explicit feature definitions for the integrated SDK.

For DS builds, `nitrosystem_mwrap_ver` and `nitrosystem_mwrap_sdk` override
the core SDK's compiler and SDK library versions on NitroSystem targets.
For example, a consumer can select `mwrap_sdk=2.0/sp1p2` for NitroSDK and
`nitrosystem_mwrap_sdk=2.0/sp2` for NitroSystem independently.
Meson 1.12 or later is required, matching pokeplatinum's bundled build tool.

## Integrated networking and voice

`wifi/`, `dwc/`, and `vct/` contain the former libntrwifi, libntrdwc, and
libvct projects. They build after `system/` and provide the existing
`NitroWiFi`, `NitroDWC`, and `libvct` Meson dependencies. Public headers,
library target names, and DS prebuilt archives are preserved. Consumers
should list all five dependency names in their libntr wrap's `[provide]`
section and remove the separate WiFi, DWC, and VCT wraps.

Imported sources are pinned here for future upstream comparisons:

| Module | Upstream repository | Imported commit |
| --- | --- | --- |
| WiFi | cybervisi0n/libntrwifi | `067dabb137dfaf80b8ceeb3b3c4a7256bfabc3e0` |
| DWC | cybervisi0n/libntrdwc | `16246ba7b4269e6686f3e4da068e3c996a103739` |
| VCT | cybervisi0n/libvct | `a34ad47fc0d7d964483c287759b704b3b1f3b967` |

Only build integration and whitespace were changed during import.
WiFi and DWC retain their original documentation as `README.upstream.md`;
build these modules through the root SDK project.

`nitrowifi_mwrap_ver` / `nitrowifi_mwrap_sdk` and
`nitrodwc_mwrap_ver` / `nitrodwc_mwrap_sdk` select their respective DS
compiler and SDK library versions. `nitrowifi_sp1p3_conditionals` controls
WiFi's legacy conditional-assembly behavior independently of the core SDK.
`nitrodwc_link_ppwlobby` enables Pokemon lobby hooks supplied by the game;
it defaults to false and must be enabled by consumers that supply those hooks.

When migrating an existing checkout, move or remove the unused downloaded
`subprojects/libvct` directory before configuring. Meson otherwise treats
that directory as another provider of the `libvct` dependency. Existing
build directories also need the renamed module options set with
`meson configure`; fresh builds receive the consuming project's defaults.
