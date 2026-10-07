#include <nitro.h>
#include <simulator/sim.h>
#include <simulator/sim_audio.h>

#include <nitro/wm/ARM7/wm_sp.h>

#include "wmsp_private.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

#ifdef SDK_TRACY_ENABLE
#include "tracy/TracyC.h"
#endif

SIM_queue_t * drawQueuePtr;
SIM_queue_t * pxiQueuePtr;
SIM_queue_t * pxi7to9QueuePtr;

static SIM_queue_t SIM_drawQueue;
static SIM_queue_t SIM_pxiQueue;
static SIM_queue_t SIM_pxi7to9Queue;

extern PXIFifoCallback FifoRecvCallbackTable7[PXI_MAX_FIFO_TAG];

static SDL_sem * pxiSemaphore;
static SDL_sem * pxi7to9Semaphore;
static SDL_sem * osThreadSemaphore;

void SIM_pxiInit(void)
{
    SIM_drawQueue.head = 0;
    SIM_drawQueue.tail = 0;
    SIM_drawQueue.size = 2048;
    SIM_drawQueue.data = malloc( sizeof(void*) * 4096 );

	drawQueuePtr = &SIM_drawQueue;

    SIM_pxiQueue.head = 0;
    SIM_pxiQueue.tail = 0;
    SIM_pxiQueue.size = 2048;
    SIM_pxiQueue.data = malloc( sizeof(void*) * 4096 );

	pxiQueuePtr = &SIM_pxiQueue;

    SIM_pxi7to9Queue.head = 0;
    SIM_pxi7to9Queue.tail = 0;
    SIM_pxi7to9Queue.size = 2048;
    SIM_pxi7to9Queue.data = malloc( sizeof(void*) * 4096 );

	pxi7to9QueuePtr = &SIM_pxi7to9Queue;

	pxiSemaphore = SDL_CreateSemaphore(0);
	pxi7to9Semaphore = SDL_CreateSemaphore(0);
}

