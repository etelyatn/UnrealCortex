#include "Session/CortexCliWorker.h"

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Session/CortexCliSession.h"

namespace
{
    constexpr double CortexCliWorkerTimeoutSeconds = 300.0;
}

FCortexCliWorker::FCortexCliWorker(TWeakPtr<FCortexCliSession> InSession)
    : Session(InSession)
{
    Thread = FRunnableThread::Create(this, TEXT("CortexCliWorker"), 0, TPri_Normal);
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
    while (!bStopRequested.load(std::memory_order_acquire))
    {
        const TSharedPtr<FCortexCliSession> SessionPin = Session.Pin();
        if (!SessionPin.IsValid())
        {
            break;
        }

        if (SessionPin->PromptReadyEvent == nullptr || !SessionPin->PromptReadyEvent->Wait(100))
        {
            continue;
        }

        if (bStopRequested.load(std::memory_order_acquire))
        {
            break;
        }

        const FString PromptEnvelope = SessionPin->ConsumePendingPromptEnvelope();
        if (PromptEnvelope.IsEmpty())
        {
            continue;
        }

        if (SessionPin->StdinWritePipe == nullptr || !FPlatformProcess::WritePipe(SessionPin->StdinWritePipe, PromptEnvelope))
        {
            AsyncTask(ENamedThreads::GameThread, [WeakSession = Session]()
            {
                if (const TSharedPtr<FCortexCliSession> Pinned = WeakSession.Pin())
                {
                    Pinned->HandleProcessExited(TEXT("Failed to write prompt to Claude CLI"));
                }
            });
            continue;
        }

        double LastDataTime = FPlatformTime::Seconds();
        bool bTurnComplete = false;

        while (!bStopRequested.load(std::memory_order_relaxed))
        {
            FString Chunk = SessionPin->StdoutReadPipe != nullptr ? FPlatformProcess::ReadPipe(SessionPin->StdoutReadPipe) : FString();
            if (!Chunk.IsEmpty())
            {
                LastDataTime = FPlatformTime::Seconds();
                const bool bSawResult = Chunk.Contains(TEXT("\"type\":\"result\""));
                ParseAndDispatch(Chunk);
                bTurnComplete = bSawResult;
                if (bTurnComplete)
                {
                    break;
                }
                continue;
            }

            if (!FPlatformProcess::IsProcRunning(SessionPin->ProcessHandle))
            {
                AsyncTask(ENamedThreads::GameThread, [WeakSession = Session]()
                {
                    if (const TSharedPtr<FCortexCliSession> Pinned = WeakSession.Pin())
                    {
                        Pinned->HandleProcessExited(TEXT("Claude CLI process exited"));
                    }
                });
                break;
            }

            if (FPlatformTime::Seconds() - LastDataTime > CortexCliWorkerTimeoutSeconds)
            {
                AsyncTask(ENamedThreads::GameThread, [WeakSession = Session]()
                {
                    if (const TSharedPtr<FCortexCliSession> Pinned = WeakSession.Pin())
                    {
                        Pinned->HandleProcessExited(TEXT("Claude CLI timed out"));
                    }
                });
                break;
            }

            FPlatformProcess::Sleep(0.01f);
        }
    }

    return 0;
}

void FCortexCliWorker::Stop()
{
    bStopRequested.store(true, std::memory_order_release);

    if (const TSharedPtr<FCortexCliSession> SessionPin = Session.Pin())
    {
        if (SessionPin->PromptReadyEvent != nullptr)
        {
            SessionPin->PromptReadyEvent->Trigger();
        }
    }
}

void FCortexCliWorker::ParseAndDispatch(const FString& Chunk)
{
    NdjsonLineBuffer += Chunk;

    int32 NewlineIndex = INDEX_NONE;
    while (NdjsonLineBuffer.FindChar(TEXT('\n'), NewlineIndex))
    {
        const FString Line = NdjsonLineBuffer.Left(NewlineIndex).TrimEnd();
        NdjsonLineBuffer.RightChopInline(NewlineIndex + 1, EAllowShrinking::No);

        if (Line.IsEmpty())
        {
            continue;
        }

        TArray<FCortexStreamEvent> Events = CortexStreamEventParser::ParseNdjsonLine(Line);
        for (const FCortexStreamEvent& Event : Events)
        {
            AsyncTask(ENamedThreads::GameThread, [WeakSession = Session, Event]()
            {
                if (const TSharedPtr<FCortexCliSession> Pinned = WeakSession.Pin())
                {
                    Pinned->HandleWorkerEvent(Event);
                }
            });
        }
    }
}
