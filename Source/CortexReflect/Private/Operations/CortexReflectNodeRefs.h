#pragma once

#include "CoreMinimal.h"

class UClass;
class UEdGraphNode;

/**
 * Class references carried by Blueprint graph nodes, shared by find_usages and export_dependency_graph so both
 * read cast, spawn and create-widget targets the same way.
 */
namespace CortexReflectNodeRefs
{
	/** Class a UK2Node_DynamicCast casts to; null for any other node or an unset target. */
	UClass* GetCastTargetClass(const UEdGraphNode* Node);

	/** Class held as the default of a UK2Node_SpawnActorFromClass class pin; null when unset or wired. */
	UClass* GetSpawnedClass(const UEdGraphNode* Node);

	/**
	 * Class held as the default of a CreateWidget node class pin. The node type lives in UMGEditor, so it is
	 * recognised by class name (K2Node_CreateWidget) on a UK2Node_ConstructObjectFromClass; null otherwise.
	 */
	UClass* GetCreatedWidgetClass(const UEdGraphNode* Node);
}
