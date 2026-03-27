#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "CortexGenSettings.h"
#include "CortexGenTypes.h"

class SMultiLineEditableTextBox;
class STextBlock;
class SButton;
class SWrapBox;
class SCortexGenOverlay;
class SExpandableArea;

class SCortexGenImageSession : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexGenImageSession) {}
        SLATE_ARGUMENT(FGuid, SessionId)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual ~SCortexGenImageSession();

    static bool IsModelCompatible(const FCortexGenModelConfig& Model);

private:
    void PopulateModelOptions();
    FReply OnGenerateClicked();
    void SubmitNextJob();
    void OnJobStateChanged(const FCortexGenJobState& JobState);
    void OnImageJobComplete(int32 ImageIndex, const FString& DownloadPath);
    void OnSaveImage(int32 ImageIndex);
    void OnRemoveImage(int32 ImageIndex);
    void OnRegenerateImage(int32 ImageIndex);
    void ShowOverlay();
    void HideOverlay();
    void CancelGeneration();

    FGuid SessionId;

    TSharedPtr<SMultiLineEditableTextBox> PromptBox;
    TSharedPtr<SButton> GenerateButton;
    TSharedPtr<SWrapBox> ResultsArea;
    TSharedPtr<SCortexGenOverlay> Overlay;
    TSharedPtr<SExpandableArea> AdvancedSection;

    TArray<TSharedPtr<FString>> ModelOptions;
    TArray<FCortexGenModelConfig> FilteredModels;
    int32 SelectedModelIndex = 0;

    int32 ImageCount = 1;

    TArray<TSharedPtr<FString>> SizeOptions;
    int32 SelectedSizeIndex = 0;

    TArray<FString> JobIds;
    int32 NextJobIndex = 0;
    int32 CompletedJobCount = 0;
    int32 ConsecutiveFailures = 0;

    FCortexGenModelConfig CachedModelConfig;

    struct FImageResult
    {
        FString DownloadPath;
        TSharedPtr<FSlateDynamicImageBrush> Brush;
        FString JobId;
        bool bSaved = false;
    };
    TArray<FImageResult> ImageResults;

    FDelegateHandle JobStateHandle;
};
