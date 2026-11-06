#include "Misc/AutomationTest.h"

#include "AnimGraphNode_SequencePlayer.h"
#include "AnimationGraph.h"
#include "CortexAnimationTestAnimBlueprint.h"
#include "CortexAnimationTestNativeInstance.h"
#include "CortexGraphMemberReferences.h"
#include "Operations/CortexAnimBindingReflection.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
using namespace CortexAnimTest;

TSharedPtr<FJsonObject> EntryForNode(const TSharedPtr<FJsonObject>& Data, const TCHAR* Collection, const FGuid& NodeGuid, const TCHAR* Field = nullptr, const FString& Value = FString())
{
	for (const TSharedPtr<FJsonObject>& Entry : CollectionItems(Data, Collection))
	{
		FString NodeGuidText;
		if (!Entry->TryGetStringField(TEXT("node_guid"), NodeGuidText) || !SameGuid(NodeGuidText, NodeGuid))
		{
			continue;
		}
		if (Field != nullptr)
		{
			FString Actual;
			if (!Entry->TryGetStringField(Field, Actual) || Actual != Value)
			{
				continue;
			}
		}
		return Entry;
	}
	return nullptr;
}

TArray<FString> StringValues(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	TArray<FString> Values;
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (Object.IsValid() && Object->TryGetArrayField(Field, Array) && Array != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			FString Text;
			if (Value.IsValid() && Value->TryGetString(Text))
			{
				Values.Add(Text);
			}
		}
	}
	return Values;
}

