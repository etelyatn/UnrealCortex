#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UObject;

class CORTEXCORE_API FCortexMutationDiff
{
public:
	static TSharedPtr<FJsonObject> SnapshotObject(const UObject* Object, int32 MaxPropertyDepth = 1);
	static TSharedPtr<FJsonObject> CompareSnapshots(
		const UObject* Object,
		const TSharedPtr<FJsonObject>& PreSnapshot,
		int32 MaxPropertyDepth = 1);

private:
	static TArray<FString> CollectSubObjectNames(const UObject* Outer);
};

class CORTEXCORE_API FScopedMutationCapture
{
public:
	FScopedMutationCapture(UObject* InObject, int32 InMaxPropertyDepth = 1);
	~FScopedMutationCapture();

	void ApplyRemoved(const TSharedPtr<FJsonObject>& TargetJson);
	void ApplyDiff(const TSharedPtr<FJsonObject>& TargetJson);

private:
	TWeakObjectPtr<UObject> Object;
	int32 MaxPropertyDepth;
	TSharedPtr<FJsonObject> PreSnapshot;
	bool bApplied = false;
};
