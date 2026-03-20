#include "CortexBPToolbarExtension.h"

#include "BlueprintEditor.h"
#include "CortexAnalysisTypes.h"
#include "CortexConversionTypes.h"
#include "CortexCoreModule.h"
#include "CortexBlueprintModule.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_Variable.h"
#include "Operations/CortexBPAnalysisOps.h"
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

	Section.AddMenuEntry(
		TEXT("AnalyzeBP"),
		NSLOCTEXT("CortexBlueprint", "AnalyzeBP", "Analyze Blueprint"),
		NSLOCTEXT("CortexBlueprint", "AnalyzeBPTip", "Analyze this Blueprint for bugs, performance issues, and quality"),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateStatic(&FCortexBPToolbarExtension::OnAnalyzeBPClicked, WeakEditor),
			FCanExecuteAction::CreateLambda([WeakEditor]() { return WeakEditor.IsValid(); })));
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

	// Detect Widget Blueprint via dynamic UMG class resolution (no compile-time UMG dependency)
	if (Blueprint->ParentClass)
	{
		static UClass* UserWidgetClass = nullptr;
		if (!UserWidgetClass)
		{
			UserWidgetClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.UserWidget"));
		}
		if (UserWidgetClass && Blueprint->ParentClass->IsChildOf(UserWidgetClass))
		{
			Payload.bIsWidgetBlueprint = true;
		}
	}

	// Extract widget-type variables for BindWidget selection UI
	if (Payload.bIsWidgetBlueprint)
	{
		static UClass* WidgetClass = nullptr;
		if (!WidgetClass)
		{
			WidgetClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.Widget"));
		}

		if (WidgetClass)
		{
			// Collect widget-type variable names
			for (const FBPVariableDescription& Var : Blueprint->NewVariables)
			{
				if (Var.VarType.PinCategory == UEdGraphSchema_K2::PC_Object
					&& Var.VarType.PinSubCategoryObject.IsValid())
				{
					UClass* VarClass = Cast<UClass>(Var.VarType.PinSubCategoryObject.Get());
					if (VarClass && VarClass->IsChildOf(WidgetClass))
					{
						Payload.WidgetVariableNames.Add(Var.VarName.ToString());
					}
				}
			}

			// Cap to prevent prompt bloat for massive widget trees — must happen before
			// graph-walking so LogicReferencedWidgets only references capped variable names
			constexpr int32 MaxWidgetVars = 50;
			if (Payload.WidgetVariableNames.Num() > MaxWidgetVars)
			{
				UE_LOG(LogCortexBlueprint, Warning,
					TEXT("Widget BP has %d widget variables, capping to %d for conversion UI"),
					Payload.WidgetVariableNames.Num(), MaxWidgetVars);
				Payload.WidgetVariableNames.SetNum(MaxWidgetVars);
			}

			// Detect which widget variables are used in graph logic
			TSet<FName> ReferencedVarNames;
			TArray<UEdGraph*> AllWidgetGraphs;
			Blueprint->GetAllGraphs(AllWidgetGraphs);
			for (UEdGraph* Graph : AllWidgetGraphs)
			{
				if (!Graph) { continue; }
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (!Node) { continue; }
					if (UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node))
					{
						ReferencedVarNames.Add(VarNode->GetVarName());
					}
				}
			}

			for (const FString& WidgetVarName : Payload.WidgetVariableNames)
			{
				if (ReferencedVarNames.Contains(FName(*WidgetVarName)))
				{
					Payload.LogicReferencedWidgets.Add(WidgetVarName);
				}
			}
		}
		else
		{
			UE_LOG(LogCortexBlueprint, Warning,
				TEXT("Failed to resolve /Script/UMG.Widget — widget variable detection skipped"));
		}
	}

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

	// Graph names and total node count for scope estimation
	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) { continue; }
		Payload.GraphNames.Add(Graph->GetFName().ToString());
		Payload.TotalNodeCount += Graph->Nodes.Num();
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

FCortexAnalysisPayload FCortexBPToolbarExtension::CaptureAnalysisPayload(
	TSharedPtr<FBlueprintEditor> Editor)
{
	FCortexAnalysisPayload Payload;

	UBlueprint* Blueprint = Editor->GetBlueprintObj();
	Payload.BlueprintPath = Blueprint->GetPathName();
	Payload.BlueprintName = Blueprint->GetName();
	Payload.ParentClassName = Blueprint->ParentClass
		? Blueprint->ParentClass->GetName() : TEXT("None");

	// Current graph
	UEdGraph* FocusedGraph = Editor->GetFocusedGraph();
	Payload.CurrentGraphName = FocusedGraph
		? FocusedGraph->GetFName().ToString() : TEXT("");

	// Selected nodes (GUIDs)
	TSet<UObject*> SelectedNodes = Editor->GetSelectedNodes();
	for (UObject* Node : SelectedNodes)
	{
		if (UEdGraphNode* GraphNode = Cast<UEdGraphNode>(Node))
		{
			Payload.SelectedNodeIds.Add(
				GraphNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		}
	}

	// All graph names
	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) { continue; }
		Payload.GraphNames.Add(Graph->GetFName().ToString());
	}

	// Events (from ubergraph pages)
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
			{
				Payload.EventNames.Add(
					EventNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
			}
		}
	}

	// Functions
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		Payload.FunctionNames.Add(Graph->GetFName().ToString());
	}

	return Payload;
}

void FCortexBPToolbarExtension::OnAnalyzeBPClicked(TWeakPtr<FBlueprintEditor> WeakEditor)
{
	TSharedPtr<FBlueprintEditor> Editor = WeakEditor.Pin();
	if (!Editor.IsValid()) return;

	UBlueprint* Blueprint = Editor->GetBlueprintObj();
	if (!Blueprint || Blueprint->bBeingCompiled) return;

	FCortexAnalysisPayload Payload = CaptureAnalysisPayload(Editor);

	// Run engine pre-scan (static analysis, no AI)
	Payload.PreScanFindings = FCortexBPAnalysisOps::RunPreScan(Blueprint);
	Payload.TotalNodeCount = FCortexBPAnalysisOps::CountTotalNodes(Blueprint);
	Payload.bTickEnabled = FCortexBPAnalysisOps::IsTickEnabled(Blueprint);

	UE_LOG(LogCortexBlueprint, Log,
		TEXT("Analyze BP: %s (%d nodes, %d pre-scan findings, tick=%s)"),
		*Payload.BlueprintName,
		Payload.TotalNodeCount,
		Payload.PreScanFindings.Num(),
		Payload.bTickEnabled ? TEXT("ON") : TEXT("OFF"));

	FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));
	Core.OnAnalysisRequested().Broadcast(Payload);
}
