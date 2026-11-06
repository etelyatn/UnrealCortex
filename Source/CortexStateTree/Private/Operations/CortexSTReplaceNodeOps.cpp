#include "Operations/CortexSTReplaceNodeOps.h"

#include "Blueprint/StateTreeConditionBlueprintBase.h"
#include "Blueprint/StateTreeConsiderationBlueprintBase.h"
#include "Blueprint/StateTreeEvaluatorBlueprintBase.h"
#include "Blueprint/StateTreeNodeBlueprintBase.h"
#include "Blueprint/StateTreeTaskBlueprintBase.h"
#include "CortexAssetMutationGuard.h"
#include "CortexCommandRouter.h"
#include "CortexSTCompat.h"
#include "CortexSTNodeLocator.h"
#include "CortexSTTypes.h"
#include "CortexStateTreeModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/IConsoleManager.h"
#include "Misc/PackageName.h"
#include "Operations/CortexSTAssetOps.h"
#include "Operations/CortexSTBindingOps.h"
#include "Operations/CortexSTValidationOps.h"
#include "ScopedTransaction.h"
#include "StateTree.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeConditionBase.h"
#include "StateTreeConsiderationBase.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreeNodeBase.h"
#include "StateTreeState.h"
#include "StateTreeTaskBase.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace
{
constexpr int32 RepNodeSchemaVersion = 1;

FString RepNodeGuidText(const FGuid& Guid)
{
	return Guid.ToString(EGuidFormats::DigitsWithHyphens);
}

TSharedPtr<FJsonValue> RepNodeGuidValue(const FGuid& Guid)
{
	if (!Guid.IsValid())
	{
		return MakeShared<FJsonValueNull>();
	}
	return MakeShared<FJsonValueString>(RepNodeGuidText(Guid));
}

TSharedPtr<FJsonValue> RepNodeNameValue(const FName Name)
{
	if (Name.IsNone())
	{
		return MakeShared<FJsonValueNull>();
	}
	return MakeShared<FJsonValueString>(Name.ToString());
}

TArray<TSharedPtr<FJsonValue>> RepNodeStringValues(const TArray<FString>& Strings)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FString& String : Strings)
	{
		Values.Add(MakeShared<FJsonValueString>(String));
	}
	return Values;
}

const UScriptStruct* RepNodeSlotBaseStruct(const ECortexSTNodeSlot Slot)
{
	switch (Slot)
	{
	case ECortexSTNodeSlot::GlobalTask:
	case ECortexSTNodeSlot::Task:
	case ECortexSTNodeSlot::SingleTask:
		return FStateTreeTaskBase::StaticStruct();
	case ECortexSTNodeSlot::Evaluator:
		return FStateTreeEvaluatorBase::StaticStruct();
	case ECortexSTNodeSlot::EnterCondition:
	case ECortexSTNodeSlot::TransitionCondition:
		return FStateTreeConditionBase::StaticStruct();
	case ECortexSTNodeSlot::Consideration:
		return FStateTreeConsiderationBase::StaticStruct();
	default:
		return nullptr;
	}
}

const UClass* RepNodeSlotBaseClass(const ECortexSTNodeSlot Slot)
{
	switch (Slot)
	{
	case ECortexSTNodeSlot::GlobalTask:
	case ECortexSTNodeSlot::Task:
	case ECortexSTNodeSlot::SingleTask:
		return UStateTreeTaskBlueprintBase::StaticClass();
	case ECortexSTNodeSlot::Evaluator:
		return UStateTreeEvaluatorBlueprintBase::StaticClass();
	case ECortexSTNodeSlot::EnterCondition:
	case ECortexSTNodeSlot::TransitionCondition:
		return UStateTreeConditionBlueprintBase::StaticClass();
	case ECortexSTNodeSlot::Consideration:
		return UStateTreeConsiderationBlueprintBase::StaticClass();
	default:
		return nullptr;
	}
}

/** The classes that declare the node-level flags a Blueprint node exposes (bConsideredForCompletion, bShouldStateChangeOnReselect, ...). */
bool RepNodeIsBlueprintBaseClass(const UClass* Class)
{
	return Class == UStateTreeNodeBlueprintBase::StaticClass()
		|| Class == UStateTreeTaskBlueprintBase::StaticClass()
		|| Class == UStateTreeEvaluatorBlueprintBase::StaticClass()
		|| Class == UStateTreeConditionBlueprintBase::StaticClass()
		|| Class == UStateTreeConsiderationBlueprintBase::StaticClass();
}

/** Copies one property value between two different containers, reading booleans with the old descriptor and writing them with the new one. */
void RepNodeCopyValue(const FProperty* OldProperty, const void* OldContainer, const FProperty* NewProperty, void* NewContainer)
{
	if (OldProperty == nullptr || NewProperty == nullptr || OldContainer == nullptr || NewContainer == nullptr)
	{
		return;
	}
	if (const FBoolProperty* OldBool = CastField<FBoolProperty>(OldProperty))
	{
		if (const FBoolProperty* NewBool = CastField<FBoolProperty>(NewProperty))
		{
			// Packed bitfields have different offsets and masks on the two types: CopyCompleteValue would write the old byte/mask.
			const int32 Count = FMath::Min(OldBool->ArrayDim, NewBool->ArrayDim);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				NewBool->SetPropertyValue_InContainer(NewContainer, OldBool->GetPropertyValue_InContainer(OldContainer, Index), Index);
			}
			return;
		}
	}
	OldProperty->CopyCompleteValue(
		NewProperty->ContainerPtrToValuePtr<void>(NewContainer),
		OldProperty->ContainerPtrToValuePtr<void>(const_cast<void*>(OldContainer)));
}

/** Build-time and runtime bookkeeping on the node struct: never carried over from one node type to another. */
bool RepNodeIsNodeBookkeeping(const FName Name)
{
	static const TSet<FName> Names = {
		FName(TEXT("BindingsBatch")),
		FName(TEXT("OutputBindingsBatch")),
		FName(TEXT("InstanceTemplateIndex")),
		FName(TEXT("ExecutionRuntimeTemplateIndex")),
		FName(TEXT("InstanceDataHandle")),
		FName(TEXT("bHasTaskCompletionDelegateDispatcher")),
	};
	return Names.Contains(Name);
}

/** Value assignability: SameType, or an object property whose old class is the new property's class or a subclass of it. */
bool RepNodeArePropertiesCompatible(const FProperty* Old, const FProperty* New, FString& OutReason)
{
	if (Old->SameType(New))
	{
		return true;
	}
	// FBoolProperty::SameType also compares the bit position inside the container, which legitimately differs between a Blueprint
	// class variable and a native struct flag; a bool copies completely in both directions regardless.
	if (CastField<FBoolProperty>(Old) != nullptr && CastField<FBoolProperty>(New) != nullptr)
	{
		return true;
	}
	const FObjectPropertyBase* OldObject = CastField<FObjectPropertyBase>(Old);
	const FObjectPropertyBase* NewObject = CastField<FObjectPropertyBase>(New);
	if (OldObject != nullptr && NewObject != nullptr && Old->GetClass() == New->GetClass()
		&& OldObject->PropertyClass != nullptr && NewObject->PropertyClass != nullptr
		&& OldObject->PropertyClass->IsChildOf(NewObject->PropertyClass))
	{
		return true;
	}
	OutReason = FString::Printf(TEXT("'%s' is %s but '%s' is %s"),
		*Old->GetName(), *Old->GetCPPType(), *New->GetName(), *New->GetCPPType());
	return false;
}

FString RepNodeUsageName(const CortexSTCompat::EPropertyUsage Usage)
{
	switch (Usage)
	{
	case CortexSTCompat::EPropertyUsage::Input: return TEXT("Input");
	case CortexSTCompat::EPropertyUsage::Output: return TEXT("Output");
	case CortexSTCompat::EPropertyUsage::Context: return TEXT("Context");
	case CortexSTCompat::EPropertyUsage::Parameter: return TEXT("Parameter");
	default: return TEXT("Invalid");
	}
}

FString RepNodeStatusName(const ECortexSTReplacePropertyStatus Status)
{
	switch (Status)
	{
	case ECortexSTReplacePropertyStatus::Copied: return TEXT("copied");
	case ECortexSTReplacePropertyStatus::DefaultEqual: return TEXT("default_equal");
	case ECortexSTReplacePropertyStatus::DroppedDefault: return TEXT("dropped_default");
	case ECortexSTReplacePropertyStatus::UnmappedNonDefault: return TEXT("unmapped_non_default");
	case ECortexSTReplacePropertyStatus::DroppedAcknowledged: return TEXT("dropped_acknowledged");
	default: return TEXT("unknown");
	}
}

/** Types the nodes become, resolved once per call. */
struct FRepNodeTargetTypes
{
	const UScriptStruct* NodeStruct = nullptr;
	const UClass* NodeClass = nullptr;
	/** Type of the new instance data (struct node: GetInstanceDataType(); class node: the class). */
	const UStruct* InstanceType = nullptr;
	/** Where node-level flags are looked up by name. */
	const UStruct* FlagType = nullptr;
	/** Default values of the destination's node-level flags (a default node struct, or the class CDO). */
	const void* FlagDefaults = nullptr;
	/** Owns the default node struct memory for a struct destination. */
	FInstancedStruct NodeStructDefaults;
};

