#include "Operations/CortexBPStructureOps.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPTypeUtils.h"
#include "CortexBlueprintModule.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Knot.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"

using CortexBPTypeUtils::ResolveVariableType;
using CortexBPTypeUtils::FriendlyTypeName;

FCortexCommandResult FCortexBPStructureOps::AddVariable(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString VarName;
	FString VarType;

	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("name"), VarName)
		|| !Params->TryGetStringField(TEXT("type"), VarType))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, name, type")
		);
	}

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	// Check if variable already exists
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == FName(*VarName))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::VariableExists,
				FString::Printf(TEXT("Variable '%s' already exists"), *VarName)
			);
		}
	}

	FEdGraphPinType PinType = ResolveVariableType(VarType);

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex:Add Variable %s"), *VarName)
	));

	bool bSuccess = FBlueprintEditorUtils::AddMemberVariable(
		Blueprint, FName(*VarName), PinType);

	if (!bSuccess)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidValue,
			FString::Printf(TEXT("Failed to add variable '%s'"), *VarName)
		);
	}

	// Set default value and flags
	FString DefaultValue;
	Params->TryGetStringField(TEXT("default_value"), DefaultValue);

	bool bIsExposed = false;
	Params->TryGetBoolField(TEXT("is_exposed"), bIsExposed);

	FString Category;
	Params->TryGetStringField(TEXT("category"), Category);

	for (FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == FName(*VarName))
		{
			if (!DefaultValue.IsEmpty())
			{
				Var.DefaultValue = DefaultValue;
			}
			if (bIsExposed)
			{
				Var.PropertyFlags |= CPF_BlueprintVisible;
			}
			if (!Category.IsEmpty())
			{
				Var.Category = FText::FromString(Category);
			}
			break;
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("added"), true);
	Data->SetStringField(TEXT("name"), VarName);
	Data->SetStringField(TEXT("type"), VarType);
	if (!DefaultValue.IsEmpty())
	{
		Data->SetStringField(TEXT("default_value"), DefaultValue);
	}

	UE_LOG(LogCortexBlueprint, Log, TEXT("Added variable '%s' (%s) to %s"),
		*VarName, *VarType, *AssetPath);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexBPStructureOps::RemoveVariable(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString VarName;

	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("name"), VarName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, name")
		);
	}

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	// Check if variable exists
	bool bFound = false;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == FName(*VarName))
		{
			bFound = true;
			break;
		}
	}

	if (!bFound)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::VariableNotFound,
			FString::Printf(TEXT("Variable '%s' not found"), *VarName)
		);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex:Remove Variable %s"), *VarName)
	));

	FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, FName(*VarName));
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("removed"), true);
	Data->SetStringField(TEXT("name"), VarName);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Removed variable '%s' from %s"), *VarName, *AssetPath);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexBPStructureOps::AddFunction(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString FuncName;

	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath)
		|| !Params->TryGetStringField(TEXT("name"), FuncName))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, name")
		);
	}

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (Blueprint == nullptr)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	// Check if function already exists
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph != nullptr && Graph->GetName() == FuncName)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::FunctionExists,
				FString::Printf(TEXT("Function '%s' already exists"), *FuncName)
			);
		}
	}

	// Parse optional inputs/outputs arrays
	const TArray<TSharedPtr<FJsonValue>>* InputsArray = nullptr;
	Params->TryGetArrayField(TEXT("inputs"), InputsArray);

	const TArray<TSharedPtr<FJsonValue>>* OutputsArray = nullptr;
	Params->TryGetArrayField(TEXT("outputs"), OutputsArray);

	// All-or-nothing validation: resolve all types before creating anything
	struct FPinSpec
	{
		FString Name;
		FEdGraphPinType PinType;
	};

	TArray<FPinSpec> InputSpecs;
	TArray<FPinSpec> OutputSpecs;

	auto ParsePinArray = [](const TArray<TSharedPtr<FJsonValue>>* Array, TArray<FPinSpec>& OutSpecs, FString& OutError) -> bool
	{
		if (!Array)
		{
			return true;
		}
		for (const TSharedPtr<FJsonValue>& Val : *Array)
		{
			const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
			if (!Obj.IsValid())
			{
				continue;
			}

			FString PinName, PinTypeStr;
			if (!Obj->TryGetStringField(TEXT("name"), PinName) || !Obj->TryGetStringField(TEXT("type"), PinTypeStr))
			{
				OutError = TEXT("Each input/output requires 'name' and 'type' fields");
				return false;
			}

			FEdGraphPinType PinType = ResolveVariableType(PinTypeStr);
			if (PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
			{
				OutError = FString::Printf(TEXT("Unknown type '%s' for parameter '%s'"), *PinTypeStr, *PinName);
				return false;
			}

			OutSpecs.Add({ PinName, PinType });
		}
		return true;
	};

	FString ValidationError;
	if (!ParsePinArray(InputsArray, InputSpecs, ValidationError) ||
		!ParsePinArray(OutputsArray, OutputSpecs, ValidationError))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidValue, ValidationError);
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex:Add Function %s"), *FuncName)
	));

	// Create the function graph
	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint,
		FName(*FuncName),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass()
	);

	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, NewGraph, false, nullptr);

	// Find entry node
	UK2Node_FunctionEntry* EntryNode = nullptr;
	for (UEdGraphNode* Node : NewGraph->Nodes)
	{
		EntryNode = Cast<UK2Node_FunctionEntry>(Node);
		if (EntryNode)
		{
			break;
		}
	}

	// Add input pins to entry node
	if (EntryNode && InputSpecs.Num() > 0)
	{
		for (const FPinSpec& Spec : InputSpecs)
		{
			EntryNode->CreateUserDefinedPin(FName(*Spec.Name), Spec.PinType, EGPD_Output);
		}
	}

	// Add output pins to result node
	if (OutputSpecs.Num() > 0 && EntryNode)
	{
		UK2Node_FunctionResult* ResultNode =
			FBlueprintEditorUtils::FindOrCreateFunctionResultNode(EntryNode);

		if (ResultNode)
		{
			for (const FPinSpec& Spec : OutputSpecs)
			{
				ResultNode->CreateUserDefinedPin(FName(*Spec.Name), Spec.PinType, EGPD_Input);
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	// Build response — read back actual created pins, don't echo input
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("added"), true);
	Data->SetStringField(TEXT("name"), FuncName);
	Data->SetStringField(TEXT("graph_name"), NewGraph->GetName());

	// Serialize inputs from actual UserDefinedPins on entry node
	TArray<TSharedPtr<FJsonValue>> ResponseInputs;
	if (EntryNode)
	{
		for (const TSharedPtr<FUserPinInfo>& PinInfo : EntryNode->UserDefinedPins)
		{
			TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
			PinObj->SetStringField(TEXT("name"), PinInfo->PinName.ToString());
			PinObj->SetStringField(TEXT("type"), FriendlyTypeName(PinInfo->PinType));
			ResponseInputs.Add(MakeShared<FJsonValueObject>(PinObj));
		}
	}
	Data->SetArrayField(TEXT("inputs"), ResponseInputs);

	// Serialize outputs from result node
	TArray<TSharedPtr<FJsonValue>> ResponseOutputs;
	for (UEdGraphNode* Node : NewGraph->Nodes)
	{
		if (UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node))
		{
			for (const TSharedPtr<FUserPinInfo>& PinInfo : ResultNode->UserDefinedPins)
			{
				TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
				PinObj->SetStringField(TEXT("name"), PinInfo->PinName.ToString());
				PinObj->SetStringField(TEXT("type"), FriendlyTypeName(PinInfo->PinType));
				ResponseOutputs.Add(MakeShared<FJsonValueObject>(PinObj));
			}
			break;
		}
	}
	Data->SetArrayField(TEXT("outputs"), ResponseOutputs);

	UE_LOG(LogCortexBlueprint, Log, TEXT("Added function '%s' to %s"), *FuncName, *AssetPath);

	return FCortexCommandRouter::Success(Data);
}

