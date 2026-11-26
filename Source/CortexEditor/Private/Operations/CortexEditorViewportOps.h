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
	/**
	 * editor.capture_editor_panel: renders an Editor panel to a PNG through Slate (FSlateApplication::TakeScreenshot),
	 * reading the window's own back buffer, so no OS screen-capture permission is needed. asset_path opens that
	 * asset's editor; panel is editor (asset editor tab content), window, details (SDetailsView) or graph (SGraphEditor).
	 */
	static FCortexCommandResult CaptureEditorPanel(const TSharedPtr<FJsonObject>& Params);
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
