#include "Operations/CortexSTBindingOps.h"

#include "CortexCommandRouter.h"
#include "CortexSTCompat.h"
#include "CortexSTNodeLocator.h"
#include "CortexSTTypes.h"
#include "CortexStateTreeModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeState.h"
#include "UObject/UnrealType.h"

namespace
{
constexpr int32 BindingReportDefaultLimit = 50;
constexpr int32 BindingReportMaxLimit = 200;

enum class EBindingReportDirection : uint8
{
	Both,
	Target,
	Source,
};

FString BindingReportGuidText(const FGuid& Guid)
{
	return Guid.ToString(EGuidFormats::DigitsWithHyphens);
}

TSharedPtr<FJsonValue> BindingReportGuidValue(const FGuid& Guid)
{
	if (!Guid.IsValid())
	{
		return MakeShared<FJsonValueNull>();
	}
	return MakeShared<FJsonValueString>(BindingReportGuidText(Guid));
}

/** Everything one report needs, computed once: all nodes, all stored bindings with resolved paths, and the ID -> node lookup (editor ID and node-struct ID). */
struct FBindingReportState
{
	const FCortexSTAssetContext& Context;
	TArray<FCortexSTNodeLocation> Nodes;
	TArray<CortexSTCompat::FBindingInfo> Bindings;
	TMap<FGuid, int32> NodeIndexByStructId;

	explicit FBindingReportState(const FCortexSTAssetContext& InContext)
		: Context(InContext)
	{
		CortexST::CollectEditorNodes(Context, Nodes);
		const int32 BindingCount = CortexSTCompat::GetEditorBindingCount(*Context.EditorData);
		Bindings.Reserve(BindingCount);
		for (int32 Index = 0; Index < BindingCount; ++Index)
		{
			CortexSTCompat::FBindingInfo& Info = Bindings.AddDefaulted_GetRef();
			CortexSTCompat::ReadEditorBinding(*Context.EditorData, Index, Info);
		}
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			NodeIndexByStructId.Add(Nodes[Index].Node->ID, Index);
			const FGuid DefinitionId = CortexSTCompat::GetNodeDefinitionId(*Nodes[Index].Node);
			if (DefinitionId.IsValid())
			{
				NodeIndexByStructId.Add(DefinitionId, Index);
			}
		}
	}

	bool IsNodeSide(const CortexSTCompat::FBindingSideInfo& Side, const FStateTreeEditorNode& Node) const
	{
		return Side.StructID == Node.ID || (Side.StructID.IsValid() && Side.StructID == CortexSTCompat::GetNodeDefinitionId(Node));
	}
};

TSharedPtr<FJsonObject> BindingReportSegmentJson(const CortexSTCompat::FBindingSegmentInfo& Segment)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Segment.Name.ToString());
	Json->SetStringField(TEXT("display_name"), Segment.DisplayName.ToString());
	if (Segment.ArrayIndex == INDEX_NONE)
	{
		Json->SetField(TEXT("array_index"), MakeShared<FJsonValueNull>());
	}
	else
	{
		Json->SetNumberField(TEXT("array_index"), Segment.ArrayIndex);
	}
	Json->SetField(TEXT("property_guid"), BindingReportGuidValue(Segment.PropertyGuid));
	if (!Segment.InstanceStructPath.IsEmpty())
	{
		Json->SetStringField(TEXT("instance_struct"), Segment.InstanceStructPath);
	}
	return Json;
}