void FCortexBPStructureOps::GatherCascadeExecNodes(
	UEdGraphNode* StartNode,
	TSet<UEdGraphNode*>& OutRemovalSet)
{
	if (!StartNode)
	{
		return;
	}

	OutRemovalSet.Add(StartNode);
	TArray<UEdGraphNode*> Queue;
	Queue.Add(StartNode);
	int32 QueueIndex = 0;

	while (QueueIndex < Queue.Num())
	{
		UEdGraphNode* Current = Queue[QueueIndex++];
		if (!Current)
		{
			continue;
		}

		for (UEdGraphPin* Pin : Current->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output)
			{
				continue;
			}

			const bool bCurrentIsKnot = Cast<UK2Node_Knot>(Current) != nullptr;
			const bool bPinIsExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;

			// Follow exec pins, or knot outputs where the receiving pin is also exec/knot
			if (!bPinIsExec && !bCurrentIsKnot)
			{
				continue;
			}

			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!LinkedPin)
				{
					continue;
				}

				UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
				if (!LinkedNode || OutRemovalSet.Contains(LinkedNode))
				{
					continue;
				}

				// For knot nodes: only follow the wire if the destination is exec-typed or another knot
				if (bCurrentIsKnot && !bPinIsExec)
				{
					bool bDestIsExecOrKnot = false;
					if (Cast<UK2Node_Knot>(LinkedNode))
					{
						bDestIsExecOrKnot = true;
					}
					else if (LinkedPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
					{
						bDestIsExecOrKnot = true;
					}
					if (!bDestIsExecOrKnot)
					{
						continue;
					}
				}

				// Check if this node has incoming exec (or wildcard for knots) from outside the removal set
				bool bHasExternalExecInput = false;
				for (UEdGraphPin* NodePin : LinkedNode->Pins)
				{
					if (!NodePin || NodePin->Direction != EGPD_Input)
					{
						continue;
					}

					// For reroute nodes, any input pin may propagate exec even if still wildcard
					const bool bNodePinIsExec = (NodePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
					const bool bLinkedIsKnot = (Cast<UK2Node_Knot>(LinkedNode) != nullptr);
					if (!bNodePinIsExec && !bLinkedIsKnot)
					{
						continue;
					}

					for (UEdGraphPin* IncomingPin : NodePin->LinkedTo)
					{
						if (IncomingPin)
						{
							UEdGraphNode* IncomingNode = IncomingPin->GetOwningNode();
							if (IncomingNode && !OutRemovalSet.Contains(IncomingNode))
							{
								bHasExternalExecInput = true;
								break;
							}
						}
					}
					if (bHasExternalExecInput)
					{
						break;
					}
				}

				if (!bHasExternalExecInput)
				{
					OutRemovalSet.Add(LinkedNode);
					Queue.Add(LinkedNode);
				}
			}
		}
	}
}

