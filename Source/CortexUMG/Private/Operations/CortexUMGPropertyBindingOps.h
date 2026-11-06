#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class UWidgetBlueprint;
class FJsonObject;
class FProperty;
struct FDelegateEditorBinding;

class FCortexUMGPropertyBindingOps
{
  public:
	static bool AppendInspection(UWidgetBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params,
								 const FString* WidgetName, const TSharedPtr<FJsonObject>& Data,
								 FCortexCommandResult& OutError);
	static FCortexCommandResult SetPropertyBinding(const TSharedPtr<FJsonObject>& Params);

	// Private-module helpers: schema validation and verified recovery without editor notification.
	static bool ValidateSegmentFields(const FProperty* Owner, const FProperty* Name, const FProperty* Guid,
									  const FProperty* IsProperty);
	static bool RestoreBindingArray(UWidgetBlueprint* Blueprint, const TArray<FDelegateEditorBinding>& Original,
									bool bWasDirty);
};
