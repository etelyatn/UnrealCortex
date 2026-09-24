#include "Operations/CortexGraphMigrationOps.h"

#include "CortexEngineCompat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_Composite.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Operations/CortexGraphImplementationOps.h"
#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphSymbolResolver.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectHash.h"

namespace
{
/** Validation budgets the reference inventory and the boundary remap never exceed. */
constexpr int32 MaxInventoryBlueprints = 512;
constexpr int32 MaxInventoryNodes = 8192;
constexpr int32 MaxMigrationPins = 64;
constexpr int32 MaxMigrationEdges = 256;

const TCHAR* const InputDirection = TEXT("input");
const TCHAR* const OutputDirection = TEXT("output");
const TCHAR* const EntryRole = TEXT("entry");
const TCHAR* const ResultRole = TEXT("result");

/** The pin_map covers every visible top-level pin of a replacement terminator. */
bool IsMappablePin(const UEdGraphPin& Pin)
{
	return Pin.ParentPin == nullptr && !Pin.bHidden;
}

/** Pins the request may never map: engine-hidden and split children. */
bool IsMappablePin(const UEdGraphPin* Pin)
{
	return Pin != nullptr && IsMappablePin(*Pin);
}

FString RoleString(const bool bResult)
{
	return bResult ? FString(ResultRole) : FString(EntryRole);
}

const TCHAR* DirectionString(const EEdGraphPinDirection Direction)
{
	return Direction == EGPD_Input ? InputDirection : OutputDirection;
}

/** Canonical identity of one link endpoint, used to canonicalize and de-duplicate planned links. */
FString EndpointKey(const FString& Role, const FString& Guid, const FName PinName)
{
	return Role.IsEmpty()
		? FString::Printf(TEXT("node:%s.%s"), *Guid, *PinName.ToString())
		: FString::Printf(TEXT("%s:%s"), *Role, *PinName.ToString());
}

/**
 * Class relationship the remapped link has to keep valid: the replacement object class must be the
 * stale class or a subclass of it, because the same downstream input keeps accepting the value.
 */
bool IsObjectCategory(const FName Category)
{
	return Category == UEdGraphSchema_K2::PC_Object
		|| Category == UEdGraphSchema_K2::PC_Interface
		|| Category == UEdGraphSchema_K2::PC_Class
		|| Category == UEdGraphSchema_K2::PC_SoftObject
		|| Category == UEdGraphSchema_K2::PC_SoftClass;
}

/**
 * Pin-level compatibility of one mapped pair. The first differing canonical dimension is reported,
 * so the refusal always names both the pin and the dimension that made the mapping unsafe.
 */
bool CompareMappedPins(
	const UEdGraphPin& Stale,
	const UEdGraphPin& Replacement,
	const FString& DeclaredEntry,
	FString& OutDimension,
	FString& OutDetail)
{
	// Direction: the declared direction, the replacement pin's real direction and the stale pin's
	// direction must all agree, otherwise the link would be re-created between incomparable ends.
	if (!FString(DirectionString(Replacement.Direction)).Equals(DeclaredEntry, ESearchCase::CaseSensitive))
	{
		OutDimension = TEXT("direction");
		OutDetail = FString::Printf(TEXT("declared as %s but the replacement pin is %s"),
			*DeclaredEntry, DirectionString(Replacement.Direction));
		return false;
	}
	if (Replacement.Direction != Stale.Direction)
	{
		OutDimension = TEXT("direction");
		OutDetail = FString::Printf(TEXT("the stale pin is %s while the replacement pin is %s"),
			DirectionString(Stale.Direction), DirectionString(Replacement.Direction));
		return false;
	}

	if (Stale.PinType.PinCategory != Replacement.PinType.PinCategory)
	{
		OutDimension = TEXT("category");
		OutDetail = FString::Printf(TEXT("stale '%s' vs replacement '%s'"),
			*Stale.PinType.PinCategory.ToString(), *Replacement.PinType.PinCategory.ToString());
		return false;
	}
	if (Stale.PinType.PinSubCategory != Replacement.PinType.PinSubCategory)
	{
		OutDimension = TEXT("subcategory");
		OutDetail = FString::Printf(TEXT("stale '%s' vs replacement '%s'"),
			*Stale.PinType.PinSubCategory.ToString(), *Replacement.PinType.PinSubCategory.ToString());
		return false;
	}
	const FString StaleObject = Stale.PinType.PinSubCategoryObject.IsValid()
		? Stale.PinType.PinSubCategoryObject->GetPathName() : FString();
	const FString ReplacementObject = Replacement.PinType.PinSubCategoryObject.IsValid()
		? Replacement.PinType.PinSubCategoryObject->GetPathName() : FString();
	if (IsObjectCategory(Stale.PinType.PinCategory))
	{
		// Assignability is direction-sensitive. A terminator *output* feeds the preserved body, so the
		// replacement class may only narrow (the body still receives what it accepted). A terminator
		// *input* receives a value the body already produces, so the replacement class may only widen
		// (it must accept everything the stale input accepted). Anything else is refused in preflight
		// instead of being discovered later by CanCreateConnection during apply.
		const UClass* const StaleClass = Cast<UClass>(Stale.PinType.PinSubCategoryObject.Get());
		const UClass* const ReplacementClass = Cast<UClass>(Replacement.PinType.PinSubCategoryObject.Get());
		if (StaleClass && !ReplacementClass)
		{
			OutDimension = TEXT("object_class");
			OutDetail = FString::Printf(TEXT("the replacement pin lost the required class '%s'"), *StaleClass->GetPathName());
			return false;
		}
		if (StaleClass && ReplacementClass && ReplacementClass != StaleClass)
		{
			const bool bReplacementIsNarrower = ReplacementClass->IsChildOf(StaleClass);
			const bool bReplacementIsWider = StaleClass->IsChildOf(ReplacementClass);
			const bool bCompatible = Replacement.Direction == EGPD_Input ? bReplacementIsWider : bReplacementIsNarrower;
			if (!bCompatible)
			{
				OutDimension = TEXT("object_class");
				OutDetail = FString::Printf(
					TEXT("%s class '%s' vs required '%s' (%s)"),
					Replacement.Direction == EGPD_Input ? TEXT("the wider input") : TEXT("the narrower output"),
					*ReplacementClass->GetPathName(), *StaleClass->GetPathName(),
					Replacement.Direction == EGPD_Input
						? TEXT("the new input must accept everything the stale input accepted")
						: TEXT("the body must still receive the stale class"));
				return false;
			}
		}
	}
	else if (StaleObject != ReplacementObject)
	{
		OutDimension = TEXT("subcategory_object");
		OutDetail = FString::Printf(TEXT("stale '%s' vs replacement '%s'"),
			StaleObject.IsEmpty() ? TEXT("<none>") : *StaleObject,
			ReplacementObject.IsEmpty() ? TEXT("<none>") : *ReplacementObject);
		return false;
	}
	if (Stale.PinType.ContainerType != Replacement.PinType.ContainerType)
	{
		OutDimension = TEXT("container");
		OutDetail = FString::Printf(TEXT("stale container kind %d vs replacement container kind %d"),
			static_cast<int32>(Stale.PinType.ContainerType), static_cast<int32>(Replacement.PinType.ContainerType));
		return false;
	}
	if (Stale.PinType.ContainerType == EPinContainerType::Map)
	{
		const FEdGraphTerminalType& StaleTerminal = Stale.PinType.PinValueType;
		const FEdGraphTerminalType& ReplacementTerminal = Replacement.PinType.PinValueType;
		const FString StaleTerminalObject = StaleTerminal.TerminalSubCategoryObject.IsValid()
			? StaleTerminal.TerminalSubCategoryObject->GetPathName() : FString();
		const FString ReplacementTerminalObject = ReplacementTerminal.TerminalSubCategoryObject.IsValid()
			? ReplacementTerminal.TerminalSubCategoryObject->GetPathName() : FString();
		if (StaleTerminal.TerminalCategory != ReplacementTerminal.TerminalCategory
			|| StaleTerminal.TerminalSubCategory != ReplacementTerminal.TerminalSubCategory
			|| StaleTerminalObject != ReplacementTerminalObject
			|| StaleTerminal.bTerminalIsConst != ReplacementTerminal.bTerminalIsConst)
		{
			OutDimension = TEXT("map_terminal");
			OutDetail = FString::Printf(TEXT("stale terminal '%s/%s/%s' vs replacement terminal '%s/%s/%s'"),
				*StaleTerminal.TerminalCategory.ToString(),
				*StaleTerminal.TerminalSubCategory.ToString(),
				StaleTerminalObject.IsEmpty() ? TEXT("<none>") : *StaleTerminalObject,
				*ReplacementTerminal.TerminalCategory.ToString(),
				*ReplacementTerminal.TerminalSubCategory.ToString(),
				ReplacementTerminalObject.IsEmpty() ? TEXT("<none>") : *ReplacementTerminalObject);
			return false;
		}
	}
	if (Stale.PinType.bIsReference != Replacement.PinType.bIsReference)
	{
		OutDimension = TEXT("reference");
		OutDetail = FString::Printf(TEXT("stale is_reference=%d vs replacement is_reference=%d"),
			Stale.PinType.bIsReference ? 1 : 0, Replacement.PinType.bIsReference ? 1 : 0);
		return false;
	}
	if (Stale.PinType.bIsConst != Replacement.PinType.bIsConst)
	{
		OutDimension = TEXT("const");
		OutDetail = FString::Printf(TEXT("stale is_const=%d vs replacement is_const=%d"),
			Stale.PinType.bIsConst ? 1 : 0, Replacement.PinType.bIsConst ? 1 : 0);
		return false;
	}
	return true;
}

/** Canonical pin signature of a live pin, through the one shared descriptor builder. */
FString PinSignature(const UEdGraphPin& Pin)
{
	return FCortexGraphPatchOps::CanonicalPinSignature(FCortexGraphPatchOps::MakePinSignatureDescriptor(Pin));
}

TArray<TSharedPtr<FJsonValue>> DescribePins(UEdGraphNode* Node)
{
	TArray<TSharedPtr<FJsonValue>> Descriptors;
	if (!Node) return Descriptors;
	TArray<UEdGraphPin*> Pins;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (IsMappablePin(Pin)) Pins.Add(Pin);
	}
	Pins.Sort([](const UEdGraphPin& A, const UEdGraphPin& B) { return A.PinName.LexicalLess(B.PinName); });
	for (const UEdGraphPin* Pin : Pins)
	{
		Descriptors.Add(MakeShared<FJsonValueObject>(FCortexGraphPatchOps::MakePinSignatureDescriptor(*Pin)));
	}
	return Descriptors;
}

UEdGraphPin* FindMappablePin(UEdGraphNode* Node, const FName PinName)
{
	if (!Node) return nullptr;
	UEdGraphPin* Pin = Node->FindPin(PinName);
	return IsMappablePin(Pin) ? Pin : nullptr;
}

/** True when the function declares an out parameter, so its graph owns a result terminator. */
bool DeclarationHasOutParameter(const UFunction* Function)
{
	if (!Function) return false;
	for (TFieldIterator<FProperty> It(Function); It && (It->PropertyFlags & CPF_Parm); ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_OutParm)) return true;
	}
	return false;
}

/** True when the compiled generated class declares the named function. */
bool DeclarationCompiledIntoClass(UBlueprint* Blueprint, const FName FunctionName)
{
	UClass* const GeneratedClass = Blueprint ? Blueprint->GeneratedClass : nullptr;
	if (!GeneratedClass || FunctionName.IsNone()) return false;
	for (TFieldIterator<UFunction> It(GeneratedClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (UFunction* Function = *It)
		{
			if (Function->GetFName() == FunctionName) return true;
		}
	}
	return false;
}

/** Canonical frozen JSON text of one value, using the shared serializer. */
FString FrozenJsonValue(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid()) return TEXT("null");
	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Value.ToSharedRef(), FString(), Writer);
	return Out;
}

/** One inventoried reference to a symbol the migration would invalidate. */
struct FInventoryReference
{
	FString Description;
	bool bExternal = false;
};

bool ReferenceOwnerMatchesAsset(UBlueprint* Target, const UClass* Owner)
{
	if (!Target || !Owner) return false;
	const UClass* const Generated = Target->GeneratedClass;
	const UClass* const Skeleton = Target->SkeletonGeneratedClass;
	return (Generated && (Owner == Generated || Owner->IsChildOf(Generated)))
		|| (Skeleton && (Owner == Skeleton || Owner->IsChildOf(Skeleton)));
}

/**
 * In-asset rule aligned with FBlueprintEditorUtils::RemoveVariableNodes(bForSelfOnly=true): a node
 * references a member this asset declares when its member parent class resolves to this asset's
 * generated class, which is exactly what the engine's self-only removal would destroy.
 */
bool NodeReferencesOwnMember(UBlueprint* Target, UK2Node_Variable* Variable, const FName MemberName, bool& bOutUnresolved)
{
	bOutUnresolved = false;
	if (!Variable || Variable->VariableReference.GetMemberName() != MemberName) return false;
	UClass* const Generated = Target->GeneratedClass;
	if (!Generated) return false;
	UClass* const Owner = Variable->VariableReference.GetMemberParentClass(Generated);
	if (!Owner)
	{
		// The name matches but no owner resolves: a stale reference, not absence.
		bOutUnresolved = true;
		return true;
	}
	return Owner == Generated;
}

/** Foreign asset that would keep referencing the removed member after the migration. */
bool NodeReferencesMemberExternally(
	UBlueprint* Target,
	UBlueprint* Other,
	UEdGraphNode* Node,
	const FName MemberName,
	bool& bOutUnresolved)
{
	bOutUnresolved = false;
	if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
	{
		if (Variable->VariableReference.GetMemberName() != MemberName) return false;
		if (Variable->VariableReference.IsSelfContext())
		{
			// Self context in another asset only resolves to this member when that asset derives
			// from the asset being changed, which a member removal would silently break.
			const UClass* const OtherClass = Other->SkeletonGeneratedClass
				? Other->SkeletonGeneratedClass : Other->GeneratedClass;
			if (!OtherClass) { bOutUnresolved = true; return true; }
			return ReferenceOwnerMatchesAsset(Target, OtherClass);
		}
		const UClass* const Owner = Variable->VariableReference.GetMemberParentClass();
		if (!Owner)
		{
			// A named reference with no resolvable owner can only be judged unsafe.
			bOutUnresolved = true;
			return true;
		}
		return ReferenceOwnerMatchesAsset(Target, Owner);
	}
	return false;
}

/**
 * Reference inventory of one app-declared member: every in-asset reference (which the member
 * removal destroys, so the apply detaches and journals it) and every reference from another loaded
 * package (which the removal cannot reach, so it blocks before any mutation).
 */
bool CollectMemberReferences(
	UBlueprint* Target,
	const FName MemberName,
	TArray<FString>& OutInAssetGuids,
	TArray<FInventoryReference>& OutExternalReferences,
	TArray<FString>& OutUnresolvedReferences,
	FCortexCommandResult& OutError)
{
	OutError = FCortexCommandResult();
	int32 ScannedNodes = 0;
	TArray<UEdGraph*> Graphs;
	Target->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (++ScannedNodes > MaxInventoryNodes)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::LimitExceeded,
					TEXT("replace_entry reference inventory exceeds the bounded node budget; reduce the asset before migrating"));
				return false;
			}
			bool bUnresolved = false;
			if (!NodeReferencesOwnMember(Target, Cast<UK2Node_Variable>(Node), MemberName, bUnresolved)) continue;
			if (bUnresolved)
			{
				OutUnresolvedReferences.Add(FString::Printf(
					TEXT("asset '%s' graph '%s' node '%s' (GUID %s) names member '%s' but its owner does not resolve"),
					*Target->GetName(), *Graph->GetName(), *Node->GetName(), *Node->NodeGuid.ToString(), *MemberName.ToString()));
				continue;
			}
			OutInAssetGuids.Add(Node->NodeGuid.ToString());
		}
	}

	int32 ScannedBlueprints = 0;
	for (TObjectIterator<UBlueprint> It; It; ++It)
	{
		UBlueprint* Other = *It;
		if (!Other || Other == Target || !IsValid(Other)) continue;
		if (++ScannedBlueprints > MaxInventoryBlueprints)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::LimitExceeded,
				TEXT("replace_entry reference inventory exceeds the bounded loaded-package budget; close unrelated assets before migrating"));
			return false;
		}
		TArray<UEdGraph*> OtherGraphs;
		Other->GetAllGraphs(OtherGraphs);
		for (UEdGraph* Graph : OtherGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;
				if (++ScannedNodes > MaxInventoryNodes)
				{
					OutError = FCortexCommandRouter::Error(CortexErrorCodes::LimitExceeded,
						TEXT("replace_entry reference inventory exceeds the bounded node budget; reduce the loaded package set before migrating"));
					return false;
				}
				bool bExternalUnresolved = false;
				if (!NodeReferencesMemberExternally(Target, Other, Node, MemberName, bExternalUnresolved)) continue;
				if (bExternalUnresolved)
				{
					OutUnresolvedReferences.Add(FString::Printf(
						TEXT("package '%s' asset '%s' graph '%s' node '%s' (GUID %s) names member '%s' but its owner does not resolve"),
						*Other->GetOutermost()->GetName(), *Other->GetName(), *Graph->GetName(), *Node->GetName(),
						*Node->NodeGuid.ToString(), *MemberName.ToString()));
					continue;
				}
				OutExternalReferences.Add({
					FString::Printf(TEXT("package '%s' asset '%s' graph '%s' node '%s' (GUID %s) outside this asset references '%s'"),
						*Other->GetOutermost()->GetName(), *Other->GetName(), *Graph->GetName(), *Node->GetName(),
						*Node->NodeGuid.ToString(), *MemberName.ToString()),
					true });
			}
		}
	}
	return true;
}

/** Second entry of the target declaration inside this asset: two implementations would be ambiguous. */
UEdGraphNode* FindCompetingDeclarationEntry(
	UBlueprint* Blueprint,
	UEdGraphNode* Replacement,
	UEdGraphNode* Source,
	const FName FunctionName,
	UClass* FunctionClass)
{
	const FGuid ReplacementGuid = Replacement ? Replacement->NodeGuid : FGuid();
	const FGuid SourceGuid = Source ? Source->NodeGuid : FGuid();
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || Node == Replacement || Node == Source) continue;
			if (ReplacementGuid.IsValid() && Node->NodeGuid == ReplacementGuid) continue;
			if (SourceGuid.IsValid() && Node->NodeGuid == SourceGuid) continue;
			if (const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
			{
				if (EventNode->EventReference.GetMemberName() == FunctionName
					&& EventNode->EventReference.GetMemberParentClass() == FunctionClass)
				{
					return Node;
				}
			}
			else if (const UK2Node_FunctionEntry* FunctionEntry = Cast<UK2Node_FunctionEntry>(Node))
			{
				if (FunctionEntry->FunctionReference.GetMemberName() == FunctionName
					&& FunctionEntry->FunctionReference.GetMemberParentClass() == FunctionClass)
				{
					return Node;
				}
			}
		}
	}
	return nullptr;
}
/** Durable identity of one replacement terminator for a given patch id. */
FGuid DeterministicGuidFromPlan(const FCortexGraphMigrationIdentity& Identity, const bool bResult)
{
	return bResult ? Identity.ResultGuid : Identity.EntryGuid;
}
}

TSharedPtr<FJsonObject> FCortexGraphMigrationPlan::ToJson() const
{
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("op"), Op);
	Out->SetStringField(TEXT("graph_guid"), GraphGuid);
	Out->SetStringField(TEXT("subgraph_path"), SubgraphPath);
	Out->SetStringField(TEXT("source_entry_guid"), SourceEntryGuid);
	Out->SetStringField(TEXT("source_result_guid"), SourceResultGuid);
	Out->SetStringField(TEXT("entry_node_guid"), Identity.EntryGuid.ToString());
	Out->SetStringField(TEXT("result_node_guid"), Identity.ResultGuid.ToString());
	Out->SetBoolField(TEXT("is_event"), bIsEvent);
	Out->SetBoolField(TEXT("has_result"), bHasResultTerminator);
	Out->SetStringField(TEXT("function_name"), FunctionName);
	Out->SetStringField(TEXT("owner_class"), OwnerClassPath);
	Out->SetArrayField(TEXT("entry_pins"), EntryPins);
	Out->SetArrayField(TEXT("result_pins"), ResultPins);

	TArray<TSharedPtr<FJsonValue>> Mappings;
	for (const FCortexGraphMigrationPinMapping& Mapping : PinMap)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("entry"), Mapping.Entry);
		Entry->SetStringField(TEXT("role"), Mapping.TargetRole);
		Entry->SetStringField(TEXT("from_pin"), Mapping.FromPin);
		Entry->SetStringField(TEXT("to_pin"), Mapping.ToPin);
		Entry->SetNumberField(TEXT("from_direction"), static_cast<int32>(Mapping.FromDirection));
		Mappings.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Out->SetArrayField(TEXT("pin_map"), Mappings);

	TArray<TSharedPtr<FJsonValue>> EdgeValues;
	for (const FCortexGraphMigrationEdge& Edge : Edges)
	{
		TSharedPtr<FJsonObject> EdgeJson = MakeShared<FJsonObject>();
		EdgeJson->SetStringField(TEXT("replacement_role"), Edge.ReplacementRole);
		EdgeJson->SetStringField(TEXT("replacement_pin"), Edge.ReplacementPin);
		EdgeJson->SetStringField(TEXT("far_role"), Edge.FarRole);
		EdgeJson->SetStringField(TEXT("far_guid"), Edge.FarGuid);
		EdgeJson->SetStringField(TEXT("far_pin"), Edge.FarPin);
		EdgeValues.Add(MakeShared<FJsonValueObject>(EdgeJson));
	}
	Out->SetArrayField(TEXT("edges"), EdgeValues);

	TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
	Position->SetNumberField(TEXT("x"), NodePosX);
	Position->SetNumberField(TEXT("y"), NodePosY);
	Out->SetObjectField(TEXT("position"), Position);
	Out->SetStringField(TEXT("comment"), NodeComment);
	Out->SetBoolField(TEXT("bubble_pinned"), bCommentBubblePinned);
	Out->SetBoolField(TEXT("bubble_visible"), bCommentBubbleVisible);
	Out->SetNumberField(TEXT("enabled_state"), EnabledState);
	Out->SetBoolField(TEXT("user_set_enabled_state"), bUserSetEnabledState);
	Out->SetBoolField(TEXT("force_display_disabled"), bForceDisplayAsDisabled);
	Out->SetBoolField(TEXT("remove_member"), bRemoveMember);
	Out->SetStringField(TEXT("member_name"), MemberName);
	Out->SetStringField(TEXT("member_kind"), MemberKind);
	Out->SetStringField(TEXT("member_node_guid"), MemberNodeGuid);
	TArray<TSharedPtr<FJsonValue>> MemberReferences;
	for (const FString& Guid : MemberReferenceNodeGuids)
	{
		MemberReferences.Add(MakeShared<FJsonValueString>(Guid));
	}
	Out->SetArrayField(TEXT("member_references"), MemberReferences);
	Out->SetStringField(TEXT("preservation_capture"), PreservationCapture);
	Out->SetNumberField(TEXT("declaration_references"), DeclarationReferences);
	Out->SetNumberField(TEXT("external_declaration_references"), ExternalDeclarationReferences);
	Out->SetBoolField(TEXT("replayed_with_absent_source"), bReplayedWithAbsentSource);
	Out->SetObjectField(TEXT("normalized_node"), NormalizedNode.IsValid() ? NormalizedNode : MakeShared<FJsonObject>());
	Out->SetObjectField(TEXT("resolved_symbol"), ResolvedSymbol.IsValid() ? ResolvedSymbol : MakeShared<FJsonObject>());
	return Out;
}

bool FCortexGraphMigrationPlan::FromJson(
	const TSharedPtr<FJsonObject>& Source,
	FCortexGraphMigrationPlan& OutPlan,
	FCortexCommandResult& OutError)
{
	OutPlan = FCortexGraphMigrationPlan();
	OutError = FCortexCommandResult();
	if (!Source.IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("prepared migration plan is missing"));
		return false;
	}
	const bool bRead = Source->TryGetStringField(TEXT("op"), OutPlan.Op)
		&& Source->TryGetStringField(TEXT("graph_guid"), OutPlan.GraphGuid)
		&& Source->TryGetStringField(TEXT("source_entry_guid"), OutPlan.SourceEntryGuid)
		&& Source->TryGetBoolField(TEXT("is_event"), OutPlan.bIsEvent)
		&& Source->TryGetBoolField(TEXT("has_result"), OutPlan.bHasResultTerminator)
		&& Source->TryGetBoolField(TEXT("remove_member"), OutPlan.bRemoveMember);
	if (!bRead)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("prepared migration plan is incomplete"));
		return false;
	}
	Source->TryGetStringField(TEXT("subgraph_path"), OutPlan.SubgraphPath);
	Source->TryGetStringField(TEXT("source_result_guid"), OutPlan.SourceResultGuid);
	Source->TryGetStringField(TEXT("function_name"), OutPlan.FunctionName);
	Source->TryGetStringField(TEXT("owner_class"), OutPlan.OwnerClassPath);
	Source->TryGetStringField(TEXT("comment"), OutPlan.NodeComment);
	Source->TryGetStringField(TEXT("member_name"), OutPlan.MemberName);
	Source->TryGetStringField(TEXT("member_kind"), OutPlan.MemberKind);
	Source->TryGetStringField(TEXT("member_node_guid"), OutPlan.MemberNodeGuid);
	Source->TryGetStringField(TEXT("preservation_capture"), OutPlan.PreservationCapture);
	int32 DeclarationReferences = 0;
	if (Source->TryGetNumberField(TEXT("declaration_references"), DeclarationReferences)) OutPlan.DeclarationReferences = DeclarationReferences;
	int32 ExternalDeclarationReferences = 0;
	if (Source->TryGetNumberField(TEXT("external_declaration_references"), ExternalDeclarationReferences)) OutPlan.ExternalDeclarationReferences = ExternalDeclarationReferences;
	Source->TryGetBoolField(TEXT("replayed_with_absent_source"), OutPlan.bReplayedWithAbsentSource);
	Source->TryGetBoolField(TEXT("bubble_pinned"), OutPlan.bCommentBubblePinned);
	Source->TryGetBoolField(TEXT("bubble_visible"), OutPlan.bCommentBubbleVisible);
	Source->TryGetBoolField(TEXT("user_set_enabled_state"), OutPlan.bUserSetEnabledState);
	Source->TryGetBoolField(TEXT("force_display_disabled"), OutPlan.bForceDisplayAsDisabled);
	int32 EnabledState = 0;
	if (Source->TryGetNumberField(TEXT("enabled_state"), EnabledState)) OutPlan.EnabledState = EnabledState;
	const TSharedPtr<FJsonObject>* PositionPtr = nullptr;
	if (Source->TryGetObjectField(TEXT("position"), PositionPtr) && PositionPtr && PositionPtr->IsValid())
	{
		int32 X = 0;
		int32 Y = 0;
		if ((*PositionPtr)->TryGetNumberField(TEXT("x"), X)) OutPlan.NodePosX = X;
		if ((*PositionPtr)->TryGetNumberField(TEXT("y"), Y)) OutPlan.NodePosY = Y;
	}
	FString EntryGuidText;
	FString ResultGuidText;
	Source->TryGetStringField(TEXT("entry_node_guid"), EntryGuidText);
	Source->TryGetStringField(TEXT("result_node_guid"), ResultGuidText);
	FGuid::Parse(EntryGuidText, OutPlan.Identity.EntryGuid);
	FGuid::Parse(ResultGuidText, OutPlan.Identity.ResultGuid);

	const TArray<TSharedPtr<FJsonValue>>* EntryPins = nullptr;
	if (Source->TryGetArrayField(TEXT("entry_pins"), EntryPins) && EntryPins) OutPlan.EntryPins = *EntryPins;
	const TArray<TSharedPtr<FJsonValue>>* ResultPins = nullptr;
	if (Source->TryGetArrayField(TEXT("result_pins"), ResultPins) && ResultPins) OutPlan.ResultPins = *ResultPins;
	const TArray<TSharedPtr<FJsonValue>>* Mappings = nullptr;
	if (Source->TryGetArrayField(TEXT("pin_map"), Mappings) && Mappings)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Mappings)
		{
			const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Entry.IsValid()) continue;
			FCortexGraphMigrationPinMapping Mapping;
			Entry->TryGetStringField(TEXT("entry"), Mapping.Entry);
			Entry->TryGetStringField(TEXT("role"), Mapping.TargetRole);
			Entry->TryGetStringField(TEXT("from_pin"), Mapping.FromPin);
			Entry->TryGetStringField(TEXT("to_pin"), Mapping.ToPin);
			int32 FromDirection = 0;
			if (Entry->TryGetNumberField(TEXT("from_direction"), FromDirection))
			{
				Mapping.FromDirection = FromDirection;
			}
			OutPlan.PinMap.Add(MoveTemp(Mapping));
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
	if (Source->TryGetArrayField(TEXT("edges"), Edges) && Edges)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Edges)
		{
			const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Entry.IsValid()) continue;
			FCortexGraphMigrationEdge Edge;
			Entry->TryGetStringField(TEXT("replacement_role"), Edge.ReplacementRole);
			Entry->TryGetStringField(TEXT("replacement_pin"), Edge.ReplacementPin);
			Entry->TryGetStringField(TEXT("far_role"), Edge.FarRole);
			Entry->TryGetStringField(TEXT("far_guid"), Edge.FarGuid);
			Entry->TryGetStringField(TEXT("far_pin"), Edge.FarPin);
			OutPlan.Edges.Add(MoveTemp(Edge));
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* MemberReferences = nullptr;
	if (Source->TryGetArrayField(TEXT("member_references"), MemberReferences) && MemberReferences)
	{
		for (const TSharedPtr<FJsonValue>& Value : *MemberReferences)
		{
			FString Guid;
			if (Value.IsValid() && Value->TryGetString(Guid) && !Guid.IsEmpty())
			{
				OutPlan.MemberReferenceNodeGuids.Add(Guid);
			}
		}
	}
	const TSharedPtr<FJsonObject>* NormalizedNode = nullptr;
	if (Source->TryGetObjectField(TEXT("normalized_node"), NormalizedNode) && NormalizedNode && NormalizedNode->IsValid())
	{
		OutPlan.NormalizedNode = *NormalizedNode;
	}
	const TSharedPtr<FJsonObject>* ResolvedSymbol = nullptr;
	if (Source->TryGetObjectField(TEXT("resolved_symbol"), ResolvedSymbol) && ResolvedSymbol && ResolvedSymbol->IsValid())
	{
		OutPlan.ResolvedSymbol = *ResolvedSymbol;
	}
	if (OutPlan.Op.IsEmpty() || OutPlan.GraphGuid.IsEmpty() || !OutPlan.Identity.EntryGuid.IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("prepared migration plan does not identify its graph or replacement identity"));
		return false;
	}
	return true;
}

namespace
{
/**
 * Allocates the replacement terminators through exactly one engine path, so planning (transient)
 * and apply (live graph) derive the same pin set from the same calls.
 */
bool BuildTerminatorPair(
	UEdGraph* Graph,
	UFunction* Function,
	UClass* FunctionClass,
	const bool bEvent,
	const bool bResult,
	const bool bTransient,
	UEdGraphNode*& OutEntry,
	UEdGraphNode*& OutResult)
{
	OutEntry = nullptr;
	OutResult = nullptr;
	if (!Graph || !Function || !FunctionClass) return false;
	const EObjectFlags Flags = bTransient ? RF_Transient : RF_Transactional;

	if (bEvent)
	{
		UK2Node_Event* Event = NewObject<UK2Node_Event>(Graph, NAME_None, Flags);
		Event->EventReference.SetExternalMember(Function->GetFName(), FunctionClass);
		Event->bOverrideFunction = true;
		Event->AllocateDefaultPins();
		Graph->AddNode(Event, false, false);
		OutEntry = Event;
		return true;
	}

	UK2Node_FunctionEntry* Entry = NewObject<UK2Node_FunctionEntry>(Graph, NAME_None, Flags);
	Entry->FunctionReference.SetExternalMember(Function->GetFName(), FunctionClass);
	Entry->AddExtraFlags(Function->FunctionFlags & FUNC_AccessSpecifiers);
	Entry->AllocateDefaultPins();
	Graph->AddNode(Entry, false, false);
	OutEntry = Entry;

	if (bResult)
	{
		UK2Node_FunctionResult* Result = NewObject<UK2Node_FunctionResult>(Graph, NAME_None, Flags);
		Result->FunctionReference = Entry->FunctionReference;
		Result->AllocateDefaultPins();
		Graph->AddNode(Result, false, false);
		OutResult = Result;
	}
	return true;
}

/** Transient terminators that model the replacement pin set without touching the asset. */
struct FTransientTerminators
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	UEdGraphNode* Entry = nullptr;
	UEdGraphNode* Result = nullptr;
};

bool BuildTransientTerminators(
	UBlueprint* LiveBlueprint,
	UFunction* Function,
	UClass* FunctionClass,
	const bool bEvent,
	const bool bResult,
	FTransientTerminators& Out)
{
	UBlueprint* Planning = NewObject<UBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
	Planning->ParentClass = LiveBlueprint->ParentClass;
	Planning->GeneratedClass = LiveBlueprint->GeneratedClass;
	Planning->SkeletonGeneratedClass = LiveBlueprint->SkeletonGeneratedClass;
	UEdGraph* Graph = NewObject<UEdGraph>(Planning, NAME_None, RF_Transient);
	Graph->Schema = UEdGraphSchema_K2::StaticClass();
	if (bEvent)
	{
		Planning->UbergraphPages.Add(Graph);
	}
	else
	{
		Planning->FunctionGraphs.Add(Graph);
	}
	if (!BuildTerminatorPair(Graph, Function, FunctionClass, bEvent, bResult, true, Out.Entry, Out.Result))
	{
		return false;
	}
	Out.Blueprint = Planning;
	Out.Graph = Graph;
	return true;
}

/** Adds one canonical link description, de-duplicated by its canonical endpoint pair. */
void AddCanonicalEdge(TArray<FCortexGraphMigrationEdge>& InOutEdges, TSet<FString>& InOutKeys, FCortexGraphMigrationEdge Edge)
{
	const FString KeyA = EndpointKey(Edge.ReplacementRole, FString(), FName(*Edge.ReplacementPin));
	const FString KeyB = EndpointKey(Edge.FarRole, Edge.FarGuid, FName(*Edge.FarPin));
	const FString Key = KeyA < KeyB ? KeyA + TEXT(">") + KeyB : KeyB + TEXT(">") + KeyA;
	if (InOutKeys.Contains(Key)) return;
	InOutKeys.Add(Key);
	InOutEdges.Add(MoveTemp(Edge));
}
}

