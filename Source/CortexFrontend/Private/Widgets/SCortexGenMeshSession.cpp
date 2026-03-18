#include "Widgets/SCortexGenMeshSession.h"
#include "Widgets/SCortexGenOverlay.h"
#include "Widgets/SCortexGenSaveDialog.h"
#include "CortexFrontendModule.h"
#include "CortexGenModule.h"
#include "Operations/CortexGenJobManager.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/STextComboBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "EditorAssetLibrary.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"
#include "Editor.h"
#include "ScopedTransaction.h"

void SCortexGenMeshSession::Construct(const FArguments& InArgs)
{
    SessionId = InArgs._SessionId;
    PopulateModelOptions();

    // Subscribe to job state changes
    if (FModuleManager::Get().IsModuleLoaded(TEXT("CortexGen")))
    {
        auto& JobManager = FModuleManager::GetModuleChecked<FCortexGenModule>(
            TEXT("CortexGen")).GetJobManager();
        JobStateHandle = JobManager.OnJobStateChanged().AddSP(
            this, &SCortexGenMeshSession::OnJobStateChanged);
    }

    TSharedPtr<STextComboBox> ModelCombo;

    ChildSlot
    [
        SNew(SOverlay)

        + SOverlay::Slot()
        [
            SNew(SScrollBox)
            + SScrollBox::Slot()
            .Padding(8.f)
            [
                SNew(SVerticalBox)

                // Mode toggle: Text-to-3D / Image-to-3D
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.f, 0.f, 0.f, 8.f)
                [
                    SNew(SSegmentedControl<int32>)
                    .Value(0)
                    .OnValueChanged_Lambda([this](int32 Val) { OnModeChanged(Val); })
                    + SSegmentedControl<int32>::Slot(0)
                    .Text(FText::FromString(TEXT("Text-to-3D")))
                    + SSegmentedControl<int32>::Slot(1)
                    .Text(FText::FromString(TEXT("Image-to-3D")))
                ]

                // Model dropdown
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.f, 0.f, 0.f, 8.f)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    .VAlign(VAlign_Center)
                    .Padding(0.f, 0.f, 8.f, 0.f)
                    [
                        SNew(STextBlock).Text(FText::FromString(TEXT("Model:")))
                    ]
                    + SHorizontalBox::Slot()
                    .FillWidth(1.f)
                    [
                        SAssignNew(ModelCombo, STextComboBox)
                        .OptionsSource(&ModelOptions)
                        .InitiallySelectedItem(ModelOptions.Num() > 0
                            ? ModelOptions[0] : TSharedPtr<FString>())
                        .OnSelectionChanged_Lambda([this](TSharedPtr<FString> Val,
                            ESelectInfo::Type)
                        {
                            SelectedModelIndex = ModelOptions.IndexOfByKey(Val);
                        })
                    ]
                ]

                // Input switcher
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.f, 0.f, 0.f, 8.f)
                [
                    SAssignNew(InputSwitcher, SWidgetSwitcher)

                    // Index 0: Text prompt
                    + SWidgetSwitcher::Slot()
                    [
                        SNew(SBox)
                        .HeightOverride(80.f)
                        [
                            SAssignNew(PromptBox, SMultiLineEditableTextBox)
                            .HintText(FText::FromString(TEXT("Describe the 3D model...")))
                        ]
                    ]

                    // Index 1: Image source
                    + SWidgetSwitcher::Slot()
                    [
                        SNew(SVerticalBox)
                        + SVerticalBox::Slot()
                        .AutoHeight()
                        [
                            SNew(SHorizontalBox)
                            + SHorizontalBox::Slot()
                            .AutoWidth()
                            .Padding(0.f, 0.f, 8.f, 0.f)
                            [
                                SNew(SButton)
                                .Text(FText::FromString(TEXT("Load from disk...")))
                                .OnClicked_Lambda([this]()
                                {
                                    IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
                                    if (!DesktopPlatform) return FReply::Handled();
                                    TArray<FString> Files;
                                    if (DesktopPlatform->OpenFileDialog(
                                        FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
                                        TEXT("Select Image"),
                                        FPaths::ProjectDir(), TEXT(""),
                                        TEXT("Images|*.png;*.jpg;*.jpeg;*.webp"),
                                        EFileDialogFlags::None, Files) && Files.Num() > 0)
                                    {
                                        if (ImageUrlInput.IsValid())
                                        {
                                            ImageUrlInput->SetText(FText::FromString(Files[0]));
                                        }
                                    }
                                    return FReply::Handled();
                                })
                            ]
                        ]
                        + SVerticalBox::Slot()
                        .AutoHeight()
                        .Padding(0.f, 4.f, 0.f, 0.f)
                        [
                            SAssignNew(ImageUrlInput, SEditableTextBox)
                            .HintText(FText::FromString(TEXT("Or enter https:// URL")))
                        ]
                    ]
                ]

                // Generate button
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.f, 0.f, 0.f, 8.f)
                [
                    SAssignNew(GenerateButton, SButton)
                    .Text(FText::FromString(TEXT("Generate")))
                    .OnClicked(FOnClicked::CreateSP(
                        this, &SCortexGenMeshSession::OnGenerateClicked))
                ]

                // Result area (hidden initially)
                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SAssignNew(ResultArea, SVerticalBox)
                    .Visibility(EVisibility::Collapsed)

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    [
                        SNew(SBox)
                        .WidthOverride(200.f)
                        .HeightOverride(200.f)
                        [
                            SAssignNew(ThumbnailImage, SImage)
                        ]
                    ]

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .Padding(0.f, 4.f)
                    [
                        SAssignNew(AssetNameLabel, STextBlock)
                    ]

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
                        [
                            SNew(SButton)
                            .Text(FText::FromString(TEXT("Save")))
                            .OnClicked_Lambda([this]()
                            {
                                OnSaveMesh();
                                return FReply::Handled();
                            })
                        ]
                        + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
                        [
                            SNew(SButton)
                            .Text(FText::FromString(TEXT("Open")))
                            .OnClicked_Lambda([this]()
                            {
                                OnOpenMesh();
                                return FReply::Handled();
                            })
                        ]
                        + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
                        [
                            SNew(SButton)
                            .Text(FText::FromString(TEXT("Remove")))
                            .OnClicked_Lambda([this]()
                            {
                                OnRemoveMesh();
                                return FReply::Handled();
                            })
                        ]
                        + SHorizontalBox::Slot().AutoWidth()
                        [
                            SNew(SButton)
                            .Text(FText::FromString(TEXT("Regenerate")))
                            .OnClicked_Lambda([this]()
                            {
                                OnRegenerateMesh();
                                return FReply::Handled();
                            })
                        ]
                    ]
                ]
            ]
        ]

        // Overlay
        + SOverlay::Slot()
        [
            SAssignNew(Overlay, SCortexGenOverlay)
            .OnCancelClicked_Lambda([this]() { CancelGeneration(); })
            .Visibility(EVisibility::Collapsed)
        ]
    ];
}