TSharedPtr<FJsonObject> BindingReportOwnerJson(const FBindingReportState& State, const FGuid& StructId)
{
	TSharedPtr<FJsonObject> Owner = MakeShared<FJsonObject>();
	if (const int32* NodeIndex = State.NodeIndexByStructId.Find(StructId))
	{
		const FCortexSTNodeLocation& Location = State.Nodes[*NodeIndex];
		Owner->SetStringField(TEXT("kind"), TEXT("node"));
		Owner->SetStringField(TEXT("node_id"), BindingReportGuidText(Location.Node->ID));
		Owner->SetStringField(TEXT("name"), Location.Node->GetName().ToString());
		Owner->SetStringField(TEXT("state_path"), Location.OwnerState != nullptr ? CortexSTCompat::GetStatePath(Location.OwnerState) : FString());
		Owner->SetStringField(TEXT("slot"), CortexST::LexNodeSlot(Location.Slot));
		Owner->SetStringField(TEXT("struct_part"), StructId == Location.Node->ID ? TEXT("instance") : TEXT("node_struct"));
		return Owner;
	}

	const CortexSTCompat::FBindableStructInfo Info = CortexSTCompat::DescribeBindableStruct(*State.Context.EditorData, StructId);
	FString Kind = TEXT("unknown");
	if (Info.bFound)
	{
		if (Info.DataSource == TEXT("Parameter"))
		{
			Kind = TEXT("root_parameters");
		}
		else if (Info.DataSource == TEXT("StateParameter"))
		{
			Kind = TEXT("state_parameters");
		}
		else if (Info.DataSource == TEXT("Context"))
		{
			Kind = TEXT("context");
		}
		else if (Info.DataSource == TEXT("PropertyFunction"))
		{
			Kind = TEXT("property_function");
		}
	}
	Owner->SetStringField(TEXT("kind"), Kind);
	Owner->SetStringField(TEXT("name"), Info.Name.ToString());
	Owner->SetStringField(TEXT("state_path"), Info.StatePath);
	Owner->SetStringField(TEXT("data_source"), Info.bFound ? Info.DataSource : FString(TEXT("not_found")));
	return Owner;
}

TSharedPtr<FJsonObject> BindingReportSideJson(const FBindingReportState& State, const CortexSTCompat::FBindingSideInfo& Side)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("struct_id"), BindingReportGuidText(Side.StructID));
	Json->SetObjectField(TEXT("owner"), BindingReportOwnerJson(State, Side.StructID));
	Json->SetStringField(TEXT("path"), Side.Path);
	Json->SetBoolField(TEXT("resolved"), Side.bResolved);
	if (!Side.bResolved)
	{
		Json->SetStringField(TEXT("resolve_error"), Side.ResolveError);
	}
	TArray<TSharedPtr<FJsonValue>> Segments;
	for (const CortexSTCompat::FBindingSegmentInfo& Segment : Side.Segments)
	{
		Segments.Add(MakeShared<FJsonValueObject>(BindingReportSegmentJson(Segment)));
	}
	Json->SetArrayField(TEXT("segments"), Segments);
	return Json;
}

TSharedPtr<FJsonObject> BindingReportBindingJson(const FBindingReportState& State, const CortexSTCompat::FBindingInfo& Binding)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("binding_index"), Binding.Index);
	Json->SetBoolField(TEXT("implicit_context"), false);
	Json->SetStringField(TEXT("direction"), Binding.bOutput ? TEXT("output") : TEXT("input"));
	Json->SetBoolField(TEXT("is_function_binding"), Binding.bFunction);
	if (Binding.bCompletion)
	{
		Json->SetStringField(TEXT("completion_condition"), Binding.CompletionCondition);
	}
	Json->SetObjectField(TEXT("source"), BindingReportSideJson(State, Binding.Source));
	Json->SetObjectField(TEXT("target"), BindingReportSideJson(State, Binding.Target));
	return Json;
}

const UClass* BindingReportNodeClass(const FStateTreeEditorNode& Node)
{
	if (const UClass* Class = CortexST::GetNodeInstanceClass(Node))
	{
		return Class;
	}
	return Node.InstanceObject != nullptr ? Node.InstanceObject->GetClass() : nullptr;
}

