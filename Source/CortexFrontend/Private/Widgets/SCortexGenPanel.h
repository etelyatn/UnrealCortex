#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/CortexGenSessionTypes.h"

class SCortexGenTabButton;
class SWidgetSwitcher;
class SHorizontalBox;

class SCortexGenPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexGenPanel) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    void AddSession(ECortexGenSessionType Type);
    void RemoveSession(int32 Index);
    void SetActiveSession(int32 Index);
    int32 GetSessionCount() const { return Sessions.Num(); }

    /** Check if any session has active jobs. Used for close confirmation. */
    bool HasActiveJobs() const;

private:
    void RebuildTabBar();
    void OnTabCloseRequested(int32 Index);
    FText GenerateSessionName(ECortexGenSessionType Type) const;
    TSharedRef<SWidget> CreateSessionWidget(const FCortexGenSessionModel& Session);

    TArray<FCortexGenSessionModel> Sessions;
    TArray<TSharedPtr<SCortexGenTabButton>> TabButtons;
    TSharedPtr<SHorizontalBox> TabBar;
    TSharedPtr<SWidgetSwitcher> ContentSwitcher;
    int32 ActiveIndex = -1;

    int32 NextImageNumber = 1;
    int32 NextMeshNumber = 1;
};