FRepNodeTargetTypes RepNodeResolveTargetTypes(const FCortexSTReplaceTarget& Target)
{
	FRepNodeTargetTypes Types;
	Types.NodeStruct = Target.NodeStruct;
	Types.NodeClass = Target.NodeClass;
	if (Target.NodeStruct != nullptr)
	{
		Types.NodeStructDefaults.InitializeAs(Target.NodeStruct);
		if (const FStateTreeNodeBase* NodeBase = Types.NodeStructDefaults.GetPtr<FStateTreeNodeBase>())
		{
			Types.InstanceType = NodeBase->GetInstanceDataType();
		}
		Types.FlagType = Target.NodeStruct;
		Types.FlagDefaults = Types.NodeStructDefaults.GetMemory();
	}
	else
	{
		Types.InstanceType = Target.NodeClass;
		Types.FlagType = Target.NodeClass;
		Types.FlagDefaults = Target.NodeClass != nullptr ? Target.NodeClass->GetDefaultObject() : nullptr;
	}
	return Types;
}

TSharedPtr<FJsonObject> RepNodeDetails(const TCHAR* Field, const FString& Value)
{
	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(Field, Value);
	return Details;
}

bool RepNodeValueDiffersFromDefault(const FProperty* Property, const UObject* Object, const UObject* Defaults)
{
	for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
	{
		if (!Property->Identical_InContainer(Object, Defaults, Index, PPF_None))
		{
			return true;
		}
	}
	return false;
}

FString RepNodeExportValue(const FProperty* Property, const UObject* Object)
{
	FString Text;
	Property->ExportText_Direct(Text, Property->ContainerPtrToValuePtr<void>(Object), nullptr, nullptr, PPF_None);
	return Text;
}

bool RepNodeIsDuplicateTarget(const TArray<FCortexSTReplaceProperty>& Properties, const ECortexSTReplacePropertyKind Kind, const FName NewName, FName& OutOtherOld)
{
	for (const FCortexSTReplaceProperty& Existing : Properties)
	{
		if (Existing.Kind == Kind && !Existing.NewName.IsNone() && Existing.NewName == NewName)
		{
			OutOtherOld = Existing.OldName;
			return true;
		}
	}
	return false;
}

