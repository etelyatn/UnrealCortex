#include "Operations/CortexSTInspectOps.h"

#include "CortexCommandRouter.h"
#include "CortexSTTypes.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTree.h"
#include "CortexSerializer.h"
#include "GameplayTagContainer.h"
#include "Misc/EngineVersionComparison.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyOptional.h"
#include <algorithm>
#include <initializer_list>

namespace
{
// Compound branches own their diagnostics until append. Raw map keys can
// contain dots/brackets, so flattened diagnostic prefixes are not ownership.
// Walk stored compounds once; shared leaf policies retain text/object/issue
// semantics without first traversing the same complete reflected value twice.
FCortexPropertySerializationResult ReadStoredValue(const FProperty* Property, const void* Memory,
	const FString& Path, int32 Depth = 0);

void AppendStoredIssues(FCortexPropertySerializationResult& Result, FCortexPropertySerializationResult& Branch)
{
	Result.bPartial |= Branch.bPartial;
	Result.Issues.Append(MoveTemp(Branch.Issues));
}

FCortexPropertySerializationResult ReadStoredStruct(const UStruct* Type, const void* Memory,
	const FString& Path, int32 Depth)
{
	FCortexPropertySerializationResult Result;
	if (Depth > 32)
	{
		Result.JsonValue = MakeShared<FJsonValueNull>();
		Result.bPartial = true;
		Result.Issues.Add({Path, TEXT("Maximum serialization depth exceeded"),
			TEXT("MAX_DEPTH_EXCEEDED"), ECortexSerializationSeverity::Error, true, true});
		return Result;
	}
	if (Type == FInstancedStruct::StaticStruct() && Memory)
	{
		const FInstancedStruct& Instance = *static_cast<const FInstancedStruct*>(Memory);
		if (Instance.IsValid())
		{
			Result = ReadStoredStruct(Instance.GetScriptStruct(), Instance.GetMemory(), Path, Depth + 1);
			if (Result.JsonValue->Type == EJson::Object)
			{
				Result.JsonValue->AsObject()->SetStringField(TEXT("_struct_type"), Instance.GetScriptStruct()->GetName());
			}
			return Result;
		}
	}
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Result.JsonValue = MakeShared<FJsonValueObject>(Object);
	if (!Type || !Memory || Type == FInstancedStruct::StaticStruct()) { return Result; }
	for (TFieldIterator<FProperty> It(Type); It; ++It)
	{
		const FProperty* Member = *It;
		const FString MemberPath = Path + TEXT(".") + Member->GetName();
		if (Member->ArrayDim == 1)
		{
			FCortexPropertySerializationResult Value = ReadStoredValue(Member,
				Member->ContainerPtrToValuePtr<void>(Memory), MemberPath, Depth);
			Object->SetField(Member->GetName(), Value.JsonValue);
			AppendStoredIssues(Result, Value);
		}
		else
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Reserve(Member->ArrayDim);
			for (int32 Index = 0; Index < Member->ArrayDim; ++Index)
			{
				FCortexPropertySerializationResult Value = ReadStoredValue(Member,
					Member->ContainerPtrToValuePtr<void>(Memory, Index),
					FString::Printf(TEXT("%s[%d]"), *MemberPath, Index), Depth);
				Values.Add(MoveTemp(Value.JsonValue));
				AppendStoredIssues(Result, Value);
			}
			Object->SetField(Member->GetName(), MakeShared<FJsonValueArray>(MoveTemp(Values)));
		}
	}
	return Result;
}

