#pragma once

// Shared fixtures of the replace_interface suites (#122 Tasks 7-8): a Blueprint interface (GetValue: function-style with a
// double output, Notify(int32 Amount): event-style), actor Blueprints implementing it with bodies, and request builders.

#include "CoreMinimal.h"

#include "CortexCommandRouter.h"
#include "CortexGraphInterfaceTestTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Message.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "UObject/Interface.h"
#include "UObject/Package.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

namespace CortexGraphReplaceInterfaceTest
{
	/** GC-safety invariant of the migration fixtures: the undo history is emptied before any fixture object is retired. */
	struct FScope
	{
		TArray<UBlueprint*> Blueprints;
		FScope() = default;
		FScope(const FScope&) = delete;
		FScope& operator=(const FScope&) = delete;
		~FScope()
		{
			if (GEditor != nullptr && GEditor->Trans != nullptr)
			{
				GEditor->Trans->Reset(FText::FromString(TEXT("Retire replace_interface test fixture")));
			}
			for (UBlueprint* Blueprint : Blueprints)
			{
				if (Blueprint == nullptr)
				{
					continue;
				}
				if (Blueprint->GeneratedClass != nullptr) { Blueprint->GeneratedClass->MarkAsGarbage(); }
				if (Blueprint->SkeletonGeneratedClass != nullptr) { Blueprint->SkeletonGeneratedClass->MarkAsGarbage(); }
				Blueprint->MarkAsGarbage();
			}
		}
	};

	/** Counts the coordinator's real target and recovery compiles. */
	struct FCompileCounter
	{
		static inline FCompileCounter* Active = nullptr;
		int32 TargetCompiles = 0;
		int32 RecoveryCompiles = 0;

		FCompileCounter()
		{
			Active = this;
			FCortexGraphPatchOps::SetOperationObserverForTesting(
				[](const FName Operation, UBlueprint*)
				{
					if (!Active) return;
					if (Operation == TEXT("target_compile")) ++Active->TargetCompiles;
					else if (Operation == TEXT("recovery_compile")) ++Active->RecoveryCompiles;
				});
		}
		~FCompileCounter()
		{
			FCortexGraphPatchOps::ClearOperationObserverForTesting();
			Active = nullptr;
		}
	};

	inline FString UniqueSuffix()
	{
		return FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	}