TSharedPtr<FJsonObject> BindingReportNodeTypeJson(const FStateTreeEditorNode& Node)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	const UClass* Class = BindingReportNodeClass(Node);
	Json->SetStringField(TEXT("kind"), Class != nullptr ? TEXT("blueprint_class") : TEXT("struct"));
	Json->SetStringField(TEXT("struct_path"), Node.Node.GetScriptStruct() != nullptr ? Node.Node.GetScriptStruct()->GetPathName() : FString());
	if (Class != nullptr)
	{
		Json->SetStringField(TEXT("class_path"), Class->GetPathName());
	}
	Json->SetBoolField(TEXT("is_blueprint"), Class != nullptr && Class->HasAnyClassFlags(CLASS_CompiledFromBlueprint));
	return Json;
}

/**
 * The compiler binds an unbound Context property to the schema's matching context data and stores no editor binding
 * (StateTreeCompiler.cpp:3140-3176). They are reported as implicit so a swap that keeps the property keeps its source visible.
 */
void BindingReportAppendImplicitContext(
	const FBindingReportState& State,
	const FGuid& StructId,
	const UStruct* Struct,
	TArray<TSharedPtr<FJsonValue>>& OutAsTarget)
{
	if (Struct == nullptr || !StructId.IsValid())
	{
		return;
	}

	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		const FProperty* Property = *It;
		if (CortexSTCompat::GetPropertyUsage(Property) != CortexSTCompat::EPropertyUsage::Context)
		{
			continue;
		}

		const bool bExplicit = State.Bindings.ContainsByPredicate([&](const CortexSTCompat::FBindingInfo& Binding)
		{
			return Binding.Target.StructID == StructId
				&& Binding.Target.Segments.Num() > 0
				&& Binding.Target.Segments[0].Name == Property->GetFName();
		});
		if (bExplicit)
		{
			continue;
		}

		FGuid ContextStructId;
		const CortexSTCompat::FBindableStructInfo ContextInfo = CortexSTCompat::FindContextStructFor(*State.Context.EditorData, Property, ContextStructId);

		TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("struct_id"), ContextStructId.IsValid() ? BindingReportGuidText(ContextStructId) : FString());
		TSharedPtr<FJsonObject> SourceOwner = MakeShared<FJsonObject>();
		SourceOwner->SetStringField(TEXT("kind"), TEXT("context"));
		SourceOwner->SetStringField(TEXT("name"), ContextInfo.Name.ToString());
		SourceOwner->SetStringField(TEXT("state_path"), ContextInfo.StatePath);
		Source->SetObjectField(TEXT("owner"), SourceOwner);
		Source->SetStringField(TEXT("path"), FString());
		Source->SetBoolField(TEXT("resolved"), ContextInfo.bFound);
		Source->SetArrayField(TEXT("segments"), TArray<TSharedPtr<FJsonValue>>());

		CortexSTCompat::FBindingSideInfo TargetSide;
		TargetSide.StructID = StructId;
		TargetSide.Path = Property->GetName();
		TargetSide.bResolved = true;
		CortexSTCompat::FBindingSegmentInfo& Segment = TargetSide.Segments.AddDefaulted_GetRef();
		Segment.Name = Property->GetFName();
		Segment.DisplayName = Property->GetFName();

		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetField(TEXT("binding_index"), MakeShared<FJsonValueNull>());
		Json->SetBoolField(TEXT("implicit_context"), true);
		Json->SetStringField(TEXT("direction"), TEXT("input"));
		Json->SetBoolField(TEXT("is_function_binding"), false);
		Json->SetObjectField(TEXT("source"), Source);
		Json->SetObjectField(TEXT("target"), BindingReportSideJson(State, TargetSide));
		OutAsTarget.Add(MakeShared<FJsonValueObject>(Json));
	}
}

