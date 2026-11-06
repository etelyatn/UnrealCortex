#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;

class FCortexEditorViewportOps
{
public:
	static FCortexCommandResult GetViewportInfo();
	static FCortexCommandResult CaptureScreenshot(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult SetViewportCamera(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult FocusActor(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult FocusNode(const TSharedPtr<FJsonObject>& Params);
	/**
	 * Resolves the graph and node focus_node targets: optional graph_name, graph_guid, then node_id. Pure lookup over
	 * UBlueprint::GetAllGraphs; it opens no editor.
	 */
	static bool ResolveFocusTarget(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params,
		UEdGraph*& OutGraph, UEdGraphNode*& OutNode, FCortexCommandResult& OutError);
	static FCortexCommandResult SetViewportMode(const TSharedPtr<FJsonObject>& Params);
};
