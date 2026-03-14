#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include <atomic>

class FCortexCliSession;

class FCortexCliWorker : public FRunnable
{
public:
	FCortexCliWorker(
		TWeakPtr<FCortexCliSession> InSession,
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
	void* StdoutReadPipe;
	void* StdinWritePipe;
	FProcHandle ProcessHandle;
	FEvent* PromptReadyEvent;
	FRunnableThread* Thread = nullptr;
	std::atomic<bool> bStopRequested{false};
	FString NdjsonLineBuffer;
};
