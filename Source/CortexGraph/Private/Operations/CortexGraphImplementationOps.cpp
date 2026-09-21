#include "Operations/CortexGraphImplementationOps.h"
#include "Operations/CortexGraphSymbolResolver.h"
#include "Operations/CortexGraphNodeContract.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"

FCortexGraphImplementationEnsureResult FCortexGraphImplementationOps::EnsureForPatch(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Selector,
	FCortexGraphPatchState& PatchState)
{
	FCortexGraphImplementationEnsureResult Result;
	Result.bSuccess = false;

	if (!Blueprint)
	{
		Result.ErrorCode = CortexErrorCodes::BlueprintNotFound;
		Result.ErrorMessage = TEXT("Blueprint is null.");
		return Result;
	}

	FCortexResolvedSymbol Symbol;
	FCortexCommandResult ResolveError;
	if (!FCortexGraphSymbolResolver::ResolveFunction(Blueprint, Selector, Symbol, ResolveError))
	{
		Result.ErrorCode = ResolveError.ErrorCode;
		Result.ErrorMessage = ResolveError.ErrorMessage;
		Result.ErrorDetails = ResolveError.ErrorDetails;
		return Result;
	}

	UFunction* Function = Symbol.Function;
	if (!Function)
	{
		Result.ErrorCode = CortexErrorCodes::SymbolNotFound;
		Result.ErrorMessage = TEXT("Resolved function is null.");
		return Result;
	}

	FName FunctionName = Function->GetFName();
	UClass* FunctionClass = Function->GetOwnerClass();

	// Verify overridable
	bool bIsOverridable = Function->HasAnyFunctionFlags(FUNC_BlueprintEvent) && !Function->HasAnyFunctionFlags(FUNC_Final);
	if (!bIsOverridable)
	{
		Result.ErrorCode = CortexErrorCodes::InvalidOperation;
		Result.ErrorMessage = TEXT("Function is not overridable.");
		return Result;
	}

	// Conflict check: same-named custom event
	for (UEdGraph* UberGraph : Blueprint->UbergraphPages)
	{
		if (UberGraph)
		{
			for (UEdGraphNode* Node : UberGraph->Nodes)
			{
				if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
				{
					if (CustomEvent->CustomFunctionName == FunctionName)
					{
						Result.ErrorCode = CortexErrorCodes::FunctionExists;
						Result.ErrorMessage = TEXT("A custom event with the same name already exists.");
						return Result;
					}
				}
			}
		}
	}

	// Verify Parent intent
	if (Symbol.CallKind == ECortexCallKind::Parent)
	{
		if (!Function->HasAnyFunctionFlags(FUNC_Native))
		{
			Result.ErrorCode = CortexErrorCodes::InvalidOperation;
			Result.ErrorMessage = TEXT("Explicit parent call requested but function is not a native event.");
			return Result;
		}
	}

	bool bCanBePlacedAsEvent = UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Function);
	UEdGraph* OverrideGraph = nullptr;
	UEdGraphNode* EntryNode = nullptr;
	UEdGraphNode* ResultNode = nullptr;

	if (bCanBePlacedAsEvent)
	{
		// Find existing event
		for (UEdGraph* UberGraph : Blueprint->UbergraphPages)
		{
			if (!UberGraph) continue;
			for (UEdGraphNode* Node : UberGraph->Nodes)
			{
				if (UK2Node_Event* EvNode = Cast<UK2Node_Event>(Node))
				{
					if (EvNode->EventReference.GetMemberName() == FunctionName && EvNode->EventReference.GetMemberParentClass() == FunctionClass)
					{
						EntryNode = EvNode;
						OverrideGraph = UberGraph;
						break;
					}
				}
			}
			if (EntryNode) break;
		}

		if (!EntryNode)
		{
			// Need to create event
			if (Blueprint->UbergraphPages.Num() == 0)
			{
				OverrideGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, UEdGraphSchema_K2::GN_EventGraph, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
				FBlueprintEditorUtils::AddUbergraphPage(Blueprint, OverrideGraph);
			}
			else
			{
				OverrideGraph = Blueprint->UbergraphPages[0];
			}
			
			int32 NodePosY = 0;
			EntryNode = FKismetEditorUtilities::AddDefaultEventNode(Blueprint, OverrideGraph, FunctionName, FunctionClass, NodePosY);
			
			if (EntryNode)
			{
				Result.bCreated = true;
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			}
		}

		if (EntryNode)
		{
			Result.bSuccess = true;
			Result.Graph = OverrideGraph;
			Result.EntryNode = EntryNode;
		}
	}
	else
	{
		// Function Graph
		for (UEdGraph* FuncGraph : Blueprint->FunctionGraphs)
		{
			if (FuncGraph && FuncGraph->GetFName() == FunctionName)
			{
				OverrideGraph = FuncGraph;
				break;
			}
		}

		if (!OverrideGraph)
		{
			// Create function graph
			OverrideGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FunctionName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			FBlueprintEditorUtils::AddFunctionGraph(Blueprint, OverrideGraph, true, FunctionClass);
			Result.bCreated = true;
		}

		if (OverrideGraph)
		{
			// Find entry and result
			for (UEdGraphNode* Node : OverrideGraph->Nodes)
			{
				if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
				{
					EntryNode = Entry;
				}
				else if (UK2Node_FunctionResult* Res = Cast<UK2Node_FunctionResult>(Node))
				{
					ResultNode = Res;
				}
			}

			Result.bSuccess = true;
			Result.Graph = OverrideGraph;
			Result.EntryNode = EntryNode;
			Result.ResultNode = ResultNode;
		}
	}

	return Result;
}
