#include "Widgets/SCortexConversionConfig.h"

#include "Widgets/Input/SButton.h"
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
					SNew(SButton)
					.Text(NSLOCTEXT("CortexConversion", "ScopeEntire", "Entire Blueprint"))
					.OnClicked_Lambda([this]()
					{
						OnScopeChanged(ECortexConversionScope::EntireBlueprint);
						return FReply::Handled();
					})
				]

				// Selected Nodes
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 2)
				[
					SNew(SButton)
					.Text(FText::FromString(FString::Printf(TEXT("Selected Nodes (%d selected)"),
						Payload.SelectedNodeIds.Num())))
					.IsEnabled(bHasSelectedNodes)
					.OnClicked_Lambda([this]()
					{
						OnScopeChanged(ECortexConversionScope::SelectedNodes);
						return FReply::Handled();
					})
				]

				// Current Graph
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0, 2)
				[
					SNew(SButton)
					.Text(FText::FromString(FString::Printf(TEXT("Current Graph (%s)"),
						*Payload.CurrentGraphName)))
					.OnClicked_Lambda([this]()
					{
						OnScopeChanged(ECortexConversionScope::CurrentGraph);
						return FReply::Handled();
					})
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
			SNew(SButton)
			.Text(FText::FromString(FString::Printf(TEXT("Event: %s"), *EventName)))
			.OnClicked_Lambda([this, EventName]()
			{
				if (Context.IsValid())
				{
					Context->SelectedScope = ECortexConversionScope::EventOrFunction;
					Context->TargetEventOrFunction = EventName;
				}
				return FReply::Handled();
			})
		];
	}

	for (const FString& FuncName : Payload.FunctionNames)
	{
		Box->AddSlot()
		.AutoHeight()
		.Padding(0, 2)
		[
			SNew(SButton)
			.Text(FText::FromString(FString::Printf(TEXT("Function: %s"), *FuncName)))
			.OnClicked_Lambda([this, FuncName]()
			{
				if (Context.IsValid())
				{
					Context->SelectedScope = ECortexConversionScope::EventOrFunction;
					Context->TargetEventOrFunction = FuncName;
				}
				return FReply::Handled();
			})
		];
	}

	return Box;
}

void SCortexConversionConfig::OnScopeChanged(ECortexConversionScope NewScope)
{
	if (Context.IsValid())
	{
		Context->SelectedScope = NewScope;
	}
}

FReply SCortexConversionConfig::OnConvertButtonClicked()
{
	OnConvert.ExecuteIfBound();
	return FReply::Handled();
}