bool RepNodeBuildNodePlan(
	const FCortexSTAssetContext& Context,
	const FCortexSTNodeLocation& Location,
	const FCortexSTReplaceRequest& Request,
	const FRepNodeTargetTypes& Target,
	const TArray<CortexSTCompat::FBindingInfo>& Bindings,
	FCortexSTNodeReplacePlan& Plan,
	FCortexCommandResult& OutError)
{
	const FStateTreeEditorNode& Node = *Location.Node;
	Plan.NodeId = Node.ID;
	Plan.Slot = Location.Slot;
	Plan.NodeName = Node.GetName().ToString();
	Plan.OwnerStatePath = Location.OwnerState != nullptr ? CortexSTCompat::GetStatePath(Location.OwnerState) : FString();

	const UObject* OldObject = Node.InstanceObject;
	const UClass* OldClass = OldObject != nullptr ? OldObject->GetClass() : nullptr;
	if (OldClass == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("Node %s (%s) is not a Blueprint class node; replace_node replaces nodes that wrap a Blueprint-base class"), *Plan.NodeName, *RepNodeGuidText(Node.ID)),
			RepNodeDetails(TEXT("node_id"), RepNodeGuidText(Node.ID)));
		return false;
	}
	Plan.OldTypePath = OldClass->GetPathName();

	// Slot kind: the new type must derive the slot's base.
	if (Target.NodeStruct != nullptr)
	{
		const UScriptStruct* Base = RepNodeSlotBaseStruct(Location.Slot);
		if (Base == nullptr || !Target.NodeStruct->IsChildOf(Base))
		{
			TSharedPtr<FJsonObject> Details = RepNodeDetails(TEXT("node_id"), RepNodeGuidText(Node.ID));
			Details->SetStringField(TEXT("slot"), CortexST::LexNodeSlot(Location.Slot));
			Details->SetStringField(TEXT("required_base_struct"), Base != nullptr ? Base->GetPathName() : FString());
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("%s does not derive %s, which the %s slot of node %s requires"),
					*Target.NodeStruct->GetPathName(), Base != nullptr ? *Base->GetName() : TEXT("?"), CortexST::LexNodeSlot(Location.Slot), *Plan.NodeName),
				Details);
			return false;
		}
		Plan.bWouldChange = true;
	}
	else
	{
		const UClass* Base = RepNodeSlotBaseClass(Location.Slot);
		if (Base == nullptr || !Target.NodeClass->IsChildOf(Base))
		{
			TSharedPtr<FJsonObject> Details = RepNodeDetails(TEXT("node_id"), RepNodeGuidText(Node.ID));
			Details->SetStringField(TEXT("slot"), CortexST::LexNodeSlot(Location.Slot));
			Details->SetStringField(TEXT("required_base_class"), Base != nullptr ? Base->GetPathName() : FString());
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("%s does not derive %s, which the %s slot of node %s requires"),
					*Target.NodeClass->GetPathName(), Base != nullptr ? *Base->GetName() : TEXT("?"), CortexST::LexNodeSlot(Location.Slot), *Plan.NodeName),
				Details);
			return false;
		}
		Plan.bWouldChange = OldClass != Target.NodeClass;
	}

	const UObject* Defaults = OldClass->GetDefaultObject();

	// ── Properties ──
	TMap<FName, const FProperty*> OldProperties;
	for (TFieldIterator<FProperty> It(OldClass); It; ++It)
	{
		OldProperties.Add((*It)->GetFName(), *It);
	}
	for (const TPair<FName, FName>& Entry : Request.PropertyMap)
	{
		if (!OldProperties.Contains(Entry.Key))
		{
			TSharedPtr<FJsonObject> Details = RepNodeDetails(TEXT("old_property"), Entry.Key.ToString());
			Details->SetStringField(TEXT("node_id"), RepNodeGuidText(Node.ID));
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::PropertyNotFound,
				FString::Printf(TEXT("property_map key '%s' is not a property of %s"), *Entry.Key.ToString(), *OldClass->GetName()),
				Details);
			return false;
		}
	}

	for (TFieldIterator<FProperty> It(OldClass); It; ++It)
	{
		const FProperty* OldProperty = *It;
		if (OldProperty->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
		{
			continue;
		}

		const FName OldName = OldProperty->GetFName();
		const bool bMapped = Request.PropertyMap.Contains(OldName);
		const UClass* DeclaringClass = OldProperty->GetOwnerClass();
		const bool bBlueprintDeclared = DeclaringClass != nullptr && DeclaringClass->HasAnyClassFlags(CLASS_CompiledFromBlueprint);
		const bool bBaseFlag = DeclaringClass != nullptr && RepNodeIsBlueprintBaseClass(DeclaringClass);

		ECortexSTReplacePropertyKind Kind = ECortexSTReplacePropertyKind::Instance;
		if (bBaseFlag)
		{
			if (!OldProperty->HasAnyPropertyFlags(CPF_Edit) && !bMapped)
			{
				continue;
			}
			Kind = ECortexSTReplacePropertyKind::NodeFlag;
		}
		else
		{
			const bool bEditable = OldProperty->HasAnyPropertyFlags(CPF_Edit) && !OldProperty->HasAnyPropertyFlags(CPF_DisableEditOnInstance);
			if (!bEditable && !bMapped)
			{
				if (bBlueprintDeclared)
				{
					Plan.BlueprintOnlyMembers.Add(OldName.ToString());
				}
				continue;
			}
		}

		FCortexSTReplaceProperty Entry;
		Entry.OldName = OldName;
		Entry.Kind = Kind;
		Entry.OldProperty = OldProperty;
		Entry.bNonDefault = RepNodeValueDiffersFromDefault(OldProperty, OldObject, Defaults);
		Entry.OldValue = RepNodeExportValue(OldProperty, OldObject);

		const UStruct* TargetType = Kind == ECortexSTReplacePropertyKind::Instance ? Target.InstanceType : Target.FlagType;
		const FName WantedName = bMapped ? Request.PropertyMap.FindChecked(OldName) : OldName;
		const FProperty* NewProperty = TargetType != nullptr ? TargetType->FindPropertyByName(WantedName) : nullptr;
		if (bMapped && NewProperty == nullptr)
		{
			TSharedPtr<FJsonObject> Details = RepNodeDetails(TEXT("old_property"), OldName.ToString());
			Details->SetStringField(TEXT("new_property"), WantedName.ToString());
			Details->SetStringField(TEXT("node_id"), RepNodeGuidText(Node.ID));
			OutError = FCortexCommandRouter::Error(
				CortexErrorCodes::PropertyNotFound,
				FString::Printf(TEXT("property_map target '%s' (for '%s') is not a property of %s"),
					*WantedName.ToString(), *OldName.ToString(), TargetType != nullptr ? *TargetType->GetName() : TEXT("the new type")),
				Details);
			return false;
		}

		if (NewProperty != nullptr)
		{
			FString Reason;
			if (!RepNodeArePropertiesCompatible(OldProperty, NewProperty, Reason))
			{
				FCortexSTReplaceMismatch& Mismatch = Plan.Mismatches.AddDefaulted_GetRef();
				Mismatch.OldName = OldName;
				Mismatch.NewName = WantedName;
				Mismatch.Reason = Reason;
				NewProperty = nullptr;
			}
			else if (Kind == ECortexSTReplacePropertyKind::Instance
				&& CortexSTCompat::GetPropertyUsage(OldProperty) != CortexSTCompat::GetPropertyUsage(NewProperty))
			{
				// A Context property loses its implicit binding, an Input one its must-be-bound rule, when its usage changes (StateTreeCompiler.cpp:3126-3176).
				FCortexSTReplaceMismatch& Mismatch = Plan.Mismatches.AddDefaulted_GetRef();
				Mismatch.OldName = OldName;
				Mismatch.NewName = WantedName;
				Mismatch.Reason = FString::Printf(TEXT("usage differs: '%s' is %s but '%s' is %s"),
					*OldName.ToString(), *RepNodeUsageName(CortexSTCompat::GetPropertyUsage(OldProperty)),
					*WantedName.ToString(), *RepNodeUsageName(CortexSTCompat::GetPropertyUsage(NewProperty)));
				NewProperty = nullptr;
			}
		}

		if (NewProperty != nullptr && Kind == ECortexSTReplacePropertyKind::NodeFlag)
		{
			// F2: a flag may only be skipped when it already holds the destination default - the Blueprint CDO is not the destination.
			const FBoolProperty* OldBool = CastField<FBoolProperty>(OldProperty);
			const FBoolProperty* NewBool = CastField<FBoolProperty>(NewProperty);
			bool bEqualsDestinationDefault = false;
			if (Target.FlagDefaults != nullptr)
			{
				// Packed booleans must be read through their own descriptors: one descriptor's mask is meaningless in the other layout.
				bEqualsDestinationDefault = OldBool != nullptr && NewBool != nullptr
					? OldBool->GetPropertyValue_InContainer(OldObject) == NewBool->GetPropertyValue_InContainer(Target.FlagDefaults)
					: OldProperty->Identical(
						OldProperty->ContainerPtrToValuePtr<void>(OldObject),
						NewProperty->ContainerPtrToValuePtr<void>(Target.FlagDefaults),
						PPF_None);
			}
			Entry.Status = bEqualsDestinationDefault
				? ECortexSTReplacePropertyStatus::DefaultEqual
				: ECortexSTReplacePropertyStatus::Copied;
		}

		// An explicit stored binding on the old property keeps its own source through the planned rewrite; the compiler resolves
		// the schema context only for a property that has no binding (StateTreeCompiler.cpp:3162-3173).
		const bool bOldPropertyBound = Bindings.ContainsByPredicate([&](const CortexSTCompat::FBindingInfo& Binding)
		{
			return Binding.Target.StructID == Node.ID
				&& Binding.Target.Segments.Num() > 0
				&& Binding.Target.Segments[0].Name == OldName;
		});
		if (NewProperty != nullptr && Kind == ECortexSTReplacePropertyKind::Instance && !bOldPropertyBound
			&& CortexSTCompat::GetPropertyUsage(OldProperty) == CortexSTCompat::EPropertyUsage::Context
			&& CortexSTCompat::GetPropertyUsage(NewProperty) == CortexSTCompat::EPropertyUsage::Context)
		{
			// F9: an unbound Context property is bound by the compiler through the schema by type AND name; with two same-type
			// candidates (an AI schema supplies the pawn and the controller) a same-usage rename changes the runtime source silently.
			FGuid OldContextId;
			FGuid NewContextId;
			const CortexSTCompat::FBindableStructInfo OldContext = CortexSTCompat::FindContextStructFor(*Context.EditorData, OldProperty, OldContextId);
			const CortexSTCompat::FBindableStructInfo NewContext = CortexSTCompat::FindContextStructFor(*Context.EditorData, NewProperty, NewContextId);
			if (OldContextId.IsValid() && NewContextId.IsValid() && OldContextId != NewContextId)
			{
				FCortexSTReplaceMismatch& Mismatch = Plan.Mismatches.AddDefaulted_GetRef();
				Mismatch.OldName = OldName;
				Mismatch.NewName = WantedName;
				Mismatch.Reason = FString::Printf(
					TEXT("the implicit Context source changes from '%s' to '%s' (same usage, different schema context)"),
					*OldContext.Name.ToString(), *NewContext.Name.ToString());
				NewProperty = nullptr;
			}
		}

		if (NewProperty != nullptr)
		{
			FName OtherOld;
			if (RepNodeIsDuplicateTarget(Plan.Properties, Kind, WantedName, OtherOld))
			{
				TSharedPtr<FJsonObject> Details = RepNodeDetails(TEXT("new_property"), WantedName.ToString());
				Details->SetStringField(TEXT("node_id"), RepNodeGuidText(Node.ID));
				OutError = FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("'%s' and '%s' both map to '%s'; property_map must be injective"), *OtherOld.ToString(), *OldName.ToString(), *WantedName.ToString()),
					Details);
				return false;
			}
			Entry.NewName = WantedName;
			Entry.NewProperty = NewProperty;
			if (Kind != ECortexSTReplacePropertyKind::NodeFlag)
			{
				Entry.Status = ECortexSTReplacePropertyStatus::Copied;
			}
		}
		else if (!Entry.bNonDefault)
		{
			Entry.Status = ECortexSTReplacePropertyStatus::DroppedDefault;
		}
		else if (Request.AcknowledgedLosses.Contains(OldName.ToString()))
		{
			Entry.Status = ECortexSTReplacePropertyStatus::DroppedAcknowledged;
		}
		else
		{
			Entry.Status = ECortexSTReplacePropertyStatus::UnmappedNonDefault;
			Plan.RequiredAcknowledgment.Add(OldName.ToString());
		}

		// A property with a type mismatch has no counterpart for the loss rule, but the mismatch itself refuses the call first.
		Plan.Properties.Add(Entry);
	}

	// ── Bindings: every stored binding side keyed by the node ID (instance data) or GetNodeID() (node struct) ──
	const FGuid DefinitionId = CortexSTCompat::GetNodeDefinitionId(Node);
	const UStruct* NodeSideType = Target.NodeStruct != nullptr ? static_cast<const UStruct*>(Target.NodeStruct) : static_cast<const UStruct*>(Node.Node.GetScriptStruct());

	auto PlanSide = [&](const CortexSTCompat::FBindingInfo& Binding, const CortexSTCompat::EBindingSide SideKind)
	{
		const CortexSTCompat::FBindingSideInfo& Side = SideKind == CortexSTCompat::EBindingSide::Target ? Binding.Target : Binding.Source;
		const bool bInstanceSide = Side.StructID == Node.ID;
		const bool bNodeStructSide = !bInstanceSide && DefinitionId.IsValid() && Side.StructID == DefinitionId;
		if ((!bInstanceSide && !bNodeStructSide) || Side.Segments.IsEmpty())
		{
			// Not this node, or a task-completion source (empty path, keyed by GetNodeID()): nothing to rewrite, it passes through unchanged.
			return;
		}

		const FName OldFirst = Side.Segments[0].Name;
		FName NewFirst = OldFirst;
		const UStruct* NewBase = bInstanceSide ? Target.InstanceType : NodeSideType;
		if (bInstanceSide)
		{
			for (const FCortexSTReplaceProperty& Property : Plan.Properties)
			{
				if (Property.Kind == ECortexSTReplacePropertyKind::Instance && Property.OldName == OldFirst)
				{
					if (Property.NewName.IsNone())
					{
						FCortexSTReplaceUnresolved& Unresolved = Plan.Unresolved.AddDefaulted_GetRef();
						Unresolved.BindingIndex = Binding.Index;
						Unresolved.Side = SideKind;
						Unresolved.Path = Side.Path;
						Unresolved.Reason = FString::Printf(TEXT("'%s' has no counterpart on the new type; map it with property_map"), *OldFirst.ToString());
						return;
					}
					NewFirst = Property.NewName;
					break;
				}
			}
		}

		FString Error;
		if (NewBase == nullptr
			|| !CortexSTCompat::ResolveRenamedBindingPath(*Context.EditorData, Binding.Index, SideKind, NewFirst, NewBase, Error))
		{
			FCortexSTReplaceUnresolved& Unresolved = Plan.Unresolved.AddDefaulted_GetRef();
			Unresolved.BindingIndex = Binding.Index;
			Unresolved.Side = SideKind;
			Unresolved.Path = Side.Path;
			Unresolved.Reason = NewBase == nullptr ? FString(TEXT("the new type has no instance data to bind to")) : Error;
			return;
		}

		FCortexSTReplaceRewrite& Rewrite = Plan.Rewrites.AddDefaulted_GetRef();
		Rewrite.BindingIndex = Binding.Index;
		Rewrite.Side = SideKind;
		Rewrite.bNodeStructSide = bNodeStructSide;
		Rewrite.OldPath = Side.Path;
		Rewrite.NewPath = Side.Path.StartsWith(OldFirst.ToString()) ? NewFirst.ToString() + Side.Path.RightChop(OldFirst.ToString().Len()) : Side.Path;
		Rewrite.OldPropertyGuid = Side.Segments[0].PropertyGuid;
		Rewrite.NewPropertyGuid = CortexSTCompat::FindStructPropertyGuid(NewBase, NewFirst);
		Rewrite.NewFirstSegment = NewFirst;
		Rewrite.FirstSegmentArrayIndex = Side.Segments[0].ArrayIndex;
		Rewrite.bRenamed = NewFirst != OldFirst;
	};

	for (const CortexSTCompat::FBindingInfo& Binding : Bindings)
	{
		PlanSide(Binding, CortexSTCompat::EBindingSide::Source);
		PlanSide(Binding, CortexSTCompat::EBindingSide::Target);
	}
	return true;
}

FString RepNodeSideName(const CortexSTCompat::EBindingSide Side)
{
	return Side == CortexSTCompat::EBindingSide::Target ? TEXT("target") : TEXT("source");
}

