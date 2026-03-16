#include "Widgets/SCortexConversionConfig.h"

#include "Styling/AppStyle.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

void SCortexConversionConfig::Construct(const FArguments& InArgs)
{
	Context = InArgs._Context;
	OnConvert = InArgs._OnConvert;

	if (!Context.IsValid())
	{
		return;
	}

	const FCortexConversionPayload& Payload = Context->Payload;
	const bool bHasSelectedNodes = Payload.SelectedNodeIds.Num() > 0;

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot()
		.Padding(16.0f)
		[
			SNew(SVerticalBox)

			// Blueprint info header
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 16)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("Blueprint: %s"), *Payload.BlueprintName)))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 4, 0, 0)
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("Path: %s"), *Payload.BlueprintPath)))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 4, 0, 0)
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("Parent Class: %s"), *Payload.ParentClassName)))
				]
			]

			// Scope selector section
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 16)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 0, 0, 8)
				[
					SNew(STextBlock)
					.Text(NSLOCTEXT("CortexConversion", "ScopeLabel", "Conversion Scope"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
				]

				// Entire Blueprint
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 2)
				[
					SNew(SCheckBox)
					.Style(FAppStyle::Get(), "RadioButton")
					.IsChecked_Lambda([this]()
					{
						return IsScopeSelected(ECortexConversionScope::EntireBlueprint)
							? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
					{
						if (State == ECheckBoxState::Checked)
						{
							OnScopeChanged(ECortexConversionScope::EntireBlueprint);
						}
					})
					[
						SNew(STextBlock)
						.Text(NSLOCTEXT("CortexConversion", "ScopeEntire", "Entire Blueprint"))
					]
				]

				// Selected Nodes
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 2)
				[
					SNew(SCheckBox)
					.Style(FAppStyle::Get(), "RadioButton")
					.IsEnabled(bHasSelectedNodes)
					.IsChecked_Lambda([this]()
					{
						return IsScopeSelected(ECortexConversionScope::SelectedNodes)
							? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
					{
						if (State == ECheckBoxState::Checked)
						{
							OnScopeChanged(ECortexConversionScope::SelectedNodes);
						}
					})
					[
						SNew(STextBlock)
						.Text(FText::FromString(FString::Printf(TEXT("Selected Nodes (%d selected)"),
							Payload.SelectedNodeIds.Num())))
						.IsEnabled(bHasSelectedNodes)
					]
				]

				// Current Graph
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 2)
				[
					SNew(SCheckBox)
					.Style(FAppStyle::Get(), "RadioButton")
					.IsChecked_Lambda([this]()
					{
						return IsScopeSelected(ECortexConversionScope::CurrentGraph)
							? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
					{
						if (State == ECheckBoxState::Checked)
						{
							OnScopeChanged(ECortexConversionScope::CurrentGraph);
						}
					})
					[
						SNew(STextBlock)
						.Text(FText::FromString(FString::Printf(TEXT("Current Graph (%s)"),
							*Payload.CurrentGraphName)))
					]
				]
			]

			// Events and Functions list
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 16)
			[
				BuildEventFunctionList(Payload)
			]

			// Convert button
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 16, 0, 0)
			[
				SNew(SButton)
				.Text(NSLOCTEXT("CortexConversion", "Convert", "Convert"))
				.OnClicked(this, &SCortexConversionConfig::OnConvertButtonClicked)
				.HAlign(HAlign_Center)
			]
		]
	];
}

TSharedRef<SWidget> SCortexConversionConfig::BuildEventFunctionList(const FCortexConversionPayload& Payload)
{
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

	if (Payload.EventNames.Num() == 0 && Payload.FunctionNames.Num() == 0)
	{
		return Box;
	}

	Box->AddSlot()
	.AutoHeight()
	.Padding(0, 0, 0, 4)
	[
		SNew(STextBlock)
		.Text(NSLOCTEXT("CortexConversion", "EventFunctionLabel", "Event / Function"))
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
	];

	for (const FString& EventName : Payload.EventNames)
	{
		Box->AddSlot()
		.AutoHeight()
		.Padding(0, 2)
		[
			SNew(SCheckBox)
			.Style(FAppStyle::Get(), "RadioButton")
			.IsChecked_Lambda([this, EventName]()
			{
				return IsEventOrFunctionSelected(EventName)
					? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([this, EventName](ECheckBoxState State)
			{
				if (State == ECheckBoxState::Checked)
				{
					OnEventOrFunctionSelected(EventName);
				}
			})
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("Event: %s"), *EventName)))
			]
		];
	}

	for (const FString& FuncName : Payload.FunctionNames)
	{
		Box->AddSlot()
		.AutoHeight()
		.Padding(0, 2)
		[
			SNew(SCheckBox)
			.Style(FAppStyle::Get(), "RadioButton")
			.IsChecked_Lambda([this, FuncName]()
			{
				return IsEventOrFunctionSelected(FuncName)
					? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([this, FuncName](ECheckBoxState State)
			{
				if (State == ECheckBoxState::Checked)
				{
					OnEventOrFunctionSelected(FuncName);
				}
			})
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("Function: %s"), *FuncName)))
			]
		];
	}

	return Box;
}

bool SCortexConversionConfig::IsScopeSelected(ECortexConversionScope Scope) const
{
	return Context.IsValid() && Context->SelectedScope == Scope;
}

bool SCortexConversionConfig::IsEventOrFunctionSelected(const FString& Name) const
{
	return Context.IsValid()
		&& Context->SelectedScope == ECortexConversionScope::EventOrFunction
		&& Context->TargetEventOrFunction == Name;
}

void SCortexConversionConfig::OnScopeChanged(ECortexConversionScope NewScope)
{
	if (Context.IsValid())
	{
		Context->SelectedScope = NewScope;
		Context->TargetEventOrFunction.Empty();
	}
}

void SCortexConversionConfig::OnEventOrFunctionSelected(const FString& Name)
{
	if (Context.IsValid())
	{
		Context->SelectedScope = ECortexConversionScope::EventOrFunction;
		Context->TargetEventOrFunction = Name;
	}
}

FReply SCortexConversionConfig::OnConvertButtonClicked()
{
	OnConvert.ExecuteIfBound();
	return FReply::Handled();
}
