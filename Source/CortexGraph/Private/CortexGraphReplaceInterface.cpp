#include "CortexGraphInterfaceMigration.h"

#include "CortexGraphCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Operations/CortexGraphNodeOps.h"
#include "Operations/CortexGraphPatchState.h"

FCortexCommandResult FCortexGraphReplaceInterface::Execute(const TSharedPtr<FJsonObject>& PatchRequest)
{
	if (!PatchRequest.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("The replace_interface request must be an object"));
	}
	const TSharedPtr<FJsonObject>* Migration = nullptr;
	FString Op;
	if (!PatchRequest->TryGetObjectField(TEXT("migration"), Migration) || !Migration || !Migration->IsValid()
		|| !(*Migration)->TryGetStringField(TEXT("op"), Op) || Op != TEXT("replace_interface"))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			TEXT("The request must carry migration.op replace_interface; this entry runs no other migration"));
	}

	TSharedPtr<FJsonObject> Request = PatchRequest;
	bool bDryRun = true;
	PatchRequest->TryGetBoolField(TEXT("dry_run"), bDryRun);
	if (bDryRun && !PatchRequest->HasField(TEXT("expected_fingerprint")))
	{
		FString AssetPath;
		PatchRequest->TryGetStringField(TEXT("asset_path"), AssetPath);
		FCortexCommandResult LoadError;
		UBlueprint* const Blueprint = FCortexGraphNodeOps::LoadBlueprint(AssetPath, LoadError);
		if (Blueprint == nullptr)
		{
			return LoadError;
		}
		Request = MakeShared<FJsonObject>();
		Request->Values = PatchRequest->Values;
		Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Blueprint));
	}

	FCortexGraphCommandHandler Handler;
	return Handler.Execute(TEXT("apply_patch"), Request);
}
