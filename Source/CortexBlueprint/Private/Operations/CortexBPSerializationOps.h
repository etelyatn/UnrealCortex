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

	/** Helper: serialize a UEdGraph to JSON. */
	static TSharedRef<FJsonObject> GraphToJson(class UEdGraph* Graph);

	/** Helper: serialize a UEdGraphNode to JSON. */
	static TSharedRef<FJsonObject> NodeToJson(class UEdGraphNode* Node);

	/** Helper: serialize Blueprint variables to JSON array. */
	static TArray<TSharedPtr<FJsonValue>> VariablesToJson(class UBlueprint* Blueprint);

	/** Helper: serialize Blueprint components to JSON array. */
	static TArray<TSharedPtr<FJsonValue>> ComponentsToJson(class UBlueprint* Blueprint);

	/** Helper: load Blueprint by path with proper guards. */
	static class UBlueprint* LoadBlueprintSafe(const FString& AssetPath, FString& OutError);
};
