#include "Operations/CortexGraphNodeSettingsOps.h"
#include "Operations/CortexGraphNodeOps.h"
#include "CortexAssetFingerprint.h"
#include "CortexBatchMutation.h"
#include "CortexEngineCompat.h"
#include "CortexGraphBindingStorage.h"
#include "CortexGraphModule.h"
#include "CortexSerializer.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimNodeBase.h"
#include "Animation/Skeleton.h"
#include "BoneContainer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Engine/Blueprint.h"
#include "K2Node.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/SecureHash.h"
#include "ScopedTransaction.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace
{
	/** One property path resolved against a node struct instance. */
	struct FResolvedSettingsPath
	{
		/** The top-level member of the node struct the path starts with. */
		const FProperty* Member = nullptr;
		/** The addressed property (an array's Inner for an indexed final segment). */
		const FProperty* Leaf = nullptr;
		void* LeafValue = nullptr;
		/** The outermost FBoneReference on the path (an ancestor or the leaf itself), when there is one. */
		FBoneReference* BoneReference = nullptr;
	};

	/** A refusal: the error code and message of the command result. */
	struct FSettingsRefusal
	{
		FString Code;
		FString Message;
	};

	/** The node's runtime struct property, found as UAnimGraphNode_Base::GetFNodeProperty does: the first FAnimNode_Base member. */
	const FStructProperty* FindNodeStructProperty(const UEdGraphNode* Node)
	{
		for (TFieldIterator<FStructProperty> It(Node->GetClass()); It; ++It)
		{
			if (It->Struct != nullptr && It->Struct->IsChildOf(FAnimNode_Base::StaticStruct()))
			{
				return *It;
			}
		}
		return nullptr;
	}

	/** The asset fingerprint graph.set_pin_value uses, plus a digest of the node settings: unsaved edits of a dirty asset leave the former unchanged. */
	TSharedPtr<FJsonObject> CurrentFingerprint(UBlueprint* Blueprint, const UEdGraphNode* Node, const FStructProperty* NodeProperty)
	{
		TSharedPtr<FJsonObject> Fingerprint = MakeObjectAssetFingerprint(Blueprint, GetTypeHash(static_cast<uint32>(Blueprint->Status))).ToJson();
		FString SettingsText;
		NodeProperty->Struct->ExportText(SettingsText, NodeProperty->ContainerPtrToValuePtr<void>(Node), nullptr, nullptr, PPF_None, nullptr);
		Fingerprint->SetStringField(TEXT("node_settings_hash"), FMD5::HashAnsiString(*SettingsText));
		return Fingerprint;
	}

	bool IsEditableSetting(const FProperty* Property)
	{
		return Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_EditConst);
	}

	bool ParseSegment(const FString& Segment, FString& OutName, int32& OutIndex)
	{
		OutIndex = INDEX_NONE;
		int32 Bracket = INDEX_NONE;
		if (!Segment.FindChar(TEXT('['), Bracket))
		{
			OutName = Segment;
			return !OutName.IsEmpty();
		}
		const FString IndexText = Segment.Mid(Bracket + 1, Segment.Len() - Bracket - 2);
		if (!Segment.EndsWith(TEXT("]")) || IndexText.IsEmpty())
		{
			return false;
		}
		for (const TCHAR Char : IndexText)
		{
			if (!FChar::IsDigit(Char))
			{
				return false;
			}
		}
		OutName = Segment.Left(Bracket);
		OutIndex = FCString::Atoi(*IndexText);
		return !OutName.IsEmpty();
	}

	/**
	 * Resolve Path (dot-separated members, each optionally indexed as Name[N]) inside a node struct value. Every named
	 * member must be editable; traversal enters structs and existing array elements only, never object references.
	 */
	bool ResolveSettingsPath(const UScriptStruct* NodeStruct, void* NodeValue, const FString& Path, FResolvedSettingsPath& Out, FString& OutError)
	{
		TArray<FString> Segments;
		Path.ParseIntoArray(Segments, TEXT("."), false);
		if (Segments.Num() == 0)
		{
			OutError = TEXT("Property path is empty");
			return false;
		}

		const UStruct* Struct = NodeStruct;
		void* Container = NodeValue;
		for (int32 SegmentIndex = 0; SegmentIndex < Segments.Num(); ++SegmentIndex)
		{
			FString Name;
			int32 ElementIndex = INDEX_NONE;
			if (!ParseSegment(Segments[SegmentIndex], Name, ElementIndex))
			{
				OutError = FString::Printf(TEXT("Malformed segment '%s' in property path '%s'"), *Segments[SegmentIndex], *Path);
				return false;
			}

			const FProperty* Property = Struct->FindPropertyByName(FName(*Name));
			if (Property == nullptr)
			{
				OutError = FString::Printf(TEXT("'%s' has no member '%s' (path '%s')"), *Struct->GetName(), *Name, *Path);
				return false;
			}
			if (!IsEditableSetting(Property))
			{
				OutError = FString::Printf(TEXT("'%s.%s' is not an editable setting (path '%s')"), *Struct->GetName(), *Name, *Path);
				return false;
			}
			if (SegmentIndex == 0)
			{
				Out.Member = Property;
			}

			void* Value = Property->ContainerPtrToValuePtr<void>(Container);
			if (ElementIndex != INDEX_NONE)
			{
				const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property);
				if (ArrayProperty == nullptr)
				{
					OutError = FString::Printf(TEXT("'%s' is not an array and cannot be indexed (path '%s')"), *Name, *Path);
					return false;
				}
				FScriptArrayHelper Helper(ArrayProperty, Value);
				if (!Helper.IsValidIndex(ElementIndex))
				{
					OutError = FString::Printf(TEXT("Index %d is out of range for '%s' (%d elements, path '%s')"), ElementIndex, *Name, Helper.Num(), *Path);
					return false;
				}
				Property = ArrayProperty->Inner;
				Value = Helper.GetRawPtr(ElementIndex);
			}

			const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			if (Out.BoneReference == nullptr && StructProperty != nullptr && StructProperty->Struct == FBoneReference::StaticStruct())
			{
				Out.BoneReference = static_cast<FBoneReference*>(Value);
			}

			if (SegmentIndex == Segments.Num() - 1)
			{
				Out.Leaf = Property;
				Out.LeafValue = Value;
				return true;
			}
			if (StructProperty == nullptr)
			{
				OutError = FString::Printf(TEXT("'%s' is not a struct and has no members (path '%s')"), *Name, *Path);
				return false;
			}
			Struct = StructProperty->Struct;
			Container = Value;
		}
		return false;
	}

	/**
	 * The write restrictions of a path, applied inside an aggregate JSON value too: every named struct field must exist
	 * and be editable, a fixed-size array keeps its length, and instanced sub-objects (which the scratch copy shares with
	 * the live node) are never replaced by value. Value is the current value when known (null for a new element).
	 */
	bool PreflightJsonValue(const FProperty* Property, const void* Value, const TSharedPtr<FJsonValue>& Json, const FString& Path, FSettingsRefusal& OutRefusal)
	{
		if (Property->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference))
		{
			OutRefusal = { CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("'%s' holds instanced sub-objects and cannot be written by value"), *Path) };
			return false;
		}

		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (Json.IsValid() && Json->TryGetObject(Object) && Object != nullptr)
			{
				for (const auto& Pair : (*Object)->Values)
				{
					const FString FieldName = CortexEngineCompat::JsonKeyToString(Pair.Key);
					const FString FieldPath = Path + TEXT(".") + FieldName;
					const FProperty* Field = StructProperty->Struct->FindPropertyByName(FName(*FieldName));
					if (Field == nullptr || !IsEditableSetting(Field))
					{
						OutRefusal = { CortexErrorCodes::InvalidPropertyPath, Field == nullptr
							? FString::Printf(TEXT("'%s' has no member '%s' (path '%s')"), *StructProperty->Struct->GetName(), *FieldName, *FieldPath)
							: FString::Printf(TEXT("'%s.%s' is not an editable setting (path '%s')"), *StructProperty->Struct->GetName(), *FieldName, *FieldPath) };
						return false;
					}
					const void* FieldValue = Value != nullptr ? Field->ContainerPtrToValuePtr<void>(Value) : nullptr;
					if (!PreflightJsonValue(Field, FieldValue, Pair.Value, FieldPath, OutRefusal))
					{
						return false;
					}
				}
			}
			return true;
		}

		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			const TArray<TSharedPtr<FJsonValue>>* Elements = nullptr;
			if (!Json.IsValid() || !Json->TryGetArray(Elements) || Elements == nullptr)
			{
				return true;
			}
			const int32 CurrentNum = Value != nullptr ? FScriptArrayHelper(ArrayProperty, Value).Num() : INDEX_NONE;
			if (ArrayProperty->HasAnyPropertyFlags(CPF_EditFixedSize) && CurrentNum != INDEX_NONE && Elements->Num() != CurrentNum)
			{
				OutRefusal = { CortexErrorCodes::InvalidValue,
					FString::Printf(TEXT("'%s' is a fixed-size array of %d elements; got %d"), *Path, CurrentNum, Elements->Num()) };
				return false;
			}
			for (int32 Index = 0; Index < Elements->Num(); ++Index)
			{
				const void* ElementValue = Value != nullptr && Index < CurrentNum
					? FScriptArrayHelper(ArrayProperty, Value).GetRawPtr(Index)
					: nullptr;
				if (!PreflightJsonValue(ArrayProperty->Inner, ElementValue, (*Elements)[Index], FString::Printf(TEXT("%s[%d]"), *Path, Index), OutRefusal))
				{
					return false;
				}
			}
		}
		return true;
	}

	/** Every FBoneReference inside a value (structs, arrays and static arrays; object references are not followed). */
	void CollectBoneReferences(const FProperty* Property, void* Value, TArray<FBoneReference*>& OutReferences)
	{
		for (int32 Dim = 0; Dim < Property->ArrayDim; ++Dim)
		{
			void* Element = static_cast<uint8*>(Value) + Dim * Property->GetElementSize();
			if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				if (StructProperty->Struct == FBoneReference::StaticStruct())
				{
					OutReferences.Add(static_cast<FBoneReference*>(Element));
					continue;
				}
				for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
				{
					CollectBoneReferences(*It, It->ContainerPtrToValuePtr<void>(Element), OutReferences);
				}
			}
			else if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
			{
				FScriptArrayHelper Helper(ArrayProperty, Element);
				for (int32 Index = 0; Index < Helper.Num(); ++Index)
				{
					CollectBoneReferences(ArrayProperty->Inner, Helper.GetRawPtr(Index), OutReferences);
				}
			}
		}
	}

	/**
	 * The properties an edit changed, at the granularity the details panel notifies: the innermost changed member of a
	 * struct, an array whose length changed, otherwise the changed element's members.
	 */
	void CollectChangedLeaves(const FProperty* Property, const void* Old, const void* New, TArray<const FProperty*>& OutLeaves)
	{
		for (int32 Dim = 0; Dim < Property->ArrayDim; ++Dim)
		{
			const void* OldElement = static_cast<const uint8*>(Old) + Dim * Property->GetElementSize();
			const void* NewElement = static_cast<const uint8*>(New) + Dim * Property->GetElementSize();
			if (Property->Identical(OldElement, NewElement, PPF_None))
			{
				continue;
			}
			if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
				{
					CollectChangedLeaves(*It, It->ContainerPtrToValuePtr<void>(OldElement), It->ContainerPtrToValuePtr<void>(NewElement), OutLeaves);
				}
			}
			else if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
			{
				FScriptArrayHelper OldHelper(ArrayProperty, OldElement);
				FScriptArrayHelper NewHelper(ArrayProperty, NewElement);
				if (OldHelper.Num() != NewHelper.Num())
				{
					OutLeaves.AddUnique(Property);
					continue;
				}
				for (int32 Index = 0; Index < NewHelper.Num(); ++Index)
				{
					CollectChangedLeaves(ArrayProperty->Inner, OldHelper.GetRawPtr(Index), NewHelper.GetRawPtr(Index), OutLeaves);
				}
			}
			else
			{
				OutLeaves.AddUnique(Property);
			}
		}
	}

	/**
	 * Whether the compiler takes the member's value from somewhere other than the node struct: an allocated pin (Name, or
	 * Name_N per array element; hidden pins included), a shown optional-pin record (an exposed array creates its element
	 * pins on reconstruction) or an active property binding.
	 */
	bool IsMemberValueFromPinOrBinding(const UEdGraphNode* Node, const FProperty* Member)
	{
		const FString Name = Member->GetName();
		const FString ElementPrefix = Name + TEXT("_");
		const bool bArray = Member->IsA<FArrayProperty>();
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin == nullptr || Pin->Direction != EGPD_Input)
			{
				continue;
			}
			const FString PinName = Pin->PinName.ToString();
			if (PinName == Name
				|| (bArray && PinName.StartsWith(ElementPrefix) && PinName.Len() > ElementPrefix.Len() && PinName.Mid(ElementPrefix.Len()).IsNumeric()))
			{
				return true;
			}
		}

		const FArrayProperty* RecordsProperty = FindFProperty<FArrayProperty>(Node->GetClass(), TEXT("ShowPinForProperties"));
		const FStructProperty* RecordProperty = RecordsProperty != nullptr ? CastField<FStructProperty>(RecordsProperty->Inner) : nullptr;
		if (RecordProperty != nullptr && RecordProperty->Struct == FOptionalPinFromProperty::StaticStruct())
		{
			const TArray<FOptionalPinFromProperty>& Records = *RecordsProperty->ContainerPtrToValuePtr<TArray<FOptionalPinFromProperty>>(Node);
			for (const FOptionalPinFromProperty& Record : Records)
			{
				if (Record.bShowPin && Record.PropertyName == Member->GetFName())
				{
					return true;
				}
			}
		}

		TArray<FCortexBindingEntry> Bindings;
		if (const UK2Node* K2Node = Cast<UK2Node>(Node))
		{
			FCortexGraphBindingStorage::ReadBindings(K2Node, Bindings);
		}
		for (const FCortexBindingEntry& Binding : Bindings)
		{
			if (Binding.bIsBound && Binding.PropertyName.GetPlainNameString() == Name)
			{
				return true;
			}
		}
		return false;
	}

	TSet<FString> VisiblePinNames(const UEdGraphNode* Node)
	{
		TSet<FString> Names;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin != nullptr && !Pin->bHidden)
			{
				Names.Add(Pin->PinName.ToString());
			}
		}
		return Names;
	}

	TArray<TSharedPtr<FJsonValue>> SortedNames(const TSet<FString>& Names)
	{
		TArray<FString> Sorted = Names.Array();
		Sorted.Sort();
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Name : Sorted)
		{
			Values.Add(MakeShared<FJsonValueString>(Name));
		}
		return Values;
	}

	TSharedPtr<FJsonValue> PathValueJson(const UScriptStruct* NodeStruct, void* NodeValue, const FString& Path)
	{
		FResolvedSettingsPath Resolved;
		FString Error;
		if (!ResolveSettingsPath(NodeStruct, NodeValue, Path, Resolved, Error))
		{
			return MakeShared<FJsonValueNull>();
		}
		const TSharedPtr<FJsonValue> Value = FCortexSerializer::PropertyToJson(Resolved.Leaf, Resolved.LeafValue);
		return Value.IsValid() ? Value : MakeShared<FJsonValueNull>();
	}

	/** The anim node a request addresses, resolved through the shared graph helpers. */
	struct FNodeTarget
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* Graph = nullptr;
		UEdGraphNode* Node = nullptr;
		const FStructProperty* NodeProperty = nullptr;
	};

	bool ResolveNodeTarget(const TSharedPtr<FJsonObject>& Params, bool bForWrite, FNodeTarget& Out, FCortexCommandResult& OutError)
	{
		FString AssetPath;
		FString NodeId;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || !Params->TryGetStringField(TEXT("node_id"), NodeId))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required params: asset_path and node_id"));
			return false;
		}
		if (bForWrite && !FCortexGraphNodeOps::ValidateWritableAssetPath(AssetPath, OutError))
		{
			return false;
		}

		Out.Blueprint = FCortexGraphNodeOps::LoadBlueprint(AssetPath, OutError);
		if (Out.Blueprint == nullptr)
		{
			return false;
		}

		FString GraphName;
		Params->TryGetStringField(TEXT("graph_name"), GraphName);
		if (bForWrite)
		{
			if (!FCortexGraphNodeOps::ResolveMutableGraph(Out.Blueprint, GraphName, Out.Graph, OutError))
			{
				return false;
			}
		}
		else
		{
			Out.Graph = FCortexGraphNodeOps::FindGraph(Out.Blueprint, GraphName, OutError);
			if (Out.Graph == nullptr)
			{
				return false;
			}
		}

		FString SubgraphPath;
		Params->TryGetStringField(TEXT("subgraph_path"), SubgraphPath);
		if (!SubgraphPath.IsEmpty())
		{
			Out.Graph = FCortexGraphNodeOps::ResolveSubgraph(Out.Graph, SubgraphPath, OutError);
			if (Out.Graph == nullptr)
			{
				return false;
			}
		}

		Out.Node = FCortexGraphNodeOps::FindNode(Out.Graph, NodeId, OutError);
		if (Out.Node == nullptr)
		{
			return false;
		}

		Out.NodeProperty = FCortexGraphBindingStorage::IsAnimNode(Out.Node) ? FindNodeStructProperty(Out.Node) : nullptr;
		if (Out.NodeProperty == nullptr)
		{
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("Node '%s' (%s) is not an animation graph node with a settings struct"), *NodeId, *Out.Node->GetClass()->GetName()));
			return false;
		}
		return true;
	}

	USkeleton* TargetSkeletonOf(const UBlueprint* Blueprint)
	{
		const UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(Blueprint);
		return AnimBlueprint != nullptr ? AnimBlueprint->TargetSkeleton.Get() : nullptr;
	}

	bool IsTemplateAnimBlueprint(const UBlueprint* Blueprint)
	{
		const UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(Blueprint);
		return AnimBlueprint != nullptr && AnimBlueprint->bIsTemplate;
	}

	/** Records an object in the active transaction even when it was created without RF_Transactional (Cortex-created nodes are). */
	void RecordForUndo(UObject* Object)
	{
		Object->SetFlags(RF_Transactional);
		Object->Modify();
	}
}

