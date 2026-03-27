#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/CortexGenSessionTypes.h"

DECLARE_DELEGATE(FOnCortexGenTabClose);

class SCircularThrobber;
class SImage;
class STextBlock;

class SCortexGenTabButton : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCortexGenTabButton)
		: _IsActive(false)
	{}
		SLATE_ATTRIBUTE(FText, DisplayName)
		SLATE_ATTRIBUTE(bool, IsActive)
		SLATE_EVENT(FOnClicked, OnClicked)
		SLATE_EVENT(FOnCortexGenTabClose, OnCloseClicked)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	void SetStatus(ECortexGenSessionStatus NewStatus);
	void SetDisplayName(const FText& Name);

private:
	TSharedPtr<STextBlock> NameLabel;
	TSharedPtr<SCircularThrobber> Throbber;
	TSharedPtr<SImage> StatusIcon;
	ECortexGenSessionStatus CurrentStatus = ECortexGenSessionStatus::Idle;
};
