#include "CortexEditorUtils.h"
#include "CortexCoreModule.h"
#include "CortexTypes.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

void FCortexEditorUtils::NotifyAssetModified(UObject* Asset)
{
	if (Asset == nullptr)
	{
		return;
	}

	// Broadcast PostEditChange so open editors (DataTable viewer, etc.) refresh
	Asset->PostEditChange();

	UE_LOG(LogCortex, Verbose, TEXT("Notified editor of modified asset: %s"), *Asset->GetName());
}

UWorld* FCortexEditorUtils::GetPIEWorld()
{
	if (GEditor == nullptr)
	{
		return nullptr;
	}

	return GEditor->PlayWorld;
}

AActor* FCortexEditorUtils::FindActorInPIE(UWorld* PIEWorld, const FString& ActorIdentifier)
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

FCortexCommandResult FCortexEditorUtils::PIENotActiveError()
{
	FCortexCommandResult Result;
	Result.bSuccess = false;
	Result.ErrorCode = CortexErrorCodes::PIENotActive;
	Result.ErrorMessage = TEXT("Operation requires an active PIE session. Start PIE with editor.start_pie, then retry.");
	return Result;
}
