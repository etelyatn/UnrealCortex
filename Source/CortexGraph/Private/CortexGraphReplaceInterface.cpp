#include "CortexGraphInterfaceMigration.h"

#include "CortexGraphCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "IO/IoHash.h"
#include "Operations/CortexGraphNodeOps.h"
#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphPatchState.h"

FString FCortexGraphReplaceInterface::DeriveRequestId(const TSharedPtr<FJsonObject>& CanonicalRequest)
{
	// The one canonical serializer of the patch shell: object keys sorted, array order significant.
	const FString Canonical = TEXT("cortex.graph.replace_interface|") + FCortexGraphPatchOps::CanonicalJson(CanonicalRequest);
	FTCHARToUTF8 Utf8(*Canonical);
	const FIoHash Hash = FIoHash::HashBuffer(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	uint32 Words[4] = {};
	FMemory::Memcpy(Words, Hash.GetBytes(), sizeof(Words));
	return FGuid(Words[0], Words[1], Words[2], Words[3]).ToString(EGuidFormats::DigitsWithHyphens);
}

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
		// A Blueprint that cannot be loaded keeps the request as it is: the shared apply_patch handler then reports the failure in its
		// own outcome-bearing envelope, exactly as a direct graph.apply_patch does.
		if (UBlueprint* const Blueprint = FCortexGraphNodeOps::LoadBlueprint(AssetPath, LoadError))
		{
			Request = MakeShared<FJsonObject>();
			Request->Values = PatchRequest->Values;
			Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Blueprint));
		}
	}

	FCortexGraphCommandHandler Handler;
	return Handler.Execute(TEXT("apply_patch"), Request);
}
