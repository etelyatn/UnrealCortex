#include "Session/CortexCliWorker.h"

#include "Async/Async.h"
#include "CortexFrontendModule.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Providers/CortexCliProvider.h"
#include "Session/CortexCliSession.h"

namespace
{
	constexpr double CortexCliWorkerTimeoutSeconds = 300.0;
}

FCortexCliWorker::FCortexCliWorker(
	TWeakPtr<FCortexCliSession> InSession,
	const ICortexCliProvider* InProvider,
	void* InStdoutReadPipe,
	void* InStdinWritePipe,
	FProcHandle InProcessHandle,
	FEvent* InPromptReadyEvent)
	: WeakSession(InSession)
	, Provider(InProvider)
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
	UE_LOG(LogCortexFrontend, Log, TEXT("CLI Worker: stdout pipe=%p, stdin pipe=%p, process valid=%s"),
		StdoutReadPipe, StdinWritePipe,
		ProcessHandle.IsValid() ? TEXT("true") : TEXT("false"));

	double LastDataTime = FPlatformTime::Seconds();
	double LastHeartbeatTime = FPlatformTime::Seconds();
	bool bAwaitingResult = false;

	// This thread ONLY reads stdout. Stdin writes happen on a background thread
	// (launched below when PromptReadyEvent fires). This prevents deadlock:
	// the reader never blocks, so stdout is always drained, so the CLI process
	// never blocks on stdout writes, so it keeps reading stdin.

	while (!bStopRequested.load(std::memory_order_acquire))
	{
		// === STEP 1: Read stdout (never blocks for long) ===
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
				bAwaitingResult = false;
			}
			// Loop immediately to drain more stdout
			continue;
		}

		// === STEP 2: Launch stdin write on separate thread when signaled ===
		// WritePipe can block if the CLI isn't reading stdin yet (pipe buffer full).
		// By writing on a SEPARATE thread, this thread keeps draining stdout,
		// preventing the bidirectional pipe deadlock.
		if (PromptReadyEvent != nullptr && PromptReadyEvent->Wait(0))
		{
			if (bStopRequested.load(std::memory_order_acquire))
			{
				break;
			}

			FString PromptEnvelope;
			{
				TSharedPtr<FCortexCliSession> SessionPin = WeakSession.Pin();
				if (!SessionPin.IsValid())
				{
					break;
				}
				PromptEnvelope = SessionPin->ConsumePendingPromptEnvelope();
			}

			if (!PromptEnvelope.IsEmpty())
			{
				UE_LOG(LogCortexFrontend, Log, TEXT("Launching stdin write on background thread (%d chars)..."),
					PromptEnvelope.Len());
				bAwaitingResult = true;
				LastDataTime = FPlatformTime::Seconds();

				// Capture by value — the background thread outlives this scope
				void* WritePipe = StdinWritePipe;
				TWeakPtr<FCortexCliSession> WeakCopy = WeakSession;
				Async(EAsyncExecution::ThreadPool,
					[PromptEnvelope, WritePipe, WeakCopy]()
					{
						if (WritePipe == nullptr)
						{
							UE_LOG(LogCortexFrontend, Warning, TEXT("Stdin write: pipe is null"));
							return;
						}

						UE_LOG(LogCortexFrontend, Log, TEXT("Stdin write: starting (%d chars)..."),
							PromptEnvelope.Len());

						const bool bSuccess = FPlatformProcess::WritePipe(WritePipe, PromptEnvelope);

						if (bSuccess)
						{
							UE_LOG(LogCortexFrontend, Log, TEXT("Stdin write: completed successfully"));
						}
						else
						{
							UE_LOG(LogCortexFrontend, Warning, TEXT("Stdin write: FAILED"));
							AsyncTask(ENamedThreads::GameThread, [WeakCopy]()
							{
								if (const TSharedPtr<FCortexCliSession> Pinned = WeakCopy.Pin())
								{
									Pinned->HandleProcessExited(TEXT("Failed to write prompt to provider CLI"));
								}
							});
						}
					});
			}
		}

		// === STEP 3: Check process health ===
		if (!FPlatformProcess::IsProcRunning(ProcessHandle))
		{
			UE_LOG(LogCortexFrontend, Log, TEXT("CLI process exited"));
			AsyncTask(ENamedThreads::GameThread, [WeakCopy = WeakSession]()
			{
				if (const TSharedPtr<FCortexCliSession> Pinned = WeakCopy.Pin())
				{
					Pinned->HandleProcessExited(TEXT("Provider CLI process exited"));
				}
			});
			break;
		}

		// Timeout check
		if (bAwaitingResult && FPlatformTime::Seconds() - LastDataTime > CortexCliWorkerTimeoutSeconds)
		{
			UE_LOG(LogCortexFrontend, Warning, TEXT("CLI response timed out after %.0f seconds"), CortexCliWorkerTimeoutSeconds);
			AsyncTask(ENamedThreads::GameThread, [WeakCopy = WeakSession]()
			{
				if (const TSharedPtr<FCortexCliSession> Pinned = WeakCopy.Pin())
				{
					Pinned->HandleProcessExited(TEXT("Provider CLI timed out"));
				}
			});
			break;
		}

		// Heartbeat
		const double Now = FPlatformTime::Seconds();
		if (Now - LastHeartbeatTime > 5.0)
		{
			const double WaitingSecs = Now - LastDataTime;
			UE_LOG(LogCortexFrontend, Log,
				TEXT("CLI Worker heartbeat: %.0fs elapsed, awaiting=%s, process=%s"),
				WaitingSecs,
				bAwaitingResult ? TEXT("YES") : TEXT("no"),
				FPlatformProcess::IsProcRunning(ProcessHandle) ? TEXT("running") : TEXT("DEAD"));
			LastHeartbeatTime = Now;
		}

		FPlatformProcess::Sleep(0.01f);
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

	if (Provider == nullptr)
	{
		return false;
	}

	TArray<FCortexStreamEvent> Events;
	Provider->ConsumeStreamChunk(Chunk, StreamBuffer, AssistantTextBuffer, Events);
	for (const FCortexStreamEvent& Event : Events)
	{
		if (Event.Type == ECortexStreamEventType::Result)
		{
			bSawResult = true;
		}

		if (Event.Type == ECortexStreamEventType::SessionInit)
		{
			UE_LOG(LogCortexFrontend, Log, TEXT("CLI init event received"));
		}

		AsyncTask(ENamedThreads::GameThread, [WeakCopy = WeakSession, Event]()
		{
			if (const TSharedPtr<FCortexCliSession> Pinned = WeakCopy.Pin())
			{
				Pinned->HandleWorkerEvent(Event);
			}
		});
	}

	return bSawResult;
}
