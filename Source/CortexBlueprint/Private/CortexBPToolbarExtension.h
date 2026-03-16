#pragma once

#include "CoreMinimal.h"

class FBlueprintEditor;

class FCortexBPToolbarExtension
{
public:
	/** Register the Cortex toolbar dropdown in the Blueprint editor toolbar. */
	static void Register();

	/** Unregister the toolbar extension. */
	static void Unregister();

private:
	/** Build the dropdown menu entries. */
	static void BuildMenu(class UToolMenu* Menu);

	/** Handle "Open Cortex Frontend" click. */
	static void OnOpenFrontendClicked();

	/** Handle "Convert BP to C++" click — captures context and broadcasts. */
	static void OnConvertBPClicked(TWeakPtr<FBlueprintEditor> WeakEditor);

	/** Build a lightweight payload from the current Blueprint editor state. */
	static struct FCortexConversionPayload CapturePayload(TSharedPtr<FBlueprintEditor> Editor);
};
