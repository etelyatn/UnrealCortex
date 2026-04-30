#include "CortexMutationDiff.h"

#include "CortexCoreModule.h"
#include "CortexSerializer.h"
#include "Dom/JsonValue.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"

TSharedPtr<FJsonObject> FCortexMutationDiff::SnapshotObject(const UObject* Object, int32 MaxPropertyDepth)
{
	check(IsInGameThread());

	if (Object == nullptr)
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> Snapshot = MakeShared<FJsonObject>();
	Snapshot->SetStringField(TEXT("class"), Object->GetClass()->GetName());
	Snapshot->SetObjectField(
		TEXT("non_default_properties"),
		FCortexSerializer::NonDefaultPropertiesToJson(Object, MaxPropertyDepth));

	const TArray<FString> SubObjectNames = CollectSubObjectNames(Object);
	TArray<TSharedPtr<FJsonValue>> JsonSubObjectNames;
	JsonSubObjectNames.Reserve(SubObjectNames.Num());
	for (const FString& SubObjectName : SubObjectNames)
	{
		JsonSubObjectNames.Add(MakeShared<FJsonValueString>(SubObjectName));
	}

	Snapshot->SetNumberField(TEXT("sub_object_count"), SubObjectNames.Num());
	Snapshot->SetArrayField(TEXT("sub_object_names"), JsonSubObjectNames);
	return Snapshot;
}

TSharedPtr<FJsonObject> FCortexMutationDiff::CompareSnapshots(
	const UObject* Object,
	const TSharedPtr<FJsonObject>& PreSnapshot,
	int32 MaxPropertyDepth)
{
	check(IsInGameThread());

	TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();

	if (PreSnapshot.IsValid())
	{
		Diff->SetObjectField(TEXT("previous"), PreSnapshot);
	}
	else
	{
		Diff->SetField(TEXT("previous"), MakeShared<FJsonValueNull>());
	}

	if (const TSharedPtr<FJsonObject> CurrentSnapshot = SnapshotObject(Object, MaxPropertyDepth))
	{
		Diff->SetObjectField(TEXT("current"), CurrentSnapshot);
	}
	else
	{
		Diff->SetField(TEXT("current"), MakeShared<FJsonValueNull>());
	}

	return Diff;
}

TArray<FString> FCortexMutationDiff::CollectSubObjectNames(const UObject* Outer)
{
	TArray<FString> Result;
	if (Outer == nullptr)
	{
		return Result;
	}

	TArray<UObject*> SubObjects;
	// GetObjectsWithOuter requires non-const but only reads
	GetObjectsWithOuter(const_cast<UObject*>(Outer), SubObjects, true);

	for (const UObject* SubObject : SubObjects)
	{
		if (SubObject == nullptr || SubObject->HasAnyFlags(RF_Transient))
		{
			continue;
		}

		Result.Add(SubObject->GetName());
	}

	Result.Sort();
	return Result;
}

FScopedMutationCapture::FScopedMutationCapture(UObject* InObject, int32 InMaxPropertyDepth)
	: Object(InObject)
	, MaxPropertyDepth(InMaxPropertyDepth)
	, PreSnapshot(FCortexMutationDiff::SnapshotObject(InObject, InMaxPropertyDepth))
{
}

FScopedMutationCapture::~FScopedMutationCapture()
{
	checkf(bApplied, TEXT("FScopedMutationCapture destroyed without calling ApplyRemoved or ApplyDiff"));
}

void FScopedMutationCapture::ApplyRemoved(const TSharedPtr<FJsonObject>& TargetJson)
{
	bApplied = true;

	if (!TargetJson.IsValid())
	{
		return;
	}

	if (!PreSnapshot.IsValid())
	{
		UE_LOG(LogCortex, Log, TEXT("ApplyRemoved: PreSnapshot is invalid, skipping"));
		return;
	}

	TargetJson->SetObjectField(TEXT("removed"), PreSnapshot);
}

void FScopedMutationCapture::ApplyDiff(const TSharedPtr<FJsonObject>& TargetJson)
{
	bApplied = true;

	if (!TargetJson.IsValid())
	{
		return;
	}

	if (!PreSnapshot.IsValid())
	{
		UE_LOG(LogCortex, Log, TEXT("ApplyDiff: PreSnapshot is invalid, skipping diff"));
		return;
	}

	if (!Object.IsValid())
	{
		UE_LOG(LogCortex, Log, TEXT("ApplyDiff: Object was garbage collected before diff could be computed"));
		TargetJson->SetStringField(TEXT("changes_error"), TEXT("object_destroyed_before_diff"));
		return;
	}

	TargetJson->SetObjectField(
		TEXT("changes"),
		FCortexMutationDiff::CompareSnapshots(Object.Get(), PreSnapshot, MaxPropertyDepth));
}
