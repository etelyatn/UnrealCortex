// SCortexGraphPreview.h
#pragma once

#include "CoreMinimal.h"
#include "Analysis/CortexAnalysisContext.h"
#include "Analysis/CortexFindingTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SGraphEditor;
class STextComboBox;

class SCortexGraphPreview : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCortexGraphPreview) {}
        SLATE_ARGUMENT(TSharedPtr<FCortexAnalysisContext>, Context)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    /** Navigate to a specific node in the preview (finding click). */
    void NavigateToNode(const FGuid& NodeGuid);

    /** Annotate a cloned node with finding severity (colored border + message). */
    void AnnotateNode(const FGuid& NodeGuid, ECortexFindingSeverity Severity, const FString& Message);

    /** Clear all annotations from the active graph. */
    void ClearAnnotations();

    /** Set the active cloned graph for initial display. */
    void SetInitialGraph(UEdGraph* ClonedGraph);

private:
    void OnGraphSelected(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectionType);
    void SwitchToGraph(FName GraphName);
    void RecreateGraphEditor(UEdGraph* Graph);
    UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FGuid& Guid) const;

    TSharedPtr<FCortexAnalysisContext> Context;
    TSharedPtr<SGraphEditor> GraphEditorWidget;
    TSharedPtr<SBox> GraphEditorContainer;
    TArray<TSharedPtr<FString>> GraphNameOptions;
    TSharedPtr<STextComboBox> GraphDropdown;
};
