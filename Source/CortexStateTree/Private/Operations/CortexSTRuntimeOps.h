#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class FJsonObject;
class UStateTree;
class UWorld;

/**
 * statetree.get_runtime_state: what the StateTrees in the active PIE world are doing right now (read-only).
 *
 * Params: asset_path? (only components using that StateTree asset), actor? (only that actor's components).
 * Requires GEditor->PlayWorld, else PIE_NOT_ACTIVE. The component's StateTreeRef is read by reflection because the
 * property is protected; active_states needs WITH_GAMEPLAY_DEBUGGER and is otherwise reported as unavailable.
 */
class FCortexSTRuntimeOps
{
public:
	static FCortexCommandResult GetRuntimeState(const TSharedPtr<FJsonObject>& Params);

	/** Every UStateTreeComponent in World whose StateTreeRef asset is Filter (null = any) and whose actor name is ActorFilter (empty = any). */
	static TSharedPtr<FJsonObject> DescribeComponents(UWorld* World, const UStateTree* Filter, const FString& ActorFilter);
};
