#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/Image.h"

#include "CortexUMGRenameDependentsTestTypes.generated.h"

/** Test-only native base with two required BindWidget members of different types. */
UCLASS(Abstract)
class UCortexUMGRenameBindWidgetBase : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UButton> NativeButton;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UImage> NativeImage;
};

/** Test-only native base with one optional BindWidget member. */
UCLASS(Abstract)
class UCortexUMGRenameOptionalBindWidgetBase : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (BindWidgetOptional))
	TObjectPtr<UButton> OptionalButton;
};