namespace
{
/** Reference inventory of the replaced declaration itself: call sites and delegate bindings. */
struct FDeclarationInventory
{
	int32 Resolved = 0;
	int32 External = 0;
	TArray<FString> Unresolved;
};

/** Concrete owner of a member reference, or null when the reference cannot be resolved. */
UClass* ResolveReferenceOwner(UBlueprint* Asset, const FMemberReference& Reference)
{
	if (Reference.IsSelfContext())
	{
		return Asset && Asset->SkeletonGeneratedClass ? Asset->SkeletonGeneratedClass.Get() : (Asset ? Asset->GeneratedClass.Get() : nullptr);
	}
	return Reference.GetMemberParentClass();
}

/**
 * One declaration owner behind a class pair. A Blueprint owns both a generated class and a skeleton
 * class, and both declare the Blueprint's own functions, but the two pointers are distinct objects:
 * the explicit target selector resolves a declaration through the generated class
 * (`FCortexGraphSymbolResolver::ResolveClass` returns `UBlueprint::GeneratedClass`, and the
 * declaration's owner is that class), while a delegate scope resolves through the skeleton class
 * (`UK2Node_CreateDelegate::GetScopeClass` returns `SkeletonGeneratedClass` when the scope pin is
 * typed as a Blueprint-generated class). Identity is therefore normalized in both directions before
 * two owners are compared, so the same declaration is never treated as two different owners; a class
 * without a generating Blueprint is returned unchanged.
 */
UClass* CanonicalDeclarationOwner(UClass* OwnerClass)
{
	if (const UBlueprint* const Blueprint = OwnerClass ? Cast<UBlueprint>(OwnerClass->ClassGeneratedBy) : nullptr)
	{
		if (UClass* const GeneratedClass = Blueprint->GeneratedClass)
		{
			return GeneratedClass;
		}
	}
	return OwnerClass;
}

/**
 * True when a resolved owner really declares the selected declaration. Name equality alone is not
 * candidate identity: an unrelated same-named call site must neither reject a valid migration nor
 * inflate the reported inventory. Both inventory paths (call sites and delegate bindings) compare
 * their resolved owner through this one predicate, so the generated/skeleton normalization above
 * applies to both.
 */
bool OwnerDeclaresSelected(UClass* Owner, const FName DeclarationName, UClass* DeclaringClass)
{
	if (!Owner) return false;
	UFunction* const Found = Owner->FindFunctionByName(DeclarationName);
	if (!Found) return false;
	return DeclaringClass
		? CanonicalDeclarationOwner(Found->GetOwnerClass()) == CanonicalDeclarationOwner(DeclaringClass)
		: true;
}

void ScanDeclarationReferences(
	UBlueprint* Asset,
	const FName DeclarationName,
	UClass* DeclaringClass,
	const bool bCountAsExternal,
	int32& InOutScannedNodes,
	FDeclarationInventory& InOut,
	FCortexCommandResult& OutError)
{
	if (!Asset || !OutError.ErrorCode.IsEmpty()) return;
	TArray<UEdGraph*> Graphs;
	Asset->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			// The same bounded node budget the member inventory enforces: exhaustion refuses, it
			// never truncates the inventory silently.
			if (++InOutScannedNodes > MaxInventoryNodes)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::LimitExceeded,
					TEXT("replace_entry reference inventory exceeds the bounded node budget; reduce the asset or the loaded package set before migrating"));
				return;
			}
			const FMemberReference* Reference = nullptr;
			if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
			{
				Reference = &Call->FunctionReference;
			}
			else if (const UK2Node_CreateDelegate* CreateDelegate = Cast<UK2Node_CreateDelegate>(Node))
			{
				if (CreateDelegate->GetFunctionName() != DeclarationName) continue;
				// A delegate resolves its binding through its own scope, never through the class of
				// the asset that happens to contain it: the engine reads the scope from the node's
				// self/object pin (`UK2Node_CreateDelegate::GetScopeClass`,
				// Engine/Source/Editor/BlueprintGraph/Private/K2Node_CreateDelegate.cpp:364, declared
				// at Classes/K2Node_CreateDelegate.h:66), falling back to the containing blueprint's
				// class only when that pin is unconnected or is itself a self pin — which is exactly
				// what GetScopeClass does. Three outcomes: the binding resolves to the selected
				// declaration (counted), it resolves to a different function with the same name (not
				// a reference at all, so ignored), or the scope or the function cannot be resolved
				// (recorded as unresolved and blocking).
				UClass* const ScopeClass = CreateDelegate->GetScopeClass();
				if (!ScopeClass)
				{
					InOut.Unresolved.Add(FString::Printf(
						TEXT("asset '%s' graph '%s' node '%s' (GUID %s) binds '%s' but its target object pin does not resolve to a scope class"),
						*Asset->GetName(), *Graph->GetName(), *Node->GetName(), *Node->NodeGuid.ToString(), *DeclarationName.ToString()));
					continue;
				}
				UFunction* const Bound = ScopeClass->FindFunctionByName(DeclarationName);
				if (!Bound)
				{
					InOut.Unresolved.Add(FString::Printf(
						TEXT("asset '%s' graph '%s' node '%s' (GUID %s) binds '%s' but no such function resolves in its scope '%s'"),
						*Asset->GetName(), *Graph->GetName(), *Node->GetName(), *Node->NodeGuid.ToString(),
						*DeclarationName.ToString(), *ScopeClass->GetPathName()));
					continue;
				}
				if (!OwnerDeclaresSelected(Bound->GetOwnerClass(), DeclarationName, DeclaringClass))
				{
					// A fully resolved binding to an unrelated same-named function is not a reference
					// to the selected declaration: it neither blocks nor inflates the inventory.
					continue;
				}
				if (bCountAsExternal) ++InOut.External; else ++InOut.Resolved;
				continue;
			}
			else
			{
				continue;
			}
			if (Reference->GetMemberName() != DeclarationName) continue;
			UClass* const Owner = ResolveReferenceOwner(Asset, *Reference);
			if (!Owner)
			{
				// A reference that names the declaration but resolves to no owner is not absence: it
				// is exactly the stale call site that must block.
				InOut.Unresolved.Add(FString::Printf(
					TEXT("asset '%s' graph '%s' node '%s' (GUID %s) names '%s' but its owner does not resolve"),
					*Asset->GetName(), *Graph->GetName(), *Node->GetName(), *Node->NodeGuid.ToString(), *DeclarationName.ToString()));
				continue;
			}
			if (!OwnerDeclaresSelected(Owner, DeclarationName, DeclaringClass)) continue;
			if (bCountAsExternal) ++InOut.External; else ++InOut.Resolved;
		}
	}
}

bool CollectDeclarationReferences(
	UBlueprint* Target,
	const FName DeclarationName,
	UClass* DeclaringClass,
	FDeclarationInventory& OutInventory,
	FCortexCommandResult& OutError)
{
	int32 ScannedNodes = 0;
	ScanDeclarationReferences(Target, DeclarationName, DeclaringClass, false, ScannedNodes, OutInventory, OutError);
	if (!OutError.ErrorCode.IsEmpty()) return false;
	int32 ScannedBlueprints = 0;
	for (TObjectIterator<UBlueprint> It; It; ++It)
	{
		UBlueprint* Other = *It;
		if (!Other || Other == Target || !IsValid(Other)) continue;
		if (++ScannedBlueprints > MaxInventoryBlueprints)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::LimitExceeded,
				TEXT("replace_entry declaration inventory exceeds the bounded loaded-package budget; close unrelated assets before migrating"));
			return false;
		}
		ScanDeclarationReferences(Other, DeclarationName, DeclaringClass, true, ScannedNodes, OutInventory, OutError);
		if (!OutError.ErrorCode.IsEmpty()) return false;
	}
	return true;
}
}

bool FCortexGraphMigrationOps::Plan(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Migration,
	const TSharedPtr<FJsonObject>& TargetSelector,
	const FCortexGraphMigrationIdentity& Identity,
	FCortexGraphMigrationPlan& OutPlan,
	bool& bOutReused,
	FCortexCommandResult& OutError)
{
	OutPlan = FCortexGraphMigrationPlan();
	OutError = FCortexCommandResult();
	bOutReused = false;
	if (!Blueprint)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, TEXT("Blueprint is null"));
		return false;
	}
	if (!Migration.IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("migration must be an object"));
		return false;
	}
	if (!FCortexGraphPatchOps::HasOnlyFields(Migration,
		{ TEXT("op"), TEXT("source"), TEXT("pin_map"), TEXT("remove_shadowing_member") }, OutError, TEXT("migration")))
	{
		return false;
	}
	FString Op;
	if (!FCortexGraphPatchOps::ReadRequiredString(Migration, TEXT("op"), Op, OutError)) return false;
	if (Op != TEXT("replace_entry"))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::UnsupportedOperation,
			FString::Printf(TEXT("Unsupported migration operation '%s'; the only published migration operation is replace_entry"), *Op));
		return false;
	}
	bool bRemoveMember = false;
	if (!FCortexGraphPatchOps::ReadStrictBool(Migration, TEXT("remove_shadowing_member"), false, bRemoveMember, OutError)) return false;
	const TArray<TSharedPtr<FJsonValue>>* PinMapValues = nullptr;
	if (!Migration->TryGetArrayField(TEXT("pin_map"), PinMapValues) || !PinMapValues)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("migration.pin_map must be an array"));
		return false;
	}
	if (PinMapValues->Num() > MaxMigrationPins)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::LimitExceeded,
			TEXT("migration.pin_map exceeds the bounded pin limit"));
		return false;
	}

	// Source graph and stale entry.
	const TSharedPtr<FJsonObject>* SourcePtr = nullptr;
	if (!Migration->TryGetObjectField(TEXT("source"), SourcePtr) || !SourcePtr || !SourcePtr->IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("migration.source must be an object"));
		return false;
	}
	const TSharedPtr<FJsonObject>& Source = *SourcePtr;
	if (!FCortexGraphPatchOps::HasOnlyFields(Source, { TEXT("graph_ref"), TEXT("entry_node_guid") }, OutError, TEXT("migration.source")))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* GraphRefPtr = nullptr;
	if (!Source->TryGetObjectField(TEXT("graph_ref"), GraphRefPtr) || !GraphRefPtr || !GraphRefPtr->IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("migration.source.graph_ref must be an object"));
		return false;
	}
	const TSharedPtr<FJsonObject>& GraphRef = *GraphRefPtr;
	if (!FCortexGraphPatchOps::HasOnlyFields(GraphRef,
		{ TEXT("graph_guid"), TEXT("graph_kind"), TEXT("subgraph_path") }, OutError, TEXT("migration.source.graph_ref")))
	{
		return false;
	}
	FGuid GraphGuid;
	if (!FCortexGraphPatchOps::ParseGuidField(GraphRef, TEXT("graph_guid"), GraphGuid, OutError)) return false;
	FString SubgraphPath;
	if (GraphRef->HasField(TEXT("subgraph_path")) && !GraphRef->TryGetStringField(TEXT("subgraph_path"), SubgraphPath))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("migration.source.graph_ref.subgraph_path must be a string"));
		return false;
	}
	UEdGraph* SourceGraph = nullptr;
	if (!FCortexGraphPatchOps::ResolveGraphByGuid(Blueprint, GraphGuid, SubgraphPath, SourceGraph, OutError)) return false;
	if (GraphRef->HasField(TEXT("graph_kind")))
	{
		FString RequestedKind;
		if (!GraphRef->TryGetStringField(TEXT("graph_kind"), RequestedKind))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("migration.source.graph_ref.graph_kind must be a string"));
			return false;
		}
		FString ResolvedKind;
		if (!FCortexGraphPatchOps::ResolveGraphKindByGuid(Blueprint, GraphGuid, ResolvedKind, OutError)) return false;
		if (ResolvedKind != RequestedKind)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				TEXT("migration source graph_kind conflicts with graph identity"));
			return false;
		}
	}
	FGuid SourceEntryGuid;
	if (!FCortexGraphPatchOps::ParseGuidField(Source, TEXT("entry_node_guid"), SourceEntryGuid, OutError)) return false;

	// Target declaration: the existing implementation selector, resolved by the shared primitive.
	if (!TargetSelector.IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			TEXT("migration requires target.implementation declaring the inherited declaration the entry must become"));
		return false;
	}
	// The declaration name is resolved first so the shadowing-member policy can run before the
	// implementation primitive, which reports a same-named custom event as a generic conflict.
	FName DeclarationName = NAME_None;
	UClass* DeclaredSymbolDeclaringClass = nullptr;
	{
		FCortexResolvedSymbol DeclaredSymbol;
		FCortexCommandResult DeclaredError;
		if (!FCortexGraphSymbolResolver::ResolveFunction(Blueprint, TargetSelector, DeclaredSymbol, DeclaredError)
			|| !DeclaredSymbol.Function)
		{
			OutError = DeclaredError.ErrorCode.IsEmpty()
				? FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("the migration target declaration does not resolve"))
				: DeclaredError;
			return false;
		}
		DeclarationName = DeclaredSymbol.Function->GetFName();
		DeclaredSymbolDeclaringClass = DeclaredSymbol.DeclaringClass;
	}
	const FString DeclaredFunctionName = DeclarationName.ToString();

	// Inventory the replaced declaration's own references: resolved call sites are reported, and a
	// reference that cannot be resolved to a concrete owner blocks instead of looking like absence.
	{
		FDeclarationInventory Inventory;
		if (!CollectDeclarationReferences(Blueprint, DeclarationName, DeclaredSymbolDeclaringClass, Inventory, OutError)) return false;
		if (Inventory.Unresolved.Num() > 0)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("the replaced declaration '%s' has %d reference(s) that cannot be resolved to a concrete owner: %s. No automatic project repair is attempted."),
					*DeclarationName.ToString(), Inventory.Unresolved.Num(), *FString::Join(Inventory.Unresolved, TEXT("; "))));
			return false;
		}
		OutPlan.DeclarationReferences = Inventory.Resolved;
		OutPlan.ExternalDeclarationReferences = Inventory.External;
	}

	// Shadowing-member policy and its reference inventory.
	FString MemberName;
	FString MemberKind;
	{
		const FName DeclName = DeclarationName;
		if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, DeclName) != INDEX_NONE)
		{
			MemberName = DeclName.ToString();
			MemberKind = TEXT("variable");
		}
		else
		{
			for (UEdGraph* Graph : Blueprint->UbergraphPages)
			{
				if (!Graph) continue;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (const UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
					{
						if (CustomEvent->CustomFunctionName == DeclName)
						{
							MemberName = DeclName.ToString();
							MemberKind = TEXT("custom_event");
							break;
						}
					}
				}
				if (!MemberName.IsEmpty()) break;
			}
			if (MemberName.IsEmpty())
			{
				for (UEdGraph* Graph : Blueprint->FunctionGraphs)
				{
					if (Graph && Graph != SourceGraph && Graph->GetFName() == DeclName)
					{
						MemberName = DeclName.ToString();
						MemberKind = TEXT("function_graph");
						break;
					}
				}
			}
		}
	}
	if (!MemberName.IsEmpty())
	{
		TArray<FString> InAssetReferences;
		TArray<FInventoryReference> ExternalReferences;
		TArray<FString> UnresolvedMemberReferences;
		if (!CollectMemberReferences(Blueprint, FName(*MemberName), InAssetReferences, ExternalReferences, UnresolvedMemberReferences, OutError)) return false;
		if (UnresolvedMemberReferences.Num() > 0)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("the shadowing member '%s' has %d reference(s) whose owner cannot be resolved: %s. No automatic project repair is attempted."),
					*MemberName, UnresolvedMemberReferences.Num(), *FString::Join(UnresolvedMemberReferences, TEXT("; "))));
			return false;
		}
		if (ExternalReferences.Num() > 0)
		{
			TArray<FString> Reasons;
			for (const FInventoryReference& Reference : ExternalReferences)
			{
				Reasons.Add(Reference.Description);
			}
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("the shadowing member '%s' is referenced outside this asset, so remove_shadowing_member cannot be proven safe: %s. No automatic project repair is attempted."),
					*MemberName, *FString::Join(Reasons, TEXT("; "))));
			return false;
		}
		if (!bRemoveMember)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("the target declaration '%s' is shadowed by a %s of the same name in this asset; remove_shadowing_member=false, so the member was not removed and the replacement was refused. Set remove_shadowing_member=true to remove the %s together with its %d resolved in-asset reference(s)."),
					*DeclaredFunctionName, *MemberKind, *MemberKind, InAssetReferences.Num()));
			return false;
		}
		if (MemberKind != TEXT("variable"))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("the shadowing member '%s' is a %s; replace_entry can remove a shadowing Blueprint variable, but never a %s"),
					*MemberName, *MemberKind, *MemberKind));
			return false;
		}
		InAssetReferences.Sort();
		OutPlan.bRemoveMember = true;
		OutPlan.MemberName = MemberName;
		OutPlan.MemberKind = MemberKind;
		OutPlan.MemberReferenceNodeGuids = InAssetReferences;
	}

	// Target declaration eligibility, resolved by the shared implementation primitive.
	FCortexGraphImplementationPlan ImplPlan;
	if (!FCortexGraphImplementationOps::ValidateEligibility(Blueprint, TargetSelector, ImplPlan, OutError)) return false;
	if (ImplPlan.bParentCall)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			TEXT("replace_entry preserves the body and never authors a parent call; a parent call the body already contains is preserved and re-wired by pin_map"));
		return false;
	}
	UFunction* const Function = ImplPlan.Function;
	UClass* const FunctionClass = ImplPlan.FunctionClass;
	const bool bIsEvent = ImplPlan.bCanBePlacedAsEvent;
	const bool bHasResult = !bIsEvent && DeclarationHasOutParameter(Function);
	const FString FunctionName = Function->GetFName().ToString();

	// The replacement must land in the graph that already implements the declaration.
	if (bIsEvent)
	{
		if (!Blueprint->UbergraphPages.Contains(SourceGraph))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("migration source graph '%s' is not an ubergraph page, so it cannot own an event implementation entry"),
					*SourceGraph->GetName()));
			return false;
		}
	}
	else
	{
		if (!Blueprint->FunctionGraphs.Contains(SourceGraph) || SourceGraph->GetFName() != Function->GetFName())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("migration source graph '%s' is not the function graph of the target declaration '%s'"),
					*SourceGraph->GetName(), *FunctionName));
			return false;
		}
		if (ImplPlan.ExistingGraph && ImplPlan.ExistingGraph != SourceGraph)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				TEXT("the target declaration is implemented by a different function graph than the migration source"));
			return false;
		}
	}

	// Deterministic identity reconciliation: T09 semantics reuse a complete match and refuse a
	// partial or conflicting identity set.
	UEdGraphNode* ReplacementEntry = FindNodeByGuid(Blueprint, Identity.EntryGuid);
	if (ReplacementEntry && ReplacementEntry->GetGraph() != SourceGraph)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("the deterministic replacement identity %s already exists in graph '%s' instead of the migration source graph"),
				*Identity.EntryGuid.ToString(), ReplacementEntry->GetGraph() ? *ReplacementEntry->GetGraph()->GetName() : TEXT("<none>")));
		return false;
	}
	const bool bReused = ReplacementEntry != nullptr;
	// The requested locator is always resolved, so an unchanged replay cannot silently accept a
	// request whose source entry is a different, still-present stale node.
	UEdGraphNode* const RequestedSource = FindNodeByGuid(Blueprint, SourceEntryGuid);
	if (bReused && RequestedSource == ReplacementEntry)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("the requested migration source locator %s is the deterministic replacement identity itself; source.entry_node_guid names the stale entry that is being replaced, not the replacement"),
				*Identity.EntryGuid.ToString()));
		return false;
	}
	if (bReused && !RequestedSource)
	{
		// The apply consumed the stale entry, so an accepted replay names a locator that no longer
		// exists. That is reported, never inferred as some other provenance.
		OutPlan.bReplayedWithAbsentSource = true;
	}
	if (bReused && RequestedSource && RequestedSource != ReplacementEntry)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("patch identity set is partial: the deterministic replacement identity %s already exists while the requested source entry %s is still present; a replace_entry must be entirely new or a complete replay"),
				*Identity.EntryGuid.ToString(), *RequestedSource->NodeGuid.ToString()));
		return false;
	}
	UEdGraphNode* SourceEntry = bReused ? nullptr : RequestedSource;
	if (bReused && SourceEntry)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("patch identity set is partial: the stale entry %s is still present while the deterministic replacement identity %s already exists; a replace_entry must be entirely new or a complete replay"),
				*SourceEntryGuid.ToString(), *Identity.EntryGuid.ToString()));
		return false;
	}
	if (SourceEntry)
	{
		if (SourceEntry->GetGraph() != SourceGraph)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("migration source entry %s does not belong to the named source graph"), *SourceEntryGuid.ToString()));
			return false;
		}
		// The source entry must be the entry/terminator *of the replaced declaration*: any other
		// entry node (including a user custom event, which derives from the event node class) would
		// let the apply detach and delete unrelated authored work.
		if (bIsEvent)
		{
			if (SourceEntry->IsA<UK2Node_CustomEvent>())
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("migration source node %s is a custom event and never an entry of the replaced declaration '%s'"),
						*SourceEntryGuid.ToString(), *DeclarationName.ToString()));
				return false;
			}
			const UK2Node_Event* const SourceEvent = Cast<UK2Node_Event>(SourceEntry);
			const UClass* const SourceOwner = SourceEvent ? SourceEvent->EventReference.GetMemberParentClass() : nullptr;
			if (!SourceEvent
				|| SourceEvent->EventReference.GetMemberName() != DeclarationName
				|| !SourceOwner
				|| !FunctionClass
				|| (SourceOwner != FunctionClass && !SourceOwner->IsChildOf(FunctionClass)))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("migration source node %s is not an entry of the replaced declaration '%s' on '%s'"),
						*SourceEntryGuid.ToString(), *DeclarationName.ToString(),
						FunctionClass ? *FunctionClass->GetPathName() : TEXT("<none>")));
				return false;
			}
		}
		else
		{
			const UK2Node_FunctionEntry* const SourceFunctionEntry = Cast<UK2Node_FunctionEntry>(SourceEntry);
			if (!SourceFunctionEntry || SourceFunctionEntry->FunctionReference.GetMemberName() != DeclarationName)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("migration source node %s is not the function entry of the replaced declaration '%s'"),
						*SourceEntryGuid.ToString(), *DeclarationName.ToString()));
				return false;
			}
		}
	}

	// A competing implementation entry for the declaration is a conflicting identity state, so it is
	// reported before a missing source entry: an already-implemented declaration must never look like
	// a dangling locator, and it is never silently reused or overwritten.
	if (const UEdGraphNode* Competing = FindCompetingDeclarationEntry(
		Blueprint, ReplacementEntry, SourceEntry, Function->GetFName(), FunctionClass))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("the target declaration '%s' already has an implementation entry in graph '%s' (node %s) that is not this patch's deterministic replacement identity %s; a partially applied or conflicting identity set is refused instead of repaired"),
				*FunctionName,
				Competing->GetGraph() ? *Competing->GetGraph()->GetName() : TEXT("<none>"),
				*Competing->NodeGuid.ToString(),
				*Identity.EntryGuid.ToString()));
		return false;
	}

	if (!bReused && !SourceEntry)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::NodeNotFound,
			FString::Printf(TEXT("migration source entry %s does not exist in this asset"), *SourceEntryGuid.ToString()));
		return false;
	}

	// Result terminator shape of the replaced set.
	UEdGraphNode* SourceResult = nullptr;
	if (bHasResult)
	{
		int32 ResultCount = 0;
		for (UEdGraphNode* Node : SourceGraph->Nodes)
		{
			if (Cast<UK2Node_FunctionResult>(Node))
			{
				++ResultCount;
				SourceResult = Node;
			}
		}
		if (ResultCount > 1)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("function graph '%s' contains %d result terminators; replace_entry owns exactly one"),
					*SourceGraph->GetName(), ResultCount));
			return false;
		}
		if (bReused)
		{
			if (!SourceResult || SourceResult->NodeGuid != Identity.ResultGuid)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("patch identity set is partial: the deterministic replacement result identity %s does not resolve in graph '%s'"),
						*Identity.ResultGuid.ToString(), *SourceGraph->GetName()));
				return false;
			}
		}
		else if (SourceResult)
		{
			OutPlan.SourceResultGuid = SourceResult->NodeGuid.ToString();
		}
	}
	else if (!bIsEvent)
	{
		for (UEdGraphNode* Node : SourceGraph->Nodes)
		{
			if (Cast<UK2Node_FunctionResult>(Node))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("the target declaration '%s' declares no out parameters, so graph '%s' must not own a function result terminator"),
						*FunctionName, *SourceGraph->GetName()));
				return false;
			}
		}
	}

	// Replacement terminator pin set: the request must cover it exactly.
	FTransientTerminators Terminators;
	if (!BuildTransientTerminators(Blueprint, Function, FunctionClass, bIsEvent, bHasResult, Terminators) || !Terminators.Entry)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("failed to model the replacement terminators for the target declaration"));
		return false;
	}
	OutPlan.EntryPins = DescribePins(Terminators.Entry);
	OutPlan.ResultPins = DescribePins(Terminators.Result);

	TSet<FString> Covered;
	for (const TSharedPtr<FJsonValue>& Value : *PinMapValues)
	{
		const TSharedPtr<FJsonObject> MappingJson = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!MappingJson.IsValid()
			|| !FCortexGraphPatchOps::HasOnlyFields(MappingJson, { TEXT("entry"), TEXT("from_pin"), TEXT("to_pin") }, OutError, TEXT("migration.pin_map entry")))
		{
			if (OutError.ErrorCode.IsEmpty())
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("migration.pin_map entries must be objects"));
			}
			return false;
		}
		FString DeclaredEntry;
		FString FromPin;
		FString ToPin;
		if (!FCortexGraphPatchOps::ReadRequiredString(MappingJson, TEXT("entry"), DeclaredEntry, OutError)) return false;
		if (DeclaredEntry != InputDirection && DeclaredEntry != OutputDirection)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				TEXT("migration.pin_map.entry must be 'input' or 'output'"));
			return false;
		}
		if (!FCortexGraphPatchOps::ReadRequiredString(MappingJson, TEXT("from_pin"), FromPin, OutError)) return false;
		if (!FCortexGraphPatchOps::ReadRequiredString(MappingJson, TEXT("to_pin"), ToPin, OutError)) return false;

		UEdGraphPin* const EntryPin = FindMappablePin(Terminators.Entry, FName(*ToPin));
		UEdGraphPin* const ResultPin = FindMappablePin(Terminators.Result, FName(*ToPin));
		const bool bDeclaredInput = DeclaredEntry == InputDirection;
		UEdGraphPin* Matching = nullptr;
		UEdGraphPin* DirectionConflict = nullptr;
		int32 MatchingRole = INDEX_NONE;
		for (int32 Role = 0; Role < 2; ++Role)
		{
			UEdGraphPin* const Candidate = Role == 0 ? EntryPin : ResultPin;
			if (!Candidate) continue;
			if ((Candidate->Direction == EGPD_Input) == bDeclaredInput)
			{
				if (Matching)
				{
					OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
						FString::Printf(TEXT("migration.pin_map target '%s' is ambiguous: both replacement terminators own a %s pin with that name"),
							*ToPin, *DeclaredEntry));
					return false;
				}
				Matching = Candidate;
				MatchingRole = Role;
			}
			else
			{
				DirectionConflict = Candidate;
			}
		}
		if (!Matching)
		{
			if (DirectionConflict)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::TypeMismatch,
					FString::Printf(TEXT("pin '%s': incompatible direction (declared %s but the replacement pin is %s)"),
						*ToPin, *DeclaredEntry, DirectionString(DirectionConflict->Direction)));
				return false;
			}
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::PinNotFound,
				FString::Printf(TEXT("pin '%s' does not exist on the replacement terminator of declaration '%s'"),
					*ToPin, *FunctionName));
			return false;
		}
		const FString CoverKey = FString::Printf(TEXT("%s:%s"), *RoleString(MatchingRole == 1), *ToPin);
		if (Covered.Contains(CoverKey))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("migration.pin_map maps the replacement pin '%s' more than once"), *ToPin));
			return false;
		}
		Covered.Add(CoverKey);

		FCortexGraphMigrationPinMapping Mapping;
		Mapping.Entry = DeclaredEntry;
		Mapping.TargetRole = RoleString(MatchingRole == 1);
		Mapping.FromPin = FromPin;
		Mapping.ToPin = ToPin;
		Mapping.FromDirection = static_cast<int32>(Matching->Direction);

		UEdGraphNode* const StaleTerminator = MatchingRole == 1 ? SourceResult : SourceEntry;
		if (StaleTerminator)
		{
			UEdGraphPin* const StalePin = FindMappablePin(StaleTerminator, FName(*FromPin));
			if (!StalePin)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::PinNotFound,
					FString::Printf(TEXT("pin '%s' does not exist on the stale %s terminator %s"),
						*FromPin, MatchingRole == 1 ? TEXT("result") : TEXT("entry"), *StaleTerminator->NodeGuid.ToString()));
				return false;
			}
			FString Dimension;
			FString Detail;
			if (!CompareMappedPins(*StalePin, *Matching, DeclaredEntry, Dimension, Detail))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::TypeMismatch,
					FString::Printf(TEXT("pin '%s': incompatible %s (%s)"), *ToPin, *Dimension, *Detail));
				return false;
			}
			Mapping.FromDirection = static_cast<int32>(StalePin->Direction);
		}
		OutPlan.PinMap.Add(MoveTemp(Mapping));
	}

	const TCHAR* const RoleNames[2] = { EntryRole, ResultRole };
	UEdGraphNode* const TerminatorNodes[2] = { Terminators.Entry, Terminators.Result };
	for (int32 Role = 0; Role < 2; ++Role)
	{
		UEdGraphNode* const Terminator = TerminatorNodes[Role];
		if (!Terminator) continue;
		for (UEdGraphPin* Pin : Terminator->Pins)
		{
			if (!IsMappablePin(Pin)) continue;
			const FString CoverKey = FString::Printf(TEXT("%s:%s"), RoleNames[Role], *Pin->PinName.ToString());
			if (!Covered.Contains(CoverKey))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("every pin of the replacement terminator must be mapped exactly once; pin '%s' has no pin_map entry"),
						*Pin->PinName.ToString()));
				return false;
			}
		}
	}

	// Boundary links: every mapped stale pin keeps its far endpoints, and no stale link is dropped.
	TArray<FCortexGraphMigrationEdge> Edges;
	TSet<FString> EdgeKeys;
	if (bReused)
	{
		UEdGraphNode* ResultNode = bHasResult ? FindNodeByGuid(Blueprint, Identity.ResultGuid) : nullptr;
		for (const FCortexGraphMigrationPinMapping& Mapping : OutPlan.PinMap)
		{
			UEdGraphNode* const ReplacementNode = Mapping.TargetRole == ResultRole ? ResultNode : ReplacementEntry;
			UEdGraphPin* const ReplacementPin = FindMappablePin(ReplacementNode, FName(*Mapping.ToPin));
			if (!ReplacementPin) continue;
			for (UEdGraphPin* LinkedPin : ReplacementPin->LinkedTo)
			{
				UEdGraphNode* const FarNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
				if (!FarNode) continue;
				FCortexGraphMigrationEdge Edge;
				Edge.ReplacementRole = Mapping.TargetRole;
				Edge.ReplacementPin = Mapping.ToPin;
				Edge.FarPin = LinkedPin->PinName.ToString();
				if (FarNode == ReplacementEntry)
				{
					Edge.FarRole = EntryRole;
				}
				else if (ResultNode && FarNode == ResultNode)
				{
					Edge.FarRole = ResultRole;
				}
				else
				{
					Edge.FarGuid = FarNode->NodeGuid.ToString();
				}
				AddCanonicalEdge(Edges, EdgeKeys, MoveTemp(Edge));
			}
		}
	}
	else
	{
		// Every stale pin that carries a link must be mapped, otherwise the replacement would
		// silently drop user wiring.
		UEdGraphNode* const StaleTerminators[2] = { SourceEntry, SourceResult };
		for (int32 Role = 0; Role < 2; ++Role)
		{
			UEdGraphNode* const Stale = StaleTerminators[Role];
			if (!Stale) continue;
			for (UEdGraphPin* StalePin : Stale->Pins)
			{
				if (!IsMappablePin(StalePin) || StalePin->LinkedTo.Num() == 0) continue;
				bool bMapped = false;
				for (const FCortexGraphMigrationPinMapping& Mapping : OutPlan.PinMap)
				{
					if (Mapping.TargetRole == RoleNames[Role] && Mapping.FromPin == StalePin->PinName.ToString())
					{
						bMapped = true;
						break;
					}
				}
				if (!bMapped)
				{
					OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
						FString::Printf(TEXT("the stale pin '%s' on terminator %s carries %d link(s) but has no pin_map entry; the replacement must not drop user wiring"),
							*StalePin->PinName.ToString(), *Stale->NodeGuid.ToString(), StalePin->LinkedTo.Num()));
					return false;
				}
			}
		}

		for (const FCortexGraphMigrationPinMapping& Mapping : OutPlan.PinMap)
		{
			UEdGraphNode* const StaleTerminator = Mapping.TargetRole == ResultRole ? SourceResult : SourceEntry;
			UEdGraphPin* const StalePin = FindMappablePin(StaleTerminator, FName(*Mapping.FromPin));
			if (!StalePin) continue;
			for (UEdGraphPin* LinkedPin : StalePin->LinkedTo)
			{
				UEdGraphNode* const FarNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
				if (!FarNode) continue;
				FCortexGraphMigrationEdge Edge;
				Edge.ReplacementRole = Mapping.TargetRole;
				Edge.ReplacementPin = Mapping.ToPin;
				if (FarNode == SourceEntry || FarNode == SourceResult)
				{
					const FString FarRole = FarNode == SourceResult ? ResultRole : EntryRole;
					bool bResolved = false;
					for (const FCortexGraphMigrationPinMapping& FarMapping : OutPlan.PinMap)
					{
						if (FarMapping.TargetRole == FarRole && FarMapping.FromPin == LinkedPin->PinName.ToString())
						{
							Edge.FarRole = FarMapping.TargetRole;
							Edge.FarPin = FarMapping.ToPin;
							bResolved = true;
							break;
						}
					}
					if (!bResolved)
					{
						OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
							FString::Printf(TEXT("the stale pin '%s' on terminator %s links to the replaced set but has no pin_map entry; the replacement must not drop that wiring"),
								*LinkedPin->PinName.ToString(), *FarNode->NodeGuid.ToString()));
						return false;
					}
				}
				else
				{
					Edge.FarRole.Reset();
					Edge.FarGuid = FarNode->NodeGuid.ToString();
					Edge.FarPin = LinkedPin->PinName.ToString();
				}
				AddCanonicalEdge(Edges, EdgeKeys, MoveTemp(Edge));
			}
		}
	}
	if (Edges.Num() > MaxMigrationEdges)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::LimitExceeded,
			FString::Printf(TEXT("the migration boundary exceeds max_migration_edges=%d"), MaxMigrationEdges));
		return false;
	}
	Edges.Sort([](const FCortexGraphMigrationEdge& A, const FCortexGraphMigrationEdge& B)
	{
		const FString KeyA = A.ReplacementRole + TEXT(".") + A.ReplacementPin + TEXT("->") + A.FarRole + A.FarGuid + TEXT(".") + A.FarPin;
		const FString KeyB = B.ReplacementRole + TEXT(".") + B.ReplacementPin + TEXT("->") + B.FarRole + B.FarGuid + TEXT(".") + B.FarPin;
		return KeyA < KeyB;
	});
	OutPlan.Edges = Edges;

	// Presentation of the stale entry moves to the replacement so the layout and comments survive.
	if (SourceEntry)
	{
		OutPlan.NodePosX = SourceEntry->NodePosX;
		OutPlan.NodePosY = SourceEntry->NodePosY;
		OutPlan.NodeComment = SourceEntry->NodeComment;
		OutPlan.bCommentBubblePinned = SourceEntry->bCommentBubblePinned;
		OutPlan.bCommentBubbleVisible = SourceEntry->bCommentBubbleVisible;
		OutPlan.EnabledState = static_cast<int32>(SourceEntry->GetDesiredEnabledState());
		OutPlan.bUserSetEnabledState = SourceEntry->HasUserSetTheEnabledState();
		OutPlan.bForceDisplayAsDisabled = SourceEntry->IsDisplayAsDisabledForced();
	}
	else if (ReplacementEntry)
	{
		OutPlan.NodePosX = ReplacementEntry->NodePosX;
		OutPlan.NodePosY = ReplacementEntry->NodePosY;
		OutPlan.NodeComment = ReplacementEntry->NodeComment;
		OutPlan.bCommentBubblePinned = ReplacementEntry->bCommentBubblePinned;
		OutPlan.bCommentBubbleVisible = ReplacementEntry->bCommentBubbleVisible;
		OutPlan.EnabledState = static_cast<int32>(ReplacementEntry->GetDesiredEnabledState());
		OutPlan.bUserSetEnabledState = ReplacementEntry->HasUserSetTheEnabledState();
		OutPlan.bForceDisplayAsDisabled = ReplacementEntry->IsDisplayAsDisabledForced();
	}

	TArray<FGuid> ExcludedGuids;
	ExcludedGuids.Add(Identity.EntryGuid);
	ExcludedGuids.Add(Identity.ResultGuid);
	ExcludedGuids.Add(SourceEntryGuid);
	if (!OutPlan.SourceResultGuid.IsEmpty())
	{
		FGuid SourceResultGuid;
		FGuid::Parse(OutPlan.SourceResultGuid, SourceResultGuid);
		ExcludedGuids.Add(SourceResultGuid);
	}
	for (const FString& GuidText : OutPlan.MemberReferenceNodeGuids)
	{
		FGuid ReferenceGuid;
		if (FGuid::Parse(GuidText, ReferenceGuid)) ExcludedGuids.Add(ReferenceGuid);
	}
	OutPlan.PreservationCapture = CapturePreservation(Blueprint, SourceGraph, ExcludedGuids);

	// The shared readback compares the replacement entry as an authoring-shaped planned node.
	if (bIsEvent)
	{
		TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("client_id"), EntryRole);
		Node->SetStringField(TEXT("node_class"), TEXT("Event"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("function_name"), FunctionName);
		Params->SetStringField(TEXT("owner_class"), FunctionClass->GetPathName());
		Node->SetObjectField(TEXT("params"), Params);
		if (SourceEntry)
		{
			TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
			Position->SetNumberField(TEXT("x"), SourceEntry->NodePosX);
			Position->SetNumberField(TEXT("y"), SourceEntry->NodePosY);
			Node->SetObjectField(TEXT("position"), Position);
		}
		Node->SetArrayField(TEXT("resolved_pins"), OutPlan.EntryPins);
		OutPlan.NormalizedNode = Node;
	}
	OutPlan.ResolvedSymbol = MakeShared<FJsonObject>();
	OutPlan.ResolvedSymbol->SetStringField(TEXT("function_name"), FunctionName);
	OutPlan.ResolvedSymbol->SetStringField(TEXT("owner_class"), FunctionClass->GetPathName());

	OutPlan.Op = Op;
	OutPlan.GraphGuid = GraphGuid.ToString();
	OutPlan.SubgraphPath = SubgraphPath;
	OutPlan.SourceEntryGuid = SourceEntryGuid.ToString();
	OutPlan.Identity = Identity;
	OutPlan.bIsEvent = bIsEvent;
	OutPlan.bHasResultTerminator = bHasResult;
	OutPlan.FunctionName = FunctionName;
	OutPlan.OwnerClassPath = FunctionClass->GetPathName();
	bOutReused = bReused;
	return true;
}

