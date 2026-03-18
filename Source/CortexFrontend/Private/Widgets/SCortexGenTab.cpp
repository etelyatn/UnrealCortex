#include "Widgets/SCortexGenTab.h"
#include "CortexFrontendModule.h"
#include "CortexCoreModule.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/STextComboBox.h"
#include "Widgets/Text/STextBlock.h"

void SCortexGenTab::Construct(const FArguments& InArgs)
{
	// Subscribe to domain progress
	if (FModuleManager::Get().IsModuleLoaded(TEXT("CortexCore")))
	{
		FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
		DomainProgressHandle = Core.OnDomainProgress().AddSP(this, &SCortexGenTab::OnDomainProgress);
	}

	// Generation type options: [0]=Image (cheap), [1]=3D Mesh
	GenerationTypeOptions.Add(MakeShared<FString>(TEXT("Image (fast/cheap)")));
	GenerationTypeOptions.Add(MakeShared<FString>(TEXT("3D Mesh")));

	// Quality options
	QualityOptions.Add(MakeShared<FString>(TEXT("extra-low")));
	QualityOptions.Add(MakeShared<FString>(TEXT("low")));
	QualityOptions.Add(MakeShared<FString>(TEXT("medium")));
	QualityOptions.Add(MakeShared<FString>(TEXT("high")));

	ChildSlot
	[
		SNew(SVerticalBox)

		// Title
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.f, 8.f, 8.f, 4.f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("Generate Asset")))
		]

		// Generation type selector
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.f, 4.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Type:")))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SAssignNew(GenerationTypeCombo, STextComboBox)
				.OptionsSource(&GenerationTypeOptions)
				.InitiallySelectedItem(GenerationTypeOptions[1]) // Default: 3D Mesh
			]
		]

		// Quality selector
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.f, 4.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Quality:")))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SAssignNew(QualityCombo, STextComboBox)
				.OptionsSource(&QualityOptions)
				.InitiallySelectedItem(QualityOptions[2]) // Default: medium
				.OnSelectionChanged(this, &SCortexGenTab::OnQualityChanged)
			]
		]

		// Prompt input
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.f, 4.f)
		[
			SNew(SBox)
			.HeightOverride(80.0f)
			[
				SAssignNew(PromptBox, SMultiLineEditableTextBox)
				.HintText(FText::FromString(TEXT("Describe the asset to generate...")))
			]
		]

		// Generate button
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.f, 4.f)
		[
			SNew(SButton)
			.Text(FText::FromString(TEXT("Generate")))
			.OnClicked(FOnClicked::CreateSP(this, &SCortexGenTab::OnGenerateClicked))
		]

		// Status text
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8.f, 4.f)
		[
			SAssignNew(StatusText, STextBlock)
			.Text(FText::FromString(TEXT("Ready")))
		]
	];
}

SCortexGenTab::~SCortexGenTab()
{
	if (DomainProgressHandle.IsValid())
	{
		if (FModuleManager::Get().IsModuleLoaded(TEXT("CortexCore")))
		{
			FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
			Core.OnDomainProgress().Remove(DomainProgressHandle);
		}
	}
}

void SCortexGenTab::OnDomainProgress(const FName& DomainName, const TSharedPtr<FJsonObject>& Data)
{
	if (DomainName != FName(TEXT("gen")) || !Data.IsValid())
	{
		return;
	}

	FString Status;
	Data->TryGetStringField(TEXT("status"), Status);

	FString Error;
	Data->TryGetStringField(TEXT("error"), Error);

	if (StatusText.IsValid())
	{
		if (!Error.IsEmpty())
		{
			StatusText->SetText(FText::FromString(FString::Printf(TEXT("%s — %s"), *Status, *Error)));
		}
		else if (!Status.IsEmpty())
		{
			StatusText->SetText(FText::FromString(Status));
		}
	}
}

void SCortexGenTab::OnQualityChanged(TSharedPtr<FString> NewValue, ESelectInfo::Type SelectInfo)
{
	if (NewValue.IsValid())
	{
		SelectedQuality = *NewValue;
	}
}

FReply SCortexGenTab::OnGenerateClicked()
{
	if (!PromptBox.IsValid())
	{
		return FReply::Handled();
	}

	const FString Prompt = PromptBox->GetText().ToString();
	if (Prompt.IsEmpty())
	{
		return FReply::Handled();
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("prompt"), Prompt);
	Params->SetStringField(TEXT("quality"), SelectedQuality);

	// Generation type options are ordered: [0]=Image, [1]=3D Mesh.
	// Compare against the known option pointer, not display text.
	const bool bIsImage = GenerationTypeCombo.IsValid()
		&& GenerationTypeCombo->GetSelectedItem() == GenerationTypeOptions[0];
	FString Command = bIsImage ? TEXT("start_image") : TEXT("start_mesh");

	ExecuteGenCommand(Command, Params);

	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::FromString(TEXT("Submitting...")));
	}

	return FReply::Handled();
}

void SCortexGenTab::ExecuteGenCommand(const FString& Command, TSharedPtr<FJsonObject> Params)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("CortexCore")))
	{
		return;
	}

	FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	Core.GetCommandRouter().Execute(
		FString::Printf(TEXT("gen.%s"), *Command),
		Params);
}