FCortexPropertySerializationResult ReadStoredValue(const FProperty* Property, const void* Memory,
	const FString& Path, int32 Depth)
{
	FCortexSerializationPolicy Policy;
	Policy.MaxDepth = 32 - Depth;
	Policy.bExpandInstancedSubobjects = false;
	if (!Property || !Memory || Depth > 32)
	{
		return FCortexSerializer::PropertyToJsonDeep(Property, Memory, Policy, Path);
	}
	FCortexPropertySerializationResult Result;
	if (const FInt8Property* SignedByte = CastField<FInt8Property>(Property))
	{
		Result.JsonValue = MakeShared<FJsonValueNumber>(SignedByte->GetPropertyValue(Memory));
	}
	else if (const FUInt16Property* UnsignedShort = CastField<FUInt16Property>(Property))
	{
		Result.JsonValue = MakeShared<FJsonValueNumber>(UnsignedShort->GetPropertyValue(Memory));
	}
	else if (const FUInt32Property* UnsignedInteger = CastField<FUInt32Property>(Property))
	{
		Result.JsonValue = MakeShared<FJsonValueNumber>(static_cast<double>(UnsignedInteger->GetPropertyValue(Memory)));
	}
	else if (const FInt64Property* SignedInteger = CastField<FInt64Property>(Property))
	{
		const int64 Value = SignedInteger->GetPropertyValue(Memory);
		constexpr int64 SafeInteger = 9007199254740991LL;
		if (Value < -SafeInteger || Value > SafeInteger)
		{
			Result.JsonValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), static_cast<long long>(Value)));
		}
		else { Result.JsonValue = MakeShared<FJsonValueNumber>(static_cast<double>(Value)); }
	}
	else if (const FOptionalProperty* Optional = CastField<FOptionalProperty>(Property))
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		const void* Stored = Optional->GetValuePointerForReadIfSet(Memory);
		Object->SetBoolField(TEXT("is_set"), Stored != nullptr);
		if (Stored)
		{
			FCortexPropertySerializationResult Value = ReadStoredValue(
				Optional->GetValueProperty(), Stored, Path + TEXT(".value"), Depth + 1);
			Object->SetField(TEXT("value"), Value.JsonValue);
			AppendStoredIssues(Result, Value);
		}
		Result.JsonValue = MakeShared<FJsonValueObject>(Object);
	}
	else if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
	{
		if (Struct->Struct == FGameplayTag::StaticStruct() || Struct->Struct == FGameplayTagContainer::StaticStruct()
			|| Struct->Struct == TBaseStructure<FSoftObjectPath>::Get())
		{
			return FCortexSerializer::PropertyToJsonDeep(Property, Memory, Policy, Path);
		}
		if (Struct->Struct == FInstancedStruct::StaticStruct())
		{
			const FInstancedStruct& Instance = *static_cast<const FInstancedStruct*>(Memory);
			if (!Instance.IsValid()) { Result.JsonValue = MakeShared<FJsonValueNull>(); return Result; }
			Result = ReadStoredStruct(Instance.GetScriptStruct(), Instance.GetMemory(), Path, Depth + 1);
			if (Result.JsonValue->Type == EJson::Object)
			{
				Result.JsonValue->AsObject()->SetStringField(TEXT("_struct_type"), Instance.GetScriptStruct()->GetName());
			}
		}
		else { Result = ReadStoredStruct(Struct->Struct, Memory, Path, Depth + 1); }
	}
	else if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
	{
		FScriptArrayHelper Helper(Array, Memory);
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(Helper.Num());
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			FCortexPropertySerializationResult Value = ReadStoredValue(Array->Inner, Helper.GetRawPtr(Index),
				FString::Printf(TEXT("%s[%d]"), *Path, Index), Depth + 1);
			Values.Add(MoveTemp(Value.JsonValue));
			AppendStoredIssues(Result, Value);
		}
		Result.JsonValue = MakeShared<FJsonValueArray>(MoveTemp(Values));
	}
	else if (const FSetProperty* Set = CastField<FSetProperty>(Property))
	{
		FScriptSetHelper Helper(Set, Memory);
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(Helper.Num());
		for (int32 SparseIndex = 0; SparseIndex < Helper.GetMaxIndex(); ++SparseIndex)
		{
			if (!Helper.IsValidIndex(SparseIndex)) { continue; }
			FCortexPropertySerializationResult Value = ReadStoredValue(Set->ElementProp, Helper.GetElementPtr(SparseIndex),
				FString::Printf(TEXT("%s[%d]"), *Path, Values.Num()), Depth + 1);
			Values.Add(MoveTemp(Value.JsonValue));
			AppendStoredIssues(Result, Value);
		}
		if (!Values.IsEmpty())
		{
			Result.Issues.Add({Path, TEXT("Set order follows Unreal set iteration order"),
				TEXT("NON_DETERMINISTIC_SET_ORDER"), ECortexSerializationSeverity::Warning, true, false});
			Result.bPartial = true;
		}
		Result.JsonValue = MakeShared<FJsonValueArray>(MoveTemp(Values));
	}
	else if (const FMapProperty* Map = CastField<FMapProperty>(Property))
	{
		FScriptMapHelper Helper(Map, Memory);
		const bool bStringKeys = CastField<FStrProperty>(Map->KeyProp) || CastField<FNameProperty>(Map->KeyProp);
		TArray<TPair<FString, int32>> Keys;
		bool bCollision = false;
		if (bStringKeys)
		{
			Keys.Reserve(Helper.Num());
			TSet<FString> Seen;
			Seen.Reserve(Helper.Num());
			for (int32 SparseIndex = 0; SparseIndex < Helper.GetMaxIndex(); ++SparseIndex)
			{
				if (!Helper.IsValidIndex(SparseIndex)) { continue; }
				FString Key;
				Map->KeyProp->ExportTextItem_Direct(Key, Helper.GetKeyPtr(SparseIndex), nullptr, nullptr, PPF_None);
				bool bAlreadySeen = false;
				Seen.Add(Key, &bAlreadySeen);
				bCollision |= bAlreadySeen;
				Keys.Emplace(MoveTemp(Key), SparseIndex);
			}
		}
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		if (bStringKeys && !bCollision)
		{
			for (const TPair<FString, int32>& Key : Keys)
			{
				FCortexPropertySerializationResult Value = ReadStoredValue(Map->ValueProp,
					Helper.GetValuePtr(Key.Value), Path + TEXT(".") + Key.Key, Depth + 1);
				Object->SetField(Key.Key, Value.JsonValue);
				AppendStoredIssues(Result, Value);
			}
		}
		else
		{
			TArray<TSharedPtr<FJsonValue>> Entries;
			Entries.Reserve(Helper.Num());
			for (int32 SparseIndex = 0; SparseIndex < Helper.GetMaxIndex(); ++SparseIndex)
			{
				if (!Helper.IsValidIndex(SparseIndex)) { continue; }
				const FString EntryPath = FString::Printf(TEXT("%s[%d]"), *Path, Entries.Num());
				FCortexPropertySerializationResult Key = ReadStoredValue(Map->KeyProp,
					Helper.GetKeyPtr(SparseIndex), EntryPath + TEXT(".key"), Depth + 1);
				FCortexPropertySerializationResult Value = ReadStoredValue(Map->ValueProp,
					Helper.GetValuePtr(SparseIndex), EntryPath + TEXT(".value"), Depth + 1);
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetField(TEXT("key"), Key.JsonValue);
				Entry->SetField(TEXT("value"), Value.JsonValue);
				AppendStoredIssues(Result, Key);
				AppendStoredIssues(Result, Value);
				Entries.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Object->SetField(TEXT("entries"), MakeShared<FJsonValueArray>(MoveTemp(Entries)));
		}
		Result.JsonValue = MakeShared<FJsonValueObject>(Object);
	}
	else { return FCortexSerializer::PropertyToJsonDeep(Property, Memory, Policy, Path); }
	return Result;
}

