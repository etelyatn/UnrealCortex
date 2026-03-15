#include "Widgets/SCortexSidebar.h"

#include "CortexFrontendModule.h"
#include "CortexFrontendSettings.h"
#include "Session/CortexCliSession.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSegmentedControl.h"
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

	// Populate model dropdown options
	const TArray<FString> Models = FCortexFrontendSettings::Get().GetAvailableModels();
	const FString& Selected = FCortexFrontendSettings::Get().GetSelectedModel();
	for (const FString& Model : Models)
	{
		TSharedPtr<FString> Option = MakeShared<FString>(Model);
		ModelOptions.Add(Option);
		if (Model == Selected)
		{
			SelectedModelOption = Option;
		}
	}
	if (!SelectedModelOption.IsValid() && ModelOptions.Num() > 0)
	{
		SelectedModelOption = ModelOptions[0];
	}

	// Populate access mode dropdown
	AccessModeOptions.Add(MakeShared<FString>(TEXT("Read-Only")));
	AccessModeOptions.Add(MakeShared<FString>(TEXT("Guided")));
	AccessModeOptions.Add(MakeShared<FString>(TEXT("Full Access")));

	const FString CurrentMode = FCortexFrontendSettings::Get().GetAccessModeString();
	for (const TSharedPtr<FString>& Option : AccessModeOptions)
	{
		if (*Option == CurrentMode)
		{
			SelectedAccessModeOption = Option;
			break;
		}
	}
	if (!SelectedAccessModeOption.IsValid())
	{
		SelectedAccessModeOption = AccessModeOptions[0];
	}

	// Populate effort dropdown
	static const TArray<FString> EffortNames = {
		TEXT("Default"), TEXT("Low"), TEXT("Medium"), TEXT("High"), TEXT("Max")
	};
	static const TArray<ECortexEffortLevel> EffortValues = {
		ECortexEffortLevel::Default, ECortexEffortLevel::Low,
		ECortexEffortLevel::Medium, ECortexEffortLevel::High, ECortexEffortLevel::Maximum
	};
	const ECortexEffortLevel CurrentEffort = FCortexFrontendSettings::Get().GetEffortLevel();
	for (int32 i = 0; i < EffortNames.Num(); ++i)
	{
		TSharedPtr<FString> Option = MakeShared<FString>(EffortNames[i]);
		EffortOptions.Add(Option);
		if (EffortValues[i] == CurrentEffort)
		{
			SelectedEffortOption = Option;
		}
	}
	if (!SelectedEffortOption.IsValid() && EffortOptions.Num() > 0)
	{
		SelectedEffortOption = EffortOptions[0];
	}

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
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 4.0f, 8.0f, 2.0f)
					[
						SAssignNew(ProviderText, STextBlock)
						.Text(FText::FromString(TEXT("\u2014")))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
					]
					// Model label
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 4.0f, 8.0f, 1.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Model")))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
						.ColorAndOpacity(FSlateColor(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("888888")))))
					]
					// Model dropdown
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 0.0f, 8.0f, 2.0f)
					[
						SAssignNew(ModelComboBox, SComboBox<TSharedPtr<FString>>)
						.OptionsSource(&ModelOptions)
						.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Selection, ESelectInfo::Type)
						{
							if (Selection.IsValid())
							{
								SelectedModelOption = Selection;
								FCortexFrontendSettings::Get().SetSelectedModel(*Selection);
							}
						})
						.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) -> TSharedRef<SWidget>
						{
							return SNew(SBox)
								.Padding(FMargin(4.0f, 2.0f))
								[
									SNew(STextBlock)
									.Text(FText::FromString(*Item))
									.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
								];
						})
						.ToolTipText(FText::FromString(TEXT("AI model for this session. Larger models are more capable but slower.")))
						[
							SNew(STextBlock)
							.Text_Lambda([this]() -> FText
							{
								return SelectedModelOption.IsValid()
									? FText::FromString(*SelectedModelOption)
									: FText::FromString(TEXT("Default"));
							})
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						]
					]
					// Effort label
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 4.0f, 8.0f, 1.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Effort")))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
						.ColorAndOpacity(FSlateColor(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("888888")))))
						.ToolTipText(FText::FromString(TEXT("How much thinking the AI does. Default = model decides, Low = fast and brief, Max = thorough and detailed.")))
					]
					// Effort dropdown
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 0.0f, 8.0f, 2.0f)
					[
						SAssignNew(EffortComboBox, SComboBox<TSharedPtr<FString>>)
						.OptionsSource(&EffortOptions)
						.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Selection, ESelectInfo::Type)
						{
							if (Selection.IsValid())
							{
								SelectedEffortOption = Selection;
								const FString& Val = *Selection;
								ECortexEffortLevel Level = ECortexEffortLevel::Default;
								if (Val == TEXT("Low")) Level = ECortexEffortLevel::Low;
								else if (Val == TEXT("Medium")) Level = ECortexEffortLevel::Medium;
								else if (Val == TEXT("High")) Level = ECortexEffortLevel::High;
								else if (Val == TEXT("Max")) Level = ECortexEffortLevel::Maximum;
								FCortexFrontendSettings::Get().SetEffortLevel(Level);
							}
						})
						.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) -> TSharedRef<SWidget>
						{
							return SNew(SBox)
								.Padding(FMargin(4.0f, 2.0f))
								[
									SNew(STextBlock)
									.Text(FText::FromString(*Item))
									.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
								];
						})
						.ToolTipText(FText::FromString(TEXT("How much thinking the AI does. Default = model decides, Low = fast and brief, Max = thorough and detailed.")))
						[
							SNew(STextBlock)
							.Text_Lambda([this]() -> FText
							{
								return SelectedEffortOption.IsValid()
									? FText::FromString(*SelectedEffortOption)
									: FText::FromString(TEXT("Default"));
							})
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						]
					]
					// Access label
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 4.0f, 8.0f, 1.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Access")))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
						.ColorAndOpacity(FSlateColor(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("888888")))))
					]
					// Access dropdown
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 0.0f, 8.0f, 2.0f)
					[
						SAssignNew(AccessModeComboBox, SComboBox<TSharedPtr<FString>>)
						.OptionsSource(&AccessModeOptions)
						.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Selection, ESelectInfo::Type)
						{
							if (Selection.IsValid())
							{
								SelectedAccessModeOption = Selection;
								if (*Selection == TEXT("Read-Only"))
								{
									FCortexFrontendSettings::Get().SetAccessMode(ECortexAccessMode::ReadOnly);
								}
								else if (*Selection == TEXT("Guided"))
								{
									FCortexFrontendSettings::Get().SetAccessMode(ECortexAccessMode::Guided);
								}
								else if (*Selection == TEXT("Full Access"))
								{
									FCortexFrontendSettings::Get().SetAccessMode(ECortexAccessMode::FullAccess);
								}
							}
						})
						.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) -> TSharedRef<SWidget>
						{
							return SNew(SBox)
								.Padding(FMargin(4.0f, 2.0f))
								[
									SNew(STextBlock)
									.Text(FText::FromString(*Item))
									.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
								];
						})
						.ToolTipText(FText::FromString(TEXT("Controls which tools the AI can use. Read-Only = queries only, Guided = can create and edit, Full Access = unrestricted.")))
						[
							SNew(STextBlock)
							.Text_Lambda([this]() -> FText
							{
								return SelectedAccessModeOption.IsValid()
									? FText::FromString(*SelectedAccessModeOption)
									: FText::FromString(TEXT("Read-Only"));
							})
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						]
					]
					// Hint text
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 0.0f, 8.0f, 4.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Applied on new chat session")))
						.Font(FCoreStyle::GetDefaultFontStyle("Italic", 7))
						.ColorAndOpacity(FSlateColor(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("666666")))))
					]
					// Active model
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 2.0f, 8.0f, 4.0f)
					[
						SAssignNew(ModelText, STextBlock)
						.Text(FText::FromString(TEXT("Active: \u2014")))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
						.ColorAndOpacity(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("888888"))))
					]
				]
			]
			// Session section
			+ SScrollBox::Slot()
			[
				SNew(SExpandableArea)
				.AreaTitle(FText::FromString(TEXT("Session")))
				.InitiallyCollapsed(false)
				.BodyContent()
				[
					SNew(SVerticalBox)
					// Workflow mode label + toggle
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 4.0f, 8.0f, 1.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("Workflow")))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
						.ColorAndOpacity(FSlateColor(FLinearColor::FromSRGBColor(FColor::FromHex(TEXT("888888")))))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 0.0f, 8.0f, 4.0f)
					[
						SAssignNew(WorkflowToggle, SSegmentedControl<ECortexWorkflowMode>)
						.Value(FCortexFrontendSettings::Get().GetWorkflowMode())
						.OnValueChanged_Lambda([](ECortexWorkflowMode NewMode)
						{
							FCortexFrontendSettings::Get().SetWorkflowMode(NewMode);
						})
						.ToolTipText(FText::FromString(TEXT("Direct = act immediately, no planning workflows. Skills like /commit and /review-pr are unavailable. Thorough = full brainstorming, spec review, and planning with all skills.")))
						+ SSegmentedControl<ECortexWorkflowMode>::Slot(ECortexWorkflowMode::Direct)
						.Text(FText::FromString(TEXT("Direct")))
						+ SSegmentedControl<ECortexWorkflowMode>::Slot(ECortexWorkflowMode::Thorough)
						.Text(FText::FromString(TEXT("Thorough")))
					]
					// Project context checkbox
					+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 4.0f, 8.0f, 2.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 4.0f, 0.0f)
						[
							SAssignNew(ProjectContextCheckbox, SCheckBox)
							.IsChecked_Lambda([]() -> ECheckBoxState
							{
								return FCortexFrontendSettings::Get().GetProjectContext()
									? ECheckBoxState::Checked
									: ECheckBoxState::Unchecked;
							})
							.OnCheckStateChanged_Lambda([](ECheckBoxState NewState)
							{
								FCortexFrontendSettings::Get().SetProjectContext(
									NewState == ECheckBoxState::Checked);
							})
							.ToolTipText(FText::FromString(TEXT("Include project instructions (CLAUDE.md), settings, and tool permissions. Turning off may require re-approving MCP tools.")))
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(FText::FromString(TEXT("Project Context")))
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
							.ToolTipText(FText::FromString(TEXT("Include project instructions (CLAUDE.md), settings, and tool permissions. Turning off may require re-approving MCP tools.")))
						]
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
		const FString ActiveLabel = Model.IsEmpty()
			? TEXT("Active: \u2014")
			: FString::Printf(TEXT("Active: %s"), *Model);
		ModelText->SetText(FText::FromString(ActiveLabel));
	}
}
