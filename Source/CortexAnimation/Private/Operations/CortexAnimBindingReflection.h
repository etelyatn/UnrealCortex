#pragma once

#include "CoreMinimal.h"

class FMapProperty;
class UAnimGraphNode_Base;
class UEdGraphNode;
class UObject;
struct FAnimGraphNodePropertyBinding;
struct FMemberReference;

/** One entry of an anim node's property binding map (a copy of FAnimGraphNodePropertyBinding's reportable fields). */
struct FCortexAnimBindingEntry
{
	/** The bound pin property; also the binding map key. */
	FName PropertyName = NAME_None;
	int32 ArrayIndex = INDEX_NONE;
	/** True for a function-typed binding (EAnimGraphNodePropertyBindingType::Function). */
	bool bFunction = false;
	bool bIsBound = false;
	bool bIsPromotion = false;
	bool bOnlyUpdateWhenActive = false;
	TArray<FString> Path;
	FString PathText;
};

/** Where a node keeps its pin bindings. */
enum class ECortexAnimBindingStorage : uint8
{
	/** The node's Binding object is the default binding class and exposes the reflected PropertyBindings map. */
	Supported,
	/** The Binding object is missing or not the default binding class; its storage is not read. */
	Unsupported,
};

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
 * The only reader and writer of engine-private anim structures: the default binding class's PropertyBindings map
 * (UAnimGraphNodeBinding_Base is private to AnimGraph) and UK2Node_PropertyAccess::Path/TextPath (private header of the
 * PropertyAccessNode plugin). Every FProperty is resolved by name once; a missing reflected field is reported by name so the caller
 * can fail with UNSUPPORTED_OPERATION. The three function-slot FMemberReferences, GetBinding() and FAnimGraphNodePropertyBinding are
 * public and are read directly.
 *
 * Game thread only.
 */
class FCortexAnimBindingReflection
{
public:
	/** Verifies the fields needed to read node bindings. False names the missing field in OutMissingField. */
	static bool CheckBindingStorage(FString& OutMissingField);

	/** Verifies the fields needed to read property-access nodes (true when the PropertyAccessNode class is not loaded). */
	static bool CheckPropertyAccess(FString& OutMissingField);

	/** Reads the node's binding map, sorted by property name then array index. Requires CheckBindingStorage. */
	static ECortexAnimBindingStorage ReadNodeBindings(const UAnimGraphNode_Base* Node, TArray<FCortexAnimBindingEntry>& OutEntries);

	/**
	 * Copies the node's whole binding map (rollback journal). False when the storage is unsupported.
	 * Requires CheckBindingStorage.
	 */
	static bool CopyBindingMap(const UAnimGraphNode_Base* Node, TMap<FName, FAnimGraphNodePropertyBinding>& OutMap);

	/** Replaces the node's whole binding map with Map (rollback restore; the node's Binding object is marked modified). */
	static bool RestoreBindingMap(UAnimGraphNode_Base* Node, const TMap<FName, FAnimGraphNodePropertyBinding>& Map);

	static bool IsPropertyAccessNode(const UEdGraphNode* Node);

	/** Reads a UK2Node_PropertyAccess's path and text path. Requires CheckPropertyAccess. */
	static bool ReadPropertyAccessNode(const UEdGraphNode* Node, TArray<FString>& OutPath, FString& OutTextPath);

	/** Writes a UK2Node_PropertyAccess's path and text path (rollback restore; the node is marked modified). */
	static bool WritePropertyAccessNode(UEdGraphNode* Node, const TArray<FString>& Path, const FString& TextPath);

	/** The node's three function slots in a fixed order (initial_update, become_relevant, update). */
	static void ReadFunctionSlots(const UAnimGraphNode_Base* Node, TArray<FCortexAnimFunctionSlot>& OutSlots);

	/** Mutable access to a slot's reference by its UPROPERTY name; null for an unknown name. */
	static FMemberReference* FindMutableSlot(UAnimGraphNode_Base* Node, FName PropertyName);

#if WITH_AUTOMATION_TESTS
	/**
	 * Test hook on the reflected-field name table: LogicalField is one of `AnimGraphNode_Base.Binding`,
	 * `AnimGraphNodeBinding_Base.PropertyBindings`, `K2Node_PropertyAccess.Path`, `K2Node_PropertyAccess.TextPath`; the field is then
	 * looked up as ActualName, which simulates an engine that renamed or removed it.
	 */
	static void SetFieldNameForTest(const FString& LogicalField, FName ActualName);
	static void ClearFieldNameOverridesForTest();
#endif
};
