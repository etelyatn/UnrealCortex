#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Animation/WidgetAnimation.h"
#include "Components/Image.h"
#include "Components/Overlay.h"

#include "CortexUMGBindWidgetTestTypes.generated.h"

/**
 * Test-only native base declaring every BindWidget / BindWidgetAnim member kind that
 * umg.check_native_bindings classifies: matched, wrong type, missing (with a near-miss name),
 * optional, a transient animation and an animation that lacks the Transient flag the widget
 * compiler requires.
 */
UCLASS(Abstract)
class UCortexUMGBindWidgetTestBase : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UImage> MatchedImage;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UOverlay> WrongType;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UImage> MissingImag;

	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UImage> OptionalImage;

	UPROPERTY(Transient, meta = (BindWidgetAnim))
	TObjectPtr<UWidgetAnimation> FadeIn;

	UPROPERTY(meta = (BindWidgetAnim))
	TObjectPtr<UWidgetAnimation> NotTransientAnim;
};

/**
 * Test-only native base for the "X is not blueprint visible" check: one BindWidget member a graph can
 * read, one it cannot (no Blueprint exposure), and one that is never read.
 */
UCLASS(Abstract)
class UCortexUMGBindWidgetVisibilityBase : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UImage> HiddenImage;

	UPROPERTY(BlueprintReadOnly, Category = "CortexTest", meta = (BindWidget))
	TObjectPtr<UImage> VisibleImage;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UImage> UnreadImage;
};
