#include "Misc/AutomationTest.h"

#include "CortexGraphMemberReferences.h"
#include "CortexGraphMemberReferencesTestTypes.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

namespace CortexGraphMemberReferencesTestPrivate
{
	UBlueprint* MakeBlueprint(const TCHAR* Name, UClass* Parent)
	{
		UPackage* Package = NewObject<UPackage>(nullptr, *FString::Printf(TEXT("/Temp/%s"), Name), RF_Transient);
		Package->SetPackageFlags(PKG_PlayInEditor);
		return FKismetEditorUtilities::CreateBlueprint(
			Parent, Package, FName(Name), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), NAME_None);
	}

	FEdGraphPinType BoolPin()
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		return PinType;
	}

	/** A get node on a member of another class (external scope). */
	UK2Node_VariableGet* AddExternalGet(UEdGraph* Graph, FName Name, UClass* MemberParent)
	{
		FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
		UK2Node_VariableGet* Node = Creator.CreateNode();
		Node->VariableReference.SetExternalMember(Name, MemberParent);
		Creator.Finalize();
		return Node;
	}

	UK2Node_VariableGet* AddGet(UEdGraph* Graph, FName Name, bool bLocalScope)
	{
		FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
		UK2Node_VariableGet* Node = Creator.CreateNode();
		if (bLocalScope)
		{
			Node->VariableReference.SetLocalMember(Name, Graph->GetName(), FGuid());
		}
		else
		{
			Node->VariableReference.SetSelfMember(Name);
		}
		Creator.Finalize();
		return Node;
	}

	UK2Node_VariableSet* AddSet(UEdGraph* Graph, FName Name)
	{
		FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
		UK2Node_VariableSet* Node = Creator.CreateNode();
		Node->VariableReference.SetSelfMember(Name);
		Creator.Finalize();
		return Node;
	}

	void Cleanup(UBlueprint* Blueprint)
	{
		if (Blueprint != nullptr)
		{
			Blueprint->MarkAsGarbage();
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMemberReferencesVariablesTest,
	"Cortex.Graph.MemberReferences.Variables",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMemberReferencesVariablesTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphMemberReferencesTestPrivate;

	UBlueprint* Blueprint = MakeBlueprint(TEXT("BP_MemberRefVariables"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}

	const FName Tracked(TEXT("Tracked"));
	const FName Other(TEXT("Other"));
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, Tracked, BoolPin());
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, Other, BoolPin());

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("Event graph"), EventGraph))
	{
		Cleanup(Blueprint);
		return false;
	}
	UK2Node_VariableGet* GetNode = AddGet(EventGraph, Tracked, false);
	UK2Node_VariableSet* SetNode = AddSet(EventGraph, Tracked);
	UK2Node_VariableGet* OtherNode = AddGet(EventGraph, Other, false);

	// A function graph holding a local variable with the same name as the member.
	UEdGraph* FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, TEXT("LocalHost"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, FunctionGraph, true, nullptr);
	UK2Node_VariableGet* LocalNode = AddGet(FunctionGraph, Tracked, true);

	TArray<FCortexMemberReference> References;
	FCortexGraphMemberReferences::FindVariableReferences(Blueprint, Tracked, References);

	TestEqual(TEXT("Get and set nodes are returned, the local one is not"), References.Num(), 2);
	bool bFoundGet = false;
	bool bFoundSet = false;
	for (const FCortexMemberReference& Reference : References)
	{
		TestEqual(TEXT("Kind"), Reference.Kind, FString(TEXT("k2_variable_node")));
		TestTrue(TEXT("Self context"), Reference.bSelfContext);
		TestEqual(TEXT("Graph guid"), Reference.GraphGuid, EventGraph->GraphGuid);
		bFoundGet |= Reference.NodeGuid == GetNode->NodeGuid;
		bFoundSet |= Reference.NodeGuid == SetNode->NodeGuid;
		TestFalse(TEXT("Local node excluded"), Reference.NodeGuid == LocalNode->NodeGuid);
		TestFalse(TEXT("Unrelated node excluded"), Reference.NodeGuid == OtherNode->NodeGuid);
	}
	TestTrue(TEXT("Get node found"), bFoundGet);
	TestTrue(TEXT("Set node found"), bFoundSet);

	TSharedPtr<FJsonObject> Json = FCortexGraphMemberReferences::ToJson(Blueprint, Tracked, References);
	TestTrue(TEXT("Json"), Json.IsValid());
	if (Json.IsValid())
	{
		bool bNative = true;
		TestTrue(TEXT("native_parent_declares field"), Json->TryGetBoolField(TEXT("native_parent_declares"), bNative));
		TestFalse(TEXT("AActor does not declare Tracked"), bNative);
		int32 Count = 0;
		TestTrue(TEXT("reference_count field"), Json->TryGetNumberField(TEXT("reference_count"), Count));
		TestEqual(TEXT("reference_count"), Count, 2);
	}

	// A same-named member of a different Blueprint: the node names that class, so it is not a reference to the owner's
	// member (it is exactly what the engine's RemoveMemberVariable leaves alone).
	UBlueprint* Foreign = MakeBlueprint(TEXT("BP_MemberRefForeign"), AActor::StaticClass());
	if (TestNotNull(TEXT("Foreign Blueprint"), Foreign))
	{
		FBlueprintEditorUtils::AddMemberVariable(Foreign, Other, BoolPin());
		FKismetEditorUtilities::CompileBlueprint(Foreign);
		UK2Node_VariableGet* ForeignNode = AddExternalGet(EventGraph, Other, Foreign->GeneratedClass);
		TestNotNull(TEXT("Foreign node"), ForeignNode);

		TArray<FCortexMemberReference> OtherReferences;
		FCortexGraphMemberReferences::FindVariableReferences(Blueprint, Other, OtherReferences);
		TestEqual(TEXT("Unrelated variable has one reference"), OtherReferences.Num(), 1);
		for (const FCortexMemberReference& Reference : OtherReferences)
		{
			TestFalse(TEXT("Same-named member of another class is not a hit"), Reference.NodeGuid == ForeignNode->NodeGuid);
			TestTrue(TEXT("Hit resolves to the owner"), Reference.bResolvesToOwner);
		}
	}
	if (Foreign != nullptr)
	{
		Foreign->MarkAsGarbage();
	}

	// Native parent declaring the same name.
	UBlueprint* NativeChild = MakeBlueprint(TEXT("BP_MemberRefNativeChild"), ACortexGraphMemberRefNativeActor::StaticClass());
	if (TestNotNull(TEXT("Native child"), NativeChild))
	{
		TSharedPtr<FJsonObject> NativeJson = FCortexGraphMemberReferences::ToJson(NativeChild, TEXT("bNativeFlag"), {});
		bool bDeclares = false;
		TestTrue(TEXT("native field"), NativeJson->TryGetBoolField(TEXT("native_parent_declares"), bDeclares));
		TestTrue(TEXT("native parent declares bNativeFlag"), bDeclares);
	}

	Cleanup(NativeChild);
	Cleanup(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMemberReferencesFunctionsTest,
	"Cortex.Graph.MemberReferences.Functions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMemberReferencesFunctionsTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphMemberReferencesTestPrivate;

	UBlueprint* Blueprint = MakeBlueprint(TEXT("BP_MemberRefFunctions"), AActor::StaticClass());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}

	const FName FunctionName(TEXT("MyFunction"));
	UEdGraph* FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, FunctionName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, FunctionGraph, true, nullptr);

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("Event graph"), EventGraph))
	{
		Cleanup(Blueprint);
		return false;
	}

	FGraphNodeCreator<UK2Node_CallFunction> OwnCreator(*EventGraph);
	UK2Node_CallFunction* OwnCall = OwnCreator.CreateNode();
	OwnCall->FunctionReference.SetSelfMember(FunctionName);
	OwnCreator.Finalize();

	FGraphNodeCreator<UK2Node_CallFunction> OtherCreator(*EventGraph);
	UK2Node_CallFunction* OtherCall = OtherCreator.CreateNode();
	OtherCall->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString)));
	OtherCreator.Finalize();

	TArray<FCortexMemberReference> References;
	FCortexGraphMemberReferences::FindFunctionReferences(Blueprint, FunctionName, References);

	TestEqual(TEXT("Exactly one reference"), References.Num(), 1);
	if (References.Num() == 1)
	{
		TestEqual(TEXT("Kind"), References[0].Kind, FString(TEXT("other_k2_node")));
		TestEqual(TEXT("Node guid"), References[0].NodeGuid, OwnCall->NodeGuid);
		TestEqual(TEXT("Graph guid"), References[0].GraphGuid, EventGraph->GraphGuid);
		TestFalse(TEXT("Unrelated call excluded"), References[0].NodeGuid == OtherCall->NodeGuid);
	}

	// A Create Event / Create Delegate node that binds the function is a reference too.
	FGraphNodeCreator<UK2Node_CreateDelegate> DelegateCreator(*EventGraph);
	UK2Node_CreateDelegate* DelegateNode = DelegateCreator.CreateNode();
	DelegateCreator.Finalize();
	DelegateNode->SetFunction(FunctionName);
	References.Reset();
	FCortexGraphMemberReferences::FindFunctionReferences(Blueprint, FunctionName, References);
	TestEqual(TEXT("Create Event binding is counted"), References.Num(), 2);
	TestTrue(TEXT("Create Event node is listed"), References.ContainsByPredicate(
		[DelegateNode](const FCortexMemberReference& Reference) { return Reference.NodeGuid == DelegateNode->NodeGuid; }));

	Cleanup(Blueprint);
	return true;
}
