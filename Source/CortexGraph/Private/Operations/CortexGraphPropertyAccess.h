#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;

/**
 * Property Access nodes (UK2Node_PropertyAccess, PropertyAccessNode plugin). The class is MinimalAPI with a private
 * header, so it is reached by path and its private `Path` / `TextPath` / `ContextId` properties by reflection; path
 * resolution and thread safety come from the public IPropertyAccessEditor modular feature, as the node computes them.
 */
class FCortexGraphPropertyAccess
{
public:
	/** The node class, or null when the PropertyAccessNode plugin is not loaded. */
	static UClass* GetNodeClass();
	static bool IsPropertyAccessNode(const UEdGraphNode* Node);

	/** Reads `params.path`: a non-empty array of segments `Name` or `Name[N]` (N a non-negative integer). */
	static bool ParsePathParam(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutPath, FString& OutError);

	/** Refuses a Blueprint that cannot hold the node (not a compiled Anim Blueprint, or a child Anim Blueprint). */
	static bool ValidateBlueprint(const UBlueprint* Blueprint, FCortexCommandResult& OutError);

	/** Refuses macro graphs: the Anim Blueprint compiler creates no cache variable for nodes inlined from them. */
	static bool ValidateGraph(const UBlueprint* Blueprint, const UEdGraph* Graph, FCortexCommandResult& OutError);

	/**
	 * Resolves the path from the Blueprint's skeleton class as the compiler does: names the failing segment, refuses
	 * indexed getters, indices on non-array properties, members and getters denied by the editor permission lists, and
	 * leaves without a concrete pin type.
	 */
	static bool ValidatePath(const UBlueprint* Blueprint, const TArray<FString>& Path, FCortexCommandResult& OutError);

	/** Writes the node's `Path`; pins allocated afterwards resolve it, set `TextPath` and type the `Value` pin. */
	static bool WritePath(UEdGraphNode* Node, const TArray<FString>& Path);
	static bool ReadPath(const UEdGraphNode* Node, TArray<FString>& OutPath);
	/** The node's call-site context; NAME_None is Automatic, which is what Cortex creates. */
	static FName ReadContextId(const UEdGraphNode* Node);

	/** `{path, text_path, resolved, thread_safe}` for a Property Access node; null for any other node. */
	static TSharedPtr<FJsonObject> Describe(const UEdGraphNode* Node);
};
