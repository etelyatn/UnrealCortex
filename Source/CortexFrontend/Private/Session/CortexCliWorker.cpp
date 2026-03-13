#include "Session/CortexCliWorker.h"

#include "Session/CortexCliSession.h"

FCortexCliWorker::FCortexCliWorker(TWeakPtr<FCortexCliSession> InSession)
    : Session(InSession)
{
}

FCortexCliWorker::~FCortexCliWorker()
{
    Stop();

    if (Thread)
    {
        Thread->Kill(true);
        delete Thread;
        Thread = nullptr;
    }
}

bool FCortexCliWorker::Init()
{
    return true;
}

uint32 FCortexCliWorker::Run()
{
    while (!bStopRequested.load())
    {
        FPlatformProcess::Sleep(0.01f);
    }

    return 0;
}

void FCortexCliWorker::Stop()
{
    bStopRequested.store(true);
}