	inline UEdGraph* AddInterfaceFunction(UBlueprint* Interface, const FName Name, const bool bWithOutput, const bool bWithAmount = false, const bool bWithInput = false, const int32 OutputKind = 0)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Interface, Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (Graph == nullptr)
		{
			return nullptr;
		}
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Interface, Graph, true, nullptr);
		UK2Node_FunctionEntry* Entry = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Entry == nullptr) { Entry = Cast<UK2Node_FunctionEntry>(Node); }
		}
		if (Entry == nullptr)
		{
			return nullptr;
		}
		if (bWithInput)
		{
			FEdGraphPinType InType;
			InType.PinCategory = UEdGraphSchema_K2::PC_Real;
			InType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
			Entry->CreateUserDefinedPin(TEXT("In"), InType, EGPD_Output, false);
		}
		if (bWithAmount)
		{
			FEdGraphPinType IntType;
			IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
			Entry->CreateUserDefinedPin(TEXT("Amount"), IntType, EGPD_Output, false);
		}
		if (bWithOutput)
		{
			FEdGraphPinType DoubleType;
			DoubleType.PinCategory = UEdGraphSchema_K2::PC_Real;
			DoubleType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
			if (OutputKind == 1)
			{
				DoubleType = FEdGraphPinType();
				DoubleType.PinCategory = UEdGraphSchema_K2::PC_String;
			}
			else if (OutputKind == 2)
			{
				DoubleType = FEdGraphPinType();
				DoubleType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				DoubleType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
			}
			UK2Node_FunctionResult* Result = NewObject<UK2Node_FunctionResult>(Graph);
			Result->FunctionReference = Entry->FunctionReference;
			Result->CreateNewGuid();
			Result->AllocateDefaultPins();
			Graph->AddNode(Result, true, false);
			Result->CreateUserDefinedPin(TEXT("Value"), DoubleType, EGPD_Input, false);
		}
		return Graph;
	}

	/** A compiled BPI: GetValue (double output, function-style), Notify(Amount) (void, event-style) and an optional extra function-style function. */
	inline UBlueprint* MakeInterface(FScope& Scope, const TCHAR* Name, const TCHAR* FunctionStyle = TEXT("GetValue"), const TCHAR* EventStyle = TEXT("Notify"),
		const TCHAR* ExtraFunctionStyle = nullptr, const bool bFunctionInput = false, const int32 OutputKind = 0)
	{
		UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Game/Temp/%s_%s"), Name, *UniqueSuffix()));
		UBlueprint* Interface = FKismetEditorUtilities::CreateBlueprint(
			UInterface::StaticClass(), Package, FName(Name), BPTYPE_Interface, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (Interface == nullptr)
		{
			return nullptr;
		}
		Scope.Blueprints.Add(Interface);
		if (AddInterfaceFunction(Interface, FName(FunctionStyle), true, false, bFunctionInput, OutputKind) == nullptr
			|| AddInterfaceFunction(Interface, FName(EventStyle), false, true) == nullptr
			|| (ExtraFunctionStyle != nullptr && AddInterfaceFunction(Interface, FName(ExtraFunctionStyle), true) == nullptr))
		{
			return nullptr;
		}
		FKismetEditorUtilities::CompileBlueprint(Interface);
		return Interface;
	}

	struct FImplementer
	{
		UBlueprint* Blueprint = nullptr;
		UClass* OldInterface = nullptr;
		UEdGraph* FunctionGraph = nullptr;
		UK2Node_FunctionEntry* FunctionEntry = nullptr;
		UK2Node_FunctionResult* FunctionResult = nullptr;
		UK2Node_ExecutionSequence* FunctionBody = nullptr;
		UEdGraph* EventGraph = nullptr;
		UK2Node_Event* Event = nullptr;
		UK2Node_ExecutionSequence* EventFirst = nullptr;
		UK2Node_ExecutionSequence* EventSecond = nullptr;
		FGuid FunctionGraphGuid;
		FGuid FunctionEntryGuid;
		FGuid FunctionResultGuid;
		FGuid FunctionBodyGuid;
		FGuid EventGuid;
		FGuid EventFirstGuid;
		FGuid EventSecondGuid;
	};

	/**
	 * An actor Blueprint implementing the BPI: GetValue as an interface graph (entry -> body -> result) and Notify as an event
	 * whose exec output feeds a two-node body. Compiled.
	 */
	inline bool MakeImplementer(FScope& Scope, UBlueprint* Interface, const TCHAR* Name, FImplementer& Out, UClass* Parent = nullptr,
		const bool bWithFunctionBody = true, const TCHAR* PackageRoot = TEXT("/Game/Temp"), const TCHAR* FunctionName = TEXT("GetValue"),
		const TCHAR* ExtraFunctionStyle = nullptr)
	{
		UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s/%s_%s"), PackageRoot, Name, *UniqueSuffix()));
		Out.Blueprint = FKismetEditorUtilities::CreateBlueprint(
			Parent != nullptr ? Parent : AActor::StaticClass(), Package, FName(Name), BPTYPE_Normal, UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());
		if (Out.Blueprint == nullptr)
		{
			return false;
		}
		Scope.Blueprints.Add(Out.Blueprint);
		Out.OldInterface = Interface->GeneratedClass;
		if (!FBlueprintEditorUtils::ImplementNewInterface(Out.Blueprint, FTopLevelAssetPath(Out.OldInterface))
			|| Out.Blueprint->ImplementedInterfaces.Num() != 1 || Out.Blueprint->ImplementedInterfaces[0].Graphs.Num() < 1)
		{
			return false;
		}
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		const int32 ExpectedGraphs = ExtraFunctionStyle != nullptr ? 2 : 1;
		for (UEdGraph* Candidate : Out.Blueprint->ImplementedInterfaces[0].Graphs)
		{
			if (Candidate != nullptr && Candidate->GetFName() == FName(FunctionName))
			{
				Out.FunctionGraph = Candidate;
			}
		}
		if (Out.FunctionGraph == nullptr || Out.Blueprint->ImplementedInterfaces[0].Graphs.Num() != ExpectedGraphs)
		{
			return false;
		}
		for (UEdGraphNode* Node : Out.FunctionGraph->Nodes)
		{
			if (Out.FunctionEntry == nullptr) { Out.FunctionEntry = Cast<UK2Node_FunctionEntry>(Node); }
			if (Out.FunctionResult == nullptr) { Out.FunctionResult = Cast<UK2Node_FunctionResult>(Node); }
		}
		if (Out.FunctionEntry == nullptr || Out.FunctionResult == nullptr)
		{
			return false;
		}
		if (bWithFunctionBody)
		{
			UEdGraphPin* EntryThen = Out.FunctionEntry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
			EntryThen->BreakAllPinLinks();
			Out.FunctionBody = NewObject<UK2Node_ExecutionSequence>(Out.FunctionGraph);
			Out.FunctionBody->CreateNewGuid();
			Out.FunctionBody->AllocateDefaultPins();
			Out.FunctionGraph->AddNode(Out.FunctionBody, true, false);
			if (!Schema->TryCreateConnection(EntryThen, Out.FunctionBody->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input))
				|| !Schema->TryCreateConnection(Out.FunctionBody->FindPin(TEXT("then_0"), EGPD_Output), Out.FunctionResult->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input)))
			{
				return false;
			}
			Out.FunctionBodyGuid = Out.FunctionBody->NodeGuid;
		}
		Out.FunctionGraphGuid = Out.FunctionGraph->GraphGuid;
		Out.FunctionEntryGuid = Out.FunctionEntry->NodeGuid;
		Out.FunctionResultGuid = Out.FunctionResult->NodeGuid;

		Out.EventGraph = FBlueprintEditorUtils::FindEventGraph(Out.Blueprint);
		if (Out.EventGraph == nullptr)
		{
			return false;
		}
		Out.Event = NewObject<UK2Node_Event>(Out.EventGraph);
		Out.Event->EventReference.SetExternalMember(TEXT("Notify"), Out.OldInterface);
		Out.Event->bOverrideFunction = true;
		Out.Event->CreateNewGuid();
		Out.Event->AllocateDefaultPins();
		Out.EventGraph->AddNode(Out.Event, true, false);
		Out.EventFirst = NewObject<UK2Node_ExecutionSequence>(Out.EventGraph);
		Out.EventFirst->CreateNewGuid();
		Out.EventFirst->AllocateDefaultPins();
		Out.EventGraph->AddNode(Out.EventFirst, true, false);
		Out.EventSecond = NewObject<UK2Node_ExecutionSequence>(Out.EventGraph);
		Out.EventSecond->CreateNewGuid();
		Out.EventSecond->AllocateDefaultPins();
		Out.EventGraph->AddNode(Out.EventSecond, true, false);
		if (!Schema->TryCreateConnection(Out.Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output), Out.EventFirst->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input))
			|| !Schema->TryCreateConnection(Out.EventFirst->FindPin(TEXT("then_0"), EGPD_Output), Out.EventSecond->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input)))
		{
			return false;
		}
		Out.EventGuid = Out.Event->NodeGuid;
		Out.EventFirstGuid = Out.EventFirst->NodeGuid;
		Out.EventSecondGuid = Out.EventSecond->NodeGuid;
		FKismetEditorUtilities::CompileBlueprint(Out.Blueprint);
		return Out.Blueprint->Status != BS_Error;
	}

	/** An in-asset message call to the old interface function (the compile is off for the preview: its target is unconnected). */
	inline UK2Node_Message* AddMessageCaller(FImplementer& Impl, UClass* Interface)
	{
		FGraphNodeCreator<UK2Node_Message> Creator(*Impl.EventGraph);
		UK2Node_Message* Message = Creator.CreateNode();
		Message->FunctionReference.SetExternalMember(TEXT("GetValue"), Interface);
		Creator.Finalize();
		return Message;
	}

	inline TSharedPtr<FJsonObject> PinMapEntry(const TCHAR* Entry, const TCHAR* From, const TCHAR* To)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("entry"), Entry);
		Json->SetStringField(TEXT("from_pin"), From);
		Json->SetStringField(TEXT("to_pin"), To);
		return Json;
	}

	inline TArray<TSharedPtr<FJsonValue>> FunctionPinMap(const bool bWithResult = true)
	{
		TArray<TSharedPtr<FJsonValue>> Map;
		Map.Add(MakeShared<FJsonValueObject>(PinMapEntry(TEXT("output"), TEXT("then"), TEXT("then"))));
		Map.Add(MakeShared<FJsonValueObject>(PinMapEntry(TEXT("input"), TEXT("execute"), TEXT("execute"))));
		if (bWithResult)
		{
			Map.Add(MakeShared<FJsonValueObject>(PinMapEntry(TEXT("input"), TEXT("Value"), TEXT("ReturnValue"))));
		}
		return Map;
	}

	inline TArray<TSharedPtr<FJsonValue>> EventPinMap()
	{
		TArray<TSharedPtr<FJsonValue>> Map;
		Map.Add(MakeShared<FJsonValueObject>(PinMapEntry(TEXT("output"), TEXT("OutputDelegate"), TEXT("OutputDelegate"))));
		Map.Add(MakeShared<FJsonValueObject>(PinMapEntry(TEXT("output"), TEXT("then"), TEXT("then"))));
		Map.Add(MakeShared<FJsonValueObject>(PinMapEntry(TEXT("output"), TEXT("Amount"), TEXT("Amount"))));
		return Map;
	}

	inline TSharedPtr<FJsonValue> MapEntry(const TCHAR* From, const TCHAR* To, const TArray<TSharedPtr<FJsonValue>>& PinMap)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("from"), From);
		if (To != nullptr)
		{
			Json->SetStringField(TEXT("to"), To);
		}
		Json->SetArrayField(TEXT("pin_map"), PinMap);
		return MakeShared<FJsonValueObject>(Json);
	}

	/** GetValue and Notify mapped onto the native interface's functions of the same names. */
	inline TArray<TSharedPtr<FJsonValue>> FullFunctionMap()
	{
		TArray<TSharedPtr<FJsonValue>> Map;
		Map.Add(MapEntry(TEXT("GetValue"), nullptr, FunctionPinMap()));
		Map.Add(MapEntry(TEXT("Notify"), nullptr, EventPinMap()));
		return Map;
	}

	inline TSharedPtr<FJsonObject> MigrationJson(const FString& OldInterface, const FString& NewInterface, const TArray<TSharedPtr<FJsonValue>>& FunctionMap)
	{
		TSharedPtr<FJsonObject> Migration = MakeShared<FJsonObject>();
		Migration->SetStringField(TEXT("op"), TEXT("replace_interface"));
		Migration->SetStringField(TEXT("old_interface"), OldInterface);
		Migration->SetStringField(TEXT("new_interface"), NewInterface);
		Migration->SetArrayField(TEXT("function_map"), FunctionMap);
		return Migration;
	}

	inline FString NativePath()
	{
		return UCortexIssue122GraphInterface::StaticClass()->GetPathName();
	}

	/** A preview request (dry_run) of the interface swap; the fingerprint is the live one. */
	inline TSharedPtr<FJsonObject> Request(UBlueprint* Blueprint, const TCHAR* PatchId, const TSharedPtr<FJsonObject>& Migration, const bool bDryRun = true)
	{
		TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Request->SetStringField(TEXT("patch_id"), PatchId);
		Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Blueprint));
		Request->SetBoolField(TEXT("dry_run"), bDryRun);
		Request->SetBoolField(TEXT("compile"), true);
		Request->SetBoolField(TEXT("save"), false);
		Request->SetObjectField(TEXT("migration"), Migration);
		return Request;
	}

	/** Previews the request, then binds it to the preview's validation hash for the apply. */
	inline bool PreviewForApply(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Request, FCortexCommandResult& OutError)
	{
		FCortexGraphPreparedPatch Preview;
		if (!FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, OutError))
		{
			return false;
		}
		Request->SetBoolField(TEXT("dry_run"), false);
		Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);
		return true;
	}

	/** Everything the swap can change that the authoring fingerprint does not see: the interface list, graph names and InterfaceGuids. */
	inline FString StructureDigest(UBlueprint* Blueprint)
	{
		FString Digest = FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash"));
		for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
		{
			Digest += FString::Printf(TEXT("|I:%s"), Description.Interface ? *Description.Interface->GetPathName() : TEXT("none"));
			for (const UEdGraph* Graph : Description.Graphs)
			{
				Digest += FString::Printf(TEXT("|G:%s,%s,%s"), *Graph->GetName(), *Graph->GraphGuid.ToString(), *Graph->InterfaceGuid.ToString());
			}
		}
		return Digest;
	}

	inline int32 InterfaceCount(UBlueprint* Blueprint, const UClass* Interface)
	{
		int32 Count = 0;
		for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
		{
			Count += Description.Interface != nullptr && Description.Interface->GetAuthoritativeClass() == Interface->GetAuthoritativeClass() ? 1 : 0;
		}
		return Count;
	}

	inline const FBPInterfaceDescription* FindDescription(UBlueprint* Blueprint, const UClass* Interface)
	{
		for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
		{
			if (Description.Interface != nullptr && Description.Interface->GetAuthoritativeClass() == Interface->GetAuthoritativeClass())
			{
				return &Description;
			}
		}
		return nullptr;
	}

	inline bool LinkedTo(const UEdGraphPin* From, const UEdGraphNode* Node, const FName PinName)
	{
		const UEdGraphPin* To = Node ? Node->FindPin(PinName) : nullptr;
		return From && To && From->LinkedTo.Contains(To);
	}
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
