#include "Widgets/SCortexGenTab.h"
#include "CortexFrontendModule.h"
#include "CortexCoreModule.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"

void SCortexGenTab::Construct(const FArguments& InArgs)
{
	// Subscribe to domain progress
	if (FModuleManager::Get().IsModuleLoaded(TEXT("CortexCore")))
	{
		FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
		DomainProgressHandle = Core.OnDomainProgress().AddSP(this, &SCortexGenTab::OnDomainProgress);
	}

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

	if (StatusText.IsValid() && !Status.IsEmpty())
	{
		StatusText->SetText(FText::FromString(Status));
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

	ExecuteGenCommand(TEXT("start_mesh"), Params);

	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::FromString(TEXT("Submitting...")));
	}

	return FReply::Handled();
}

void SCortexGenTab::OnCancelClicked(const FString& JobId)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("job_id"), JobId);
	ExecuteGenCommand(TEXT("cancel_job"), Params);
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

void SCortexGenTab::RefreshProviders()
{
	ExecuteGenCommand(TEXT("list_providers"), MakeShared<FJsonObject>());
}

void SCortexGenTab::RefreshJobs()
{
	ExecuteGenCommand(TEXT("list_jobs"), MakeShared<FJsonObject>());
}
