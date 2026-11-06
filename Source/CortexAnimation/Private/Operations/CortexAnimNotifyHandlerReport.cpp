#include "Operations/CortexAnimNotifyHandlerReport.h"

#include "Animation/AnimBlueprint.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "Operations/CortexAnimAssetUtils.h"

const TCHAR* FCortexAnimNotifyHandlerReport::HandlerPrefix = TEXT("AnimNotify_");

namespace CortexAnimNotifyHandlerReportPrivate
{
	FString GuidText(const FGuid& Guid)
	{
		return Guid.ToString(EGuidFormats::DigitsWithHyphens);
	}

	/** The function a handler graph implements: its entry's function reference, else the graph name. */
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

	/** The engine accepts no parameters or exactly one object parameter that receives the UAnimNotify (AnimInstance.cpp:2088-2099). */
	bool IsSignatureCompatible(const UFunction* Function)
	{
		if (Function == nullptr)
		{
			return false;
		}
		if (Function->NumParms == 0)
		{
			return true;
		}
		if (Function->NumParms != 1)
		{
			return false;
		}
		TFieldIterator<FProperty> Parameter(Function);
		return Parameter && CastField<FObjectProperty>(*Parameter) != nullptr && Parameter->HasAnyPropertyFlags(CPF_Parm);
	}

	/** The handler's declared parameters: the skeleton class function when the Blueprint was compiled, else the node's own user pins. */
	int32 ParameterCount(const UFunction* BlueprintFunction, const UEdGraphNode* Node)
	{
		if (BlueprintFunction != nullptr)
		{
			return BlueprintFunction->NumParms;
		}
		const UK2Node_EditablePinBase* Editable = Cast<UK2Node_EditablePinBase>(Node);
		return Editable != nullptr ? Editable->UserDefinedPins.Num() : 0;
	}

	/** Signature compatibility for a handler that has no compiled function yet (its user pins are the signature). */
	bool IsPinSignatureCompatible(const UEdGraphNode* Node)
	{
		const UK2Node_EditablePinBase* Editable = Cast<UK2Node_EditablePinBase>(Node);
		if (Editable == nullptr)
		{
			return false;
		}
		const int32 Count = Editable->UserDefinedPins.Num();
		if (Count == 0)
		{
			return true;
		}
		if (Count != 1)
		{
			return false;
		}
		const FEdGraphPinType& PinType = Editable->UserDefinedPins[0]->PinType;
		return PinType.PinCategory == UEdGraphSchema_K2::PC_Object;
	}

	TSharedRef<FJsonObject> MakeEntry(const UAnimBlueprint* AnimBlueprint, const FName FunctionName, const UEdGraph* Graph, const UEdGraphNode* Node, const TCHAR* Kind)
	{
		const FName NotifyName(FunctionName.ToString().RightChop(FCortexAnimNotifyHandlerReport::HandlerPrefix ? FCString::Strlen(FCortexAnimNotifyHandlerReport::HandlerPrefix) : 0));
		const UFunction* BlueprintFunction = AnimBlueprint->SkeletonGeneratedClass != nullptr
			? AnimBlueprint->SkeletonGeneratedClass->FindFunctionByName(FunctionName) : nullptr;
		const UFunction* NativeFunction = AnimBlueprint->ParentClass != nullptr
			? AnimBlueprint->ParentClass->FindFunctionByName(FunctionName) : nullptr;

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("notify_name"), NotifyName.ToString());
		Entry->SetStringField(TEXT("function_name"), FunctionName.ToString());
		Entry->SetStringField(TEXT("kind"), Kind);
		Entry->SetStringField(TEXT("graph_guid"), Graph != nullptr ? GuidText(Graph->GraphGuid) : FString());
		Entry->SetStringField(TEXT("graph_name"), Graph != nullptr ? Graph->GetName() : FString());
		Entry->SetStringField(TEXT("node_guid"), Node != nullptr ? GuidText(Node->NodeGuid) : FString());
		Entry->SetNumberField(TEXT("parameter_count"), ParameterCount(BlueprintFunction, Node));
		Entry->SetBoolField(TEXT("native_declares"), NativeFunction != nullptr);
		Entry->SetStringField(TEXT("native_declared_in"), NativeFunction != nullptr && NativeFunction->GetOwnerClass() != nullptr
			? NativeFunction->GetOwnerClass()->GetPathName() : FString());
		Entry->SetBoolField(TEXT("native_signature_compatible"), BlueprintFunction != nullptr ? IsSignatureCompatible(BlueprintFunction) : IsPinSignatureCompatible(Node));
		Entry->SetBoolField(TEXT("conflict"), NativeFunction != nullptr);
		return Entry;
	}

	/** Deterministic order: by function name, then graph guid. */
	void SortEntries(TArray<TSharedPtr<FJsonValue>>& Entries)
	{
		Entries.Sort([](const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
		{
			const TSharedPtr<FJsonObject> LeftObject = Left->AsObject();
			const TSharedPtr<FJsonObject> RightObject = Right->AsObject();
			const FString LeftName = LeftObject->GetStringField(TEXT("function_name"));
			const FString RightName = RightObject->GetStringField(TEXT("function_name"));
			const int32 Compare = LeftName.Compare(RightName);
			return Compare != 0 ? Compare < 0 : LeftObject->GetStringField(TEXT("graph_guid")) < RightObject->GetStringField(TEXT("graph_guid"));
		});
	}
}

bool FCortexAnimNotifyHandlerReport::AppendNotifyHandlers(UAnimBlueprint* AnimBlueprint, const int32 Limit, const TSharedPtr<FJsonObject>& Data, FCortexCommandResult& OutError)
{
	using namespace CortexAnimNotifyHandlerReportPrivate;
	(void)OutError;

	TArray<TSharedPtr<FJsonValue>> Entries;
	TArray<UEdGraph*> Graphs;
	AnimBlueprint->GetAllGraphs(Graphs);
	for (const UEdGraph* Graph : Graphs)
	{
		if (Graph == nullptr)
		{
			continue;
		}
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			const UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
			if (Event == nullptr)
			{
				continue;
			}
			const FName FunctionName = Event->GetFunctionName();
			if (!FunctionName.ToString().StartsWith(HandlerPrefix))
			{
				continue;
			}
			Entries.Add(MakeShared<FJsonValueObject>(MakeEntry(AnimBlueprint, FunctionName, Graph, Node,
				Event->IsA<UK2Node_CustomEvent>() ? TEXT("custom_event") : TEXT("event"))));
		}
	}

	// Function graphs that implement a handler (nested graphs are covered by GetAllGraphs above).
	for (const UEdGraph* Graph : AnimBlueprint->FunctionGraphs)
	{
		if (Graph == nullptr)
		{
			continue;
		}
		const FName FunctionName = ImplementedFunctionName(Graph);
		if (!FunctionName.ToString().StartsWith(HandlerPrefix))
		{
			continue;
		}
		Entries.Add(MakeShared<FJsonValueObject>(MakeEntry(AnimBlueprint, FunctionName, Graph, nullptr, TEXT("function_override"))));
	}

	SortEntries(Entries);
	Data->SetObjectField(TEXT("notify_handlers"), FCortexAnimAssetUtils::MakeLimitedArray(Entries, Limit));
	return true;
}
