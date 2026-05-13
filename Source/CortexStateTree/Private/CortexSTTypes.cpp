#include "CortexSTTypes.h"

#include "CortexAssetFingerprint.h"
#include "CortexBatchMutation.h"
#include "CortexCommandRouter.h"
#include "CortexEditorUtils.h"
#include "GameplayTagsManager.h"
#include "Misc/PackageName.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"

namespace CortexST
{
bool GetRequiredString(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FString& OutValue, FCortexCommandResult& OutError)
{
	if (!Params.IsValid() || !Params->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Missing required param: %s"), FieldName));
		return false;
	}

	return true;
}

bool GetOptionalBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool DefaultValue)
{
	bool bValue = DefaultValue;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(FieldName, bValue);
	}

	return bValue;
}

FString NormalizeAssetPath(const FString& AssetPath)
{
	return FPackageName::ExportTextPathToObjectPath(
		FCortexEditorUtils::NormalizeMountedContentPath(AssetPath));
}

bool ValidateReadablePackage(const FString& AssetPath, FString& OutPackageName, FCortexCommandResult& OutError)
{
	const FString Normalized = NormalizeAssetPath(AssetPath);
	OutPackageName = FPackageName::ObjectPathToPackageName(Normalized);
	if (OutPackageName.IsEmpty() || (!FindPackage(nullptr, *OutPackageName) && !FPackageName::DoesPackageExist(OutPackageName)))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::StateTreeNotFound,
			FString::Printf(TEXT("StateTree not found: %s"), *AssetPath));
		return false;
	}

	return true;
}

bool ValidateWritablePackage(const FString& AssetPath, FString& OutPackageName, FCortexCommandResult& OutError)
{
	const FString Normalized = NormalizeAssetPath(AssetPath);
	OutPackageName = FPackageName::ObjectPathToPackageName(Normalized);
	if (!FPackageName::IsValidLongPackageName(OutPackageName))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Invalid StateTree package path: %s"), *AssetPath));
		return false;
	}

	FString WritableError;
	if (!FCortexEditorUtils::IsWritableMountedContentPath(OutPackageName, WritableError))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			WritableError);
		return false;
	}

	FString Filename;
	if (!FPackageName::TryConvertLongPackageNameToFilename(
		OutPackageName,
		Filename,
		FPackageName::GetAssetPackageExtension()))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("StateTree package path is not under a writable mounted root: %s"), *AssetPath));
		return false;
	}

	return true;
}

bool ValidateGameplayTagString(const FString& TagString, FGameplayTag& OutTag, FCortexCommandResult& OutError)
{
	if (TagString.IsEmpty())
	{
		OutTag = FGameplayTag();
		return true;
	}

	OutTag = FGameplayTag::RequestGameplayTag(FName(*TagString), false);
	if (!OutTag.IsValid())
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidTag,
			FString::Printf(TEXT("Gameplay Tag is not registered: %s"), *TagString));
		return false;
	}

	return true;
}

bool LoadAssetContext(const FString& AssetPath, FCortexSTAssetContext& OutContext, FCortexCommandResult& OutError)
{
	FString PackageName;
	if (!ValidateReadablePackage(AssetPath, PackageName, OutError))
	{
		return false;
	}

	const FString ObjectPath = NormalizeAssetPath(AssetPath);
	UStateTree* StateTree = LoadObject<UStateTree>(nullptr, *ObjectPath);
	if (!StateTree)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::StateTreeNotFound,
			FString::Printf(TEXT("Asset is not a StateTree: %s"), *AssetPath));
		return false;
	}

	UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(StateTree->EditorData);
	if (!EditorData)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("StateTree has no editor data: %s"), *AssetPath));
		return false;
	}

	OutContext.AssetPath = ObjectPath;
	OutContext.StateTree = StateTree;
	OutContext.EditorData = EditorData;
	return true;
}

TSharedPtr<FJsonObject> MakeFingerprint(UObject* Asset)
{
	return MakeObjectAssetFingerprint(Asset).ToJson();
}

bool CheckExpectedFingerprint(UObject* Asset, const TSharedPtr<FJsonObject>& Params, FCortexCommandResult& OutError)
{
	if (!Params.IsValid() || !Params->HasTypedField<EJson::Object>(TEXT("expected_fingerprint")))
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::StalePrecondition,
			TEXT("Mutating StateTree command requires expected_fingerprint"));
		return false;
	}

	const TSharedPtr<FJsonObject>* Expected = nullptr;
	Params->TryGetObjectField(TEXT("expected_fingerprint"), Expected);

	const TSharedPtr<FJsonObject> Current = MakeFingerprint(Asset);
	if (!Expected || !(*Expected).IsValid() || !FCortexBatchMutation::FingerprintsMatch(Current, *Expected))
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetObjectField(TEXT("current_fingerprint"), Current);
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::StalePrecondition,
			TEXT("Expected fingerprint does not match current StateTree fingerprint"),
			Details);
		return false;
	}

	return true;
}

TSharedPtr<FJsonObject> MakeValidationPayload(bool bValid, const TArray<FString>& Errors, const TArray<FString>& Warnings)
{
	TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
	Validation->SetBoolField(TEXT("valid"), bValid);

	TArray<TSharedPtr<FJsonValue>> ErrorValues;
	for (const FString& Error : Errors)
	{
		ErrorValues.Add(MakeShared<FJsonValueString>(Error));
	}
	Validation->SetArrayField(TEXT("errors"), ErrorValues);

	TArray<TSharedPtr<FJsonValue>> WarningValues;
	for (const FString& Warning : Warnings)
	{
		WarningValues.Add(MakeShared<FJsonValueString>(Warning));
	}
	Validation->SetArrayField(TEXT("warnings"), WarningValues);

	return Validation;
}
}