/** Builds one node's report; bOutHasBindings tells whether any stored binding touched it in the requested direction. */
TSharedPtr<FJsonObject> BindingReportNodeJson(
	const FBindingReportState& State,
	const int32 NodeIndex,
	const EBindingReportDirection Direction,
	bool& bOutHasBindings)
{
	const FCortexSTNodeLocation& Location = State.Nodes[NodeIndex];
	const FStateTreeEditorNode& Node = *Location.Node;

	TArray<TSharedPtr<FJsonValue>> AsTarget;
	TArray<TSharedPtr<FJsonValue>> AsSource;
	bOutHasBindings = false;
	for (const CortexSTCompat::FBindingInfo& Binding : State.Bindings)
	{
		if (Direction != EBindingReportDirection::Source && State.IsNodeSide(Binding.Target, Node))
		{
			AsTarget.Add(MakeShared<FJsonValueObject>(BindingReportBindingJson(State, Binding)));
			bOutHasBindings = true;
		}
		if (Direction != EBindingReportDirection::Target && State.IsNodeSide(Binding.Source, Node))
		{
			AsSource.Add(MakeShared<FJsonValueObject>(BindingReportBindingJson(State, Binding)));
			bOutHasBindings = true;
		}
	}

	if (Direction != EBindingReportDirection::Source)
	{
		const UStruct* InstanceStruct = BindingReportNodeClass(Node) != nullptr
			? static_cast<const UStruct*>(BindingReportNodeClass(Node))
			: static_cast<const UStruct*>(Node.Instance.GetScriptStruct());
		BindingReportAppendImplicitContext(State, Node.ID, InstanceStruct, AsTarget);
		BindingReportAppendImplicitContext(State, CortexSTCompat::GetNodeDefinitionId(Node), Node.Node.GetScriptStruct(), AsTarget);
		// An implicit context entry is reported content of the node even though no stored binding backs it, so the default
		// all-nodes page must include a node that only has one (the compiler binds it at build time).
		bOutHasBindings = bOutHasBindings || !AsTarget.IsEmpty();
	}

	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("node_id"), BindingReportGuidText(Node.ID));
	const FGuid DefinitionId = CortexSTCompat::GetNodeDefinitionId(Node);
	if (DefinitionId.IsValid())
	{
		Json->SetStringField(TEXT("definition_id"), BindingReportGuidText(DefinitionId));
	}
	Json->SetStringField(TEXT("slot"), CortexST::LexNodeSlot(Location.Slot));
	Json->SetStringField(TEXT("owner_state_id"), Location.OwnerState != nullptr ? BindingReportGuidText(Location.OwnerState->ID) : FString());
	Json->SetStringField(TEXT("owner_state_path"), Location.OwnerState != nullptr ? CortexSTCompat::GetStatePath(Location.OwnerState) : FString());
	if (Location.TransitionIndex != INDEX_NONE)
	{
		Json->SetNumberField(TEXT("owner_transition_index"), Location.TransitionIndex);
	}
	else
	{
		Json->SetField(TEXT("owner_transition_index"), MakeShared<FJsonValueNull>());
	}
	Json->SetStringField(TEXT("name"), Node.GetName().ToString());
	Json->SetObjectField(TEXT("node_type"), BindingReportNodeTypeJson(Node));
	const UClass* InstanceClass = BindingReportNodeClass(Node);
	Json->SetStringField(
		TEXT("instance_type_path"),
		InstanceClass != nullptr ? InstanceClass->GetPathName()
			: (Node.Instance.GetScriptStruct() != nullptr ? Node.Instance.GetScriptStruct()->GetPathName() : FString()));
	Json->SetArrayField(TEXT("as_target"), AsTarget);
	Json->SetArrayField(TEXT("as_source"), AsSource);
	return Json;
}

bool ReadBindingReportInteger(
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* FieldName,
	const int32 DefaultValue,
	int32& OutValue,
	FCortexCommandResult& OutError)
{
	OutValue = DefaultValue;
	if (!Params.IsValid() || !Params->HasField(FieldName))
	{
		return true;
	}

	double Number = 0.0;
	if (!Params->TryGetNumberField(FieldName, Number) || Number != FMath::FloorToDouble(Number) || FMath::Abs(Number) > 2147483647.0)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("%s must be an integral JSON number"), FieldName));
		return false;
	}
	OutValue = static_cast<int32>(Number);
	return true;
}
}