namespace
{
/** Compares one live terminator's visible pin set against the planned canonical signatures. */
bool VerifyPinSet(UEdGraphNode* Node, const TArray<TSharedPtr<FJsonValue>>& Planned, const TCHAR* Role, FString& OutFailure)
{
	if (!Node)
	{
		OutFailure = FString::Printf(TEXT("the %s terminator did not re-resolve"), Role);
		return false;
	}
	TSet<FString> PlannedNames;
	for (const TSharedPtr<FJsonValue>& Value : Planned)
	{
		const TSharedPtr<FJsonObject> Descriptor = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!Descriptor.IsValid())
		{
			OutFailure = FString::Printf(TEXT("planned %s pin signature is invalid"), Role);
			return false;
		}
		const FString PinName = Descriptor->GetStringField(TEXT("name"));
		PlannedNames.Add(PinName);
		UEdGraphPin* const Pin = FindMappablePin(Node, FName(*PinName));
		if (!Pin)
		{
			OutFailure = FString::Printf(TEXT("planned %s pin '%s' no longer exists on the replacement terminator"), Role, *PinName);
			return false;
		}
		const FString Expected = FCortexGraphPatchOps::CanonicalPinSignature(Descriptor);
		const FString Actual = PinSignature(*Pin);
		if (Expected != Actual)
		{
			OutFailure = FString::Printf(TEXT("planned %s pin '%s' signature mismatch: expected '%s', found '%s'"),
				Role, *PinName, *Expected, *Actual);
			return false;
		}
	}
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (!IsMappablePin(Pin)) continue;
		if (!PlannedNames.Contains(Pin->PinName.ToString()))
		{
			OutFailure = FString::Printf(TEXT("the replacement %s terminator has an unexpected native pin '%s' the plan never declared"),
				Role, *Pin->PinName.ToString());
			return false;
		}
	}
	return true;
}
}

bool FCortexGraphMigrationOps::VerifyReplacementAgainstNative(
	UBlueprint* Blueprint,
	const FCortexGraphMigrationPlan& Plan,
	const bool bCompiled,
	const bool bVerifyPreservation,
	FString& OutFailure)
{
	OutFailure.Reset();
	if (!Blueprint)
	{
		OutFailure = TEXT("blueprint is missing");
		return false;
	}
	FGuid GraphGuid;
	FGuid::Parse(Plan.GraphGuid, GraphGuid);
	UEdGraph* const Graph = FindGraphByGuid(Blueprint, GraphGuid);
	if (!Graph)
	{
		OutFailure = TEXT("the target graph did not re-resolve");
		return false;
	}
	UEdGraphNode* const Entry = FindNodeByGuid(Blueprint, Plan.Identity.EntryGuid);
	if (!Entry || Entry->GetGraph() != Graph)
	{
		OutFailure = FString::Printf(TEXT("the replacement entry identity %s did not re-resolve in the target graph"),
			*Plan.Identity.EntryGuid.ToString());
		return false;
	}
	const FName FunctionName(*Plan.FunctionName);
	UClass* DeclaringClass = nullptr;
	{
		FCortexCommandResult ResolveError;
		if (!FCortexGraphSymbolResolver::ResolveClass(Plan.OwnerClassPath, DeclaringClass, ResolveError))
		{
			OutFailure = FString::Printf(TEXT("the declared owner class '%s' no longer resolves"), *Plan.OwnerClassPath);
			return false;
		}
	}
	if (Plan.bIsEvent)
	{
		UK2Node_Event* const Event = Cast<UK2Node_Event>(Entry);
		if (!Event || !Event->bOverrideFunction)
		{
			OutFailure = TEXT("the replacement entry is not an inherited event override");
			return false;
		}
		if (Event->EventReference.GetMemberName() != FunctionName)
		{
			OutFailure = FString::Printf(TEXT("replacement event symbol mismatch: expected '%s', found '%s'"),
				*Plan.FunctionName, *Event->EventReference.GetMemberName().ToString());
			return false;
		}
		const UClass* const ActualOwner = Event->EventReference.GetMemberParentClass();
		if (DeclaringClass && ActualOwner != DeclaringClass)
		{
			OutFailure = FString::Printf(TEXT("replacement event owner mismatch: expected '%s', found '%s'"),
				*DeclaringClass->GetPathName(), ActualOwner ? *ActualOwner->GetPathName() : TEXT("none"));
			return false;
		}
	}
	else
	{
		UK2Node_FunctionEntry* const FunctionEntry = Cast<UK2Node_FunctionEntry>(Entry);
		if (!FunctionEntry)
		{
			OutFailure = TEXT("the replacement entry is not a function entry terminator");
			return false;
		}
		if (Graph->GetFName() != FunctionName)
		{
			OutFailure = FString::Printf(TEXT("replacement function graph mismatch: expected '%s', found '%s'"),
				*Plan.FunctionName, *Graph->GetName());
			return false;
		}
		if (FunctionEntry->FunctionReference.GetMemberName() != FunctionName)
		{
			OutFailure = FString::Printf(TEXT("replacement function entry symbol mismatch: expected '%s', found '%s'"),
				*Plan.FunctionName, *FunctionEntry->FunctionReference.GetMemberName().ToString());
			return false;
		}
	}
	UEdGraphNode* Result = nullptr;
	if (Plan.HasResultTerminator())
	{
		Result = FindNodeByGuid(Blueprint, Plan.Identity.ResultGuid);
		if (!Result || Result->GetGraph() != Graph || !Cast<UK2Node_FunctionResult>(Result))
		{
			OutFailure = FString::Printf(TEXT("the replacement result identity %s did not re-resolve as a function result terminator"),
				*Plan.Identity.ResultGuid.ToString());
			return false;
		}
	}
	if (!VerifyPinSet(Entry, Plan.EntryPins, EntryRole, OutFailure)) return false;
	if (Plan.HasResultTerminator() && !VerifyPinSet(Result, Plan.ResultPins, ResultRole, OutFailure)) return false;

	for (const FCortexGraphMigrationEdge& Edge : Plan.Edges)
	{
		UEdGraphNode* const ReplacementNode = Edge.ReplacementRole == ResultRole ? Result : Entry;
		UEdGraphPin* const ReplacementPin = FindMappablePin(ReplacementNode, FName(*Edge.ReplacementPin));
		UEdGraphPin* FarPin = nullptr;
		FString FarDescription;
		if (Edge.FarRole.IsEmpty())
		{
			FGuid FarGuid;
			FGuid::Parse(Edge.FarGuid, FarGuid);
			UEdGraphNode* const FarNode = FindNodeByGuid(Blueprint, FarGuid);
			FarPin = FindMappablePin(FarNode, FName(*Edge.FarPin));
			FarDescription = FString::Printf(TEXT("node %s pin '%s'"), *Edge.FarGuid, *Edge.FarPin);
		}
		else
		{
			UEdGraphNode* const Sibling = Edge.FarRole == ResultRole ? Result : Entry;
			FarPin = FindMappablePin(Sibling, FName(*Edge.FarPin));
			FarDescription = FString::Printf(TEXT("%s terminator pin '%s'"), *Edge.FarRole, *Edge.FarPin);
		}
		if (!ReplacementPin || !FarPin)
		{
			OutFailure = FString::Printf(TEXT("planned boundary link %s.%s -> %s does not re-resolve"),
				*Edge.ReplacementRole, *Edge.ReplacementPin, *FarDescription);
			return false;
		}
		if (!ReplacementPin->LinkedTo.Contains(FarPin) || !FarPin->LinkedTo.Contains(ReplacementPin))
		{
			OutFailure = FString::Printf(TEXT("planned boundary link %s.%s -> %s is not the native link"),
				*Edge.ReplacementRole, *Edge.ReplacementPin, *FarDescription);
			return false;
		}
	}

	if (bCompiled && !DeclarationCompiledIntoClass(Blueprint, FunctionName))
	{
		OutFailure = FString::Printf(TEXT("compiled generated class does not declare the replaced declaration '%s'"), *Plan.FunctionName);
		return false;
	}
	if (bVerifyPreservation)
	{
		TArray<FGuid> ExcludedGuids;
		ExcludedGuids.Add(Plan.Identity.EntryGuid);
		ExcludedGuids.Add(Plan.Identity.ResultGuid);
		FGuid SourceGuid;
		if (FGuid::Parse(Plan.SourceEntryGuid, SourceGuid)) ExcludedGuids.Add(SourceGuid);
		FGuid SourceResultGuid;
		if (FGuid::Parse(Plan.SourceResultGuid, SourceResultGuid)) ExcludedGuids.Add(SourceResultGuid);
		for (const FString& GuidText : Plan.MemberReferenceNodeGuids)
		{
			FGuid ReferenceGuid;
			if (FGuid::Parse(GuidText, ReferenceGuid)) ExcludedGuids.Add(ReferenceGuid);
		}
		const FString Current = CapturePreservation(Blueprint, Graph, ExcludedGuids);
		if (Current != Plan.PreservationCapture)
		{
			OutFailure = TEXT("the downstream node set outside the replaced entry/terminator set changed");
			return false;
		}
	}
	return true;
}

bool FCortexGraphMigrationOps::RegisterReplacement(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const FCortexGraphMigrationPlan& Plan,
	UEdGraphNode*& OutEntry,
	UEdGraphNode*& OutResult,
	FCortexCommandResult& OutError)
{
	OutEntry = nullptr;
	OutResult = nullptr;
	OutError = FCortexCommandResult();
	if (!Blueprint || !Graph)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("prepared migration has no target graph"));
		return false;
	}
	UFunction* Function = nullptr;
	{
		// The stale entry has already been detached when this runs, so the declaration is resolved
		// through the symbol resolver instead of the graph-eligibility primitive.
		const TSharedPtr<FJsonObject> Selector = MakeShared<FJsonObject>();
		Selector->SetStringField(TEXT("function_name"), Plan.FunctionName);
		Selector->SetStringField(TEXT("owner_class"), Plan.OwnerClassPath);
		FCortexResolvedSymbol Symbol;
		FCortexCommandResult ResolveError;
		if (!FCortexGraphSymbolResolver::ResolveFunction(Blueprint, Selector, Symbol, ResolveError) || !Symbol.Function)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("the replaced declaration '%s' no longer resolves: %s"), *Plan.FunctionName, *ResolveError.ErrorMessage));
			return false;
		}
		Function = Symbol.Function;
	}
	UClass* DeclaringClass = nullptr;
	{
		FCortexCommandResult ResolveError;
		if (!FCortexGraphSymbolResolver::ResolveClass(Plan.OwnerClassPath, DeclaringClass, ResolveError) || !DeclaringClass)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("the declared owner class '%s' no longer resolves"), *Plan.OwnerClassPath));
			return false;
		}
	}
	if (!BuildTerminatorPair(Graph, Function, DeclaringClass, Plan.bIsEvent, Plan.HasResultTerminator(), false, OutEntry, OutResult))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("failed to create the replacement terminators"));
		return false;
	}
	OutEntry->NodeGuid = Plan.Identity.EntryGuid;
	if (OutResult) OutResult->NodeGuid = Plan.Identity.ResultGuid;
	UEdGraphNode* const Nodes[2] = { OutEntry, OutResult };
	for (UEdGraphNode* Node : Nodes)
	{
		if (!Node) continue;
		Node->Modify();
		Node->NodePosX = Plan.NodePosX;
		Node->NodePosY = Plan.NodePosY;
		Node->NodeComment = Plan.NodeComment;
		Node->bCommentBubblePinned = Plan.bCommentBubblePinned;
		Node->bCommentBubbleVisible = Plan.bCommentBubbleVisible;
		Node->SetEnabledState(static_cast<ENodeEnabledState>(Plan.EnabledState), Plan.bUserSetEnabledState);
		Node->SetForceDisplayAsDisabled(Plan.bForceDisplayAsDisabled);
	}
	if (OutResult)
	{
		OutResult->NodePosX = OutEntry->NodePosX + OutEntry->NodeWidth + 256;
		OutResult->NodePosY = OutEntry->NodePosY;
	}
	Graph->NotifyGraphChanged();
	return true;
}

bool FCortexGraphMigrationOps::RemapBoundary(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const FCortexGraphMigrationPlan& Plan,
	UEdGraphNode* Entry,
	UEdGraphNode* Result,
	TArray<FCortexGraphMigrationLink>& OutCreatedLinks,
	FCortexCommandResult& OutError)
{
	OutCreatedLinks.Reset();
	OutError = FCortexCommandResult();
	if (!Blueprint || !Graph || !Entry)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("prepared migration has no replacement entry to wire"));
		return false;
	}
	const UEdGraphSchema* const Schema = Graph->GetSchema();
	if (!Schema)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("the target graph has no schema"));
		return false;
	}
	for (const FCortexGraphMigrationEdge& Edge : Plan.Edges)
	{
		UEdGraphNode* const ReplacementNode = Edge.ReplacementRole == ResultRole ? Result : Entry;
		UEdGraphPin* const ReplacementPin = FindMappablePin(ReplacementNode, FName(*Edge.ReplacementPin));
		UEdGraphPin* FarPin = nullptr;
		if (Edge.FarRole.IsEmpty())
		{
			FGuid FarGuid;
			FGuid::Parse(Edge.FarGuid, FarGuid);
			FarPin = FindMappablePin(FindNodeByGuid(Blueprint, FarGuid), FName(*Edge.FarPin));
		}
		else
		{
			UEdGraphNode* const Sibling = Edge.FarRole == ResultRole ? Result : Entry;
			FarPin = FindMappablePin(Sibling, FName(*Edge.FarPin));
		}
		if (!ReplacementPin || !FarPin)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("planned boundary link %s.%s could not re-resolve its pins"), *Edge.ReplacementRole, *Edge.ReplacementPin));
			return false;
		}
		if (ReplacementPin->LinkedTo.Contains(FarPin)) continue;
		UEdGraphPin* const OutputPin = ReplacementPin->Direction == EGPD_Output ? ReplacementPin : FarPin;
		UEdGraphPin* const InputPin = ReplacementPin->Direction == EGPD_Output ? FarPin : ReplacementPin;
		const FPinConnectionResponse Response = Schema->CanCreateConnection(OutputPin, InputPin);
		if (Response.Response != CONNECT_RESPONSE_MAKE || !Schema->TryCreateConnection(OutputPin, InputPin))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("the remapped boundary link %s.%s is no longer safe: %s"),
					*Edge.ReplacementRole, *Edge.ReplacementPin, *Response.Message.ToString()));
			return false;
		}
		FCortexGraphMigrationLink Link;
		Link.FromNodeGuid = OutputPin->GetOwningNode()->NodeGuid;
		Link.FromPin = OutputPin->PinName;
		Link.ToNodeGuid = InputPin->GetOwningNode()->NodeGuid;
		Link.ToPin = InputPin->PinName;
		OutCreatedLinks.Add(MoveTemp(Link));
	}
	Graph->NotifyGraphChanged();
	return true;
}

namespace
{
/** Canonical presentation vector of one link endpoint inside the captured set. */
FString CaptureLinkKey(const UEdGraphPin& Pin)
{
	const UEdGraphNode* const Node = Pin.GetOwningNode();
	return FString::Printf(TEXT("%s.%s"), Node ? *Node->NodeGuid.ToString() : TEXT("<none>"), *Pin.PinName.ToString());
}

void AppendPinCapture(const UEdGraphPin& Pin, const TSet<FGuid>& InSet, FString& Out)
{
	const FEdGraphTerminalType& Terminal = Pin.PinType.PinValueType;
	Out += FString::Printf(TEXT("    Pin: Name=%s Dir=%d Cat=%s SubCat=%s SubObj=%s Container=%d Term=%s/%s/%s/%d Ref=%d Const=%d Def=\"%s\" DefText=\"%s\" DefObj=%s\n"),
		*Pin.PinName.ToString(),
		static_cast<int32>(Pin.Direction),
		*Pin.PinType.PinCategory.ToString(),
		*Pin.PinType.PinSubCategory.ToString(),
		Pin.PinType.PinSubCategoryObject.IsValid() ? *Pin.PinType.PinSubCategoryObject->GetPathName() : TEXT("None"),
		static_cast<int32>(Pin.PinType.ContainerType),
		*Terminal.TerminalCategory.ToString(),
		*Terminal.TerminalSubCategory.ToString(),
		Terminal.TerminalSubCategoryObject.IsValid() ? *Terminal.TerminalSubCategoryObject->GetPathName() : TEXT("None"),
		Terminal.bTerminalIsConst ? 1 : 0,
		Pin.PinType.bIsReference ? 1 : 0,
		Pin.PinType.bIsConst ? 1 : 0,
		*Pin.DefaultValue,
		*Pin.DefaultTextValue.ToString(),
		Pin.DefaultObject ? *Pin.DefaultObject->GetPathName() : TEXT("None"));

	TArray<FString> Links;
	for (const UEdGraphPin* LinkedPin : Pin.LinkedTo)
	{
		const UEdGraphNode* const LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
		// Only links inside the captured set are part of the preservation contract; boundary links
		// are asserted separately as the deliberately remapped edges.
		if (!LinkedNode || !InSet.Contains(LinkedNode->NodeGuid)) continue;
		Links.Add(CaptureLinkKey(*LinkedPin));
	}
	Links.Sort();
	Out += FString::Printf(TEXT("     InternalLinks: %s\n"), *FString::Join(Links, TEXT(",")));
}

void AppendNodeCapture(UEdGraphNode* Node, const TSet<FGuid>& InSet, FString& Out)
{
	Out += FString::Printf(TEXT("  Node: Guid=%s Class=%s Pos=(%d,%d) Comment=\"%s\" Bubble=%d/%d Enabled=%d User=%d Forced=%d\n"),
		*Node->NodeGuid.ToString(),
		*Node->GetClass()->GetPathName(),
		Node->NodePosX,
		Node->NodePosY,
		*Node->NodeComment,
		Node->bCommentBubblePinned ? 1 : 0,
		Node->bCommentBubbleVisible ? 1 : 0,
		static_cast<int32>(Node->GetDesiredEnabledState()),
		Node->HasUserSetTheEnabledState() ? 1 : 0,
		Node->IsDisplayAsDisabledForced() ? 1 : 0);

	if (const UK2Node_CallFunction* const Call = Cast<UK2Node_CallFunction>(Node))
	{
		Out += FString::Printf(TEXT("   CallFunc: Member=%s Parent=%s Pure=%d\n"),
			*Call->FunctionReference.GetMemberName().ToString(),
			Call->FunctionReference.GetMemberParentClass() ? *Call->FunctionReference.GetMemberParentClass()->GetPathName() : TEXT("None"),
			Call->IsNodePure() ? 1 : 0);
	}
	else if (const UK2Node_CallParentFunction* const Parent = Cast<UK2Node_CallParentFunction>(Node))
	{
		Out += FString::Printf(TEXT("   CallParent: Member=%s Parent=%s\n"),
			*Parent->FunctionReference.GetMemberName().ToString(),
			Parent->FunctionReference.GetMemberParentClass() ? *Parent->FunctionReference.GetMemberParentClass()->GetPathName() : TEXT("None"));
	}
	else if (const UK2Node_CustomEvent* const CustomEvent = Cast<UK2Node_CustomEvent>(Node))
	{
		Out += FString::Printf(TEXT("   CustomEvent: Name=%s\n"), *CustomEvent->CustomFunctionName.ToString());
	}
	else if (const UK2Node_Event* const Event = Cast<UK2Node_Event>(Node))
	{
		Out += FString::Printf(TEXT("   Event: Name=%s Parent=%s Override=%d\n"),
			*Event->EventReference.GetMemberName().ToString(),
			Event->EventReference.GetMemberParentClass() ? *Event->EventReference.GetMemberParentClass()->GetPathName() : TEXT("None"),
			Event->bOverrideFunction ? 1 : 0);
	}
	else if (const UK2Node_Variable* const Variable = Cast<UK2Node_Variable>(Node))
	{
		Out += FString::Printf(TEXT("   Variable: Member=%s Parent=%s Self=%d Write=%d\n"),
			*Variable->VariableReference.GetMemberName().ToString(),
			Variable->VariableReference.GetMemberParentClass() ? *Variable->VariableReference.GetMemberParentClass()->GetPathName() : TEXT("None"),
			Variable->VariableReference.IsSelfContext() ? 1 : 0,
			Variable->IsA<UK2Node_VariableSet>() ? 1 : 0);
	}
	else if (const UK2Node_FunctionEntry* const FunctionEntry = Cast<UK2Node_FunctionEntry>(Node))
	{
		Out += FString::Printf(TEXT("   FunctionEntry: Member=%s Parent=%s\n"),
			*FunctionEntry->FunctionReference.GetMemberName().ToString(),
			FunctionEntry->FunctionReference.GetMemberParentClass() ? *FunctionEntry->FunctionReference.GetMemberParentClass()->GetPathName() : TEXT("None"));
	}
	else if (const UK2Node_FunctionResult* const FunctionResult = Cast<UK2Node_FunctionResult>(Node))
	{
		Out += FString::Printf(TEXT("   FunctionResult: Member=%s Parent=%s\n"),
			*FunctionResult->FunctionReference.GetMemberName().ToString(),
			FunctionResult->FunctionReference.GetMemberParentClass() ? *FunctionResult->FunctionReference.GetMemberParentClass()->GetPathName() : TEXT("None"));
	}
	else if (const UK2Node_DynamicCast* const DynCast = Cast<UK2Node_DynamicCast>(Node))
	{
		Out += FString::Printf(TEXT("   DynCast: Target=%s Pure=%d\n"),
			DynCast->TargetType ? *DynCast->TargetType->GetPathName() : TEXT("None"),
			DynCast->IsNodePure() ? 1 : 0);
	}
	else if (const UK2Node_CreateDelegate* const CreateDelegate = Cast<UK2Node_CreateDelegate>(Node))
	{
		Out += FString::Printf(TEXT("   CreateDelegate: Function=%s\n"), *CreateDelegate->GetFunctionName().ToString());
	}
	else if (const UK2Node_Composite* const Composite = Cast<UK2Node_Composite>(Node))
	{
		Out += FString::Printf(TEXT("   Composite: BoundGraph=%s\n"), Composite->BoundGraph ? *Composite->BoundGraph->GetName() : TEXT("None"));
	}

	TArray<UEdGraphPin*> SortedPins;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->ParentPin == nullptr) SortedPins.Add(Pin);
	}
	SortedPins.Sort([](const UEdGraphPin& A, const UEdGraphPin& B)
	{
		if (A.Direction != B.Direction) return static_cast<int32>(A.Direction) < static_cast<int32>(B.Direction);
		return A.PinName.LexicalLess(B.PinName);
	});
	Out += FString::Printf(TEXT("   Pins: %d\n"), SortedPins.Num());
	for (const UEdGraphPin* Pin : SortedPins)
	{
		AppendPinCapture(*Pin, InSet, Out);
	}
}
}

FString FCortexGraphMigrationOps::CapturePreservation(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TArray<FGuid>& ExcludedGuids)
{
	if (!Blueprint || !Graph) return FString();
	TSet<FGuid> Excluded;
	for (const FGuid& Guid : ExcludedGuids) Excluded.Add(Guid);
	TSet<FGuid> InSet;
	TArray<UEdGraphNode*> Nodes;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!IsValid(Node) || Excluded.Contains(Node->NodeGuid)) continue;
		Nodes.Add(Node);
		InSet.Add(Node->NodeGuid);
	}
	Nodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
	{
		return A.NodeGuid.ToString() < B.NodeGuid.ToString();
	});
	FString Capture;
	Capture.Reserve(8192);
	Capture += FString::Printf(TEXT("Graph: %s\nNodes: %d\n"), *Graph->GraphGuid.ToString(), Nodes.Num());
	for (UEdGraphNode* Node : Nodes)
	{
		AppendNodeCapture(Node, InSet, Capture);
	}
	return Capture;
}

TSharedPtr<FJsonObject> FCortexGraphMigrationOps::CaptureShadowingMember(UBlueprint* Blueprint, const FName MemberName)
{
	if (!Blueprint || MemberName.IsNone()) return nullptr;
	const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, MemberName);
	if (Index == INDEX_NONE) return nullptr;
	const FBPVariableDescription& Variable = Blueprint->NewVariables[Index];
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("index"), Index);
	Out->SetStringField(TEXT("name"), Variable.VarName.ToString());
	Out->SetStringField(TEXT("guid"), Variable.VarGuid.ToString());
	Out->SetStringField(TEXT("pin_category"), Variable.VarType.PinCategory.ToString());
	Out->SetStringField(TEXT("pin_subcategory"), Variable.VarType.PinSubCategory.ToString());
	Out->SetStringField(TEXT("pin_subobject"), Variable.VarType.PinSubCategoryObject.IsValid()
		? Variable.VarType.PinSubCategoryObject->GetPathName() : FString());
	Out->SetNumberField(TEXT("container"), static_cast<int32>(Variable.VarType.ContainerType));
	Out->SetNumberField(TEXT("flags"), static_cast<double>(Variable.PropertyFlags));
	Out->SetStringField(TEXT("default_value"), Variable.DefaultValue);
	Out->SetStringField(TEXT("friendly_name"), Variable.FriendlyName);
	Out->SetStringField(TEXT("category"), Variable.Category.ToString());
	// The remaining state the authoring fingerprint cannot see has to be captured explicitly.
	Out->SetBoolField(TEXT("is_reference"), Variable.VarType.bIsReference);
	Out->SetBoolField(TEXT("is_const"), Variable.VarType.bIsConst);
	Out->SetBoolField(TEXT("is_weak_pointer"), Variable.VarType.bIsWeakPointer);
	Out->SetBoolField(TEXT("is_uobject_wrapper"), Variable.VarType.bIsUObjectWrapper);
	Out->SetBoolField(TEXT("single_precision"), Variable.VarType.bSerializeAsSinglePrecisionFloat);
	Out->SetStringField(TEXT("terminal_category"), Variable.VarType.PinValueType.TerminalCategory.ToString());
	Out->SetStringField(TEXT("terminal_subcategory"), Variable.VarType.PinValueType.TerminalSubCategory.ToString());
	Out->SetStringField(TEXT("terminal_subobject"), Variable.VarType.PinValueType.TerminalSubCategoryObject.IsValid()
		? Variable.VarType.PinValueType.TerminalSubCategoryObject->GetPathName() : FString());
	Out->SetBoolField(TEXT("terminal_const"), Variable.VarType.PinValueType.bTerminalIsConst);
	Out->SetBoolField(TEXT("terminal_uobject_wrapper"), Variable.VarType.PinValueType.bTerminalIsUObjectWrapper);
	const FSimpleMemberReference& MemberReference = Variable.VarType.PinSubCategoryMemberReference;
	Out->SetStringField(TEXT("member_reference_name"), MemberReference.MemberName.ToString());
	Out->SetStringField(TEXT("member_reference_parent"),
		MemberReference.MemberParent ? MemberReference.MemberParent->GetPathName() : FString());
	Out->SetStringField(TEXT("member_reference_guid"), MemberReference.MemberGuid.ToString());
	Out->SetStringField(TEXT("rep_notify"), Variable.RepNotifyFunc.ToString());
	Out->SetNumberField(TEXT("replication_condition"), static_cast<int32>(Variable.ReplicationCondition.GetValue()));
	TArray<TSharedPtr<FJsonValue>> Metadata;
	for (const FBPVariableMetaDataEntry& Entry : Variable.MetaDataArray)
	{
		TSharedPtr<FJsonObject> MetadataEntry = MakeShared<FJsonObject>();
		MetadataEntry->SetStringField(TEXT("key"), Entry.DataKey.ToString());
		MetadataEntry->SetStringField(TEXT("value"), Entry.DataValue);
		Metadata.Add(MakeShared<FJsonValueObject>(MetadataEntry));
	}
	Out->SetArrayField(TEXT("metadata"), Metadata);
	return Out;
}

bool FCortexGraphMigrationOps::RemoveShadowingMember(
	UBlueprint* Blueprint,
	const FName MemberName,
	FCortexCommandResult& OutError)
{
	OutError = FCortexCommandResult();
	if (!Blueprint || MemberName.IsNone())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("no shadowing member was planned for removal"));
		return false;
	}
	if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, MemberName) == INDEX_NONE)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::VariableNotFound,
			FString::Printf(TEXT("shadowing member '%s' no longer exists"), *MemberName.ToString()));
		return false;
	}
	FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, MemberName);
	if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, MemberName) != INDEX_NONE)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("shadowing member '%s' could not be removed"), *MemberName.ToString()));
		return false;
	}
	return true;
}