TSharedPtr<FJsonObject> InspectFields(const UStruct* Type, const void* Memory, std::initializer_list<FName> Excluded = {})
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	if (Type == nullptr || Memory == nullptr)
	{
		Result->SetBoolField(TEXT("available"), false);
		return Result;
	}
	Result->SetBoolField(TEXT("available"), true);
	Result->SetStringField(TEXT("type_path"), Type->GetPathName());
	TArray<TSharedPtr<FJsonValue>> Fields;
	for (TFieldIterator<FProperty> It(Type); It; ++It)
	{
		const FProperty* Property = *It;
		if (std::find(Excluded.begin(), Excluded.end(), Property->GetFName()) != Excluded.end())
		{
			continue;
		}
		TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
		Field->SetStringField(TEXT("name"), Property->GetName());
		Field->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
		Field->SetStringField(TEXT("origin"), Property->GetOwnerStruct()->GetPathName());
		Field->SetStringField(TEXT("property_flags"), FString::Printf(TEXT("%llu"), static_cast<unsigned long long>(Property->GetPropertyFlags())));
		TArray<FCortexSerializationIssue> Issues;
		if (Property->ArrayDim == 1)
		{
			FCortexPropertySerializationResult Value = ReadStoredValue(
				Property, Property->ContainerPtrToValuePtr<void>(Memory), Property->GetName());
			Field->SetField(TEXT("value"), Value.JsonValue);
			Issues = MoveTemp(Value.Issues);
		}
		else
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Reserve(Property->ArrayDim);
			for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
			{
				FCortexPropertySerializationResult Value = ReadStoredValue(Property,
					Property->ContainerPtrToValuePtr<void>(Memory, Index),
					FString::Printf(TEXT("%s[%d]"), *Property->GetName(), Index));
				Values.Add(Value.JsonValue);
				Issues.Append(Value.Issues);
			}
			Field->SetField(TEXT("value"), MakeShared<FJsonValueArray>(MoveTemp(Values)));
		}
		Field->SetBoolField(TEXT("partial"), !Issues.IsEmpty());
		Field->SetArrayField(TEXT("issues"), FCortexSerializer::SerializationIssuesToJson(Issues));
		Fields.Add(MakeShared<FJsonValueObject>(Field));
	}
	Result->SetArrayField(TEXT("fields"), Fields);
	return Result;
}