void SIM_procPXI(void)
{

	void * data;
	data = SIM_queue_read( pxiQueuePtr );
	while( data != NULL )
	{
		WIN_PXIFifoMessage * msg;
		msg = (WIN_PXIFifoMessage *)data;

		switch( msg->e.tag )
		{
			case PXI_FIFO_TAG_TOUCHPANEL:

				break;
			case PXI_FIFO_TAG_SOUND:
				{
					SNDCommand * sndCommandList;
					sndCommandList = (SNDCommand * )msg->e.data;

					if( sndCommandList == NULL )
					{
						break;
					}
                    // Commands and synthesis share players, channels and decoder state.
                    SDL_LockAudio();
					while( sndCommandList != NULL ){
							switch( sndCommandList->id )
							{
                            case SND_COMMAND_SIM_AUDIO_TIMING:
                                if (sndCommandList->arg[0] == 0) {
                                    SIM_Audio_ApplyPlayerFastForward(sndCommandList->arg[1], sndCommandList->arg[2]);
                                } else {
                                    SIM_Audio_ApplyChannelFastForward(sndCommandList->arg[1], sndCommandList->arg[2]);
                                }
                                break;
							case SND_COMMAND_START_SEQ:
								{

									u8 playerNum = sndCommandList->arg[0];
									u64 sseqDataPtr = sndCommandList->arg[1];
									u32 sseqDataOffset = sndCommandList->arg[2];
									u64 waveBankPtr = sndCommandList->arg[3];

                                    SND_StartSeq7(playerNum, (void *)sseqDataPtr, sseqDataOffset, (void *)waveBankPtr);
								}

            				    break;

            				case SND_COMMAND_STOP_SEQ:

								SND_StopSeq7(sndCommandList->arg[0]);
            				    break;

            				case SND_COMMAND_PREPARE_SEQ:
								{

									u8 playerNum = sndCommandList->arg[0];
									u64 sseqDataPtr = sndCommandList->arg[1];
									u32 sseqDataOffset = sndCommandList->arg[2];
									u64 waveBankPtr = sndCommandList->arg[3];

									SND_PrepareSeq7(playerNum, (void *)sseqDataPtr, sseqDataOffset, (void *)waveBankPtr);
								}

            				    break;

            				case SND_COMMAND_START_PREPARED_SEQ:

								SND_StartPreparedSeq7( sndCommandList->arg[0] );
            				    break;

            				case SND_COMMAND_PAUSE_SEQ:
								{
									u8 playerNum = sndCommandList->arg[0];
									BOOL flag = sndCommandList->arg[1];

									SND_PauseSeq7(playerNum, flag);
								}
            				    break;

            				case SND_COMMAND_SKIP_SEQ:

            				    break;

            				case SND_COMMAND_PLAYER_PARAM:

            				    break;

            				case SND_COMMAND_TRACK_PARAM:

            				    break;

            				case SND_COMMAND_MUTE_TRACK:

            				    break;

            				case SND_COMMAND_ALLOCATABLE_CHANNEL:

            				    break;

            				case SND_COMMAND_PLAYER_LOCAL_VAR:

            				    break;

            				case SND_COMMAND_PLAYER_GLOBAL_VAR:

            				    break;

            				case SND_COMMAND_START_TIMER:

								{
									u32 wav_len = 200;
									u8 channelsEnabled[32] = {0};
									
									u8 * sdlAudioBuf;
									unsigned int sdlAudioLen;
									u32 audioLen;
									for( int i=0; i<32; i++)
									{
										if( (sndCommandList->arg[0] >> i) & 1 )
										{
											channelsEnabled[i] = 1;
                                        if (i < SND_CHANNEL_NUM) SND_StartChannel7(i);
										}
									}

									for( int i=0; i<32; i++)
									{
										if(channelsEnabled[i])
										{

											win_snd_channels[i].alarms_bitfield = sndCommandList->arg[2];

											win_snd_channels[i].audio_len = wav_len;
											win_snd_channels[i].total_audio_len = wav_len;
										}
									}

									for( int i=0; i<32; i++)
									{
										if( (sndCommandList->arg[2] >> i) & 1 )
										{

											if( win_snd_alarms[i].isRunning )
											{

												win_snd_alarms[i].isRunning = 0;
												pthread_join(win_snd_alarms[i].thread, NULL);
											}

											
											pthread_create(  &win_snd_alarms[i].thread, NULL, SIM_sndAlarm, (void *)&win_snd_alarms[i]);
											win_snd_alarms[i].isRunning = 1;

										}
									}
								}
            				    break;

            				case SND_COMMAND_STOP_TIMER:

								u8 channelsEnabled[32] = {0};
								u8 alarmsEnabled[32] = {0};
								for( int i=0; i<32; i++ )
								{
									if( (sndCommandList->arg[0] >> i) & 1 )
									{
										channelsEnabled[i] = 1;
										if (i < SND_CHANNEL_NUM) SND_StopChannel7(i, 0);
                                    win_snd_channels[i].audio_len = 0;
										win_snd_channels[i].total_audio_len = 0;
										win_snd_channel_bufs[i] = NULL;
									}
									if( (sndCommandList->arg[2] >> i) & 1 )
									{
										alarmsEnabled[i] = 1;
										if( win_snd_alarms[i].isRunning )
										{

											win_snd_alarms[i].isRunning = 0;
											pthread_join(win_snd_alarms[i].thread, NULL);
										}
									}
								}								
            				    break;

            				case SND_COMMAND_SETUP_CAPTURE:

            				    break;

            				case SND_COMMAND_SETUP_ALARM:

								win_snd_alarms[sndCommandList->arg[0]].alarmNo = sndCommandList->arg[0];
								win_snd_alarms[sndCommandList->arg[0]].time = sndCommandList->arg[1];
								win_snd_alarms[sndCommandList->arg[0]].period = sndCommandList->arg[2];
								win_snd_alarms[sndCommandList->arg[0]].id = sndCommandList->arg[3];
            				    break;

            				case SND_COMMAND_CHANNEL_TIMER:
                            case SND_COMMAND_CHANNEL_VOLUME:
                            case SND_COMMAND_CHANNEL_PAN:
                                for (int ch = 0; ch < SND_CHANNEL_NUM; ++ch) {
                                    if (!(sndCommandList->arg[0] & (1u << ch))) continue;
                                    if (sndCommandList->id == SND_COMMAND_CHANNEL_TIMER)
                                        SND_SetChannelTimer7(ch, sndCommandList->arg[1]);
                                    else if (sndCommandList->id == SND_COMMAND_CHANNEL_VOLUME)
                                        SND_SetChannelVolume7(ch, sndCommandList->arg[1], sndCommandList->arg[2]);
                                    else
                                        SND_SetChannelPan7(ch, sndCommandList->arg[1]);
                                }
                                break;
                            case SND_COMMAND_SETUP_CHANNEL_PCM: {
                                const u64 *arg = sndCommandList->arg;
                                SND_SetupChannelPcm7(arg[0] & 0xffff, (const void *)arg[1],
                                    (arg[3] >> 24) & 3, (arg[3] >> 26) & 3,
                                    arg[3] & 0xffff, arg[2] & 0x3fffff,
                                    (arg[2] >> 24) & 127, (arg[2] >> 22) & 3,
                                    (arg[0] >> 16) & 0xffff, (arg[3] >> 16) & 127);
                                break;
                            }
                            case SND_COMMAND_SETUP_CHANNEL_PSG:
            				    break;

            				case SND_COMMAND_SETUP_CHANNEL_NOISE:
            				    break;

            				case SND_COMMAND_SURROUND_DECAY:
            				    break;

            				case SND_COMMAND_MASTER_VOLUME:
            				    break;

            				case SND_COMMAND_MASTER_PAN:
            				    break;

            				case SND_COMMAND_OUTPUT_SELECTOR:
            				    break;

            				case SND_COMMAND_LOCK_CHANNEL:
                                SND_LockChannel7(sndCommandList->arg[0], sndCommandList->arg[1]);
            				    break;

            				case SND_COMMAND_UNLOCK_CHANNEL:
                                SND_UnlockChannel7(sndCommandList->arg[0], sndCommandList->arg[1]);
            				    break;

            				case SND_COMMAND_STOP_UNLOCKED_CHANNEL:
                                SND_StopUnlockedChannel7(sndCommandList->arg[0], sndCommandList->arg[1]);
            				    break;

            				case SND_COMMAND_INVALIDATE_SEQ:
            				    break;

            				case SND_COMMAND_INVALIDATE_BANK:
            				    break;

            				case SND_COMMAND_INVALIDATE_WAVE:
            				    break;

            				case SND_COMMAND_SHARED_WORK:
            				    win_SNDi_SharedWork = (SNDSharedWork *)sndCommandList->arg[0];
            				    break;

            				case SND_COMMAND_READ_DRIVER_INFO:
            				    break;
							default:
								break;
						}
						sndCommandList = sndCommandList->next;
					} 
                    if (SNDi_SharedWork) {
                        u16 status = 0;
                        for (int ch = 0; ch < SND_CHANNEL_NUM; ++ch) {
                            if (SND_IsChannelActive7(ch)) status |= 1u << ch;
                        }
                        SNDi_SharedWork->channelStatus = status;
                    }
                    SDL_UnlockAudio();
						if( win_SNDi_SharedWork )
						{
							win_SNDi_SharedWork->finishCommandTag++;
						}
					
				}
				break;
            case PXI_FIFO_TAG_WM:

                {

					if(FifoRecvCallbackTable7[PXI_FIFO_TAG_WM]) {
						FifoRecvCallbackTable7[PXI_FIFO_TAG_WM](PXI_FIFO_TAG_WM, msg->e.data, (BOOL)msg->e.err);
					}
                }
                break;
			case PXI_FIFO_TAG_OS: {

				u32 pxiData = msg->e.data;
				u32 osCommand = pxiData >> 8 ;
				switch(osCommand) {
					case 0x10 :
						OS_Printf("Reset command received.\n");
						break;
					default:
						break;
				}
			} break;
			case PXI_FIFO_TAG_WVR: {
				if(FifoRecvCallbackTable7[PXI_FIFO_TAG_WVR]) {
					FifoRecvCallbackTable7[PXI_FIFO_TAG_WVR](PXI_FIFO_TAG_WVR, msg->e.data, (BOOL)msg->e.err);
				}
			} break;
			default:
				break;
		}

		free( data );

		data = SIM_queue_read( pxiQueuePtr );

	}
}

