#include "Operations/CortexAnimBindingReport.h"

#include "AnimGraphNode_Base.h"
#include "Animation/AnimBlueprint.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/MemberReference.h"
#include "Features/IModularFeatures.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "IPropertyAccessEditor.h"
#include "Operations/CortexAnimAssetUtils.h"
#include "Operations/CortexAnimBindingReflection.h"

namespace CortexAnimBindingReportPrivate
{
	FString GuidText(const FGuid& Guid)
	{
		return Guid.ToString(EGuidFormats::DigitsWithHyphens);
	}

	TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& Strings)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(Strings.Num());
		for (const FString& String : Strings)
		{
			Values.Add(MakeShared<FJsonValueString>(String));
		}
		return Values;
	}

	/** node_guid, graph_guid, graph_name and node_class: where the entry's node lives. */
	void SetNodeLocator(const TSharedRef<FJsonObject>& Entry, const UEdGraphNode* Node)
	{
		Entry->SetStringField(TEXT("node_guid"), GuidText(Node->NodeGuid));
		const UEdGraph* Graph = Node->GetGraph();
		Entry->SetStringField(TEXT("graph_guid"), Graph != nullptr ? GuidText(Graph->GraphGuid) : FString());
		Entry->SetStringField(TEXT("graph_name"), Graph != nullptr ? Graph->GetName() : FString());
		Entry->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
	}

	TSharedRef<FJsonObject> LeafToJson(const FCortexAnimBindingReport::FPathResolution& Resolution)
	{
		TSharedRef<FJsonObject> Leaf = MakeShared<FJsonObject>();
		Leaf->SetStringField(TEXT("name"), Resolution.LeafName);
		Leaf->SetStringField(TEXT("kind"), Resolution.LeafKind);
		Leaf->SetStringField(TEXT("cpp_type"), Resolution.LeafCppType);
		return Leaf;
	}

	void SetResolution(const TSharedRef<FJsonObject>& Entry, const FCortexAnimBindingReport::FPathResolution& Resolution)
	{
		Entry->SetBoolField(TEXT("resolved"), Resolution.bResolved);
		Entry->SetStringField(TEXT("root_declared_in"), Resolution.RootDeclaredIn);
		if (Resolution.bHasLeaf)
		{
			Entry->SetObjectField(TEXT("leaf"), LeafToJson(Resolution));
		}
		else
		{
			Entry->SetField(TEXT("leaf"), MakeShared<FJsonValueNull>());
		}
	}

	FCortexCommandResult UnsupportedField(const FString& Field)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(TEXT("missing_field"), Field);
		return FCortexCommandRouter::Error(
			CortexErrorCodes::UnsupportedOperation,
			FString::Printf(TEXT("This engine build does not expose the reflected field '%s' that the animation binding report reads"), *Field),
			Details);
	}

	/** Every anim graph node of the Blueprint in graph then node order (nested graphs included). */
	void CollectAnimNodes(const UBlueprint* Blueprint, TArray<UAnimGraphNode_Base*>& OutNodes)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			if (Graph == nullptr)
			{
				continue;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node))
				{
					OutNodes.Add(AnimNode);
				}
			}
		}
	}
}

FString FCortexAnimBindingReport::DeclaredIn(const UStruct* Owner)
{
	if (const UClass* Class = Cast<UClass>(Owner))
	{
		return Class->HasAnyClassFlags(CLASS_CompiledFromBlueprint) || UBlueprint::GetBlueprintFromClass(Class) != nullptr
			? TEXT("blueprint") : TEXT("native");
	}
	return Owner != nullptr ? TEXT("struct") : TEXT("unresolved");
}

