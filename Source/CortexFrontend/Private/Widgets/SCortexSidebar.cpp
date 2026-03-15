#include "Widgets/SCortexSidebar.h"

#include "CortexFrontendModule.h"
#include "CortexFrontendSettings.h"
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

		TokenUsageHandle = Session->OnTokenUsageUpdated.AddLambda([SidebarWeak]()
		{
			if (TSharedPtr<SCortexSidebar> Pinned = SidebarWeak.Pin())
			{
				Pinned->OnTokenUsageUpdated();
			}
		});

		StateChangedHandle = Session->OnStateChanged.AddLambda([SidebarWeak](const FCortexSessionStateChange& Change)
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
				SAssignNew(CollapseButtonText, STextBlock)
				.Text(FText::FromString(TEXT("\u25C0")))
				.ColorAndOpacity(FSlateColor(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("888888")))))
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
						.ColorAndOpacity(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("888888"))))
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
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(4.0f, 2.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(FText::FromString(TEXT("Access: ")))
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
							.ColorAndOpacity(FSlateColor(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("888888")))))
						]
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text_Lambda([]()
							{
								return FText::FromString(FCortexFrontendSettings::Get().GetAccessModeString());
							})
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
							.ColorAndOpacity(FSlateColor(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("cccccc")))))
						]
					]
				]
			]
		]
	];

	// Initial update
	UpdateModelDisplay();
}

SCortexSidebar::~SCortexSidebar()
{
	if (TSharedPtr<FCortexCliSession> Session = SessionWeak.Pin())
	{
		Session->OnTokenUsageUpdated.Remove(TokenUsageHandle);
		Session->OnStateChanged.Remove(StateChangedHandle);
	}
}

void SCortexSidebar::SetCollapsed(bool bCollapsed)
{
	if (CollapseButtonText.IsValid())
	{
		CollapseButtonText->SetText(FText::FromString(bCollapsed ? TEXT("\u25B6") : TEXT("\u25C0")));
	}
}

void SCortexSidebar::OnTokenUsageUpdated()
{
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
