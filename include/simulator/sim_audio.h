#ifndef SIM_AUDIO_H
#define SIM_AUDIO_H

#include <nitro.h>
#include <nitro/types.h>
#include <simulator/sim.h>
#include <SDL2/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

extern u32 s_SIM_sndcnt[16];
extern u8 * s_SIM_sndsad[16];
extern u16 s_SIM_sndtmr[16];
extern u16 s_SIM_sndpnt[16];
extern u32 s_SIM_sndlen[16];

void SIM_Audio_Init(int aAudioFrequency);
void SIM_Audio_Callback(void *userdata, Uint8 *stream, int len);

// Main-thread speed publication. The callback snapshots it atomically.
void SIM_Audio_SetFastForwardMultiplier(float multiplier);
void SIM_Audio_SetMasterVolume(int volume);
// Queue timing ownership alongside the usual sound commands.
void SIM_Audio_SetPlayerFastForward(int playerNo, BOOL enabled);
void SIM_Audio_SetChannelFastForward(int chNo, BOOL enabled);
// Sound-worker/callback only: ownership survives detached release tails.
void SIM_Audio_ApplyPlayerFastForward(int playerNo, BOOL enabled);
void SIM_Audio_ApplyChannelFastForward(int chNo, BOOL enabled);
void SIM_Audio_InheritChannelFastForward(int chNo, int playerNo);
BOOL SIM_Audio_IsPlayerFastForward(int playerNo);
BOOL SIM_Audio_IsChannelFastForward(int chNo);

void SIM_Audio_StartChannel(int chNo);
// The cycles a channel has run, at SND_TIMER_CLOCK, as of the last audio callback. Sound alarms count on it, so that a
// stream's refills keep pace with its channel, as the ARM7's timers do on a DS.
u64 SIM_Audio_GetChannelClock(int chNo);
// Whether the audio device is open, so that the channel clocks advance
BOOL SIM_Audio_IsRunning(void);

s32 SIM_Audio_RunChannel(u32 cycles, int chNo);

void SIM_Audio_NextSamplePCM8(int chNo); 
void SIM_Audio_NextSamplePCM16(int chNo);
void SIM_Audio_NextSampleADPCM(int chNo);
void SIM_Audio_NextSamplePSG(int chNo);
void SIM_Audio_NextSampleNoise(int chNo);

#ifdef __cplusplus
}
#endif

#endif
