#include "Widgets/SCortexGenImageSession.h"
#include "Widgets/SCortexGenOverlay.h"
#include "Widgets/SCortexGenSaveDialog.h"
#include "CortexFrontendModule.h"
#include "CortexGenModule.h"
#include "Operations/CortexGenJobManager.h"

#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Images/SImage.h"
#include "Framework/Application/SlateApplication.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Rendering/SlateRenderer.h"
#include "Math/Vector2D.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetImportTask.h"
#include "ScopedTransaction.h"

// ---------------------------------------------------------------------------

void SCortexGenImageSession::Construct(const FArguments& InArgs)
{
    SessionId = InArgs._SessionId;

    PopulateModelOptions();

    // Size options
    SizeOptions.Add(MakeShared<FString>(TEXT("512x512")));
    SizeOptions.Add(MakeShared<FString>(TEXT("768x768")));
    SizeOptions.Add(MakeShared<FString>(TEXT("1024x1024")));
    SizeOptions.Add(MakeShared<FString>(TEXT("1024x768")));
    SizeOptions.Add(MakeShared<FString>(TEXT("768x1024")));
    SelectedSizeIndex = 0;

    // Subscribe to job state changes
    if (FModuleManager::Get().IsModuleLoaded(TEXT("CortexGen")))
    {
        FCortexGenModule& GenModule = FModuleManager::GetModuleChecked<FCortexGenModule>(TEXT("CortexGen"));
        FCortexGenJobManager& Manager = GenModule.GetJobManager();
        JobStateHandle = Manager.OnJobStateChanged().AddSP(
            this, &SCortexGenImageSession::OnJobStateChanged);
    }

    TSharedRef<SCortexGenOverlay> OverlayWidget = SNew(SCortexGenOverlay)
        .OnCancelClicked_Lambda([this]() { CancelGeneration(); });
    Overlay = OverlayWidget;

    ChildSlot
    [
        SNew(SOverlay)

        // Main content
        + SOverlay::Slot()
        [
            SNew(SVerticalBox)

            // Config bar
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(8.f, 8.f, 8.f, 4.f)
            [
                SNew(SHorizontalBox)

                // Model selector
                + SHorizontalBox::Slot()
                .FillWidth(1.f)
                .Padding(0.f, 0.f, 8.f, 0.f)
                [
                    SNew(SComboBox<TSharedPtr<FString>>)
                    .OptionsSource(&ModelOptions)
                    .OnSelectionChanged_Lambda([this](TSharedPtr<FString> Selected, ESelectInfo::Type)
                    {
                        if (Selected.IsValid())
                        {
                            for (int32 i = 0; i < ModelOptions.Num(); ++i)
                            {
                                if (ModelOptions[i] == Selected)
                                {
                                    SelectedModelIndex = i;
                                    break;
                                }
                            }
                        }
                    })
                    .OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) -> TSharedRef<SWidget>
                    {
                        return SNew(STextBlock)
                            .Text(Item.IsValid() ? FText::FromString(*Item) : FText::GetEmpty());
                    })
                    [
                        SNew(STextBlock)
                        .Text_Lambda([this]() -> FText
                        {
                            if (ModelOptions.IsValidIndex(SelectedModelIndex))
                            {
                                return FText::FromString(*ModelOptions[SelectedModelIndex]);
                            }
                            return FText::FromString(TEXT("No models"));
                        })
                    ]
                ]

                // Image count
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(0.f, 0.f, 8.f, 0.f)
                [
                    SNew(SBox)
                    .WidthOverride(60.f)
                    [
                        SNew(SSpinBox<int32>)
                        .MinValue(1)
                        .MaxValue(4)
                        .Value_Lambda([this]() { return ImageCount; })
                        .OnValueChanged_Lambda([this](int32 Val) { ImageCount = Val; })
                    ]
                ]

                // Size selector
                + SHorizontalBox::Slot()
                .AutoWidth()
                [
                    SNew(SComboBox<TSharedPtr<FString>>)
                    .OptionsSource(&SizeOptions)
                    .OnSelectionChanged_Lambda([this](TSharedPtr<FString> Selected, ESelectInfo::Type)
                    {
                        if (Selected.IsValid())
                        {
                            for (int32 i = 0; i < SizeOptions.Num(); ++i)
                            {
                                if (SizeOptions[i] == Selected)
                                {
                                    SelectedSizeIndex = i;
                                    break;
                                }
                            }
                        }
                    })
                    .OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) -> TSharedRef<SWidget>
                    {
                        return SNew(STextBlock)
                            .Text(Item.IsValid() ? FText::FromString(*Item) : FText::GetEmpty());
                    })
                    [
                        SNew(STextBlock)
                        .Text_Lambda([this]() -> FText
                        {
                            if (SizeOptions.IsValidIndex(SelectedSizeIndex))
                            {
                                return FText::FromString(*SizeOptions[SelectedSizeIndex]);
                            }
                            return FText::GetEmpty();
                        })
                    ]
                ]
            ]

            // Prompt area
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(8.f, 0.f)
            [
                SAssignNew(PromptBox, SMultiLineEditableTextBox)
                .HintText(FText::FromString(TEXT("Describe the image to generate...")))
                .AutoWrapText(true)
            ]

            // Generate button
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(8.f, 4.f)
            .HAlign(HAlign_Right)
            [
                SAssignNew(GenerateButton, SButton)
                .Text(FText::FromString(TEXT("Generate")))
                .OnClicked(FOnClicked::CreateSP(this, &SCortexGenImageSession::OnGenerateClicked))
            ]

            // Results area
            + SVerticalBox::Slot()
            .FillHeight(1.f)
            .Padding(8.f, 0.f, 8.f, 8.f)
            [
                SNew(SScrollBox)
                + SScrollBox::Slot()
                [
                    SAssignNew(ResultsArea, SWrapBox)
                    .UseAllottedSize(true)
                    .InnerSlotPadding(FVector2D(8.f, 8.f))
                ]
            ]
        ]

        // Overlay (on top)
        + SOverlay::Slot()
        [
            OverlayWidget
        ]
    ];

    // Start hidden
    if (Overlay.IsValid())
    {
        Overlay->Hide();
    }
}

