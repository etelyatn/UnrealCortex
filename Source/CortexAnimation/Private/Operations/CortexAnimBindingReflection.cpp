#include "Operations/CortexAnimBindingReflection.h"

#include "AnimGraphNode_Base.h"
#include "Engine/MemberReference.h"

ECortexAnimBindingStorage FCortexAnimBindingReflection::ReadNodeBindings(const UAnimGraphNode_Base* Node, TArray<FCortexAnimBindingEntry>& OutEntries)
{
	return FCortexGraphBindingStorage::ReadBindings(Node, OutEntries);
}

bool FCortexAnimBindingReflection::CaptureBindingMap(const UAnimGraphNode_Base* Node, FCortexBindingMapSnapshot& OutSnapshot)
{
	return FCortexGraphBindingStorage::CaptureBindingMap(Node, OutSnapshot);
}

bool FCortexAnimBindingReflection::RestoreBindingMap(UAnimGraphNode_Base* Node, const FCortexBindingMapSnapshot& Snapshot)
{
	return FCortexGraphBindingStorage::RestoreBindingMap(Node, Snapshot);
}

bool FCortexAnimBindingReflection::BindingMapMatches(const UAnimGraphNode_Base* Node, const FCortexBindingMapSnapshot& Snapshot)
{
	return FCortexGraphBindingStorage::BindingMapMatches(Node, Snapshot);
}

void FCortexAnimBindingReflection::MarkBindingModified(UAnimGraphNode_Base* Node)
{
	FCortexGraphBindingStorage::MarkBindingModified(Node);
}

void FCortexAnimBindingReflection::ReadFunctionSlots(const UAnimGraphNode_Base* Node, TArray<FCortexAnimFunctionSlot>& OutSlots)
{
	OutSlots.Reset();
	if (Node == nullptr)
	{
		return;
	}
	OutSlots.Add({ TEXT("initial_update"), TEXT("InitialUpdateFunction"), &Node->InitialUpdateFunction });
	OutSlots.Add({ TEXT("become_relevant"), TEXT("BecomeRelevantFunction"), &Node->BecomeRelevantFunction });
	OutSlots.Add({ TEXT("update"), TEXT("UpdateFunction"), &Node->UpdateFunction });
}

FMemberReference* FCortexAnimBindingReflection::FindMutableSlot(UAnimGraphNode_Base* Node, FName PropertyName)
{
	if (Node == nullptr)
	{
		return nullptr;
	}
	if (PropertyName == TEXT("InitialUpdateFunction"))
	{
		return &Node->InitialUpdateFunction;
	}
	if (PropertyName == TEXT("BecomeRelevantFunction"))
	{
		return &Node->BecomeRelevantFunction;
	}
	if (PropertyName == TEXT("UpdateFunction"))
	{
		return &Node->UpdateFunction;
	}
	return nullptr;
}
