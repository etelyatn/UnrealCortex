#include "CortexPIEUtils.h"

#include "Containers/Ticker.h"
#include "CortexCommandRouter.h"
#include "CortexEditorModule.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

UWorld* FCortexPIEUtils::GetPIEWorld()
{
	if (GEditor == nullptr)
	{
		return nullptr;
	}

	return GEditor->PlayWorld;
}

AActor* FCortexPIEUtils::FindActorInPIE(UWorld* PIEWorld, const FString& ActorIdentifier)
{
	if (PIEWorld == nullptr || ActorIdentifier.IsEmpty())
	{
		return nullptr;
	}

	for (TActorIterator<AActor> It(PIEWorld); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor))
		{
			continue;
		}

		if (Actor->GetActorLabel() == ActorIdentifier
			|| Actor->GetName() == ActorIdentifier
			|| Actor->GetPathName() == ActorIdentifier)
		{
			return Actor;
		}
	}

	return nullptr;
}

FCortexCommandResult FCortexPIEUtils::PIENotActiveError()
{
	FCortexCommandResult Result;
	Result.bSuccess = false;
	Result.ErrorCode = CortexErrorCodes::PIENotActive;
	Result.ErrorMessage = TEXT("Operation requires an active PIE session. Start PIE with editor.start_pie, then retry.");
	return Result;
}

FCortexCommandResult FCortexPIEUtils::StartDeferredPIEPoll(
	UWorld* World,
	const float IntervalSeconds,
	const double OperationSeconds,
	const FString& OperationLabel,
	FDeferredResponseCallback DeferredCallback,
	FPIEPoll Poll)
{
	// The core ticker keeps running after PIE teardown destroys the world and its timer manager.
	constexpr double TransportGraceSeconds = 5.0;

	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda(
			[WeakWorld = TWeakObjectPtr<UWorld>(World), OperationLabel, Callback = MoveTemp(DeferredCallback), Poll = MoveTemp(Poll)](float DeltaTime) mutable
			{
				(void)DeltaTime;
				UWorld* PollWorld = WeakWorld.Get();
				if (PollWorld == nullptr || PollWorld != GetPIEWorld())
				{
					Callback(FCortexCommandRouter::Error(
						CortexErrorCodes::PIETerminated,
						FString::Printf(TEXT("PIE terminated during %s"), *OperationLabel)));
					return false;
				}

				if (PollWorld->IsPaused())
				{
					return true;
				}

				TOptional<FCortexCommandResult> Final = Poll(*PollWorld);
				if (!Final.IsSet())
				{
					return true;
				}

				Callback(MoveTemp(Final.GetValue()));
				return false;
			}),
		IntervalSeconds);

	FCortexCommandResult Deferred;
	Deferred.bIsDeferred = true;
	Deferred.DeferredTimeoutSeconds = FMath::Max(OperationSeconds, 0.0) + TransportGraceSeconds;
	return Deferred;
}