SCortexGenMeshSession::~SCortexGenMeshSession()
{
    if (JobStateHandle.IsValid() && FModuleManager::Get().IsModuleLoaded(TEXT("CortexGen")))
    {
        auto& JobManager = FModuleManager::GetModuleChecked<FCortexGenModule>(
            TEXT("CortexGen")).GetJobManager();
        JobManager.OnJobStateChanged().Remove(JobStateHandle);
    }

    // Clean up temp assets
    if (!TempAssetPath.IsEmpty() && UEditorAssetLibrary::DoesAssetExist(TempAssetPath))
    {
        UEditorAssetLibrary::DeleteAsset(TempAssetPath);
    }
}

bool SCortexGenMeshSession::IsModelCompatible(const FCortexGenModelConfig& Model,
    ECortexGenJobType JobType)
{
    if (Model.Category != TEXT("mesh")) return false;

    uint8 RequiredCap = 0;
    if (JobType == ECortexGenJobType::MeshFromText)
    {
        RequiredCap = static_cast<uint8>(ECortexGenCapabilityFlags::MeshFromText);
    }
    else if (JobType == ECortexGenJobType::MeshFromImage)
    {
        RequiredCap = static_cast<uint8>(ECortexGenCapabilityFlags::MeshFromImage);
    }
    return (Model.Capabilities & RequiredCap) != 0;
}