TSharedPtr<FJsonObject> InspectBag(const FInstancedPropertyBag& Bag)
{
	const FConstStructView View = Bag.GetValue();
	return InspectFields(View.GetScriptStruct(), View.GetMemory());
}

TSharedPtr<FJsonObject> InspectStoredObject(const UObject* Object)
{
	TSharedPtr<FJsonObject> Values = InspectFields(Object ? Object->GetClass() : nullptr, Object);
	if (Object) { Values->SetStringField(TEXT("object_path"), Object->GetPathName()); }
	return Values;
}

TSharedPtr<FJsonObject> InspectNode(const FStateTreeEditorNode& Node, const TCHAR* Kind, int32 Index)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("id"), Node.ID.ToString(EGuidFormats::DigitsWithHyphens));
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	Result->SetBoolField(TEXT("definition_id_available"), false);
#else
	Result->SetBoolField(TEXT("definition_id_available"), true);
	Result->SetStringField(TEXT("definition_id"), Node.GetNodeID().ToString(EGuidFormats::DigitsWithHyphens));
#endif
	Result->SetStringField(TEXT("kind"), Kind);
	Result->SetNumberField(TEXT("index"), Index);
	Result->SetObjectField(TEXT("editor_node"), InspectFields(FStateTreeEditorNode::StaticStruct(), &Node,
		{TEXT("Node"), TEXT("Instance"), TEXT("InstanceObject"), TEXT("ExecutionRuntimeData"), TEXT("ExecutionRuntimeDataObject")}));
	Result->SetObjectField(TEXT("definition"), InspectFields(Node.Node.GetScriptStruct(), Node.Node.GetMemory()));
	Result->SetObjectField(TEXT("instance_struct"), InspectFields(Node.Instance.GetScriptStruct(), Node.Instance.GetMemory()));
	Result->SetObjectField(TEXT("instance_object"), InspectStoredObject(Node.InstanceObject.Get()));
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	constexpr bool bRuntimeMemberAvailable = false;
	TSharedPtr<FJsonObject> RuntimeStruct = InspectFields(nullptr, nullptr);
	TSharedPtr<FJsonObject> RuntimeObject = InspectStoredObject(nullptr);
#else
	constexpr bool bRuntimeMemberAvailable = true;
	TSharedPtr<FJsonObject> RuntimeStruct = InspectFields(Node.ExecutionRuntimeData.GetScriptStruct(), Node.ExecutionRuntimeData.GetMemory());
	TSharedPtr<FJsonObject> RuntimeObject = InspectStoredObject(Node.ExecutionRuntimeDataObject.Get());
#endif
	RuntimeStruct->SetBoolField(TEXT("engine_member_available"), bRuntimeMemberAvailable);
	RuntimeObject->SetBoolField(TEXT("engine_member_available"), bRuntimeMemberAvailable);
	Result->SetObjectField(TEXT("execution_runtime_struct"), RuntimeStruct);
	Result->SetObjectField(TEXT("execution_runtime_object"), RuntimeObject);
	return Result;
}

