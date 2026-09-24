#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FJsonObject;

struct FCortexBPRemoveGraphPrepared
{
	FString AssetPath;
	FString Name;
	bool bCascadeExecChain = false;
	bool bDryRun = true;
	bool bCompile = false;
	bool bSave = false;
	TSharedPtr<FJsonObject> FingerprintBefore;
	TSharedPtr<FJsonObject> Target;
	TSharedPtr<FJsonObject> Deletion;
	FString ValidationHash;
};

struct FCortexBPRemoveGraphOutcome
{
	bool bChanged = false;
	FString ApplyStatus = TEXT("not_requested");
	FString CompileStatus = TEXT("not_requested");
	FString ReadbackStatus = TEXT("not_requested");
	FString RollbackStatus = TEXT("not_requested");
	FString SaveStatus = TEXT("not_requested");
	FString PostSaveStatus = TEXT("not_requested");
	bool bSaved = false;
	bool bBlocked = false;
	bool bDirtyBefore = false;
	bool bDirtyAfter = false;
	TSharedPtr<FJsonObject> FingerprintBefore;
	TSharedPtr<FJsonObject> FingerprintAfter;
	TSharedPtr<FJsonObject> Target;
	TSharedPtr<FJsonObject> Deletion;
	TArray<FString> Diagnostics;
};

class FCortexBPRemoveGraphOps
{
public:
	static FCortexCommandResult Execute(const TSharedPtr<FJsonObject>& Params);

#if WITH_AUTOMATION_TESTS
	static void SetFaultPointForTesting(FName Point);
	static void ClearFaultPointForTesting();
#endif
};
