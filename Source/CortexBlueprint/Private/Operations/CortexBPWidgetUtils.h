#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UWidget;

namespace CortexBPWidgetUtils
{
	/**
	 * Enumerate every widget in a Widget Blueprint's WidgetTree, including widgets nested in NamedSlots.
	 * Uses UWidgetTree::GetAllWidgets, which walks the tree itself: the serialized AllWidgets array is
	 * emptied by UWidgetTree::PostLoad and only refilled by PreSave, so it is empty after a cold load.
	 * Returns an empty array for a null Blueprint or one without a WidgetTree.
	 */
	TArray<UWidget*> CollectWidgets(const UBlueprint* WidgetBlueprint);
}