bool FCortexGraphMigrationOps::RestoreShadowingMember(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& CapturedVariable,
	const int32 Index,
	FCortexCommandResult& OutError)
{
	OutError = FCortexCommandResult();
	if (!Blueprint || !CapturedVariable.IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("captured shadowing member is missing"));
		return false;
	}
	FString Name;
	if (!CapturedVariable->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("captured shadowing member has no name"));
		return false;
	}
	const FName MemberName(*Name);
	if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, MemberName) != INDEX_NONE)
	{
		// The engine already restored the member (for example through a transaction cancel).
		return true;
	}
	FEdGraphPinType Type;
	FString Text;
	CapturedVariable->TryGetStringField(TEXT("pin_category"), Text);
	Type.PinCategory = FName(*Text);
	Text.Reset();
	CapturedVariable->TryGetStringField(TEXT("pin_subcategory"), Text);
	Type.PinSubCategory = FName(*Text);
	int32 Container = 0;
	if (CapturedVariable->TryGetNumberField(TEXT("container"), Container))
	{
		Type.ContainerType = static_cast<EPinContainerType>(Container);
	}
	FString SubObjectPath;
	CapturedVariable->TryGetStringField(TEXT("pin_subobject"), SubObjectPath);
	if (!SubObjectPath.IsEmpty())
	{
		UObject* const SubObject = FindObject<UObject>(nullptr, *SubObjectPath);
		if (!SubObject)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("captured shadowing member '%s' references '%s', which no longer resolves"), *Name, *SubObjectPath));
			return false;
		}
		Type.PinSubCategoryObject = SubObject;
	}
	bool bIsReference = false;
	if (CapturedVariable->TryGetBoolField(TEXT("is_reference"), bIsReference)) Type.bIsReference = bIsReference;
	bool bIsConst = false;
	if (CapturedVariable->TryGetBoolField(TEXT("is_const"), bIsConst)) Type.bIsConst = bIsConst;
	bool bIsWeakPointer = false;
	if (CapturedVariable->TryGetBoolField(TEXT("is_weak_pointer"), bIsWeakPointer)) Type.bIsWeakPointer = bIsWeakPointer;
	bool bIsUobjectWrapper = false;
	if (CapturedVariable->TryGetBoolField(TEXT("is_uobject_wrapper"), bIsUobjectWrapper)) Type.bIsUObjectWrapper = bIsUobjectWrapper;
	bool bSinglePrecision = false;
	if (CapturedVariable->TryGetBoolField(TEXT("single_precision"), bSinglePrecision)) Type.bSerializeAsSinglePrecisionFloat = bSinglePrecision;
	FString TerminalText;
	CapturedVariable->TryGetStringField(TEXT("terminal_category"), TerminalText);
	Type.PinValueType.TerminalCategory = FName(*TerminalText);
	TerminalText.Reset();
	CapturedVariable->TryGetStringField(TEXT("terminal_subcategory"), TerminalText);
	Type.PinValueType.TerminalSubCategory = FName(*TerminalText);
	FString TerminalSubObjectPath;
	CapturedVariable->TryGetStringField(TEXT("terminal_subobject"), TerminalSubObjectPath);
	if (!TerminalSubObjectPath.IsEmpty())
	{
		UObject* const TerminalSubObject = FindObject<UObject>(nullptr, *TerminalSubObjectPath);
		if (!TerminalSubObject)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("captured shadowing member '%s' references terminal type '%s', which no longer resolves"), *Name, *TerminalSubObjectPath));
			return false;
		}
		Type.PinValueType.TerminalSubCategoryObject = TerminalSubObject;
	}
	bool bTerminalIsConstFlag = false;
	if (CapturedVariable->TryGetBoolField(TEXT("terminal_const"), bTerminalIsConstFlag)) Type.PinValueType.bTerminalIsConst = bTerminalIsConstFlag;
	bool bTerminalIsUObjectWrapperFlag = false;
	if (CapturedVariable->TryGetBoolField(TEXT("terminal_uobject_wrapper"), bTerminalIsUObjectWrapperFlag)) Type.PinValueType.bTerminalIsUObjectWrapper = bTerminalIsUObjectWrapperFlag;
	FString MemberReferenceGuidText;
	FGuid MemberReferenceGuid;
	if (CapturedVariable->TryGetStringField(TEXT("member_reference_guid"), MemberReferenceGuidText)
		&& FGuid::Parse(MemberReferenceGuidText, MemberReferenceGuid))
	{
		Type.PinSubCategoryMemberReference.MemberGuid = MemberReferenceGuid;
	}
	FString MemberReferenceName;
	CapturedVariable->TryGetStringField(TEXT("member_reference_name"), MemberReferenceName);
	if (!MemberReferenceName.IsEmpty())
	{
		Type.PinSubCategoryMemberReference.MemberName = FName(*MemberReferenceName);
	}
	FString MemberReferenceParentPath;
	CapturedVariable->TryGetStringField(TEXT("member_reference_parent"), MemberReferenceParentPath);
	if (!MemberReferenceParentPath.IsEmpty())
	{
		UClass* const MemberReferenceParent = FindObject<UClass>(nullptr, *MemberReferenceParentPath);
		if (!MemberReferenceParent)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("captured shadowing member '%s' references owner '%s', which no longer resolves"), *Name, *MemberReferenceParentPath));
			return false;
		}
		Type.PinSubCategoryMemberReference.MemberParent = MemberReferenceParent;
	}
	const int32 InsertIndex = Index >= 0 ? FMath::Clamp(Index, 0, Blueprint->NewVariables.Num()) : Blueprint->NewVariables.Num();
	FString DefaultValue;
	CapturedVariable->TryGetStringField(TEXT("default_value"), DefaultValue);
	double NumericFlags = 0.0;
	CapturedVariable->TryGetNumberField(TEXT("flags"), NumericFlags);
	FString FriendlyName;
	CapturedVariable->TryGetStringField(TEXT("friendly_name"), FriendlyName);
	FString CategoryText;
	CapturedVariable->TryGetStringField(TEXT("category"), CategoryText);
	FBPVariableDescription Description;
	Description.VarName = MemberName;
	Description.VarType = Type;
	Description.DefaultValue = DefaultValue;
	Description.PropertyFlags = static_cast<uint64>(NumericFlags);
	Description.FriendlyName = FriendlyName;
	Description.Category = FText::FromString(CategoryText);
	FString GuidText;
	FGuid ParsedGuid;
	if (CapturedVariable->TryGetStringField(TEXT("guid"), GuidText) && FGuid::Parse(GuidText, ParsedGuid))
	{
		Description.VarGuid = ParsedGuid;
	}
	FString RepNotify;
	if (CapturedVariable->TryGetStringField(TEXT("rep_notify"), RepNotify) && !RepNotify.IsEmpty())
	{
		Description.RepNotifyFunc = FName(*RepNotify);
	}
	int32 ReplicationCondition = 0;
	if (CapturedVariable->TryGetNumberField(TEXT("replication_condition"), ReplicationCondition))
	{
		Description.ReplicationCondition = static_cast<ELifetimeCondition>(ReplicationCondition);
	}
	const TArray<TSharedPtr<FJsonValue>>* Metadata = nullptr;
	if (CapturedVariable->TryGetArrayField(TEXT("metadata"), Metadata) && Metadata)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Metadata)
		{
			const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Entry.IsValid()) continue;
			FString Key;
			FString MetadataValue;
			if (Entry->TryGetStringField(TEXT("key"), Key) && Entry->TryGetStringField(TEXT("value"), MetadataValue) && !Key.IsEmpty())
			{
				FBPVariableMetaDataEntry MetadataEntry;
				MetadataEntry.DataKey = FName(*Key);
				MetadataEntry.DataValue = MetadataValue;
				Description.MetaDataArray.Add(MetadataEntry);
			}
		}
	}
	// The captured description is re-inserted verbatim: a re-creation round trip through the engine
	// variable API cannot preserve the captured identity exactly, and it would regenerate the class
	// while the recovered variable is present.
	Blueprint->Modify();
	Blueprint->NewVariables.Insert(Description, InsertIndex);
	return true;
}

bool FCortexGraphMigrationOps::MemberMatchesCapture(
	UBlueprint* Blueprint,
	const FName MemberName,
	const TSharedPtr<FJsonObject>& Captured,
	FString& OutFailure)
{
	OutFailure.Reset();
	if (!Blueprint || !Captured.IsValid())
	{
		OutFailure = TEXT("captured shadowing member is missing");
		return false;
	}
	if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, MemberName) == INDEX_NONE)
	{
		OutFailure = FString::Printf(TEXT("shadowing member '%s' was not restored"), *MemberName.ToString());
		return false;
	}
	const TSharedPtr<FJsonObject> Live = CaptureShadowingMember(Blueprint, MemberName);
	if (!Live.IsValid())
	{
		OutFailure = TEXT("restored shadowing member cannot be captured");
		return false;
	}
	// The whole captured description must match, field by field: the authoring fingerprint cannot see
	// rep-notify, replication, metadata or the remaining pin-type flags, so a lost field must fail here.
	const FString LiveText = FrozenJsonValue(MakeShared<FJsonValueObject>(Live));
	const FString CapturedText = FrozenJsonValue(MakeShared<FJsonValueObject>(Captured));
	if (LiveText != CapturedText)
	{
		OutFailure = FString::Printf(TEXT("restored shadowing member '%s' does not match its captured description: captured %s, found %s"),
			*MemberName.ToString(), *CapturedText, *LiveText);
		return false;
	}
	return true;
}

UEdGraphNode* FCortexGraphMigrationOps::FindNodeByGuid(UBlueprint* Blueprint, const FGuid& NodeGuid)
{
	if (!Blueprint || !NodeGuid.IsValid()) return nullptr;
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == NodeGuid) return Node;
		}
	}
	return nullptr;
}

UEdGraph* FCortexGraphMigrationOps::FindGraphByGuid(UBlueprint* Blueprint, const FGuid& GraphGuid)
{
	if (!Blueprint || !GraphGuid.IsValid()) return nullptr;
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph && Graph->GraphGuid == GraphGuid) return Graph;
	}
	return nullptr;
}

UEdGraphNode* FCortexGraphMigrationOps::FindNodeByGuidInGraph(UEdGraph* Graph, const FGuid& NodeGuid)
{
	if (!Graph || !NodeGuid.IsValid()) return nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->NodeGuid == NodeGuid) return Node;
	}
	return nullptr;
}

// ===========================================================================
// Bounded same-asset transfer: `copy_subgraph` / `move_subgraph`
// ===========================================================================

namespace
{
constexpr int32 MaxTransferNodes = 64;
constexpr int32 MaxTransferBoundary = 64;

const TCHAR* const TransferCopyOp = TEXT("copy_subgraph");
const TCHAR* const TransferMoveOp = TEXT("move_subgraph");

bool IsTransferOp(const FString& Op)
{
	return Op == TransferCopyOp || Op == TransferMoveOp;
}

/** Canonical symbol descriptor of one transferable node, or empty when the class carries none. */
FString TransferNodeSymbol(const UEdGraphNode* Node)
{
	if (const UK2Node_CallFunction* const Call = Cast<UK2Node_CallFunction>(Node))
	{
		const UFunction* const Target = Call->GetTargetFunction();
		const UClass* const Owner = Target
			? Target->GetOuterUClass()
			: Call->FunctionReference.GetMemberParentClass();
		return FString::Printf(TEXT("call:%s@%s|self=%d|pure=%d"),
			*Call->FunctionReference.GetMemberName().ToString(),
			Owner ? *Owner->GetPathName() : TEXT("none"),
			Call->FunctionReference.IsSelfContext() ? 1 : 0,
			Call->IsNodePure() ? 1 : 0);
	}
	if (const UK2Node_Variable* const Variable = Cast<UK2Node_Variable>(Node))
	{
		const UClass* const Owner = Variable->VariableReference.IsSelfContext()
			? nullptr
			: Variable->VariableReference.GetMemberParentClass();
		return FString::Printf(TEXT("var:%s@%s|self=%d|local=%d|set=%d"),
			*Variable->VariableReference.GetMemberName().ToString(),
			Owner ? *Owner->GetPathName() : TEXT("self"),
			Variable->VariableReference.IsSelfContext() ? 1 : 0,
			Variable->VariableReference.IsLocalScope() ? 1 : 0,
			Variable->IsA<UK2Node_VariableSet>() ? 1 : 0);
	}
	if (const UK2Node_DynamicCast* const DynCast = Cast<UK2Node_DynamicCast>(Node))
	{
		return FString::Printf(TEXT("cast:%s"), DynCast->TargetType ? *DynCast->TargetType->GetPathName() : TEXT("none"));
	}
	return FString();
}

/** Canonical capture of one node's class, layout, comment, bubble and enabled state. */
FString TransferNodePresentation(const FCortexGraphTransferNode& Node)
{
	return FString::Printf(TEXT("pos=(%d,%d)|comment=\"%s\"|bubble=%d/%d|enabled=%d/%d/%d"),
		Node.PosX, Node.PosY, *Node.Comment,
		Node.bCommentBubblePinned ? 1 : 0, Node.bCommentBubbleVisible ? 1 : 0,
		Node.EnabledState, Node.bUserSetEnabledState ? 1 : 0, Node.bForceDisplayAsDisabled ? 1 : 0);
}

/** The same capture read from live native state instead of from the durable plan. */
FString TransferNodePresentation(const UEdGraphNode& Node)
{
	return FString::Printf(TEXT("pos=(%d,%d)|comment=\"%s\"|bubble=%d/%d|enabled=%d/%d/%d"),
		Node.NodePosX, Node.NodePosY, *Node.NodeComment,
		Node.bCommentBubblePinned ? 1 : 0, Node.bCommentBubbleVisible ? 1 : 0,
		static_cast<int32>(Node.GetDesiredEnabledState()),
		Node.HasUserSetTheEnabledState() ? 1 : 0,
		Node.IsDisplayAsDisabledForced() ? 1 : 0);
}

/**
 * Canonical authored capture of one node's pins: the shared canonical pin signature plus the
 * authored default, and every link whose far endpoint is inside the captured set expressed as a
 * slot index. Crossing links are deliberately outside this capture: for the source side they are
 * the explicit boundary map and for the destination side the realized boundary edges.
 */
FString TransferNodePins(UEdGraphNode* Node, const TMap<FGuid, int32>& SlotByGuid)
{
	if (!Node) return FString();
	TArray<UEdGraphPin*> Sorted;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		// A split parent delegates its links to its expanded children, so the children are captured
		// and the parent contributes its expanded state instead of duplicating those links.
		if (Pin && Pin->SubPins.Num() == 0) Sorted.Add(Pin);
	}
	Sorted.Sort([](const UEdGraphPin& A, const UEdGraphPin& B)
	{
		if (A.Direction != B.Direction) return static_cast<int32>(A.Direction) < static_cast<int32>(B.Direction);
		return A.PinName.LexicalLess(B.PinName);
	});
	FString Capture = FString::Printf(TEXT("pins=%d"), Sorted.Num());
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->SubPins.Num() == 0) continue;
		TArray<FString> Children;
		for (const UEdGraphPin* Child : Pin->SubPins)
		{
			if (Child) Children.Add(Child->PinName.ToString());
		}
		Children.Sort();
		Capture += FString::Printf(TEXT(";split=%s|children=[%s]"), *Pin->PinName.ToString(), *FString::Join(Children, TEXT(",")));
	}
	for (const UEdGraphPin* Pin : Sorted)
	{
		TArray<FString> Links;
		for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			const UEdGraphNode* const LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
			if (!LinkedNode) continue;
			const int32* const Slot = SlotByGuid.Find(LinkedNode->NodeGuid);
			if (!Slot) continue;
			Links.Add(FString::Printf(TEXT("slot:%d.%s"), *Slot, *LinkedPin->PinName.ToString()));
		}
		Links.Sort();
		Capture += FString::Printf(TEXT(";%s|def=\"%s\"|deftext=\"%s\"|defobj=%s|links=[%s]"),
			*FCortexGraphPatchOps::CanonicalPinSignature(FCortexGraphPatchOps::MakePinSignatureDescriptor(*Pin)),
			*Pin->DefaultValue,
			*Pin->DefaultTextValue.ToString(),
			Pin->DefaultObject ? *Pin->DefaultObject->GetPathName() : TEXT("none"),
			*FString::Join(Links, TEXT(",")));
	}
	return Capture;
}

/** Every graph of the asset that owns one node GUID, plus the total number of matching nodes. */
void TransferGraphsOwningGuid(UBlueprint* Blueprint, const FGuid& NodeGuid, TArray<UEdGraph*>& OutGraphs, int32& OutMatchCount)
{
	OutGraphs.Reset();
	OutMatchCount = 0;
	if (!Blueprint || !NodeGuid.IsValid()) return;
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph) continue;
		int32 MatchesInGraph = 0;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == NodeGuid) ++MatchesInGraph;
		}
		if (MatchesInGraph == 0) continue;
		OutGraphs.Add(Graph);
		OutMatchCount += MatchesInGraph;
	}
}

/**
 * Engine class names a bounded same-asset transfer refuses before any mutation, with the reason the
 * refusal reports. The test walks the node's own inheritance chain, so a subclass of a refused class
 * is refused under the same reason instead of slipping through the selection.
 */
const TMap<FString, FString>& TransferRefusedClassNames()
{
	static const TMap<FString, FString> Refused = {
		{ TEXT("K2Node_Tunnel"), TEXT("tunnel") },
		{ TEXT("K2Node_TunnelBoundary"), TEXT("tunnel boundary") },
		{ TEXT("K2Node_Knot"), TEXT("knot") },
		{ TEXT("K2Node_Composite"), TEXT("composite node with a bound subgraph") },
		{ TEXT("K2Node_Timeline"), TEXT("timeline") },
		{ TEXT("K2Node_EditablePinBase"), TEXT("editable-pin terminator owned by its graph lifecycle") },
		{ TEXT("K2Node_Event"), TEXT("event node owned by its graph lifecycle") },
		{ TEXT("K2Node_FunctionEntry"), TEXT("function terminator owned by its graph lifecycle") },
		{ TEXT("K2Node_FunctionResult"), TEXT("function terminator owned by its graph lifecycle") },
		{ TEXT("K2Node_CustomEvent"), TEXT("custom event node owned by its graph lifecycle") },
		{ TEXT("K2Node_BaseMCDelegate"), TEXT("delegate node") },
		{ TEXT("K2Node_CallDelegate"), TEXT("delegate node") },
		{ TEXT("K2Node_CreateDelegate"), TEXT("delegate node") },
		{ TEXT("K2Node_AssignDelegate"), TEXT("delegate node") },
		{ TEXT("K2Node_DelegateSet"), TEXT("delegate node") },
		{ TEXT("K2Node_BaseAsyncTask"), TEXT("latent async task node") },
		{ TEXT("K2Node_AsyncAction"), TEXT("latent async action node") },
		{ TEXT("K2Node_LatentGameplayTaskCall"), TEXT("latent gameplay task call") },
	};
	return Refused;
}

/** True when the node may be transferred into the given destination graph; otherwise names why not. */
bool ClassifyTransferNode(UEdGraph* DestinationGraph, UEdGraphNode* Node, FString& OutReason)
{
	OutReason.Reset();
	if (!Node || !Node->GetClass() || !Node->NodeGuid.IsValid())
	{
		OutReason = TEXT("invalid node");
		return false;
	}
	if (const UK2Node_CallFunction* const Call = Cast<UK2Node_CallFunction>(Node))
	{
		if (Call->IsLatentFunction())
		{
			OutReason = FString::Printf(TEXT("latent function call '%s'"), *Call->FunctionReference.GetMemberName().ToString());
			return false;
		}
	}
	for (UClass* Class = Node->GetClass(); Class && Class != UObject::StaticClass(); Class = Class->GetSuperClass())
	{
		const FString ClassName = Class->GetName();
		if (ClassName.Contains(TEXT("Latent")))
		{
			OutReason = FString::Printf(TEXT("latent node class '%s'"), *Node->GetClass()->GetName());
			return false;
		}
		if (const FString* const Reason = TransferRefusedClassNames().Find(ClassName))
		{
			OutReason = *Reason;
			return false;
		}
	}
	if (!Node->IsA<UK2Node>() && !Node->IsA<UEdGraphNode_Comment>())
	{
		OutReason = FString::Printf(TEXT("non-K2 graph node class '%s'"), *Node->GetClass()->GetName());
		return false;
	}
	if (!Node->CanDuplicateNode())
	{
		OutReason = FString::Printf(TEXT("the engine refuses to duplicate node class '%s'"), *Node->GetClass()->GetName());
		return false;
	}
	if (!DestinationGraph || !Node->CanCreateUnderSpecifiedSchema(DestinationGraph->GetSchema()))
	{
		OutReason = FString::Printf(TEXT("node class '%s' cannot be created under the destination graph schema"), *Node->GetClass()->GetName());
		return false;
	}
	// The engine paste factory drops or substitutes a node it cannot paste, so the same predicate
	// preflight uses is the factory's own, and a selection the factory would drop never reaches it.
	if (!DestinationGraph || !Node->CanPasteHere(DestinationGraph))
	{
		OutReason = FString::Printf(TEXT("the engine refuses to paste node class '%s' into the destination graph"), *Node->GetClass()->GetName());
		return false;
	}
	// An expanded (split) struct pin is authored state the engine clone path does not round-trip, so
	// any split pin is refused: a linked child would otherwise be dropped silently and an unlinked
	// one would lose its expanded state.
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) continue;
		if (Pin->SubPins.Num() == 0 && Pin->ParentPin == nullptr) continue;
		OutReason = FString::Printf(TEXT("split pin '%s' is struct-expanded; the engine clone path does not prove the expanded state or its links round-trip"),
			*Pin->PinName.ToString());
		return false;
	}
	return true;
}

/** The interface that declares the named member for this asset's class, when one does. */
UClass* InterfaceDeclaringMember(UBlueprint* Blueprint, const FName MemberName)
{
	UClass* const AssetClass = Blueprint && Blueprint->SkeletonGeneratedClass
		? Blueprint->SkeletonGeneratedClass.Get()
		: (Blueprint ? Blueprint->GeneratedClass.Get() : nullptr);
	if (!AssetClass || MemberName.IsNone()) return nullptr;
	for (UClass* CurrentClass = AssetClass; CurrentClass; CurrentClass = CurrentClass->GetSuperClass())
	{
		for (const FImplementedInterface& Implemented : CurrentClass->Interfaces)
		{
			UClass* const Interface = Implemented.Class.Get();
			if (Interface && Interface->FindFunctionByName(MemberName))
			{
				return Interface;
			}
		}
	}
	return nullptr;
}

/** Signature identity of one declared local variable, so a mismatch is comparable. */
FString LocalVariableTypeIdentity(const FEdGraphPinType& PinType)
{
	return FCortexGraphPatchOps::CanonicalPinSignature(
		FCortexGraphPatchOps::MakePinSignatureDescriptorForType(PinType));
}

/** Dependency inventory of one selected node. */
void CollectTransferDependencies(
	UBlueprint* Blueprint,
	UEdGraphNode* Node,
	TArray<FCortexGraphTransferDependency>& OutDependencies)
{
	const FName MemberName = [Node]() -> FName
	{
		if (const UK2Node_CallFunction* const Call = Cast<UK2Node_CallFunction>(Node))
		{
			return Call->FunctionReference.GetMemberName();
		}
		if (const UK2Node_Variable* const Variable = Cast<UK2Node_Variable>(Node))
		{
			return Variable->VariableReference.GetMemberName();
		}
		return NAME_None;
	}();
	if (MemberName.IsNone()) return;

	FCortexGraphTransferDependency Dependency;
	Dependency.NodeGuid = Node->NodeGuid.ToString();
	Dependency.Member = MemberName.ToString();

	const UK2Node_Variable* const Variable = Cast<UK2Node_Variable>(Node);
	const UK2Node_CallFunction* const Call = Cast<UK2Node_CallFunction>(Node);
	if (Variable && Variable->VariableReference.IsLocalScope())
	{
		Dependency.Kind = TEXT("local_variable");
		if (const FBPVariableDescription* const Description =
			FBlueprintEditorUtils::FindLocalVariable(Blueprint, Node->GetGraph(), MemberName))
		{
			Dependency.Type = LocalVariableTypeIdentity(Description->VarType);
		}
		Dependency.Detail = TEXT("a local variable of the source graph scope; the destination graph must declare an identically named and typed local variable when the graphs differ");
		OutDependencies.Add(MoveTemp(Dependency));
		return;
	}

	UClass* DeclaringClass = nullptr;
	if (Call)
	{
		const UFunction* const Target = Call->GetTargetFunction();
		DeclaringClass = Target ? Target->GetOuterUClass() : Call->FunctionReference.GetMemberParentClass();
	}
	else if (Variable)
	{
		DeclaringClass = Variable->VariableReference.GetMemberParentClass();
	}
	Dependency.OwnerClass = DeclaringClass ? DeclaringClass->GetPathName() : FString(TEXT("self"));

	UClass* const Interface = InterfaceDeclaringMember(Blueprint, MemberName);
	if (Interface)
	{
		Dependency.Kind = TEXT("interface");
		Dependency.OwnerClass = Interface->GetPathName();
		Dependency.Detail = FString::Printf(TEXT("declared by implemented interface '%s'"), *Interface->GetName());
	}
	else if (DeclaringClass && DeclaringClass->HasAnyClassFlags(CLASS_Interface))
	{
		Dependency.Kind = TEXT("interface");
		Dependency.Detail = TEXT("declared by an interface class");
	}
	else if (DeclaringClass && Blueprint && Blueprint->ParentClass && DeclaringClass->IsChildOf(Blueprint->ParentClass))
	{
		Dependency.Kind = TEXT("member");
		Dependency.Detail = TEXT("declared by this asset's own class hierarchy");
	}
	else if (DeclaringClass)
	{
		Dependency.Kind = TEXT("external");
		Dependency.Detail = TEXT("declared outside this asset");
	}
	else
	{
		Dependency.Kind = TEXT("member");
		Dependency.Detail = TEXT("self-context member reference resolved by the asset's class");
	}
	OutDependencies.Add(MoveTemp(Dependency));
}

/** Canonical key of one directed graph link endpoint. */
FString TransferEndpointKey(const FGuid& NodeGuid, const FName PinName)
{
	return FString::Printf(TEXT("%s.%s"), *NodeGuid.ToString(), *PinName.ToString());
}

/** One crossing or internal link of the selection, canonicalized so both sides agree. */
struct FTransferLink
{
	FGuid FromNode;
	FName FromPin;
	FGuid ToNode;
	FName ToPin;

	FString Key() const
	{
		return TransferEndpointKey(FromNode, FromPin) + TEXT("->") + TransferEndpointKey(ToNode, ToPin);
	}
};

/**
 * Inventories every link that touches the selection exactly once: a link with both endpoints inside
 * the selection is an internal edge, any other link is a crossing edge. A crossing edge is recorded
 * from its selected endpoint, so the boundary map is keyed by the pin the transfer owns.
 */
void InventoryTransferLinks(
	UEdGraphNode* Node,
	const TSet<FGuid>& Selection,
	TArray<FTransferLink>& OutInternal,
	TArray<FTransferLink>& OutCrossing,
	TSet<FString>& InOutKeys)
{
	if (!Node) return;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		// A split parent delegates its links to its expanded children, so only leaf pins are walked.
		if (!Pin || Pin->SubPins.Num() > 0) continue;
		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			UEdGraphNode* const FarNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
			if (!FarNode) continue;
			const bool bFarInside = Selection.Contains(FarNode->NodeGuid);
			FTransferLink Link;
			if (!bFarInside)
			{
				// The selected endpoint owns the crossing edge, whichever side of the link it is.
				Link = { Node->NodeGuid, Pin->PinName, FarNode->NodeGuid, LinkedPin->PinName };
			}
			else if (Pin->Direction == EGPD_Output
				|| (Pin->Direction == LinkedPin->Direction && Pin->PinName.LexicalLess(LinkedPin->PinName)))
			{
				Link = { Node->NodeGuid, Pin->PinName, FarNode->NodeGuid, LinkedPin->PinName };
			}
			else
			{
				Link = { FarNode->NodeGuid, LinkedPin->PinName, Node->NodeGuid, Pin->PinName };
			}
			if (InOutKeys.Contains(Link.Key())) continue;
			InOutKeys.Add(Link.Key());
			if (bFarInside)
			{
				OutInternal.Add(Link);
			}
			else
			{
				OutCrossing.Add(Link);
			}
		}
	}
}

/** Canonical key of one boundary `from` endpoint: the selected source pin a crossing edge leaves. */
FString TransferBoundaryFromKey(const UEdGraphNode* Node, const UEdGraphPin& Pin)
{
	return TransferEndpointKey(Node ? Node->NodeGuid : FGuid(), Pin.PinName);
}
}

// ---------------------------------------------------------------------------
// Durable plan transport
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FCortexGraphMigrationTransferPlan::ToJson() const
{
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("op"), Op);
	Out->SetStringField(TEXT("source_graph_guid"), SourceGraphGuid);
	Out->SetStringField(TEXT("source_subgraph_path"), SourceSubgraphPath);
	Out->SetStringField(TEXT("destination_graph_guid"), DestinationGraphGuid);
	Out->SetStringField(TEXT("destination_subgraph_path"), DestinationSubgraphPath);
	Out->SetBoolField(TEXT("reused"), bReused);

	TArray<TSharedPtr<FJsonValue>> NodeValues;
	for (const FCortexGraphTransferNode& Node : Nodes)
	{
		TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
		NodeJson->SetStringField(TEXT("source_guid"), Node.SourceGuid);
		NodeJson->SetStringField(TEXT("destination_guid"), Node.DestinationGuid);
		NodeJson->SetStringField(TEXT("class_path"), Node.ClassPath);
		NodeJson->SetStringField(TEXT("symbol"), Node.Symbol);
		NodeJson->SetNumberField(TEXT("pos_x"), Node.PosX);
		NodeJson->SetNumberField(TEXT("pos_y"), Node.PosY);
		NodeJson->SetStringField(TEXT("comment"), Node.Comment);
		NodeJson->SetBoolField(TEXT("bubble_pinned"), Node.bCommentBubblePinned);
		NodeJson->SetBoolField(TEXT("bubble_visible"), Node.bCommentBubbleVisible);
		NodeJson->SetNumberField(TEXT("enabled_state"), Node.EnabledState);
		NodeJson->SetBoolField(TEXT("user_set_enabled_state"), Node.bUserSetEnabledState);
		NodeJson->SetBoolField(TEXT("force_display_disabled"), Node.bForceDisplayAsDisabled);
		NodeJson->SetStringField(TEXT("pins"), Node.Pins);
		NodeValues.Add(MakeShared<FJsonValueObject>(NodeJson));
	}
	Out->SetArrayField(TEXT("nodes"), NodeValues);

	TArray<TSharedPtr<FJsonValue>> EdgeValues;
	for (const FCortexGraphTransferEdge& Edge : InternalEdges)
	{
		TSharedPtr<FJsonObject> EdgeJson = MakeShared<FJsonObject>();
		EdgeJson->SetStringField(TEXT("from_guid"), Edge.FromGuid);
		EdgeJson->SetStringField(TEXT("from_pin"), Edge.FromPin);
		EdgeJson->SetStringField(TEXT("to_guid"), Edge.ToGuid);
		EdgeJson->SetStringField(TEXT("to_pin"), Edge.ToPin);
		EdgeValues.Add(MakeShared<FJsonValueObject>(EdgeJson));
	}
	Out->SetArrayField(TEXT("internal_edges"), EdgeValues);

	TArray<TSharedPtr<FJsonValue>> BoundaryValues;
	for (const FCortexGraphTransferBoundary& Entry : Boundary)
	{
		TSharedPtr<FJsonObject> BoundaryJson = MakeShared<FJsonObject>();
		BoundaryJson->SetStringField(TEXT("source_guid"), Entry.SourceGuid);
		BoundaryJson->SetStringField(TEXT("source_pin"), Entry.SourcePin);
		BoundaryJson->SetStringField(TEXT("source_far_guid"), Entry.SourceFarGuid);
		BoundaryJson->SetStringField(TEXT("source_far_pin"), Entry.SourceFarPin);
		BoundaryJson->SetStringField(TEXT("destination_node_guid"), Entry.DestinationNodeGuid);
		BoundaryJson->SetStringField(TEXT("destination_pin"), Entry.DestinationPin);
		BoundaryValues.Add(MakeShared<FJsonValueObject>(BoundaryJson));
	}
	Out->SetArrayField(TEXT("boundary"), BoundaryValues);

	TArray<TSharedPtr<FJsonValue>> DependencyValues;
	for (const FCortexGraphTransferDependency& Dependency : Dependencies)
	{
		TSharedPtr<FJsonObject> DependencyJson = MakeShared<FJsonObject>();
		DependencyJson->SetStringField(TEXT("node_guid"), Dependency.NodeGuid);
		DependencyJson->SetStringField(TEXT("kind"), Dependency.Kind);
		DependencyJson->SetStringField(TEXT("member"), Dependency.Member);
		DependencyJson->SetStringField(TEXT("owner_class"), Dependency.OwnerClass);
		DependencyJson->SetStringField(TEXT("type"), Dependency.Type);
		DependencyJson->SetStringField(TEXT("detail"), Dependency.Detail);
		DependencyValues.Add(MakeShared<FJsonValueObject>(DependencyJson));
	}
	Out->SetArrayField(TEXT("dependencies"), DependencyValues);

	TArray<TSharedPtr<FJsonValue>> RemovalValues;
	for (const FString& Guid : RemovalSet)
	{
		RemovalValues.Add(MakeShared<FJsonValueString>(Guid));
	}
	Out->SetArrayField(TEXT("removal_set"), RemovalValues);

	TArray<TSharedPtr<FJsonValue>> PreservationValues;
	for (const FCortexGraphTransferPreservation& Contract : Preservations)
	{
		TSharedPtr<FJsonObject> ContractJson = MakeShared<FJsonObject>();
		ContractJson->SetStringField(TEXT("label"), Contract.Label);
		ContractJson->SetStringField(TEXT("graph_guid"), Contract.GraphGuid);
		TArray<TSharedPtr<FJsonValue>> ExcludedValues;
		for (const FString& Guid : Contract.ExcludedGuids)
		{
			ExcludedValues.Add(MakeShared<FJsonValueString>(Guid));
		}
		ContractJson->SetArrayField(TEXT("excluded_guids"), ExcludedValues);
		ContractJson->SetStringField(TEXT("capture"), Contract.Capture);
		PreservationValues.Add(MakeShared<FJsonValueObject>(ContractJson));
	}
	Out->SetArrayField(TEXT("preservations"), PreservationValues);
	return Out;
}

