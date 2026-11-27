#pragma once

#include "CoreMinimal.h"

class FJsonValue;
class UBlueprint;
class UEdGraphNode;

/**
 * Pre-flight facts for override events (K2Node_Event with bOverrideFunction) that graph.remove_nodes is about to remove.
 * Informational only: it never blocks the removal and never mutates or loads anything.
 */
class FCortexGraphOverrideEvents
{
public:
	/**
	 * One `override_events` entry per override event in Removed: function (resolved on a copy of the event reference, the
	 * live node is never rewritten), declaration_resolved, declared_in, native_declaration, has_native_default,
	 * blueprint_callers (call nodes of this and other loaded Blueprints that resolve to the function, nodes in Removed
	 * excluded; empty for an unresolved declaration), blueprint_callers_complete, unloaded_referencer_count,
	 * unloaded_referencers and native_callers ("not_enumerable"). Empty when Removed holds no override event.
	 */
	static TArray<TSharedPtr<FJsonValue>> Describe(UBlueprint* Blueprint, const TArray<UEdGraphNode*>& Removed);
};
