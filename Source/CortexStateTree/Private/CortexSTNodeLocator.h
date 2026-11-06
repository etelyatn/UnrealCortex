#pragma once

#include "CoreMinimal.h"
#include "CortexSTTypes.h"
#include "Misc/Guid.h"

struct FStateTreeEditorNode;
class UClass;
class UStateTreeState;

/** Where an editor node lives in the tree. */
enum class ECortexSTNodeSlot : uint8
{
	GlobalTask,
	Evaluator,
	Task,
	SingleTask,
	EnterCondition,
	Consideration,
	TransitionCondition,
};

/** A mutable editor node together with the owner the engine uses as the outer of its instance object (StateTreeState.cpp, StateTreeEditorData.cpp FixObjectNodes). */
struct FCortexSTNodeLocation
{
	ECortexSTNodeSlot Slot = ECortexSTNodeSlot::Task;
	FStateTreeEditorNode* Node = nullptr;
	/** Null for evaluators and global tasks. */
	UStateTreeState* OwnerState = nullptr;
	/** INDEX_NONE unless the node is a transition condition. */
	int32 TransitionIndex = INDEX_NONE;
	/** Index in the owning array (INDEX_NONE for SingleTask). */
	int32 NodeIndex = INDEX_NONE;
	/** State for state-owned slots, the editor data for evaluators and global tasks. */
	UObject* InstanceOuter = nullptr;

	bool IsCondition() const
	{
		return Slot == ECortexSTNodeSlot::EnterCondition || Slot == ECortexSTNodeSlot::TransitionCondition;
	}
};

namespace CortexST
{
const TCHAR* LexNodeSlot(ECortexSTNodeSlot Slot);

/** Every editor node in the tree in a stable order: evaluators, global tasks, then per state (CollectAllStates order) enter conditions, considerations, tasks, single task, transition conditions. Pointers are valid until the tree's node arrays change. */
void CollectEditorNodes(const FCortexSTAssetContext& Context, TArray<FCortexSTNodeLocation>& OutNodes);

/** Finds the node whose FStateTreeEditorNode::ID (the key of its bindings) is NodeId. */
bool FindEditorNode(const FCortexSTAssetContext& Context, const FGuid& NodeId, FCortexSTNodeLocation& Out);

/** Every Blueprint-wrapper node whose instance class is BlueprintClass, or whose class path text equals BlueprintClassPath when the class cannot be loaded any more. */
void FindEditorNodesByBlueprintClass(const FCortexSTAssetContext& Context, const UClass* BlueprintClass, const FString& BlueprintClassPath, TArray<FCortexSTNodeLocation>& OutNodes);

/** The Blueprint class a wrapper node instantiates (the node struct's instance data type is a UClass), nullptr for native struct nodes. */
const UClass* GetNodeInstanceClass(const FStateTreeEditorNode& Node);
}