void SIM_procPXI7to9(void)
{

	void * data;
	data = SIM_queue_read( pxi7to9QueuePtr );
	while( data != NULL)
	{
		WIN_PXIFifoMessage * msg;
		msg = (WIN_PXIFifoMessage *)data;

		PXIFifoCallback myCallback = NULL;

		myCallback = PXI_WIN_GetCallback(msg->e.tag);

		if( myCallback != NULL )
		{
			myCallback(msg->e.tag, msg->e.data, msg->e.err);
		}

		free( data );
		
		data = SIM_queue_read( pxi7to9QueuePtr );
	}
}

int SIM_procPxiThread( void * arg )
{
	while(1)
	{
		SDL_SemWait(pxiSemaphore);
		#ifdef SDK_TRACY_ENABLE
		TracyCZone(ctx, 1);
		#endif
		SIM_procPXI();
		#ifdef SDK_TRACY_ENABLE
		TracyCZoneEnd(ctx);
		#endif
	}
	return 0;
}

void SIM_PostPxiThread()
{
	SDL_SemPost(pxiSemaphore);
}

int SIM_procPxi7to9Thread( void * arg )
{
	while(1)
	{
		SDL_SemWait(pxi7to9Semaphore);
		#ifdef SDK_TRACY_ENABLE
		TracyCZone(ctx, 1);
		#endif
		SIM_procPXI7to9();
		#ifdef SDK_TRACY_ENABLE
		TracyCZoneEnd(ctx);
		#endif
	}
	return 0;
}

void SIM_PostPxi7to9Thread()
{
	SDL_SemPost(pxi7to9Semaphore);
}

void * SIM_sndAlarm(void * arg)
{
	WIN_snd_alarm_info * sndAlarmInfo;
	sndAlarmInfo = (WIN_snd_alarm_info *)arg;
	u32 timeMs;
	u32 periodMs;

	double timeSec = (double)(sndAlarmInfo->time * 32.0f) / (double)SND_TIMER_CLOCK;
	timeMs = (u32)(timeSec * 1000.0f);
	double periodSec = (double)(sndAlarmInfo->period * 32.0f) / (double)SND_TIMER_CLOCK;
	periodMs = (u32)(periodSec * 1000.0f);
	SDL_Delay( timeMs );
	SNDi_CallAlarmHandler( sndAlarmInfo->alarmNo | (sndAlarmInfo->id) <<8 );

	if( sndAlarmInfo->period != 0 )
	{
		while( 1 )
		{
			if( !sndAlarmInfo->isRunning )
			{
				return NULL;
			}
			SDL_Delay( periodMs );
			SNDi_CallAlarmHandler( sndAlarmInfo->alarmNo | (sndAlarmInfo->id) <<8 );
		}
	}

}
