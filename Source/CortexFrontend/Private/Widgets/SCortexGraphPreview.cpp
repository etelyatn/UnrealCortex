// SCortexGraphPreview.cpp
#include "Widgets/SCortexGraphPreview.h"

#include "CortexFrontendModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "GraphEditor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Styling/CoreStyle.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Input/STextComboBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"

void SCortexGraphPreview::Construct(const FArguments& InArgs)
{
    Context = InArgs._Context;

    if (!Context.IsValid()) return;

    for (const FString& Name : Context->Payload.GraphNames)
    {
        GraphNameOptions.Add(MakeShared<FString>(Name));
    }

    ChildSlot
    [
        SNew(SVerticalBox)

        // Graph dropdown
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(4)
        [
            SAssignNew(GraphDropdown, STextComboBox)
            .OptionsSource(&GraphNameOptions)
            .OnSelectionChanged(this, &SCortexGraphPreview::OnGraphSelected)
        ]

        // Graph editor container — SGraphEditor is recreated on graph switch
        // because SGraphEditor has no SetGraphToEdit() method (graph is a
        // construction argument only). This matches the SBlueprintDiff pattern.
        + SVerticalBox::Slot()
        .FillHeight(1.0f)
        [
            SAssignNew(GraphEditorContainer, SBox)
        ]
    ];

    // Set initial dropdown selection
    if (GraphNameOptions.Num() > 0 && !Context->Payload.CurrentGraphName.IsEmpty())
    {
        for (const TSharedPtr<FString>& Option : GraphNameOptions)
        {
            if (*Option == Context->Payload.CurrentGraphName)
            {
                GraphDropdown->SetSelectedItem(Option);
                break;
            }
        }
    }
}

void SCortexGraphPreview::SetInitialGraph(FName GraphName)
{
    if (!Context.IsValid())
    {
        return;
    }

    // Set active graph on context (this updates ActiveClonedGraph)
    Context->SetActiveGraph(GraphName);

    if (Context->ActiveClonedGraph)
    {
        RecreateGraphEditor(Context->ActiveClonedGraph);
    }
    else
    {
        UE_LOG(LogCortexFrontend, Warning,
            TEXT("Graph '%s' not found in clone map. Available: %d graphs"),
            *GraphName.ToString(), Context->ClonedGraphs.Num());

        // Show fallback message instead of empty canvas
        GraphEditorContainer->SetContent(
            SNew(SBox)
            .HAlign(HAlign_Center)
            .VAlign(VAlign_Center)
            [
                SNew(STextBlock)
                .Text(FText::FromString(FString::Printf(
                    TEXT("Graph preview not available for '%s'"), *GraphName.ToString())))
                .Font(FCoreStyle::GetDefaultFontStyle("Italic", 10))
                .ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.55f)))
            ]
        );
    }
}

void SCortexGraphPreview::RecreateGraphEditor(UEdGraph* Graph)
{
    if (!GraphEditorContainer.IsValid() || !Graph) return;

    FGraphAppearanceInfo AppearanceInfo;
    AppearanceInfo.CornerText = FText::GetEmpty();

    SGraphEditor::FGraphEditorEvents GraphEvents;
    GraphEvents.OnNodeDoubleClicked = FSingleNodeEvent::CreateLambda(
        [this](UEdGraphNode* ClonedNode)
        {
            if (!ClonedNode || !Context.IsValid()) return;
            if (GEditor && GEditor->IsPlaySessionInProgress()) return;

            // Find the real node in the source Blueprint by matching GUID
            const FString PkgName = FPackageName::ObjectPathToPackageName(
                Context->Payload.BlueprintPath);
            if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
            {
                return;
            }

            UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr,
                *Context->Payload.BlueprintPath);
            if (!Blueprint) return;

            TArray<UEdGraph*> AllGraphs;
            Blueprint->GetAllGraphs(AllGraphs);
            for (UEdGraph* Graph : AllGraphs)
            {
                for (UEdGraphNode* Node : Graph->Nodes)
                {
                    if (Node && Node->NodeGuid == ClonedNode->NodeGuid)
                    {
                        // Ensure Blueprint editor is open first (idempotent — returns existing if already open)
                        GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Blueprint);

                        // Then navigate to node
                        FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(Node);
                        return;
                    }
                }
            }
        });

    GraphEditorContainer->SetContent(
        SAssignNew(GraphEditorWidget, SGraphEditor)
        .GraphToEdit(Graph)
        .IsEditable(false)
        .Appearance(AppearanceInfo)
        .GraphEvents(GraphEvents)
    );
}

void SCortexGraphPreview::NavigateToNode(const FGuid& NodeGuid)
{
    if (!GraphEditorWidget.IsValid() || !Context.IsValid()) return;

    UEdGraph* ActiveGraph = Context->GetActiveClonedGraph();
    if (!ActiveGraph) return;

    UEdGraphNode* Node = FindNodeByGuid(ActiveGraph, NodeGuid);
    if (Node)
    {
        GraphEditorWidget->JumpToNode(Node, false);
    }
}

