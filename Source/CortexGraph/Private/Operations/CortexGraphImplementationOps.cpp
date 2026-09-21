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
#include "K2Node_CallParentFunction.h"

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

	// Inherited owner mismatch check
	FString ExpectedOwnerClass;
	if (Selector->TryGetStringField(TEXT("owner_class"), ExpectedOwnerClass) && !ExpectedOwnerClass.IsEmpty())
	{
		// Validated implicitly via SymbolResolver unless exact match needed, but the prompt says 
		// "verifying rejection when function is not an inherited override"
		// If the function's declaring class is not in the Blueprint's ancestry, it's not an inherited override.
		// Wait, SymbolResolver already checks if it is in the Blueprint's class hierarchy!
		// However, let's explicitly verify it is inherited (i.e. OwnerClass != Blueprint->GeneratedClass and is parent)
		if (!Blueprint->GeneratedClass || !Blueprint->GeneratedClass->IsChildOf(FunctionClass) || Blueprint->GeneratedClass == FunctionClass)
		{
			Result.ErrorCode = CortexErrorCodes::InvalidOperation;
			Result.ErrorMessage = TEXT("Function is not an inherited override.");
			return Result;
		}
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
	bool bGraphCreated = false;

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
				bGraphCreated = true;
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
			bGraphCreated = true;
		}

		if (OverrideGraph)
		{
			// Find entry and result
			for (UEdGraphNode* Node : OverrideGraph->Nodes)
			{
				if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
				{
					EntryNode = Entry;
					if (bGraphCreated && Function)
					{
						Entry->ClearExtraFlags(FUNC_AccessSpecifiers);
						Entry->AddExtraFlags(Function->FunctionFlags & FUNC_AccessSpecifiers);
					}
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

	// Handle Journaling
	if (Result.bSuccess && Result.bCreated)
	{
		if (bGraphCreated)
		{
			PatchState.JournalGraphAdded(OverrideGraph);
		}
		PatchState.JournalNodeAdded(EntryNode);
		if (ResultNode)
		{
			PatchState.JournalNodeAdded(ResultNode);
		}

		// Connect Parent Call if requested
		if (Symbol.CallKind == ECortexCallKind::Parent)
		{
			UK2Node_CallParentFunction* ParentNode = NewObject<UK2Node_CallParentFunction>(OverrideGraph);
			ParentNode->SetFromFunction(Function);
			ParentNode->CreateNewGuid();
			ParentNode->PostPlacedNewNode();
			ParentNode->AllocateDefaultPins();
			
			// Position it nicely
			ParentNode->NodePosX = EntryNode->NodePosX + 300;
			ParentNode->NodePosY = EntryNode->NodePosY;
			
			OverrideGraph->AddNode(ParentNode, true, false);
			PatchState.JournalNodeAdded(ParentNode);

			const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
			
			// Connect Exec Pins
			UEdGraphPin* EntryThenPin = EntryNode->FindPin(UEdGraphSchema_K2::PN_Then);
			UEdGraphPin* ParentExecPin = ParentNode->GetExecPin();
			if (EntryThenPin && ParentExecPin)
			{
				Schema->TryCreateConnection(EntryThenPin, ParentExecPin);
			}

			// Connect Data Pins
			for (UEdGraphPin* EntryPin : EntryNode->Pins)
			{
				if (EntryPin->Direction == EGPD_Output && EntryPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					if (UEdGraphPin* ParentParamPin = ParentNode->FindPin(EntryPin->PinName))
					{
						Schema->TryCreateConnection(EntryPin, ParentParamPin);
					}
				}
			}

			if (ResultNode)
			{
				UEdGraphPin* ParentThenPin = ParentNode->FindPin(UEdGraphSchema_K2::PN_Then);
				UEdGraphPin* ResultExecPin = Cast<UK2Node_FunctionResult>(ResultNode)->GetExecPin();
				if (ParentThenPin && ResultExecPin)
				{
					Schema->TryCreateConnection(ParentThenPin, ResultExecPin);
				}

				// Connect Output Data Pins
				for (UEdGraphPin* ResultPin : ResultNode->Pins)
				{
					if (ResultPin->Direction == EGPD_Input && ResultPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
					{
						if (UEdGraphPin* ParentOutputPin = ParentNode->FindPin(ResultPin->PinName))
						{
							Schema->TryCreateConnection(ParentOutputPin, ResultPin);
						}
					}
				}
			}
		}
	}

	return Result;
}