bool FCortexGraphMigrationTransferPlan::FromJson(
	const TSharedPtr<FJsonObject>& Source,
	FCortexGraphMigrationTransferPlan& OutPlan,
	FCortexCommandResult& OutError)
{
	OutPlan = FCortexGraphMigrationTransferPlan();
	OutError = FCortexCommandResult();
	if (!Source.IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("prepared transfer plan is missing"));
		return false;
	}
	const bool bRead = Source->TryGetStringField(TEXT("op"), OutPlan.Op)
		&& Source->TryGetStringField(TEXT("source_graph_guid"), OutPlan.SourceGraphGuid)
		&& Source->TryGetStringField(TEXT("destination_graph_guid"), OutPlan.DestinationGraphGuid);
	if (!bRead || !IsTransferOp(OutPlan.Op))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("prepared transfer plan is incomplete or names an unsupported transfer operation"));
		return false;
	}
	Source->TryGetStringField(TEXT("source_subgraph_path"), OutPlan.SourceSubgraphPath);
	Source->TryGetStringField(TEXT("destination_subgraph_path"), OutPlan.DestinationSubgraphPath);
	Source->TryGetBoolField(TEXT("reused"), OutPlan.bReused);

	const TArray<TSharedPtr<FJsonValue>>* NodeValues = nullptr;
	if (Source->TryGetArrayField(TEXT("nodes"), NodeValues) && NodeValues)
	{
		for (const TSharedPtr<FJsonValue>& Value : *NodeValues)
		{
			const TSharedPtr<FJsonObject> NodeJson = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!NodeJson.IsValid()) continue;
			FCortexGraphTransferNode Node;
			NodeJson->TryGetStringField(TEXT("source_guid"), Node.SourceGuid);
			NodeJson->TryGetStringField(TEXT("destination_guid"), Node.DestinationGuid);
			NodeJson->TryGetStringField(TEXT("class_path"), Node.ClassPath);
			NodeJson->TryGetStringField(TEXT("symbol"), Node.Symbol);
			NodeJson->TryGetStringField(TEXT("comment"), Node.Comment);
			NodeJson->TryGetStringField(TEXT("pins"), Node.Pins);
			int32 Number = 0;
			if (NodeJson->TryGetNumberField(TEXT("pos_x"), Number)) Node.PosX = Number;
			if (NodeJson->TryGetNumberField(TEXT("pos_y"), Number)) Node.PosY = Number;
			if (NodeJson->TryGetNumberField(TEXT("enabled_state"), Number)) Node.EnabledState = Number;
			NodeJson->TryGetBoolField(TEXT("bubble_pinned"), Node.bCommentBubblePinned);
			NodeJson->TryGetBoolField(TEXT("bubble_visible"), Node.bCommentBubbleVisible);
			NodeJson->TryGetBoolField(TEXT("user_set_enabled_state"), Node.bUserSetEnabledState);
			NodeJson->TryGetBoolField(TEXT("force_display_disabled"), Node.bForceDisplayAsDisabled);
			OutPlan.Nodes.Add(MoveTemp(Node));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* EdgeValues = nullptr;
	if (Source->TryGetArrayField(TEXT("internal_edges"), EdgeValues) && EdgeValues)
	{
		for (const TSharedPtr<FJsonValue>& Value : *EdgeValues)
		{
			const TSharedPtr<FJsonObject> EdgeJson = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!EdgeJson.IsValid()) continue;
			FCortexGraphTransferEdge Edge;
			EdgeJson->TryGetStringField(TEXT("from_guid"), Edge.FromGuid);
			EdgeJson->TryGetStringField(TEXT("from_pin"), Edge.FromPin);
			EdgeJson->TryGetStringField(TEXT("to_guid"), Edge.ToGuid);
			EdgeJson->TryGetStringField(TEXT("to_pin"), Edge.ToPin);
			OutPlan.InternalEdges.Add(MoveTemp(Edge));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* BoundaryValues = nullptr;
	if (Source->TryGetArrayField(TEXT("boundary"), BoundaryValues) && BoundaryValues)
	{
		for (const TSharedPtr<FJsonValue>& Value : *BoundaryValues)
		{
			const TSharedPtr<FJsonObject> BoundaryJson = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!BoundaryJson.IsValid()) continue;
			FCortexGraphTransferBoundary Entry;
			BoundaryJson->TryGetStringField(TEXT("source_guid"), Entry.SourceGuid);
			BoundaryJson->TryGetStringField(TEXT("source_pin"), Entry.SourcePin);
			BoundaryJson->TryGetStringField(TEXT("source_far_guid"), Entry.SourceFarGuid);
			BoundaryJson->TryGetStringField(TEXT("source_far_pin"), Entry.SourceFarPin);
			BoundaryJson->TryGetStringField(TEXT("destination_node_guid"), Entry.DestinationNodeGuid);
			BoundaryJson->TryGetStringField(TEXT("destination_pin"), Entry.DestinationPin);
			OutPlan.Boundary.Add(MoveTemp(Entry));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* DependencyValues = nullptr;
	if (Source->TryGetArrayField(TEXT("dependencies"), DependencyValues) && DependencyValues)
	{
		for (const TSharedPtr<FJsonValue>& Value : *DependencyValues)
		{
			const TSharedPtr<FJsonObject> DependencyJson = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!DependencyJson.IsValid()) continue;
			FCortexGraphTransferDependency Dependency;
			DependencyJson->TryGetStringField(TEXT("node_guid"), Dependency.NodeGuid);
			DependencyJson->TryGetStringField(TEXT("kind"), Dependency.Kind);
			DependencyJson->TryGetStringField(TEXT("member"), Dependency.Member);
			DependencyJson->TryGetStringField(TEXT("owner_class"), Dependency.OwnerClass);
			DependencyJson->TryGetStringField(TEXT("type"), Dependency.Type);
			DependencyJson->TryGetStringField(TEXT("detail"), Dependency.Detail);
			OutPlan.Dependencies.Add(MoveTemp(Dependency));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* RemovalValues = nullptr;
	if (Source->TryGetArrayField(TEXT("removal_set"), RemovalValues) && RemovalValues)
	{
		for (const TSharedPtr<FJsonValue>& Value : *RemovalValues)
		{
			FString Guid;
			if (Value.IsValid() && Value->TryGetString(Guid) && !Guid.IsEmpty())
			{
				OutPlan.RemovalSet.Add(Guid);
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* PreservationValues = nullptr;
	if (Source->TryGetArrayField(TEXT("preservations"), PreservationValues) && PreservationValues)
	{
		for (const TSharedPtr<FJsonValue>& Value : *PreservationValues)
		{
			const TSharedPtr<FJsonObject> ContractJson = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!ContractJson.IsValid()) continue;
			FCortexGraphTransferPreservation Contract;
			ContractJson->TryGetStringField(TEXT("label"), Contract.Label);
			ContractJson->TryGetStringField(TEXT("graph_guid"), Contract.GraphGuid);
			ContractJson->TryGetStringField(TEXT("capture"), Contract.Capture);
			const TArray<TSharedPtr<FJsonValue>>* ExcludedValues = nullptr;
			if (ContractJson->TryGetArrayField(TEXT("excluded_guids"), ExcludedValues) && ExcludedValues)
			{
				for (const TSharedPtr<FJsonValue>& Excluded : *ExcludedValues)
				{
					FString Guid;
					if (Excluded.IsValid() && Excluded->TryGetString(Guid) && !Guid.IsEmpty())
					{
						Contract.ExcludedGuids.Add(Guid);
					}
				}
			}
			OutPlan.Preservations.Add(MoveTemp(Contract));
		}
	}

	if (OutPlan.Nodes.Num() == 0 || OutPlan.SourceGraphGuid.IsEmpty() || OutPlan.DestinationGraphGuid.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("prepared transfer plan does not identify its graphs or its selected nodes"));
		return false;
	}
	return true;
}

namespace
{
/** Reads one canonical `graph_ref` of a transfer request, without resolving it in the asset. */
bool ReadTransferGraphRef(
	const TSharedPtr<FJsonObject>& Container,
	const TCHAR* Context,
	FGuid& OutGraphGuid,
	FString& OutSubgraphPath,
	FCortexCommandResult& OutError)
{
	const TSharedPtr<FJsonObject>* GraphRefPtr = nullptr;
	if (!Container.IsValid() || !Container->TryGetObjectField(TEXT("graph_ref"), GraphRefPtr)
		|| !GraphRefPtr || !GraphRefPtr->IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("%s.graph_ref must be an object"), Context));
		return false;
	}
	const TSharedPtr<FJsonObject>& GraphRef = *GraphRefPtr;
	if (!FCortexGraphPatchOps::HasOnlyFields(GraphRef,
		{ TEXT("graph_guid"), TEXT("graph_kind"), TEXT("subgraph_path") }, OutError,
		FString::Printf(TEXT("%s.graph_ref"), Context)))
	{
		return false;
	}
	if (!FCortexGraphPatchOps::ParseGuidField(GraphRef, TEXT("graph_guid"), OutGraphGuid, OutError)) return false;
	if (GraphRef->HasField(TEXT("subgraph_path"))
		&& !GraphRef->TryGetStringField(TEXT("subgraph_path"), OutSubgraphPath))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("%s.graph_ref.subgraph_path must be a string"), Context));
		return false;
	}
	if (GraphRef->HasField(TEXT("graph_kind")))
	{
		FString RequestedKind;
		if (!GraphRef->TryGetStringField(TEXT("graph_kind"), RequestedKind))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("%s.graph_ref.graph_kind must be a string"), Context));
			return false;
		}
	}
	return true;
}

/** True when the requested `graph_kind` agrees with the graph identity the asset really owns. */
bool ValidateTransferGraphKind(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Container,
	const FGuid& GraphGuid,
	FCortexCommandResult& OutError)
{
	const TSharedPtr<FJsonObject>* GraphRefPtr = nullptr;
	FString RequestedKind;
	if (!Container->TryGetObjectField(TEXT("graph_ref"), GraphRefPtr)
		|| !GraphRefPtr || !GraphRefPtr->IsValid()
		|| !(*GraphRefPtr)->TryGetStringField(TEXT("graph_kind"), RequestedKind))
	{
		return true;
	}
	// The same identity resolution the reference was looked up by, so a nested composite child is
	// compared against its own owning graph's kind instead of skipping the check.
	FString ResolvedKind;
	if (!FCortexGraphPatchOps::ResolveGraphKindByGuid(Blueprint, GraphGuid, ResolvedKind, OutError)) return false;
	if (ResolvedKind != RequestedKind)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			TEXT("migration graph_kind conflicts with graph identity"));
		return false;
	}
	return true;
}
}

bool FCortexGraphMigrationOps::PlanTransfer(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Migration,
	const FString& PatchId,
	FCortexGraphMigrationTransferPlan& OutPlan,
	bool& bOutReused,
	FCortexCommandResult& OutError)
{
	OutPlan = FCortexGraphMigrationTransferPlan();
	OutError = FCortexCommandResult();
	bOutReused = false;
	if (!Blueprint)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, TEXT("Blueprint is null"));
		return false;
	}
	if (!Migration.IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("migration must be an object"));
		return false;
	}
	if (!FCortexGraphPatchOps::HasOnlyFields(Migration,
		{ TEXT("op"), TEXT("source"), TEXT("destination"), TEXT("boundary") }, OutError, TEXT("migration")))
	{
		return false;
	}
	FString Op;
	if (!FCortexGraphPatchOps::ReadRequiredString(Migration, TEXT("op"), Op, OutError)) return false;
	if (!IsTransferOp(Op))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::UnsupportedOperation,
			FString::Printf(TEXT("Unsupported migration operation '%s'; the published migration operations are replace_entry, copy_subgraph and move_subgraph"), *Op));
		return false;
	}
	// Source graph and selection.
	const TSharedPtr<FJsonObject>* SourcePtr = nullptr;
	if (!Migration->TryGetObjectField(TEXT("source"), SourcePtr) || !SourcePtr || !SourcePtr->IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("migration.source must be an object"));
		return false;
	}
	const TSharedPtr<FJsonObject>& Source = *SourcePtr;
	if (!FCortexGraphPatchOps::HasOnlyFields(Source,
		{ TEXT("graph_ref"), TEXT("node_guids") }, OutError, TEXT("migration.source")))
	{
		return false;
	}
	FGuid SourceGraphGuid;
	FString SourceSubgraphPath;
	UEdGraph* SourceGraph = nullptr;
	if (!ReadTransferGraphRef(Source, TEXT("migration.source"), SourceGraphGuid, SourceSubgraphPath, OutError)) return false;
	if (!FindGraphByGuid(Blueprint, SourceGraphGuid))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("migration.source.graph_ref names graph '%s', which is not a graph of this asset"),
				*SourceGraphGuid.ToString()));
		return false;
	}
	if (!FCortexGraphPatchOps::ResolveGraphByGuid(Blueprint, SourceGraphGuid, SourceSubgraphPath, SourceGraph, OutError)) return false;
	if (!ValidateTransferGraphKind(Blueprint, Source, SourceGraphGuid, OutError)) return false;

	const TArray<TSharedPtr<FJsonValue>>* NodeGuids = nullptr;
	if (!Source->TryGetArrayField(TEXT("node_guids"), NodeGuids) || !NodeGuids)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			TEXT("migration.source.node_guids must be an array of node GUIDs"));
		return false;
	}
	if (NodeGuids->Num() == 0)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			TEXT("migration.source.node_guids must select at least one node"));
		return false;
	}
	if (NodeGuids->Num() > MaxTransferNodes)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::LimitExceeded,
			TEXT("migration.source.node_guids exceeds the bounded selection limit"));
		return false;
	}
	TArray<FGuid> Selection;
	TArray<UEdGraphNode*> SelectedNodes;
	TArray<FGuid> AbsentGuids;
	for (const TSharedPtr<FJsonValue>& Value : *NodeGuids)
	{
		FString GuidText;
		FGuid NodeGuid;
		if (!Value.IsValid() || !Value->TryGetString(GuidText) || !FGuid::Parse(GuidText, NodeGuid) || !NodeGuid.IsValid())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				TEXT("migration.source.node_guids entries must be node GUID strings"));
			return false;
		}
		if (Selection.Contains(NodeGuid))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				FString::Printf(TEXT("migration.source.node_guids names node '%s' more than once"), *NodeGuid.ToString()));
			return false;
		}
		Selection.Add(NodeGuid);
		UEdGraphNode* const Node = FindNodeByGuidInGraph(SourceGraph, NodeGuid);
		if (Node)
		{
			SelectedNodes.Add(Node);
			continue;
		}
		if (!FindNodeByGuid(Blueprint, NodeGuid))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::NodeNotFound,
				FString::Printf(TEXT("migration.source.node_guids names node '%s', which does not exist in this asset"), *NodeGuid.ToString()));
			return false;
		}
		AbsentGuids.Add(NodeGuid);
	}
	Selection.Sort([](const FGuid& A, const FGuid& B) { return A.ToString() < B.ToString(); });
	SelectedNodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return A.NodeGuid.ToString() < B.NodeGuid.ToString(); });
	if (AbsentGuids.Num() > 0 && AbsentGuids.Num() != Selection.Num())
	{
		TArray<FString> AbsentText;
		for (const FGuid& Guid : AbsentGuids) AbsentText.Add(Guid.ToString());
		TArray<FString> PresentText;
		for (const UEdGraphNode* Node : SelectedNodes) PresentText.Add(Node->NodeGuid.ToString());
		AbsentText.Sort();
		PresentText.Sort();
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("the selected state is partial: %d of %d selected node(s) are already absent from the source graph (%s) while %d are still present (%s); the request is neither a fresh transfer nor a complete replay and is refused instead of transferring a smaller set"),
				AbsentText.Num(), Selection.Num(), *FString::Join(AbsentText, TEXT(", ")),
				PresentText.Num(), *FString::Join(PresentText, TEXT(", "))));
		return false;
	}

	// Destination graph.
	const TSharedPtr<FJsonObject>* DestinationPtr = nullptr;
	if (!Migration->TryGetObjectField(TEXT("destination"), DestinationPtr) || !DestinationPtr || !DestinationPtr->IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("migration.destination must be an object"));
		return false;
	}
	const TSharedPtr<FJsonObject>& Destination = *DestinationPtr;
	if (!FCortexGraphPatchOps::HasOnlyFields(Destination,
		{ TEXT("graph_ref") }, OutError, TEXT("migration.destination")))
	{
		return false;
	}
	FGuid DestinationGraphGuid;
	FString DestinationSubgraphPath;
	if (!ReadTransferGraphRef(Destination, TEXT("migration.destination"), DestinationGraphGuid, DestinationSubgraphPath, OutError)) return false;
	if (!FindGraphByGuid(Blueprint, DestinationGraphGuid))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("migration.destination.graph_ref names graph '%s', which is not a graph of this asset; cross-asset transfer is not supported"),
				*DestinationGraphGuid.ToString()));
		return false;
	}
	UEdGraph* DestinationGraph = nullptr;
	if (!FCortexGraphPatchOps::ResolveGraphByGuid(Blueprint, DestinationGraphGuid, DestinationSubgraphPath, DestinationGraph, OutError)) return false;
	if (!ValidateTransferGraphKind(Blueprint, Destination, DestinationGraphGuid, OutError)) return false;
	if (Op == TransferMoveOp && DestinationGraph == SourceGraph)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("move_subgraph requires a destination graph different from the source graph; use copy_subgraph to duplicate inside one graph"));
		return false;
	}

	OutPlan.Op = Op;
	OutPlan.SourceGraphGuid = SourceGraphGuid.ToString();
	OutPlan.SourceSubgraphPath = SourceSubgraphPath;
	OutPlan.DestinationGraphGuid = DestinationGraphGuid.ToString();
	OutPlan.DestinationSubgraphPath = DestinationSubgraphPath;

	// Boundary entries are read once, so the fresh and the replay path validate the same shape.
	struct FPendingBoundary
	{
		FGuid FromNode;
		FString FromPin;
		FGuid ToNode;
		FString ToPin;
		int32 Index = INDEX_NONE;
	};
	TArray<FPendingBoundary> Pending;
	{
		const TArray<TSharedPtr<FJsonValue>>* BoundaryValues = nullptr;
		if (Migration->TryGetArrayField(TEXT("boundary"), BoundaryValues) && BoundaryValues)
		{
			if (BoundaryValues->Num() > MaxTransferBoundary)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::LimitExceeded,
					TEXT("migration.boundary exceeds the bounded boundary limit"));
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *BoundaryValues)
			{
				const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Entry.IsValid()
					|| !FCortexGraphPatchOps::HasOnlyFields(Entry, { TEXT("from"), TEXT("to") }, OutError, TEXT("migration.boundary entry")))
				{
					if (OutError.ErrorCode.IsEmpty())
					{
						OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
							TEXT("migration.boundary entries must be objects"));
					}
					return false;
				}
				const TSharedPtr<FJsonObject>* FromPtr = nullptr;
				const TSharedPtr<FJsonObject>* ToPtr = nullptr;
				if (!Entry->TryGetObjectField(TEXT("from"), FromPtr) || !FromPtr || !FromPtr->IsValid()
					|| !Entry->TryGetObjectField(TEXT("to"), ToPtr) || !ToPtr || !ToPtr->IsValid())
				{
					OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
						TEXT("migration.boundary entries require a 'from' and a 'to' object"));
					return false;
				}
				if (!FCortexGraphPatchOps::HasOnlyFields(*FromPtr, { TEXT("node_guid"), TEXT("pin") }, OutError, TEXT("migration.boundary.from"))
					|| !FCortexGraphPatchOps::HasOnlyFields(*ToPtr, { TEXT("node_guid"), TEXT("pin") }, OutError, TEXT("migration.boundary.to")))
				{
					return false;
				}
				FPendingBoundary Boundary;
				if (!FCortexGraphPatchOps::ParseGuidField(*FromPtr, TEXT("node_guid"), Boundary.FromNode, OutError)
					|| !FCortexGraphPatchOps::ParseGuidField(*ToPtr, TEXT("node_guid"), Boundary.ToNode, OutError)
					|| !FCortexGraphPatchOps::ReadRequiredString(*FromPtr, TEXT("pin"), Boundary.FromPin, OutError)
					|| !FCortexGraphPatchOps::ReadRequiredString(*ToPtr, TEXT("pin"), Boundary.ToPin, OutError))
				{
					return false;
				}
				if (Op == TransferMoveOp && Selection.Contains(Boundary.ToNode))
				{
					OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
						FString::Printf(TEXT("migration.boundary maps onto node '%s', which this move removes from the source graph"), *Boundary.ToNode.ToString()));
					return false;
				}
				UEdGraphNode* const DestinationNode = FindNodeByGuidInGraph(DestinationGraph, Boundary.ToNode);
				if (!DestinationNode)
				{
					OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
						FString::Printf(TEXT("migration.boundary destination node '%s' is not a node of the named destination graph"), *Boundary.ToNode.ToString()));
					return false;
				}
				if (!DestinationNode->FindPin(FName(*Boundary.ToPin)))
				{
					OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
						FString::Printf(TEXT("migration.boundary destination pin '%s.%s' does not exist"), *Boundary.ToNode.ToString(), *Boundary.ToPin));
					return false;
				}
				Boundary.Index = Pending.Num();
				Pending.Add(Boundary);
			}
		}
	}

	// Dependency inventory: refusal source for local variables, preview output for the rest.
	TArray<FCortexGraphTransferDependency> Dependencies;
	for (UEdGraphNode* Node : SelectedNodes)
	{
		CollectTransferDependencies(Blueprint, Node, Dependencies);
	}
	if (SourceGraph != DestinationGraph)
	{
		for (const FCortexGraphTransferDependency& Dependency : Dependencies)
		{
			if (Dependency.Kind != TEXT("local_variable")) continue;
			const FBPVariableDescription* const Target =
				FBlueprintEditorUtils::FindLocalVariable(Blueprint, DestinationGraph, FName(*Dependency.Member));
			if (!Target)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("selected node '%s' reads local variable '%s', which the destination graph does not declare; declare an identically named local variable in the destination graph or transfer a node without it"),
						*Dependency.NodeGuid, *Dependency.Member));
				return false;
			}
			if (LocalVariableTypeIdentity(Target->VarType) != Dependency.Type)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("the destination graph declares local variable '%s' with a different type than the source graph: '%s' vs '%s'"),
						*Dependency.Member, *LocalVariableTypeIdentity(Target->VarType), *Dependency.Type));
				return false;
			}
		}
	}

	// Supported node kinds and the deterministic identity map.
	TSet<FGuid> SelectionSet;
	for (const FGuid& Guid : Selection) SelectionSet.Add(Guid);
	TMap<FGuid, int32> SlotByGuid;
	for (int32 Index = 0; Index < Selection.Num(); ++Index) SlotByGuid.Add(Selection[Index], Index);
	TSet<FGuid> DestinationGuidSet;
	TArray<FGuid> DestinationGuids;
	for (int32 Index = 0; Index < SelectedNodes.Num(); ++Index)
	{
		UEdGraphNode* const Node = SelectedNodes[Index];
		FString Reason;
		if (!ClassifyTransferNode(DestinationGraph, Node, Reason))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("selected node '%s' is a %s and cannot be transferred: %s"),
					*Node->NodeGuid.ToString(), *Node->GetClass()->GetName(), *Reason));
			return false;
		}
		const FGuid DestinationGuid = Op == TransferMoveOp
			? Node->NodeGuid
			: FCortexGraphPatchOps::DeriveNodeGuid(PatchId, Node->NodeGuid.ToString());
		if (!DestinationGuid.IsValid())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("selected node '%s' has no derivable destination identity"), *Node->NodeGuid.ToString()));
			return false;
		}
		DestinationGuids.Add(DestinationGuid);
		DestinationGuidSet.Add(DestinationGuid);
	}

	auto CollectCrossing = [&SelectionSet](const TArray<UEdGraphNode*>& Nodes, TArray<FTransferLink>& OutInternal, TArray<FTransferLink>& OutCrossing)
	{
		TSet<FString> Keys;
		for (UEdGraphNode* Node : Nodes)
		{
			InventoryTransferLinks(Node, SelectionSet, OutInternal, OutCrossing, Keys);
		}
		OutInternal.Sort([](const FTransferLink& A, const FTransferLink& B) { return A.Key() < B.Key(); });
		OutCrossing.Sort([](const FTransferLink& A, const FTransferLink& B) { return A.Key() < B.Key(); });
	};

	TArray<FTransferLink> InternalLinks;
	TArray<FTransferLink> CrossingLinks;
	CollectCrossing(SelectedNodes, InternalLinks, CrossingLinks);

	// Asset-wide destination identity set, before anything is planned as a mutation.
	bool bAnyDestinationPresent = false;
	if (AbsentGuids.Num() == 0)
	{
		for (int32 Index = 0; Index < SelectedNodes.Num(); ++Index)
		{
			const FGuid DestinationGuid = DestinationGuids[Index];
			TArray<UEdGraph*> Owners;
			int32 MatchCount = 0;
			TransferGraphsOwningGuid(Blueprint, DestinationGuid, Owners, MatchCount);
			if (Owners.Num() == 0)
			{
				continue;
			}
			bAnyDestinationPresent = true;
			if (Owners.Num() > 1)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("the planned destination identity '%s' is owned by %d graphs ('%s', '%s'); a cross-graph duplicate identity is not a state this operation may repair"),
						*DestinationGuid.ToString(), Owners.Num(),
						*Owners[0]->GraphGuid.ToString(), *Owners[1]->GraphGuid.ToString()));
				return false;
			}
			if (MatchCount > Owners.Num())
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("the planned destination identity '%s' is already owned by more than one graph; the asset is not in a state this operation may repair"),
						*DestinationGuid.ToString()));
				return false;
			}
			if (Owners[0] != DestinationGraph
				&& !(Op == TransferMoveOp && Owners[0] == SourceGraph))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("the planned destination identity '%s' already exists in graph '%s'"),
						*DestinationGuid.ToString(), *Owners[0]->GraphGuid.ToString()));
				return false;
			}
		}
	}
	if (Op == TransferCopyOp && !bAnyDestinationPresent && SelectedNodes.Num() > 0)
	{
		for (const FGuid& SourceGuid : Selection)
		{
			if (DestinationGuidSet.Contains(SourceGuid))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("the derived copy identity of node '%s' collides with a selected source identity"), *SourceGuid.ToString()));
				return false;
			}
		}
	}

	const bool bAbsentSelectionReplay = AbsentGuids.Num() > 0 && AbsentGuids.Num() == Selection.Num();

	// Boundary coverage: every crossing edge needs exactly one entry, and every entry needs exactly
	// one crossing edge. A replay names a selection that is already absent, so it has no crossing
	// inventory left to cover; its entries are validated against the recorded intent instead.
	TMap<FString, TArray<FTransferLink>> CrossingByFrom;
	for (const FTransferLink& Link : CrossingLinks)
	{
		CrossingByFrom.FindOrAdd(TransferEndpointKey(Link.FromNode, Link.FromPin)).Add(Link);
	}
	TMap<FString, TArray<const FPendingBoundary*>> EntriesByFrom;
	TSet<FString> EntryKeys;
	for (const FPendingBoundary& Boundary : Pending)
	{
		UEdGraphNode* SourceNode = FindNodeByGuidInGraph(SourceGraph, Boundary.FromNode);
		if (!SourceNode && bAbsentSelectionReplay)
		{
			// A replay's selected nodes already live in the destination graph, so the entry's source
			// endpoint resolves there while the recorded intent is the selection itself.
			SourceNode = FindNodeByGuidInGraph(DestinationGraph, Boundary.FromNode);
		}
		if (!SourceNode || !SelectionSet.Contains(Boundary.FromNode))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("migration.boundary names source node '%s', which is not a selected node of the source graph"),
					*Boundary.FromNode.ToString()));
			return false;
		}
		UEdGraphPin* const SourcePin = SourceNode->FindPin(FName(*Boundary.FromPin));
		if (!SourcePin)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("migration.boundary names pin '%s' that selected node '%s' does not have"),
					*Boundary.FromPin, *Boundary.FromNode.ToString()));
			return false;
		}
		const FString FromKey = TransferEndpointKey(Boundary.FromNode, FName(*Boundary.FromPin));
		if (bAbsentSelectionReplay)
		{
			// The selected nodes are already in the destination graph, so the recorded intent is the
			// selection itself: an entry must name a selected node and one of its pins.
			UEdGraphNode* const DestinationNode = FindNodeByGuidInGraph(DestinationGraph, Boundary.FromNode);
			if (!SelectionSet.Contains(Boundary.FromNode) || !DestinationNode
				|| !DestinationNode->FindPin(FName(*Boundary.FromPin)))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("migration.boundary names '%s.%s', which is not a selected pin of this replay's destination identity set"),
						*Boundary.FromNode.ToString(), *Boundary.FromPin));
				return false;
			}
			EntriesByFrom.FindOrAdd(FromKey).Add(&Boundary);
			continue;
		}
		if (!CrossingByFrom.Contains(FromKey))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("migration.boundary names '%s.%s', which is not a crossing edge of the selection"),
					*Boundary.FromNode.ToString(), *Boundary.FromPin));
			return false;
		}
		const FString EntryKey = FromKey + TEXT("->") + TransferEndpointKey(Boundary.ToNode, FName(*Boundary.ToPin));
		if (EntryKeys.Contains(EntryKey))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("migration.boundary maps '%s.%s' onto '%s.%s' more than once"),
					*Boundary.FromNode.ToString(), *Boundary.FromPin, *Boundary.ToNode.ToString(), *Boundary.ToPin));
			return false;
		}
		EntryKeys.Add(EntryKey);
		EntriesByFrom.FindOrAdd(FromKey).Add(&Boundary);
	}
	for (const TPair<FString, TArray<FTransferLink>>& Pair : bAbsentSelectionReplay ? TMap<FString, TArray<FTransferLink>>() : CrossingByFrom)
	{
		const TArray<const FPendingBoundary*>* const Entries = EntriesByFrom.Find(Pair.Key);
		if (!Entries || Entries->Num() < Pair.Value.Num())
		{
			const FTransferLink& Uncovered = Pair.Value.Num() > 0 ? Pair.Value[0] : FTransferLink();
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("crossing edge '%s.%s' -> '%s.%s' is not covered by a boundary entry; every crossing edge must be mapped explicitly, never dropped"),
					*Uncovered.FromNode.ToString(), *Uncovered.FromPin.ToString(),
					*Uncovered.ToNode.ToString(), *Uncovered.ToPin.ToString()));
			return false;
		}
		if (Entries->Num() > Pair.Value.Num())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("migration.boundary duplicates the mapping of crossing edge '%s', which has %d crossing link(s)"),
					*Pair.Key, Pair.Value.Num()));
			return false;
		}
	}
	for (const TPair<FString, TArray<const FPendingBoundary*>>& Pair : EntriesByFrom)
	{
		if (bAbsentSelectionReplay || CrossingByFrom.Contains(Pair.Key)) continue;
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("migration.boundary names '%s', which is not a crossing edge of the selection"), *Pair.Key));
		return false;
	}

	if (AbsentGuids.Num() == Selection.Num())
	{
		// Move replay: the request names nodes that are already absent from the source graph. The
		// authority of an accepted replay is the ruling's structural condition, so the plan is built
		// from the live destination state and the whole request performs no work.
		if (Op != TransferMoveOp)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("selected node '%s' is not a node of the named source graph; a copy never removes its source nodes"),
					*Selection[0].ToString()));
			return false;
		}
		for (int32 Index = 0; Index < Selection.Num(); ++Index)
		{
			const FGuid SourceGuid = Selection[Index];
			TArray<UEdGraph*> Owners;
			int32 MatchCount = 0;
			TransferGraphsOwningGuid(Blueprint, SourceGuid, Owners, MatchCount);
			if (Owners.Num() == 0)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("move replay cannot be proven: selected node '%s' exists in neither the source nor the destination graph"),
						*SourceGuid.ToString()));
				return false;
			}
			if (Owners.Num() > 1 || MatchCount > 1 || Owners[0] != DestinationGraph)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("move replay cannot be proven: node '%s' is absent from the source graph but lives in graph '%s' instead of the named destination graph"),
						*SourceGuid.ToString(), *Owners[0]->GraphGuid.ToString()));
				return false;
			}
		}
		TMap<FGuid, int32> DestinationSlots;
		for (int32 Index = 0; Index < Selection.Num(); ++Index) DestinationSlots.Add(Selection[Index], Index);
		for (int32 Index = 0; Index < Selection.Num(); ++Index)
		{
			UEdGraphNode* const Node = FindNodeByGuidInGraph(DestinationGraph, Selection[Index]);
			if (!Node)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("move replay cannot be proven: node '%s' does not resolve in the destination graph"), *Selection[Index].ToString()));
				return false;
			}
			FCortexGraphTransferNode Entry;
			Entry.SourceGuid = Selection[Index].ToString();
			Entry.DestinationGuid = Selection[Index].ToString();
			Entry.ClassPath = Node->GetClass()->GetPathName();
			Entry.Symbol = TransferNodeSymbol(Node);
			Entry.PosX = Node->NodePosX;
			Entry.PosY = Node->NodePosY;
			Entry.Comment = Node->NodeComment;
			Entry.bCommentBubblePinned = Node->bCommentBubblePinned;
			Entry.bCommentBubbleVisible = Node->bCommentBubbleVisible;
			Entry.EnabledState = static_cast<int32>(Node->GetDesiredEnabledState());
			Entry.bUserSetEnabledState = Node->HasUserSetTheEnabledState();
			Entry.bForceDisplayAsDisabled = Node->IsDisplayAsDisabledForced();
			Entry.Pins = TransferNodePins(Node, DestinationSlots);
			OutPlan.Nodes.Add(MoveTemp(Entry));
		}
		for (const FTransferLink& Link : InternalLinks)
		{
			FCortexGraphTransferEdge Edge;
			Edge.FromGuid = Link.FromNode.ToString();
			Edge.FromPin = Link.FromPin.ToString();
			Edge.ToGuid = Link.ToNode.ToString();
			Edge.ToPin = Link.ToPin.ToString();
			OutPlan.InternalEdges.Add(MoveTemp(Edge));
		}
		for (const FPendingBoundary& Boundary : Pending)
		{
			FCortexGraphTransferBoundary Entry;
			Entry.SourceGuid = Boundary.FromNode.ToString();
			Entry.SourcePin = Boundary.FromPin;
			Entry.DestinationNodeGuid = Boundary.ToNode.ToString();
			Entry.DestinationPin = Boundary.ToPin;
			OutPlan.Boundary.Add(MoveTemp(Entry));
		}
		OutPlan.Dependencies = Dependencies;
		OutPlan.bReused = true;
		OutPlan.RemovalSet = TArray<FString>();
		{
			FCortexGraphTransferPreservation SourceContract;
			SourceContract.Label = TEXT("source_graph");
			SourceContract.GraphGuid = SourceGraphGuid.ToString();
			SourceContract.Capture = CapturePreservation(Blueprint, SourceGraph, Selection);
			OutPlan.Preservations.Add(MoveTemp(SourceContract));
			FCortexGraphTransferPreservation DestinationContract;
			DestinationContract.Label = TEXT("destination_graph");
			DestinationContract.GraphGuid = DestinationGraphGuid.ToString();
			for (const FGuid& Guid : Selection) DestinationContract.ExcludedGuids.Add(Guid.ToString());
			DestinationContract.Capture = CapturePreservation(Blueprint, DestinationGraph, Selection);
			OutPlan.Preservations.Add(MoveTemp(DestinationContract));
		}
		bOutReused = true;
		return true;
	}

	// Fresh transfer: the plan is complete before the caller may mutate anything.
	for (int32 Index = 0; Index < SelectedNodes.Num(); ++Index)
	{
		UEdGraphNode* const Node = SelectedNodes[Index];
		FCortexGraphTransferNode Entry;
		Entry.SourceGuid = Node->NodeGuid.ToString();
		Entry.DestinationGuid = DestinationGuids[Index].ToString();
		Entry.ClassPath = Node->GetClass()->GetPathName();
		Entry.Symbol = TransferNodeSymbol(Node);
		Entry.PosX = Node->NodePosX;
		Entry.PosY = Node->NodePosY;
		Entry.Comment = Node->NodeComment;
		Entry.bCommentBubblePinned = Node->bCommentBubblePinned;
		Entry.bCommentBubbleVisible = Node->bCommentBubbleVisible;
		Entry.EnabledState = static_cast<int32>(Node->GetDesiredEnabledState());
		Entry.bUserSetEnabledState = Node->HasUserSetTheEnabledState();
		Entry.bForceDisplayAsDisabled = Node->IsDisplayAsDisabledForced();
		Entry.Pins = TransferNodePins(Node, SlotByGuid);
		OutPlan.Nodes.Add(MoveTemp(Entry));
	}
	for (const FTransferLink& Link : InternalLinks)
	{
		FCortexGraphTransferEdge Edge;
		Edge.FromGuid = Link.FromNode.ToString();
		Edge.FromPin = Link.FromPin.ToString();
		Edge.ToGuid = Link.ToNode.ToString();
		Edge.ToPin = Link.ToPin.ToString();
		OutPlan.InternalEdges.Add(MoveTemp(Edge));
	}
	for (const FPendingBoundary& Boundary : Pending)
	{
		UEdGraphNode* const SourceNode = FindNodeByGuidInGraph(SourceGraph, Boundary.FromNode);
		UEdGraphPin* const SourcePin = SourceNode ? SourceNode->FindPin(FName(*Boundary.FromPin)) : nullptr;
		UEdGraphNode* const DestinationNode = FindNodeByGuidInGraph(DestinationGraph, Boundary.ToNode);
		UEdGraphPin* const DestinationPin = DestinationNode ? DestinationNode->FindPin(FName(*Boundary.ToPin)) : nullptr;
		if (!SourcePin || !DestinationPin)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				TEXT("a planned boundary pin no longer resolves"));
			return false;
		}
		if (SourcePin->Direction == DestinationPin->Direction)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::TypeMismatch,
				FString::Printf(TEXT("boundary mapping '%s.%s' -> '%s.%s' connects two %s pins; a link needs one output and one input"),
					*Boundary.FromNode.ToString(), *Boundary.FromPin, *Boundary.ToNode.ToString(), *Boundary.ToPin,
					SourcePin->Direction == EGPD_Output ? TEXT("output") : TEXT("input")));
			return false;
		}
		UEdGraphPin* const OutputPin = SourcePin->Direction == EGPD_Output ? SourcePin : DestinationPin;
		UEdGraphPin* const InputPin = SourcePin->Direction == EGPD_Output ? DestinationPin : SourcePin;
		if (!DestinationGraph->GetSchema()
			|| !DestinationGraph->GetSchema()->ArePinsCompatible(OutputPin, InputPin, Blueprint->SkeletonGeneratedClass.Get(), false))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::TypeMismatch,
				FString::Printf(TEXT("boundary mapping '%s.%s' -> '%s.%s' is not type-compatible through the destination schema: '%s' vs '%s'"),
					*Boundary.FromNode.ToString(), *Boundary.FromPin, *Boundary.ToNode.ToString(), *Boundary.ToPin,
					*FCortexGraphPatchOps::CanonicalPinSignature(FCortexGraphPatchOps::MakePinSignatureDescriptor(*OutputPin)),
					*FCortexGraphPatchOps::CanonicalPinSignature(FCortexGraphPatchOps::MakePinSignatureDescriptor(*InputPin))));
			return false;
		}
		if (DestinationPin->SubPins.Num() > 0)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::TypeMismatch,
				FString::Printf(TEXT("boundary destination pin '%s.%s' is an expanded struct pin, which the transfer cannot prove"),
					*Boundary.ToNode.ToString(), *Boundary.ToPin));
			return false;
		}
		// The transferred clone's pin is new, so only the named destination pin can already be taken.
		// An input endpoint must be free, except when a repetition finds exactly the link this
		// request realized the first time, which is the expected state of an idempotent replay.
		if (DestinationPin->Direction == EGPD_Input && DestinationPin->LinkedTo.Num() > 0)
		{
			bool bAlreadyRealized = false;
			const FCortexGraphTransferNode* const Planned = OutPlan.Nodes.FindByPredicate(
				[&Boundary](const FCortexGraphTransferNode& Candidate)
				{
					return Candidate.SourceGuid == Boundary.FromNode.ToString();
				});
			if (Planned)
			{
				FGuid PlannedGuid;
				FGuid::Parse(Planned->DestinationGuid, PlannedGuid);
				UEdGraphNode* const PlannedNode = FindNodeByGuidInGraph(DestinationGraph, PlannedGuid);
				UEdGraphPin* const PlannedPin = PlannedNode ? PlannedNode->FindPin(FName(*Boundary.FromPin)) : nullptr;
				// The pin must carry exactly the intended link: an extra peer is not a replay, and a
				// request may only claim no work when the realized mapping is the planned mapping.
				bAlreadyRealized = PlannedPin
					&& DestinationPin->LinkedTo.Num() == 1
					&& DestinationPin->LinkedTo.Contains(PlannedPin);
			}
			if (!bAlreadyRealized)
			{
				// A taken destination pin whose link is not exactly the planned mapping is a conflict
				// with live state, not a type mismatch: the request cannot own that pin.
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
					FString::Printf(TEXT("boundary destination pin '%s.%s' already carries %d link(s) that are not exactly the planned mapping, so the mapping cannot own it"),
						*Boundary.ToNode.ToString(), *Boundary.ToPin, DestinationPin->LinkedTo.Num()));
				return false;
			}
		}
		FCortexGraphTransferBoundary Entry;
		Entry.SourceGuid = Boundary.FromNode.ToString();
		Entry.SourcePin = Boundary.FromPin;
		const TArray<FTransferLink>* const Covered = CrossingByFrom.Find(TransferEndpointKey(Boundary.FromNode, FName(*Boundary.FromPin)));
		const TArray<const FPendingBoundary*>* const Entries = EntriesByFrom.Find(TransferEndpointKey(Boundary.FromNode, FName(*Boundary.FromPin)));
		if (Covered && Entries)
		{
			const int32 Position = Entries->IndexOfByKey(&Boundary);
			if (Covered->IsValidIndex(Position))
			{
				Entry.SourceFarGuid = (*Covered)[Position].ToNode.ToString();
				Entry.SourceFarPin = (*Covered)[Position].ToPin.ToString();
			}
		}
		Entry.DestinationNodeGuid = Boundary.ToNode.ToString();
		Entry.DestinationPin = Boundary.ToPin;
		OutPlan.Boundary.Add(MoveTemp(Entry));
	}
	OutPlan.Dependencies = Dependencies;
	if (Op == TransferMoveOp)
	{
		for (const FGuid& Guid : Selection) OutPlan.RemovalSet.Add(Guid.ToString());
	}

	// Preservation contracts of both graphs, captured before the first mutation.
	{
		FCortexGraphTransferPreservation SourceContract;
		SourceContract.Label = TEXT("source_graph");
		SourceContract.GraphGuid = SourceGraphGuid.ToString();
		// A move excludes the identities it removes; a copy excludes the identities it creates, so a
		// copy inside the source graph proves the pre-existing source body instead of the copy.
		for (const FGuid& Guid : (Op == TransferMoveOp ? Selection : DestinationGuids))
		{
			SourceContract.ExcludedGuids.Add(Guid.ToString());
		}
		SourceContract.Capture = CapturePreservation(Blueprint, SourceGraph,
			Op == TransferMoveOp ? Selection : DestinationGuids);
		OutPlan.Preservations.Add(MoveTemp(SourceContract));

		FCortexGraphTransferPreservation DestinationContract;
		DestinationContract.Label = TEXT("destination_graph");
		DestinationContract.GraphGuid = DestinationGraphGuid.ToString();
		for (const FGuid& Guid : DestinationGuids) DestinationContract.ExcludedGuids.Add(Guid.ToString());
		// Captured with the planned identities excluded, so the contract reads the pre-existing
		// destination body identically before the transfer and during a repetition of it.
		DestinationContract.Capture = CapturePreservation(Blueprint, DestinationGraph, DestinationGuids);
		OutPlan.Preservations.Add(MoveTemp(DestinationContract));
	}

	// Idempotent replay reconciliation of a repeated copy: the deterministic destination identities
	// make the destination identity set complete, so the whole planned intent is proven before the
	// request may claim no work is needed.
	if (Op == TransferCopyOp && bAnyDestinationPresent)
	{
		TMap<FGuid, int32> DestinationSlots;
		for (int32 Index = 0; Index < DestinationGuids.Num(); ++Index) DestinationSlots.Add(DestinationGuids[Index], Index);
		TArray<FString> Missing;
		for (int32 Index = 0; Index < DestinationGuids.Num(); ++Index)
		{
			if (!FindNodeByGuidInGraph(DestinationGraph, DestinationGuids[Index]))
			{
				Missing.Add(DestinationGuids[Index].ToString());
			}
		}
		if (Missing.Num() > 0)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("the destination identity set of this copy is partial: %d of %d planned destination nodes already exist; missing: %s. A partial identity set is refused instead of repaired."),
					DestinationGuids.Num() - Missing.Num(), DestinationGuids.Num(), *FString::Join(Missing, TEXT(", "))));
			return false;
		}
		FString ReuseFailure;
		FCortexGraphMigrationTransferPlan ReusePlan = OutPlan;
		ReusePlan.bReused = true;
		if (!VerifyTransferAgainstNative(Blueprint, ReusePlan, ReuseFailure))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("the existing destination identity set does not match the planned transfer: %s"), *ReuseFailure));
			return false;
		}
		OutPlan.bReused = true;
		bOutReused = true;
	}
	return true;
}

