#pragma once

#include "CoreMinimal.h"
#include "CortexConversionTypes.h"

class FCortexBPSerializationOps
{
public:
	/**
	 * Serialize a Blueprint to compact JSON based on the requested scope.
	 * Executes the callback synchronously with the result.
	 */
	static void Serialize(const FCortexSerializationRequest& Request, FOnSerializationComplete Callback);

private:
	/** Serialize the entire Blueprint. */
	static FString SerializeEntireBlueprint(class UBlueprint* Blueprint);

	/** Serialize only selected nodes. */
	static FString SerializeSelectedNodes(class UBlueprint* Blueprint, const TArray<FString>& NodeIds);

	/** Serialize a single graph. */
	static FString SerializeGraph(class UBlueprint* Blueprint, const FString& GraphName);

	/** Serialize a single event or function. */
	static FString SerializeEventOrFunction(class UBlueprint* Blueprint, const FString& TargetName);

	/** Serialize multiple events/functions into a combined JSON array. */
	static FString SerializeMultipleEventOrFunction(class UBlueprint* Blueprint, const TArray<FString>& TargetNames);

	/** Helper: serialize a UEdGraph to JSON. */
	static TSharedRef<FJsonObject> GraphToJson(class UEdGraph* Graph);

	/** Helper: serialize a UEdGraphNode to JSON. */
	static TSharedRef<FJsonObject> NodeToJson(class UEdGraphNode* Node);

	// ── Compact variants (bConversionMode=true) ──
	// Strip x/y positions, comments, full type paths; use sequential int IDs.

	static FString SerializeEntireBlueprintCompact(class UBlueprint* Blueprint);
	static FString SerializeSelectedNodesCompact(class UBlueprint* Blueprint, const TArray<FString>& NodeIds);
	static FString SerializeGraphCompact(class UBlueprint* Blueprint, const FString& GraphName);
	static FString SerializeEventOrFunctionCompact(class UBlueprint* Blueprint, const FString& TargetName);
	static FString SerializeMultipleEventOrFunctionCompact(class UBlueprint* Blueprint, const TArray<FString>& TargetNames);

	static TSharedRef<FJsonObject> GraphToJsonCompact(class UEdGraph* Graph);
	static TSharedRef<FJsonObject> NodeToJsonCompact(class UEdGraphNode* Node, const TMap<class UEdGraphNode*, int32>& IndexMap);

	/** Helper: serialize Blueprint variables to JSON array. */
	static TArray<TSharedPtr<FJsonValue>> VariablesToJson(class UBlueprint* Blueprint);

	/** Helper: serialize Blueprint components to JSON array. */
	static TArray<TSharedPtr<FJsonValue>> ComponentsToJson(class UBlueprint* Blueprint);

	/** Helper: load Blueprint by path with proper guards. */
	static class UBlueprint* LoadBlueprintSafe(const FString& AssetPath, FString& OutError);
};
