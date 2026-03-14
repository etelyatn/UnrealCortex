#include "Widgets/SCortexSidebar.h"

#include "CortexFrontendModule.h"
#include "Session/CortexCliSession.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SBoxPanel.h"

void SCortexSidebar::Construct(const FArguments& InArgs)
{
	SessionWeak = InArgs._Session;
	OnCollapse = InArgs._OnCollapse;

	// Subscribe to session events using weak lambda (SWidget doesn't support AddSP directly)
	if (TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin())
	{
		TWeakPtr<SCortexSidebar> SidebarWeak = SharedThis(this);

		Session->OnTokenUsageUpdated.AddLambda([SidebarWeak]()
		{
			if (TSharedPtr<SCortexSidebar> Pinned = SidebarWeak.Pin())
			{
				Pinned->OnTokenUsageUpdated();
			}
		});

		Session->OnStateChanged.AddLambda([SidebarWeak](const FCortexSessionStateChange& Change)
		{
			if (TSharedPtr<SCortexSidebar> Pinned = SidebarWeak.Pin())
			{
				Pinned->OnSessionStateChanged(Change);
			}
		});
	}

	ChildSlot
	[
		SNew(SVerticalBox)
		// Collapse button row
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Right)
		[
			SNew(SButton)
			.ButtonStyle(FCoreStyle::Get(), "NoBorder")
			.OnClicked_Lambda([this]() -> FReply
			{
				OnCollapse.ExecuteIfBound();
				return FReply::Handled();
			})
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("\u25C0")))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
			]
		]
		// Scrollable content
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SScrollBox)
			// Header
			+ SScrollBox::Slot()
			.Padding(8.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("CORTEX")))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
			]
			// AI Model section
			+ SScrollBox::Slot()
			[
				SNew(SExpandableArea)
				.AreaTitle(FText::FromString(TEXT("AI Model")))
				.InitiallyCollapsed(false)
				.BodyContent()
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f)
					[
						SAssignNew(ProviderText, STextBlock)
						.Text(FText::FromString(TEXT("\u2014")))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f)
					[
						SAssignNew(ModelText, STextBlock)
						.Text(FText::FromString(TEXT("\u2014")))
						.ColorAndOpacity(FLinearColor(0.6f, 0.6f, 0.6f))
					]
				]
			]
			// Connection section
			+ SScrollBox::Slot()
			[
				SNew(SExpandableArea)
				.AreaTitle(FText::FromString(TEXT("Connection")))
				.InitiallyCollapsed(false)
				.BodyContent()
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f)
					[
						SAssignNew(StateText, STextBlock)
						.Text(FText::FromString(TEXT("Inactive")))
					]
				]
			]
			// Tokens section
			+ SScrollBox::Slot()
			[
				SNew(SExpandableArea)
				.AreaTitle(FText::FromString(TEXT("Tokens")))
				.InitiallyCollapsed(false)
				.BodyContent()
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f)
						[ SNew(STextBlock).Text(FText::FromString(TEXT("Input:"))) ]
						+ SHorizontalBox::Slot().AutoWidth()
						[ SAssignNew(InputTokensText, STextBlock).Text(FText::FromString(TEXT("0"))) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f)
						[ SNew(STextBlock).Text(FText::FromString(TEXT("Output:"))) ]
						+ SHorizontalBox::Slot().AutoWidth()
						[ SAssignNew(OutputTokensText, STextBlock).Text(FText::FromString(TEXT("0"))) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f)
						[ SNew(STextBlock).Text(FText::FromString(TEXT("Cache:"))) ]
						+ SHorizontalBox::Slot().AutoWidth()
						[ SAssignNew(CacheTokensText, STextBlock).Text(FText::FromString(TEXT("0"))) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f)
						[ SNew(STextBlock).Text(FText::FromString(TEXT("Hit Rate:"))) ]
						+ SHorizontalBox::Slot().AutoWidth()
						[ SAssignNew(CacheHitRateText, STextBlock).Text(FText::FromString(TEXT("0%"))) ]
					]
				]
			]
		]
	];

	// Initial update
	UpdateModelDisplay();
}

void SCortexSidebar::SetCollapsed(bool /*bCollapsed*/)
{
	// Visual collapse is driven by SCortexWorkbench via GetSidebarWidth()
}

void SCortexSidebar::OnTokenUsageUpdated()
{
	UpdateTokenDisplay();
	UpdateModelDisplay();
}

void SCortexSidebar::OnSessionStateChanged(const FCortexSessionStateChange& Change)
{
	if (!StateText.IsValid())
	{
		return;
	}

	auto StateToString = [](ECortexSessionState S) -> FString
	{
		switch (S)
		{
		case ECortexSessionState::Inactive:    return TEXT("Inactive");
		case ECortexSessionState::Spawning:    return TEXT("Spawning...");
		case ECortexSessionState::Idle:        return TEXT("Idle");
		case ECortexSessionState::Processing:  return TEXT("Processing...");
		case ECortexSessionState::Cancelling:  return TEXT("Cancelling...");
		case ECortexSessionState::Respawning:  return TEXT("Reconnecting...");
		case ECortexSessionState::Terminated:  return TEXT("Terminated");
		default:                               return TEXT("Unknown");
		}
	};

	StateText->SetText(FText::FromString(StateToString(Change.NewState)));
}

void SCortexSidebar::UpdateTokenDisplay()
{
	const TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin();
	if (!Session.IsValid())
	{
		return;
	}

	if (InputTokensText.IsValid())
	{
		InputTokensText->SetText(FText::FromString(FString::Printf(TEXT("%lld"), Session->GetTotalInputTokens())));
	}
	if (OutputTokensText.IsValid())
	{
		OutputTokensText->SetText(FText::FromString(FString::Printf(TEXT("%lld"), Session->GetTotalOutputTokens())));
	}
	if (CacheTokensText.IsValid())
	{
		CacheTokensText->SetText(FText::FromString(FString::Printf(TEXT("%lld"), Session->GetTotalCacheReadTokens())));
	}
	if (CacheHitRateText.IsValid())
	{
		const float Rate = FCortexCliSession::CalculateCacheHitRate(Session->GetTotalCacheReadTokens(), Session->GetTotalInputTokens());
		CacheHitRateText->SetText(FText::FromString(FString::Printf(TEXT("%.1f%%"), Rate)));
	}
}

void SCortexSidebar::UpdateModelDisplay()
{
	const TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin();
	if (!Session.IsValid())
	{
		return;
	}

	const FString& Provider = Session->GetProvider();
	const FString& Model = Session->GetModelId();

	if (ProviderText.IsValid())
	{
		ProviderText->SetText(FText::FromString(Provider.IsEmpty() ? TEXT("\u2014") : Provider));
	}
	if (ModelText.IsValid())
	{
		ModelText->SetText(FText::FromString(Model.IsEmpty() ? TEXT("\u2014") : Model));
	}
}