void SCortexGraphPreview::AnnotateNode(
    const FGuid& NodeGuid,
    ECortexFindingSeverity Severity,
    const FString& Message)
{
    if (!Context.IsValid()) return;

    // Only annotate nodes that were part of the analyzed scope. Lazily-cloned graphs
    // (added via SwitchToGraph for graphs outside the original serialization scope) have
    // the same node GUIDs as the source Blueprint but were never analyzed — annotating
    // them would show findings that don't correspond to anything the AI examined.
    if (!Context->AnalyzedNodeGuids.Contains(NodeGuid)) return;

    UEdGraph* ActiveGraph = Context->GetActiveClonedGraph();
    if (!ActiveGraph) return;

    UEdGraphNode* Node = FindNodeByGuid(ActiveGraph, NodeGuid);
    if (!Node) return;

    // No Modify() — these are RF_Transient clones that are never saved or undone
    Node->bHasCompilerMessage = true;
    Node->ErrorMsg = Message;

    switch (Severity)
    {
    case ECortexFindingSeverity::Critical:
        Node->ErrorType = EMessageSeverity::Error;
        break;
    case ECortexFindingSeverity::Warning:
        Node->ErrorType = EMessageSeverity::Warning;
        break;
    default:
        Node->ErrorType = EMessageSeverity::Info;
        break;
    }

    // RefreshNode forces the existing SGraphNode widget to reconstruct and re-read
    // bHasCompilerMessage/ErrorMsg. NotifyGraphChanged() only handles topology changes
    // (nodes added/removed) and does not update error state on existing node widgets.
    if (GraphEditorWidget.IsValid())
    {
        GraphEditorWidget->RefreshNode(*Node);
    }
}

void SCortexGraphPreview::ClearAnnotations()
{
    if (!Context.IsValid()) return;

    UEdGraph* ActiveGraph = Context->GetActiveClonedGraph();
    if (!ActiveGraph) return;

    for (UEdGraphNode* Node : ActiveGraph->Nodes)
    {
        if (Node)
        {
            Node->bHasCompilerMessage = false;
            Node->ErrorMsg.Empty();
        }
    }

    // Recreate the graph editor to force all SGraphNode widgets to reconstruct and
    // re-read the cleared error state. NotifyGraphChanged() only handles topology
    // changes and does not update error visuals on existing node widgets.
    RecreateGraphEditor(ActiveGraph);
}

void SCortexGraphPreview::OnGraphSelected(
    TSharedPtr<FString> NewSelection,
    ESelectInfo::Type SelectionType)
{
    if (!NewSelection.IsValid() || !Context.IsValid()) return;

    SwitchToGraph(FName(**NewSelection));
}

void SCortexGraphPreview::SwitchToGraph(FName GraphName)
{
    if (!Context.IsValid()) return;

    // Check if already cloned — reuse cached clone
    if (Context->ClonedGraphs.Contains(GraphName))
    {
        Context->SetActiveGraph(GraphName);
        RecreateGraphEditor(Context->GetActiveClonedGraph());
        return;
    }

    // On-demand cloning is only possible after analysis has started and the transient
    // Blueprint shell exists. During SCortexGraphPreview::Construct() (before OnAnalyzeClicked)
    // TempBlueprint is null — bail silently; the graph preview stays empty until analysis runs.
    // TempBlueprint is required as the clone outer so UK2Node_Event::FixupEventReference can
    // find a UBlueprint in the outer chain (FindBlueprintForNodeChecked asserts otherwise).
    if (!Context->TempPackage || !Context->TempBlueprint)
    {
        return;
    }

    // Load source Blueprint and clone the requested graph on demand
    const FString PkgName = FPackageName::ObjectPathToPackageName(Context->Payload.BlueprintPath);
    if (!FindPackage(nullptr, *PkgName) && !FPackageName::DoesPackageExist(PkgName))
    {
        return;
    }
    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Context->Payload.BlueprintPath);
    if (!Blueprint) return;

    TArray<UEdGraph*> AllGraphs;
    Blueprint->GetAllGraphs(AllGraphs);

    for (UEdGraph* SourceGraph : AllGraphs)
    {
        if (SourceGraph->GetFName() == GraphName)
        {
            UEdGraph* Clone = DuplicateObject<UEdGraph>(
                SourceGraph, Context->TempBlueprint, GraphName);
            Clone->SetFlags(RF_Transient);

            Context->ClonedGraphs.Add(GraphName, Clone);
            Context->SetActiveGraph(GraphName);
            RecreateGraphEditor(Clone);
            return;
        }
    }
}

UEdGraphNode* SCortexGraphPreview::FindNodeByGuid(UEdGraph* Graph, const FGuid& Guid) const
{
    if (!Graph) return nullptr;

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node && Node->NodeGuid == Guid)
        {
            return Node;
        }
    }
    return nullptr;
}
