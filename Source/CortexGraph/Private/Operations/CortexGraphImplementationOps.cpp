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

bool FCortexGraphImplementationOps::ValidateEligibility(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Selector,
	FCortexGraphImplementationPlan& OutPlan,
	FCortexCommandResult& OutError)
{
	OutPlan = FCortexGraphImplementationPlan();
	if (!Blueprint)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, TEXT("Blueprint is null."));
		return false;
	}
	FCortexResolvedSymbol Symbol;
	if (!FCortexGraphSymbolResolver::ResolveFunction(Blueprint, Selector, Symbol, OutError)) return false;
	UFunction* Function = Symbol.Function;
	UClass* FunctionClass = Function ? Function->GetOwnerClass() : nullptr;
	if (!Function)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::SymbolNotFound, TEXT("Resolved function is null."));
		return false;
	}
	if (!Function->HasAnyFunctionFlags(FUNC_BlueprintEvent) || Function->HasAnyFunctionFlags(FUNC_Final))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Function is not overridable."));
		return false;
	}
	UClass* SelfClass = Blueprint->GeneratedClass ? Blueprint->GeneratedClass : Blueprint->SkeletonGeneratedClass;
	if (!SelfClass || !FunctionClass || SelfClass == FunctionClass || !SelfClass->IsChildOf(FunctionClass))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Function is not an inherited override."));
		return false;
	}
	for (UEdGraph* UberGraph : Blueprint->UbergraphPages)
	{
		if (!UberGraph) continue;
		for (UEdGraphNode* Node : UberGraph->Nodes)
		{
			if (const UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
			{
				if (CustomEvent->CustomFunctionName == Function->GetFName())
				{
					OutError = FCortexCommandRouter::Error(CortexErrorCodes::FunctionExists, TEXT("A custom event with the same name already exists."));
					return false;
				}
			}
		}
	}
	if (Symbol.CallKind == ECortexCallKind::Parent && !Function->HasAnyFunctionFlags(FUNC_Native))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("Explicit parent call requested but function is not a native event."));
		return false;
	}
	OutPlan.Function = Function;
	OutPlan.FunctionClass = FunctionClass;
	OutPlan.bCanBePlacedAsEvent = UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Function);
	OutPlan.bWouldCreate = true;
	OutPlan.bParentCall = Symbol.CallKind == ECortexCallKind::Parent;
	if (OutPlan.bCanBePlacedAsEvent)
	{
		for (UEdGraph* UberGraph : Blueprint->UbergraphPages)
		{
			if (!UberGraph) continue;
			for (UEdGraphNode* Node : UberGraph->Nodes)
			{
				UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
				if (EventNode && EventNode->EventReference.GetMemberName() == Function->GetFName()
					&& EventNode->EventReference.GetMemberParentClass() == FunctionClass)
				{
					OutPlan.bWouldCreate = false;
					OutPlan.ExistingGraph = UberGraph;
					OutPlan.ExistingEntryNode = EventNode;
				}
		}
	}
	}
	else
	{
		for (UEdGraph* FunctionGraph : Blueprint->FunctionGraphs)
		{
			if (FunctionGraph && FunctionGraph->GetFName() == Function->GetFName())
			{
				OutPlan.bWouldCreate = false;
				OutPlan.ExistingGraph = FunctionGraph;
				for (UEdGraphNode* Node : FunctionGraph->Nodes)
				{
					if (Cast<UK2Node_FunctionEntry>(Node))
					{
						OutPlan.ExistingEntryNode = Node;
						break;
					}
				}
				if (!OutPlan.ExistingEntryNode)
				{
					// A function graph without an entry cannot be re-entered by a patch: apply would
					// mutate it and report success with no entry at all (readback skips the entry
					// comparison when nothing was applied), so the target is refused before mutation.
					OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
						FString::Printf(TEXT("Function graph '%s' has no FunctionEntry node; the target graph is malformed and cannot be patched"),
							*FunctionGraph->GetName()));
					return false;
				}
			}
		}
	}
	return true;
}

FCortexGraphImplementationEnsureResult FCortexGraphImplementationOps::EnsureForPatch(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Selector,
	FCortexGraphPatchState& PatchState)
{
	FCortexGraphImplementationEnsureResult Result;
	Result.bSuccess = false;

	FCortexGraphImplementationPlan Plan;
	FCortexCommandResult EligibilityError;
	if (!ValidateEligibility(Blueprint, Selector, Plan, EligibilityError))
	{
		Result.ErrorCode = EligibilityError.ErrorCode;
		Result.ErrorMessage = EligibilityError.ErrorMessage;
		Result.ErrorDetails = EligibilityError.ErrorDetails;
		return Result;
	}

	UFunction* Function = Plan.Function;
	FName FunctionName = Function->GetFName();
	UClass* FunctionClass = Plan.FunctionClass;
	const bool bCanBePlacedAsEvent = Plan.bCanBePlacedAsEvent;
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
		if (Plan.bParentCall)
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
