#pragma once

#include "CoreMinimal.h"
#include "Operations/CortexBPDependentScan.h"

class UBlueprint;
class UEdGraphNode;
class USCS_Node;

/** Graph and SCS queries shared by the SCS migration commands (adopt_scs_into_native, swap_scs_component_class). Parameter parsing lives in CortexBPParams. */
namespace CortexBPSCSSupport
{
	UEdGraphNode* FindNodeByGuid(const UBlueprint* Blueprint, const FGuid& Guid);
	TSet<FGuid> AllNodeGuids(const UBlueprint* Blueprint);
	bool IsUpToDate(const UBlueprint* Blueprint);

	/** The Blueprint's own component-bound events that name the component variable. */
	void CollectBoundEvents(UBlueprint* Blueprint, FName Variable, TArray<FGuid>& OutEvents);

	/** Bound-event node GUID to the component property it names. */
	TMap<FGuid, FName> CollectBoundEventNames(UBlueprint* Blueprint);

	/** Loaded child Blueprints (from a dependent scan) that hold an InheritableComponentHandler override keyed by the node. Sorted. */
	void FindChildOverrides(const USCS_Node* Node, const TArray<FCortexBPDependentHit>& Hits, TArray<FString>& OutPaths);

	/**
	 * Gives the template of the SCS node back to the generated class. The compile moves the template of a removed node out of the class and
	 * an undo brings the node back without the template's owner, so the recovery compile would recreate the template with a warning.
	 */
	void RestoreTemplateOwnership(UBlueprint* Blueprint, const FString& NodeName);
}
