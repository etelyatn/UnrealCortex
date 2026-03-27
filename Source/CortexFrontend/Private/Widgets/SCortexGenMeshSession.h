#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "CortexGenSettings.h"
#include "CortexGenTypes.h"

class SMultiLineEditableTextBox;
class SEditableTextBox;
class STextBlock;
class SButton;
class SImage;
class SCortexGenOverlay;
class SWidgetSwitcher;
class SVerticalBox;

class SCortexGenMeshSession : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexGenMeshSession) {}
        SLATE_ARGUMENT(FGuid, SessionId)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual ~SCortexGenMeshSession();

    static bool IsModelCompatible(const FCortexGenModelConfig& Model,
        ECortexGenJobType JobType);

private:
    void PopulateModelOptions();
    void OnModeChanged(int32 NewMode);
    FReply OnGenerateClicked();
    void OnJobStateChanged(const FCortexGenJobState& JobState);
    void OnMeshJobComplete(const FString& DownloadPath);
    void OnSaveMesh();
    void OnOpenMesh();
    void OnRemoveMesh();
    void OnRegenerateMesh();
    void ShowOverlay();
    void HideOverlay();
    void CancelGeneration();

    FGuid SessionId;
    ECortexGenJobType CurrentMode = ECortexGenJobType::MeshFromText;

    // Widgets
    TSharedPtr<SWidgetSwitcher> InputSwitcher;
    TSharedPtr<SMultiLineEditableTextBox> PromptBox;
    TSharedPtr<SEditableTextBox> ImageUrlInput;
    TSharedPtr<SButton> GenerateButton;
    TSharedPtr<SCortexGenOverlay> Overlay;

    // Result area
    TSharedPtr<SImage> ThumbnailImage;
    TSharedPtr<STextBlock> AssetNameLabel;
    TSharedPtr<SVerticalBox> ResultArea;

    // Model selection
    TArray<TSharedPtr<FString>> ModelOptions;
    TArray<FCortexGenModelConfig> FilteredModels;
    int32 SelectedModelIndex = 0;

    // Job tracking
    FString CurrentJobId;
    FString TempAssetPath;    // /Game/Generated/Temp/{SessionId}/... (empty after save)
    FString SavedAssetPath;   // Set after user saves — used by Open, not deleted by destructor
    FString DownloadedGlbPath;

    FDelegateHandle JobStateHandle;
};
