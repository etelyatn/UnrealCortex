#include "Operations/CortexBPWidgetUtils.h"

#include "Blueprint/WidgetTree.h"
#include "Engine/Blueprint.h"
#include "UObject/UnrealType.h"

TArray<UWidget*> CortexBPWidgetUtils::CollectWidgets(const UBlueprint* WidgetBlueprint)
{
	TArray<UWidget*> Widgets;
	if (!WidgetBlueprint)
	{
		return Widgets;
	}

	// UWidgetBlueprint lives in UMGEditor, which this module does not link; read the tree by reflection.
	const FObjectProperty* WidgetTreeProp = CastField<FObjectProperty>(
		WidgetBlueprint->GetClass()->FindPropertyByName(TEXT("WidgetTree")));
	const UWidgetTree* WidgetTree = WidgetTreeProp
		? Cast<UWidgetTree>(WidgetTreeProp->GetObjectPropertyValue_InContainer(WidgetBlueprint))
		: nullptr;
	if (!WidgetTree)
	{
		return Widgets;
	}

	WidgetTree->GetAllWidgets(Widgets);
	return Widgets;
}
