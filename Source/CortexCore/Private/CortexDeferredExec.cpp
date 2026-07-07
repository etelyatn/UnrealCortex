#include "CortexDeferredExec.h"

#include "Containers/Ticker.h"
#include "CortexCommandRouter.h"

FCortexCommandResult FCortexDeferredExec::RunNextTick(
	TFunction<FCortexCommandResult()>&& Work,
	FDeferredResponseCallback DeferredCallback)
{
	if (!IsInGameThread())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("run_next_tick must be scheduled on the Game Thread"));
	}

	if (!DeferredCallback)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("run_next_tick requires a deferred callback"));
	}

	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda(
			[Work = MoveTemp(Work), Callback = MoveTemp(DeferredCallback)](float DeltaTime) mutable
			{
				(void)DeltaTime;
				Callback(Work());
				return false;
			}),
		0.0f);

	FCortexCommandResult Deferred;
	Deferred.bIsDeferred = true;
	return Deferred;
}
