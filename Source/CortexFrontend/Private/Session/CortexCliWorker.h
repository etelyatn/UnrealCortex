#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include <atomic>

class FCortexCliSession;

class FCortexCliWorker : public FRunnable
{
public:
    explicit FCortexCliWorker(TWeakPtr<FCortexCliSession> InSession);
    virtual ~FCortexCliWorker() override;

    virtual bool Init() override;
    virtual uint32 Run() override;
    virtual void Stop() override;

private:
    TWeakPtr<FCortexCliSession> Session;
    FRunnableThread* Thread = nullptr;
    std::atomic<bool> bStopRequested{false};
};
