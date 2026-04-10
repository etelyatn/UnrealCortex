#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UObject;

class CORTEXCORE_API FCortexMutationDiff
{
public:
	TSharedPtr<FJsonObject> SnapshotObject(const UObject* Object, int32 MaxPropertyDepth = 1) const;
	TSharedPtr<FJsonObject> CompareSnapshots(const UObject* Object, const TSharedPtr<FJsonObject>& PreSnapshot) const;
	TArray<FString> CollectSubObjectNames(const UObject* Outer) const;
};

class CORTEXCORE_API FScopedMutationCapture
{
public:
	FScopedMutationCapture(const FCortexMutationDiff& InMutationDiff, const UObject* InObject, int32 InMaxPropertyDepth = 1);

	void ApplyRemoved(const TSharedPtr<FJsonObject>& TargetJson) const;
	void ApplyDiff(const TSharedPtr<FJsonObject>& TargetJson) const;

private:
	const FCortexMutationDiff& MutationDiff;
	TWeakObjectPtr<UObject> Object;
	TSharedPtr<FJsonObject> PreSnapshot;
};
