#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

class FCortexGraphNodeOps
{
public:
	static FCortexCommandResult ListGraphs(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult ListNodes(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetNode(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult AddNode(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult RemoveNode(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult SetPinValue(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult AutoLayout(const TSharedPtr<FJsonObject>& Params);

	// Shared helpers for reuse across CortexGraph operations
	static UBlueprint* LoadBlueprint(const FString& AssetPath, FCortexCommandResult& OutError);
	static UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName, FCortexCommandResult& OutError);
	static UEdGraphNode* FindNode(UEdGraph* Graph, const FString& NodeId, FCortexCommandResult& OutError);
	static UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName, FCortexCommandResult& OutError);
	// Serialize pin to JSON. bDetailed: if true, includes is_connected and default_value fields
	static TSharedRef<FJsonObject> SerializePin(const UEdGraphPin* Pin, bool bDetailed = true);
};
