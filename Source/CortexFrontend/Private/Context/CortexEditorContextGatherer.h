#pragma once

#include "CoreMinimal.h"

struct FCortexEditorContextSnapshot
{
    FString SelectedActors;
    FString OpenAssetEditors;
    FString ContentBrowserSelection;
    FString ViewportCamera;
};

class FCortexEditorContextGatherer
{
public:
    static FCortexEditorContextSnapshot GatherAll();
    static FString FormatAsContextPreamble(const FCortexEditorContextSnapshot& Snapshot);

    static FString GatherSelectedActors();
    static FString GatherOpenAssetEditors();

private:
    static FString GatherContentBrowserSelection();
    static FString GatherViewportCamera();
};