void SCortexGenMeshSession::PopulateModelOptions()
{
    const UCortexGenSettings* Settings = UCortexGenSettings::Get();
    if (!Settings) return;

    ModelOptions.Empty();
    FilteredModels.Empty();

    for (const FCortexGenModelConfig& Config : Settings->ModelRegistry)
    {
        if (IsModelCompatible(Config, CurrentMode))
        {
            FilteredModels.Add(Config);
            FString Label = Config.DisplayName;
            if (!Config.PricingNote.IsEmpty())
            {
                Label += FString::Printf(TEXT(" (%s)"), *Config.PricingNote);
            }
            ModelOptions.Add(MakeShared<FString>(Label));
        }
    }
}

void SCortexGenMeshSession::OnModeChanged(int32 NewMode)
{
    CurrentMode = (NewMode == 0)
        ? ECortexGenJobType::MeshFromText
        : ECortexGenJobType::MeshFromImage;

    if (InputSwitcher.IsValid())
    {
        InputSwitcher->SetActiveWidgetIndex(NewMode);
    }

    PopulateModelOptions();
}

FReply SCortexGenMeshSession::OnGenerateClicked()
{
    if (!FModuleManager::Get().IsModuleLoaded(TEXT("CortexGen"))) return FReply::Handled();
    if (!FilteredModels.IsValidIndex(SelectedModelIndex)) return FReply::Handled();

    const FCortexGenModelConfig& Model = FilteredModels[SelectedModelIndex];

    FCortexGenJobRequest Request;
    Request.Type = CurrentMode;
    Request.ModelId = Model.ModelId;

    if (CurrentMode == ECortexGenJobType::MeshFromText)
    {
        if (!PromptBox.IsValid()) return FReply::Handled();
        Request.Prompt = PromptBox->GetText().ToString();
        if (Request.Prompt.IsEmpty()) return FReply::Handled();
    }
    else
    {
        if (!ImageUrlInput.IsValid()) return FReply::Handled();
        Request.SourceImagePath = ImageUrlInput->GetText().ToString();
        if (Request.SourceImagePath.IsEmpty()) return FReply::Handled();
    }

    // Set temp destination for import
    Request.Destination = FString::Printf(TEXT("/Game/Generated/Temp/%s"),
        *SessionId.ToString());

    auto& JobManager = FModuleManager::GetModuleChecked<FCortexGenModule>(
        TEXT("CortexGen")).GetJobManager();

    FString Error;
    if (JobManager.SubmitJob(Model.Provider, Request, CurrentJobId, Error))
    {
        ShowOverlay();
    }
    else
    {
        UE_LOG(LogCortexFrontend, Warning, TEXT("Failed to submit mesh job: %s"), *Error);
    }

    return FReply::Handled();
}

void SCortexGenMeshSession::OnJobStateChanged(const FCortexGenJobState& JobState)
{
    if (JobState.JobId != CurrentJobId) return;

    if (Overlay.IsValid())
    {
        if (JobState.Progress > 0.f && JobState.Progress < 1.f)
        {
            Overlay->SetProgressIndeterminate(false);
            Overlay->SetProgress(JobState.Progress);
        }
    }

    if (JobState.Status == ECortexGenJobStatus::Imported)
    {
        HideOverlay();
        DownloadedGlbPath = JobState.DownloadPath;
        if (JobState.ImportedAssetPaths.Num() > 0)
        {
            TempAssetPath = JobState.ImportedAssetPaths[0];
            OnMeshJobComplete(JobState.DownloadPath);
        }
    }
    else if (JobState.Status == ECortexGenJobStatus::Failed ||
             JobState.Status == ECortexGenJobStatus::Cancelled)
    {
        HideOverlay();
    }
}