int32 FCortexSTBindingOps::CountTreeBindings(const FCortexSTAssetContext& Context)
{
	return Context.EditorData != nullptr ? CortexSTCompat::GetEditorBindingCount(*Context.EditorData) : 0;
}

TSharedPtr<FJsonObject> FCortexSTBindingOps::BuildTreeReport(const FCortexSTAssetContext& Context)
{
	const FBindingReportState State(Context);
	TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> NodeValues;
	for (int32 Index = 0; Index < State.Nodes.Num(); ++Index)
	{
		bool bHasBindings = false;
		NodeValues.Add(MakeShared<FJsonValueObject>(BindingReportNodeJson(State, Index, EBindingReportDirection::Both, bHasBindings)));
	}
	Report->SetArrayField(TEXT("nodes"), NodeValues);

	TArray<TSharedPtr<FJsonValue>> BindingValues;
	for (const CortexSTCompat::FBindingInfo& Binding : State.Bindings)
	{
		BindingValues.Add(MakeShared<FJsonValueObject>(BindingReportBindingJson(State, Binding)));
	}
	Report->SetArrayField(TEXT("bindings"), BindingValues);
	Report->SetNumberField(TEXT("whole_tree_binding_count"), State.Bindings.Num());
	return Report;
}

TSharedPtr<FJsonObject> FCortexSTBindingOps::BuildNodesReport(const FCortexSTAssetContext& Context, const TArray<FGuid>& NodeIds)
{
	const FBindingReportState State(Context);
	TArray<TSharedPtr<FJsonValue>> NodeValues;
	for (const FGuid& NodeId : NodeIds)
	{
		const int32* NodeIndex = State.NodeIndexByStructId.Find(NodeId);
		if (NodeIndex == nullptr || State.Nodes[*NodeIndex].Node->ID != NodeId)
		{
			TSharedPtr<FJsonObject> Missing = MakeShared<FJsonObject>();
			Missing->SetStringField(TEXT("node_id"), BindingReportGuidText(NodeId));
			Missing->SetBoolField(TEXT("missing"), true);
			NodeValues.Add(MakeShared<FJsonValueObject>(Missing));
			continue;
		}
		bool bHasBindings = false;
		NodeValues.Add(MakeShared<FJsonValueObject>(BindingReportNodeJson(State, *NodeIndex, EBindingReportDirection::Both, bHasBindings)));
	}

	TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetArrayField(TEXT("nodes"), NodeValues);
	Report->SetNumberField(TEXT("whole_tree_binding_count"), State.Bindings.Num());
	return Report;
}

