#pragma once

#include "CoreMinimal.h"
#include "CortexGraphBindingStorage.h"

class UAnimGraphNode_Base;
class UEdGraphNode;
struct FMemberReference;

/** One entry of an anim node's property binding map. */
using FCortexAnimBindingEntry = FCortexBindingEntry;
using ECortexAnimBindingStorage = ECortexBindingStorage;

/** One of the three public function slots of a UAnimGraphNode_Base. */
struct FCortexAnimFunctionSlot
{
	/** initial_update | become_relevant | update */
	FString Name;
	/** The editor node's UPROPERTY name: InitialUpdateFunction | BecomeRelevantFunction | UpdateFunction. */
	FName PropertyName = NAME_None;
	const FMemberReference* Reference = nullptr;
};

/**
 * The typed animation view of the engine-private binding storage. The private field lookups live in one place,
 * FCortexGraphBindingStorage (CortexGraph shared infrastructure, no animation module dependency); this class delegates every
 * private read and write there and adds only what is public on the editor node: the three function-slot FMemberReferences.
 *
 * Game thread only.
 */
class FCortexAnimBindingReflection
{
public:
	static bool CheckBindingStorage(FString& OutMissingField) { return FCortexGraphBindingStorage::CheckBindingStorage(OutMissingField); }
	static bool CheckPropertyAccess(FString& OutMissingField) { return FCortexGraphBindingStorage::CheckPropertyAccess(OutMissingField); }

	/** Reads the node's binding map, sorted by property name then array index. */
	static ECortexAnimBindingStorage ReadNodeBindings(const UAnimGraphNode_Base* Node, TArray<FCortexAnimBindingEntry>& OutEntries);

	/** Copies the node's whole binding map (rollback journal). False when the storage is unsupported. */
	static bool CaptureBindingMap(const UAnimGraphNode_Base* Node, FCortexBindingMapSnapshot& OutSnapshot);
	static bool RestoreBindingMap(UAnimGraphNode_Base* Node, const FCortexBindingMapSnapshot& Snapshot);
	static bool BindingMapMatches(const UAnimGraphNode_Base* Node, const FCortexBindingMapSnapshot& Snapshot);
	static void MarkBindingModified(UAnimGraphNode_Base* Node);

	static bool IsPropertyAccessNode(const UEdGraphNode* Node) { return FCortexGraphBindingStorage::IsPropertyAccessNode(Node); }
	static bool ReadPropertyAccessNode(const UEdGraphNode* Node, TArray<FString>& OutPath, FString& OutTextPath)
	{
		return FCortexGraphBindingStorage::ReadPropertyAccess(Node, OutPath, OutTextPath);
	}
	static bool WritePropertyAccessNode(UEdGraphNode* Node, const TArray<FString>& Path, const FString& TextPath)
	{
		return FCortexGraphBindingStorage::WritePropertyAccess(Node, Path, TextPath);
	}

	/** The node's three function slots in a fixed order (initial_update, become_relevant, update). */
	static void ReadFunctionSlots(const UAnimGraphNode_Base* Node, TArray<FCortexAnimFunctionSlot>& OutSlots);

	/** Mutable access to a slot's reference by its UPROPERTY name; null for an unknown name. */
	static FMemberReference* FindMutableSlot(UAnimGraphNode_Base* Node, FName PropertyName);

#if WITH_AUTOMATION_TESTS
	static void SetFieldNameForTest(const FString& LogicalField, FName ActualName) { FCortexGraphBindingStorage::SetFieldNameForTest(LogicalField, ActualName); }
	static void ClearFieldNameOverridesForTest() { FCortexGraphBindingStorage::ClearFieldNameOverridesForTest(); }
#endif
};
