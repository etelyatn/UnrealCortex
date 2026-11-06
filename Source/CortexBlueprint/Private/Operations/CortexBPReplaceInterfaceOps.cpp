#include "Operations/CortexBPReplaceInterfaceOps.h"

#include "CortexGraphInterfaceMigration.h"
#include "Operations/CortexBPCommandParams.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "IO/IoHash.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
using CortexBPParams::ReadRequiredString;

/** A strict JSON boolean: numeric and string values are refused, never coerced. */
bool ReadStrictBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, const bool bRequired, const bool bDefault, bool& OutValue, FCortexCommandResult& OutError)
{
	const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
	if (!Value.IsValid())
	{
		if (bRequired)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("%s is required and must be an explicit boolean"), Field));
			return false;
		}
		OutValue = bDefault;
		return true;
	}
	if (Value->Type != EJson::Boolean || !Value->TryGetBool(OutValue))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("%s must be a strict boolean"), Field));
		return false;
	}
	return true;
}


}

FCortexCommandResult FCortexBPReplaceInterfaceOps::Execute(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Params object is required"));
	}
	FCortexCommandResult Error;
	FString AssetPath, OldInterface, NewInterface;
	if (!ReadRequiredString(Params, TEXT("asset_path"), AssetPath, Error)
		|| !ReadRequiredString(Params, TEXT("old_interface"), OldInterface, Error)
		|| !ReadRequiredString(Params, TEXT("new_interface"), NewInterface, Error))
	{
		return Error;
	}
	const TArray<TSharedPtr<FJsonValue>>* FunctionMap = nullptr;
	if (!Params->TryGetArrayField(TEXT("function_map"), FunctionMap) || FunctionMap == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			TEXT("function_map is required: an array of {from, to?, pin_map} entries, one per implemented function of the old interface"));
	}
	bool bDryRun = false, bCompile = true, bSave = false;
	if (!ReadStrictBool(Params, TEXT("dry_run"), /*bRequired=*/true, false, bDryRun, Error)
		|| !ReadStrictBool(Params, TEXT("compile"), false, true, bCompile, Error)
		|| !ReadStrictBool(Params, TEXT("save"), false, false, bSave, Error))
	{
		return Error;
	}

	const TSharedPtr<FJsonObject>* Fingerprint = nullptr;
	if (Params->HasField(TEXT("expected_fingerprint")) && (!Params->TryGetObjectField(TEXT("expected_fingerprint"), Fingerprint) || !Fingerprint))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("expected_fingerprint must be an object"));
	}
	FString ValidationHash;
	if (Params->HasField(TEXT("expected_validation_hash")) && (!Params->TryGetStringField(TEXT("expected_validation_hash"), ValidationHash) || ValidationHash.IsEmpty()))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("expected_validation_hash must be a non-empty string"));
	}
	if (bDryRun && !ValidationHash.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("expected_validation_hash is only valid for apply"));
	}
	if (!bDryRun && (Fingerprint == nullptr || ValidationHash.IsEmpty()))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::StalePrecondition,
			TEXT("Apply requires expected_fingerprint (= fingerprint_before) and expected_validation_hash (= validation_hash) from a dry_run preview"));
	}

	FString PatchId;
	if (Params->HasField(TEXT("patch_id")))
	{
		if (!ReadRequiredString(Params, TEXT("patch_id"), PatchId, Error))
		{
			return Error;
		}
	}
	else
	{
		TSharedPtr<FJsonObject> Seed = MakeShared<FJsonObject>();
		Seed->SetStringField(TEXT("asset_path"), AssetPath);
		Seed->SetStringField(TEXT("old_interface"), OldInterface);
		Seed->SetStringField(TEXT("new_interface"), NewInterface);
		Seed->SetArrayField(TEXT("function_map"), *FunctionMap);
		PatchId = FCortexGraphReplaceInterface::DeriveRequestId(Seed);
	}

	TSharedPtr<FJsonObject> Migration = MakeShared<FJsonObject>();
	Migration->SetStringField(TEXT("op"), TEXT("replace_interface"));
	Migration->SetStringField(TEXT("old_interface"), OldInterface);
	Migration->SetStringField(TEXT("new_interface"), NewInterface);
	Migration->SetArrayField(TEXT("function_map"), *FunctionMap);

	TSharedPtr<FJsonObject> PatchRequest = MakeShared<FJsonObject>();
	PatchRequest->SetStringField(TEXT("asset_path"), AssetPath);
	PatchRequest->SetStringField(TEXT("patch_id"), PatchId);
	PatchRequest->SetBoolField(TEXT("dry_run"), bDryRun);
	PatchRequest->SetBoolField(TEXT("compile"), bCompile);
	PatchRequest->SetBoolField(TEXT("save"), bSave);
	PatchRequest->SetObjectField(TEXT("migration"), Migration);
	if (Fingerprint != nullptr)
	{
		PatchRequest->SetObjectField(TEXT("expected_fingerprint"), *Fingerprint);
	}
	if (!ValidationHash.IsEmpty())
	{
		PatchRequest->SetStringField(TEXT("expected_validation_hash"), ValidationHash);
	}
	return FCortexGraphReplaceInterface::Execute(PatchRequest);
}
