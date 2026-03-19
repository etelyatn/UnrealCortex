#include "Widgets/SCortexGenSaveDialog.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "EditorAssetLibrary.h"
#include "Framework/Application/SlateApplication.h"

void SCortexGenSaveDialog::Construct(const FArguments& InArgs)
{
    SaveConfirmedDelegate = InArgs._OnSaveConfirmed;

    ChildSlot
    [
        SNew(SBox)
        .WidthOverride(400.f)
        .Padding(16.f)
        [
            SNew(SVerticalBox)

            // Asset name label
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 0.f, 0.f, 4.f)
            [
                SNew(STextBlock)
                .Text(FText::FromString(TEXT("Asset Name")))
            ]

            // Asset name input
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 0.f, 0.f, 8.f)
            [
                SAssignNew(NameInput, SEditableTextBox)
                .Text(FText::FromString(InArgs._DefaultAssetName))
            ]

            // Destination path label
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 0.f, 0.f, 4.f)
            [
                SNew(STextBlock)
                .Text(FText::FromString(TEXT("Destination Path")))
            ]

            // Destination path input
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 0.f, 0.f, 8.f)
            [
                SAssignNew(PathInput, SEditableTextBox)
                .Text(FText::FromString(InArgs._DefaultDestinationPath))
            ]

            // Warning label
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 0.f, 0.f, 8.f)
            [
                SAssignNew(WarningLabel, STextBlock)
                .ColorAndOpacity(FLinearColor(1.f, 0.5f, 0.f))
                .Visibility(EVisibility::Collapsed)
            ]

            // Buttons
            + SVerticalBox::Slot()
            .AutoHeight()
            .HAlign(HAlign_Right)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(0.f, 0.f, 8.f, 0.f)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Cancel")))
                    .OnClicked(FOnClicked::CreateSP(this, &SCortexGenSaveDialog::OnCancelClicked))
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Save")))
                    .OnClicked(FOnClicked::CreateSP(this, &SCortexGenSaveDialog::OnSaveClicked))
                ]
            ]
        ]
    ];
}

FReply SCortexGenSaveDialog::OnSaveClicked()
{
    FString AssetName = NameInput.IsValid() ? NameInput->GetText().ToString() : TEXT("");
    FString Path = PathInput.IsValid() ? PathInput->GetText().ToString() : TEXT("");

    if (AssetName.IsEmpty() || Path.IsEmpty())
    {
        if (WarningLabel.IsValid())
        {
            WarningLabel->SetText(FText::FromString(TEXT("Name and path are required.")));
            WarningLabel->SetVisibility(EVisibility::Visible);
        }
        return FReply::Handled();
    }

    // Collision check
    FString FullPath = Path / AssetName;
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        if (WarningLabel.IsValid())
        {
            WarningLabel->SetText(FText::FromString(
                FString::Printf(TEXT("Asset '%s' already exists. Rename it or choose a different path."),
                    *AssetName)));
            WarningLabel->SetVisibility(EVisibility::Visible);
        }
        return FReply::Handled();   // stop here, don't save
    }

    // Clear any previous warning
    if (WarningLabel.IsValid())
    {
        WarningLabel->SetText(FText::GetEmpty());
        WarningLabel->SetVisibility(EVisibility::Collapsed);
    }

    SaveConfirmedDelegate.ExecuteIfBound(AssetName, Path);

    if (ParentWindow.IsValid())
    {
        ParentWindow.Pin()->RequestDestroyWindow();
    }

    return FReply::Handled();
}

FReply SCortexGenSaveDialog::OnCancelClicked()
{
    if (ParentWindow.IsValid())
    {
        ParentWindow.Pin()->RequestDestroyWindow();
    }
    return FReply::Handled();
}

FString SCortexGenSaveDialog::PromptToSlug(const FString& Prompt, int32 MaxLength)
{
    FString Slug;
    Slug.Reserve(FMath::Min(Prompt.Len(), MaxLength));
    bool bPrevUnderscore = false;
    for (TCHAR Ch : Prompt)
    {
        if (FChar::IsAlnum(Ch))
        {
            Slug.AppendChar(Ch);
            bPrevUnderscore = false;
        }
        else if (!bPrevUnderscore)
        {
            Slug.AppendChar('_');
            bPrevUnderscore = true;
        }
        if (Slug.Len() >= MaxLength) break;
    }
    Slug.TrimStartAndEndInline();
    return Slug;
}

bool SCortexGenSaveDialog::ShowModal(const FString& DefaultName, const FString& DefaultPath,
    FString& OutAssetName, FString& OutPath)
{
    check(IsInGameThread());

    bool bConfirmed = false;
    FString ResultName;
    FString ResultPath;

    TSharedRef<SWindow> Window = SNew(SWindow)
        .Title(FText::FromString(TEXT("Save Generated Asset")))
        .SizingRule(ESizingRule::Autosized)
        .SupportsMaximize(false)
        .SupportsMinimize(false);

    TSharedRef<SCortexGenSaveDialog> Dialog = SNew(SCortexGenSaveDialog)
        .DefaultAssetName(DefaultName)
        .DefaultDestinationPath(DefaultPath)
        .OnSaveConfirmed_Lambda([&bConfirmed, &ResultName, &ResultPath](
            const FString& Name, const FString& Path)
        {
            bConfirmed = true;
            ResultName = Name;
            ResultPath = Path;
        });

    Dialog->ParentWindow = Window;
    Window->SetContent(Dialog);

    FSlateApplication::Get().AddModalWindow(Window, FSlateApplication::Get().GetActiveTopLevelWindow());

    if (bConfirmed)
    {
        OutAssetName = ResultName;
        OutPath = ResultPath;
    }

    return bConfirmed;
}
