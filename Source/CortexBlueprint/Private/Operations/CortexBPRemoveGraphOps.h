#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FJsonObject;
class FJsonValue;

/** One removal target resolved from a requested name: a whole graph or a custom-event node set. */
struct FCortexBPRemoveGraphItem
{
	FString Name;
	TSharedPtr<FJsonObject> Target;
	TSharedPtr<FJsonObject> Deletion;
	TSharedPtr<FJsonObject> Removed;
	TArray<TSharedPtr<FJsonValue>> RemovedNodes;
};

struct FCortexBPRemoveGraphPrepared
{
	FString AssetPath;
	/** One entry for the single `name` form, the requested set for the `names` form. */
	TArray<FString> Names;
	/** True when the request used `names[]`; selects the set-shaped response. */
	bool bBatch = false;
	bool bCascadeExecChain = false;
	bool bDryRun = true;
	bool bCompile = false;
	bool bSave = false;
	TSharedPtr<FJsonObject> FingerprintBefore;
	TArray<FCortexBPRemoveGraphItem> Items;
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
	bool bBatch = false;
	TSharedPtr<FJsonObject> Target;
	TSharedPtr<FJsonObject> Deletion;
	TArray<TSharedPtr<FJsonValue>> Targets;
	TArray<TSharedPtr<FJsonValue>> Deletions;
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
