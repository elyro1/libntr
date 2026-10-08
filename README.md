# libntr
![logo](images/libntr.png)
Drop-in replacement for NitroSDK that targets Nintendo DS/DSi, PC, and other platforms.

libntr makes it easier to port Nintendo DS/DSi games to PC and other platforms by handling the logic for common libraries used by nearly all Nintendo DS games.

See also: 
 * [libntrsystem](https://github.com/cybervisi0n/libntrsystem)
 * [libntrwifi](https://github.com/cybervisi0n/libntrwifi)
 * [libntrdwc](https://github.com/cybervisi0n/libntrdwc)

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