bool FCortexGraphMigrationOps::RegisterTransferNodes(
	UBlueprint* Blueprint,
	const FCortexGraphMigrationTransferPlan& Plan,
	TArray<FGuid>& OutCreatedGuids,
	FCortexCommandResult& OutError)
{
	OutCreatedGuids.Reset();
	OutError = FCortexCommandResult();
	if (!Blueprint)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, TEXT("Blueprint is null"));
		return false;
	}
	FGuid SourceGraphGuid;
	FGuid DestinationGraphGuid;
	FGuid::Parse(Plan.SourceGraphGuid, SourceGraphGuid);
	FGuid::Parse(Plan.DestinationGraphGuid, DestinationGraphGuid);
	UEdGraph* const SourceGraph = FindGraphByGuid(Blueprint, SourceGraphGuid);
	UEdGraph* const DestinationGraph = FindGraphByGuid(Blueprint, DestinationGraphGuid);
	if (!SourceGraph || !DestinationGraph)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("the planned transfer graphs did not re-resolve after the final guard"));
		return false;
	}
	TSet<UObject*> ExportSet;
	for (const FCortexGraphTransferNode& Node : Plan.Nodes)
	{
		FGuid SourceGuid;
		FGuid::Parse(Node.SourceGuid, SourceGuid);
		UEdGraphNode* const SourceNode = FindNodeByGuidInGraph(SourceGraph, SourceGuid);
		if (!SourceNode)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("the planned source node '%s' no longer resolves in the source graph"), *Node.SourceGuid));
			return false;
		}
		ExportSet.Add(SourceNode);
	}
	if (ExportSet.Num() != Plan.Nodes.Num())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("the planned selection no longer resolves to distinct source nodes"));
		return false;
	}

	// The engine node-clone path the editor itself uses for graph copy/paste: the exported text is
	// the node's own serialization, so every authored field the node owns travels with it and links
	// inside the exported set resolve against the imported nodes instead of the originals.
	FString ExportedText;
	FEdGraphUtilities::ExportNodesToText(ExportSet, ExportedText);
	if (ExportedText.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("the engine clone path produced no text for the selected nodes"));
		return false;
	}
	OutCreatedGuids.Reset();
	TSet<UEdGraphNode*> ImportedNodes;
	DestinationGraph->Modify();
	// The engine text factory gives an imported object the name the text carries. When the
	// destination graph already owns an object of that name, the engine renames the *existing*
	// object to keep the imported name, which would change authored state the request never
	// selected. The pre-import names are snapshotted so every pre-existing node is restored.
	TMap<FGuid, FName> PreExistingNames;
	for (UEdGraphNode* Existing : DestinationGraph->Nodes)
	{
		if (Existing && Existing->NodeGuid.IsValid())
		{
			PreExistingNames.Add(Existing->NodeGuid, Existing->GetFName());
		}
	}
	FEdGraphUtilities::ImportNodesFromText(DestinationGraph, ExportedText, ImportedNodes);

	// Everything the import registered is identified and handed to the caller before any check can
	// fail, so a partially accepted import is always reversible by the journal.
	TMap<UEdGraphNode*, const FCortexGraphTransferNode*> PlannedByRegistered;
	for (UEdGraphNode* Registered : ImportedNodes)
	{
		if (!Registered) continue;
		const FCortexGraphTransferNode* const Planned = Plan.Nodes.FindByPredicate(
			[Registered](const FCortexGraphTransferNode& Candidate)
			{
				return Candidate.SourceGuid == Registered->NodeGuid.ToString();
			});
		PlannedByRegistered.Add(Registered, Planned);
		if (Planned)
		{
			FGuid DestinationGuid;
			if (FGuid::Parse(Planned->DestinationGuid, DestinationGuid) && DestinationGuid.IsValid())
			{
				Registered->Modify();
				Registered->NodeGuid = DestinationGuid;
			}
		}
		OutCreatedGuids.AddUnique(Registered->NodeGuid);
	}

	// The clones release the names they took first, so each pre-existing node can be restored to
	// exactly the name it had before the transfer. This runs before any validation can return, so a
	// refused registration never leaves a renamed pre-existing node behind.
	for (UEdGraphNode* Registered : ImportedNodes)
	{
		if (!Registered) continue;
		Registered->Modify();
		const FName Unique = MakeUniqueObjectName(DestinationGraph, Registered->GetClass(), FName(TEXT("CortexTransferNode")));
		Registered->Rename(*Unique.ToString(), DestinationGraph, REN_DontCreateRedirectors);
	}
	for (UEdGraphNode* Existing : DestinationGraph->Nodes)
	{
		if (!Existing || !Existing->NodeGuid.IsValid()) continue;
		if (PlannedByRegistered.Contains(Existing)) continue;
		const FName* const Original = PreExistingNames.Find(Existing->NodeGuid);
		if (Original && Existing->GetFName() != *Original)
		{
			Existing->Modify();
			Existing->Rename(*Original->ToString(), DestinationGraph, REN_DontCreateRedirectors);
		}
	}

	if (ImportedNodes.Num() != Plan.Nodes.Num())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("the engine clone path produced %d node(s) for %d planned node(s); a silently substituted or dropped node is never accepted"),
				ImportedNodes.Num(), Plan.Nodes.Num()));
		return false;
	}
	TSet<FGuid> Matched;
	for (UEdGraphNode* Imported : ImportedNodes)
	{
		if (!Imported || !Imported->GetGraph() || Imported->GetGraph() != DestinationGraph)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				TEXT("the engine clone path registered a node outside the destination graph"));
			return false;
		}
		const FCortexGraphTransferNode* const Planned = PlannedByRegistered.FindRef(Imported);
		if (!Planned)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("the engine clone path registered unplanned node '%s'"), *Imported->NodeGuid.ToString()));
			return false;
		}
		if (Imported->GetClass()->GetPathName() != Planned->ClassPath)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("the engine clone path substituted node class '%s' for planned class '%s'; a substitution is never accepted"),
					*Imported->GetClass()->GetPathName(), *Planned->ClassPath));
			return false;
		}
		FGuid DestinationGuid;
		if (!FGuid::Parse(Planned->DestinationGuid, DestinationGuid) || !DestinationGuid.IsValid())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				TEXT("a planned transfer identity is invalid"));
			return false;
		}
		Imported->Modify();
		Imported->NodeGuid = DestinationGuid;
		Imported->NodePosX = Planned->PosX;
		Imported->NodePosY = Planned->PosY;
		Imported->NodeComment = Planned->Comment;
		Imported->bCommentBubblePinned = Planned->bCommentBubblePinned;
		Imported->bCommentBubbleVisible = Planned->bCommentBubbleVisible;
		Imported->SetEnabledState(static_cast<ENodeEnabledState>(Planned->EnabledState), Planned->bUserSetEnabledState);
		Imported->SetForceDisplayAsDisabled(Planned->bForceDisplayAsDisabled);
		if (SourceGraph != DestinationGraph)
		{
			// A local variable belongs to the graph scope, so a transferred node must reference the
			// destination graph's declaration. PlanTransfer already proved the declaration exists
			// with the same name and type; here the reference is re-scoped onto it.
			if (UK2Node_Variable* const Variable = Cast<UK2Node_Variable>(Imported))
			{
				if (Variable->VariableReference.IsLocalScope())
				{
					const FName MemberName = Variable->VariableReference.GetMemberName();
					const FBPVariableDescription* const Target =
						FBlueprintEditorUtils::FindLocalVariable(Blueprint, DestinationGraph, MemberName);
					if (!Target)
					{
						OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
							FString::Printf(TEXT("the destination graph no longer declares local variable '%s'"), *MemberName.ToString()));
						return false;
					}
					Variable->VariableReference.SetLocalMember(MemberName, DestinationGraph->GetName(), Target->VarGuid);
				}
			}
		}
		Matched.Add(DestinationGuid);
	}
	if (Matched.Num() != Plan.Nodes.Num())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("the engine clone path did not register every planned destination identity"));
		return false;
	}
	OutCreatedGuids.Sort([](const FGuid& A, const FGuid& B) { return A.ToString() < B.ToString(); });
	DestinationGraph->NotifyGraphChanged();
	return true;
}
bool FCortexGraphMigrationOps::WireTransfer(
	UBlueprint* Blueprint,
	const FCortexGraphMigrationTransferPlan& Plan,
	TArray<FCortexGraphMigrationLink>& OutCreatedLinks,
	FCortexCommandResult& OutError)
{
	OutCreatedLinks.Reset();
	OutError = FCortexCommandResult();
	if (!Blueprint)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, TEXT("Blueprint is null"));
		return false;
	}
	FGuid DestinationGraphGuid;
	FGuid::Parse(Plan.DestinationGraphGuid, DestinationGraphGuid);
	UEdGraph* const DestinationGraph = FindGraphByGuid(Blueprint, DestinationGraphGuid);
	if (!DestinationGraph)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("the planned destination graph did not re-resolve after the final guard"));
		return false;
	}
	const UEdGraphSchema* const Schema = DestinationGraph->GetSchema();
	if (!Schema)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("the destination graph has no schema"));
		return false;
	}
	TMap<FString, FString> DestinationBySource;
	for (const FCortexGraphTransferNode& Node : Plan.Nodes)
	{
		DestinationBySource.Add(Node.SourceGuid, Node.DestinationGuid);
	}

	/** Realizes one link through the schema and journals it by durable identity. */
	auto Connect = [&](UEdGraphPin* Pin, UEdGraphPin* FarPin, const FString& Context) -> bool
	{
		if (!Pin || !FarPin)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("%s did not re-resolve its pins after apply"), *Context));
			return false;
		}
		if (Pin->LinkedTo.Contains(FarPin)) return true;
		UEdGraphPin* const OutputPin = Pin->Direction == EGPD_Output ? Pin : FarPin;
		UEdGraphPin* const InputPin = Pin->Direction == EGPD_Output ? FarPin : Pin;
		const FPinConnectionResponse Response = Schema->CanCreateConnection(OutputPin, InputPin);
		if (Response.Response != CONNECT_RESPONSE_MAKE || !Schema->TryCreateConnection(OutputPin, InputPin))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("%s is no longer safe: %s"), *Context, *Response.Message.ToString()));
			return false;
		}
		OutCreatedLinks.Add({ OutputPin->GetOwningNode()->NodeGuid, OutputPin->PinName,
			InputPin->GetOwningNode()->NodeGuid, InputPin->PinName });
		return true;
	};

	DestinationGraph->Modify();
	for (const FCortexGraphTransferEdge& Edge : Plan.InternalEdges)
	{
		// Internal edges are planned on source identities, so both endpoints are mapped through the
		// transfer's identity map: a copy resolves its derived identities, a move resolves its own.
		FGuid MappedFrom;
		FGuid MappedTo;
		const FString* const FromText = DestinationBySource.Find(Edge.FromGuid);
		const FString* const ToText = DestinationBySource.Find(Edge.ToGuid);
		if (!FromText || !ToText
			|| !FGuid::Parse(*FromText, MappedFrom) || !FGuid::Parse(*ToText, MappedTo))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("planned internal edge '%s.%s' -> '%s.%s' names an unplanned source node"),
					*Edge.FromGuid, *Edge.FromPin, *Edge.ToGuid, *Edge.ToPin));
			return false;
		}
		UEdGraphNode* const FromNode = FindNodeByGuidInGraph(DestinationGraph, MappedFrom);
		UEdGraphNode* const ToNode = FindNodeByGuidInGraph(DestinationGraph, MappedTo);
		const FString Context = FString::Printf(TEXT("planned internal edge '%s.%s' -> '%s.%s'"),
			*Edge.FromGuid, *Edge.FromPin, *Edge.ToGuid, *Edge.ToPin);
		if (!FromNode || !ToNode)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("%s did not re-resolve its nodes after apply"), *Context));
			return false;
		}
		if (!Connect(FromNode->FindPin(FName(*Edge.FromPin)), ToNode->FindPin(FName(*Edge.ToPin)), Context)) return false;
	}
	for (const FCortexGraphTransferBoundary& Boundary : Plan.Boundary)
	{
		const FString* const DestinationGuid = DestinationBySource.Find(Boundary.SourceGuid);
		if (!DestinationGuid)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("boundary mapping '%s.%s' names an unplanned source node"), *Boundary.SourceGuid, *Boundary.SourcePin));
			return false;
		}
		FGuid TransferredGuid;
		FGuid::Parse(*DestinationGuid, TransferredGuid);
		FGuid BoundaryNodeGuid;
		FGuid::Parse(Boundary.DestinationNodeGuid, BoundaryNodeGuid);
		UEdGraphNode* const Transferred = FindNodeByGuidInGraph(DestinationGraph, TransferredGuid);
		UEdGraphNode* const BoundaryNode = FindNodeByGuidInGraph(DestinationGraph, BoundaryNodeGuid);
		const FString Context = FString::Printf(TEXT("boundary mapping '%s.%s' -> '%s.%s'"),
			*Boundary.SourceGuid, *Boundary.SourcePin, *Boundary.DestinationNodeGuid, *Boundary.DestinationPin);
		if (!Transferred || !BoundaryNode)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("%s did not re-resolve its nodes after apply"), *Context));
			return false;
		}
		if (!Connect(Transferred->FindPin(FName(*Boundary.SourcePin)), BoundaryNode->FindPin(FName(*Boundary.DestinationPin)), Context)) return false;
	}
	DestinationGraph->NotifyGraphChanged();
	return true;
}

bool FCortexGraphMigrationOps::VerifyPreservationContracts(
	UBlueprint* Blueprint,
	const TArray<FCortexGraphTransferPreservation>& Contracts,
	FString& OutFailure)
{
	OutFailure.Reset();
	for (const FCortexGraphTransferPreservation& Contract : Contracts)
	{
		FGuid GraphGuid;
		FGuid::Parse(Contract.GraphGuid, GraphGuid);
		UEdGraph* const Graph = FindGraphByGuid(Blueprint, GraphGuid);
		if (!Graph)
		{
			OutFailure = FString::Printf(TEXT("the '%s' preservation graph '%s' did not re-resolve"), *Contract.Label, *Contract.GraphGuid);
			return false;
		}
		TArray<FGuid> Excluded;
		for (const FString& GuidText : Contract.ExcludedGuids)
		{
			FGuid ExcludedGuid;
			if (FGuid::Parse(GuidText, ExcludedGuid)) Excluded.Add(ExcludedGuid);
		}
		if (CapturePreservation(Blueprint, Graph, Excluded) != Contract.Capture)
		{
			OutFailure = FString::Printf(TEXT("the '%s' preservation contract of graph '%s' does not match live native state"),
				*Contract.Label, *Contract.GraphGuid);
			return false;
		}
	}
	return true;
}

TSharedPtr<FJsonObject> FCortexGraphMigrationOps::MakeTransferInventory(const TSharedPtr<FJsonObject>& TransferPlanJson)
{
	if (!TransferPlanJson.IsValid()) return nullptr;
	FCortexGraphMigrationTransferPlan Plan;
	FCortexCommandResult PlanError;
	if (!FCortexGraphMigrationTransferPlan::FromJson(TransferPlanJson, Plan, PlanError)) return nullptr;

	TSharedPtr<FJsonObject> Inventory = MakeShared<FJsonObject>();
	Inventory->SetStringField(TEXT("operation"), Plan.Op);
	Inventory->SetStringField(TEXT("source_graph_guid"), Plan.SourceGraphGuid);
	Inventory->SetStringField(TEXT("destination_graph_guid"), Plan.DestinationGraphGuid);
	Inventory->SetNumberField(TEXT("node_count"), Plan.Nodes.Num());

	TArray<FString> Crossing;
	for (const FCortexGraphTransferBoundary& Entry : Plan.Boundary)
	{
		Crossing.Add(FString::Printf(TEXT("%s.%s -> %s.%s"), *Entry.SourceGuid, *Entry.SourcePin,
			*Entry.SourceFarGuid, *Entry.SourceFarPin));
	}
	TArray<FString> Boundary;
	for (const FCortexGraphTransferBoundary& Entry : Plan.Boundary)
	{
		Boundary.Add(FString::Printf(TEXT("%s.%s -> %s.%s"), *Entry.SourceGuid, *Entry.SourcePin,
			*Entry.DestinationNodeGuid, *Entry.DestinationPin));
	}
	TArray<FString> Dependencies;
	for (const FCortexGraphTransferDependency& Dependency : Plan.Dependencies)
	{
		Dependencies.Add(FString::Printf(TEXT("%s %s@%s (%s)"), *Dependency.Kind, *Dependency.Member,
			*Dependency.OwnerClass, *Dependency.NodeGuid));
	}
	TArray<FString> Removal = Plan.RemovalSet;
	TArray<FString> InternalEdges;
	for (const FCortexGraphTransferEdge& Edge : Plan.InternalEdges)
	{
		InternalEdges.Add(FString::Printf(TEXT("%s.%s -> %s.%s"), *Edge.FromGuid, *Edge.FromPin, *Edge.ToGuid, *Edge.ToPin));
	}
	FCortexGraphPatchOps::TrimDiagnostics(Crossing);
	FCortexGraphPatchOps::TrimDiagnostics(Boundary);
	FCortexGraphPatchOps::TrimDiagnostics(Dependencies);
	FCortexGraphPatchOps::TrimDiagnostics(Removal);
	FCortexGraphPatchOps::TrimDiagnostics(InternalEdges);

	auto ToValues = [](const TArray<FString>& Lines)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Line : Lines) Values.Add(MakeShared<FJsonValueString>(Line));
		return Values;
	};
	Inventory->SetArrayField(TEXT("crossing_edges"), ToValues(Crossing));
	Inventory->SetArrayField(TEXT("boundary"), ToValues(Boundary));
	Inventory->SetArrayField(TEXT("dependencies"), ToValues(Dependencies));
	Inventory->SetArrayField(TEXT("removal_set"), ToValues(Removal));
	Inventory->SetArrayField(TEXT("internal_edges"), ToValues(InternalEdges));
	return Inventory;
}

bool FCortexGraphMigrationOps::VerifyTransferAgainstNative(
	UBlueprint* Blueprint,
	const FCortexGraphMigrationTransferPlan& Plan,
	FString& OutFailure)
{
	OutFailure.Reset();
	if (!Blueprint)
	{
		OutFailure = TEXT("the blueprint is null");
		return false;
	}
	FGuid SourceGraphGuid;
	FGuid DestinationGraphGuid;
	FGuid::Parse(Plan.SourceGraphGuid, SourceGraphGuid);
	FGuid::Parse(Plan.DestinationGraphGuid, DestinationGraphGuid);
	UEdGraph* const SourceGraph = FindGraphByGuid(Blueprint, SourceGraphGuid);
	UEdGraph* const DestinationGraph = FindGraphByGuid(Blueprint, DestinationGraphGuid);
	if (!SourceGraph || !DestinationGraph)
	{
		OutFailure = TEXT("the planned transfer graphs did not re-resolve");
		return false;
	}
	TMap<FGuid, int32> DestinationSlots;
	TMap<FGuid, int32> SourceSlots;
	TSet<FGuid> RemovalSet;
	for (int32 Index = 0; Index < Plan.Nodes.Num(); ++Index)
	{
		FGuid SourceGuid;
		FGuid DestinationGuid;
		FGuid::Parse(Plan.Nodes[Index].SourceGuid, SourceGuid);
		FGuid::Parse(Plan.Nodes[Index].DestinationGuid, DestinationGuid);
		if (!SourceGuid.IsValid() || !DestinationGuid.IsValid())
		{
			OutFailure = TEXT("the prepared transfer plan carries an invalid node identity");
			return false;
		}
		SourceSlots.Add(SourceGuid, Index);
		DestinationSlots.Add(DestinationGuid, Index);
		RemovalSet.Add(SourceGuid);
	}

	for (const FCortexGraphTransferNode& Planned : Plan.Nodes)
	{
		FGuid DestinationGuid;
		FGuid::Parse(Planned.DestinationGuid, DestinationGuid);
		UEdGraphNode* const Node = FindNodeByGuidInGraph(DestinationGraph, DestinationGuid);
		if (!Node)
		{
			OutFailure = FString::Printf(TEXT("the planned destination node '%s' is missing from the destination graph"), *Planned.DestinationGuid);
			return false;
		}
		TArray<UEdGraph*> Owners;
		int32 MatchCount = 0;
		TransferGraphsOwningGuid(Blueprint, DestinationGuid, Owners, MatchCount);
		if (Owners.Num() != 1 || MatchCount != 1 || Owners[0] != DestinationGraph)
		{
			OutFailure = FString::Printf(TEXT("the destination identity '%s' is owned by %d graph(s) and %d node(s) instead of exactly the destination graph"),
				*Planned.DestinationGuid, Owners.Num(), MatchCount);
			return false;
		}
		if (Node->GetClass()->GetPathName() != Planned.ClassPath)
		{
			OutFailure = FString::Printf(TEXT("the destination node '%s' has class '%s' instead of the planned '%s'"),
				*Planned.DestinationGuid, *Node->GetClass()->GetPathName(), *Planned.ClassPath);
			return false;
		}
		if (TransferNodeSymbol(Node) != Planned.Symbol)
		{
			OutFailure = FString::Printf(TEXT("the destination node '%s' resolves symbol '%s' instead of the planned '%s'"),
				*Planned.DestinationGuid, *TransferNodeSymbol(Node), *Planned.Symbol);
			return false;
		}
		if (TransferNodePresentation(*Node) != TransferNodePresentation(Planned))
		{
			OutFailure = FString::Printf(TEXT("the destination node '%s' does not carry the planned authored presentation"), *Planned.DestinationGuid);
			return false;
		}
		if (TransferNodePins(Node, DestinationSlots) != Planned.Pins)
		{
			OutFailure = FString::Printf(TEXT("the destination node '%s' does not carry the planned authored pin state or internal edges"), *Planned.DestinationGuid);
			return false;
		}
	}

	if (Plan.IsMove())
	{
		for (const FCortexGraphTransferNode& Planned : Plan.Nodes)
		{
			FGuid SourceGuid;
			FGuid::Parse(Planned.SourceGuid, SourceGuid);
			UEdGraphNode* const Stale = FindNodeByGuidInGraph(SourceGraph, SourceGuid);
			if (Stale)
			{
				OutFailure = FString::Printf(TEXT("the moved node '%s' is still present in the source graph"), *Planned.SourceGuid);
				return false;
			}
		}
		// No dangling link may survive the removal: every pin of the source body must be free of a
		// link to a removed identity.
		for (UEdGraphNode* Body : SourceGraph->Nodes)
		{
			if (!Body) continue;
			for (UEdGraphPin* Pin : Body->Pins)
			{
				if (!Pin) continue;
				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					UEdGraphNode* const Far = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
					if (Far && RemovalSet.Contains(Far->NodeGuid))
					{
						OutFailure = FString::Printf(TEXT("a dangling link from '%s.%s' reaches the removed node '%s'"),
							*Body->NodeGuid.ToString(), *Pin->PinName.ToString(), *Far->NodeGuid.ToString());
						return false;
					}
				}
			}
		}
	}
	else
	{
		// A copy never mutates its source: the named source nodes must still carry exactly the
		// planned authored state.
		for (const FCortexGraphTransferNode& Planned : Plan.Nodes)
		{
			FGuid SourceGuid;
			FGuid::Parse(Planned.SourceGuid, SourceGuid);
			UEdGraphNode* const SourceNode = FindNodeByGuidInGraph(SourceGraph, SourceGuid);
			if (!SourceNode)
			{
				OutFailure = FString::Printf(TEXT("the source node '%s' the copy names no longer exists"), *Planned.SourceGuid);
				return false;
			}
			if (TransferNodeSymbol(SourceNode) != Planned.Symbol
				|| TransferNodePresentation(*SourceNode) != TransferNodePresentation(Planned)
				|| TransferNodePins(SourceNode, SourceSlots) != Planned.Pins)
			{
				OutFailure = FString::Printf(TEXT("the copy modified its source node '%s'"), *Planned.SourceGuid);
				return false;
			}
		}
	}

	for (const FCortexGraphTransferBoundary& Boundary : Plan.Boundary)
	{
		FGuid SourceGuid;
		FGuid TransferredGuid;
		FGuid BoundaryNodeGuid;
		if (!FGuid::Parse(Boundary.SourceGuid, SourceGuid) || !FGuid::Parse(Boundary.DestinationNodeGuid, BoundaryNodeGuid))
		{
			OutFailure = TEXT("a prepared boundary mapping carries an invalid identity");
			return false;
		}
		if (Plan.IsMove())
		{
			// A move preserves the documented identity, so the moved node is found by its own GUID.
			TransferredGuid = SourceGuid;
		}
		else
		{
			const FCortexGraphTransferNode* const Planned = Plan.Nodes.FindByPredicate(
				[&Boundary](const FCortexGraphTransferNode& Candidate) { return Candidate.SourceGuid == Boundary.SourceGuid; });
			if (!Planned || !FGuid::Parse(Planned->DestinationGuid, TransferredGuid))
			{
				OutFailure = FString::Printf(TEXT("the boundary mapping '%s.%s' names an unplanned source node"), *Boundary.SourceGuid, *Boundary.SourcePin);
				return false;
			}
		}
		UEdGraphNode* const Transferred = FindNodeByGuidInGraph(DestinationGraph, TransferredGuid);
		UEdGraphNode* const BoundaryNode = FindNodeByGuidInGraph(DestinationGraph, BoundaryNodeGuid);
		if (!Transferred || !BoundaryNode)
		{
			OutFailure = FString::Printf(TEXT("the boundary mapping '%s.%s' did not re-resolve its nodes"), *Boundary.SourceGuid, *Boundary.SourcePin);
			return false;
		}
		UEdGraphPin* const TransferredPin = Transferred->FindPin(FName(*Boundary.SourcePin));
		UEdGraphPin* const BoundaryPin = BoundaryNode->FindPin(FName(*Boundary.DestinationPin));
		if (!TransferredPin || !BoundaryPin || !TransferredPin->LinkedTo.Contains(BoundaryPin))
		{
			OutFailure = FString::Printf(TEXT("the boundary mapping '%s.%s' -> '%s.%s' is not realized in the destination graph"),
				*Boundary.SourceGuid, *Boundary.SourcePin, *Boundary.DestinationNodeGuid, *Boundary.DestinationPin);
			return false;
		}
		// The complete live link set is the oracle, not containment: the input side of the pair must
		// carry exactly the planned link, so an extra peer is a divergence instead of a replay.
		UEdGraphPin* const BoundaryInputSide = TransferredPin->Direction == EGPD_Input ? TransferredPin : BoundaryPin;
		if (BoundaryInputSide->LinkedTo.Num() != 1 || !BoundaryInputSide->LinkedTo.Contains(
			BoundaryInputSide == TransferredPin ? BoundaryPin : TransferredPin))
		{
			OutFailure = FString::Printf(TEXT("the boundary mapping '%s.%s' -> '%s.%s' leaves its input pin with %d link(s) instead of exactly the planned one"),
				*Boundary.SourceGuid, *Boundary.SourcePin, *Boundary.DestinationNodeGuid, *Boundary.DestinationPin,
				BoundaryInputSide->LinkedTo.Num());
			return false;
		}
	}
	return VerifyPreservationContracts(Blueprint, Plan.Preservations, OutFailure);
}

// ===========================================================================
// Execution-island pruning: `prune_island`
// ===========================================================================

namespace
{
/**
 * Graph-wide scan budget of one prune preflight. The value is the published `max_scanned_nodes`
 * bound, and the budgeted unit is a *distinct node* — exactly what that published bound means — so a
 * legitimate asset below the node limit can never become unprunable because its island has many links.
 */
constexpr int32 PruneScanNodeLimit = FCortexGraphPatchOps::MaxScannedNodes;

const TCHAR* const PruneOp = TEXT("prune_island");

#if WITH_AUTOMATION_TESTS
/** Test-only prune readback fault: fails exactly the named check of the prune readback verifier. */
FName PruneReadbackFaultForTesting = NAME_None;
#endif

bool ShouldInjectPruneReadbackFault(const FName Check)
{
#if WITH_AUTOMATION_TESTS
	return PruneReadbackFaultForTesting == Check;
#else
	(void)Check;
	return false;
#endif
}

/**
 * One accumulating bounded scan. Every distinct node identity the scan examines is charged once
 * against the published node bound; examined links are counted for the refusal report only, because
 * the traversal is already bounded by its visited sets and the link inventory of one node is finite.
 */
struct FPruneScan
{
	TSet<FGuid> Counted;
	int32 Nodes = 0;
	int32 Links = 0;
	bool bExhausted = false;

