#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"
#include "GameplayTagContainer.h"

class UStateTree;
class UStateTreeEditorData;
class UStateTreeState;
class FJsonObject;

struct FCortexSTAssetContext
{
	FString AssetPath;
	UStateTree* StateTree = nullptr;
	UStateTreeEditorData* EditorData = nullptr;
};

struct FCortexSTStateRef
{
	FString Id;
	FString Path;
	UStateTreeState* State = nullptr;
	UStateTreeState* Parent = nullptr;
	int32 Index = INDEX_NONE;
};

namespace CortexST
{
bool GetRequiredString(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FString& OutValue, FCortexCommandResult& OutError);
bool GetOptionalBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool DefaultValue);
FString NormalizeAssetPath(const FString& AssetPath);
bool ValidateReadablePackage(const FString& AssetPath, FString& OutPackageName, FCortexCommandResult& OutError);
bool ValidateWritablePackage(const FString& AssetPath, FString& OutPackageName, FCortexCommandResult& OutError);
bool ValidateGameplayTagString(const FString& TagString, FGameplayTag& OutTag, FCortexCommandResult& OutError);
bool LoadAssetContext(const FString& AssetPath, FCortexSTAssetContext& OutContext, FCortexCommandResult& OutError);
TSharedPtr<FJsonObject> MakeFingerprint(UObject* Asset);
bool CheckExpectedFingerprint(UObject* Asset, const TSharedPtr<FJsonObject>& Params, FCortexCommandResult& OutError);
TSharedPtr<FJsonObject> MakeValidationPayload(bool bValid, const TArray<FString>& Errors, const TArray<FString>& Warnings);
TSharedPtr<FJsonObject> BuildValidationPayload(UStateTree* StateTree);
void CollectStates(UStateTreeState* Root, TArray<FCortexSTStateRef>& OutStates);
bool ResolveState(const FCortexSTAssetContext& Context, const TSharedPtr<FJsonObject>& Params, FCortexSTStateRef& OutState, FCortexCommandResult& OutError);
TSharedPtr<FJsonObject> SerializeState(const FCortexSTStateRef& StateRef, bool bIncludeTransitions, bool bIncludeNodes);
}
