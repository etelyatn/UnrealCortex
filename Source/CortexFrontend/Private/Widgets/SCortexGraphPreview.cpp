// SCortexGraphPreview.cpp
#include "Widgets/SCortexGraphPreview.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "GraphEditor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Input/STextComboBox.h"
#include "Widgets/Layout/SBox.h"
#include "Editor.h"

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

void SCortexGraphPreview::SetInitialGraph(UEdGraph* ClonedGraph)
{
    if (ClonedGraph)
    {
        RecreateGraphEditor(ClonedGraph);
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

    if (GraphEditorWidget.IsValid())
    {
        GraphEditorWidget->NotifyGraphChanged();
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

    if (GraphEditorWidget.IsValid())
    {
        GraphEditorWidget->NotifyGraphChanged();
    }
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

    // On-demand cloning: load source Blueprint and clone the requested graph
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
                SourceGraph, Context->TempPackage, GraphName);
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