TSharedPtr<FJsonObject> RepNodePlanJson(const FCortexSTReplacePlan& Plan)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> PropertyValues;
	TArray<TSharedPtr<FJsonValue>> NodeValues;
	TArray<TSharedPtr<FJsonValue>> RemappedValues;
	TArray<TSharedPtr<FJsonValue>> UnresolvedValues;
	TArray<TSharedPtr<FJsonValue>> MismatchValues;
	TArray<FString> BlueprintOnly;
	TArray<FString> Required;

	for (const FCortexSTNodeReplacePlan& NodePlan : Plan.Nodes)
	{
		const FString NodeId = RepNodeGuidText(NodePlan.NodeId);

		TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
		NodeJson->SetStringField(TEXT("node_id"), NodeId);
		NodeJson->SetStringField(TEXT("name"), NodePlan.NodeName);
		NodeJson->SetStringField(TEXT("slot"), CortexST::LexNodeSlot(NodePlan.Slot));
		NodeJson->SetStringField(TEXT("owner_state_path"), NodePlan.OwnerStatePath);
		NodeJson->SetStringField(TEXT("old_type_path"), NodePlan.OldTypePath);
		NodeJson->SetBoolField(TEXT("would_change"), NodePlan.bWouldChange);
		NodeValues.Add(MakeShared<FJsonValueObject>(NodeJson));

		for (const FCortexSTReplaceProperty& Property : NodePlan.Properties)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("node_id"), NodeId);
			Entry->SetStringField(TEXT("old_property"), Property.OldName.ToString());
			Entry->SetField(TEXT("new_property"), RepNodeNameValue(Property.NewName));
			Entry->SetStringField(TEXT("kind"), Property.Kind == ECortexSTReplacePropertyKind::NodeFlag ? TEXT("node_flag") : TEXT("instance"));
			Entry->SetStringField(TEXT("status"), RepNodeStatusName(Property.Status));
			Entry->SetBoolField(TEXT("non_default"), Property.bNonDefault);
			Entry->SetStringField(TEXT("old_value"), Property.OldValue);
			PropertyValues.Add(MakeShared<FJsonValueObject>(Entry));
		}
		for (const FString& Member : NodePlan.BlueprintOnlyMembers)
		{
			BlueprintOnly.AddUnique(Member);
		}
		for (const FString& Name : NodePlan.RequiredAcknowledgment)
		{
			Required.AddUnique(Name);
		}
		for (const FCortexSTReplaceRewrite& Rewrite : NodePlan.Rewrites)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("node_id"), NodeId);
			Entry->SetNumberField(TEXT("binding_index"), Rewrite.BindingIndex);
			Entry->SetStringField(TEXT("side"), RepNodeSideName(Rewrite.Side));
			Entry->SetBoolField(TEXT("node_struct_side"), Rewrite.bNodeStructSide);
			Entry->SetStringField(TEXT("old_path"), Rewrite.OldPath);
			Entry->SetStringField(TEXT("new_path"), Rewrite.NewPath);
			Entry->SetField(TEXT("old_property_guid"), RepNodeGuidValue(Rewrite.OldPropertyGuid));
			Entry->SetField(TEXT("new_property_guid"), RepNodeGuidValue(Rewrite.NewPropertyGuid));
			Entry->SetBoolField(TEXT("renamed"), Rewrite.bRenamed);
			Entry->SetNumberField(TEXT("first_segment_array_index"), Rewrite.FirstSegmentArrayIndex);
			RemappedValues.Add(MakeShared<FJsonValueObject>(Entry));
		}
		for (const FCortexSTReplaceUnresolved& Unresolved : NodePlan.Unresolved)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("node_id"), NodeId);
			Entry->SetNumberField(TEXT("binding_index"), Unresolved.BindingIndex);
			Entry->SetStringField(TEXT("side"), RepNodeSideName(Unresolved.Side));
			Entry->SetStringField(TEXT("path"), Unresolved.Path);
			Entry->SetStringField(TEXT("reason"), Unresolved.Reason);
			UnresolvedValues.Add(MakeShared<FJsonValueObject>(Entry));
		}
		for (const FCortexSTReplaceMismatch& Mismatch : NodePlan.Mismatches)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("node_id"), NodeId);
			Entry->SetStringField(TEXT("old_property"), Mismatch.OldName.ToString());
			Entry->SetStringField(TEXT("new_property"), Mismatch.NewName.ToString());
			Entry->SetStringField(TEXT("reason"), Mismatch.Reason);
			MismatchValues.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	TSharedPtr<FJsonObject> BindingsJson = MakeShared<FJsonObject>();
	BindingsJson->SetNumberField(TEXT("before_count"), Plan.BindingsBefore);
	BindingsJson->SetArrayField(TEXT("remapped"), RemappedValues);
	// Tree-wide drops have no owning node of the replacement.
	for (const FCortexSTReplaceUnresolved& Drop : Plan.ValidationDrops)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetField(TEXT("node_id"), MakeShared<FJsonValueNull>());
		Entry->SetNumberField(TEXT("binding_index"), Drop.BindingIndex);
		Entry->SetStringField(TEXT("side"), RepNodeSideName(Drop.Side));
		Entry->SetStringField(TEXT("path"), Drop.Path);
		Entry->SetStringField(TEXT("reason"), Drop.Reason);
		UnresolvedValues.Add(MakeShared<FJsonValueObject>(Entry));
	}
	BindingsJson->SetArrayField(TEXT("unresolved"), UnresolvedValues);

	Json->SetNumberField(TEXT("schema_version"), RepNodeSchemaVersion);
	Json->SetArrayField(TEXT("nodes"), NodeValues);
	Json->SetArrayField(TEXT("property_plan"), PropertyValues);
	Json->SetObjectField(TEXT("bindings"), BindingsJson);
	Json->SetArrayField(TEXT("blueprint_only_members"), RepNodeStringValues(BlueprintOnly));
	Json->SetArrayField(TEXT("required_acknowledgment"), RepNodeStringValues(Required));
	Json->SetArrayField(TEXT("type_mismatches"), MismatchValues);
	Json->SetBoolField(TEXT("would_change"), Plan.WouldChange());
	return Json;
}

bool RepNodeReadStringParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FString& OutValue, bool& bOutPresent, FCortexCommandResult& OutError)
{
	OutValue.Reset();
	bOutPresent = false;
	if (!Params->HasField(Field))
	{
		return true;
	}
	if (!Params->TryGetStringField(Field, OutValue))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("%s must be a string"), Field));
		return false;
	}
	bOutPresent = !OutValue.IsEmpty();
	return true;
}

bool RepNodeReadBoolParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, const bool bDefault, bool& bOutValue, FCortexCommandResult& OutError)
{
	bOutValue = bDefault;
	if (!Params->HasField(Field))
	{
		return true;
	}
	if (!Params->TryGetBoolField(Field, bOutValue))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("%s must be a boolean"), Field));
		return false;
	}
	return true;
}

UScriptStruct* RepNodeFindStruct(const FString& Path)
{
	if (UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *Path))
	{
		return Struct;
	}
	return FindFirstObject<UScriptStruct>(*Path, EFindFirstObjectOptions::NativeFirst);
}

UClass* RepNodeFindClass(const FString& Path)
{
	if (UClass* Class = LoadClass<UObject>(nullptr, *Path))
	{
		return Class;
	}
	return FindFirstObject<UClass>(*Path, EFindFirstObjectOptions::NativeFirst);
}

/** One replaced node's pre-call content: the whole editor node (Node, Instance, InstanceObject, execution runtime data, ID, expression fields) and where it lives. */
struct FRepNodeJournalEntry
{
	FCortexSTNodeLocation Location;
	FStateTreeEditorNode Copy;
};

/** Everything an apply overwrites, so a failure can be undone by content rather than by the transaction record. */
struct FRepNodeJournal
{
	TArray<FRepNodeJournalEntry> Entries;
	CortexSTCompat::FEditorBindingsJournal Bindings;
	bool bWasDirty = false;
	int32 BindingsBefore = 0;
	/** Whole-tree report before the call: the content readback of the rollback compares against it. */
	TSharedPtr<FJsonObject> TreeReport;
};

TSet<FCortexSTReplaceNodeOps::EFaultPoint>& RepNodeFaultStorage()
{
	static TSet<FCortexSTReplaceNodeOps::EFaultPoint> Points;
	return Points;
}

/** Full content equality between a live editor node and its journal copy: structs, values, flags and expression fields. */
bool RepNodeContentMatches(const FRepNodeJournalEntry& Entry)
{
	const FStateTreeEditorNode& Live = *Entry.Location.Node;
	const FStateTreeEditorNode& Copy = Entry.Copy;
	if (Live.ID != Copy.ID || Live.ExpressionIndent != Copy.ExpressionIndent || Live.ExpressionOperand != Copy.ExpressionOperand)
	{
		return false;
	}
	if (Live.Node.GetScriptStruct() != Copy.Node.GetScriptStruct()
		|| Live.Instance.GetScriptStruct() != Copy.Instance.GetScriptStruct()
		|| !CortexSTCompat::EditorNodeRuntimeDataMatches(Live, Copy)
		|| Live.InstanceObject != Copy.InstanceObject
		|| Live.GetName() != Copy.GetName())
	{
		return false;
	}

	const auto ContentsMatch = [](const UStruct* Struct, const void* LiveMemory, const void* CopyMemory)
	{
		if (Struct == nullptr)
		{
			return true;
		}
		if (LiveMemory == nullptr || CopyMemory == nullptr)
		{
			return LiveMemory == CopyMemory;
		}
		for (const FProperty* Property : TFieldRange<FProperty>(Struct, EFieldIteratorFlags::IncludeSuper))
		{
			if (Property->HasAnyPropertyFlags(CPF_Transient))
			{
				continue;
			}
			if (!Property->Identical(
				Property->ContainerPtrToValuePtr<void>(LiveMemory),
				Property->ContainerPtrToValuePtr<void>(const_cast<void*>(CopyMemory)),
				PPF_None))
			{
				return false;
			}
		}
		return true;
	};

	return ContentsMatch(Copy.Node.GetScriptStruct(), Live.Node.GetMemory(), Copy.Node.GetMemory())
		&& ContentsMatch(Copy.Instance.GetScriptStruct(), Live.Instance.GetMemory(), Copy.Instance.GetMemory())
		&& (Copy.InstanceObject == nullptr
			|| ContentsMatch(Copy.InstanceObject->GetClass(), Live.InstanceObject, Copy.InstanceObject));
}

