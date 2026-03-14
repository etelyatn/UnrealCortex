#include "Session/CortexCliWorker.h"

#include "Async/Async.h"
#include "CortexFrontendModule.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Session/CortexCliSession.h"

namespace
{
	constexpr double CortexCliWorkerTimeoutSeconds = 300.0;
}

FCortexCliWorker::FCortexCliWorker(
	TWeakPtr<FCortexCliSession> InSession,
	void* InStdoutReadPipe,
	void* InStdinWritePipe,
	FProcHandle InProcessHandle,
	FEvent* InPromptReadyEvent)
	: WeakSession(InSession)
	, StdoutReadPipe(InStdoutReadPipe)
	, StdinWritePipe(InStdinWritePipe)
	, ProcessHandle(InProcessHandle)
	, PromptReadyEvent(InPromptReadyEvent)
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
	UE_LOG(LogCortexFrontend, Log, TEXT("CLI Worker thread started"));

	while (!bStopRequested.load(std::memory_order_acquire))
	{
		if (PromptReadyEvent == nullptr || !PromptReadyEvent->Wait(1000))
		{
			continue;
		}

		if (bStopRequested.load(std::memory_order_acquire))
		{
			break;
		}

		// Briefly pin Session to consume the pending prompt envelope
		FString PromptEnvelope;
		{
			TSharedPtr<FCortexCliSession> SessionPin = WeakSession.Pin();
			if (!SessionPin.IsValid())
			{
				break;
			}
			PromptEnvelope = SessionPin->ConsumePendingPromptEnvelope();
		}

		if (PromptEnvelope.IsEmpty())
		{
			continue;
		}

		if (StdinWritePipe == nullptr || !FPlatformProcess::WritePipe(StdinWritePipe, PromptEnvelope))
		{
			UE_LOG(LogCortexFrontend, Warning, TEXT("Failed to write prompt to CLI stdin"));
			AsyncTask(ENamedThreads::GameThread, [WeakCopy = WeakSession]()
			{
				if (const TSharedPtr<FCortexCliSession> Pinned = WeakCopy.Pin())
				{
					Pinned->HandleProcessExited(TEXT("Failed to write prompt to Claude CLI"));
				}
			});
			continue;
		}

		UE_LOG(LogCortexFrontend, Verbose, TEXT("Prompt written to CLI stdin, entering response read loop"));

		double LastDataTime = FPlatformTime::Seconds();

		while (!bStopRequested.load(std::memory_order_relaxed))
		{
			FString Chunk = StdoutReadPipe != nullptr
				? FPlatformProcess::ReadPipe(StdoutReadPipe)
				: FString();

			if (!Chunk.IsEmpty())
			{
				LastDataTime = FPlatformTime::Seconds();
				const bool bSawResult = ParseAndDispatch(Chunk);
				if (bSawResult)
				{
					UE_LOG(LogCortexFrontend, Log, TEXT("Turn complete (result event received)"));
					break;
				}
				continue;
			}

			if (!FPlatformProcess::IsProcRunning(ProcessHandle))
			{
				UE_LOG(LogCortexFrontend, Log, TEXT("CLI process exited during response read"));
				AsyncTask(ENamedThreads::GameThread, [WeakCopy = WeakSession]()
				{
					if (const TSharedPtr<FCortexCliSession> Pinned = WeakCopy.Pin())
					{
						Pinned->HandleProcessExited(TEXT("Claude CLI process exited"));
					}
				});
				break;
			}

			if (FPlatformTime::Seconds() - LastDataTime > CortexCliWorkerTimeoutSeconds)
			{
				UE_LOG(LogCortexFrontend, Warning, TEXT("CLI response timed out after %.0f seconds"), CortexCliWorkerTimeoutSeconds);
				AsyncTask(ENamedThreads::GameThread, [WeakCopy = WeakSession]()
				{
					if (const TSharedPtr<FCortexCliSession> Pinned = WeakCopy.Pin())
					{
						Pinned->HandleProcessExited(TEXT("Claude CLI timed out"));
					}
				});
				break;
			}

			FPlatformProcess::Sleep(0.01f);
		}
	}

	UE_LOG(LogCortexFrontend, Log, TEXT("CLI Worker thread exiting"));
	return 0;
}

void FCortexCliWorker::Stop()
{
	bStopRequested.store(true, std::memory_order_release);

	if (PromptReadyEvent != nullptr)
	{
		PromptReadyEvent->Trigger();
	}
}

bool FCortexCliWorker::ParseAndDispatch(const FString& Chunk)
{
	bool bSawResult = false;

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
			if (Event.Type == ECortexStreamEventType::Result)
			{
				bSawResult = true;
			}

			AsyncTask(ENamedThreads::GameThread, [WeakCopy = WeakSession, Event]()
			{
				if (const TSharedPtr<FCortexCliSession> Pinned = WeakCopy.Pin())
				{
					Pinned->HandleWorkerEvent(Event);
				}
			});
		}
	}

	return bSawResult;
}