FCortexCommandResult FCortexBPStructureOps::RemoveGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString Name;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty()
		|| !Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Missing required params: asset_path, name"));
	}

	FString LoadError;
	UBlueprint* BP = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (!BP)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, LoadError);
	}

	const bool bCompile = Params->HasField(TEXT("compile")) ? Params->GetBoolField(TEXT("compile")) : true;
	const bool bCascade = Params->HasField(TEXT("cascade_exec_chain")) ? Params->GetBoolField(TEXT("cascade_exec_chain")) : false;
	const bool bDryRun = Params->HasField(TEXT("dry_run")) ? Params->GetBoolField(TEXT("dry_run")) : false;

	// Normalize ConstructionScript friendly name to internal name
	const FString ResolvedName = (Name == TEXT("ConstructionScript"))
		? TEXT("UserConstructionScript") : Name;

	// --- Protected graph check ---
	// Primary EventGraph
	if (BP->UbergraphPages.Num() > 0 && BP->UbergraphPages[0])
	{
		const FString EventGraphName = BP->UbergraphPages[0]->GetName();
		if (ResolvedName == EventGraphName)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				TEXT("Cannot remove primary EventGraph — it is a protected structural graph"));
		}
	}
	// ConstructionScript
	if (ResolvedName == TEXT("UserConstructionScript"))
	{
		// Verify it actually exists before rejecting
		for (UEdGraph* Graph : BP->FunctionGraphs)
		{
			if (Graph && (Graph->GetName() == ResolvedName))
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidOperation,
					TEXT("Cannot remove ConstructionScript — it is a protected structural graph"));
			}
		}
	}

	// --- Name resolution: search graphs ---
	UEdGraph* FoundGraph = nullptr;
	FString FoundType;

	// 1. FunctionGraphs
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == ResolvedName)
		{
			FoundGraph = Graph;
			FoundType = TEXT("Function");
			break;
		}
	}

	// 2. MacroGraphs
	if (!FoundGraph)
	{
		for (UEdGraph* Graph : BP->MacroGraphs)
		{
			if (Graph && Graph->GetName() == ResolvedName)
			{
				FoundGraph = Graph;
				FoundType = TEXT("Macro");
				break;
			}
		}
	}

	// 3. UbergraphPages (skip index 0 = primary EventGraph)
	if (!FoundGraph)
	{
		for (int32 i = 1; i < BP->UbergraphPages.Num(); ++i)
		{
			if (BP->UbergraphPages[i] && BP->UbergraphPages[i]->GetName() == ResolvedName)
			{
				FoundGraph = BP->UbergraphPages[i];
				FoundType = TEXT("EventGraph");
				break;
			}
		}
	}

	// --- Shared helper: compile, save, and build response ---
	auto FinalizeResponse = [&](TSharedPtr<FJsonObject>& ResponseData) -> FCortexCommandResult
	{
		if (!bDryRun)
		{
			if (bCompile)
			{
				FKismetEditorUtilities::CompileBlueprint(BP);
				ResponseData->SetBoolField(TEXT("compiled"), true);
				ResponseData->SetStringField(TEXT("compile_status"),
					(BP->Status == BS_UpToDate || BP->Status == BS_UpToDateWithWarnings)
						? TEXT("UpToDate") : TEXT("Error"));
				ResponseData->SetBoolField(TEXT("compile_has_warnings"),
					BP->Status == EBlueprintStatus::BS_UpToDateWithWarnings);
			}
			else
			{
				ResponseData->SetBoolField(TEXT("compiled"), false);
			}

			// Persist to disk (skip transient packages used by tests)
			if (!BP->GetPackage()->GetName().StartsWith(TEXT("/Engine/Transient")))
			{
				FString PackageFilename;
				if (FPackageName::TryConvertLongPackageNameToFilename(
						BP->GetPackage()->GetName(), PackageFilename, TEXT(".uasset")))
				{
					FSavePackageArgs SaveArgs;
					SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
					UPackage::SavePackage(BP->GetPackage(), BP, *PackageFilename, SaveArgs);
				}
			}
		}
		else
		{
			ResponseData->SetBoolField(TEXT("compiled"), false);
			ResponseData->SetBoolField(TEXT("dry_run"), true);
		}
		return FCortexCommandRouter::Success(ResponseData);
	};

	// --- If found a graph, remove it ---
	if (FoundGraph)
	{
		const int32 NodeCount = FoundGraph->Nodes.Num();

		// Build node list for response (always, not just dry_run)
		TArray<TSharedPtr<FJsonValue>> RemovedNodesArray;
		for (UEdGraphNode* Node : FoundGraph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
			NodeObj->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
			NodeObj->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
			FText NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView);
			if (!NodeTitle.IsEmpty())
			{
				NodeObj->SetStringField(TEXT("title"), NodeTitle.ToString());
			}
			RemovedNodesArray.Add(MakeShared<FJsonValueObject>(NodeObj));
		}

		if (!bDryRun)
		{
			FScopedTransaction Transaction(FText::FromString(
				FString::Printf(TEXT("Cortex: Remove %s graph %s"), *FoundType, *Name)));

			FBlueprintEditorUtils::RemoveGraph(BP, FoundGraph);
		}

		TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
		ResponseData->SetStringField(TEXT("asset_path"), AssetPath);

		TSharedPtr<FJsonObject> RemovedObj = MakeShared<FJsonObject>();
		RemovedObj->SetStringField(TEXT("name"), Name);
		RemovedObj->SetStringField(TEXT("type"), FoundType);
		RemovedObj->SetNumberField(TEXT("node_count"), NodeCount);
		ResponseData->SetObjectField(TEXT("removed"), RemovedObj);
		ResponseData->SetArrayField(TEXT("removed_nodes"), RemovedNodesArray);

		UE_LOG(LogCortexBlueprint, Log, TEXT("Removed %s graph '%s' from %s (%d nodes)%s"),
			*FoundType, *Name, *BP->GetName(), NodeCount, bDryRun ? TEXT(" [dry_run]") : TEXT(""));

		return FinalizeResponse(ResponseData);
	}

	// --- 4. Custom event nodes in all EventGraph pages ---
	UK2Node_CustomEvent* FoundEvent = nullptr;
	for (UEdGraph* EventGraphPage : BP->UbergraphPages)
	{
		if (!EventGraphPage)
		{
			continue;
		}
		for (UEdGraphNode* Node : EventGraphPage->Nodes)
		{
			UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node);
			if (CustomEvent && CustomEvent->CustomFunctionName.ToString() == ResolvedName)
			{
				FoundEvent = CustomEvent;
				break;
			}
		}
		if (FoundEvent)
		{
			break;
		}
	}

	if (FoundEvent)
	{
		TSet<UEdGraphNode*> RemovalSet;
		if (bCascade)
		{
			GatherCascadeExecNodes(FoundEvent, RemovalSet);
		}
		else
		{
			RemovalSet.Add(FoundEvent);
		}

		const int32 NodeCount = RemovalSet.Num();

		// Build node list for response (useful for dry_run preview and logging)
		TArray<TSharedPtr<FJsonValue>> RemovedNodesArray;
		for (UEdGraphNode* Node : RemovalSet)
		{
			if (!Node)
			{
				continue;
			}
			TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
			NodeObj->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
			NodeObj->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
			FText NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView);
			if (!NodeTitle.IsEmpty())
			{
				NodeObj->SetStringField(TEXT("title"), NodeTitle.ToString());
			}
			RemovedNodesArray.Add(MakeShared<FJsonValueObject>(NodeObj));
		}

		if (!bDryRun)
		{
			FScopedTransaction Transaction(FText::FromString(
				FString::Printf(TEXT("Cortex: Remove custom event %s"), *Name)));

			for (UEdGraphNode* NodeToRemove : RemovalSet)
			{
				FBlueprintEditorUtils::RemoveNode(BP, NodeToRemove, /*bDontRecompile=*/true);
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		}

		TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
		ResponseData->SetStringField(TEXT("asset_path"), AssetPath);

		TSharedPtr<FJsonObject> RemovedObj = MakeShared<FJsonObject>();
		RemovedObj->SetStringField(TEXT("name"), Name);
		RemovedObj->SetStringField(TEXT("type"), TEXT("CustomEvent"));
		RemovedObj->SetNumberField(TEXT("node_count"), NodeCount);
		RemovedObj->SetBoolField(TEXT("cascade_exec_chain"), bCascade);
		ResponseData->SetObjectField(TEXT("removed"), RemovedObj);
		ResponseData->SetArrayField(TEXT("removed_nodes"), RemovedNodesArray);

		UE_LOG(LogCortexBlueprint, Log,
			TEXT("Removed custom event '%s' from %s (%d nodes, cascade=%d)%s"),
			*Name, *BP->GetName(), NodeCount, bCascade, bDryRun ? TEXT(" [dry_run]") : TEXT(""));

		return FinalizeResponse(ResponseData);
	}

	// Build list of available names for hint
	TArray<FString> AvailableNames;
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph)
		{
			AvailableNames.Add(Graph->GetName());
		}
	}
	for (UEdGraph* Graph : BP->MacroGraphs)
	{
		if (Graph)
		{
			AvailableNames.Add(Graph->GetName());
		}
	}
	for (int32 i = 1; i < BP->UbergraphPages.Num(); ++i)
	{
		if (BP->UbergraphPages[i])
		{
			AvailableNames.Add(BP->UbergraphPages[i]->GetName());
		}
	}
	for (UEdGraph* EventGraph : BP->UbergraphPages)
	{
		if (!EventGraph)
		{
			continue;
		}
		for (UEdGraphNode* Node : EventGraph->Nodes)
		{
			UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node);
			if (CE)
			{
				AvailableNames.Add(CE->CustomFunctionName.ToString());
			}
		}
	}
	const FString AvailableHint = AvailableNames.Num() > 0
		? FString::Printf(TEXT(" Available: [%s]"), *FString::Join(AvailableNames, TEXT(", ")))
		: TEXT("");

	return FCortexCommandRouter::Error(
		CortexErrorCodes::GraphNotFound,
		FString::Printf(TEXT("Graph or custom event '%s' not found in %s.%s"),
			*Name, *BP->GetName(), *AvailableHint));
}
