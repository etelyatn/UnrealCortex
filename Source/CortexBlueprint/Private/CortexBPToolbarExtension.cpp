#include "CortexBPToolbarExtension.h"

#include "BlueprintEditor.h"
#include "CortexAnalysisTypes.h"
#include "CortexConversionTypes.h"
#include "GameFramework/Actor.h"
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
			auto AddCortexButton = [](const TCHAR* MenuName)
			{
				UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu(MenuName);
				FToolMenuSection& Section = ToolbarMenu->FindOrAddSection(TEXT("Cortex"));
				Section.AddEntry(FToolMenuEntry::InitComboButton(
					TEXT("CortexBPTools"),
					FUIAction(),
					FNewToolMenuDelegate::CreateStatic(&FCortexBPToolbarExtension::BuildMenu),
					NSLOCTEXT("CortexBlueprint", "CortexToolbar", "Cortex"),
					NSLOCTEXT("CortexBlueprint", "CortexToolbarTooltip", "Cortex AI tools for this Blueprint"),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details")
				));
			};

			AddCortexButton(TEXT("AssetEditor.BlueprintEditor.ToolBar"));
			AddCortexButton(TEXT("AssetEditor.WidgetBlueprintEditor.ToolBar"));
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

	// Detect actor descendant via class hierarchy check (not string matching)
	if (Blueprint->ParentClass)
	{
		Payload.bIsActorDescendant = Blueprint->ParentClass->IsChildOf(AActor::StaticClass());
	}

	// Detect Widget Blueprint via dynamic UMG class resolution (no compile-time UMG dependency)
	// Resolve each time rather than caching static UClass* — static pointers become stale after hot reload
	if (Blueprint->ParentClass)
	{
		UClass* UserWidgetClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.UserWidget"));
		if (UserWidgetClass && Blueprint->ParentClass->IsChildOf(UserWidgetClass))
		{
			Payload.bIsWidgetBlueprint = true;
		}
	}

	// Extract designer widget variables (marked "Is Variable" in widget tree) for BindWidget selection UI
	if (Payload.bIsWidgetBlueprint)
	{
		// Build set of designer widget names from the widget tree via reflection
		TSet<FString> DesignerWidgetNames;
		const FObjectProperty* WidgetTreeProp = CastField<FObjectProperty>(
			Blueprint->GetClass()->FindPropertyByName(TEXT("WidgetTree")));
		UObject* WidgetTreeObj = WidgetTreeProp
			? WidgetTreeProp->GetObjectPropertyValue_InContainer(Blueprint)
			: nullptr;
		if (WidgetTreeObj)
		{
			const FArrayProperty* AllWidgetsProp = CastField<FArrayProperty>(
				WidgetTreeObj->GetClass()->FindPropertyByName(TEXT("AllWidgets")));
			const FObjectProperty* WidgetObjProp = AllWidgetsProp
				? CastField<FObjectProperty>(AllWidgetsProp->Inner)
				: nullptr;
			if (AllWidgetsProp && WidgetObjProp)
			{
				FScriptArrayHelper WidgetsArray(AllWidgetsProp,
					AllWidgetsProp->ContainerPtrToValuePtr<void>(WidgetTreeObj));
				for (int32 Index = 0; Index < WidgetsArray.Num(); ++Index)
				{
					UObject* WidgetObject = WidgetObjProp->GetObjectPropertyValue(
						WidgetsArray.GetRawPtr(Index));
					if (!WidgetObject) { continue; }

					const FBoolProperty* IsVarProp = CastField<FBoolProperty>(
						WidgetObject->GetClass()->FindPropertyByName(TEXT("bIsVariable")));
					if (IsVarProp && IsVarProp->GetPropertyValue_InContainer(WidgetObject))
					{
						DesignerWidgetNames.Add(WidgetObject->GetName());
					}
				}
			}
		}

		// Collect widget-type variable names, filtering to only designer widgets
		for (const FBPVariableDescription& Var : Blueprint->NewVariables)
		{
			if (Var.VarType.PinCategory == UEdGraphSchema_K2::PC_Object
				&& Var.VarType.PinSubCategoryObject.IsValid())
			{
				UClass* WidgetClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.Widget"));
				UClass* VarClass = Cast<UClass>(Var.VarType.PinSubCategoryObject.Get());
				if (VarClass && WidgetClass && VarClass->IsChildOf(WidgetClass))
				{
					const FString VarName = Var.VarName.ToString();
					// Only include if it matches a designer widget marked "Is Variable"
					if (DesignerWidgetNames.Contains(VarName))
					{
						Payload.WidgetVariableNames.Add(VarName);
					}
				}
			}
		}

		// Detect which widget variables are used in graph logic (before capping)
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

		// Cap after logic-reference walk — prioritize logic-referenced widgets
		constexpr int32 MaxWidgetVars = 50;
		if (Payload.WidgetVariableNames.Num() > MaxWidgetVars)
		{
			UE_LOG(LogCortexBlueprint, Warning,
				TEXT("Widget BP has %d widget variables, capping to %d for conversion UI"),
				Payload.WidgetVariableNames.Num(), MaxWidgetVars);

			// Sort: logic-referenced first, then alphabetical within each group
			TSet<FString> LogicRefSet(Payload.LogicReferencedWidgets);
			Payload.WidgetVariableNames.Sort([&LogicRefSet](const FString& A, const FString& B)
			{
				const bool bARef = LogicRefSet.Contains(A);
				const bool bBRef = LogicRefSet.Contains(B);
				if (bARef != bBRef) { return bARef; }
				return A < B;
			});
			Payload.WidgetVariableNames.SetNum(MaxWidgetVars);
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

	// Parent class path and Blueprint-parent detection for dependency analysis
	if (Blueprint->ParentClass)
	{
		Payload.ParentClassPath = Blueprint->ParentClass->GetPathName();
		Payload.bParentIsBlueprint = (Blueprint->ParentClass->ClassGeneratedBy != nullptr);
	}

	// Implemented interfaces for dependency analysis
	for (const FBPInterfaceDescription& IfaceDesc : Blueprint->ImplementedInterfaces)
	{
		if (!IfaceDesc.Interface)
		{
			continue;
		}
		FCortexConversionPayload::FPayloadInterfaceInfo Info;
		Info.InterfaceName = IfaceDesc.Interface->GetName();
		// Native C++ interfaces live under /Script/, everything else is a Blueprint interface
		Info.bIsBlueprint = !IfaceDesc.Interface->GetPathName().StartsWith(TEXT("/Script/"));
		Payload.ImplementedInterfaces.Add(MoveTemp(Info));
	}

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
