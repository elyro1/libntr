#include <nitro.h>
#include <vct.h>


int VCT_AddConferenceClient(u8 aid)
{
    return 0;
}


int VCT_Contact(VCTSession **outSession)
{
    return 0;
}


VCTSession * VCT_CreateSession(u8 aid)
{
    return NULL;
}


BOOL VCT_DeleteSession(VCTSession *session)
{
    return FALSE;
}


int VCT_Release(VCTSession *session)
{
    return 0;
}


int VCT_RemoveConferenceClient(u8 aid)
{
    return 0;
}


int VCT_Request(VCTSession *session, VCTRequestCode request)
{
    return 0;
}


int VCT_Response(VCTSession *session, VCTResponseCode response)
{
    return 0;
}


int VCT_SetTransceiverClients(u8 aidList[], int num_of_aid)
{
    return 0;
}


int VCT_SetTransceiverServer(u8 aid)
{
    return 0;
}


void VCT_SetTransceiverTimeout(u32 sec)
{

}