FCortexAnimBindingReport::FPathResolution FCortexAnimBindingReport::ResolvePath(const UBlueprint* Blueprint, const TArray<FString>& Path)
{
	FPathResolution Resolution;
	if (Blueprint == nullptr || Blueprint->SkeletonGeneratedClass == nullptr || Path.IsEmpty()
		|| !IModularFeatures::Get().IsModularFeatureAvailable(TEXT("PropertyAccessEditor")))
	{
		return Resolution;
	}

	IPropertyAccessEditor& PropertyAccessEditor = IModularFeatures::Get().GetModularFeature<IPropertyAccessEditor>(TEXT("PropertyAccessEditor"));

	const UStruct* RootOwner = nullptr;
	int32 LeafSegment = INDEX_NONE;
	const FProperty* LeafProperty = nullptr;
	const UFunction* LeafFunction = nullptr;

	IPropertyAccessEditor::FResolvePropertyAccessArgs Args;
	Args.bUseMostUpToDateClasses = true;
	Args.PropertyFunction = [&](int32 Segment, FProperty* Property, int32)
	{
		if (Segment == 0)
		{
			RootOwner = Property->GetOwnerStruct();
		}
		if (Segment >= LeafSegment)
		{
			LeafSegment = Segment;
			LeafProperty = Property;
			LeafFunction = nullptr;
		}
	};
	Args.ArrayFunction = [&](int32 Segment, FArrayProperty* Property, int32)
	{
		if (Segment == 0)
		{
			RootOwner = Property->GetOwnerStruct();
		}
		if (Segment >= LeafSegment)
		{
			LeafSegment = Segment;
			LeafProperty = Property;
			LeafFunction = nullptr;
		}
	};
	Args.FunctionFunction = [&](int32 Segment, UFunction* Function, FProperty* ReturnProperty)
	{
		if (Segment == 0)
		{
			RootOwner = Function->GetOuterUClass();
		}
		if (Segment >= LeafSegment)
		{
			LeafSegment = Segment;
			LeafProperty = ReturnProperty;
			LeafFunction = Function;
		}
	};

	const FPropertyAccessResolveResult Result = PropertyAccessEditor.ResolvePropertyAccess(Blueprint->SkeletonGeneratedClass, Path, Args);
	Resolution.bResolved = Result.Result != EPropertyAccessResolveResult::Failed;
	if (!Resolution.bResolved)
	{
		return Resolution;
	}

	Resolution.RootDeclaredIn = DeclaredIn(RootOwner);
	if (LeafFunction != nullptr)
	{
		Resolution.bHasLeaf = true;
		Resolution.LeafName = LeafFunction->GetName();
		Resolution.LeafKind = TEXT("function");
		Resolution.LeafCppType = LeafProperty != nullptr ? LeafProperty->GetCPPType() : FString();
	}
	else if (LeafProperty != nullptr)
	{
		Resolution.bHasLeaf = true;
		Resolution.LeafName = LeafProperty->GetName();
		Resolution.LeafCppType = LeafProperty->GetCPPType();
	}
	return Resolution;
}

void FCortexAnimBindingReport::ResolveFunctionSegments(const UBlueprint* Blueprint, const TArray<FString>& Path, const FName FunctionName,
	TArray<int32>& OutOwned, TArray<int32>& OutForeign)
{
	if (Blueprint == nullptr || Blueprint->SkeletonGeneratedClass == nullptr || Path.IsEmpty()
		|| !IModularFeatures::Get().IsModularFeatureAvailable(TEXT("PropertyAccessEditor")))
	{
		return;
	}

	IPropertyAccessEditor& PropertyAccessEditor = IModularFeatures::Get().GetModularFeature<IPropertyAccessEditor>(TEXT("PropertyAccessEditor"));
	const UClass* SkeletonClass = FBlueprintEditorUtils::GetMostUpToDateClass(Blueprint->SkeletonGeneratedClass);

	IPropertyAccessEditor::FResolvePropertyAccessArgs Args;
	Args.bUseMostUpToDateClasses = true;
	Args.FunctionFunction = [&](int32 Segment, UFunction* Function, FProperty*)
	{
		if (Function->GetFName() != FunctionName)
		{
			return;
		}
		const UClass* Owner = FBlueprintEditorUtils::GetMostUpToDateClass(Function->GetOuterUClass());
		(Owner != nullptr && Owner->IsChildOf(SkeletonClass) ? OutOwned : OutForeign).Add(Segment);
	};
	PropertyAccessEditor.ResolvePropertyAccess(Blueprint->SkeletonGeneratedClass, Path, Args);
}

