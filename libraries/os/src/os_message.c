#include <nitro/os.h>
#include <nitro/os/common/message.h>

#ifdef SDK_PORT
#include <SDL2/SDL.h>
#endif

void OS_InitMessageQueue (OSMessageQueue * mq, OSMessage * msgArray, s32 msgCount)
{
    OS_InitThreadQueue(&mq->queueSend);
    OS_InitThreadQueue(&mq->queueReceive);

    mq->msgArray = msgArray;
    mq->msgCount = msgCount;
    mq->firstIndex = 0;
    mq->usedCount = 0;
    #ifdef SDK_PORT
    mq->semaphore = SDL_CreateSemaphore(0);
    #endif
}

// Disabling interrupts does nothing on the host, where the sender and the receiver of a queue are threads that run
// at once: without a lock they lose updates of usedCount, and with them messages, such as a WM request
#ifdef SDK_PORT
static SDL_SpinLock sMessageLock;
#define MESSAGE_LOCK() SDL_AtomicLock(&sMessageLock)
#define MESSAGE_UNLOCK() SDL_AtomicUnlock(&sMessageLock)
#else
#define MESSAGE_LOCK()
#define MESSAGE_UNLOCK()
#endif

BOOL OS_SendMessage (OSMessageQueue * mq, OSMessage msg, s32 flags)
{
    OSIntrMode enabled;
    s32 lastIndex;

    enabled = OS_DisableInterrupts();
    MESSAGE_LOCK();

    while (mq->msgCount <= mq->usedCount) {
        if (!(flags & OS_MESSAGE_BLOCK)) {
            MESSAGE_UNLOCK();
            (void)OS_RestoreInterrupts(enabled);
            return FALSE;
        } else {
            MESSAGE_UNLOCK();
            OS_SleepThread(&mq->queueSend);
            MESSAGE_LOCK();
        }
    }

    lastIndex = (mq->firstIndex + mq->usedCount) % mq->msgCount;
    mq->msgArray[lastIndex] = msg;
    mq->usedCount++;
    MESSAGE_UNLOCK();

    OS_WakeupThread(&mq->queueReceive);
    #ifdef SDK_PORT
    SDL_SemPost(mq->semaphore);
    #endif

    (void)OS_RestoreInterrupts(enabled);
    return TRUE;
}

BOOL OS_ReceiveMessage (OSMessageQueue * mq, OSMessage * msg, s32 flags)
{
    OSIntrMode enabled;

    enabled = OS_DisableInterrupts();
    MESSAGE_LOCK();

    while (mq->usedCount == 0) {
        if (!(flags & OS_MESSAGE_BLOCK)) {
            MESSAGE_UNLOCK();
            (void)OS_RestoreInterrupts(enabled);
            return FALSE;
        } else {
            MESSAGE_UNLOCK();
            #ifdef SDK_PORT
            SDL_SemWaitTimeout(mq->semaphore, 1);
            #else
            OS_SleepThread(&mq->queueReceive);
            #endif
            MESSAGE_LOCK();
        }
    }

    if (msg != NULL) {
        *msg = mq->msgArray[mq->firstIndex];
    }

    mq->firstIndex = (mq->firstIndex + 1) % mq->msgCount;
    mq->usedCount--;
    MESSAGE_UNLOCK();

    OS_WakeupThread(&mq->queueSend);

    (void)OS_RestoreInterrupts(enabled);
    return TRUE;
}

BOOL OS_JamMessage (OSMessageQueue * mq, OSMessage msg, s32 flags)
{
    OSIntrMode enabled;

    enabled = OS_DisableInterrupts();
    MESSAGE_LOCK();

    while (mq->msgCount <= mq->usedCount) {
        if (!(flags & OS_MESSAGE_BLOCK)) {
            MESSAGE_UNLOCK();
            (void)OS_RestoreInterrupts(enabled);
            return FALSE;
        } else {
            MESSAGE_UNLOCK();
            OS_SleepThread(&mq->queueSend);
            MESSAGE_LOCK();
        }
    }

    mq->firstIndex = (mq->firstIndex + mq->msgCount - 1) % mq->msgCount;
    mq->msgArray[mq->firstIndex] = msg;
    mq->usedCount++;
    MESSAGE_UNLOCK();

    OS_WakeupThread(&mq->queueReceive);

    (void)OS_RestoreInterrupts(enabled);
    return TRUE;
}

BOOL OS_ReadMessage (OSMessageQueue * mq, OSMessage * msg, s32 flags)
{
    OSIntrMode enabled;

    enabled = OS_DisableInterrupts();
    MESSAGE_LOCK();

    while (mq->usedCount == 0) {
        if (!(flags & OS_MESSAGE_BLOCK)) {
            MESSAGE_UNLOCK();
            (void)OS_RestoreInterrupts(enabled);
            return FALSE;
        } else {
            MESSAGE_UNLOCK();
            OS_SleepThread(&mq->queueReceive);
            MESSAGE_LOCK();
        }
    }

    if (msg != NULL) {
        *msg = mq->msgArray[mq->firstIndex];
    }
    MESSAGE_UNLOCK();

    (void)OS_RestoreInterrupts(enabled);
    return TRUE;
}