TArray<TSharedPtr<FJsonValue>> InspectNodes(const TArray<FStateTreeEditorNode>& Nodes, const TCHAR* Kind)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Nodes.Num());
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		Result.Add(MakeShared<FJsonValueObject>(InspectNode(Nodes[Index], Kind, Index)));
	}
	return Result;
}

constexpr const TCHAR* InspectionCompleteness = TEXT("Reflected stored fields with per-field partial/issues. UObject references identity-only; instance objects expanded one explicit level. Depth limit 32, no array count truncation. Signed64 outside +/-9007199254740991 uses lossless decimal strings; cpp_type retains the stored type. definition_id_available and runtime engine_member_available disclose engine-version API availability, distinct from a present but empty stored slot. Non-reflected engine caches not captured.");

void AddInspectionMetadata(const FCortexSTAssetContext& Context, const TSharedPtr<FJsonObject>& Result)
{
	Result->SetStringField(TEXT("schema"), TEXT("cortex.stored-editor-inspection.v1"));
	Result->SetStringField(TEXT("asset_path"), Context.AssetPath);
	Result->SetStringField(TEXT("scope"), TEXT("All stored editor subtrees; external linked assets are identity references only. No runtime execution or class-default substitution."));
	Result->SetStringField(TEXT("enabled_origin"), TEXT("definition.fields.bTaskEnabled where present; other node types have only their actual reflected flags, no synthetic enabled default"));
	Result->SetStringField(TEXT("completeness"), InspectionCompleteness);
	Result->SetBoolField(TEXT("package_dirty_before"), Context.StateTree->GetOutermost()->IsDirty());
}

void AddRootInspection(const FCortexSTAssetContext& Context, const TSharedPtr<FJsonObject>& Result)
{
	Result->SetObjectField(TEXT("root"), InspectFields(Context.EditorData->GetClass(), Context.EditorData,
		{TEXT("SubTrees"), TEXT("Evaluators"), TEXT("GlobalTasks"), TEXT("EditorBindings"), TEXT("RootParameterPropertyBag"), TEXT("RootParameters")}));
	Result->SetStringField(TEXT("root_parameters_id"), Context.EditorData->GetRootParametersGuid().ToString(EGuidFormats::DigitsWithHyphens));
	Result->SetObjectField(TEXT("root_parameters"), InspectBag(Context.EditorData->GetRootParametersPropertyBag()));
	TArray<TSharedPtr<FJsonValue>> Roots;
	Roots.Reserve(Context.EditorData->SubTrees.Num());
	for (const UStateTreeState* Root : Context.EditorData->SubTrees)
	{
		if (Root)
		{
			Roots.Add(MakeShared<FJsonValueString>(Root->ID.ToString(EGuidFormats::DigitsWithHyphens)));
		}
		else
		{
			Roots.Add(MakeShared<FJsonValueNull>());
		}
	}
	Result->SetArrayField(TEXT("subtree_roots"), MoveTemp(Roots));
}

TSharedPtr<FJsonObject> InspectState(const UStateTreeState& State, int32 RootIndex, bool bIncludeNodes)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("id"), State.ID.ToString(EGuidFormats::DigitsWithHyphens));
	Result->SetStringField(TEXT("object_path"), State.GetPathName());
	Result->SetNumberField(TEXT("subtree_index"), RootIndex);
	Result->SetObjectField(TEXT("properties"), InspectFields(State.GetClass(), &State,
		{TEXT("Tasks"), TEXT("SingleTask"), TEXT("EnterConditions"), TEXT("Considerations"), TEXT("Transitions"), TEXT("Parameters")}));
	Result->SetObjectField(TEXT("parameters_metadata"), InspectFields(FStateTreeStateParameters::StaticStruct(), &State.Parameters, {TEXT("Parameters")}));
	Result->SetObjectField(TEXT("parameters"), InspectBag(State.Parameters.Parameters));
	if (bIncludeNodes)
	{
		Result->SetArrayField(TEXT("tasks"), InspectNodes(State.Tasks, TEXT("task")));
		Result->SetObjectField(TEXT("single_task"), InspectNode(State.SingleTask, TEXT("single_task"), 0));
		Result->SetArrayField(TEXT("enter_conditions"), InspectNodes(State.EnterConditions, TEXT("enter_condition")));
		Result->SetArrayField(TEXT("considerations"), InspectNodes(State.Considerations, TEXT("consideration")));
	}
	TArray<TSharedPtr<FJsonValue>> Transitions;
	Transitions.Reserve(State.Transitions.Num());
	for (int32 Index = 0; Index < State.Transitions.Num(); ++Index)
	{
		const FStateTreeTransition& Transition = State.Transitions[Index];
		TSharedPtr<FJsonObject> Value = InspectFields(FStateTreeTransition::StaticStruct(), &Transition, {TEXT("Conditions")});
		Value->SetNumberField(TEXT("index"), Index);
		if (bIncludeNodes)
		{
			Value->SetArrayField(TEXT("conditions"), InspectNodes(Transition.Conditions, TEXT("transition_condition")));
		}
		Transitions.Add(MakeShared<FJsonValueObject>(Value));
	}
	Result->SetArrayField(TEXT("transitions"), MoveTemp(Transitions));
	return Result;
}