FCortexCommandResult FCortexSTBindingOps::GetNodeBindings(const TSharedPtr<FJsonObject>& Params)
{
	if (!CortexSTCompat::bNodeBindingsAvailable)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::NotAvailable,
			TEXT("Per-node StateTree binding inspection needs Unreal Engine 5.8 or newer"));
	}

	FString AssetPath;
	FCortexCommandResult Error;
	if (!CortexST::GetRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error;
	}

	FString NodeIdText;
	const bool bHasNodeId = Params->TryGetStringField(TEXT("node_id"), NodeIdText) && !NodeIdText.IsEmpty();
	FString StateIdText;
	FString StatePathText;
	const bool bHasStateSelector =
		(Params->TryGetStringField(TEXT("state_id"), StateIdText) && !StateIdText.IsEmpty())
		|| (Params->TryGetStringField(TEXT("state_path"), StatePathText) && !StatePathText.IsEmpty());
	if (bHasNodeId && bHasStateSelector)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			TEXT("Specify either node_id or a state selector (state_id/state_path), not both"));
	}

	EBindingReportDirection Direction = EBindingReportDirection::Both;
	FString DirectionText;
	if (Params->TryGetStringField(TEXT("direction"), DirectionText) && !DirectionText.IsEmpty())
	{
		if (DirectionText == TEXT("target"))
		{
			Direction = EBindingReportDirection::Target;
		}
		else if (DirectionText == TEXT("source"))
		{
			Direction = EBindingReportDirection::Source;
		}
		else if (DirectionText != TEXT("both"))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("direction must be target, source or both, got '%s'"), *DirectionText));
		}
	}

	int32 Limit = BindingReportDefaultLimit;
	int32 Offset = 0;
	if (!ReadBindingReportInteger(Params, TEXT("limit"), BindingReportDefaultLimit, Limit, Error)
		|| !ReadBindingReportInteger(Params, TEXT("offset"), 0, Offset, Error))
	{
		return Error;
	}
	if (Limit > BindingReportMaxLimit)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetNumberField(TEXT("max_limit"), BindingReportMaxLimit);
		return FCortexCommandRouter::Error(
			CortexErrorCodes::LimitExceeded,
			FString::Printf(TEXT("limit %d exceeds the maximum of %d"), Limit, BindingReportMaxLimit),
			Details);
	}
	if (Limit < 1 || Offset < 0)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("limit must be 1..200 and offset must not be negative"));
	}

	FCortexSTAssetContext Context;
	if (!CortexST::LoadAssetContext(AssetPath, Context, Error))
	{
		return Error;
	}

	FGuid NodeId;
	if (bHasNodeId && !FGuid::Parse(NodeIdText, NodeId))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("node_id is not a GUID: %s"), *NodeIdText));
	}

	FCortexSTStateRef SelectedState;
	if (bHasStateSelector && !CortexST::ResolveState(Context, Params, SelectedState, Error))
	{
		return Error;
	}

	const FBindingReportState State(Context);

	TArray<int32> Selected;
	FString Selection = TEXT("all");
	if (bHasNodeId)
	{
		Selection = TEXT("node");
		const int32* NodeIndex = State.NodeIndexByStructId.Find(NodeId);
		if (NodeIndex == nullptr || State.Nodes[*NodeIndex].Node->ID != NodeId)
		{
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetStringField(TEXT("node_id"), NodeIdText);
			return FCortexCommandRouter::Error(
				CortexErrorCodes::NodeNotFound,
				FString::Printf(TEXT("No StateTree node has the editor ID %s"), *NodeIdText),
				Details);
		}
		Selected.Add(*NodeIndex);
	}
	else if (bHasStateSelector)
	{
		Selection = TEXT("state");
		for (int32 Index = 0; Index < State.Nodes.Num(); ++Index)
		{
			if (State.Nodes[Index].OwnerState == SelectedState.State)
			{
				Selected.Add(Index);
			}
		}
	}
	else
	{
		for (int32 Index = 0; Index < State.Nodes.Num(); ++Index)
		{
			bool bHasBindings = false;
			BindingReportNodeJson(State, Index, Direction, bHasBindings);
			if (bHasBindings)
			{
				Selected.Add(Index);
			}
		}
	}

	const int32 Total = Selected.Num();
	TArray<TSharedPtr<FJsonValue>> NodeValues;
	for (int32 Position = Offset; Position < Total && NodeValues.Num() < Limit; ++Position)
	{
		bool bHasBindings = false;
		NodeValues.Add(MakeShared<FJsonValueObject>(BindingReportNodeJson(State, Selected[Position], Direction, bHasBindings)));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Context.AssetPath);
	Data->SetStringField(TEXT("selection"), Selection);
	Data->SetStringField(TEXT("direction"), DirectionText.IsEmpty() ? FString(TEXT("both")) : DirectionText);
	Data->SetArrayField(TEXT("nodes"), NodeValues);
	Data->SetNumberField(TEXT("total"), Total);
	Data->SetNumberField(TEXT("offset"), Offset);
	Data->SetNumberField(TEXT("limit"), Limit);
	Data->SetNumberField(TEXT("returned_count"), NodeValues.Num());
	Data->SetBoolField(TEXT("has_more"), Offset + NodeValues.Num() < Total);
	Data->SetNumberField(TEXT("whole_tree_binding_count"), State.Bindings.Num());
	Data->SetObjectField(TEXT("fingerprint"), CortexST::MakeFingerprint(Context.StateTree));
	return FCortexCommandRouter::Success(Data);
}