bool RepNodeFault(const FCortexSTReplaceNodeOps::EFaultPoint Point)
{
	return RepNodeFaultStorage().Contains(Point);
}

int32& RepNodeCompileCount()
{
	static int32 Count = 0;
	return Count;
}

/** Every compile this command issues, so the recovery compile is counted too. */
bool RepNodeCompile(UStateTree* StateTree, FStateTreeCompilerLog& Log)
{
	++RepNodeCompileCount();
	return CortexSTCompat::CompileStateTree(StateTree, Log);
}

void* RepNodeInstanceContainer(FStateTreeEditorNode& Node)
{
	return Node.InstanceObject != nullptr ? static_cast<void*>(Node.InstanceObject) : Node.Instance.GetMutableMemory();
}

const void* RepNodeInstanceContainer(const FStateTreeEditorNode& Node)
{
	return Node.InstanceObject != nullptr ? static_cast<const void*>(Node.InstanceObject) : Node.Instance.GetMemory();
}

FCortexSTNodeLocation LocateById(const TArray<FCortexSTNodeLocation>& Locations, const FGuid& NodeId)
{
	for (const FCortexSTNodeLocation& Location : Locations)
	{
		if (Location.Node != nullptr && Location.Node->ID == NodeId)
		{
			return Location;
		}
	}
	return FCortexSTNodeLocation();
}

/** Keeps the wrapper node struct and points it at the new native Blueprint-base class (TaskClass/ConditionClass/EvaluatorClass/ConsiderationClass). */
bool RepNodeSetWrapperClass(FStateTreeEditorNode& Node, const UClass* NewClass)
{
	UScriptStruct* Wrapper = const_cast<UScriptStruct*>(Node.Node.GetScriptStruct());
	if (Wrapper == nullptr || NewClass == nullptr)
	{
		return false;
	}
	const FClassProperty* ClassProperty = nullptr;
	for (const TCHAR* Candidate : { TEXT("TaskClass"), TEXT("ConditionClass"), TEXT("EvaluatorClass"), TEXT("ConsiderationClass") })
	{
		ClassProperty = FindFProperty<FClassProperty>(Wrapper, FName(Candidate));
		if (ClassProperty != nullptr)
		{
			break;
		}
	}
	if (ClassProperty == nullptr || ClassProperty->MetaClass == nullptr || !NewClass->IsChildOf(ClassProperty->MetaClass))
	{
		return false;
	}
	ClassProperty->SetObjectPropertyValue_InContainer(Node.Node.GetMutableMemory(), const_cast<UClass*>(NewClass));
	return true;
}
}

bool FCortexSTReplacePlan::HasMismatches() const
{
	for (const FCortexSTNodeReplacePlan& Node : Nodes)
	{
		if (!Node.Mismatches.IsEmpty())
		{
			return true;
		}
	}
	return false;
}

bool FCortexSTReplacePlan::HasUnresolved() const
{
	if (!ValidationDrops.IsEmpty())
	{
		return true;
	}
	for (const FCortexSTNodeReplacePlan& Node : Nodes)
	{
		if (!Node.Unresolved.IsEmpty())
		{
			return true;
		}
	}
	return false;
}

bool FCortexSTReplacePlan::NeedsAcknowledgment() const
{
	for (const FCortexSTNodeReplacePlan& Node : Nodes)
	{
		if (!Node.RequiredAcknowledgment.IsEmpty())
		{
			return true;
		}
	}
	return false;
}

bool FCortexSTReplacePlan::WouldChange() const
{
	for (const FCortexSTNodeReplacePlan& Node : Nodes)
	{
		if (Node.bWouldChange)
		{
			return true;
		}
	}
	return false;
}

bool FCortexSTReplaceNodeOps::BuildPlan(
	const FCortexSTAssetContext& Context,
	const TArray<FCortexSTNodeLocation>& Locations,
	const FCortexSTReplaceRequest& Request,
	FCortexSTReplacePlan& OutPlan,
	FCortexCommandResult& OutError)
{
	OutPlan = FCortexSTReplacePlan();
	OutPlan.BindingsBefore = FCortexSTBindingOps::CountTreeBindings(Context);

	TArray<CortexSTCompat::FBindingInfo> Bindings;
	Bindings.Reserve(OutPlan.BindingsBefore);
	for (int32 Index = 0; Index < OutPlan.BindingsBefore; ++Index)
	{
		CortexSTCompat::ReadEditorBinding(*Context.EditorData, Index, Bindings.AddDefaulted_GetRef());
	}

	const FRepNodeTargetTypes Target = RepNodeResolveTargetTypes(Request.Target);

	// The compile validates first and RemoveInvalidBindings() deletes: a missing target struct, an empty target path, an unresolved
	// target path, and an unresolved source path only when StateTree.Editor.AutoRemoveSourcePathInvalidEditorBindings is on
	// (StateTreeEditorData.cpp:1230-1245, PropertyBindingBindingCollection.cpp:198-257). Refuse them now so the readback can never
	// discover a count that shrank.
	{
		const IConsoleVariable* AutoRemoveSourcePath =
			IConsoleManager::Get().FindConsoleVariable(TEXT("StateTree.Editor.AutoRemoveSourcePathInvalidEditorBindings"));
		const bool bRemoveUnresolvedSourcePath = AutoRemoveSourcePath != nullptr && AutoRemoveSourcePath->GetBool();
		for (const CortexSTCompat::FBindingInfo& Binding : Bindings)
		{
			FString Reason;
			if (!Binding.Target.bStructFound)
			{
				Reason = TEXT("the target struct is not part of this StateTree");
			}
			else if (Binding.Target.Segments.IsEmpty())
			{
				Reason = TEXT("the target path is empty");
			}
			else if (!Binding.Target.bResolved)
			{
				Reason = FString::Printf(TEXT("the target path does not resolve: %s"), *Binding.Target.ResolveError);
			}
			else if (bRemoveUnresolvedSourcePath && !Binding.Source.bResolved)
			{
				Reason = FString::Printf(TEXT("the source path does not resolve and validation removes those bindings: %s"), *Binding.Source.ResolveError);
			}
			if (!Reason.IsEmpty())
			{
				FCortexSTReplaceUnresolved& Drop = OutPlan.ValidationDrops.AddDefaulted_GetRef();
				Drop.BindingIndex = Binding.Index;
				Drop.Side = CortexSTCompat::EBindingSide::Target;
				Drop.Path = Binding.Target.Path;
				Drop.Reason = FString::Printf(TEXT("validation would remove this binding: %s"), *Reason);
			}
		}
	}

	for (const FCortexSTNodeLocation& Location : Locations)
	{
		FCortexSTNodeReplacePlan& NodePlan = OutPlan.Nodes.AddDefaulted_GetRef();
		if (!RepNodeBuildNodePlan(Context, Location, Request, Target, Bindings, NodePlan, OutError))
		{
			return false;
		}
	}
	return true;
}