	void Visit(const FGuid& NodeGuid)
	{
		if (Counted.Contains(NodeGuid)) return;
		Counted.Add(NodeGuid);
		++Nodes;
		if (Nodes > PruneScanNodeLimit) bExhausted = true;
	}

	void ExamineLink() { ++Links; }
};

/** The refusal of an exhausted scan: the observed counts and the limit, never a partial partition. */
FCortexCommandResult MakePruneScanRefusal(const FPruneScan& Scan)
{
	FCortexCommandResult Error = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
		FString::Printf(TEXT("the graph-wide scan of this prune island exceeded max_scanned_nodes=%d after %d node(s) (%d link(s) examined); the island partition is incomplete, so the request is refused instead of pruning a set that was never proven"),
			PruneScanNodeLimit, Scan.Nodes, Scan.Links));
	Error.ErrorDetails = MakeShared<FJsonObject>();
	Error.ErrorDetails->SetNumberField(TEXT("scan_limit"), PruneScanNodeLimit);
	Error.ErrorDetails->SetNumberField(TEXT("scanned_nodes"), Scan.Nodes);
	Error.ErrorDetails->SetNumberField(TEXT("scanned_links"), Scan.Links);
	Error.ErrorDetails->SetBoolField(TEXT("complete"), false);
	return Error;
}

/**
 * Engine class names a prune refuses to remove, with the reason the partition reports afterwards. The
 * walk is over the node's own inheritance chain, so a subclass of a refused class is refused under
 * the same reason instead of slipping through the ownership proof.
 */
const TMap<FString, FString>& PruneBlockedClassNames()
{
	static const TMap<FString, FString> Blocked = {
		{ TEXT("K2Node_Tunnel"), TEXT("tunnel node whose pins are owned by a bound graph") },
		{ TEXT("K2Node_TunnelBase"), TEXT("tunnel base whose pins are owned by a bound graph") },
		{ TEXT("K2Node_TunnelBoundary"), TEXT("tunnel boundary whose pins are owned by a bound graph") },
		{ TEXT("K2Node_Knot"), TEXT("reroute knot whose pin type is inferred from its links") },
		{ TEXT("K2Node_Timeline"), TEXT("timeline node with its own bound state") },
		{ TEXT("K2Node_EditablePinBase"), TEXT("editable-pin terminator owned by its graph lifecycle") },
		{ TEXT("K2Node_BaseMCDelegate"), TEXT("delegate node owned by its delegate declaration") },
		{ TEXT("K2Node_CallDelegate"), TEXT("delegate node owned by its delegate declaration") },
		{ TEXT("K2Node_CreateDelegate"), TEXT("delegate node owned by its delegate declaration") },
		{ TEXT("K2Node_AssignDelegate"), TEXT("delegate node owned by its delegate declaration") },
		{ TEXT("K2Node_DelegateSet"), TEXT("delegate node owned by its delegate declaration") },
		{ TEXT("K2Node_BaseAsyncTask"), TEXT("latent async task node") },
		{ TEXT("K2Node_AsyncAction"), TEXT("latent async action node") },
		{ TEXT("K2Node_LatentGameplayTaskCall"), TEXT("latent gameplay task call") },
	};
	return Blocked;
}

/**
 * True when the node terminates a graph and is therefore owned by its graph lifecycle, so a prune
 * always retains it. The reason is reported in the partition.
 */
bool PruneTerminatorReason(const UEdGraphNode* Node, FString& OutReason)
{
	OutReason.Reset();
	if (!Node) return false;
	if (Node->IsA<UK2Node_Event>())
	{
		OutReason = TEXT("an event terminator owned by its graph lifecycle");
		return true;
	}
	if (Node->IsA<UK2Node_FunctionEntry>())
	{
		OutReason = TEXT("a function entry terminator owned by its graph lifecycle");
		return true;
	}
	if (Node->IsA<UK2Node_FunctionResult>())
	{
		OutReason = TEXT("a function result terminator owned by its graph lifecycle");
		return true;
	}
	return false;
}

/** True when the node may open a prune island: an event or function entry terminator with exec pins. */
bool IsPruneEntryNode(const UEdGraphNode* Node, FString& OutReason)
{
	OutReason.Reset();
	if (!Node || !Node->GetClass())
	{
		OutReason = TEXT("the node is null");
		return false;
	}
	if (Node->IsA<UK2Node_Event>() || Node->IsA<UK2Node_FunctionEntry>())
	{
		if (Node->Pins.ContainsByPredicate(
			[](const UEdGraphPin* Pin)
			{
				return Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
			}))
		{
			return true;
		}
		OutReason = FString::Printf(TEXT("node class '%s' has no execution output pin"), *Node->GetClass()->GetName());
		return false;
	}
	OutReason = FString::Printf(TEXT("node class '%s' is not an event or function entry terminator"), *Node->GetClass()->GetName());
	return false;
}

/** True when the ownership proof fails for the node; the reason names why it is never removable. */
bool PruneBlockedReason(UEdGraphNode* Node, FString& OutReason)
{
	OutReason.Reset();
	if (!Node || !Node->GetClass() || !Node->NodeGuid.IsValid())
	{
		OutReason = TEXT("invalid node identity");
		return true;
	}
	if (Node->GetSubGraphs().Num() > 0)
	{
		OutReason = TEXT("the node owns a bound subgraph, which a removal would orphan");
		return true;
	}
	if (const UK2Node_CallFunction* const Call = Cast<UK2Node_CallFunction>(Node))
	{
		if (Call->IsLatentFunction())
		{
			OutReason = FString::Printf(TEXT("latent function call '%s'"), *Call->FunctionReference.GetMemberName().ToString());
			return true;
		}
	}
	for (UClass* Class = Node->GetClass(); Class && Class != UObject::StaticClass(); Class = Class->GetSuperClass())
	{
		const FString ClassName = Class->GetName();
		if (ClassName.Contains(TEXT("Latent")))
		{
			OutReason = FString::Printf(TEXT("latent node class '%s'"), *Node->GetClass()->GetName());
			return true;
		}
		if (const FString* const Reason = PruneBlockedClassNames().Find(ClassName))
		{
			OutReason = *Reason;
			return true;
		}
	}
	if (!Node->IsA<UK2Node>() && !Node->IsA<UEdGraphNode_Comment>())
	{
		OutReason = FString::Printf(TEXT("non-K2 graph node class '%s'"), *Node->GetClass()->GetName());
		return true;
	}
	return false;
}

FCortexGraphPruneNode MakePruneNode(const UEdGraphNode* Node, const FString& Reason)
{
	FCortexGraphPruneNode Entry;
	Entry.NodeGuid = Node->NodeGuid.ToString();
	Entry.ClassPath = Node->GetClass()->GetPathName();
	Entry.Reason = Reason;
	return Entry;
}

/** Canonical key of one directed link endpoint, so a planned edge is de-duplicated once. */
FString PruneEndpointKey(const FGuid& NodeGuid, const FName PinName)
{
	return FString::Printf(TEXT("%s.%s"), *NodeGuid.ToString(), *PinName.ToString());
}

/** The complete partition of one prune island plus the scan work that produced it. */
struct FPrunePartition
{
	/** Uniquely owned by this island: no consumer is retained. */
	TArray<FGuid> Removable;
	/** Island nodes retained because a retained consumer uses them. */
	TArray<FCortexGraphPruneNode> Shared;
	/** Island nodes retained because their ownership or traversal cannot be proven. */
	TArray<FCortexGraphPruneNode> Blocked;
	/** Every link of the removable set that reaches a retained node: the approved boundary edges. */
	TArray<FCortexGraphPruneEdge> ExternalEdges;
	FPruneScan Scan;
};

/**
 * Computes the partition of the island of Entry inside Graph, with a bounded scan over the whole
 * asset and the island.
 *
 * The island is the execution-reachable set of the entry (exec outputs only, so branch, sequence and
 * loop bodies follow their own exec pins exactly as the trace helpers model them) plus the reverse
 * data-producer closure feeding it. A visited set bounds every traversal, so a data cycle inside the
 * island terminates instead of being refused. The entry and every graph terminator are retained by
 * kind, a node whose ownership cannot be proven is blocked, and a candidate is retained as shared as
 * soon as one of its consumers is not itself removable. That rule is also what makes a removal
 * unable to orphan a retained consumer's link: a consumer outside the removable set is exactly what
 * makes its producer shared, and the readback proves the retained link sets afterwards.
 */
bool ComputeOwnedIslandPartition(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TArray<UEdGraphNode*>& SeedEntries,
	const bool bSelectedEntriesRemovable,
	FPrunePartition& OutPartition,
	FCortexCommandResult& OutError)
{
	OutPartition = FPrunePartition();
	OutError = FCortexCommandResult();
	if (!Blueprint || !Graph || SeedEntries.IsEmpty()
		|| SeedEntries.ContainsByPredicate([](const UEdGraphNode* Node) { return Node == nullptr; }))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("the ownership partition requires a blueprint, its graph and at least one seed entry"));
		return false;
	}

	TMap<FGuid, int32> GuidOwners;
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Candidate : Graphs)
	{
		if (!Candidate) continue;
		for (UEdGraphNode* Node : Candidate->Nodes)
		{
			if (!Node) continue;
			OutPartition.Scan.Visit(Node->NodeGuid);
			if (OutPartition.Scan.bExhausted)
			{
				OutError = MakePruneScanRefusal(OutPartition.Scan);
				return false;
			}
			++GuidOwners.FindOrAdd(Node->NodeGuid);
		}
	}

	TSet<FGuid> Selected;
	TSet<FGuid> Island;
	TArray<UEdGraphNode*> Worklist;
	for (UEdGraphNode* Entry : SeedEntries)
	{
		if (!Entry || Entry->GetGraph() != Graph || Island.Contains(Entry->NodeGuid)) continue;
		Selected.Add(Entry->NodeGuid);
		Island.Add(Entry->NodeGuid);
		Worklist.Add(Entry);
	}
	if (Worklist.Num() != SeedEntries.Num())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("the ownership partition seed entries must be distinct nodes of the named graph"));
		return false;
	}

	// Union execution reachability of every selected entry.
	for (int32 Index = 0; Index < Worklist.Num(); ++Index)
	{
		UEdGraphNode* Current = Worklist[Index];
		for (UEdGraphPin* Pin : Current->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				OutPartition.Scan.ExamineLink();
				UEdGraphNode* Next = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
				if (!Next || Island.Contains(Next->NodeGuid)) continue;
				Island.Add(Next->NodeGuid);
				Worklist.Add(Next);
				OutPartition.Scan.Visit(Next->NodeGuid);
				if (OutPartition.Scan.bExhausted)
				{
					OutError = MakePruneScanRefusal(OutPartition.Scan);
					return false;
				}
			}
		}
	}
	// Reverse data-producer closure over the union.
	for (int32 Index = 0; Index < Worklist.Num(); ++Index)
	{
		UEdGraphNode* Current = Worklist[Index];
		for (UEdGraphPin* Pin : Current->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				OutPartition.Scan.ExamineLink();
				UEdGraphNode* Producer = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
				if (!Producer || Island.Contains(Producer->NodeGuid)) continue;
				Island.Add(Producer->NodeGuid);
				Worklist.Add(Producer);
				OutPartition.Scan.Visit(Producer->NodeGuid);
				if (OutPartition.Scan.bExhausted)
				{
					OutError = MakePruneScanRefusal(OutPartition.Scan);
					return false;
				}
			}
		}
	}

	TArray<UEdGraphNode*> IslandNodes;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Island.Contains(Node->NodeGuid)) IslandNodes.Add(Node);
	}
	IslandNodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
	{
		return A.NodeGuid.ToString() < B.NodeGuid.ToString();
	});

	TSet<FGuid> Candidates;
	TMap<FGuid, FString> RetainedReasons;
	TMap<FGuid, FString> BlockedReasons;
	for (UEdGraphNode* Node : IslandNodes)
	{
		FString Reason;
		const bool bSelectedEntry = Selected.Contains(Node->NodeGuid);
		if (bSelectedEntry && !bSelectedEntriesRemovable)
		{
			RetainedReasons.Add(Node->NodeGuid, TEXT("the entry terminator whose island is pruned"));
			continue;
		}
		const int32* OwnerCount = GuidOwners.Find(Node->NodeGuid);
		if (!Node->NodeGuid.IsValid() || !OwnerCount || *OwnerCount != 1)
		{
			BlockedReasons.Add(Node->NodeGuid, FString::Printf(
				TEXT("the node identity is owned by %d node(s) in this asset instead of exactly one"),
				OwnerCount ? *OwnerCount : 0));
			continue;
		}
		if (!bSelectedEntry && PruneTerminatorReason(Node, Reason))
		{
			RetainedReasons.Add(Node->NodeGuid, Reason);
			continue;
		}
		if (!bSelectedEntry && PruneBlockedReason(Node, Reason))
		{
			BlockedReasons.Add(Node->NodeGuid, Reason);
			continue;
		}
		Candidates.Add(Node->NodeGuid);
	}

	TArray<UEdGraphNode*> RetainedWorklist;
	for (UEdGraphNode* Node : IslandNodes)
	{
		if (!Candidates.Contains(Node->NodeGuid)) RetainedWorklist.Add(Node);
	}
	for (UEdGraphNode* Node : IslandNodes)
	{
		if (!Candidates.Contains(Node->NodeGuid)) continue;
		FString ConsumingReason;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output) continue;
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				OutPartition.Scan.ExamineLink();
				UEdGraphNode* Consumer = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
				if (Consumer && Candidates.Contains(Consumer->NodeGuid)) continue;
				ConsumingReason = Consumer
					? (bSelectedEntriesRemovable
						? FString::Printf(TEXT("consumed by retained consumer '%s'"), *Consumer->NodeGuid.ToString())
						: FString::Printf(TEXT("consumed by node '%s', which the approved set does not remove"), *Consumer->NodeGuid.ToString()))
					: TEXT("consumed by a link whose far endpoint does not resolve");
				break;
			}
			if (!ConsumingReason.IsEmpty()) break;
		}
		if (ConsumingReason.IsEmpty()) continue;
		Candidates.Remove(Node->NodeGuid);
		(bSelectedEntriesRemovable && Selected.Contains(Node->NodeGuid) ? BlockedReasons : RetainedReasons)
			.Add(Node->NodeGuid, ConsumingReason);
		RetainedWorklist.Add(Node);
	}
	for (int32 Index = 0; Index < RetainedWorklist.Num(); ++Index)
	{
		UEdGraphNode* RetainedNode = RetainedWorklist[Index];
		for (UEdGraphPin* Pin : RetainedNode->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input) continue;
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				OutPartition.Scan.ExamineLink();
				UEdGraphNode* Producer = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
				if (!Producer || !Candidates.Contains(Producer->NodeGuid)) continue;
				Candidates.Remove(Producer->NodeGuid);
				const FString Reason = bSelectedEntriesRemovable
					? FString::Printf(TEXT("produced for retained consumer '%s'"), *RetainedNode->NodeGuid.ToString())
					: FString::Printf(TEXT("produced for the retained node '%s'"), *RetainedNode->NodeGuid.ToString());
				(bSelectedEntriesRemovable && Selected.Contains(Producer->NodeGuid) ? BlockedReasons : RetainedReasons)
					.Add(Producer->NodeGuid, Reason);
				RetainedWorklist.Add(Producer);
			}
		}
	}

	TSet<FString> EdgeKeys;
	for (UEdGraphNode* Node : IslandNodes)
	{
		if (!Node || !Candidates.Contains(Node->NodeGuid)) continue;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->SubPins.Num() > 0) continue;
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				OutPartition.Scan.ExamineLink();
				if (OutPartition.Scan.bExhausted)
				{
					OutError = MakePruneScanRefusal(OutPartition.Scan);
					return false;
				}
				UEdGraphNode* Far = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
				if (!Far || Candidates.Contains(Far->NodeGuid)) continue;
				UEdGraphPin* OutputPin = Pin->Direction == EGPD_Output ? Pin : LinkedPin;
				UEdGraphPin* InputPin = Pin->Direction == EGPD_Output ? LinkedPin : Pin;
				FCortexGraphPruneEdge Edge;
				Edge.FromGuid = OutputPin->GetOwningNode()->NodeGuid.ToString();
				Edge.FromPin = OutputPin->PinName.ToString();
				Edge.ToGuid = InputPin->GetOwningNode()->NodeGuid.ToString();
				Edge.ToPin = InputPin->PinName.ToString();
				const FString Key = PruneEndpointKey(OutputPin->GetOwningNode()->NodeGuid, OutputPin->PinName)
					+ TEXT("->") + PruneEndpointKey(InputPin->GetOwningNode()->NodeGuid, InputPin->PinName);
				if (EdgeKeys.Contains(Key)) continue;
				EdgeKeys.Add(Key);
				OutPartition.ExternalEdges.Add(MoveTemp(Edge));
			}
		}
	}
	OutPartition.ExternalEdges.Sort([](const FCortexGraphPruneEdge& A, const FCortexGraphPruneEdge& B)
	{
		return (A.FromGuid + TEXT(".") + A.FromPin + TEXT("->") + A.ToGuid + TEXT(".") + A.ToPin)
			< (B.FromGuid + TEXT(".") + B.FromPin + TEXT("->") + B.ToGuid + TEXT(".") + B.ToPin);
	});
	for (UEdGraphNode* Node : IslandNodes)
	{
		if (Candidates.Contains(Node->NodeGuid)) OutPartition.Removable.Add(Node->NodeGuid);
		else if (const FString* Blocked = BlockedReasons.Find(Node->NodeGuid))
			OutPartition.Blocked.Add(MakePruneNode(Node, *Blocked));
		else
		{
			const FString* Retained = RetainedReasons.Find(Node->NodeGuid);
			OutPartition.Shared.Add(MakePruneNode(Node, Retained ? *Retained : FString(TEXT("retained"))));
		}
	}
	OutPartition.Removable.Sort([](const FGuid& A, const FGuid& B) { return A.ToString() < B.ToString(); });
	return true;
}

bool ComputePrunePartition(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	UEdGraphNode* Entry,
	FPrunePartition& OutPartition,
	FCortexCommandResult& OutError)
{
	return ComputeOwnedIslandPartition(Blueprint, Graph, { Entry }, false, OutPartition, OutError);
}


TArray<FString> PruneGuidText(const TArray<FGuid>& Guids)
{
	TArray<FString> Text;
	for (const FGuid& Guid : Guids) Text.Add(Guid.ToString());
	Text.Sort();
	return Text;
}
}

TSharedPtr<FJsonObject> FCortexGraphMigrationPrunePlan::ToJson() const
{
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("op"), Op);
	Out->SetStringField(TEXT("graph_guid"), GraphGuid);
	Out->SetStringField(TEXT("subgraph_path"), SubgraphPath);
	Out->SetStringField(TEXT("entry_node_guid"), EntryNodeGuid);
	Out->SetBoolField(TEXT("complete"), bComplete);
	Out->SetBoolField(TEXT("awaiting_approval"), bAwaitingApproval);
	Out->SetBoolField(TEXT("reused"), bReused);
	Out->SetNumberField(TEXT("scanned_nodes"), ScannedNodes);
	Out->SetNumberField(TEXT("scanned_links"), ScannedLinks);

	auto ToStringValues = [](const TArray<FString>& Lines)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Line : Lines) Values.Add(MakeShared<FJsonValueString>(Line));
		return Values;
	};
	Out->SetArrayField(TEXT("approved_guids"), ToStringValues(ApprovedGuids));
	Out->SetArrayField(TEXT("removable_guids"), ToStringValues(RemovableGuids));

	auto ToPartitionValues = [](const TArray<FCortexGraphPruneNode>& Nodes)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FCortexGraphPruneNode& Node : Nodes)
		{
			TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
			NodeJson->SetStringField(TEXT("node_guid"), Node.NodeGuid);
			NodeJson->SetStringField(TEXT("class_path"), Node.ClassPath);
			NodeJson->SetStringField(TEXT("reason"), Node.Reason);
			Values.Add(MakeShared<FJsonValueObject>(NodeJson));
		}
		return Values;
	};
	Out->SetArrayField(TEXT("shared"), ToPartitionValues(Shared));
	Out->SetArrayField(TEXT("blocked"), ToPartitionValues(Blocked));

	TArray<TSharedPtr<FJsonValue>> EdgeValues;
	for (const FCortexGraphPruneEdge& Edge : ExternalEdges)
	{
		TSharedPtr<FJsonObject> EdgeJson = MakeShared<FJsonObject>();
		EdgeJson->SetStringField(TEXT("from_guid"), Edge.FromGuid);
		EdgeJson->SetStringField(TEXT("from_pin"), Edge.FromPin);
		EdgeJson->SetStringField(TEXT("to_guid"), Edge.ToGuid);
		EdgeJson->SetStringField(TEXT("to_pin"), Edge.ToPin);
		EdgeValues.Add(MakeShared<FJsonValueObject>(EdgeJson));
	}
	Out->SetArrayField(TEXT("external_edges"), EdgeValues);

	TSharedPtr<FJsonObject> ContractJson = MakeShared<FJsonObject>();
	ContractJson->SetStringField(TEXT("label"), Preservation.Label);
	ContractJson->SetStringField(TEXT("graph_guid"), Preservation.GraphGuid);
	ContractJson->SetArrayField(TEXT("excluded_guids"), ToStringValues(Preservation.ExcludedGuids));
	ContractJson->SetStringField(TEXT("capture"), Preservation.Capture);
	Out->SetObjectField(TEXT("preservation"), ContractJson);
	return Out;
}

bool FCortexGraphMigrationPrunePlan::FromJson(
	const TSharedPtr<FJsonObject>& Source,
	FCortexGraphMigrationPrunePlan& OutPlan,
	FCortexCommandResult& OutError)
{
	OutPlan = FCortexGraphMigrationPrunePlan();
	OutError = FCortexCommandResult();
	if (!Source.IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("prepared prune plan is missing"));
		return false;
	}
	const bool bRead = Source->TryGetStringField(TEXT("op"), OutPlan.Op)
		&& Source->TryGetStringField(TEXT("graph_guid"), OutPlan.GraphGuid)
		&& Source->TryGetStringField(TEXT("entry_node_guid"), OutPlan.EntryNodeGuid);
	if (!bRead || OutPlan.Op != PruneOp)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("prepared prune plan is incomplete or names an unsupported migration operation"));
		return false;
	}
	Source->TryGetStringField(TEXT("subgraph_path"), OutPlan.SubgraphPath);
	Source->TryGetBoolField(TEXT("complete"), OutPlan.bComplete);
	Source->TryGetBoolField(TEXT("awaiting_approval"), OutPlan.bAwaitingApproval);
	Source->TryGetBoolField(TEXT("reused"), OutPlan.bReused);
	int32 Number = 0;
	if (Source->TryGetNumberField(TEXT("scanned_nodes"), Number)) OutPlan.ScannedNodes = Number;
	if (Source->TryGetNumberField(TEXT("scanned_links"), Number)) OutPlan.ScannedLinks = Number;

	auto ReadStringValues = [](const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, TArray<FString>& OutValues)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(Field, Values) || !Values) return;
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Line;
			if (Value.IsValid() && Value->TryGetString(Line) && !Line.IsEmpty()) OutValues.Add(Line);
		}
	};
	ReadStringValues(Source, TEXT("approved_guids"), OutPlan.ApprovedGuids);
	ReadStringValues(Source, TEXT("removable_guids"), OutPlan.RemovableGuids);

	auto ReadPartition = [](const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, TArray<FCortexGraphPruneNode>& OutNodes)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(Field, Values) || !Values) return;
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> NodeJson = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!NodeJson.IsValid()) continue;
			FCortexGraphPruneNode Node;
			NodeJson->TryGetStringField(TEXT("node_guid"), Node.NodeGuid);
			NodeJson->TryGetStringField(TEXT("class_path"), Node.ClassPath);
			NodeJson->TryGetStringField(TEXT("reason"), Node.Reason);
			if (Node.NodeGuid.IsEmpty()) continue;
			OutNodes.Add(MoveTemp(Node));
		}
	};
	ReadPartition(Source, TEXT("shared"), OutPlan.Shared);
	ReadPartition(Source, TEXT("blocked"), OutPlan.Blocked);

	const TArray<TSharedPtr<FJsonValue>>* EdgeValues = nullptr;
	if (Source->TryGetArrayField(TEXT("external_edges"), EdgeValues) && EdgeValues)
	{
		for (const TSharedPtr<FJsonValue>& Value : *EdgeValues)
		{
			const TSharedPtr<FJsonObject> EdgeJson = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!EdgeJson.IsValid()) continue;
			FCortexGraphPruneEdge Edge;
			EdgeJson->TryGetStringField(TEXT("from_guid"), Edge.FromGuid);
			EdgeJson->TryGetStringField(TEXT("from_pin"), Edge.FromPin);
			EdgeJson->TryGetStringField(TEXT("to_guid"), Edge.ToGuid);
			EdgeJson->TryGetStringField(TEXT("to_pin"), Edge.ToPin);
			if (Edge.FromGuid.IsEmpty() || Edge.ToGuid.IsEmpty()) continue;
			OutPlan.ExternalEdges.Add(MoveTemp(Edge));
		}
	}

	const TSharedPtr<FJsonObject>* ContractPtr = nullptr;
	if (Source->TryGetObjectField(TEXT("preservation"), ContractPtr) && ContractPtr && ContractPtr->IsValid())
	{
		const TSharedPtr<FJsonObject>& ContractJson = *ContractPtr;
		ContractJson->TryGetStringField(TEXT("label"), OutPlan.Preservation.Label);
		ContractJson->TryGetStringField(TEXT("graph_guid"), OutPlan.Preservation.GraphGuid);
		ContractJson->TryGetStringField(TEXT("capture"), OutPlan.Preservation.Capture);
		ReadStringValues(ContractJson, TEXT("excluded_guids"), OutPlan.Preservation.ExcludedGuids);
	}
	if (OutPlan.GraphGuid.IsEmpty() || OutPlan.EntryNodeGuid.IsEmpty() || OutPlan.Preservation.Capture.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("prepared prune plan does not identify its graph, its entry or its preservation contract"));
		return false;
	}
	return true;
}

bool FCortexGraphMigrationOps::PlanPrune(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Migration,
	FCortexGraphMigrationPrunePlan& OutPlan,
	bool& bOutReused,
	FCortexCommandResult& OutError)
{
	OutPlan = FCortexGraphMigrationPrunePlan();
	OutError = FCortexCommandResult();
	bOutReused = false;
	if (!Blueprint)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, TEXT("Blueprint is null"));
		return false;
	}
	if (!Migration.IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("migration must be an object"));
		return false;
	}
	if (!FCortexGraphPatchOps::HasOnlyFields(Migration,
		{ TEXT("op"), TEXT("source"), TEXT("approved_node_guids") }, OutError, TEXT("migration")))
	{
		return false;
	}
	FString Op;
	if (!FCortexGraphPatchOps::ReadRequiredString(Migration, TEXT("op"), Op, OutError)) return false;
	if (Op != PruneOp)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::UnsupportedOperation,
			FString::Printf(TEXT("Unsupported migration operation '%s'; the published migration operations are replace_entry, copy_subgraph, move_subgraph and prune_island"), *Op));
		return false;
	}

	// Source graph and entry terminator.
	const TSharedPtr<FJsonObject>* SourcePtr = nullptr;
	if (!Migration->TryGetObjectField(TEXT("source"), SourcePtr) || !SourcePtr || !SourcePtr->IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("migration.source must be an object"));
		return false;
	}
	const TSharedPtr<FJsonObject>& Source = *SourcePtr;
	if (!FCortexGraphPatchOps::HasOnlyFields(Source,
		{ TEXT("graph_ref"), TEXT("entry_node_guid") }, OutError, TEXT("migration.source")))
	{
		return false;
	}
	FGuid GraphGuid;
	FString SubgraphPath;
	if (!ReadTransferGraphRef(Source, TEXT("migration.source"), GraphGuid, SubgraphPath, OutError)) return false;
	if (!FindGraphByGuid(Blueprint, GraphGuid))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
			FString::Printf(TEXT("migration.source.graph_ref names graph '%s', which is not a graph of this asset"),
				*GraphGuid.ToString()));
		return false;
	}
	UEdGraph* Graph = nullptr;
	if (!FCortexGraphPatchOps::ResolveGraphByGuid(Blueprint, GraphGuid, SubgraphPath, Graph, OutError)) return false;
	if (!ValidateTransferGraphKind(Blueprint, Source, GraphGuid, OutError)) return false;

	FGuid EntryGuid;
	if (!FCortexGraphPatchOps::ParseGuidField(Source, TEXT("entry_node_guid"), EntryGuid, OutError)) return false;
	UEdGraphNode* const Entry = FindNodeByGuidInGraph(Graph, EntryGuid);
	if (!Entry)
	{
		UEdGraphNode* const Elsewhere = FindNodeByGuid(Blueprint, EntryGuid);
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::NodeNotFound, Elsewhere
			? FString::Printf(TEXT("migration.source.entry_node_guid names node '%s', which is a node of graph '%s' instead of the named graph '%s'"),
				*EntryGuid.ToString(), *Elsewhere->GetGraph()->GraphGuid.ToString(), *GraphGuid.ToString())
			: FString::Printf(TEXT("migration.source.entry_node_guid names node '%s', which does not exist in the named graph '%s'"),
				*EntryGuid.ToString(), *GraphGuid.ToString()));
		return false;
	}
	FString EntryRefusalReason;
	if (!IsPruneEntryNode(Entry, EntryRefusalReason))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("the prune entry '%s' cannot open an island: %s"), *EntryGuid.ToString(), *EntryRefusalReason));
		return false;
	}

	// The approved set is the caller's echo of the published removable set. It may be absent, which
	// asks for the partition alone, but an explicitly empty set is refused: a prune that removes
	// nothing is not a prune.
	bool bHasApproved = false;
	TArray<FGuid> Approved;
	if (Migration->HasField(TEXT("approved_node_guids")))
	{
		const TArray<TSharedPtr<FJsonValue>>* ApprovedValues = nullptr;
		if (!Migration->TryGetArrayField(TEXT("approved_node_guids"), ApprovedValues) || !ApprovedValues)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				TEXT("migration.approved_node_guids must be an array of node GUIDs"));
			return false;
		}
		bHasApproved = true;
		if (ApprovedValues->Num() == 0)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
				TEXT("migration.approved_node_guids is empty, and an empty approved set is refused: a prune that removes nothing is not a prune. Omit the field to preview the partition, or approve at least one removable node"));
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *ApprovedValues)
		{
			FString GuidText;
			FGuid ApprovedGuid;
			if (!Value.IsValid() || !Value->TryGetString(GuidText) || !FGuid::Parse(GuidText, ApprovedGuid) || !ApprovedGuid.IsValid())
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					TEXT("migration.approved_node_guids entries must be node GUID strings"));
				return false;
			}
			if (Approved.Contains(ApprovedGuid))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					FString::Printf(TEXT("migration.approved_node_guids names node '%s' more than once"), *ApprovedGuid.ToString()));
				return false;
			}
			Approved.Add(ApprovedGuid);
		}
	}

	// Ownership of the entry identity: one asset, one owner.
	{
		TArray<UEdGraph*> Owners;
		int32 MatchCount = 0;
		TransferGraphsOwningGuid(Blueprint, EntryGuid, Owners, MatchCount);
		if (Owners.Num() != 1 || MatchCount != 1 || Owners[0] != Graph)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("the prune entry identity '%s' is owned by %d graph(s) and %d node(s) instead of exactly the named graph; a duplicate identity is not a state this operation may repair"),
					*EntryGuid.ToString(), Owners.Num(), MatchCount));
			return false;
		}
	}

	// Where each approved identity lives right now: present in the pruned graph, or absent from it.
	TArray<FGuid> Present;
	TArray<FString> Absent;
	for (const FGuid& ApprovedGuid : Approved)
	{
		if (FindNodeByGuidInGraph(Graph, ApprovedGuid))
		{
			Present.Add(ApprovedGuid);
			continue;
		}
		Absent.Add(ApprovedGuid.ToString());
	}
	// An approved identity that lives in another graph of this asset is an ownership conflict: this
	// request may not prune a node it does not name a graph for, and it is never a replay.
	for (const FString& AbsentText : Absent)
	{
		FGuid AbsentGuid;
		if (!FGuid::Parse(AbsentText, AbsentGuid)) continue;
		UEdGraphNode* const Elsewhere = FindNodeByGuid(Blueprint, AbsentGuid);
		if (!Elsewhere) continue;
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("migration.approved_node_guids names node '%s', which lives in graph '%s' instead of the pruned graph '%s'"),
				*AbsentText, *Elsewhere->GetGraph()->GraphGuid.ToString(), *GraphGuid.ToString()));
		return false;
	}

	OutPlan.Op = Op;
	OutPlan.GraphGuid = GraphGuid.ToString();
	OutPlan.SubgraphPath = SubgraphPath;
	OutPlan.EntryNodeGuid = EntryGuid.ToString();
	OutPlan.bAwaitingApproval = !bHasApproved;

	if (Absent.Num() == Approved.Num() && Approved.Num() > 0)
	{
		// The whole approved set is already absent: an idempotent replay. Nothing is computed about
		// an island that no longer exists; the live graph must instead prove the postcondition this
		// request asks for, so the operation reconciles by inspection instead of trusting the caller.
		OutPlan.ApprovedGuids = PruneGuidText(Approved);
		OutPlan.Preservation.Label = TEXT("graph");
		OutPlan.Preservation.GraphGuid = GraphGuid.ToString();
		OutPlan.Preservation.ExcludedGuids = OutPlan.ApprovedGuids;
		OutPlan.Preservation.Capture = CapturePreservation(Blueprint, Graph, Approved);
		FString Failure;
		if (!VerifyPruneAgainstNative(Blueprint, OutPlan, Failure))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("migration.approved_node_guids is already absent, but the live graph is not the postcondition of this request: %s"), *Failure));
			return false;
		}
		OutPlan.bReused = true;
		OutPlan.bComplete = true;
		bOutReused = true;
		return true;
	}
	if (Absent.Num() > 0)
	{
		TArray<FString> PresentText;
		for (const FGuid& Guid : Present) PresentText.Add(Guid.ToString());
		PresentText.Sort();
		Absent.Sort();
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			FString::Printf(TEXT("migration.approved_node_guids is a partial removal state: %d of %d approved node(s) are already absent (%s) while %d are still present in the pruned graph (%s); a partial island is refused instead of pruning a smaller set"),
				Absent.Num(), Approved.Num(), *FString::Join(Absent, TEXT(", ")),
				PresentText.Num(), *FString::Join(PresentText, TEXT(", "))));
		return false;
	}

	// The fresh island: the bounded graph-wide partition of the entry's island.
	FPrunePartition Partition;
	if (!ComputePrunePartition(Blueprint, Graph, Entry, Partition, OutError)) return false;
	OutPlan.ScannedNodes = Partition.Scan.Nodes;
	OutPlan.ScannedLinks = Partition.Scan.Links;
	OutPlan.bComplete = !Partition.Scan.bExhausted;
	OutPlan.Shared = Partition.Shared;
	OutPlan.Blocked = Partition.Blocked;
	OutPlan.ExternalEdges = Partition.ExternalEdges;
	OutPlan.RemovableGuids = PruneGuidText(Partition.Removable);

	if (bHasApproved)
	{
		// The approved set must match the freshly recomputed removable set exactly, in both
		// directions, before anything may be deleted: this is the operation's staleness guard.
		const TArray<FString> ApprovedText = PruneGuidText(Approved);
		TArray<FString> Missing;
		for (const FString& Removable : OutPlan.RemovableGuids)
		{
			if (!ApprovedText.Contains(Removable)) Missing.Add(Removable);
		}
		TArray<FString> Extra;
		for (const FString& ApprovedGuid : ApprovedText)
		{
			if (OutPlan.RemovableGuids.Contains(ApprovedGuid)) continue;
			FGuid ExtraGuid;
			FGuid::Parse(ApprovedGuid, ExtraGuid);
			const FCortexGraphPruneNode* const BlockedNode = Partition.Blocked.FindByPredicate(
				[&ApprovedGuid](const FCortexGraphPruneNode& Candidate) { return Candidate.NodeGuid == ApprovedGuid; });
			const FCortexGraphPruneNode* const SharedNode = Partition.Shared.FindByPredicate(
				[&ApprovedGuid](const FCortexGraphPruneNode& Candidate) { return Candidate.NodeGuid == ApprovedGuid; });
			if (BlockedNode)
			{
				Extra.Add(FString::Printf(TEXT("%s (%s)"), *ApprovedGuid, *BlockedNode->Reason));
			}
			else if (SharedNode)
			{
				Extra.Add(FString::Printf(TEXT("%s (%s)"), *ApprovedGuid, *SharedNode->Reason));
			}
			else
			{
				Extra.Add(FString::Printf(TEXT("%s (not part of this entry's island)"), *ApprovedGuid));
			}
		}
		if (Missing.Num() > 0 || Extra.Num() > 0)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("migration.approved_node_guids does not match the removable set of this island in the current state: %d removable node(s) are missing from the approved set (%s) and %d approved node(s) are not removable (%s); approve exactly the removable set this preview publishes"),
					Missing.Num(), Missing.Num() > 0 ? *FString::Join(Missing, TEXT(", ")) : TEXT("none"),
					Extra.Num(), Extra.Num() > 0 ? *FString::Join(Extra, TEXT(", ")) : TEXT("none")));
			return false;
		}
		OutPlan.ApprovedGuids = ApprovedText;
	}

	// The preservation contract of the retained body: captured before the first mutation, verified by
	// the readback and again after recovery. The removable set is excluded in both the preview and the
	// apply, so one contract describes both.
	OutPlan.Preservation.Label = TEXT("graph");
	OutPlan.Preservation.GraphGuid = GraphGuid.ToString();
	OutPlan.Preservation.ExcludedGuids = OutPlan.RemovableGuids;
	TArray<FGuid> ExcludedGuids;
	for (const FString& GuidText : OutPlan.RemovableGuids)
	{
		FGuid ExcludedGuid;
		if (FGuid::Parse(GuidText, ExcludedGuid)) ExcludedGuids.Add(ExcludedGuid);
	}
	OutPlan.Preservation.Capture = CapturePreservation(Blueprint, Graph, ExcludedGuids);
	if (OutPlan.Preservation.Capture.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("the preservation contract of the pruned graph could not be captured"));
		return false;
	}
	return true;
}