TSharedPtr<FJsonObject> InspectTree(const FCortexSTAssetContext& Context)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	AddInspectionMetadata(Context, Result);
	AddRootInspection(Context, Result);
	Result->SetArrayField(TEXT("evaluators"), InspectNodes(Context.EditorData->Evaluators, TEXT("evaluator")));
	Result->SetArrayField(TEXT("global_tasks"), InspectNodes(Context.EditorData->GlobalTasks, TEXT("global_task")));
	Result->SetObjectField(TEXT("bindings"), InspectFields(FStateTreeEditorPropertyBindings::StaticStruct(), &Context.EditorData->EditorBindings));
	TArray<FCortexSTStateRef> States;
	CortexST::CollectAllStates(Context, States, false);
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Reserve(States.Num());
	int32 RootIndex = INDEX_NONE;
	for (const FCortexSTStateRef& State : States)
	{
		if (State.Parent == nullptr)
		{
			RootIndex = Context.EditorData->SubTrees.IndexOfByKey(State.State);
		}
		Values.Add(MakeShared<FJsonValueObject>(InspectState(*State.State, RootIndex, true)));
	}
	Result->SetArrayField(TEXT("states"), MoveTemp(Values));
	Result->SetBoolField(TEXT("package_dirty_after"), Context.StateTree->GetOutermost()->IsDirty());
	return Result;
}

struct FInspectionRequest
{
	bool bEnabled = false;
	FString Section;
	int32 Offset = 0;
	int32 Count = 1;
};

bool ParseInspectionRequest(const TSharedPtr<FJsonObject>& Params, FInspectionRequest& Request, FCortexCommandResult& Error)
{
	auto Invalid = [&Error](const TCHAR* Message)
	{
		Error = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, Message);
		return false;
	};
	if (Params->HasField(TEXT("inspect_instances")))
	{
		if (!Params->HasTypedField<EJson::Boolean>(TEXT("inspect_instances"))
			|| !Params->TryGetBoolField(TEXT("inspect_instances"), Request.bEnabled))
		{
			return Invalid(TEXT("inspect_instances must be a boolean"));
		}
	}
	const bool bSection = Params->HasField(TEXT("inspect_section"));
	if (bSection)
	{
		if (!Params->HasTypedField<EJson::String>(TEXT("inspect_section"))
			|| !Params->TryGetStringField(TEXT("inspect_section"), Request.Section)
			|| (Request.Section != TEXT("root") && Request.Section != TEXT("states")
				&& Request.Section != TEXT("nodes") && Request.Section != TEXT("bindings")))
		{
			return Invalid(TEXT("inspect_section must be root, states, nodes or bindings"));
		}
	}
	for (const bool bCount : {false, true})
	{
		const TCHAR* Name = bCount ? TEXT("inspect_count") : TEXT("inspect_offset");
		if (Params->HasField(Name))
		{
			double Value = 0;
			if (!Params->HasTypedField<EJson::Number>(Name) || !Params->TryGetNumberField(Name, Value)
				|| !FMath::IsFinite(Value) || Value != FMath::FloorToDouble(Value)
				|| Value < (bCount ? 1 : 0) || Value > (bCount ? 100 : MAX_int32))
			{
				return Invalid(TEXT("Inspection offset/count must be bounded nonnegative/positive integer JSON numbers"));
			}
			(bCount ? Request.Count : Request.Offset) = static_cast<int32>(Value);
		}
	}
	if ((bSection || Params->HasField(TEXT("inspect_offset")) || Params->HasField(TEXT("inspect_count")))
		&& (!Request.bEnabled || !bSection))
	{
		return Invalid(TEXT("Inspection paging controls require inspect_instances=true and a named inspect_section"));
	}
	return true;
}