FCortexCommandResult FCortexSTReplaceNodeOps::ReplaceNode(const TSharedPtr<FJsonObject>& Params)
{
	if (!CortexSTCompat::bNodeBindingsAvailable)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::NotAvailable,
			TEXT("Replacing StateTree nodes with binding remap needs Unreal Engine 5.7 or newer"));
	}

	FString AssetPath;
	FCortexCommandResult Error;
	if (!CortexST::GetRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return Error;
	}

	// Mutation guard first: a blocked asset is refused before anything is read, planned or mutated.
	FString BlockedReason;
	if (FCortexAssetMutationGuard::IsPathBlocked(AssetPath, BlockedReason))
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("Asset path is blocked by a previous failed operation: %s (%s)"), *AssetPath, *BlockedReason),
			RepNodeDetails(TEXT("asset_path"), AssetPath));
	}

	FString NodeIdText;
	FString BlueprintClassText;
	FString NewStructText;
	FString NewClassText;
	bool bHasNodeId = false;
	bool bHasBlueprintClass = false;
	bool bHasNewStruct = false;
	bool bHasNewClass = false;
	if (!RepNodeReadStringParam(Params, TEXT("node_id"), NodeIdText, bHasNodeId, Error)
		|| !RepNodeReadStringParam(Params, TEXT("blueprint_class"), BlueprintClassText, bHasBlueprintClass, Error)
		|| !RepNodeReadStringParam(Params, TEXT("new_node_struct"), NewStructText, bHasNewStruct, Error)
		|| !RepNodeReadStringParam(Params, TEXT("new_node_class"), NewClassText, bHasNewClass, Error))
	{
		return Error;
	}
	if (bHasNodeId == bHasBlueprintClass)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Specify exactly one of node_id or blueprint_class"));
	}
	if (bHasNewStruct == bHasNewClass)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Specify exactly one of new_node_struct or new_node_class"));
	}

	bool bDryRun = false;
	bool bCompile = true;
	bool bSave = false;
	if (!RepNodeReadBoolParam(Params, TEXT("dry_run"), false, bDryRun, Error)
		|| !RepNodeReadBoolParam(Params, TEXT("compile"), true, bCompile, Error)
		|| !RepNodeReadBoolParam(Params, TEXT("save"), false, bSave, Error))
	{
		return Error;
	}

	FCortexSTReplaceRequest Request;
	if (Params->HasField(TEXT("property_map")))
	{
		const TSharedPtr<FJsonObject>* MapObject = nullptr;
		if (!Params->TryGetObjectField(TEXT("property_map"), MapObject) || MapObject == nullptr)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("property_map must be an object of \"OldProperty\": \"NewProperty\""));
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : (*MapObject)->Values)
		{
			FString NewName;
			if (!Entry.Value.IsValid() || !Entry.Value->TryGetString(NewName) || NewName.IsEmpty() || Entry.Key.IsEmpty())
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("property_map value for '%s' must be a non-empty string"), *Entry.Key));
			}
			Request.PropertyMap.Add(FName(*Entry.Key), FName(*NewName));
		}
	}
	if (Params->HasField(TEXT("acknowledged_losses")))
	{
		const TArray<TSharedPtr<FJsonValue>>* Acknowledged = nullptr;
		if (!Params->TryGetArrayField(TEXT("acknowledged_losses"), Acknowledged) || Acknowledged == nullptr)
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("acknowledged_losses must be an array of property names"));
		}
		for (const TSharedPtr<FJsonValue>& Value : *Acknowledged)
		{
			FString Name;
			if (!Value.IsValid() || !Value->TryGetString(Name))
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("acknowledged_losses must be an array of property names"));
			}
			Request.AcknowledgedLosses.Add(Name);
		}
	}

	FGuid NodeId;
	if (bHasNodeId && !FGuid::Parse(NodeIdText, NodeId))
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("node_id is not a GUID: %s"), *NodeIdText));
	}

	// ── The new type: native only ──
	if (bHasNewStruct)
	{
		const UScriptStruct* Struct = RepNodeFindStruct(NewStructText);
		if (Struct == nullptr || !Struct->IsChildOf(FStateTreeNodeBase::StaticStruct()))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("new_node_struct '%s' is not a StateTree node struct"), *NewStructText),
				RepNodeDetails(TEXT("new_node_struct"), NewStructText));
		}
		if (Struct->IsChildOf(FStateTreeBlueprintTaskWrapper::StaticStruct())
			|| Struct->IsChildOf(FStateTreeBlueprintEvaluatorWrapper::StaticStruct())
			|| Struct->IsChildOf(FStateTreeBlueprintConditionWrapper::StaticStruct())
			|| Struct->IsChildOf(FStateTreeBlueprintConsiderationWrapper::StaticStruct()))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("new_node_struct must be a native node struct; use new_node_class for a native Blueprint-base class"),
				RepNodeDetails(TEXT("new_node_struct"), NewStructText));
		}
		Request.Target.NodeStruct = Struct;
	}
	else
	{
		const UClass* Class = RepNodeFindClass(NewClassText);
		if (Class == nullptr)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("new_node_class '%s' does not resolve to a class"), *NewClassText),
				RepNodeDetails(TEXT("new_node_class"), NewClassText));
		}
		if (Class->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("new_node_class must be a native class (native only); a Blueprint class is what replace_node moves away from"),
				RepNodeDetails(TEXT("new_node_class"), NewClassText));
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("new_node_class must be a concrete, current native class"),
				RepNodeDetails(TEXT("new_node_class"), NewClassText));
		}
		const bool bBlueprintBase = Class->IsChildOf(UStateTreeTaskBlueprintBase::StaticClass())
			|| Class->IsChildOf(UStateTreeEvaluatorBlueprintBase::StaticClass())
			|| Class->IsChildOf(UStateTreeConditionBlueprintBase::StaticClass())
			|| Class->IsChildOf(UStateTreeConsiderationBlueprintBase::StaticClass());
		if (!bBlueprintBase)
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("new_node_class must derive a StateTree Blueprint base (task, condition, evaluator or consideration)"),
				RepNodeDetails(TEXT("new_node_class"), NewClassText));
		}
		Request.Target.NodeClass = Class;
	}

	FCortexSTAssetContext Context;
	if (!CortexST::LoadAssetContext(AssetPath, Context, Error))
	{
		return Error;
	}
	if (!CortexST::CheckExpectedFingerprint(Context.StateTree, Params, Error))
	{
		return Error;
	}

	// ── Locate ──
	TArray<FCortexSTNodeLocation> Locations;
	if (bHasNodeId)
	{
		FCortexSTNodeLocation Location;
		if (!CortexST::FindEditorNode(Context, NodeId, Location))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::NodeNotFound,
				FString::Printf(TEXT("No StateTree node has the editor ID %s"), *NodeIdText),
				RepNodeDetails(TEXT("node_id"), NodeIdText));
		}
		Locations.Add(Location);
	}
	else
	{
		const UClass* BlueprintClass = RepNodeFindClass(BlueprintClassText);
		CortexST::FindEditorNodesByBlueprintClass(Context, BlueprintClass, BlueprintClassText, Locations);
		if (Locations.IsEmpty())
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::NodeNotFound,
				FString::Printf(TEXT("No StateTree node uses the Blueprint class %s"), *BlueprintClassText),
				RepNodeDetails(TEXT("blueprint_class"), BlueprintClassText));
		}
	}

	// ── Plan ──
	FCortexSTReplacePlan Plan;
	if (!BuildPlan(Context, Locations, Request, Plan, Error))
	{
		return Error;
	}
	TSharedPtr<FJsonObject> PlanJson = RepNodePlanJson(Plan);
	PlanJson->SetStringField(TEXT("asset_path"), Context.AssetPath);
	PlanJson->SetObjectField(TEXT("fingerprint"), CortexST::MakeFingerprint(Context.StateTree));
	// Every refusal carries the same unresolved list, so a caller always sees which binding blocked the call.
	PlanJson->SetArrayField(TEXT("unresolved_bindings"), PlanJson->GetObjectField(TEXT("bindings"))->GetArrayField(TEXT("unresolved")));

	if (Plan.HasMismatches())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::TypeMismatch,
			TEXT("A mapped property pair is not type- or usage-compatible; nothing was changed"),
			PlanJson);
	}
	if (Plan.NeedsAcknowledgment())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::PotentialDataLoss,
			TEXT("The replacement drops non-default values; list the keys of required_acknowledgment in acknowledged_losses to proceed"),
			PlanJson);
	}
	if (Plan.HasUnresolved())
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::VerificationFailed,
			TEXT("A binding would not resolve on the new type; replace_node never drops a binding, nothing was changed"),
			PlanJson);
	}

	PlanJson->SetBoolField(TEXT("dry_run"), bDryRun);
	if (bDryRun || !Plan.WouldChange())
	{
		return FCortexCommandRouter::Success(PlanJson);
	}

	// ── Journal, then apply (design decision 2 steps 5-6) ──
	FRepNodeJournal Journal;
	Journal.BindingsBefore = Plan.BindingsBefore;
	Journal.Bindings.Count = Plan.BindingsBefore;
	CortexSTCompat::CaptureEditorBindings(*Context.EditorData, Journal.Bindings);
	UPackage* Package = Context.StateTree->GetOutermost();
	Journal.bWasDirty = Package != nullptr && Package->IsDirty();
	Journal.TreeReport = FCortexSTBindingOps::BuildTreeReport(Context);
	TArray<FGuid> ReplacedIds;
	Journal.Entries.Reserve(Plan.Nodes.Num());
	for (const FCortexSTNodeReplacePlan& NodePlan : Plan.Nodes)
	{
		FRepNodeJournalEntry& Entry = Journal.Entries.AddDefaulted_GetRef();
		Entry.Location = LocateById(Locations, NodePlan.NodeId);
		Entry.Copy = *Entry.Location.Node;
		ReplacedIds.Add(NodePlan.NodeId);
	}

	TUniquePtr<FScopedTransaction> Transaction = MakeUnique<FScopedTransaction>(FText::FromString(
		FString::Printf(TEXT("Cortex: replace StateTree nodes %s"), *FPackageName::GetShortName(Context.AssetPath))));
	Context.StateTree->Modify();
	Context.EditorData->Modify();
	{
		TSet<UObject*> ModifiedStates;
		for (const FRepNodeJournalEntry& Entry : Journal.Entries)
		{
			if (Entry.Location.OwnerState != nullptr && !ModifiedStates.Contains(Entry.Location.OwnerState))
			{
				ModifiedStates.Add(Entry.Location.OwnerState);
				Entry.Location.OwnerState->Modify();
			}
		}
	}

	bool bTouchedCompile = false;
	for (int32 Index = 0; Index < Plan.Nodes.Num(); ++Index)
	{
		const FCortexSTNodeReplacePlan& NodePlan = Plan.Nodes[Index];
		FRepNodeJournalEntry& Entry = Journal.Entries[Index];
		FStateTreeEditorNode& Node = *Entry.Location.Node;
		if (!NodePlan.bWouldChange)
		{
			continue;
		}

		if (Request.Target.NodeStruct != nullptr)
		{
			// Node.Node.InitializeAs, not FStateTreeEditorNode::InitializeAs: the editor node ID is what every binding is keyed by.
			CortexSTCompat::InitializeNodeAs(Node, Request.Target.NodeStruct);
		}
		else if (!RepNodeSetWrapperClass(Node, Request.Target.NodeClass))
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidField,
				TEXT("The Blueprint wrapper does not accept the requested class"),
				RepNodeDetails(TEXT("new_node_class"), Request.Target.NodeClass->GetPathName()));
		}
		// The owning state for state slots, the editor data for global tasks and evaluators (StateTreeState.cpp, FixObjectNodes).
		CortexSTCompat::ReallocInstanceDataKeepingId(Node, Entry.Location.InstanceOuter);

		// F3: editor-owned controls live on the node struct (bTaskEnabled, bConsideredForCompletion, TransitionHandlingPriority),
		// not on the instance object the property plan enumerates; carry every member the two node types share.
		TSet<FName> PlannedFlagNames;
		for (const FCortexSTReplaceProperty& Planned : NodePlan.Properties)
		{
			if (Planned.Kind == ECortexSTReplacePropertyKind::NodeFlag)
			{
				PlannedFlagNames.Add(Planned.OldName);
			}
		}
		if (Entry.Copy.Node.GetScriptStruct() != Node.Node.GetScriptStruct())
		{
			const UScriptStruct* OldNodeStruct = Entry.Copy.Node.GetScriptStruct();
			const UScriptStruct* NewNodeStruct = Node.Node.GetScriptStruct();
			if (OldNodeStruct != nullptr && NewNodeStruct != nullptr)
			{
				for (TFieldIterator<FProperty> It(OldNodeStruct, EFieldIteratorFlags::IncludeSuper); It; ++It)
				{
					const FProperty* OldControl = *It;
					const UStruct* Declaring = OldControl->GetOwnerStruct();
					if (Declaring == nullptr || !NewNodeStruct->IsChildOf(Declaring))
					{
						// Wrapper-only members (TaskClass, TaskFlags) are not shared by the replacement type.
						continue;
					}
					if (OldControl->GetFName() == TEXT("Name") || RepNodeIsNodeBookkeeping(OldControl->GetFName())
						|| PlannedFlagNames.Contains(OldControl->GetFName()))
					{
						// The property plan decides every flag it lists (copied, default-equal, mapped or acknowledged); the shared
						// copy only carries members the plan never saw, so the two can never disagree about one value.
						continue;
					}
					const FProperty* NewControl = NewNodeStruct->FindPropertyByName(OldControl->GetFName());
					FString Reason;
					if (NewControl == nullptr || !RepNodeArePropertiesCompatible(OldControl, NewControl, Reason))
					{
						continue;
					}
					RepNodeCopyValue(OldControl, Entry.Copy.Node.GetMemory(), NewControl, Node.Node.GetMutableMemory());
				}
			}
		}


		// Values come from the journal copy, which still holds the old instance data and object.
		for (const FCortexSTReplaceProperty& Property : NodePlan.Properties)
		{
			if (Property.Status != ECortexSTReplacePropertyStatus::Copied || Property.OldProperty == nullptr || Property.NewProperty == nullptr)
			{
				continue;
			}
			// F1: the old property is declared by the Blueprint instance class, so its container is always the journaled instance
			// object, never the wrapper struct memory its offset knows nothing about.
			void* NewContainer = Property.Kind == ECortexSTReplacePropertyKind::NodeFlag && Request.Target.NodeStruct != nullptr
				? Node.Node.GetMutableMemory()
				: RepNodeInstanceContainer(Node);
			const void* OldContainer = RepNodeInstanceContainer(Entry.Copy);
			RepNodeCopyValue(Property.OldProperty, OldContainer, Property.NewProperty, NewContainer);
		}
		// Keeps the node's own name (a Blueprint node is named after its class until the user renames it).
		CortexSTCompat::SetEditorNodeName(Node, Entry.Copy.GetName());
	}

	for (const FCortexSTNodeReplacePlan& NodePlan : Plan.Nodes)
	{
		for (const FCortexSTReplaceRewrite& Rewrite : NodePlan.Rewrites)
		{
			if (!CortexSTCompat::RemapBindingSegments(*Context.EditorData, Rewrite.BindingIndex, Rewrite.Side, Rewrite.NewFirstSegment))
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::VerificationFailed,
					FString::Printf(TEXT("Binding %d could not be rewritten"), Rewrite.BindingIndex),
					PlanJson);
			}
		}
	}

	// One failure route for every phase after the first mutation.
	auto Recover = [&](const TCHAR* Phase, const FString& Reason, const FString& ErrorCode) -> FCortexCommandResult
	{
		TArray<FString> Problems;
		bool bRestoreOk = true;
		if (RepNodeFault(EFaultPoint::SkipJournalRestore))
		{
			Problems.Add(TEXT("journal restore skipped"));
			bRestoreOk = false;
		}
		else
		{
			for (FRepNodeJournalEntry& Entry : Journal.Entries)
			{
				if (Entry.Location.Node != nullptr)
				{
					*Entry.Location.Node = Entry.Copy;
				}
			}
			if (!CortexSTCompat::RestoreEditorBindings(*Context.EditorData, Journal.Bindings))
			{
				Problems.Add(TEXT("the stored binding array could not be restored in place"));
				bRestoreOk = false;
			}
		}
		if (RepNodeFault(EFaultPoint::AfterRestore))
		{
			Problems.Add(TEXT("injected restore failure"));
			bRestoreOk = false;
		}
		// Cancel() only drops the undo record; the restored content is what reverts the state (ScopedTransaction.cpp:42-48).
		Transaction->Cancel();
		Transaction.Reset();

		bool bRecoveryCompiled = false;
		if (bTouchedCompile)
		{
			FStateTreeCompilerLog RecoveryLog;
			bRecoveryCompiled = RepNodeCompile(Context.StateTree, RecoveryLog);
		}
		if (Package != nullptr)
		{
			Package->SetDirtyFlag(Journal.bWasDirty);
		}

		const TSharedPtr<FJsonObject> After = FCortexSTBindingOps::BuildTreeReport(Context);
		const bool bReportEqual = Journal.TreeReport.IsValid() && After.IsValid()
			&& FJsonValue::CompareEqual(
				*MakeShared<FJsonValueObject>(Journal.TreeReport),
				*MakeShared<FJsonValueObject>(After));
		if (!bReportEqual)
		{
			Problems.Add(TEXT("the whole-tree binding report differs from the pre-call report"));
		}
		// The report carries node types and bindings, not values or flags: compare the journaled nodes by content too.
		bool bNodesEqual = true;
		for (const FRepNodeJournalEntry& Entry : Journal.Entries)
		{
			if (Entry.Location.Node != nullptr && !RepNodeContentMatches(Entry))
			{
				bNodesEqual = false;
				Problems.Add(FString::Printf(TEXT("node %s differs from its journal copy"), *RepNodeGuidText(Entry.Copy.ID)));
			}
		}
		const bool bContentEqual = bReportEqual && bNodesEqual;
		// F6: restoring the data does not restore a runnable tree. When the apply compiled, the recovery compile has to succeed and
		// the tree has to be ready again, otherwise recovery is unverified (the engine cleared the compiled data on failure,
		// StateTreeCompiler.cpp:454-462).
		const bool bRecoveryVerified = !bTouchedCompile || (bRecoveryCompiled && Context.StateTree->IsReadyToRun());
		if (!bRecoveryVerified)
		{
			Problems.Add(TEXT("the recovery compile did not restore a runnable tree"));
		}
		const bool bRestored = bRestoreOk && bContentEqual && bRecoveryVerified;
		if (!bRestored)
		{
			FCortexAssetMutationGuard::Block(Context.StateTree, FString::Printf(
				TEXT("statetree.replace_node: the pre-call state could not be restored and verified after a %s failure"), Phase));
		}

		TSharedPtr<FJsonObject> Failure = RepNodeDetails(TEXT("asset_path"), Context.AssetPath);
		Failure->SetStringField(TEXT("failed_phase"), Phase);
		Failure->SetStringField(TEXT("failure"), Reason);
		Failure->SetStringField(TEXT("rollback_status"), bRestored ? TEXT("restored") : TEXT("verification_failed"));
		Failure->SetStringField(TEXT("recovery_compile"), bRecoveryCompiled ? TEXT("succeeded") : (bTouchedCompile ? TEXT("failed") : TEXT("not_needed")));
		Failure->SetBoolField(TEXT("content_restored"), bRestored);
		Failure->SetBoolField(TEXT("asset_blocked"), !bRestored);
		Failure->SetArrayField(TEXT("restore_problems"), RepNodeStringValues(Problems));
		Failure->SetObjectField(TEXT("fingerprint"), CortexST::MakeFingerprint(Context.StateTree));
		return FCortexCommandRouter::Error(
			bRestored ? ErrorCode : CortexErrorCodes::VerificationFailed,
			FString::Printf(TEXT("Replacing StateTree nodes failed in the %s phase (%s); %s"), Phase, *Reason,
				bRestored ? TEXT("the pre-call content was restored and verified") : TEXT("the pre-call content could not be verified and the asset is blocked")),
			Failure);
	};

	// ── Compile ──
	TSharedPtr<FJsonObject> CompilePayload;
	if (bCompile)
	{
		bTouchedCompile = true;
		if (RepNodeFault(EFaultPoint::Compile))
		{
			return Recover(TEXT("compile"), TEXT("injected compile failure"), CortexErrorCodes::CompileFailed);
		}
		FStateTreeCompilerLog CompileLog;
		const bool bCompiled = RepNodeCompile(Context.StateTree, CompileLog);
		CompilePayload = FCortexSTValidationOps::MakeCompilePayload(Context.AssetPath, Context.StateTree, CompileLog, bCompiled);
		const int32 ErrorCount = static_cast<int32>(CompilePayload->GetNumberField(TEXT("error_count")));
		if (!bCompiled || ErrorCount > 0 || !Context.StateTree->IsReadyToRun())
		{
			return Recover(TEXT("compile"),
				FString::Printf(TEXT("%d compile error(s) after the replacement"), ErrorCount),
				CortexErrorCodes::CompileFailed);
		}
	}

	// ── Readback: the whole-tree binding count and every rewritten path ──
	TArray<FString> Mismatches;
	{
		const int32 AfterCount = FCortexSTBindingOps::CountTreeBindings(Context);
		if (AfterCount != Journal.BindingsBefore)
		{
			Mismatches.Add(FString::Printf(TEXT("the whole-tree binding count changed from %d to %d"), Journal.BindingsBefore, AfterCount));
		}
		const TSharedPtr<FJsonObject> NodeReport = FCortexSTBindingOps::BuildNodesReport(Context, ReplacedIds);
		const TArray<TSharedPtr<FJsonValue>>* NodeValues = nullptr;
		NodeReport->TryGetArrayField(TEXT("nodes"), NodeValues);
		if (NodeValues == nullptr || NodeValues->Num() != Plan.Nodes.Num())
		{
			Mismatches.Add(TEXT("a replaced node is missing from the readback"));
		}
		else
		{
			for (int32 Index = 0; Index < Plan.Nodes.Num(); ++Index)
			{
				const FCortexSTNodeReplacePlan& NodePlan = Plan.Nodes[Index];
				const TSharedPtr<FJsonObject> NodeJson = (*NodeValues)[Index]->AsObject();
				bool bMissingNode = false;
				if (NodeJson.IsValid())
				{
					NodeJson->TryGetBoolField(TEXT("missing"), bMissingNode);
				}
				if (!NodeJson.IsValid() || bMissingNode)
				{
					Mismatches.Add(FString::Printf(TEXT("node %s is missing"), *RepNodeGuidText(NodePlan.NodeId)));
					continue;
				}
				const TSharedPtr<FJsonObject> NodeType = NodeJson->HasTypedField<EJson::Object>(TEXT("node_type"))
					? NodeJson->GetObjectField(TEXT("node_type")) : nullptr;
				if (!NodeType.IsValid())
				{
					Mismatches.Add(FString::Printf(TEXT("node %s has no node_type"), *RepNodeGuidText(NodePlan.NodeId)));
					continue;
				}
				const FString ExpectedType = Request.Target.NodeStruct != nullptr
					? Request.Target.NodeStruct->GetPathName()
					: Request.Target.NodeClass->GetPathName();
				const FString ReportedType = Request.Target.NodeStruct != nullptr
					? NodeType->GetStringField(TEXT("struct_path"))
					: NodeType->GetStringField(TEXT("class_path"));
				if (ReportedType != ExpectedType)
				{
					Mismatches.Add(FString::Printf(TEXT("node %s is %s, expected %s"), *RepNodeGuidText(NodePlan.NodeId), *ReportedType, *ExpectedType));
				}
				if (NodeType->GetBoolField(TEXT("is_blueprint")))
				{
					Mismatches.Add(FString::Printf(TEXT("node %s is still a Blueprint class"), *RepNodeGuidText(NodePlan.NodeId)));
				}

				for (const FCortexSTReplaceRewrite& Rewrite : NodePlan.Rewrites)
				{
					const TCHAR* SideField = Rewrite.Side == CortexSTCompat::EBindingSide::Target ? TEXT("as_target") : TEXT("as_source");
					const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
					if (!NodeJson->TryGetArrayField(SideField, Entries) || Entries == nullptr)
					{
						Mismatches.Add(FString::Printf(TEXT("node %s has no %s"), *RepNodeGuidText(NodePlan.NodeId), SideField));
						continue;
					}
					bool bFound = false;
					for (const TSharedPtr<FJsonValue>& Value : *Entries)
					{
						const TSharedPtr<FJsonObject> Entry = Value->AsObject();
						if (!Entry.IsValid() || static_cast<int32>(Entry->GetNumberField(TEXT("binding_index"))) != Rewrite.BindingIndex)
						{
							continue;
						}
						const TCHAR* SideObjectField = Rewrite.Side == CortexSTCompat::EBindingSide::Target ? TEXT("target") : TEXT("source");
						if (!Entry->HasTypedField<EJson::Object>(SideObjectField))
						{
							continue;
						}
						const TSharedPtr<FJsonObject> SideObject = Entry->GetObjectField(SideObjectField);
						const bool bResolved = SideObject->GetBoolField(TEXT("resolved"));
						// F10: compare the resolved first segment structurally (name and array index), not a dot-prefixed string:
						// an indexed root such as Values[0] is neither equal to "Values" nor prefixed with "Values.".
						bool bSegmentMatches = Rewrite.NewFirstSegment.IsNone();
						const TArray<TSharedPtr<FJsonValue>>* SideSegments = nullptr;
						if (!bSegmentMatches && SideObject->TryGetArrayField(TEXT("segments"), SideSegments) && SideSegments != nullptr && SideSegments->Num() > 0)
						{
							const TSharedPtr<FJsonObject> FirstSegment = (*SideSegments)[0]->AsObject();
							FString SegmentName;
							double SegmentIndex = INDEX_NONE;
							const bool bHasIndex = FirstSegment.IsValid() && FirstSegment->TryGetNumberField(TEXT("array_index"), SegmentIndex);
							bSegmentMatches = FirstSegment.IsValid()
								&& FirstSegment->TryGetStringField(TEXT("name"), SegmentName)
								&& SegmentName == Rewrite.NewFirstSegment.ToString()
								&& (Rewrite.FirstSegmentArrayIndex == INDEX_NONE
									? !bHasIndex
									: (bHasIndex && static_cast<int32>(SegmentIndex) == Rewrite.FirstSegmentArrayIndex));
						}
						bFound = bResolved && bSegmentMatches;
						if (bFound)
						{
							break;
						}
					}
					if (!bFound)
					{
						Mismatches.Add(FString::Printf(TEXT("binding %d (%s) of node %s does not resolve as %s"),
							Rewrite.BindingIndex, Rewrite.Side == CortexSTCompat::EBindingSide::Target ? TEXT("target") : TEXT("source"),
							*RepNodeGuidText(NodePlan.NodeId), *Rewrite.NewFirstSegment.ToString()));
					}
				}
			}
		}
	}
	if (RepNodeFault(EFaultPoint::Readback))
	{
		Mismatches.Add(TEXT("injected readback mismatch"));
	}
	if (!Mismatches.IsEmpty())
	{
		return Recover(TEXT("readback"), FString::Join(Mismatches, TEXT("; ")), CortexErrorCodes::VerificationFailed);
	}

	// ── Save only after the readback ──
	bool bSaved = false;
	TSharedPtr<FJsonObject> SavedFingerprint;
	if (bSave)
	{
		if (RepNodeFault(EFaultPoint::Save))
		{
			return Recover(TEXT("save"), TEXT("injected save failure"), CortexErrorCodes::SaveFailed);
		}
		const FCortexCommandResult SaveResult = FCortexSTAssetOps::SaveAsset(Context.AssetPath);
		if (!SaveResult.bSuccess)
		{
			return Recover(TEXT("save"), SaveResult.ErrorMessage, CortexErrorCodes::SaveFailed);
		}
		bSaved = true;
		if (SaveResult.Data.IsValid() && SaveResult.Data->HasTypedField<EJson::Object>(TEXT("fingerprint")))
		{
			SavedFingerprint = SaveResult.Data->GetObjectField(TEXT("fingerprint"));
		}
	}
	Transaction.Reset();

	TArray<TSharedPtr<FJsonValue>> ReplacedValues;
	for (const FCortexSTNodeReplacePlan& NodePlan : Plan.Nodes)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("node_id"), RepNodeGuidText(NodePlan.NodeId));
		Entry->SetStringField(TEXT("name"), NodePlan.NodeName);
		Entry->SetStringField(TEXT("slot"), CortexST::LexNodeSlot(NodePlan.Slot));
		Entry->SetStringField(TEXT("owner_state_path"), NodePlan.OwnerStatePath);
		Entry->SetStringField(TEXT("old_type_path"), NodePlan.OldTypePath);
		Entry->SetStringField(TEXT("new_type_path"), Request.Target.NodeStruct != nullptr
			? Request.Target.NodeStruct->GetPathName() : Request.Target.NodeClass->GetPathName());
		ReplacedValues.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> BindingsJson = PlanJson->GetObjectField(TEXT("bindings"));
	BindingsJson->SetNumberField(TEXT("after_count"), FCortexSTBindingOps::CountTreeBindings(Context));

	PlanJson->SetBoolField(TEXT("dry_run"), false);
	PlanJson->SetArrayField(TEXT("replaced"), ReplacedValues);
	if (CompilePayload.IsValid())
	{
		PlanJson->SetObjectField(TEXT("compile"), CompilePayload);
		PlanJson->SetStringField(TEXT("compile_status"), CompilePayload->GetStringField(TEXT("status")));
		PlanJson->SetNumberField(TEXT("error_count"), static_cast<int32>(CompilePayload->GetNumberField(TEXT("error_count"))));
	}
	else
	{
		PlanJson->SetStringField(TEXT("compile_status"), TEXT("not_run"));
		PlanJson->SetNumberField(TEXT("error_count"), 0);
	}
	PlanJson->SetBoolField(TEXT("ready_to_run"), Context.StateTree->IsReadyToRun());
	PlanJson->SetBoolField(TEXT("saved"), bSaved);
	PlanJson->SetBoolField(TEXT("dirty"), Package != nullptr && Package->IsDirty());
	if (SavedFingerprint.IsValid())
	{
		PlanJson->SetObjectField(TEXT("fingerprint"), SavedFingerprint);
	}
	else
	{
		PlanJson->SetObjectField(TEXT("fingerprint"), CortexST::MakeFingerprint(Context.StateTree));
	}

	UE_LOG(LogCortexStateTree, Log, TEXT("Replaced %d StateTree node(s) in %s"), ReplacedValues.Num(), *Context.AssetPath);
	return FCortexCommandRouter::Success(PlanJson);
}

#if WITH_AUTOMATION_TESTS
void FCortexSTReplaceNodeOps::SetFaultPointForTesting(const EFaultPoint Point)
{
	RepNodeFaultStorage().Add(Point);
}

int32 FCortexSTReplaceNodeOps::GetCompileCountForTesting()
{
	return RepNodeCompileCount();
}

void FCortexSTReplaceNodeOps::ResetCompileCountForTesting()
{
	RepNodeCompileCount() = 0;
}

void FCortexSTReplaceNodeOps::ClearFaultPointForTesting()
{
	RepNodeFaultStorage().Reset();
}
#endif
