#include "Operations/CortexGraphMigrationOps.h"

#include "CortexEngineCompat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
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
#include "Operations/CortexGraphNodeOps.h"
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
		TArray<FCortexGraphEntry> Entries;
		FCortexGraphNodeOps::EnumerateUserGraphs(Blueprint, Entries);
		for (const FCortexGraphEntry& Entry : Entries)
		{
			if (Entry.Graph && Entry.Graph->GraphGuid == GraphGuid
				&& FCortexGraphNodeOps::GraphKindToString(Entry.Kind) != RequestedKind)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidField,
					TEXT("migration source graph_kind conflicts with graph identity"));
				return false;
			}
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
