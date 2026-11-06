#pragma once

#include "CoreMinimal.h"
#include "Operations/CortexBPDependentScan.h"

class FJsonObject;
class FJsonValue;
class UBlueprint;
class UEdGraphNode;
class USCS_Node;
struct FCortexCommandResult;

/** Parameter parsing and graph/SCS queries shared by the SCS migration commands (adopt_scs_into_native, swap_scs_component_class). */
namespace CortexBPSCSSupport
{
	TArray<TSharedPtr<FJsonValue>> ToJsonStrings(const TArray<FString>& Values);

	bool ReadOptionalBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, bool Default, bool& OutValue, FCortexCommandResult& OutError);
	bool ReadRequiredString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FString& OutValue, FCortexCommandResult& OutError);

	/** An optional array of non-empty strings; absent leaves OutValues untouched and bOutPresent false. */
	bool ReadStringArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, TArray<FString>& OutValues, bool& bOutPresent, FCortexCommandResult& OutError);

	/** expected_fingerprint, when present, must be an object. */
	bool ReadExpectedFingerprint(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutExpected, FCortexCommandResult& OutError);

	/** The caller's acknowledged_losses (sorted, unique) must equal the required set exactly; otherwise POTENTIAL_DATA_LOSS with both lists. */
	bool CheckAcknowledged(const TArray<FString>& Required, const TArray<FString>& Acknowledged, FCortexCommandResult& OutError);

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
