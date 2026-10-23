#include <nitro.h>
#include <vct.h>

void VCT_EnableEchoCancel(BOOL flag)
{

}


void VCT_EnableVAD(BOOL flag)
{

}


void VCT_GetAudioInfo(u32 channel, VCTAudioInfo *info)
{

}


VCTCodec VCT_GetCodec(void)
{
    return VCT_CODEC_8BIT_RAW;
}


BOOL VCT_ReceiveAudio(void* audio_data, u32 length, u32 *outAIDBitmap)
{
    return FALSE;
}


BOOL VCT_SendAudio(void* audio_data, u32 length)
{
    return FALSE;
}


BOOL VCT_SetCodec(VCTCodec codec)
{
    return FALSE;
}


BOOL VCT_StartStreaming(VCTSession *session)
{
    return FALSE;
}


void VCT_StopStreaming(VCTSession *session)
{

}
