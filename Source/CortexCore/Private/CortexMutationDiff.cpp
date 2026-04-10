#include "CortexMutationDiff.h"

#include "CortexSerializer.h"
#include "Dom/JsonValue.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"

TSharedPtr<FJsonObject> FCortexMutationDiff::SnapshotObject(const UObject* Object, int32 MaxPropertyDepth) const
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
	int32 MaxPropertyDepth) const
{
	TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
	if (PreSnapshot.IsValid())
	{
		Diff->SetObjectField(TEXT("previous"), PreSnapshot);
	}

	if (const TSharedPtr<FJsonObject> CurrentSnapshot = SnapshotObject(Object, MaxPropertyDepth))
	{
		Diff->SetObjectField(TEXT("current"), CurrentSnapshot);
	}

	return Diff;
}

TArray<FString> FCortexMutationDiff::CollectSubObjectNames(const UObject* Outer) const
{
	TArray<FString> Result;
	if (Outer == nullptr)
	{
		return Result;
	}

	TArray<UObject*> SubObjects;
	GetObjectsWithOuter(const_cast<UObject*>(Outer), SubObjects, false);

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

FScopedMutationCapture::FScopedMutationCapture(
	const FCortexMutationDiff& InMutationDiff,
	const UObject* InObject,
	int32 InMaxPropertyDepth)
	: MutationDiff(InMutationDiff)
	, Object(const_cast<UObject*>(InObject))
	, MaxPropertyDepth(InMaxPropertyDepth)
	, PreSnapshot(InMutationDiff.SnapshotObject(InObject, InMaxPropertyDepth))
{
}

void FScopedMutationCapture::ApplyRemoved(const TSharedPtr<FJsonObject>& TargetJson) const
{
	if (!TargetJson.IsValid() || !PreSnapshot.IsValid())
	{
		return;
	}

	TargetJson->SetObjectField(TEXT("removed"), PreSnapshot);
}

void FScopedMutationCapture::ApplyDiff(const TSharedPtr<FJsonObject>& TargetJson) const
{
	if (!TargetJson.IsValid() || !PreSnapshot.IsValid() || !Object.IsValid())
	{
		return;
	}

	TargetJson->SetObjectField(
		TEXT("changes"),
		MutationDiff.CompareSnapshots(Object.Get(), PreSnapshot, MaxPropertyDepth));
}