struct FStoredNodeRef
{
	const FStateTreeEditorNode* Node;
	const TCHAR* Kind;
	const UStateTreeState* Owner;
	int32 Index;
	int32 TransitionIndex;
};

void AppendNodeRefs(const TArray<FStateTreeEditorNode>& Nodes, const TCHAR* Kind, const UStateTreeState* Owner,
	TArray<FStoredNodeRef>& Result, int32 TransitionIndex = INDEX_NONE)
{
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		Result.Add({&Nodes[Index], Kind, Owner, Index, TransitionIndex});
	}
}

FCortexCommandResult InspectPage(const FCortexSTAssetContext& Context, const FInspectionRequest& Request)
{
	TArray<FCortexSTStateRef> States;
	TArray<FStoredNodeRef> Nodes;
	const bool bStates = Request.Section == TEXT("states");
	const bool bNodes = Request.Section == TEXT("nodes");
	const bool bBindings = Request.Section == TEXT("bindings");
	if (bStates || bNodes)
	{
		CortexST::CollectAllStates(Context, States, false);
	}
	if (bNodes)
	{
		for (const FCortexSTStateRef& State : States)
		{
			AppendNodeRefs(State.State->Tasks, TEXT("task"), State.State, Nodes);
			AppendNodeRefs(State.State->EnterConditions, TEXT("enter_condition"), State.State, Nodes);
			AppendNodeRefs(State.State->Considerations, TEXT("consideration"), State.State, Nodes);
			// A single task is a real stored slot whenever any of its body slots
			// holds actual data, even if its definition is missing.
			const FStateTreeEditorNode& SingleTask = State.State->SingleTask;
			bool bSingleTaskBody = SingleTask.Node.IsValid() || SingleTask.Instance.IsValid()
				|| SingleTask.InstanceObject != nullptr;
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
			bSingleTaskBody = bSingleTaskBody || SingleTask.ExecutionRuntimeData.IsValid()
				|| SingleTask.ExecutionRuntimeDataObject != nullptr;
#endif
			if (bSingleTaskBody)
			{
				Nodes.Add({&State.State->SingleTask, TEXT("single_task"), State.State, 0, INDEX_NONE});
			}
			for (int32 Index = 0; Index < State.State->Transitions.Num(); ++Index)
			{
				AppendNodeRefs(State.State->Transitions[Index].Conditions, TEXT("transition_condition"), State.State, Nodes, Index);
			}
		}
		AppendNodeRefs(Context.EditorData->Evaluators, TEXT("evaluator"), nullptr, Nodes);
		AppendNodeRefs(Context.EditorData->GlobalTasks, TEXT("global_task"), nullptr, Nodes);
	}
	const auto Bindings = Context.EditorData->EditorBindings.GetBindings();
	const int32 Total = bStates ? States.Num() : bNodes ? Nodes.Num() : bBindings ? Bindings.Num() : 1;
	if (Request.Offset > Total)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("inspect_offset exceeds the section total"));
	}
	const int32 Returned = FMath::Min(Request.Count, Total - Request.Offset);
	const int32 End = Request.Offset + Returned;
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	AddInspectionMetadata(Context, Result);
	TArray<TSharedPtr<FJsonValue>> Entries;
	Entries.Reserve(Returned);
	if (bStates)
	{
		int32 RootIndex = INDEX_NONE;
		for (int32 Index = 0; Index < End; ++Index)
		{
			const FCortexSTStateRef& State = States[Index];
			if (State.Parent == nullptr)
			{
				RootIndex = Context.EditorData->SubTrees.IndexOfByKey(State.State);
			}
			if (Index >= Request.Offset)
			{
				Entries.Add(MakeShared<FJsonValueObject>(InspectState(*State.State, RootIndex, false)));
			}
		}
	}
	else
	{
		for (int32 Index = Request.Offset; Index < End; ++Index)
		{
			TSharedPtr<FJsonObject> Entry;
			if (bNodes)
			{
				const FStoredNodeRef& Node = Nodes[Index];
				Entry = InspectNode(*Node.Node, Node.Kind, Node.Index);
				if (Node.Owner)
				{
					Entry->SetStringField(TEXT("owner_state_id"), Node.Owner->ID.ToString(EGuidFormats::DigitsWithHyphens));
				}
				if (Node.TransitionIndex != INDEX_NONE)
				{
					Entry->SetNumberField(TEXT("owner_transition_index"), Node.TransitionIndex);
				}
			}
			else if (bBindings)
			{
				Entry = InspectFields(FStateTreePropertyPathBinding::StaticStruct(), &Bindings[Index]);
			}
			else
			{
				Entry = MakeShared<FJsonObject>();
				AddRootInspection(Context, Entry);
			}
			Entries.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}
	Result->SetStringField(TEXT("section"), Request.Section);
	Result->SetNumberField(TEXT("total"), Total);
	Result->SetNumberField(TEXT("offset"), Request.Offset);
	Result->SetNumberField(TEXT("returned_count"), Returned);
	Result->SetBoolField(TEXT("has_more"), End < Total);
	Result->SetArrayField(TEXT("entries"), MoveTemp(Entries));
	Result->SetBoolField(TEXT("package_dirty_after"), Context.StateTree->GetOutermost()->IsDirty());
	return FCortexCommandRouter::Success(Result);
}
} // namespace

