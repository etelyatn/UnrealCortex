#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include <atomic>

class FCortexCliSession;
class ICortexCliProvider;

class FCortexCliWorker : public FRunnable
{
public:
	FCortexCliWorker(
		TWeakPtr<FCortexCliSession> InSession,
		const ICortexCliProvider* InProvider,
		void* InStdoutReadPipe,
		void* InStdinWritePipe,
		FProcHandle InProcessHandle,
		FEvent* InPromptReadyEvent);
	virtual ~FCortexCliWorker() override;

	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;

private:
	bool ParseAndDispatch(const FString& Chunk);

	TWeakPtr<FCortexCliSession> WeakSession;
	const ICortexCliProvider* Provider;
	void* StdoutReadPipe;
	void* StdinWritePipe;
	FProcHandle ProcessHandle;
	FEvent* PromptReadyEvent;
	FRunnableThread* Thread = nullptr;
	std::atomic<bool> bStopRequested{false};
	FString StreamBuffer;
	FString AssistantTextBuffer;
};