SCortexGenImageSession::~SCortexGenImageSession()
{
    // Unsubscribe from delegate
    if (JobStateHandle.IsValid() && FModuleManager::Get().IsModuleLoaded(TEXT("CortexGen")))
    {
        FCortexGenModule& GenModule = FModuleManager::GetModuleChecked<FCortexGenModule>(TEXT("CortexGen"));
        GenModule.GetJobManager().OnJobStateChanged().Remove(JobStateHandle);
    }
    JobStateHandle.Reset();

    CancelGeneration();

    // Release brushes and delete temp files
    for (FImageResult& Result : ImageResults)
    {
        if (Result.Brush.IsValid())
        {
            Result.Brush->ReleaseResource();
            Result.Brush.Reset();
        }
        if (!Result.DownloadPath.IsEmpty() && !Result.bSaved)
        {
            IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
            if (PlatformFile.FileExists(*Result.DownloadPath))
            {
                PlatformFile.DeleteFile(*Result.DownloadPath);
            }
        }
    }
    ImageResults.Empty();
}

// ---------------------------------------------------------------------------

bool SCortexGenImageSession::IsModelCompatible(const FCortexGenModelConfig& Model)
{
    if (Model.Category != TEXT("image"))
    {
        return false;
    }
    const uint8 ImageFromTextFlag = static_cast<uint8>(ECortexGenCapabilityFlags::ImageFromText);
    return (Model.Capabilities & ImageFromTextFlag) != 0;
}

// ---------------------------------------------------------------------------