FCortexCommandResult FCortexGraphNodeSettingsOps::GetNodeSettings(const TSharedPtr<FJsonObject>& Params)
{
	FNodeTarget Target;
	FCortexCommandResult Error;
	if (!ResolveNodeTarget(Params, false, Target, Error))
	{
		return Error;
	}

	const UScriptStruct* NodeStruct = Target.NodeProperty->Struct;
	void* NodeValue = Target.NodeProperty->ContainerPtrToValuePtr<void>(Target.Node);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_id"), Target.Node->GetName());
	Data->SetStringField(TEXT("node_class"), Target.Node->GetClass()->GetName());
	Data->SetStringField(TEXT("node_struct"), NodeStruct->GetName());

	FString PropertyPath;
	if (Params->TryGetStringField(TEXT("property_path"), PropertyPath) && !PropertyPath.IsEmpty())
	{
		FResolvedSettingsPath Resolved;
		FString PathError;
		if (!ResolveSettingsPath(NodeStruct, NodeValue, PropertyPath, Resolved, PathError))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidPropertyPath, PathError);
		}
		Data->SetStringField(TEXT("property_path"), PropertyPath);
		Data->SetField(TEXT("settings"), FCortexSerializer::PropertyToJson(Resolved.Leaf, Resolved.LeafValue));
	}
	else
	{
		TSharedPtr<FJsonObject> Settings = MakeShared<FJsonObject>();
		for (TFieldIterator<FProperty> It(NodeStruct); It; ++It)
		{
			if (IsEditableSetting(*It))
			{
				Settings->SetField(It->GetName(), FCortexSerializer::PropertyToJson(*It, It->ContainerPtrToValuePtr<void>(NodeValue)));
			}
		}
		Data->SetObjectField(TEXT("settings"), Settings);
	}

	TArray<TSharedPtr<FJsonValue>> PinProperties;
	for (TFieldIterator<FProperty> It(NodeStruct); It; ++It)
	{
		if (IsEditableSetting(*It) && IsMemberValueFromPinOrBinding(Target.Node, *It))
		{
			PinProperties.Add(MakeShared<FJsonValueString>(It->GetName()));
		}
	}
	Data->SetArrayField(TEXT("pin_properties"), PinProperties);

	const USkeleton* Skeleton = TargetSkeletonOf(Target.Blueprint);
	Data->SetStringField(TEXT("target_skeleton"), Skeleton != nullptr ? Skeleton->GetPathName() : FString());
	Data->SetObjectField(TEXT("fingerprint"), CurrentFingerprint(Target.Blueprint, Target.Node, Target.NodeProperty));
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexGraphNodeSettingsOps::SetNodeSettings(const TSharedPtr<FJsonObject>& Params)
{
	const TSharedPtr<FJsonObject>* Values = nullptr;
	if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("values"), Values) || Values == nullptr || (*Values)->Values.Num() == 0)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("values must be a non-empty object of property path -> JSON value"));
	}
	bool bDryRun = false;
	if (Params->HasField(TEXT("dry_run")) && !Params->TryGetBoolField(TEXT("dry_run"), bDryRun))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("dry_run must be a boolean"));
	}
	if (Params->HasField(TEXT("expected_fingerprint")) && !Params->HasTypedField<EJson::Object>(TEXT("expected_fingerprint")))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("expected_fingerprint must be an object when provided"));
	}

	FNodeTarget Target;
	FCortexCommandResult Error;
	if (!ResolveNodeTarget(Params, true, Target, Error))
	{
		return Error;
	}
	UEdGraphNode* Node = Target.Node;
	const FStructProperty* NodeProperty = Target.NodeProperty;
	const UScriptStruct* NodeStruct = NodeProperty->Struct;
	void* LiveValue = NodeProperty->ContainerPtrToValuePtr<void>(Node);

	if (Params->HasTypedField<EJson::Object>(TEXT("expected_fingerprint")))
	{
		const TSharedPtr<FJsonObject> Expected = Params->GetObjectField(TEXT("expected_fingerprint"));
		const TSharedPtr<FJsonObject> Current = CurrentFingerprint(Target.Blueprint, Node, NodeProperty);
		FString ExpectedSettingsHash;
		if (!FCortexBatchMutation::FingerprintsMatch(Current, Expected)
			|| !Expected->TryGetStringField(TEXT("node_settings_hash"), ExpectedSettingsHash)
			|| ExpectedSettingsHash != Current->GetStringField(TEXT("node_settings_hash")))
		{
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetObjectField(TEXT("current_fingerprint"), Current);
			return FCortexCommandRouter::Error(
				CortexErrorCodes::StalePrecondition,
				FString::Printf(TEXT("Expected fingerprint does not match the current fingerprint of node '%s' in %s; read it again with graph.get_node_settings"),
					*Node->GetName(), *Target.Blueprint->GetPathName()),
				Details);
		}
	}

	// Convert and validate every path on a scratch copy: a refusal changes nothing.
	FStructOnScope Scratch(NodeStruct);
	NodeStruct->CopyScriptStruct(Scratch.GetStructMemory(), LiveValue);

	const USkeleton* Skeleton = TargetSkeletonOf(Target.Blueprint);
	const bool bTemplate = IsTemplateAnimBlueprint(Target.Blueprint);
	bool bWroteUnvalidatedBones = false;
	TArray<FString> Paths;
	TArray<TSharedPtr<FJsonValue>> BeforeValues;
	for (const auto& Pair : (*Values)->Values)
	{
		const FString Path = CortexEngineCompat::JsonKeyToString(Pair.Key);
		BeforeValues.Add(PathValueJson(NodeStruct, LiveValue, Path));

		FResolvedSettingsPath Resolved;
		FString PathError;
		if (!ResolveSettingsPath(NodeStruct, Scratch.GetStructMemory(), Path, Resolved, PathError))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidPropertyPath, PathError);
		}
		if (IsMemberValueFromPinOrBinding(Node, Resolved.Member))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("'%s' takes its value from a pin or binding on node '%s' at compile; use graph.set_pin_value"),
					*Resolved.Member->GetName(), *Node->GetName()));
		}
		FSettingsRefusal Refusal;
		if (!PreflightJsonValue(Resolved.Leaf, Resolved.LeafValue, Pair.Value, Path, Refusal))
		{
			return FCortexCommandRouter::Error(Refusal.Code, Refusal.Message);
		}

		TArray<FString> Warnings;
		if (!FCortexSerializer::JsonToProperty(Pair.Value, Resolved.Leaf, Resolved.LeafValue, Node, Warnings) || Warnings.Num() > 0)
		{
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> WarningValues;
			for (const FString& Warning : Warnings)
			{
				WarningValues.Add(MakeShared<FJsonValueString>(Warning));
			}
			Details->SetStringField(TEXT("path"), Path);
			Details->SetArrayField(TEXT("warnings"), WarningValues);
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidValue,
				FString::Printf(TEXT("Value for '%s' does not convert to %s: %s"), *Path, *Resolved.Leaf->GetCPPType(),
					Warnings.Num() > 0 ? *Warnings[0] : TEXT("conversion failed")),
				Details);
		}

		// Validate now: a later path may reallocate the memory these references point into.
		TArray<FBoneReference*> WrittenBones;
		if (Resolved.BoneReference != nullptr)
		{
			WrittenBones.Add(Resolved.BoneReference);
		}
		else
		{
			CollectBoneReferences(Resolved.Leaf, Resolved.LeafValue, WrittenBones);
		}
		for (const FBoneReference* Bone : WrittenBones)
		{
			if (Bone->BoneName.IsNone())
			{
				continue;
			}
			if (Skeleton == nullptr)
			{
				if (!bTemplate)
				{
					return FCortexCommandRouter::Error(
						CortexErrorCodes::InvalidOperation,
						FString::Printf(TEXT("%s has no target skeleton, so bone '%s' (path '%s') cannot be validated"),
							*Target.Blueprint->GetPathName(), *Bone->BoneName.ToString(), *Path));
				}
				bWroteUnvalidatedBones = true;
			}
			else if (Skeleton->GetReferenceSkeleton().FindBoneIndex(Bone->BoneName) == INDEX_NONE)
			{
				TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
				Details->SetStringField(TEXT("path"), Path);
				Details->SetStringField(TEXT("bone"), Bone->BoneName.ToString());
				Details->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidValue,
					FString::Printf(TEXT("Bone '%s' (path '%s') does not exist in skeleton %s"), *Bone->BoneName.ToString(), *Path, *Skeleton->GetPathName()),
					Details);
			}
		}
		Paths.Add(Path);
	}

	TArray<TSharedPtr<FJsonValue>> ResultWarnings;
	if (bWroteUnvalidatedBones)
	{
		ResultWarnings.Add(MakeShared<FJsonValueString>(
			TEXT("The template Anim Blueprint has no target skeleton; written bone names were not validated")));
	}

	TArray<const FProperty*> ChangedLeaves;
	for (TFieldIterator<FProperty> It(NodeStruct); It; ++It)
	{
		CollectChangedLeaves(*It, It->ContainerPtrToValuePtr<void>(LiveValue), It->ContainerPtrToValuePtr<void>(Scratch.GetStructMemory()), ChangedLeaves);
	}
	const bool bChanged = ChangedLeaves.Num() > 0;

	TSet<FString> PinsAdded;
	TSet<FString> PinsRemoved;
	if (!bDryRun && bChanged)
	{
		const TSet<FString> PinsBefore = VisiblePinNames(Node);
		{
			FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Cortex: Set node settings %s"), *Node->GetName())));
			// Node-class reactions break links and edit the binding map; record everything they can touch.
			RecordForUndo(Node);
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin == nullptr)
				{
					continue;
				}
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (Linked != nullptr && Linked->GetOwningNodeUnchecked() != nullptr)
					{
						RecordForUndo(Linked->GetOwningNodeUnchecked());
					}
				}
			}
			if (UK2Node* K2Node = Cast<UK2Node>(Node))
			{
				FCortexGraphBindingStorage::MarkBindingModified(K2Node);
			}

			FProperty* EditedMember = const_cast<FStructProperty*>(NodeProperty);
			const TArray<UEdGraphPin*> PinsBeforeEdit = Node->Pins;
			Node->PreEditChange(EditedMember);
			NodeStruct->CopyScriptStruct(LiveValue, Scratch.GetStructMemory());
			// The details panel's notification per changed property, with the node struct as member: node classes react here.
			for (const FProperty* Leaf : ChangedLeaves)
			{
				FPropertyChangedEvent Event(const_cast<FProperty*>(Leaf), EPropertyChangeType::ValueSet);
				Event.SetActiveMemberProperty(EditedMember);
				Node->PostEditChangeProperty(Event);
			}
			// Reconstruct unless a reaction already did (it replaces the pin objects).
			if (Node->Pins == PinsBeforeEdit)
			{
				if (const UEdGraphSchema* Schema = Target.Graph->GetSchema())
				{
					Schema->ReconstructNode(*Node);
				}
			}
			Target.Graph->NotifyGraphChanged();
			FBlueprintEditorUtils::MarkBlueprintAsModified(Target.Blueprint);
		}
		const TSet<FString> PinsAfter = VisiblePinNames(Node);
		PinsAdded = PinsAfter.Difference(PinsBefore);
		PinsRemoved = PinsBefore.Difference(PinsAfter);
		UE_LOG(LogCortexGraph, Log, TEXT("Set %d node setting path(s) on %s"), Paths.Num(), *Node->GetName());
	}

	// Read back from what the request leaves: the live node after an apply, the scratch copy for a preview.
	TArray<TSharedPtr<FJsonValue>> Changes;
	for (int32 Index = 0; Index < Paths.Num(); ++Index)
	{
		const TSharedPtr<FJsonValue> Requested = PathValueJson(NodeStruct, Scratch.GetStructMemory(), Paths[Index]);
		const TSharedPtr<FJsonValue> After = bDryRun ? Requested : PathValueJson(NodeStruct, LiveValue, Paths[Index]);
		TSharedPtr<FJsonObject> Change = MakeShared<FJsonObject>();
		Change->SetStringField(TEXT("path"), Paths[Index]);
		Change->SetField(TEXT("before"), BeforeValues[Index]);
		Change->SetField(TEXT("after"), After);
		if (!FJsonValue::CompareEqual(*Requested, *After))
		{
			// The node's own edit reaction changed the value, as it would for an edit in the details panel.
			Change->SetBoolField(TEXT("adjusted"), true);
			Change->SetField(TEXT("requested"), Requested);
			ResultWarnings.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("The node adjusted '%s' after the edit; 'after' is the node's value"), *Paths[Index])));
		}
		Changes.Add(MakeShared<FJsonValueObject>(Change));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_id"), Node->GetName());
	Data->SetBoolField(TEXT("dry_run"), bDryRun);
	Data->SetBoolField(TEXT("changed"), bChanged);
	Data->SetArrayField(TEXT("changes"), Changes);
	if (!bDryRun)
	{
		Data->SetArrayField(TEXT("pins_added"), SortedNames(PinsAdded));
		Data->SetArrayField(TEXT("pins_removed"), SortedNames(PinsRemoved));
	}
	Data->SetArrayField(TEXT("warnings"), ResultWarnings);
	Data->SetObjectField(TEXT("fingerprint"), CurrentFingerprint(Target.Blueprint, Node, NodeProperty));
	return FCortexCommandRouter::Success(Data);
}
