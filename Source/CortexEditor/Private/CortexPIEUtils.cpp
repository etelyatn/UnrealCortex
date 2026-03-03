#include "CortexPIEUtils.h"

#include "CortexEditorModule.h"
#include "Editor.h"
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