void SCortexGenImageSession::PopulateModelOptions()
{
    FilteredModels.Empty();
    ModelOptions.Empty();

    const UCortexGenSettings* Settings = UCortexGenSettings::Get();
    if (!Settings)
    {
        return;
    }

    for (const FCortexGenModelConfig& Model : Settings->ModelRegistry)
    {
        if (IsModelCompatible(Model))
        {
            FilteredModels.Add(Model);
            FString DisplayText = Model.DisplayName.IsEmpty() ? Model.ModelId : Model.DisplayName;
            ModelOptions.Add(MakeShared<FString>(DisplayText));
        }
    }

    SelectedModelIndex = 0;
}

// ---------------------------------------------------------------------------

FReply SCortexGenImageSession::OnGenerateClicked()
{
    if (!PromptBox.IsValid())
    {
        return FReply::Handled();
    }

    FString Prompt = PromptBox->GetText().ToString().TrimStartAndEnd();
    if (Prompt.IsEmpty())
    {
        UE_LOG(LogCortexFrontend, Warning, TEXT("SCortexGenImageSession: Prompt is empty"));
        return FReply::Handled();
    }

    if (!FModuleManager::Get().IsModuleLoaded(TEXT("CortexGen")))
    {
        UE_LOG(LogCortexFrontend, Warning, TEXT("SCortexGenImageSession: CortexGen module not loaded"));
        return FReply::Handled();
    }

    // Clear previous results
    for (FImageResult& Result : ImageResults)
    {
        if (Result.Brush.IsValid())
        {
            Result.Brush->ReleaseResource();
            Result.Brush.Reset();
        }
    }
    ImageResults.Empty();
    JobIds.Empty();

    if (ResultsArea.IsValid())
    {
        ResultsArea->ClearChildren();
    }

    // Cache model config
    if (FilteredModels.IsValidIndex(SelectedModelIndex))
    {
        CachedModelConfig = FilteredModels[SelectedModelIndex];
    }
    else
    {
        CachedModelConfig = FCortexGenModelConfig();
    }

    NextJobIndex = 0;
    CompletedJobCount = 0;
    ConsecutiveFailures = 0;

    // Pre-allocate result slots
    ImageResults.SetNum(ImageCount);
    JobIds.SetNum(ImageCount);

    ShowOverlay();
    SubmitNextJob();

    return FReply::Handled();
}

// ---------------------------------------------------------------------------

void SCortexGenImageSession::SubmitNextJob()
{
    // Circuit breaker: stop after 2 consecutive failures
    if (ConsecutiveFailures >= 2)
    {
        UE_LOG(LogCortexFrontend, Warning,
            TEXT("SCortexGenImageSession: stopping after %d consecutive failures"), ConsecutiveFailures);
        HideOverlay();
        return;
    }

    if (NextJobIndex >= ImageCount)
    {
        return;
    }

    if (!FModuleManager::Get().IsModuleLoaded(TEXT("CortexGen")))
    {
        HideOverlay();
        return;
    }

    FCortexGenModule& GenModule = FModuleManager::GetModuleChecked<FCortexGenModule>(TEXT("CortexGen"));
    FCortexGenJobManager& Manager = GenModule.GetJobManager();

    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::ImageFromText;
    Request.Prompt = PromptBox.IsValid() ? PromptBox->GetText().ToString().TrimStartAndEnd() : TEXT("");
    Request.ModelId = CachedModelConfig.ModelId;

    // Pass size as a param
    if (SizeOptions.IsValidIndex(SelectedSizeIndex))
    {
        Request.Params.Add(TEXT("image_size"), *SizeOptions[SelectedSizeIndex]);
    }

    FString JobId;
    FString ErrorMsg;
    bool bOk = Manager.SubmitJob(CachedModelConfig.Provider, Request, JobId, ErrorMsg);

    if (!bOk)
    {
        UE_LOG(LogCortexFrontend, Warning,
            TEXT("SCortexGenImageSession: SubmitJob failed for index %d: %s"),
            NextJobIndex, *ErrorMsg);
        ++ConsecutiveFailures;
        ++NextJobIndex;
        // Try next
        SubmitNextJob();
        return;
    }

    JobIds[NextJobIndex] = JobId;
    ++NextJobIndex;

    if (Overlay.IsValid())
    {
        Overlay->SetStatusText(FString::Printf(
            TEXT("Submitted %d of %d..."), NextJobIndex, ImageCount));
    }
}

