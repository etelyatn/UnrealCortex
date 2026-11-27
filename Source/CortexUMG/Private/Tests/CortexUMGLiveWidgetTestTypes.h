#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"

#include "CortexUMGLiveWidgetTestTypes.generated.h"

/**
 * Test-only native UserWidget for the live-widget readback tests. Initialize builds a runtime widget tree:
 * a VerticalBox "Root" holding a TextBlock "Title" (text "Hello", bIsVariable = false) and an Image "Icon".
 * It exposes one plain property (Score) and one widget reference (TitleRef) that the readback must omit.
 */
UCLASS()
class UCortexUMGLiveWidgetTestWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY()
	int32 Score = 42;

	UPROPERTY()
	TObjectPtr<UTextBlock> TitleRef;

	/** Returned by the Title text binding of the bound-text variant. */
	UFUNCTION()
	FText GetBoundTitle() const
	{
		return FText::FromString(TEXT("Bound"));
	}

	virtual bool Initialize() override
	{
		const bool bSuperInitialized = Super::Initialize();
		if (bSuperInitialized && WidgetTree && !WidgetTree->RootWidget)
		{
			UVerticalBox* Root = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Root"));
			WidgetTree->RootWidget = Root;

			TitleRef = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Title"));
			TitleRef->bIsVariable = false;
			TitleRef->SetText(FText::FromString(TEXT("Hello")));
			if (bBindTitleText)
			{
				// After SetText, which clears any binding.
				TitleRef->TextDelegate.BindUFunction(this, GET_FUNCTION_NAME_CHECKED(UCortexUMGLiveWidgetTestWidget, GetBoundTitle));
			}
			Root->AddChild(TitleRef);

			UImage* Icon = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("Icon"));
			Root->AddChild(Icon);
		}
		return bSuperInitialized;
	}

protected:
	bool bBindTitleText = false;
};

/** Variant whose Title text is bound through a property delegate (TextDelegate). */
UCLASS()
class UCortexUMGLiveBoundTextTestWidget : public UCortexUMGLiveWidgetTestWidget
{
	GENERATED_BODY()

public:
	UCortexUMGLiveBoundTextTestWidget(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		bBindTitleText = true;
	}
};
