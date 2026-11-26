#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "CortexGraphTestContentRoot.h"
#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Factories/AnimBlueprintFactory.h"
#include "GameFramework/Actor.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

/**
 * Issue #232: Property Access nodes (UK2Node_PropertyAccess, PropertyAccessNode plugin). Reads expose the path and its
 * thread safety; add_node and apply_patch create the node from a validated `path`. The node class lives in a plugin
 * CortexGraph does not link, so the tests reach it by path and read its private `Path` by reflection.
 */
namespace CortexGraphPropertyAccessTest
{
	constexpr const TCHAR* NodeClassPath = TEXT("/Script/PropertyAccessNode.K2Node_PropertyAccess");
	constexpr const TCHAR* FunctionGraphName = TEXT("ReadState");
	constexpr const TCHAR* OwnerFunctionGraphName = TEXT("ReadOwner");
	constexpr const TCHAR* MacroGraphName = TEXT("AccessMacro");

	void Retire(UBlueprint* Blueprint)
	{
		if (Blueprint == nullptr)
		{
			return;
		}
		UPackage* Package = Blueprint->GetPackage();
		if (Blueprint->GeneratedClass != nullptr)
		{
			Blueprint->GeneratedClass->MarkAsGarbage();
		}
		if (Blueprint->SkeletonGeneratedClass != nullptr)
		{
			Blueprint->SkeletonGeneratedClass->MarkAsGarbage();
		}
		Blueprint->ClearFlags(RF_Public | RF_Standalone);
		Blueprint->MarkAsGarbage();
		if (Package != nullptr)
		{
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
		}
	}

	/**
	 * A template Anim Blueprint with bool members bIsDead and bCopy, empty thread-safe function graphs ReadState and
	 * ReadOwner, and an empty macro graph AccessMacro.
	 */
	struct FFixture
	{
		UAnimBlueprint* AnimBlueprint = nullptr;
		TArray<UBlueprint*> Extra;
		FString OwnedPackageName;