FCortexCommandResult FCortexSTInspectOps::DumpTree(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FCortexCommandResult Error;
	if (!CortexST::GetRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error;
	}
	FInspectionRequest Inspection;
	if (!ParseInspectionRequest(Params, Inspection, Error))
	{
		return Error;
	}

	FCortexSTAssetContext Context;
	if (!CortexST::LoadAssetContext(AssetPath, Context, Error))
	{
		return Error;
	}

	if (Inspection.bEnabled)
	{
		return Inspection.Section.IsEmpty()
			? FCortexCommandRouter::Success(InspectTree(Context))
			: InspectPage(Context, Inspection);
	}

	const bool bIncludeTransitions = CortexST::GetOptionalBool(Params, TEXT("include_transitions"), true);
	const bool bIncludeNodes = CortexST::GetOptionalBool(Params, TEXT("include_nodes"), false);

	TArray<FCortexSTStateRef> States;
	CortexST::CollectAllStates(Context, States);

	TArray<TSharedPtr<FJsonValue>> SerializedStates;
	SerializedStates.Reserve(States.Num());
	for (const FCortexSTStateRef& StateRef : States)
	{
		SerializedStates.Add(MakeShared<FJsonValueObject>(
			CortexST::SerializeState(StateRef, bIncludeTransitions, bIncludeNodes)));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Context.AssetPath);
	Data->SetArrayField(TEXT("states"), SerializedStates);
	Data->SetObjectField(TEXT("validation"), CortexST::BuildValidationPayload(Context.StateTree));
	Data->SetObjectField(TEXT("fingerprint"), CortexST::MakeFingerprint(Context.StateTree));
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexSTInspectOps::GetState(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FCortexCommandResult Error;
	if (!CortexST::GetRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error;
	}

	FCortexSTAssetContext Context;
	if (!CortexST::LoadAssetContext(AssetPath, Context, Error))
	{
		return Error;
	}

	FCortexSTStateRef StateRef;
	if (!CortexST::ResolveState(Context, Params, StateRef, Error))
	{
		return Error;
	}

	TSharedPtr<FJsonObject> Data = CortexST::SerializeState(StateRef, true, false);
	Data->SetStringField(TEXT("asset_path"), Context.AssetPath);
	Data->SetObjectField(TEXT("validation"), CortexST::BuildValidationPayload(Context.StateTree));
	Data->SetObjectField(TEXT("fingerprint"), CortexST::MakeFingerprint(Context.StateTree));
	return FCortexCommandRouter::Success(Data);
}