FString Serialize(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	if (Object.IsValid())
	{
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Text;
}

FCortexCommandResult AnimBlueprintInfo(FCortexCommandRouter& Router, const UAnimBlueprint* AnimBlueprint, const TArray<FString>& Include)
{
	TSharedPtr<FJsonObject> Params = MakeAssetParams(AnimBlueprint);
	if (Include.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Entry : Include)
		{
			Values.Add(MakeShared<FJsonValueString>(Entry));
		}
		Params->SetArrayField(TEXT("include"), Values);
	}
	return Router.Execute(TEXT("anim.get_animbp_info"), Params);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationAnimBlueprintBindingsTest,
	"Cortex.Animation.AnimBlueprint.Bindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationAnimBlueprintBindingsTest::RunTest(const FString& Parameters)
{
	UAnimBlueprint* AnimBlueprint = MakeAnimBlueprint(MakeSuffix(), UCortexAnimTestNativeInstance::StaticClass());
	if (!TestNotNull(TEXT("Anim Blueprint fixture"), AnimBlueprint))
	{
		return false;
	}
	FFixtureGuard Guard(AnimBlueprint);
	TestTrue(TEXT("the parent is the native test instance"), AnimBlueprint->ParentClass == UCortexAnimTestNativeInstance::StaticClass());
	TestNotNull(TEXT("the skeleton class inherits NativeSpeed"), FindFProperty<FProperty>(AnimBlueprint->SkeletonGeneratedClass, TEXT("NativeSpeed")));

	TestTrue(TEXT("Speed variable"), AddVariable(AnimBlueprint, TEXT("Speed"), FloatPinType(), TEXT("0.0")));
	UEdGraph* OnUpdate = AddBlueprintFunction(AnimBlueprint, TEXT("OnUpdate"));
	UAnimGraphNode_SequencePlayer* BoundToBlueprint = AddSequencePlayerNode(AnimBlueprint);
	UAnimGraphNode_SequencePlayer* BoundToNative = AddSequencePlayerNode(AnimBlueprint);
	UAnimGraphNode_SequencePlayer* Unresolvable = AddSequencePlayerNode(AnimBlueprint);
	UAnimGraphNode_SequencePlayer* NoStorage = AddSequencePlayerNode(AnimBlueprint);
	if (!TestTrue(TEXT("sequence players"), BoundToBlueprint != nullptr && BoundToNative != nullptr && Unresolvable != nullptr && NoStorage != nullptr && OnUpdate != nullptr))
	{
		return false;
	}

	TestTrue(TEXT("Blueprint binding"), BindPropertyByReflection(BoundToBlueprint, TEXT("PlayRate"), { TEXT("Speed") }));
	TestTrue(TEXT("native binding"), BindPropertyByReflection(BoundToNative, TEXT("PlayRate"), { TEXT("NativeSpeed") }));
	TestTrue(TEXT("unresolvable binding"), BindPropertyByReflection(Unresolvable, TEXT("PlayRate"), { TEXT("NoSuchVariable") }));
	BoundToBlueprint->UpdateFunction.SetSelfMember(TEXT("OnUpdate"));

	// The engine's own virtuals are the oracle: the fixture really is what the report is asked to describe.
	TestTrue(TEXT("engine HasBinding agrees"), BoundToBlueprint->HasBinding(TEXT("PlayRate")));
	TestTrue(TEXT("engine ReferencesVariable agrees"), BoundToBlueprint->ReferencesVariable(TEXT("Speed"), nullptr));
	TestFalse(TEXT("engine ReferencesVariable rejects a foreign name"), BoundToBlueprint->ReferencesVariable(TEXT("NativeSpeed"), nullptr));

	// A node whose binding storage is not the default class: no Binding object at all.
	const FObjectProperty* BindingProperty = FindFProperty<FObjectProperty>(NoStorage->GetClass(), TEXT("Binding"));
	if (TestNotNull(TEXT("Binding is reflected"), BindingProperty))
	{
		BindingProperty->SetObjectPropertyValue_InContainer(NoStorage, nullptr);
		TestTrue(TEXT("the binding object is gone"), BindingProperty->GetObjectPropertyValue_InContainer(NoStorage) == nullptr);
		TestFalse(TEXT("a node without a binding object reports no binding"), NoStorage->HasBinding(TEXT("PlayRate")));
	}

	FCortexCommandRouter Router = MakeAnimGraphRouter();

	// ── Absent include: payload identical to the current one ──
	{
		const FCortexCommandResult Plain = AnimBlueprintInfo(Router, AnimBlueprint, {});
		if (TestTrue(TEXT("plain info succeeds"), Plain.bSuccess) && TestTrue(TEXT("plain data"), Plain.Data.IsValid()))
		{
			TestFalse(TEXT("no bindings without include"), Plain.Data->HasField(TEXT("bindings")));
			TestFalse(TEXT("no node_functions without include"), Plain.Data->HasField(TEXT("node_functions")));
			TestFalse(TEXT("no property_access without include"), Plain.Data->HasField(TEXT("property_access")));
			TestFalse(TEXT("no unsupported_binding_nodes without include"), Plain.Data->HasField(TEXT("unsupported_binding_nodes")));
			const FCortexCommandResult Again = AnimBlueprintInfo(Router, AnimBlueprint, {});
			TestEqual(TEXT("the plain payload is stable"), Serialize(Plain.Data), Serialize(Again.Data));
		}
	}

	// ── Unknown include entry ──
	{
		const FCortexCommandResult Unknown = AnimBlueprintInfo(Router, AnimBlueprint, { TEXT("bindings"), TEXT("bogus_section") });
		TestFalse(TEXT("an unknown include entry is refused"), Unknown.bSuccess);
		TestEqual(TEXT("INVALID_FIELD"), Unknown.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// ── bindings + node_functions ──
	const FCortexCommandResult Info = AnimBlueprintInfo(Router, AnimBlueprint, { TEXT("bindings"), TEXT("node_functions") });
	if (!TestTrue(TEXT("info with include succeeds"), Info.bSuccess) || !TestTrue(TEXT("data"), Info.Data.IsValid()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> BlueprintBinding = EntryForNode(Info.Data, TEXT("bindings"), BoundToBlueprint->NodeGuid, TEXT("property_name"), TEXT("PlayRate"));
	if (TestTrue(TEXT("the Blueprint-bound node is reported"), BlueprintBinding.IsValid()))
	{
		TestEqual(TEXT("type"), BlueprintBinding->GetStringField(TEXT("type")), FString(TEXT("property")));
		TestTrue(TEXT("path"), StringValues(BlueprintBinding, TEXT("path")) == TArray<FString>{ TEXT("Speed") });
		TestEqual(TEXT("root_declared_in"), BlueprintBinding->GetStringField(TEXT("root_declared_in")), FString(TEXT("blueprint")));
		TestTrue(TEXT("resolved"), BlueprintBinding->GetBoolField(TEXT("resolved")));
		const TSharedPtr<FJsonObject>* Leaf = nullptr;
		if (TestTrue(TEXT("leaf"), BlueprintBinding->TryGetObjectField(TEXT("leaf"), Leaf)))
		{
			TestEqual(TEXT("leaf name"), (*Leaf)->GetStringField(TEXT("name")), FString(TEXT("Speed")));
			TestEqual(TEXT("leaf cpp_type"), (*Leaf)->GetStringField(TEXT("cpp_type")), FString(TEXT("float")));
		}
		TestTrue(TEXT("node graph_guid is the AnimGraph"), SameGuid(BlueprintBinding->GetStringField(TEXT("graph_guid")), FindRootAnimGraph(AnimBlueprint)->GraphGuid));
		TestEqual(TEXT("binding_storage"), BlueprintBinding->GetStringField(TEXT("binding_storage")), FString(TEXT("default")));
	}

	const TSharedPtr<FJsonObject> NativeBinding = EntryForNode(Info.Data, TEXT("bindings"), BoundToNative->NodeGuid, TEXT("property_name"), TEXT("PlayRate"));
	if (TestTrue(TEXT("the native-bound node is reported"), NativeBinding.IsValid()))
	{
		TestEqual(TEXT("root_declared_in"), NativeBinding->GetStringField(TEXT("root_declared_in")), FString(TEXT("native")));
		TestTrue(TEXT("resolved"), NativeBinding->GetBoolField(TEXT("resolved")));
	}

	const TSharedPtr<FJsonObject> UnresolvedBinding = EntryForNode(Info.Data, TEXT("bindings"), Unresolvable->NodeGuid, TEXT("property_name"), TEXT("PlayRate"));
	if (TestTrue(TEXT("an unresolvable path is still reported"), UnresolvedBinding.IsValid()))
	{
		TestFalse(TEXT("resolved false"), UnresolvedBinding->GetBoolField(TEXT("resolved")));
		TestEqual(TEXT("root_declared_in"), UnresolvedBinding->GetStringField(TEXT("root_declared_in")), FString(TEXT("unresolved")));
	}

	const TArray<TSharedPtr<FJsonObject>> Unsupported = JsonObjectArray(Info.Data, TEXT("unsupported_binding_nodes"));
	if (TestEqual(TEXT("exactly one node reports unsupported storage"), Unsupported.Num(), 1))
	{
		FString NodeGuidText;
		Unsupported[0]->TryGetStringField(TEXT("node_guid"), NodeGuidText);
		TestTrue(TEXT("the node without a binding object"), SameGuid(NodeGuidText, NoStorage->NodeGuid));
		TestEqual(TEXT("binding_storage"), Unsupported[0]->GetStringField(TEXT("binding_storage")), FString(TEXT("unsupported")));
	}
	TestFalse(TEXT("the unsupported node has no binding entries"), EntryForNode(Info.Data, TEXT("bindings"), NoStorage->NodeGuid).IsValid());

	const TSharedPtr<FJsonObject> UpdateSlot = EntryForNode(Info.Data, TEXT("node_functions"), BoundToBlueprint->NodeGuid, TEXT("slot"), TEXT("update"));
	if (TestTrue(TEXT("the update slot is reported"), UpdateSlot.IsValid()))
	{
		TestEqual(TEXT("function_name"), UpdateSlot->GetStringField(TEXT("function_name")), FString(TEXT("OnUpdate")));
		TestEqual(TEXT("declared_in"), UpdateSlot->GetStringField(TEXT("declared_in")), FString(TEXT("blueprint")));
		TestTrue(TEXT("resolved"), UpdateSlot->GetBoolField(TEXT("resolved")));
		TestTrue(TEXT("self context"), UpdateSlot->GetBoolField(TEXT("self_context")));
	}
	TestFalse(TEXT("an unset slot is not reported"), EntryForNode(Info.Data, TEXT("node_functions"), BoundToBlueprint->NodeGuid, TEXT("slot"), TEXT("initial_update")).IsValid());

	// ── A missing reflected field fails the report naming the field ──
	{
		FCortexAnimBindingReflection::SetFieldNameForTest(TEXT("AnimGraphNodeBinding_Base.PropertyBindings"), TEXT("PropertyBindingsGone"));
		const FCortexCommandResult Broken = AnimBlueprintInfo(Router, AnimBlueprint, { TEXT("bindings") });
		FCortexAnimBindingReflection::ClearFieldNameOverridesForTest();
		TestFalse(TEXT("a missing reflected field fails the call"), Broken.bSuccess);
		TestEqual(TEXT("UNSUPPORTED_OPERATION"), Broken.ErrorCode, CortexErrorCodes::UnsupportedOperation);
		FString Missing;
		if (Broken.ErrorDetails.IsValid())
		{
			Broken.ErrorDetails->TryGetStringField(TEXT("missing_field"), Missing);
		}
		TestEqual(TEXT("the missing field is named"), Missing, FString(TEXT("AnimGraphNodeBinding_Base.PropertyBindings")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationAnimBlueprintPropertyAccessTest,
	"Cortex.Animation.AnimBlueprint.PropertyAccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationAnimBlueprintPropertyAccessTest::RunTest(const FString& Parameters)
{
	UAnimBlueprint* AnimBlueprint = MakeAnimBlueprint(MakeSuffix());
	if (!TestNotNull(TEXT("Anim Blueprint fixture"), AnimBlueprint))
	{
		return false;
	}
	FFixtureGuard Guard(AnimBlueprint);

	TestTrue(TEXT("Speed variable"), AddVariable(AnimBlueprint, TEXT("Speed"), FloatPinType(), TEXT("0.0")));
	UAnimationGraph* AnimGraph = FindRootAnimGraph(AnimBlueprint);
	UAnimGraphNode_SequencePlayer* Player = AddSequencePlayerNode(AnimBlueprint);
	if (!TestTrue(TEXT("fixture nodes"), AnimGraph != nullptr && Player != nullptr))
	{
		return false;
	}

	UEdGraphNode* Access = AddPropertyAccessNode(AnimGraph, { TEXT("Speed") });
	if (!TestNotNull(TEXT("property-access node"), Access))
	{
		return false;
	}
	UEdGraphPin* Output = Access->FindPin(TEXT("Value"), EGPD_Output);
	UEdGraphPin* PlayRate = Player->FindPin(TEXT("PlayRate"));
	if (TestTrue(TEXT("value pin and PlayRate pin exist"), Output != nullptr && PlayRate != nullptr))
	{
		TestTrue(TEXT("the value pin is linked to PlayRate"), AnimGraph->GetSchema()->TryCreateConnection(Output, PlayRate));
	}

	FCortexCommandRouter Router = MakeAnimGraphRouter();
	const FCortexCommandResult Info = AnimBlueprintInfo(Router, AnimBlueprint, { TEXT("property_access") });
	if (!TestTrue(TEXT("info with property_access succeeds"), Info.bSuccess) || !TestTrue(TEXT("data"), Info.Data.IsValid()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Entry = EntryForNode(Info.Data, TEXT("property_access"), Access->NodeGuid);
	if (!TestTrue(TEXT("the property-access node is reported"), Entry.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("path"), StringValues(Entry, TEXT("path")) == TArray<FString>{ TEXT("Speed") });
	TestEqual(TEXT("text_path"), Entry->GetStringField(TEXT("text_path")), FString(TEXT("Speed")));
	TestTrue(TEXT("resolved"), Entry->GetBoolField(TEXT("resolved")));
	TestEqual(TEXT("root_declared_in"), Entry->GetStringField(TEXT("root_declared_in")), FString(TEXT("blueprint")));
	TestTrue(TEXT("graph_guid"), SameGuid(Entry->GetStringField(TEXT("graph_guid")), AnimGraph->GraphGuid));

	const TSharedPtr<FJsonObject>* PinType = nullptr;
	if (TestTrue(TEXT("pin_type"), Entry->TryGetObjectField(TEXT("pin_type"), PinType)))
	{
		TestEqual(TEXT("pin category"), (*PinType)->GetStringField(TEXT("category")), UEdGraphSchema_K2::PC_Real.ToString());
	}

	const TArray<TSharedPtr<FJsonObject>> Linked = JsonObjectArray(Entry, TEXT("linked_to"));
	if (TestEqual(TEXT("one link"), Linked.Num(), 1))
	{
		FString LinkedGuid;
		Linked[0]->TryGetStringField(TEXT("node_guid"), LinkedGuid);
		TestTrue(TEXT("linked to the sequence player"), SameGuid(LinkedGuid, Player->NodeGuid));
		TestEqual(TEXT("linked pin"), Linked[0]->GetStringField(TEXT("pin_name")), FString(TEXT("PlayRate")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationMemberReferencesAnimKindsTest,
	"Cortex.Graph.MemberReferences.AnimKinds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationMemberReferencesAnimKindsTest::RunTest(const FString& Parameters)
{
	UAnimBlueprint* AnimBlueprint = MakeAnimBlueprint(MakeSuffix(), UCortexAnimTestNativeInstance::StaticClass());
	if (!TestNotNull(TEXT("Anim Blueprint fixture"), AnimBlueprint))
	{
		return false;
	}
	FFixtureGuard Guard(AnimBlueprint);
	TestTrue(TEXT("the parent is the native test instance"), AnimBlueprint->ParentClass == UCortexAnimTestNativeInstance::StaticClass());
	TestNotNull(TEXT("the skeleton class inherits NativeSpeed"), FindFProperty<FProperty>(AnimBlueprint->SkeletonGeneratedClass, TEXT("NativeSpeed")));

	TestTrue(TEXT("Speed variable"), AddVariable(AnimBlueprint, TEXT("Speed"), FloatPinType(), TEXT("0.0")));
	UEdGraph* OnUpdate = AddBlueprintFunction(AnimBlueprint, TEXT("OnUpdate"));
	UAnimationGraph* AnimGraph = FindRootAnimGraph(AnimBlueprint);
	UAnimGraphNode_SequencePlayer* Bound = AddSequencePlayerNode(AnimBlueprint);
	if (!TestTrue(TEXT("fixture nodes"), OnUpdate != nullptr && AnimGraph != nullptr && Bound != nullptr))
	{
		return false;
	}
	TestTrue(TEXT("binding"), BindPropertyByReflection(Bound, TEXT("PlayRate"), { TEXT("Speed") }));
	Bound->UpdateFunction.SetSelfMember(TEXT("OnUpdate"));
	UEdGraphNode* Access = AddPropertyAccessNode(AnimGraph, { TEXT("Speed") });
	if (!TestNotNull(TEXT("property-access node"), Access))
	{
		return false;
	}

	// Variables: the anim-node binding and the property-access node both name Speed, with the binding's pin as its slot.
	TArray<FCortexMemberReference> VariableReferences;
	FCortexGraphMemberReferences::FindVariableReferences(AnimBlueprint, TEXT("Speed"), VariableReferences);
	const FCortexMemberReference* BindingReference = VariableReferences.FindByPredicate([Bound](const FCortexMemberReference& Reference)
	{
		return Reference.NodeGuid == Bound->NodeGuid;
	});
	if (TestNotNull(TEXT("the anim-node binding is returned"), BindingReference))
	{
		TestEqual(TEXT("kind"), BindingReference->Kind, FString(TEXT("anim_node_binding")));
		TestEqual(TEXT("slot is the bound pin"), BindingReference->Slot, FString(TEXT("PlayRate")));
		TestTrue(TEXT("graph_guid is the nested AnimGraph"), BindingReference->GraphGuid == AnimGraph->GraphGuid);
		TestEqual(TEXT("graph_name"), BindingReference->GraphName, AnimGraph->GetName());
	}
	const FCortexMemberReference* AccessReference = VariableReferences.FindByPredicate([Access](const FCortexMemberReference& Reference)
	{
		return Reference.NodeGuid == Access->NodeGuid;
	});
	if (TestNotNull(TEXT("the property-access node is returned"), AccessReference))
	{
		TestEqual(TEXT("kind"), AccessReference->Kind, FString(TEXT("property_access_node")));
		TestTrue(TEXT("graph_guid"), AccessReference->GraphGuid == AnimGraph->GraphGuid);
	}

	for (const FCortexMemberReference& Reference : VariableReferences)
	{
		TestFalse(TEXT("the get node does not exist in this fixture"), Reference.Kind == TEXT("k2_variable_node"));
	}

	// Functions: the update slot names OnUpdate.
	TArray<FCortexMemberReference> FunctionReferences;
	FCortexGraphMemberReferences::FindFunctionReferences(AnimBlueprint, TEXT("OnUpdate"), FunctionReferences);
	const FCortexMemberReference* SlotReference = FunctionReferences.FindByPredicate([Bound](const FCortexMemberReference& Reference)
	{
		return Reference.NodeGuid == Bound->NodeGuid;
	});
	if (TestNotNull(TEXT("the function slot is returned"), SlotReference))
	{
		TestEqual(TEXT("kind"), SlotReference->Kind, FString(TEXT("anim_node_function")));
		TestEqual(TEXT("slot"), SlotReference->Slot, FString(TEXT("UpdateFunction")));
	}

	// The JSON shape carries the same kind and slot.
	const TSharedPtr<FJsonObject> Json = FCortexGraphMemberReferences::ToJson(AnimBlueprint, TEXT("Speed"), VariableReferences);
	const TArray<TSharedPtr<FJsonObject>> References = JsonObjectArray(Json, TEXT("references"));
	TestTrue(TEXT("native_parent_declares is false for a Blueprint variable"), Json->HasField(TEXT("native_parent_declares")) && !Json->GetBoolField(TEXT("native_parent_declares")));
	const TSharedPtr<FJsonObject> BindingJson = FindJsonObjectByGuid(References, TEXT("node_guid"), Bound->NodeGuid);
	if (TestTrue(TEXT("the binding is in the JSON"), BindingJson.IsValid()))
	{
		TestEqual(TEXT("json kind"), BindingJson->GetStringField(TEXT("kind")), FString(TEXT("anim_node_binding")));
		TestEqual(TEXT("json slot"), BindingJson->GetStringField(TEXT("slot")), FString(TEXT("PlayRate")));
	}

	return true;
}