TSharedRef<FJsonObject> FCortexAnimBindingReport::PinTypeToJson(const FEdGraphPinType& PinType)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("category"), PinType.PinCategory.ToString());
	Json->SetStringField(TEXT("sub_category"), PinType.PinSubCategory.ToString());
	const UObject* SubCategoryObject = PinType.PinSubCategoryObject.Get();
	Json->SetStringField(TEXT("sub_category_object"), SubCategoryObject != nullptr ? SubCategoryObject->GetPathName() : FString());
	switch (PinType.ContainerType)
	{
	case EPinContainerType::Array:
		Json->SetStringField(TEXT("container"), TEXT("array"));
		break;
	case EPinContainerType::Set:
		Json->SetStringField(TEXT("container"), TEXT("set"));
		break;
	case EPinContainerType::Map:
		Json->SetStringField(TEXT("container"), TEXT("map"));
		break;
	default:
		Json->SetStringField(TEXT("container"), TEXT("none"));
		break;
	}
	return Json;
}

bool FCortexAnimBindingReport::AppendBindings(UAnimBlueprint* AnimBlueprint, const int32 Limit, const TSharedPtr<FJsonObject>& Data, FCortexCommandResult& OutError)
{
	using namespace CortexAnimBindingReportPrivate;

	FString Missing;
	if (!FCortexAnimBindingReflection::CheckBindingStorage(Missing))
	{
		OutError = UnsupportedField(Missing);
		return false;
	}

	TArray<UAnimGraphNode_Base*> Nodes;
	CollectAnimNodes(AnimBlueprint, Nodes);

	TArray<TSharedPtr<FJsonValue>> Bindings;
	TArray<TSharedPtr<FJsonValue>> UnsupportedNodes;
	for (const UAnimGraphNode_Base* Node : Nodes)
	{
		TArray<FCortexAnimBindingEntry> Entries;
		if (FCortexAnimBindingReflection::ReadNodeBindings(Node, Entries) == ECortexAnimBindingStorage::Unsupported)
		{
			TSharedRef<FJsonObject> Unsupported = MakeShared<FJsonObject>();
			SetNodeLocator(Unsupported, Node);
			Unsupported->SetStringField(TEXT("binding_storage"), TEXT("unsupported"));
			UnsupportedNodes.Add(MakeShared<FJsonValueObject>(Unsupported));
			continue;
		}

		for (const FCortexAnimBindingEntry& Binding : Entries)
		{
			const FPathResolution Resolution = ResolvePath(AnimBlueprint, Binding.Path);
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			SetNodeLocator(Entry, Node);
			Entry->SetStringField(TEXT("property_name"), Binding.PropertyName.ToString());
			Entry->SetNumberField(TEXT("array_index"), Binding.ArrayIndex);
			Entry->SetStringField(TEXT("type"), Binding.bFunction ? TEXT("function") : TEXT("property"));
			Entry->SetBoolField(TEXT("is_bound"), Binding.bIsBound);
			Entry->SetArrayField(TEXT("path"), StringArray(Binding.Path));
			Entry->SetStringField(TEXT("path_text"), Binding.PathText);
			SetResolution(Entry, Resolution);
			Entry->SetBoolField(TEXT("is_promotion"), Binding.bIsPromotion);
			Entry->SetBoolField(TEXT("only_update_when_active"), Binding.bOnlyUpdateWhenActive);
			Entry->SetStringField(TEXT("binding_storage"), TEXT("default"));
			Bindings.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	Data->SetObjectField(TEXT("bindings"), FCortexAnimAssetUtils::MakeLimitedArray(Bindings, Limit));
	Data->SetArrayField(TEXT("unsupported_binding_nodes"), UnsupportedNodes);
	return true;
}

bool FCortexAnimBindingReport::AppendNodeFunctions(UAnimBlueprint* AnimBlueprint, const int32 Limit, const TSharedPtr<FJsonObject>& Data, FCortexCommandResult& OutError)
{
	using namespace CortexAnimBindingReportPrivate;
	(void)OutError;

	TArray<UAnimGraphNode_Base*> Nodes;
	CollectAnimNodes(AnimBlueprint, Nodes);

	TArray<TSharedPtr<FJsonValue>> Functions;
	for (const UAnimGraphNode_Base* Node : Nodes)
	{
		TArray<FCortexAnimFunctionSlot> Slots;
		FCortexAnimBindingReflection::ReadFunctionSlots(Node, Slots);
		for (const FCortexAnimFunctionSlot& Slot : Slots)
		{
			const FMemberReference& Reference = *Slot.Reference;
			if (Reference.GetMemberName().IsNone())
			{
				continue;
			}

			// Resolved against the skeleton class: a Blueprint function and an inherited native function both resolve there.
			const UFunction* Function = Reference.ResolveMember<UFunction>(AnimBlueprint->SkeletonGeneratedClass.Get());
			const UClass* ParentClass = Reference.IsSelfContext() ? nullptr : Reference.GetMemberParentClass();

			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			SetNodeLocator(Entry, Node);
			Entry->SetStringField(TEXT("slot"), Slot.Name);
			Entry->SetStringField(TEXT("function_name"), Reference.GetMemberName().ToString());
			if (ParentClass != nullptr)
			{
				Entry->SetStringField(TEXT("owner_class"), ParentClass->GetPathName());
			}
			else
			{
				Entry->SetField(TEXT("owner_class"), MakeShared<FJsonValueNull>());
			}
			Entry->SetBoolField(TEXT("self_context"), Reference.IsSelfContext());
			Entry->SetBoolField(TEXT("resolved"), Function != nullptr);
			Entry->SetStringField(TEXT("declared_in"), Function != nullptr ? DeclaredIn(Function->GetOuterUClass()) : FString(TEXT("unresolved")));
			Functions.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	Data->SetObjectField(TEXT("node_functions"), FCortexAnimAssetUtils::MakeLimitedArray(Functions, Limit));
	return true;
}

bool FCortexAnimBindingReport::AppendPropertyAccess(UAnimBlueprint* AnimBlueprint, const int32 Limit, const TSharedPtr<FJsonObject>& Data, FCortexCommandResult& OutError)
{
	using namespace CortexAnimBindingReportPrivate;

	FString Missing;
	if (!FCortexAnimBindingReflection::CheckPropertyAccess(Missing))
	{
		OutError = UnsupportedField(Missing);
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Accesses;
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
			TArray<FString> Path;
			FString TextPath;
			if (!FCortexAnimBindingReflection::ReadPropertyAccessNode(Node, Path, TextPath))
			{
				continue;
			}

			const FPathResolution Resolution = ResolvePath(AnimBlueprint, Path);
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			SetNodeLocator(Entry, Node);
			Entry->SetArrayField(TEXT("path"), StringArray(Path));
			Entry->SetStringField(TEXT("text_path"), TextPath);
			SetResolution(Entry, Resolution);

			const UEdGraphPin* Output = Node->FindPin(TEXT("Value"), EGPD_Output);
			TArray<TSharedPtr<FJsonValue>> Linked;
			if (Output != nullptr)
			{
				Entry->SetObjectField(TEXT("pin_type"), PinTypeToJson(Output->PinType));
				for (const UEdGraphPin* Other : Output->LinkedTo)
				{
					if (Other == nullptr || Other->GetOwningNodeUnchecked() == nullptr)
					{
						continue;
					}
					TSharedRef<FJsonObject> Link = MakeShared<FJsonObject>();
					Link->SetStringField(TEXT("node_guid"), GuidText(Other->GetOwningNode()->NodeGuid));
					Link->SetStringField(TEXT("node_class"), Other->GetOwningNode()->GetClass()->GetName());
					Link->SetStringField(TEXT("pin_name"), Other->PinName.ToString());
					Linked.Add(MakeShared<FJsonValueObject>(Link));
				}
			}
			else
			{
				Entry->SetField(TEXT("pin_type"), MakeShared<FJsonValueNull>());
			}
			Entry->SetArrayField(TEXT("linked_to"), Linked);
			Accesses.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	Data->SetObjectField(TEXT("property_access"), FCortexAnimAssetUtils::MakeLimitedArray(Accesses, Limit));
	return true;
}