// ---------------------------------------------------------------------------

void SCortexGenImageSession::OnJobStateChanged(const FCortexGenJobState& JobState)
{
    // Check if this job belongs to us
    int32 ImageIndex = JobIds.IndexOfByKey(JobState.JobId);
    if (ImageIndex == INDEX_NONE)
    {
        return;
    }

    switch (JobState.Status)
    {
    case ECortexGenJobStatus::Processing:
    {
        if (Overlay.IsValid())
        {
            Overlay->SetProgress(JobState.Progress);
            Overlay->SetStatusText(FString::Printf(
                TEXT("Generating image %d of %d..."), ImageIndex + 1, ImageCount));
        }
        break;
    }
    case ECortexGenJobStatus::Downloading:
    {
        if (Overlay.IsValid())
        {
            Overlay->SetStatusText(FString::Printf(
                TEXT("Downloading image %d of %d..."), ImageIndex + 1, ImageCount));
        }
        break;
    }
    case ECortexGenJobStatus::DownloadFailed:
    case ECortexGenJobStatus::Failed:
    {
        ++ConsecutiveFailures;
        ++CompletedJobCount;
        UE_LOG(LogCortexFrontend, Warning,
            TEXT("SCortexGenImageSession: job %s failed: %s"),
            *JobState.JobId, *JobState.ErrorMessage);

        if (CompletedJobCount >= ImageCount || ConsecutiveFailures >= 2)
        {
            HideOverlay();
        }
        else
        {
            SubmitNextJob();
        }
        break;
    }
    case ECortexGenJobStatus::Imported:
    case ECortexGenJobStatus::ImportFailed:
    // For image sessions we treat "downloaded" as the terminal state
    // (we load for display; save to asset is user-initiated)
    case ECortexGenJobStatus::Complete:
    {
        // Fall through to download-complete handling if path available
        if (!JobState.DownloadPath.IsEmpty())
        {
            ConsecutiveFailures = 0;
            OnImageJobComplete(ImageIndex, JobState.DownloadPath);
        }
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------------------

void SCortexGenImageSession::OnImageJobComplete(int32 ImageIndex, const FString& DownloadPath)
{
    if (!ImageResults.IsValidIndex(ImageIndex))
    {
        return;
    }

    ImageResults[ImageIndex].DownloadPath = DownloadPath;
    ImageResults[ImageIndex].JobId = JobIds.IsValidIndex(ImageIndex) ? JobIds[ImageIndex] : TEXT("");

    // Load image data
    TArray<uint8> FileData;
    if (!FFileHelper::LoadFileToArray(FileData, *DownloadPath))
    {
        UE_LOG(LogCortexFrontend, Warning,
            TEXT("SCortexGenImageSession: Failed to load image file: %s"), *DownloadPath);
        ++CompletedJobCount;
        if (CompletedJobCount >= ImageCount)
        {
            HideOverlay();
        }
        return;
    }

    // Decode via ImageWrapper
    IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(
        TEXT("ImageWrapper"));

    EImageFormat ImageFormat = ImageWrapperModule.DetectImageFormat(FileData.GetData(), FileData.Num());
    TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(ImageFormat);

    int32 Width = 0;
    int32 Height = 0;
    TArray<uint8> RawData;

    if (ImageWrapper.IsValid() && ImageWrapper->SetCompressed(FileData.GetData(), FileData.Num()))
    {
        Width = ImageWrapper->GetWidth();
        Height = ImageWrapper->GetHeight();
        ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, RawData);
    }

    TSharedPtr<FSlateDynamicImageBrush> NewBrush;
    if (RawData.Num() > 0 && Width > 0 && Height > 0)
    {
        FName BrushName = FName(*FString::Printf(
            TEXT("CortexGenImage_%s_%d"), *SessionId.ToString(), ImageIndex));

        NewBrush = FSlateDynamicImageBrush::CreateWithImageData(
            BrushName,
            FVector2D(static_cast<float>(Width), static_cast<float>(Height)),
            RawData,
            FLinearColor::White,
            ESlateBrushTileType::NoTile,
            ESlateBrushImageType::FullColor);
    }

    ImageResults[ImageIndex].Brush = NewBrush;

    // Build image card and add to results area
    if (ResultsArea.IsValid())
    {
        const int32 CardIndex = ImageIndex; // capture by value

        TSharedRef<SWidget> Card =
            SNew(SBox)
            .WidthOverride(200.f)
            .HeightOverride(240.f)
            [
                SNew(SVerticalBox)

                // Image display
                + SVerticalBox::Slot()
                .FillHeight(1.f)
                [
                    SNew(SImage)
                    .Image_Lambda([this, CardIndex]() -> const FSlateBrush*
                    {
                        if (ImageResults.IsValidIndex(CardIndex) &&
                            ImageResults[CardIndex].Brush.IsValid())
                        {
                            return ImageResults[CardIndex].Brush.Get();
                        }
                        return FCoreStyle::Get().GetBrush("WhiteTexture");
                    })
                ]

                // Action buttons
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(4.f, 2.f)
                [
                    SNew(SHorizontalBox)

                    + SHorizontalBox::Slot()
                    .FillWidth(1.f)
                    .Padding(0.f, 0.f, 2.f, 0.f)
                    [
                        SNew(SButton)
                        .Text(FText::FromString(TEXT("Save")))
                        .OnClicked_Lambda([this, CardIndex]() -> FReply
                        {
                            OnSaveImage(CardIndex);
                            return FReply::Handled();
                        })
                    ]

                    + SHorizontalBox::Slot()
                    .FillWidth(1.f)
                    .Padding(0.f, 0.f, 2.f, 0.f)
                    [
                        SNew(SButton)
                        .Text(FText::FromString(TEXT("Regen")))
                        .OnClicked_Lambda([this, CardIndex]() -> FReply
                        {
                            OnRegenerateImage(CardIndex);
                            return FReply::Handled();
                        })
                    ]

                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    [
                        SNew(SButton)
                        .Text(FText::FromString(TEXT("X")))
                        .OnClicked_Lambda([this, CardIndex]() -> FReply
                        {
                            OnRemoveImage(CardIndex);
                            return FReply::Handled();
                        })
                    ]
                ]
            ];

        ResultsArea->AddSlot()
        [
            Card
        ];
    }

    ++CompletedJobCount;
    if (CompletedJobCount >= ImageCount)
    {
        HideOverlay();
    }
}

// ---------------------------------------------------------------------------

void SCortexGenImageSession::OnSaveImage(int32 ImageIndex)
{
    if (!ImageResults.IsValidIndex(ImageIndex))
    {
        return;
    }

    const FImageResult& Result = ImageResults[ImageIndex];
    if (Result.DownloadPath.IsEmpty())
    {
        return;
    }

    const UCortexGenSettings* Settings = UCortexGenSettings::Get();
    FString DefaultPath = Settings ? Settings->DefaultTextureDestination : TEXT("/Game/Generated/Textures");
    FString Prompt = PromptBox.IsValid() ? PromptBox->GetText().ToString() : TEXT("image");
    FString DefaultName = SCortexGenSaveDialog::PromptToSlug(Prompt, 32);
    if (DefaultName.IsEmpty())
    {
        DefaultName = FString::Printf(TEXT("GenImage_%d"), ImageIndex);
    }

    FString OutAssetName;
    FString OutPath;
    bool bConfirmed = SCortexGenSaveDialog::ShowModal(DefaultName, DefaultPath, OutAssetName, OutPath);
    if (!bConfirmed)
    {
        return;
    }

    // Import via AssetTools (mirrors FCortexGenAssetImporter::ImportTexture)
    FScopedTransaction Transaction(FText::FromString(
        FString::Printf(TEXT("Cortex: Save Generated Image %s"), *OutAssetName)));

    UAssetImportTask* ImportTask = NewObject<UAssetImportTask>();
    ImportTask->Filename = Result.DownloadPath;
    ImportTask->DestinationPath = OutPath;
    ImportTask->DestinationName = OutAssetName;
    ImportTask->bAutomated = true;
    ImportTask->bSave = true;
    ImportTask->bReplaceExisting = true;

    IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(
        TEXT("AssetTools")).Get();
    AssetTools.ImportAssetTasks({ ImportTask });

    ImageResults[ImageIndex].bSaved = true;
}

// ---------------------------------------------------------------------------

void SCortexGenImageSession::OnRemoveImage(int32 ImageIndex)
{
    if (!ImageResults.IsValidIndex(ImageIndex))
    {
        return;
    }

    FImageResult& Result = ImageResults[ImageIndex];

    if (Result.Brush.IsValid())
    {
        Result.Brush->ReleaseResource();
        Result.Brush.Reset();
    }

    if (!Result.DownloadPath.IsEmpty() && !Result.bSaved)
    {
        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        if (PlatformFile.FileExists(*Result.DownloadPath))
        {
            PlatformFile.DeleteFile(*Result.DownloadPath);
        }
    }

    Result.DownloadPath.Empty();
    Result.JobId.Empty();
    Result.bSaved = false;

    // Rebuild results area to reflect removal
    // (Simply clearing is the simplest approach; widget will be gone on next rebuild)
    if (ResultsArea.IsValid())
    {
        // Remove card at this index by clearing and re-adding remaining
        // For simplicity: just set image invisible by clearing the slot
        // Full rebuild would require tracking slot references.
        // We clear the whole area and re-add valid images.
        ResultsArea->ClearChildren();

        for (int32 i = 0; i < ImageResults.Num(); ++i)
        {
            const FImageResult& R = ImageResults[i];
            if (R.Brush.IsValid() || !R.DownloadPath.IsEmpty())
            {
                const int32 CardIndex = i;

                TSharedRef<SWidget> Card =
                    SNew(SBox)
                    .WidthOverride(200.f)
                    .HeightOverride(240.f)
                    [
                        SNew(SVerticalBox)
                        + SVerticalBox::Slot()
                        .FillHeight(1.f)
                        [
                            SNew(SImage)
                            .Image_Lambda([this, CardIndex]() -> const FSlateBrush*
                            {
                                if (ImageResults.IsValidIndex(CardIndex) &&
                                    ImageResults[CardIndex].Brush.IsValid())
                                {
                                    return ImageResults[CardIndex].Brush.Get();
                                }
                                return FCoreStyle::Get().GetBrush("WhiteTexture");
                            })
                        ]
                        + SVerticalBox::Slot()
                        .AutoHeight()
                        .Padding(4.f, 2.f)
                        [
                            SNew(SHorizontalBox)
                            + SHorizontalBox::Slot()
                            .FillWidth(1.f)
                            .Padding(0.f, 0.f, 2.f, 0.f)
                            [
                                SNew(SButton)
                                .Text(FText::FromString(TEXT("Save")))
                                .OnClicked_Lambda([this, CardIndex]() -> FReply
                                {
                                    OnSaveImage(CardIndex);
                                    return FReply::Handled();
                                })
                            ]
                            + SHorizontalBox::Slot()
                            .FillWidth(1.f)
                            .Padding(0.f, 0.f, 2.f, 0.f)
                            [
                                SNew(SButton)
                                .Text(FText::FromString(TEXT("Regen")))
                                .OnClicked_Lambda([this, CardIndex]() -> FReply
                                {
                                    OnRegenerateImage(CardIndex);
                                    return FReply::Handled();
                                })
                            ]
                            + SHorizontalBox::Slot()
                            .AutoWidth()
                            [
                                SNew(SButton)
                                .Text(FText::FromString(TEXT("X")))
                                .OnClicked_Lambda([this, CardIndex]() -> FReply
                                {
                                    OnRemoveImage(CardIndex);
                                    return FReply::Handled();
                                })
                            ]
                        ]
                    ];

                ResultsArea->AddSlot()
                [
                    Card
                ];
            }
        }
    }
}

// ---------------------------------------------------------------------------

void SCortexGenImageSession::OnRegenerateImage(int32 ImageIndex)
{
    if (!ImageResults.IsValidIndex(ImageIndex))
    {
        return;
    }

    // Clear old result
    FImageResult& Result = ImageResults[ImageIndex];
    if (Result.Brush.IsValid())
    {
        Result.Brush->ReleaseResource();
        Result.Brush.Reset();
    }

    Result.DownloadPath.Empty();
    Result.bSaved = false;

    if (!FModuleManager::Get().IsModuleLoaded(TEXT("CortexGen")))
    {
        return;
    }

    FCortexGenModule& GenModule = FModuleManager::GetModuleChecked<FCortexGenModule>(TEXT("CortexGen"));
    FCortexGenJobManager& Manager = GenModule.GetJobManager();

    FCortexGenJobRequest Request;
    Request.Type = ECortexGenJobType::ImageFromText;
    Request.Prompt = PromptBox.IsValid() ? PromptBox->GetText().ToString().TrimStartAndEnd() : TEXT("");
    Request.ModelId = CachedModelConfig.ModelId;

    if (SizeOptions.IsValidIndex(SelectedSizeIndex))
    {
        Request.Params.Add(TEXT("image_size"), *SizeOptions[SelectedSizeIndex]);
    }

    FString NewJobId;
    FString ErrorMsg;
    bool bOk = Manager.SubmitJob(CachedModelConfig.Provider, Request, NewJobId, ErrorMsg);

    if (bOk)
    {
        if (JobIds.IsValidIndex(ImageIndex))
        {
            JobIds[ImageIndex] = NewJobId;
        }
        ShowOverlay();
    }
    else
    {
        UE_LOG(LogCortexFrontend, Warning,
            TEXT("SCortexGenImageSession: Regen failed for index %d: %s"),
            ImageIndex, *ErrorMsg);
    }
}

// ---------------------------------------------------------------------------

void SCortexGenImageSession::ShowOverlay()
{
    if (Overlay.IsValid())
    {
        Overlay->Show();
        Overlay->SetProgressIndeterminate(true);
        Overlay->SetStatusText(TEXT("Generating..."));
    }
}

void SCortexGenImageSession::HideOverlay()
{
    if (Overlay.IsValid())
    {
        Overlay->Hide();
    }
}

void SCortexGenImageSession::CancelGeneration()
{
    if (!FModuleManager::Get().IsModuleLoaded(TEXT("CortexGen")))
    {
        return;
    }

    FCortexGenModule& GenModule = FModuleManager::GetModuleChecked<FCortexGenModule>(TEXT("CortexGen"));
    FCortexGenJobManager& Manager = GenModule.GetJobManager();

    for (const FString& JobId : JobIds)
    {
        if (JobId.IsEmpty())
        {
            continue;
        }
        const FCortexGenJobState* State = Manager.GetJobState(JobId);
        if (!State)
        {
            continue;
        }
        if (State->Status == ECortexGenJobStatus::Pending ||
            State->Status == ECortexGenJobStatus::Processing)
        {
            FString ErrorMsg;
            Manager.CancelJob(JobId, ErrorMsg);
        }
    }
}
