#include "Operations/CortexAnimLifecycleReport.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Operations/CortexAnimAssetUtils.h"

namespace CortexAnimLifecycleReportPrivate
{
	constexpr int32 MaxReportedCalls = 50;

	struct FHook
	{
		const TCHAR* Name;
		/** The native override point the hook corresponds to (AnimInstance.h); null when the engine declares none. */
		const TCHAR* NativeEquivalent;
	};

	/** The lifecycle hooks in report order, with their native equivalents (AnimInstance.h: NativeInitializeAnimation ... NativeBeginPlay). */
	const FHook Hooks[] = {
		{ TEXT("BlueprintInitializeAnimation"), TEXT("NativeInitializeAnimation") },
		{ TEXT("BlueprintUpdateAnimation"), TEXT("NativeUpdateAnimation") },
		{ TEXT("BlueprintThreadSafeUpdateAnimation"), TEXT("NativeThreadSafeUpdateAnimation") },
		{ TEXT("BlueprintPostEvaluateAnimation"), TEXT("NativePostEvaluateAnimation") },
		{ TEXT("BlueprintBeginPlay"), TEXT("NativeBeginPlay") },
		{ TEXT("BlueprintLinkedAnimationLayersInitialized"), TEXT("NativeLinkedAnimationLayersInitialized") },
	};

	FString GuidText(const FGuid& Guid)
	{
		return Guid.ToString(EGuidFormats::DigitsWithHyphens);
	}

	/** The name a function graph implements: its function entry's reference (so a renamed graph is still recognized), else its name. */
	FName ImplementedFunctionName(const UEdGraph* Graph)
	{
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
			{
				const FName Name = Entry->FunctionReference.GetMemberName();
				if (!Name.IsNone())
				{
					return Name;
				}
			}
		}
		return Graph->GetFName();
	}

	/** Called functions in the graph that are not thread safe (function or owning-class BlueprintThreadSafe metadata). */
	TArray<TSharedPtr<FJsonValue>> CollectNonThreadSafeCalls(const UEdGraph* Graph)
	{
		TArray<TSharedPtr<FJsonValue>> Calls;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
			const UFunction* Function = Call != nullptr ? Call->GetTargetFunction() : nullptr;
			if (Function == nullptr || FBlueprintEditorUtils::HasFunctionBlueprintThreadSafeMetaData(Function))
			{
				continue;
			}
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("node_guid"), GuidText(Node->NodeGuid));
			Entry->SetStringField(TEXT("node_name"), Node->GetName());
			Entry->SetStringField(TEXT("function_name"), Function->GetName());
			Entry->SetStringField(TEXT("class_name"), Function->GetOwnerClass() != nullptr ? Function->GetOwnerClass()->GetPathName() : FString());
			Calls.Add(MakeShared<FJsonValueObject>(Entry));
		}
		return Calls;
	}

	TSharedRef<FJsonObject> MakeEntry(const UAnimBlueprint* AnimBlueprint, const FHook& Hook, const UEdGraph* Graph, const UEdGraphNode* Node, const TCHAR* Kind)
	{
		const UFunction* ParentFunction = AnimBlueprint->ParentClass != nullptr ? AnimBlueprint->ParentClass->FindFunctionByName(Hook.Name) : nullptr;
		const bool bThreadSafeMetadata = ParentFunction != nullptr && FBlueprintEditorUtils::HasFunctionBlueprintThreadSafeMetaData(ParentFunction);
		const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph != nullptr ? Graph->GetSchema() : nullptr);
		const bool bGraphThreadSafe = Schema != nullptr && Schema->IsGraphMarkedThreadSafe(Graph);
		// The native hooks are plain C++ virtuals on UAnimInstance, not UFUNCTIONs (AnimInstance.h:1436-1457), so they cannot be
		// probed by reflection: every mapped name exists on the engine class by construction.

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Hook.Name);
		Entry->SetStringField(TEXT("kind"), Kind);
		Entry->SetStringField(TEXT("graph_guid"), Graph != nullptr ? GuidText(Graph->GraphGuid) : FString());
		Entry->SetStringField(TEXT("graph_name"), Graph != nullptr ? Graph->GetName() : FString());
		Entry->SetStringField(TEXT("node_guid"), Node != nullptr ? GuidText(Node->NodeGuid) : FString());
		Entry->SetBoolField(TEXT("thread_safe"), bThreadSafeMetadata || bGraphThreadSafe);
		Entry->SetBoolField(TEXT("thread_safe_metadata"), bThreadSafeMetadata);
		Entry->SetBoolField(TEXT("thread_safe_graph"), bGraphThreadSafe);
		Entry->SetStringField(TEXT("native_equivalent"), Hook.NativeEquivalent);
		Entry->SetNumberField(TEXT("node_count"), Graph != nullptr ? Graph->Nodes.Num() : 0);
		Entry->SetObjectField(TEXT("non_thread_safe_calls"), FCortexAnimAssetUtils::MakeLimitedArray(
			Graph != nullptr ? CollectNonThreadSafeCalls(Graph) : TArray<TSharedPtr<FJsonValue>>(), MaxReportedCalls));
		return Entry;
	}
}

bool FCortexAnimLifecycleReport::AppendLifecycle(UAnimBlueprint* AnimBlueprint, const int32 Limit, const TSharedPtr<FJsonObject>& Data, FCortexCommandResult& OutError)
{
	using namespace CortexAnimLifecycleReportPrivate;
	(void)OutError;

	TArray<TSharedPtr<FJsonValue>> Entries;
	for (const FHook& Hook : Hooks)
	{
		const FName HookName(Hook.Name);
		if (AnimBlueprint->ParentClass == nullptr || AnimBlueprint->ParentClass->FindFunctionByName(HookName) == nullptr)
		{
			// The parent does not declare this hook, so nothing here can implement it.
			continue;
		}

		// An event node (BlueprintImplementableEvent) in any graph.
		TArray<UEdGraph*> Graphs;
		AnimBlueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph == nullptr)
			{
				continue;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				const UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
				if (Event != nullptr && Event->GetFunctionName() == HookName)
				{
					Entries.Add(MakeShared<FJsonValueObject>(MakeEntry(AnimBlueprint, Hook, Graph, Node, TEXT("event"))));
				}
			}
		}

		// The override function graph (identified by its entry's function reference, so a renamed graph is still recognized).
		for (UEdGraph* Graph : AnimBlueprint->FunctionGraphs)
		{
			if (Graph != nullptr && ImplementedFunctionName(Graph) == HookName)
			{
				Entries.Add(MakeShared<FJsonValueObject>(MakeEntry(AnimBlueprint, Hook, Graph, nullptr, TEXT("function_override"))));
			}
		}
	}

	Data->SetObjectField(TEXT("lifecycle"), FCortexAnimAssetUtils::MakeLimitedArray(Entries, Limit));
	Data->SetBoolField(TEXT("uses_multithreaded_update"), UsesMultiThreadedUpdate(AnimBlueprint));
	return true;
}

bool FCortexAnimLifecycleReport::UsesMultiThreadedUpdate(const UAnimBlueprint* AnimBlueprint)
{
	return AnimBlueprint != nullptr && AnimBlueprint->bUseMultiThreadedAnimationUpdate;
}

void FCortexAnimLifecycleReport::GetHookNames(TArray<FString>& OutNames)
{
	OutNames.Reset();
	for (const CortexAnimLifecycleReportPrivate::FHook& Hook : CortexAnimLifecycleReportPrivate::Hooks)
	{
		OutNames.Add(Hook.Name);
	}
}
