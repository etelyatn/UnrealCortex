#pragma once

#include "CoreMinimal.h"

class FMapProperty;
class UEdGraphNode;
class UK2Node;

/** One entry of an anim node's property binding map, decomposed from the engine's FAnimGraphNodePropertyBinding by reflection. */
struct CORTEXGRAPH_API FCortexBindingEntry
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
enum class ECortexBindingStorage : uint8
{
	/** The node's Binding object is the default binding class and exposes the reflected PropertyBindings map. */
	Supported,
	/** The Binding object is missing or not the default binding class; its storage is not read. */
	Unsupported,
};

/** An exact, opaque copy of a node's whole binding map: every field of every entry (paths, display text, flags, pin types). */
class CORTEXGRAPH_API FCortexBindingMapSnapshot
{
public:
	FCortexBindingMapSnapshot() = default;
	FCortexBindingMapSnapshot(const FCortexBindingMapSnapshot&) = delete;
	FCortexBindingMapSnapshot& operator=(const FCortexBindingMapSnapshot&) = delete;
	FCortexBindingMapSnapshot(FCortexBindingMapSnapshot&& Other) noexcept;
	FCortexBindingMapSnapshot& operator=(FCortexBindingMapSnapshot&& Other) noexcept;
	~FCortexBindingMapSnapshot();

	bool IsValid() const { return Memory != nullptr; }

private:
	friend class FCortexGraphBindingStorage;
	void Reset();

	const FMapProperty* Property = nullptr;
	uint8* Memory = nullptr;
};

/**
 * The single owner of engine-private anim binding storage: the default binding class's PropertyBindings map
 * (UAnimGraphNodeBinding_Base is private to AnimGraph) and UK2Node_PropertyAccess::Path/TextPath (private header of the
 * PropertyAccessNode plugin). Everything is reached by reflection, so CortexGraph links no animation module; CortexAnimation's typed
 * CortexAnimBindingReflection and the generic member-reference query both delegate here. Every FProperty is resolved by name once and a
 * missing field is reported by name so a caller can fail with UNSUPPORTED_OPERATION.
 *
 * Game thread only.
 */
class CORTEXGRAPH_API FCortexGraphBindingStorage
{
public:
	/** Verifies the fields needed to read node bindings. False names the missing field in OutMissingField. */
	static bool CheckBindingStorage(FString& OutMissingField);

	/** Verifies the fields needed to read property-access nodes (true when the PropertyAccessNode class is not loaded). */
	static bool CheckPropertyAccess(FString& OutMissingField);

	/** Reads the node's binding map, sorted by property name then array index. */
	static ECortexBindingStorage ReadBindings(const UK2Node* Node, TArray<FCortexBindingEntry>& OutEntries);

	/** Copies the node's whole binding map (rollback journal). False when the storage is unsupported. */
	static bool CaptureBindingMap(const UK2Node* Node, FCortexBindingMapSnapshot& OutSnapshot);

	/** Replaces the node's whole binding map with the snapshot. False when the storage is unsupported. */
	static bool RestoreBindingMap(UK2Node* Node, const FCortexBindingMapSnapshot& Snapshot);

	/**
	 * Whether the node's binding map has exactly the content of the snapshot: the same entries (by key) and, per entry, the same exported
	 * text of every authoring field (paths, display text, flags, pin types, contexts); it does not depend on FText identity. The compiler's
	 * feedback fields of an entry (CompiledContext, CompiledContextDesc: the anim compiler writes them back onto the original node's
	 * bindings on every compile, AnimBlueprintExtension_Base.cpp:205-240) are not authoring state and are not compared.
	 */
	static bool BindingMapMatches(const UK2Node* Node, const FCortexBindingMapSnapshot& Snapshot);

	/** The node's whole binding map as the engine's exported text (journal text); false when the storage is unsupported. */
	static bool ExportBindingText(const UK2Node* Node, FString& OutText);

	/** Replaces the node's whole binding map from ExportBindingText output (the Binding sub-object is recorded in the transaction first). */
	static bool ImportBindingText(UK2Node* Node, const FString& Text);

	/** Whether the node is an animation graph node (class UAnimGraphNode_Base, found by path so no animation module is linked). */
	static bool IsAnimNode(const UEdGraphNode* Node);

	/** An independent copy of a snapshot (the basis of an expected state: copy the journal, apply the planned edits, compare). */
	static bool CloneBindingMap(const FCortexBindingMapSnapshot& Source, FCortexBindingMapSnapshot& OutClone);

	/** Rewrites one entry of a snapshot to a new path and display text; every other field of the entry is kept. False when the entry is absent. */
	static bool RewriteSnapshotEntry(FCortexBindingMapSnapshot& Snapshot, FName PropertyName, int32 ArrayIndex, const TArray<FString>& NewPath, const FText& NewPathText);

	/**
	 * Records the node's Binding sub-object in the active editor transaction. The sub-object is created without
	 * RF_Transactional, so a plain Modify() records nothing and an undo would leave the map as it was.
	 */
	static void MarkBindingModified(UK2Node* Node);

	/** The binding-map key whose path names Member as a segment; empty when none does or the storage is unsupported. */
	static FString FindBindingKeyNaming(const UK2Node* Node, FName Member);

	/** The function slot (UPROPERTY name) whose FMemberReference names Member; empty when none does. */
	static FString FindFunctionSlotNaming(const UK2Node* Node, FName Member);

	static bool IsPropertyAccessNode(const UEdGraphNode* Node);

	/** Reads a UK2Node_PropertyAccess's path and text path. Requires CheckPropertyAccess. */
	static bool ReadPropertyAccess(const UEdGraphNode* Node, TArray<FString>& OutPath, FString& OutTextPath);

	/** Writes a UK2Node_PropertyAccess's path and text path (the node is marked modified). */
	static bool WritePropertyAccess(UEdGraphNode* Node, const TArray<FString>& Path, const FString& TextPath);

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