		bool Create()
		{
			UPackage* Package = CreateCortexGraphTestPackage(TEXT("ABP_Issue232"), OwnedPackageName);
			TStrongObjectPtr<UAnimBlueprintFactory> Factory(NewObject<UAnimBlueprintFactory>());
			Factory->BlueprintType = BPTYPE_Normal;
			Factory->ParentClass = UAnimInstance::StaticClass();
			Factory->TargetSkeleton = nullptr;
			Factory->bTemplate = true;
			AnimBlueprint = Cast<UAnimBlueprint>(Factory->FactoryCreateNew(
				UAnimBlueprint::StaticClass(), Package, TEXT("ABP_Issue232"), RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
			if (AnimBlueprint == nullptr)
			{
				return false;
			}

			FEdGraphPinType BoolType;
			BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			FBlueprintEditorUtils::AddMemberVariable(AnimBlueprint, TEXT("bIsDead"), BoolType);
			FBlueprintEditorUtils::AddMemberVariable(AnimBlueprint, TEXT("bCopy"), BoolType);

			for (const TCHAR* Name : { FunctionGraphName, OwnerFunctionGraphName })
			{
				UEdGraph* Function = FBlueprintEditorUtils::CreateNewGraph(
					AnimBlueprint, Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
				FBlueprintEditorUtils::AddFunctionGraph<UClass>(AnimBlueprint, Function, true, nullptr);
				if (UK2Node_FunctionEntry* Entry = FunctionEntry(Name))
				{
					Entry->MetaData.bThreadSafe = true;
				}
			}
			UEdGraph* Macro = FBlueprintEditorUtils::CreateNewGraph(
				AnimBlueprint, MacroGraphName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			FBlueprintEditorUtils::AddMacroGraph(AnimBlueprint, Macro, true, nullptr);

			FKismetEditorUtilities::CompileBlueprint(AnimBlueprint, EBlueprintCompileOptions::SkipSave);
			return AnimBlueprint->Status == BS_UpToDate;
		}

		UBlueprint* CreateActorBlueprint()
		{
			FString PackageName;
			UPackage* Package = CreateCortexGraphTestPackage(TEXT("BP_Issue232Actor"), PackageName);
			UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
				AActor::StaticClass(), Package, TEXT("BP_Issue232Actor"), BPTYPE_Normal,
				UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
			Extra.Add(Blueprint);
			return Blueprint;
		}

		UAnimBlueprint* CreateChildAnimBlueprint()
		{
			FString PackageName;
			UPackage* Package = CreateCortexGraphTestPackage(TEXT("ABP_Issue232Child"), PackageName);
			UAnimBlueprint* Child = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
				AnimBlueprint->GeneratedClass, Package, TEXT("ABP_Issue232Child"), BPTYPE_Normal,
				UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass()));
			Extra.Add(Child);
			return Child;
		}

		UEdGraph* EventGraph() const
		{
			return AnimBlueprint->UbergraphPages.Num() > 0 ? AnimBlueprint->UbergraphPages[0] : nullptr;
		}

		UEdGraph* FunctionGraph(const TCHAR* Name = FunctionGraphName) const
		{
			for (UEdGraph* Graph : AnimBlueprint->FunctionGraphs)
			{
				if (Graph != nullptr && Graph->GetName() == Name)
				{
					return Graph;
				}
			}
			return nullptr;
		}

		UK2Node_FunctionEntry* FunctionEntry(const TCHAR* Name = FunctionGraphName) const
		{
			UEdGraph* Graph = FunctionGraph(Name);
			if (Graph == nullptr)
			{
				return nullptr;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
				{
					return Entry;
				}
			}
			return nullptr;
		}

		~FFixture()
		{
			if (GEditor != nullptr && GEditor->Trans != nullptr)
			{
				GEditor->Trans->Reset(FText::FromString(TEXT("Retire issue-232 fixtures")));
			}
			for (UBlueprint* Blueprint : Extra)
			{
				Retire(Blueprint);
			}
			Retire(AnimBlueprint);
		}
	};

	FCortexCommandRouter MakeRouter()
	{
		FCortexCommandRouter Router;
		Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"), MakeShared<FCortexGraphCommandHandler>());
		return Router;
	}

	TSharedPtr<FJsonObject> PathParams(std::initializer_list<const TCHAR*> Segments)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const TCHAR* Segment : Segments)
		{
			Values.Add(MakeShared<FJsonValueString>(Segment));
		}
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("path"), Values);
		return Params;
	}

	FCortexCommandResult AddNode(FCortexCommandRouter& Router, const UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& NodeParams,
		const TCHAR* GraphName = TEXT("EventGraph"))
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Params->SetStringField(TEXT("graph_name"), GraphName);
		Params->SetStringField(TEXT("node_class"), TEXT("PropertyAccess"));
		if (NodeParams.IsValid())
		{
			Params->SetObjectField(TEXT("params"), NodeParams);
		}
		return Router.Execute(TEXT("graph.add_node"), Params);
	}

	UEdGraphNode* FindNode(const UEdGraph* Graph, const FString& Name)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node != nullptr && Node->GetName() == Name)
			{
				return Node;
			}
		}
		return nullptr;
	}

	int32 CountNodes(UBlueprint* Blueprint)
	{
		int32 Count = 0;
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			Count += Graph != nullptr ? Graph->Nodes.Num() : 0;
		}
		return Count;
	}

	/** Independent oracle: the node's private Path read by reflection, joined with '.'. */
	FString NativePath(const UEdGraphNode* Node)
	{
		const FArrayProperty* PathProperty = Node != nullptr
			? CastField<FArrayProperty>(Node->GetClass()->FindPropertyByName(TEXT("Path"))) : nullptr;
		if (PathProperty == nullptr)
		{
			return TEXT("<no Path property>");
		}
		const TArray<FString>& Path = *PathProperty->ContainerPtrToValuePtr<TArray<FString>>(Node);
		return FString::Join(Path, TEXT("."));
	}

	FString JsonPath(const TSharedPtr<FJsonObject>& Access)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Access.IsValid() || !Access->TryGetArrayField(TEXT("path"), Values))
		{
			return TEXT("<missing>");
		}
		TArray<FString> Segments;
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			Segments.Add(Value.IsValid() ? Value->AsString() : FString());
		}
		return FString::Join(Segments, TEXT("."));
	}

	TSharedPtr<FJsonObject> AccessOf(const TSharedPtr<FJsonObject>& NodeJson)
	{
		const TSharedPtr<FJsonObject>* Access = nullptr;
		return NodeJson.IsValid() && NodeJson->TryGetObjectField(TEXT("property_access"), Access) ? *Access : nullptr;
	}

	TSharedPtr<FJsonObject> FindNodeJson(const TSharedPtr<FJsonObject>& Data, const TCHAR* ArrayField, const FString& NodeId)
	{
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (!Data.IsValid() || !Data->TryGetArrayField(ArrayField, Nodes))
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Nodes)
		{
			const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
			if (Object.IsValid() && Object->GetStringField(TEXT("node_id")) == NodeId)
			{
				return Object;
			}
		}
		return nullptr;
	}

	FString PinType(const TSharedPtr<FJsonObject>& Data, const TCHAR* PinName)
	{
		const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
		if (!Data.IsValid() || !Data->TryGetArrayField(TEXT("pins"), Pins))
		{
			return TEXT("<no pins>");
		}
		for (const TSharedPtr<FJsonValue>& Value : *Pins)
		{
			const TSharedPtr<FJsonObject> Pin = Value.IsValid() ? Value->AsObject() : nullptr;
			if (Pin.IsValid() && Pin->GetStringField(TEXT("name")) == PinName)
			{
				return Pin->GetStringField(TEXT("type"));
			}
		}
		return TEXT("<no pin>");
	}

	bool ExpectAccess(FAutomationTestBase& Test, const FString& Context, const TSharedPtr<FJsonObject>& NodeJson,
		const FString& ExpectedPath, bool bExpectedThreadSafe)
	{
		const TSharedPtr<FJsonObject> Access = AccessOf(NodeJson);
		if (!Test.TestTrue(*FString::Printf(TEXT("%s: property_access present"), *Context), Access.IsValid()))
		{
			return false;
		}
		Test.TestEqual(*FString::Printf(TEXT("%s: path"), *Context), JsonPath(Access), ExpectedPath);
		Test.TestTrue(*FString::Printf(TEXT("%s: resolved"), *Context), Access->GetBoolField(TEXT("resolved")));
		Test.TestEqual(*FString::Printf(TEXT("%s: thread_safe"), *Context), Access->GetBoolField(TEXT("thread_safe")), bExpectedThreadSafe);
		Test.TestFalse(*FString::Printf(TEXT("%s: text_path"), *Context), Access->GetStringField(TEXT("text_path")).IsEmpty());
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPropertyAccessDescribeTest,
	"Cortex.Graph.PropertyAccess.Describe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphPropertyAccessDescribeTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphPropertyAccessTest;
	FFixture Fixture;
	if (!TestTrue(TEXT("fixture Anim Blueprint compiles"), Fixture.Create()))
	{
		return false;
	}
	FCortexCommandRouter Router = MakeRouter();

	for (const TCHAR* Alias : { TEXT("PropertyAccess"), NodeClassPath })
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("node_class"), Alias);
		const FCortexCommandResult Result = Router.Execute(TEXT("graph.describe_node"), Params);
		if (!TestTrue(*FString::Printf(TEXT("describe_node %s succeeds: %s"), Alias, *Result.ErrorMessage), Result.bSuccess))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("%s resolves to the node class"), Alias),
			Result.Data->GetStringField(TEXT("resolved_class")), FString(TEXT("K2Node_PropertyAccess")));
		const TArray<TSharedPtr<FJsonValue>>& Required = Result.Data->GetArrayField(TEXT("required_params"));
		TestTrue(*FString::Printf(TEXT("%s requires path"), Alias),
			Required.Num() == 1 && Required[0]->AsObject()->GetStringField(TEXT("name")) == TEXT("path")
			&& Required[0]->AsObject()->GetStringField(TEXT("type")) == TEXT("array"));
	}

	{
		// Without an asset there is no Anim Blueprint class to resolve from: the family contract only, no probe.
		TSharedPtr<FJsonObject> FamilyOnly = MakeShared<FJsonObject>();
		FamilyOnly->SetStringField(TEXT("node_class"), TEXT("PropertyAccess"));
		FamilyOnly->SetObjectField(TEXT("params"), PathParams({ TEXT("bIsDead[") }));
		const FCortexCommandResult Result = Router.Execute(TEXT("graph.describe_node"), FamilyOnly);
		if (TestTrue(FString::Printf(TEXT("family-only describe with params succeeds: %s"), *Result.ErrorMessage), Result.bSuccess))
		{
			TestEqual(TEXT("family-only describe is not validated"), Result.Data->GetStringField(TEXT("validation_status")), FString(TEXT("family_only")));
			TestFalse(TEXT("family-only describe allocates no probe pins"), Result.Data->GetBoolField(TEXT("pins_allocated")));
		}
	}

	const bool bDirtyBefore = Fixture.AnimBlueprint->GetPackage()->IsDirty();
	const int32 NodesBefore = CountNodes(Fixture.AnimBlueprint);
	TSharedPtr<FJsonObject> Contextual = MakeShared<FJsonObject>();
	Contextual->SetStringField(TEXT("node_class"), TEXT("PropertyAccess"));
	Contextual->SetStringField(TEXT("asset_path"), Fixture.AnimBlueprint->GetPathName());
	Contextual->SetObjectField(TEXT("params"), PathParams({ TEXT("bIsDead") }));
	const FCortexCommandResult Described = Router.Execute(TEXT("graph.describe_node"), Contextual);
	if (TestTrue(FString::Printf(TEXT("contextual describe succeeds: %s"), *Described.ErrorMessage), Described.bSuccess))
	{
		TSharedPtr<FJsonObject> PinsHolder = MakeShared<FJsonObject>();
		PinsHolder->SetArrayField(TEXT("pins"), Described.Data->GetArrayField(TEXT("expected_pins")));
		TestEqual(TEXT("contextual describe types the Value pin from the leaf"), PinType(PinsHolder, TEXT("Value")), FString(TEXT("bool")));
	}
	TestEqual(TEXT("contextual describe adds no node"), CountNodes(Fixture.AnimBlueprint), NodesBefore);
	TestEqual(TEXT("contextual describe leaves the package dirty state"), Fixture.AnimBlueprint->GetPackage()->IsDirty(), bDirtyBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPropertyAccessAddAndReadTest,
	"Cortex.Graph.PropertyAccess.AddAndRead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphPropertyAccessAddAndReadTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphPropertyAccessTest;
	FFixture Fixture;
	if (!TestTrue(TEXT("fixture Anim Blueprint compiles"), Fixture.Create()))
	{
		return false;
	}
	FCortexCommandRouter Router = MakeRouter();

	const FCortexCommandResult Member = AddNode(Router, Fixture.AnimBlueprint, PathParams({ TEXT("bIsDead") }));
	const FCortexCommandResult Owner = AddNode(Router, Fixture.AnimBlueprint, PathParams({ TEXT("GetOwningActor"), TEXT("bHidden") }));
	if (!TestTrue(FString::Printf(TEXT("add_node member path: %s"), *Member.ErrorMessage), Member.bSuccess)
		|| !TestTrue(FString::Printf(TEXT("add_node owner path: %s"), *Owner.ErrorMessage), Owner.bSuccess))
	{
		return false;
	}

	const FString MemberId = Member.Data->GetStringField(TEXT("node_id"));
	const FString OwnerId = Owner.Data->GetStringField(TEXT("node_id"));
	TestEqual(TEXT("add_node reports the node class"), Member.Data->GetStringField(TEXT("class")), FString(TEXT("K2Node_PropertyAccess")));
	TestEqual(TEXT("member access Value pin is bool"), PinType(Member.Data, TEXT("Value")), FString(TEXT("bool")));
	TestEqual(TEXT("owner access Value pin is bool"), PinType(Owner.Data, TEXT("Value")), FString(TEXT("bool")));
	ExpectAccess(*this, TEXT("add_node member"), Member.Data, TEXT("bIsDead"), true);
	ExpectAccess(*this, TEXT("add_node owner"), Owner.Data, TEXT("GetOwningActor.bHidden"), false);

	UEdGraph* EventGraph = Fixture.EventGraph();
	TestEqual(TEXT("native member path"), NativePath(FindNode(EventGraph, MemberId)), FString(TEXT("bIsDead")));
	TestEqual(TEXT("native owner path"), NativePath(FindNode(EventGraph, OwnerId)), FString(TEXT("GetOwningActor.bHidden")));

	for (const bool bCompact : { true, false })
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Fixture.AnimBlueprint->GetPathName());
		Params->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
		Params->SetBoolField(TEXT("compact"), bCompact);
		const FCortexCommandResult Subgraph = Router.Execute(TEXT("graph.get_subgraph"), Params);
		const FString Mode = bCompact ? TEXT("compact") : TEXT("verbose");
		if (TestTrue(FString::Printf(TEXT("get_subgraph %s: %s"), *Mode, *Subgraph.ErrorMessage), Subgraph.bSuccess))
		{
			ExpectAccess(*this, FString::Printf(TEXT("get_subgraph %s member"), *Mode), FindNodeJson(Subgraph.Data, TEXT("nodes"), MemberId), TEXT("bIsDead"), true);
			ExpectAccess(*this, FString::Printf(TEXT("get_subgraph %s owner"), *Mode), FindNodeJson(Subgraph.Data, TEXT("nodes"), OwnerId), TEXT("GetOwningActor.bHidden"), false);
		}
	}

	TSharedPtr<FJsonObject> SearchParams = MakeShared<FJsonObject>();
	SearchParams->SetStringField(TEXT("asset_path"), Fixture.AnimBlueprint->GetPathName());
	SearchParams->SetStringField(TEXT("node_class"), TEXT("K2Node_PropertyAccess"));
	const FCortexCommandResult Search = Router.Execute(TEXT("graph.search_nodes"), SearchParams);
	if (TestTrue(FString::Printf(TEXT("search_nodes: %s"), *Search.ErrorMessage), Search.bSuccess))
	{
		TestEqual(TEXT("search_nodes finds both accesses"), static_cast<int32>(Search.Data->GetNumberField(TEXT("count"))), 2);
		ExpectAccess(*this, TEXT("search_nodes member"), FindNodeJson(Search.Data, TEXT("results"), MemberId), TEXT("bIsDead"), true);
		ExpectAccess(*this, TEXT("search_nodes owner"), FindNodeJson(Search.Data, TEXT("results"), OwnerId), TEXT("GetOwningActor.bHidden"), false);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPropertyAccessRefusalsTest,
	"Cortex.Graph.PropertyAccess.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphPropertyAccessRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphPropertyAccessTest;
	FFixture Fixture;
	if (!TestTrue(TEXT("fixture Anim Blueprint compiles"), Fixture.Create()))
	{
		return false;
	}
	FCortexCommandRouter Router = MakeRouter();

	struct FCase
	{
		const TCHAR* Name;
		TSharedPtr<FJsonObject> Params;
		FString Code;
		FString MessagePart;
	};

	TSharedPtr<FJsonObject> EmptyPath = MakeShared<FJsonObject>();
	EmptyPath->SetArrayField(TEXT("path"), TArray<TSharedPtr<FJsonValue>>());
	TSharedPtr<FJsonObject> NumberSegment = MakeShared<FJsonObject>();
	NumberSegment->SetArrayField(TEXT("path"), { MakeShared<FJsonValueNumber>(1) });
	TSharedPtr<FJsonObject> StringPath = MakeShared<FJsonObject>();
	StringPath->SetStringField(TEXT("path"), TEXT("bIsDead"));

	const TArray<FCase> Cases = {
		{ TEXT("missing path"), MakeShared<FJsonObject>(), CortexErrorCodes::InvalidField, TEXT("path") },
		{ TEXT("empty path"), EmptyPath, CortexErrorCodes::InvalidField, TEXT("path") },
		{ TEXT("non-string segment"), NumberSegment, CortexErrorCodes::InvalidField, TEXT("path") },
		{ TEXT("string instead of array"), StringPath, CortexErrorCodes::InvalidField, TEXT("path") },
		{ TEXT("unknown member"), PathParams({ TEXT("NoSuchMember") }), CortexErrorCodes::InvalidField, TEXT("NoSuchMember") },
		{ TEXT("unknown nested segment"), PathParams({ TEXT("GetOwningActor"), TEXT("NoSuchField") }), CortexErrorCodes::InvalidField, TEXT("NoSuchField") },
		{ TEXT("non-getter function"), PathParams({ TEXT("BlueprintUpdateAnimation") }), CortexErrorCodes::InvalidField, TEXT("BlueprintUpdateAnimation") },
		{ TEXT("unterminated index"), PathParams({ TEXT("bIsDead[") }), CortexErrorCodes::InvalidField, TEXT("path[0]") },
		{ TEXT("non-numeric index"), PathParams({ TEXT("bIsDead[x]") }), CortexErrorCodes::InvalidField, TEXT("path[0]") },
		{ TEXT("index on a non-array"), PathParams({ TEXT("bIsDead[0]") }), CortexErrorCodes::InvalidField, TEXT("not an array") },
		{ TEXT("indexed getter"), PathParams({ TEXT("GetOwningActor[0]"), TEXT("bHidden") }), CortexErrorCodes::InvalidField, TEXT("cannot be indexed") },
		{ TEXT("member of a scalar"), PathParams({ TEXT("bIsDead"), TEXT("Member") }), CortexErrorCodes::InvalidField, TEXT("'bIsDead' (index 0) is a bool") },
	};

	const int32 NodesBefore = CountNodes(Fixture.AnimBlueprint);
	for (const FCase& Case : Cases)
	{
		const FCortexCommandResult Result = AddNode(Router, Fixture.AnimBlueprint, Case.Params);
		TestFalse(*FString::Printf(TEXT("%s is refused"), Case.Name), Result.bSuccess);
		TestEqual(*FString::Printf(TEXT("%s error code"), Case.Name), Result.ErrorCode, Case.Code);
		TestTrue(*FString::Printf(TEXT("%s message names '%s': %s"), Case.Name, *Case.MessagePart, *Result.ErrorMessage),
			Result.ErrorMessage.Contains(Case.MessagePart));
	}
	TestEqual(TEXT("refused requests add no node"), CountNodes(Fixture.AnimBlueprint), NodesBefore);

	{
		const FCortexCommandResult Result = AddNode(Router, Fixture.AnimBlueprint, PathParams({ TEXT("GetOwningActor"), TEXT("bHidden") }), MacroGraphName);
		TestFalse(TEXT("a macro graph is refused"), Result.bSuccess);
		TestEqual(TEXT("a macro graph is an invalid operation"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
		TestEqual(TEXT("the macro refusal adds no node"), CountNodes(Fixture.AnimBlueprint), NodesBefore);
	}

	UBlueprint* ActorBlueprint = Fixture.CreateActorBlueprint();
	if (TestNotNull(TEXT("actor Blueprint created"), ActorBlueprint))
	{
		const int32 ActorNodesBefore = CountNodes(ActorBlueprint);
		const FCortexCommandResult Result = AddNode(Router, ActorBlueprint, PathParams({ TEXT("bHidden") }));
		TestFalse(TEXT("an Actor Blueprint is refused"), Result.bSuccess);
		TestEqual(TEXT("an Actor Blueprint is an invalid operation"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
		TestEqual(TEXT("the Actor Blueprint gains no node"), CountNodes(ActorBlueprint), ActorNodesBefore);
	}

	UAnimBlueprint* Child = Fixture.CreateChildAnimBlueprint();
	if (TestNotNull(TEXT("child Anim Blueprint created"), Child))
	{
		const int32 ChildNodesBefore = CountNodes(Child);
		const FCortexCommandResult Result = AddNode(Router, Child, PathParams({ TEXT("bIsDead") }));
		TestFalse(TEXT("a child Anim Blueprint is refused"), Result.bSuccess);
		TestEqual(TEXT("a child Anim Blueprint is an invalid operation"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
		TestEqual(TEXT("the child Anim Blueprint gains no node"), CountNodes(Child), ChildNodesBefore);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPropertyAccessPatchTest,
	"Cortex.Graph.PropertyAccess.Patch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphPropertyAccessPatchTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphPropertyAccessTest;
	FFixture Fixture;
	if (!TestTrue(TEXT("fixture Anim Blueprint compiles"), Fixture.Create()))
	{
		return false;
	}
	auto MakeRequest = [&Fixture](const TCHAR* PatchId, const UEdGraph* Graph, const TCHAR* GraphKind,
		const UK2Node_FunctionEntry* Entry, const TSharedPtr<FJsonObject>& AccessParams)
	{
		EnsureCortexGraphTestTempContentRoot();
		TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("asset_path"), Fixture.AnimBlueprint->GetPathName());
		Request->SetStringField(TEXT("patch_id"), PatchId);
		TSharedPtr<FJsonObject> GraphRef = MakeShared<FJsonObject>();
		GraphRef->SetStringField(TEXT("graph_guid"), Graph->GraphGuid.ToString());
		GraphRef->SetStringField(TEXT("graph_kind"), GraphKind);
		TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetObjectField(TEXT("graph_ref"), GraphRef);
		Request->SetObjectField(TEXT("target"), Target);
		Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Fixture.AnimBlueprint));

		auto Node = [](const TCHAR* ClientId, const TCHAR* NodeClass, const TSharedPtr<FJsonObject>& Params)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("client_id"), ClientId);
			Object->SetStringField(TEXT("node_class"), NodeClass);
			Object->SetObjectField(TEXT("params"), Params);
			return MakeShared<FJsonValueObject>(Object);
		};
		auto Endpoint = [](const TCHAR* Field, const FString& Identity, const TCHAR* Pin)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(Field, Identity);
			Object->SetStringField(TEXT("pin"), Pin);
			return Object;
		};
		auto Connection = [](const TSharedPtr<FJsonObject>& From, const TSharedPtr<FJsonObject>& To)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetObjectField(TEXT("from"), From);
			Object->SetObjectField(TEXT("to"), To);
			return MakeShared<FJsonValueObject>(Object);
		};

		TArray<TSharedPtr<FJsonValue>> Nodes = { Node(TEXT("access"), TEXT("PropertyAccess"), AccessParams) };
		TArray<TSharedPtr<FJsonValue>> Connections;
		if (Entry != nullptr)
		{
			TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
			SetParams->SetStringField(TEXT("variable_name"), TEXT("bCopy"));
			Nodes.Add(Node(TEXT("copy"), TEXT("VariableSet"), SetParams));
			Connections.Add(Connection(Endpoint(TEXT("node_guid"), Entry->NodeGuid.ToString(), TEXT("then")), Endpoint(TEXT("client_id"), TEXT("copy"), TEXT("execute"))));
			Connections.Add(Connection(Endpoint(TEXT("client_id"), TEXT("access"), TEXT("Value")), Endpoint(TEXT("client_id"), TEXT("copy"), TEXT("bCopy"))));
		}
		Request->SetArrayField(TEXT("nodes"), Nodes);
		Request->SetArrayField(TEXT("connections"), Connections);
		Request->SetBoolField(TEXT("dry_run"), true);
		Request->SetBoolField(TEXT("compile"), true);
		Request->SetBoolField(TEXT("save"), false);
		Request->SetBoolField(TEXT("allow_noop"), false);
		return Request;
	};

	const int32 NodesBefore = CountNodes(Fixture.AnimBlueprint);
	{
		TSharedPtr<FJsonObject> Bad = MakeRequest(TEXT("00000000-0000-0000-0000-000000000232"),
			Fixture.FunctionGraph(), TEXT("function"), Fixture.FunctionEntry(), PathParams({ TEXT("NoSuchMember") }));
		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult Error;
		TestFalse(TEXT("an unresolvable path is refused in preflight"), FCortexGraphPatchOps::Preflight(Fixture.AnimBlueprint, Bad, Prepared, Error));
		TestEqual(TEXT("an unresolvable path is INVALID_FIELD"), Error.ErrorCode, CortexErrorCodes::InvalidField);
	}
	{
		UEdGraph* Macro = nullptr;
		for (UEdGraph* Graph : Fixture.AnimBlueprint->MacroGraphs)
		{
			Macro = Graph != nullptr && Graph->GetName() == MacroGraphName ? Graph : Macro;
		}
		if (TestNotNull(TEXT("macro graph"), Macro))
		{
			TSharedPtr<FJsonObject> InMacro = MakeRequest(TEXT("00000000-0000-0000-0000-000000000235"),
				Macro, TEXT("macro"), nullptr, PathParams({ TEXT("GetOwningActor"), TEXT("bHidden") }));
			FCortexGraphPreparedPatch Prepared;
			FCortexCommandResult Error;
			TestFalse(TEXT("a macro graph target is refused in preflight"), FCortexGraphPatchOps::Preflight(Fixture.AnimBlueprint, InMacro, Prepared, Error));
			TestEqual(TEXT("a macro graph target is INVALID_OPERATION"), Error.ErrorCode, CortexErrorCodes::InvalidOperation);
		}
	}
	TestEqual(TEXT("preflight refusals add no node"), CountNodes(Fixture.AnimBlueprint), NodesBefore);

	struct FCase
	{
		const TCHAR* PatchId;
		const TCHAR* GraphName;
		TSharedPtr<FJsonObject> Params;
		FString ExpectedPath;
	};
	// A thread-safe member read, and the issue's owner-state read (not thread-safe: compiled through a cached copy).
	const TArray<FCase> Cases = {
		{ TEXT("00000000-0000-0000-0000-000000000233"), FunctionGraphName, PathParams({ TEXT("bIsDead") }), TEXT("bIsDead") },
		{ TEXT("00000000-0000-0000-0000-000000000234"), OwnerFunctionGraphName, PathParams({ TEXT("GetOwningActor"), TEXT("bHidden") }), TEXT("GetOwningActor.bHidden") },
	};
	for (const FCase& Case : Cases)
	{
		UEdGraph* Function = Fixture.FunctionGraph(Case.GraphName);
		TSharedPtr<FJsonObject> Request = MakeRequest(Case.PatchId, Function, TEXT("function"), Fixture.FunctionEntry(Case.GraphName), Case.Params);
		FCortexGraphPreparedPatch Preview;
		FCortexCommandResult Error;
		if (!TestTrue(FString::Printf(TEXT("%s preview succeeds: %s"), *Case.ExpectedPath, *Error.ErrorMessage),
			FCortexGraphPatchOps::Preflight(Fixture.AnimBlueprint, Request, Preview, Error)))
		{
			continue;
		}
		Request->SetBoolField(TEXT("dry_run"), false);
		Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);

		FCortexGraphPatchOutcome Outcome;
		const bool bApplied = FCortexGraphPatchOps::Execute(Fixture.AnimBlueprint, Request, Outcome, Error);
		TestTrue(FString::Printf(TEXT("%s apply succeeds: %s"), *Case.ExpectedPath, *Error.ErrorMessage), bApplied);
		TestEqual(FString::Printf(TEXT("%s apply status"), *Case.ExpectedPath), Outcome.ApplyStatus, FString(TEXT("applied")));
		TestEqual(FString::Printf(TEXT("%s compile status"), *Case.ExpectedPath), Outcome.CompileStatus, FString(TEXT("compiled")));
		TestEqual(FString::Printf(TEXT("%s readback status"), *Case.ExpectedPath), Outcome.ReadbackStatus, FString(TEXT("matched")));
		// A property access library error is downgraded to a compile warning by the engine: no warnings proves the copy compiled.
		TestEqual(FString::Printf(TEXT("%s compiles without warnings"), *Case.ExpectedPath),
			static_cast<int32>(Fixture.AnimBlueprint->Status), static_cast<int32>(BS_UpToDate));

		UEdGraphNode* Access = nullptr;
		for (UEdGraphNode* Node : Function->Nodes)
		{
			Access = Node != nullptr && Node->GetClass()->GetPathName() == NodeClassPath ? Node : Access;
		}
		if (TestNotNull(FString::Printf(TEXT("%s node created"), *Case.ExpectedPath), Access))
		{
			TestEqual(FString::Printf(TEXT("%s native path"), *Case.ExpectedPath), NativePath(Access), Case.ExpectedPath);
			const UEdGraphPin* Value = Access->FindPin(TEXT("Value"));
			TestTrue(FString::Printf(TEXT("%s Value pin wired to the setter"), *Case.ExpectedPath), Value != nullptr && Value->LinkedTo.Num() == 1
				&& Value->LinkedTo[0]->GetOwningNode()->IsA<UK2Node_VariableSet>());
			// The compiler records the cached copy it generated (none for a thread-safe path) on the node.
			const FNameProperty* Generated = CastField<FNameProperty>(Access->GetClass()->FindPropertyByName(TEXT("GeneratedPropertyName")));
			const FName CacheName = Generated != nullptr ? *Generated->ContainerPtrToValuePtr<FName>(Access) : NAME_None;
			const bool bExpectCache = Case.ExpectedPath != TEXT("bIsDead");
			TestEqual(FString::Printf(TEXT("%s cached copy generated"), *Case.ExpectedPath), CacheName != NAME_None, bExpectCache);
			if (bExpectCache)
			{
				TestNotNull(FString::Printf(TEXT("%s cached copy is a class property"), *Case.ExpectedPath),
					Fixture.AnimBlueprint->GeneratedClass->FindPropertyByName(CacheName));
			}
		}
	}
	return true;
}

#endif
