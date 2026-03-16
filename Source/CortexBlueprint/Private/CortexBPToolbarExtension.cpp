#include "CortexBPToolbarExtension.h"

#include "BlueprintEditor.h"
#include "CortexConversionTypes.h"
#include "CortexCoreModule.h"
#include "CortexBlueprintModule.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "Operations/CortexProjectClassDetector.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Toolkits/AssetEditorToolkitMenuContext.h"
#include "Framework/Application/SlateApplication.h"

void FCortexBPToolbarExtension::Register()
{
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateLambda([]()
		{
			UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu(
				TEXT("AssetEditor.BlueprintEditor.ToolBar"));

			FToolMenuSection& Section = ToolbarMenu->FindOrAddSection(TEXT("Cortex"));

			Section.AddEntry(FToolMenuEntry::InitComboButton(
				TEXT("CortexBPTools"),
				FUIAction(),
				FNewToolMenuDelegate::CreateStatic(&FCortexBPToolbarExtension::BuildMenu),
				NSLOCTEXT("CortexBlueprint", "CortexToolbar", "Cortex"),
				NSLOCTEXT("CortexBlueprint", "CortexToolbarTooltip", "Cortex AI tools for this Blueprint"),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details")
			));
		}));
}

void FCortexBPToolbarExtension::Unregister()
{
	if (UToolMenus* ToolMenus = UToolMenus::Get())
	{
		ToolMenus->UnregisterOwner(TEXT("CortexBlueprint"));
	}
}

void FCortexBPToolbarExtension::BuildMenu(UToolMenu* Menu)
{
	// Resolve the FBlueprintEditor from the menu context
	FToolMenuContext& MenuContext = Menu->Context;
	UObject* ContextObject = MenuContext.FindByClass(UAssetEditorToolkitMenuContext::StaticClass());
	UAssetEditorToolkitMenuContext* AssetEditorContext = Cast<UAssetEditorToolkitMenuContext>(ContextObject);

	TWeakPtr<FBlueprintEditor> WeakEditor;
	if (AssetEditorContext && AssetEditorContext->Toolkit.IsValid())
	{
		TSharedPtr<FAssetEditorToolkit> Toolkit = AssetEditorContext->Toolkit.Pin();
		WeakEditor = StaticCastSharedPtr<FBlueprintEditor>(Toolkit);
	}

	FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("CortexActions"));

	Section.AddMenuEntry(
		TEXT("OpenFrontend"),
		NSLOCTEXT("CortexBlueprint", "OpenFrontend", "Open Cortex Frontend"),
		NSLOCTEXT("CortexBlueprint", "OpenFrontendTip", "Opens the Cortex Frontend panel"),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&FCortexBPToolbarExtension::OnOpenFrontendClicked)));

	Section.AddMenuEntry(
		TEXT("ConvertBP"),
		NSLOCTEXT("CortexBlueprint", "ConvertBP", "Convert BP to C++"),
		NSLOCTEXT("CortexBlueprint", "ConvertBPTip", "Convert this Blueprint to C++ code"),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateStatic(&FCortexBPToolbarExtension::OnConvertBPClicked, WeakEditor),
			FCanExecuteAction::CreateLambda([WeakEditor]() { return WeakEditor.IsValid(); })
		));
}

void FCortexBPToolbarExtension::OnOpenFrontendClicked()
{
	FGlobalTabmanager::Get()->TryInvokeTab(FName(TEXT("CortexFrontend")));
}

void FCortexBPToolbarExtension::OnConvertBPClicked(TWeakPtr<FBlueprintEditor> WeakEditor)
{
	TSharedPtr<FBlueprintEditor> Editor = WeakEditor.Pin();
	if (!Editor.IsValid()) return;

	FCortexConversionPayload Payload = CapturePayload(Editor);

	FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	Core.OnConversionRequested().Broadcast(Payload);
}

FCortexConversionPayload FCortexBPToolbarExtension::CapturePayload(TSharedPtr<FBlueprintEditor> Editor)
{
	FCortexConversionPayload Payload;

	UBlueprint* Blueprint = Editor->GetBlueprintObj();
	if (!Blueprint) return Payload;

	Payload.BlueprintPath = Blueprint->GetPathName();
	Payload.BlueprintName = Blueprint->GetName();
	Payload.ParentClassName = Blueprint->ParentClass ? Blueprint->ParentClass->GetName() : TEXT("None");

	// Current graph
	UEdGraph* FocusedGraph = Editor->GetFocusedGraph();
	Payload.CurrentGraphName = FocusedGraph ? FocusedGraph->GetFName().ToString() : TEXT("");

	// Selected nodes
	TSet<UObject*> SelectedNodes = Editor->GetSelectedNodes();
	for (UObject* Node : SelectedNodes)
	{
		if (UEdGraphNode* GraphNode = Cast<UEdGraphNode>(Node))
		{
			Payload.SelectedNodeIds.Add(GraphNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		}
	}

	// Graph names
	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		Payload.GraphNames.Add(Graph->GetFName().ToString());
	}

	// Events (from ubergraph pages — find K2Node_Event nodes)
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
			{
				Payload.EventNames.Add(EventNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
			}
		}
	}

	// Functions
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		Payload.FunctionNames.Add(Graph->GetFName().ToString());
	}

	// Detect project-owned ancestor classes for destination selection
	Payload.DetectedProjectAncestors = FCortexProjectClassDetector::FindProjectAncestors(Blueprint);

	return Payload;
}