void SCortexGenMeshSession::OnMeshJobComplete(const FString& DownloadPath)
{
    if (ResultArea.IsValid())
    {
        ResultArea->SetVisibility(EVisibility::Visible);
    }

    if (AssetNameLabel.IsValid())
    {
        AssetNameLabel->SetText(FText::FromString(
            FPaths::GetBaseFilename(TempAssetPath)));
    }

    // TODO: Load thumbnail from asset registry
}

void SCortexGenMeshSession::OnSaveMesh()
{
    if (TempAssetPath.IsEmpty()) return;

    FString DefaultName = FPaths::GetBaseFilename(TempAssetPath);
    const UCortexGenSettings* Settings = UCortexGenSettings::Get();
    FString DefaultPath = Settings ? Settings->DefaultMeshDestination
        : TEXT("/Game/Generated/Meshes");

    FString AssetName, DestPath;
    if (SCortexGenSaveDialog::ShowModal(DefaultName, DefaultPath, AssetName, DestPath))
    {
        FScopedTransaction Transaction(FText::FromString(
            FString::Printf(TEXT("Cortex: Save Generated Mesh %s"), *AssetName)));

        FString FinalPath = DestPath / AssetName;
        if (UEditorAssetLibrary::DuplicateAsset(TempAssetPath, FinalPath))
        {
            UEditorAssetLibrary::DeleteAsset(TempAssetPath);
            TempAssetPath.Empty();
            SavedAssetPath = FinalPath;

            if (AssetNameLabel.IsValid())
            {
                AssetNameLabel->SetText(FText::FromString(AssetName));
            }
        }
    }
}

void SCortexGenMeshSession::OnOpenMesh()
{
    FString PathToOpen = SavedAssetPath.IsEmpty() ? TempAssetPath : SavedAssetPath;
    if (PathToOpen.IsEmpty()) return;

    UObject* Asset = UEditorAssetLibrary::LoadAsset(PathToOpen);
    if (Asset && GEditor)
    {
        GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Asset);
    }
}

void SCortexGenMeshSession::OnRemoveMesh()
{
    if (!TempAssetPath.IsEmpty() && UEditorAssetLibrary::DoesAssetExist(TempAssetPath))
    {
        UEditorAssetLibrary::DeleteAsset(TempAssetPath);
    }
    TempAssetPath.Empty();

    if (!DownloadedGlbPath.IsEmpty())
    {
        IFileManager::Get().Delete(*DownloadedGlbPath);
    }

    if (ResultArea.IsValid())
    {
        ResultArea->SetVisibility(EVisibility::Collapsed);
    }
}

void SCortexGenMeshSession::OnRegenerateMesh()
{
    OnRemoveMesh();
    OnGenerateClicked();
}

void SCortexGenMeshSession::ShowOverlay()
{
    if (Overlay.IsValid())
    {
        Overlay->Show();
        Overlay->SetStatusText(TEXT("Generating 3D mesh..."));

        if (FModuleManager::Get().IsModuleLoaded(TEXT("CortexGen")) &&
            FilteredModels.IsValidIndex(SelectedModelIndex))
        {
            auto& JobManager = FModuleManager::GetModuleChecked<FCortexGenModule>(
                TEXT("CortexGen")).GetJobManager();
            float Avg = JobManager.GetAverageTime(FilteredModels[SelectedModelIndex].ModelId);
            Overlay->SetExpectedTime(Avg);
            Overlay->SetProgressIndeterminate(Avg <= 0.f);
        }
    }
    if (GenerateButton.IsValid())
    {
        GenerateButton->SetEnabled(false);
    }
}

void SCortexGenMeshSession::HideOverlay()
{
    if (Overlay.IsValid())
    {
        Overlay->Hide();
    }
    if (GenerateButton.IsValid())
    {
        GenerateButton->SetEnabled(true);
    }
}

void SCortexGenMeshSession::CancelGeneration()
{
    if (CurrentJobId.IsEmpty()) return;
    if (!FModuleManager::Get().IsModuleLoaded(TEXT("CortexGen"))) return;

    auto& JobManager = FModuleManager::GetModuleChecked<FCortexGenModule>(
        TEXT("CortexGen")).GetJobManager();
    FString Error;
    JobManager.CancelJob(CurrentJobId, Error);
    HideOverlay();
}