bool FCortexGraphMigrationOps::VerifyPruneAgainstNative(
	UBlueprint* Blueprint,
	const FCortexGraphMigrationPrunePlan& Plan,
	FString& OutFailure)
{
	OutFailure.Reset();
	if (!Blueprint)
	{
		OutFailure = TEXT("the blueprint is null");
		return false;
	}
	FGuid GraphGuid;
	FGuid EntryGuid;
	if (!FGuid::Parse(Plan.GraphGuid, GraphGuid) || !FGuid::Parse(Plan.EntryNodeGuid, EntryGuid))
	{
		OutFailure = TEXT("the prepared prune plan carries an invalid graph or entry identity");
		return false;
	}
	UEdGraph* const Graph = FindGraphByGuid(Blueprint, GraphGuid);
	if (!Graph)
	{
		OutFailure = FString::Printf(TEXT("the pruned graph '%s' did not re-resolve"), *Plan.GraphGuid);
		return false;
	}
	if (!FindNodeByGuidInGraph(Graph, EntryGuid))
	{
		OutFailure = FString::Printf(TEXT("the prune entry terminator '%s' did not re-resolve in the pruned graph"), *Plan.EntryNodeGuid);
		return false;
	}

	TSet<FGuid> Removed;
	for (const FString& GuidText : Plan.ApprovedGuids)
	{
		FGuid RemovedGuid;
		if (!FGuid::Parse(GuidText, RemovedGuid))
		{
			OutFailure = TEXT("the prepared prune plan carries an invalid node identity");
			return false;
		}
		Removed.Add(RemovedGuid);
		if (FindNodeByGuidInGraph(Graph, RemovedGuid))
		{
			OutFailure = FString::Printf(TEXT("the approved node '%s' is still present in the pruned graph"), *GuidText);
			return false;
		}
		if (FindNodeByGuid(Blueprint, RemovedGuid))
		{
			OutFailure = FString::Printf(TEXT("the approved node '%s' still exists in another graph of this asset"), *GuidText);
			return false;
		}
	}

	// Every approved boundary edge is gone from its retained endpoint. A removed endpoint cannot hold
	// a link at all, so only the retained side needs the proof.
	for (const FCortexGraphPruneEdge& Edge : Plan.ExternalEdges)
	{
		FGuid FromGuid;
		FGuid ToGuid;
		if (!FGuid::Parse(Edge.FromGuid, FromGuid) || !FGuid::Parse(Edge.ToGuid, ToGuid))
		{
			OutFailure = TEXT("the prepared prune plan carries an invalid boundary edge");
			return false;
		}
		const bool bFromRemoved = Removed.Contains(FromGuid);
		const bool bToRemoved = Removed.Contains(ToGuid);
		if (!bFromRemoved && !bToRemoved)
		{
			OutFailure = FString::Printf(TEXT("the planned boundary edge '%s' names two retained nodes"), *Edge.FromGuid);
			return false;
		}
		const FGuid RetainedGuid = bFromRemoved ? ToGuid : FromGuid;
		const FName RetainedPin = FName(*(bFromRemoved ? Edge.ToPin : Edge.FromPin));
		const FGuid RemovedFarGuid = bFromRemoved ? FromGuid : ToGuid;
		const FName RemovedFarPin = FName(*(bFromRemoved ? Edge.FromPin : Edge.ToPin));
		UEdGraphNode* const RetainedNode = FindNodeByGuidInGraph(Graph, RetainedGuid);
		if (!RetainedNode)
		{
			OutFailure = FString::Printf(TEXT("the retained endpoint '%s' of a planned boundary edge did not re-resolve"), *RetainedGuid.ToString());
			return false;
		}
		UEdGraphPin* const RetainedPinPtr = RetainedNode->FindPin(RetainedPin);
		if (!RetainedPinPtr) continue;
		const bool bStillLinked = RetainedPinPtr->LinkedTo.ContainsByPredicate(
			[&](const UEdGraphPin* LinkedPin)
			{
				const UEdGraphNode* const FarNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
				return FarNode && FarNode->NodeGuid == RemovedFarGuid && LinkedPin->PinName == RemovedFarPin;
			});
		if (bStillLinked)
		{
			OutFailure = FString::Printf(TEXT("the approved boundary edge '%s.%s' -> '%s.%s' still carries a link"),
				*Edge.FromGuid, *Edge.FromPin, *Edge.ToGuid, *Edge.ToPin);
			return false;
		}
	}
	if (ShouldInjectPruneReadbackFault(TEXT("prune_after_removal")))
	{
		OutFailure = TEXT("the prune readback failed by test injection after the removal check");
		return false;
	}

	// No dangling link may survive anywhere in the pruned graph: every link must resolve to a node of
	// this graph and be reciprocal from the far side.
	{
		TSet<FGuid> InGraph;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node) InGraph.Add(Node->NodeGuid);
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin) continue;
				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					UEdGraphNode* const FarNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
					if (!FarNode || !InGraph.Contains(FarNode->NodeGuid))
					{
						OutFailure = FString::Printf(TEXT("a dangling link survives on '%s.%s'"), *Node->NodeGuid.ToString(), *Pin->PinName.ToString());
						return false;
					}
					if (!LinkedPin->LinkedTo.Contains(Pin))
					{
						OutFailure = FString::Printf(TEXT("the link on '%s.%s' is not reciprocal"), *Node->NodeGuid.ToString(), *Pin->PinName.ToString());
						return false;
					}
				}
			}
		}
	}

	const TArray<FCortexGraphTransferPreservation> Contracts = { Plan.Preservation };
	if (!VerifyPreservationContracts(Blueprint, Contracts, OutFailure)) return false;
	if (ShouldInjectPruneReadbackFault(TEXT("prune_after_preservation")))
	{
		OutFailure = TEXT("the prune readback failed by test injection after the preservation check");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FCortexGraphMigrationOps::MakePruneInventory(const TSharedPtr<FJsonObject>& PrunePlanJson)
{
	if (!PrunePlanJson.IsValid()) return nullptr;
	FCortexGraphMigrationPrunePlan Plan;
	FCortexCommandResult PlanError;
	if (!FCortexGraphMigrationPrunePlan::FromJson(PrunePlanJson, Plan, PlanError)) return nullptr;

	TSharedPtr<FJsonObject> Inventory = MakeShared<FJsonObject>();
	Inventory->SetStringField(TEXT("operation"), Plan.Op);
	Inventory->SetStringField(TEXT("graph_guid"), Plan.GraphGuid);
	if (!Plan.SubgraphPath.IsEmpty())
	{
		Inventory->SetStringField(TEXT("subgraph_path"), Plan.SubgraphPath);
	}
	Inventory->SetStringField(TEXT("entry_node_guid"), Plan.EntryNodeGuid);
	Inventory->SetBoolField(TEXT("awaiting_approval"), Plan.bAwaitingApproval);
	Inventory->SetBoolField(TEXT("complete"), Plan.bComplete);
	Inventory->SetBoolField(TEXT("reused"), Plan.bReused);
	Inventory->SetNumberField(TEXT("scan_limit"), PruneScanNodeLimit);
	Inventory->SetNumberField(TEXT("scanned_nodes"), Plan.ScannedNodes);
	Inventory->SetNumberField(TEXT("scanned_links"), Plan.ScannedLinks);

	auto ToValues = [](const TArray<FString>& Lines)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Line : Lines) Values.Add(MakeShared<FJsonValueString>(Line));
		return Values;
	};
	// The removable set is published complete, because the caller has to echo it exactly; the
	// informational partitions are bounded by the shared diagnostics bound like every other preview.
	Inventory->SetArrayField(TEXT("removable"), ToValues(Plan.RemovableGuids));
	Inventory->SetArrayField(TEXT("approved_guids"), ToValues(Plan.ApprovedGuids));
	auto PartitionLines = [](const TArray<FCortexGraphPruneNode>& Nodes)
	{
		TArray<FString> Lines;
		for (const FCortexGraphPruneNode& Node : Nodes)
		{
			Lines.Add(FString::Printf(TEXT("%s %s (%s)"), *Node.NodeGuid, *Node.ClassPath, *Node.Reason));
		}
		return Lines;
	};
	TArray<FString> Shared = PartitionLines(Plan.Shared);
	TArray<FString> Blocked = PartitionLines(Plan.Blocked);
	TArray<FString> ExternalEdges;
	for (const FCortexGraphPruneEdge& Edge : Plan.ExternalEdges)
	{
		ExternalEdges.Add(FString::Printf(TEXT("%s.%s -> %s.%s"), *Edge.FromGuid, *Edge.FromPin, *Edge.ToGuid, *Edge.ToPin));
	}
	FCortexGraphPatchOps::TrimDiagnostics(Shared);
	FCortexGraphPatchOps::TrimDiagnostics(Blocked);
	FCortexGraphPatchOps::TrimDiagnostics(ExternalEdges);
	Inventory->SetArrayField(TEXT("shared"), ToValues(Shared));
	Inventory->SetArrayField(TEXT("blocked_nodes"), ToValues(Blocked));
	Inventory->SetArrayField(TEXT("external_edges"), ToValues(ExternalEdges));
	return Inventory;
}

namespace
{
const TCHAR* const RetireEntriesOp = TEXT("retire_entries");

FString BlueprintStatusName(const EBlueprintStatus Status)
{
	switch (Status)
	{
	case BS_Error: return TEXT("BS_Error");
	case BS_UpToDate: return TEXT("BS_UpToDate");
	case BS_UpToDateWithWarnings: return TEXT("BS_UpToDateWithWarnings");
	case BS_Dirty:
		return TEXT("BS_Dirty");
	case BS_BeingCreated: return TEXT("BS_BeingCreated");
	case BS_Unknown:
	default: return TEXT("BS_Unknown");
	}
}

void AppendRetireStrings(const TArray<FString>& Strings, TArray<TSharedPtr<FJsonValue>>& Out)
{
	for (const FString& String : Strings) Out.Add(MakeShared<FJsonValueString>(String));
}

bool ReadRetireStrings(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, TArray<FString>& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(Field, Values) || !Values) return false;
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		FString String;
		if (!Value.IsValid() || !Value->TryGetString(String) || String.IsEmpty()) return false;
		Out.Add(String);
	}
	return true;
}

void WriteRetirePartition(const TArray<FCortexGraphPruneNode>& Nodes, TArray<TSharedPtr<FJsonValue>>& Out)
{
	for (const FCortexGraphPruneNode& Node : Nodes)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("node_guid"), Node.NodeGuid);
		Json->SetStringField(TEXT("class_path"), Node.ClassPath);
		Json->SetStringField(TEXT("reason"), Node.Reason);
		Out.Add(MakeShared<FJsonValueObject>(Json));
	}
}

bool ReadRetirePartition(const TSharedPtr<FJsonObject>& Source, const TCHAR* Field, TArray<FCortexGraphPruneNode>& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Source->TryGetArrayField(Field, Values) || !Values) return false;
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject> Json = Value.IsValid() ? Value->AsObject() : nullptr;
		FCortexGraphPruneNode Node;
		if (!Json.IsValid() || !Json->TryGetStringField(TEXT("node_guid"), Node.NodeGuid)
			|| !Json->TryGetStringField(TEXT("class_path"), Node.ClassPath)
			|| !Json->TryGetStringField(TEXT("reason"), Node.Reason) || Node.NodeGuid.IsEmpty()) return false;
		Out.Add(MoveTemp(Node));
	}
	return true;
}
}

TSharedPtr<FJsonObject> FCortexGraphMigrationRetirePlan::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("op"), Op);
	Json->SetStringField(TEXT("graph_guid"), GraphGuid);
	Json->SetBoolField(TEXT("complete"), bComplete);
	Json->SetBoolField(TEXT("awaiting_approval"), bAwaitingApproval);
	Json->SetBoolField(TEXT("reused"), bReused);
	Json->SetNumberField(TEXT("scanned_nodes"), ScannedNodes);
	Json->SetNumberField(TEXT("scanned_links"), ScannedLinks);
	Json->SetStringField(TEXT("blueprint_status_before"), BlueprintStatusBefore);
	Json->SetBoolField(TEXT("preexisting_diagnostics_truncated"), bPreexistingDiagnosticsTruncated);
	TArray<TSharedPtr<FJsonValue>> Values;
	AppendRetireStrings(SelectedEntryGuids, Values); Json->SetArrayField(TEXT("selected_entry_guids"), Values);
	Values.Reset(); AppendRetireStrings(ApprovedGuids, Values); Json->SetArrayField(TEXT("approved_guids"), Values);
	Values.Reset(); AppendRetireStrings(RemovableGuids, Values); Json->SetArrayField(TEXT("removable_guids"), Values);
	Values.Reset(); AppendRetireStrings(PreexistingDiagnostics, Values); Json->SetArrayField(TEXT("preexisting_diagnostics"), Values);
	Values.Reset(); WriteRetirePartition(Shared, Values); Json->SetArrayField(TEXT("shared"), Values);
	Values.Reset(); WriteRetirePartition(Blocked, Values); Json->SetArrayField(TEXT("blocked"), Values);
	Values.Reset();
	for (const FCortexGraphPruneEdge& Edge : ExternalEdges)
	{
		TSharedPtr<FJsonObject> EdgeJson = MakeShared<FJsonObject>();
		EdgeJson->SetStringField(TEXT("from_guid"), Edge.FromGuid);
		EdgeJson->SetStringField(TEXT("from_pin"), Edge.FromPin);
		EdgeJson->SetStringField(TEXT("to_guid"), Edge.ToGuid);
		EdgeJson->SetStringField(TEXT("to_pin"), Edge.ToPin);
		Values.Add(MakeShared<FJsonValueObject>(EdgeJson));
	}
	Json->SetArrayField(TEXT("external_edges"), Values);
	TSharedPtr<FJsonObject> Preserve = MakeShared<FJsonObject>();
	Preserve->SetStringField(TEXT("label"), Preservation.Label);
	Preserve->SetStringField(TEXT("graph_guid"), Preservation.GraphGuid);
	Preserve->SetStringField(TEXT("capture"), Preservation.Capture);
	Values.Reset(); AppendRetireStrings(Preservation.ExcludedGuids, Values);
	Preserve->SetArrayField(TEXT("excluded_guids"), Values);
	Json->SetObjectField(TEXT("preservation"), Preserve);
	return Json;
}

bool FCortexGraphMigrationRetirePlan::FromJson(
	const TSharedPtr<FJsonObject>& Source,
	FCortexGraphMigrationRetirePlan& OutPlan,
	FCortexCommandResult& OutError)
{
	OutPlan = FCortexGraphMigrationRetirePlan();
	OutError = FCortexCommandResult();
	if (!Source.IsValid()
		|| !Source->TryGetStringField(TEXT("op"), OutPlan.Op)
		|| OutPlan.Op != RetireEntriesOp
		|| !Source->TryGetStringField(TEXT("graph_guid"), OutPlan.GraphGuid)
		|| !ReadRetireStrings(Source, TEXT("selected_entry_guids"), OutPlan.SelectedEntryGuids)
		|| !ReadRetireStrings(Source, TEXT("approved_guids"), OutPlan.ApprovedGuids)
		|| !ReadRetireStrings(Source, TEXT("removable_guids"), OutPlan.RemovableGuids)
		|| !ReadRetireStrings(Source, TEXT("preexisting_diagnostics"), OutPlan.PreexistingDiagnostics)
		|| !ReadRetirePartition(Source, TEXT("shared"), OutPlan.Shared)
		|| !ReadRetirePartition(Source, TEXT("blocked"), OutPlan.Blocked)
		|| !Source->TryGetBoolField(TEXT("complete"), OutPlan.bComplete)
		|| !Source->TryGetBoolField(TEXT("awaiting_approval"), OutPlan.bAwaitingApproval)
		|| !Source->TryGetBoolField(TEXT("reused"), OutPlan.bReused)
		|| !Source->TryGetBoolField(TEXT("preexisting_diagnostics_truncated"), OutPlan.bPreexistingDiagnosticsTruncated))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("prepared retirement plan is incomplete"));
		return false;
	}
	Source->TryGetStringField(TEXT("blueprint_status_before"), OutPlan.BlueprintStatusBefore);
	Source->TryGetNumberField(TEXT("scanned_nodes"), OutPlan.ScannedNodes);
	Source->TryGetNumberField(TEXT("scanned_links"), OutPlan.ScannedLinks);
	const TSharedPtr<FJsonObject>* PreservationJson = nullptr;
	if (!Source->TryGetObjectField(TEXT("preservation"), PreservationJson) || !PreservationJson || !PreservationJson->IsValid()
		|| !(*PreservationJson)->TryGetStringField(TEXT("label"), OutPlan.Preservation.Label)
		|| !(*PreservationJson)->TryGetStringField(TEXT("graph_guid"), OutPlan.Preservation.GraphGuid)
		|| !(*PreservationJson)->TryGetStringField(TEXT("capture"), OutPlan.Preservation.Capture)
		|| !ReadRetireStrings(*PreservationJson, TEXT("excluded_guids"), OutPlan.Preservation.ExcludedGuids))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("prepared retirement plan has no preservation contract"));
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* EdgeValues = nullptr;
	if (!Source->TryGetArrayField(TEXT("external_edges"), EdgeValues) || !EdgeValues)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("prepared retirement plan has no external-edge inventory"));
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *EdgeValues)
	{
		const TSharedPtr<FJsonObject> EdgeJson = Value.IsValid() ? Value->AsObject() : nullptr;
		FCortexGraphPruneEdge Edge;
		if (!EdgeJson.IsValid() || !EdgeJson->TryGetStringField(TEXT("from_guid"), Edge.FromGuid)
			|| !EdgeJson->TryGetStringField(TEXT("from_pin"), Edge.FromPin)
			|| !EdgeJson->TryGetStringField(TEXT("to_guid"), Edge.ToGuid)
			|| !EdgeJson->TryGetStringField(TEXT("to_pin"), Edge.ToPin))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("prepared retirement plan contains an invalid edge"));
			return false;
		}
		OutPlan.ExternalEdges.Add(MoveTemp(Edge));
	}
	return true;
}

bool FCortexGraphMigrationOps::PlanRetirement(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Migration,
	FCortexGraphMigrationRetirePlan& OutPlan,
	bool& bOutReused,
	FCortexCommandResult& OutError)
{
	OutPlan = FCortexGraphMigrationRetirePlan();
	OutError = FCortexCommandResult();
	bOutReused = false;
	if (!Blueprint)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound, TEXT("Blueprint is null"));
		return false;
	}
	if (!Migration.IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("migration must be an object"));
		return false;
	}
	if (!FCortexGraphPatchOps::HasOnlyFields(Migration,
		{ TEXT("op"), TEXT("source"), TEXT("approved_node_guids") }, OutError, TEXT("migration"))) return false;
	FString Op;
	if (!FCortexGraphPatchOps::ReadRequiredString(Migration, TEXT("op"), Op, OutError)) return false;
	if (Op != RetireEntriesOp)
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::UnsupportedOperation, TEXT("migration.op must be retire_entries"));
		return false;
	}
	const TSharedPtr<FJsonObject>* SourcePtr = nullptr;
	if (!Migration->TryGetObjectField(TEXT("source"), SourcePtr) || !SourcePtr || !SourcePtr->IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("migration.source must be an object"));
		return false;
	}
	if (!FCortexGraphPatchOps::HasOnlyFields(*SourcePtr,
		{ TEXT("graph_ref"), TEXT("entry_node_guids") }, OutError, TEXT("migration.source"))) return false;
	const TSharedPtr<FJsonObject>* GraphRefPtr = nullptr;
	if (!(*SourcePtr)->TryGetObjectField(TEXT("graph_ref"), GraphRefPtr) || !GraphRefPtr || !GraphRefPtr->IsValid())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("migration.source.graph_ref must be an object"));
		return false;
	}
	if (!FCortexGraphPatchOps::HasOnlyFields(*GraphRefPtr,
		{ TEXT("graph_guid"), TEXT("graph_kind") }, OutError, TEXT("migration.source.graph_ref"))) return false;
	FGuid GraphGuid;
	if (!FCortexGraphPatchOps::ParseGuidField(*GraphRefPtr, TEXT("graph_guid"), GraphGuid, OutError)) return false;
	UEdGraph* Graph = nullptr;
	if (!FCortexGraphPatchOps::ResolveGraphByGuid(Blueprint, GraphGuid, FString(), Graph, OutError)) return false;
	FString GraphKind;
	if (!FCortexGraphPatchOps::ResolveGraphKindByGuid(Blueprint, GraphGuid, GraphKind, OutError)) return false;
	if (GraphKind != TEXT("ubergraph"))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("retirement is supported only in a top-level ubergraph"));
		return false;
	}
	if (!Blueprint->UbergraphPages.Contains(Graph))
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
			TEXT("retirement is supported only in a top-level ubergraph"));
		return false;
	}
	if ((*GraphRefPtr)->HasField(TEXT("graph_kind")))
	{
		FString RequestedKind;
		if (!(*GraphRefPtr)->TryGetStringField(TEXT("graph_kind"), RequestedKind) || RequestedKind != GraphKind)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("migration.source.graph_ref.graph_kind conflicts with the resolved graph"));
			return false;
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* SelectedValues = nullptr;
	if (!(*SourcePtr)->TryGetArrayField(TEXT("entry_node_guids"), SelectedValues) || !SelectedValues || SelectedValues->IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("migration.source.entry_node_guids must be a non-empty array"));
		return false;
	}
	TArray<FGuid> SelectedGuids;
	TArray<UEdGraphNode*> SelectedNodes;
	TSet<FGuid> SelectedSet;
	for (const TSharedPtr<FJsonValue>& Value : *SelectedValues)
	{
		FString Text;
		FGuid Guid;
		if (!Value.IsValid() || !Value->TryGetString(Text) || !FGuid::Parse(Text, Guid) || !Guid.IsValid())
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("entry_node_guids entries must be valid GUID strings"));
			return false;
		}
		if (SelectedSet.Contains(Guid))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, FString::Printf(TEXT("entry_node_guids repeats node '%s'"), *Guid.ToString()));
			return false;
		}
		SelectedSet.Add(Guid);
		UEdGraphNode* Node = FindNodeByGuidInGraph(Graph, Guid);
		if (!Node)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::NodeNotFound, FString::Printf(TEXT("selected entry '%s' is not in the named graph"), *Guid.ToString()));
			return false;
		}
		int32 Owners = 0;
		TArray<UEdGraph*> AssetGraphs;
		Blueprint->GetAllGraphs(AssetGraphs);
		for (UEdGraph* AssetGraph : AssetGraphs)
		{
			if (!AssetGraph) continue;
			for (UEdGraphNode* Owned : AssetGraph->Nodes) if (Owned && Owned->NodeGuid == Guid) ++Owners;
		}
		if (Owners != 1)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("selected entry identity '%s' is owned by %d nodes instead of exactly one"), *Guid.ToString(), Owners));
			return false;
		}
		UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
		UClass* Parent = Event ? Event->EventReference.GetMemberParentClass() : nullptr;
		const FName Member = Event ? Event->EventReference.GetMemberName() : NAME_None;
		UFunction* Function = Parent && !Member.IsNone() ? Parent->FindFunctionByName(Member) : nullptr;
		if (!Event || Node->IsA<UK2Node_CustomEvent>() || !Event->bOverrideFunction || Event->bInternalEvent
			|| !Parent || Member.IsNone() || !Function
			|| !Function->HasAnyFunctionFlags(FUNC_BlueprintEvent)
			|| Event->GetSubGraphs().Num() > 0
			|| (Event->GetDelegatePin() && Event->GetDelegatePin()->LinkedTo.Num() > 0)
			|| !Node->Pins.ContainsByPredicate([](const UEdGraphPin* Pin)
				{ return Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec; }))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("selected node '%s' is not a supported unbound override event with a valid parent member and exec output"), *Guid.ToString()));
			return false;
		}
		const FString MemberName = Member.ToString();
		if (MemberName == TEXT("Construct") || MemberName == TEXT("PreConstruct") || MemberName == TEXT("Destruct")
			|| MemberName == TEXT("OnInitialized"))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("lifecycle event '%s' is not supported for retirement"), *MemberName));
			return false;
		}
		SelectedGuids.Add(Guid);
		SelectedNodes.Add(Node);
	}
	bool bHasApproval = Migration->HasField(TEXT("approved_node_guids"));
	TArray<FGuid> Approved;
	if (bHasApproval)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Migration->TryGetArrayField(TEXT("approved_node_guids"), Values) || !Values)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("approved_node_guids must be an array"));
			return false;
		}
		TSet<FGuid> Seen;
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Text;
			FGuid Guid;
			if (!Value.IsValid() || !Value->TryGetString(Text) || !FGuid::Parse(Text, Guid) || !Guid.IsValid() || Seen.Contains(Guid))
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("approved_node_guids must contain unique valid GUID strings"));
				return false;
			}
			Seen.Add(Guid);
			Approved.Add(Guid);
		}
	}
	FPrunePartition Partition;
	if (!ComputeOwnedIslandPartition(Blueprint, Graph, SelectedNodes, true, Partition, OutError)) return false;
	OutPlan.Op = Op;
	OutPlan.GraphGuid = GraphGuid.ToString();
	for (const FGuid& Guid : SelectedGuids) OutPlan.SelectedEntryGuids.Add(Guid.ToString());
	OutPlan.SelectedEntryGuids.Sort();
	OutPlan.ScannedNodes = Partition.Scan.Nodes;
	OutPlan.ScannedLinks = Partition.Scan.Links;
	OutPlan.bComplete = !Partition.Scan.bExhausted;
	OutPlan.bAwaitingApproval = !bHasApproval;
	OutPlan.Shared = Partition.Shared;
	OutPlan.Blocked = Partition.Blocked;
	OutPlan.ExternalEdges = Partition.ExternalEdges;
	OutPlan.RemovableGuids = PruneGuidText(Partition.Removable);
	if (bHasApproval)
	{
		if (OutPlan.Blocked.Num() > 0 || SelectedGuids.ContainsByPredicate(
			[&](const FGuid& Guid) { return !Partition.Removable.Contains(Guid); }))
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("reviewed retirement is refused because one or more selected entries are blocked or retained"));
			return false;
		}
		const TArray<FString> ApprovedText = PruneGuidText(Approved);
		if (ApprovedText != OutPlan.RemovableGuids)
		{
			TArray<FString> Missing;
			TArray<FString> Extra;
			for (const FString& Guid : OutPlan.RemovableGuids) if (!ApprovedText.Contains(Guid)) Missing.Add(Guid);
			for (const FString& Guid : ApprovedText) if (!OutPlan.RemovableGuids.Contains(Guid)) Extra.Add(Guid);
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("approved_node_guids must exactly equal the removable set (missing: %s; extra: %s)"),
					Missing.IsEmpty() ? TEXT("none") : *FString::Join(Missing, TEXT(", ")),
					Extra.IsEmpty() ? TEXT("none") : *FString::Join(Extra, TEXT(", "))));
			return false;
		}
		OutPlan.ApprovedGuids = ApprovedText;
	}
	OutPlan.Preservation.Label = TEXT("graph");
	OutPlan.Preservation.GraphGuid = GraphGuid.ToString();
	OutPlan.Preservation.ExcludedGuids = OutPlan.RemovableGuids;
	TArray<FGuid> ExcludedGuids = Partition.Removable;
	OutPlan.Preservation.Capture = CapturePreservation(Blueprint, Graph, ExcludedGuids);
	if (OutPlan.Preservation.Capture.IsEmpty())
	{
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("retirement preservation could not be captured"));
		return false;
	}
	OutPlan.BlueprintStatusBefore = BlueprintStatusName(Blueprint->Status);
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->bHasCompilerMessage && !Node->ErrorMsg.IsEmpty()) OutPlan.PreexistingDiagnostics.Add(Node->ErrorMsg);
	}
	const bool bDiagnosticsTruncated = OutPlan.PreexistingDiagnostics.Num() > 15
		|| OutPlan.PreexistingDiagnostics.Contains(TEXT("additional compiler diagnostics omitted"));
	FCortexGraphPatchOps::TrimDiagnostics(OutPlan.PreexistingDiagnostics);
	OutPlan.bPreexistingDiagnosticsTruncated = bDiagnosticsTruncated
		|| OutPlan.PreexistingDiagnostics.Contains(TEXT("additional compiler diagnostics omitted"));
	return true;
}

TSharedPtr<FJsonObject> FCortexGraphMigrationOps::MakeRetirementInventory(const TSharedPtr<FJsonObject>& RetirePlanJson)
{
	FCortexGraphMigrationRetirePlan Plan;
	FCortexCommandResult Error;
	if (!RetirePlanJson.IsValid() || !FCortexGraphMigrationRetirePlan::FromJson(RetirePlanJson, Plan, Error)) return nullptr;
	TSharedPtr<FJsonObject> Inventory = MakeShared<FJsonObject>();
	Inventory->SetStringField(TEXT("operation"), Plan.Op);
	Inventory->SetStringField(TEXT("graph_guid"), Plan.GraphGuid);
	auto ToValues = [](const TArray<FString>& Lines)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		AppendRetireStrings(Lines, Values);
		return Values;
	};
	Inventory->SetArrayField(TEXT("selected_entry_guids"), ToValues(Plan.SelectedEntryGuids));
	Inventory->SetBoolField(TEXT("awaiting_approval"), Plan.bAwaitingApproval);
	Inventory->SetBoolField(TEXT("complete"), Plan.bComplete);
	Inventory->SetBoolField(TEXT("reused"), Plan.bReused);
	Inventory->SetNumberField(TEXT("scan_limit"), FCortexGraphPatchOps::MaxScannedNodes);
	Inventory->SetNumberField(TEXT("scanned_nodes"), Plan.ScannedNodes);
	Inventory->SetNumberField(TEXT("scanned_links"), Plan.ScannedLinks);
	Inventory->SetArrayField(TEXT("removable"), ToValues(Plan.RemovableGuids));
	Inventory->SetArrayField(TEXT("approved_guids"), ToValues(Plan.ApprovedGuids));
	Inventory->SetStringField(TEXT("blueprint_status_before"), Plan.BlueprintStatusBefore);
	Inventory->SetArrayField(TEXT("preexisting_diagnostics"), ToValues(Plan.PreexistingDiagnostics));
	Inventory->SetBoolField(TEXT("preexisting_diagnostics_truncated"), Plan.bPreexistingDiagnosticsTruncated);
	Inventory->SetStringField(TEXT("preexisting_diagnostics_source"), TEXT("cached_node_messages"));
	auto PartitionLines = [](const TArray<FCortexGraphPruneNode>& Nodes)
	{
		TArray<FString> Lines;
		for (const FCortexGraphPruneNode& Node : Nodes)
			Lines.Add(FString::Printf(TEXT("%s %s (%s)"), *Node.NodeGuid, *Node.ClassPath, *Node.Reason));
		FCortexGraphPatchOps::TrimDiagnostics(Lines);
		return Lines;
	};
	Inventory->SetArrayField(TEXT("shared"), ToValues(PartitionLines(Plan.Shared)));
	Inventory->SetArrayField(TEXT("blocked_nodes"), ToValues(PartitionLines(Plan.Blocked)));
	TArray<FString> Edges;
	for (const FCortexGraphPruneEdge& Edge : Plan.ExternalEdges)
		Edges.Add(FString::Printf(TEXT("%s.%s -> %s.%s"), *Edge.FromGuid, *Edge.FromPin, *Edge.ToGuid, *Edge.ToPin));
	FCortexGraphPatchOps::TrimDiagnostics(Edges);
	Inventory->SetArrayField(TEXT("external_edges"), ToValues(Edges));
	return Inventory;
}

#if WITH_AUTOMATION_TESTS
void FCortexGraphMigrationOps::SetPruneReadbackFaultForTesting(const FName Check)
{
	PruneReadbackFaultForTesting = Check;
}

void FCortexGraphMigrationOps::ClearPruneReadbackFaultForTesting()
{
	PruneReadbackFaultForTesting = NAME_None;
}
#endif
