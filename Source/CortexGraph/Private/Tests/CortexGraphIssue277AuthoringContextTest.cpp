#include "Misc/AutomationTest.h"

#include "CortexGraphIssue277TestTypes.h"
#include "CortexGraphTreeTestTypes.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

/**
 * Issue 277 item 3: graph.get_authoring_context accepts graph_name (top level, or target.graph_name without graph_ref),
 * resolved like the other graph reads. Every case goes through the command router, so the published declaration
 * is enforced too.
 */
namespace CortexGraphIssue277AuthoringContextTest
{
	FCortexCommandResult ReadContext(FCortexCommandRouter& Router, const FString& AssetPath, const TFunctionRef<void(const TSharedPtr<FJsonObject>&)>& Fill)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Fill(Params);
		return Router.Execute(TEXT("graph.get_authoring_context"), Params);
	}

	TSharedPtr<FJsonObject> GraphRefTarget(const UEdGraph* Graph)
	{
		TSharedPtr<FJsonObject> Ref = MakeShared<FJsonObject>();
		Ref->SetStringField(TEXT("graph_guid"), Graph->GraphGuid.ToString());
		TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetObjectField(TEXT("graph_ref"), Ref);
		return Target;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277AuthoringContextGraphNameTest,
	"Cortex.Graph.Issue277.AuthoringContext.GraphName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277AuthoringContextGraphNameTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphTreeTest;
	using namespace CortexGraphIssue277AuthoringContextTest;

	FNestedFixture Fixture;
	if (!TestTrue(TEXT("nested fixture builds"), BuildTreeNestedFixture(TEXT("BP_Issue277ContextName"), Fixture)))
	{
		return false;
	}
	FCortexCommandRouter Router;
	RegisterTreeGraphDomain(Router);
	const FString AssetPath = Fixture.Blueprint->GetPathName();

	// A unique root name resolves through the top-level graph_name.
	{
		const FCortexCommandResult Result = ReadContext(Router, AssetPath,
			[](const TSharedPtr<FJsonObject>& Params) { Params->SetStringField(TEXT("graph_name"), TEXT("HostFn")); });
		if (TestTrue(FString::Printf(TEXT("top-level graph_name resolves: %s"), *Result.ErrorMessage), Result.bSuccess))
		{
			const TSharedPtr<FJsonObject> Target = Result.Data->GetObjectField(TEXT("target"));
			TestEqual(TEXT("top-level graph_name targets HostFn"), Target->GetStringField(TEXT("graph_guid")), TreeGuidString(Fixture.HostFn));
			TestEqual(TEXT("target graph_name"), Target->GetStringField(TEXT("graph_name")), FString(TEXT("HostFn")));
			TestEqual(TEXT("target graph_kind"), Target->GetStringField(TEXT("graph_kind")), FString(TEXT("function")));
			TestTrue(TEXT("target is mutable"), Target->GetBoolField(TEXT("is_mutable")));
			TestTrue(TEXT("the response still carries the fingerprint"), Result.Data->HasField(TEXT("fingerprint")));
			TestTrue(TEXT("the response still lists graph_choices"), Result.Data->HasField(TEXT("graph_choices")));
		}
	}

	// target.graph_name without graph_ref resolves the same way.
	{
		const FCortexCommandResult Result = ReadContext(Router, AssetPath,
			[](const TSharedPtr<FJsonObject>& Params)
			{
				TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
				Target->SetStringField(TEXT("graph_name"), TEXT("HostFn"));
				Params->SetObjectField(TEXT("target"), Target);
			});
		if (TestTrue(FString::Printf(TEXT("target.graph_name resolves: %s"), *Result.ErrorMessage), Result.bSuccess))
		{
			const TSharedPtr<FJsonObject> Target = Result.Data->GetObjectField(TEXT("target"));
			TestEqual(TEXT("target.graph_name targets HostFn"), Target->GetStringField(TEXT("graph_guid")), TreeGuidString(Fixture.HostFn));
			TestEqual(TEXT("target.graph_name graph_kind"), Target->GetStringField(TEXT("graph_kind")), FString(TEXT("function")));
		}
	}

	// graph_ref keeps precedence over a graph_name that names nothing, at the top level and inside target.
	{
		const FCortexCommandResult TopLevel = ReadContext(Router, AssetPath,
			[&](const TSharedPtr<FJsonObject>& Params)
			{
				Params->SetStringField(TEXT("graph_name"), TEXT("NoSuchGraph"));
				Params->SetObjectField(TEXT("target"), GraphRefTarget(Fixture.Composite->BoundGraph));
			});
		if (TestTrue(FString::Printf(TEXT("graph_ref wins over a top-level graph_name: %s"), *TopLevel.ErrorMessage), TopLevel.bSuccess))
		{
			const TSharedPtr<FJsonObject> Target = TopLevel.Data->GetObjectField(TEXT("target"));
			TestEqual(TEXT("the graph_ref graph is the target"),
				Target->GetStringField(TEXT("graph_guid")), TreeGuidString(Fixture.Composite->BoundGraph));
			TestEqual(TEXT("the composite subgraph_path is reported"), Target->GetStringField(TEXT("subgraph_path")), FString(TEXT("Inner")));
		}

		const FCortexCommandResult InTarget = ReadContext(Router, AssetPath,
			[&](const TSharedPtr<FJsonObject>& Params)
			{
				TSharedPtr<FJsonObject> Target = GraphRefTarget(Fixture.HostFn);
				Target->SetStringField(TEXT("graph_name"), TEXT("NoSuchGraph"));
				Params->SetObjectField(TEXT("target"), Target);
			});
		if (TestTrue(FString::Printf(TEXT("graph_ref wins over target.graph_name: %s"), *InTarget.ErrorMessage), InTarget.bSuccess))
		{
			TestEqual(TEXT("the graph_ref graph is the target"),
				InTarget.Data->GetObjectField(TEXT("target"))->GetStringField(TEXT("graph_guid")), TreeGuidString(Fixture.HostFn));
		}
	}

	// An unknown name is GRAPH_NOT_FOUND, like every other graph read.
	{
		const FCortexCommandResult Unknown = ReadContext(Router, AssetPath,
			[](const TSharedPtr<FJsonObject>& Params) { Params->SetStringField(TEXT("graph_name"), TEXT("NoSuchGraph")); });
		TestFalse(TEXT("an unknown graph_name is refused"), Unknown.bSuccess);
		TestEqual(TEXT("an unknown graph_name is GRAPH_NOT_FOUND"), Unknown.ErrorCode, FString(CortexErrorCodes::GraphNotFound));
	}

	// A name carried only by nested graphs returns the resolver's nested_candidates.
	{
		const FCortexCommandResult Nested = ReadContext(Router, AssetPath,
			[](const TSharedPtr<FJsonObject>& Params) { Params->SetStringField(TEXT("graph_name"), TEXT("Transition")); });
		TestFalse(TEXT("a nested-only name is refused"), Nested.bSuccess);
		TestEqual(TEXT("a nested-only name is GRAPH_NOT_FOUND"), Nested.ErrorCode, FString(CortexErrorCodes::GraphNotFound));
		const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
		if (TestTrue(TEXT("the refusal lists nested_candidates"),
			Nested.ErrorDetails.IsValid() && Nested.ErrorDetails->TryGetArrayField(TEXT("nested_candidates"), Candidates) && Candidates != nullptr))
		{
			TestEqual(TEXT("both nested graphs are listed"), Candidates->Num(), 2);
			TestTrue(TEXT("the first nested graph is addressable by path"),
				FindTreeJsonObject(*Candidates, TEXT("graph_path"), TEXT("HostFn/Transition")).IsValid());
			TestTrue(TEXT("the second nested graph is addressable by path"),
				FindTreeJsonObject(*Candidates, TEXT("graph_path"), TEXT("HostFn/Transition~2")).IsValid());
		}
	}

	Fixture.Blueprint->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277AuthoringContextAmbiguousNameTest,
	"Cortex.Graph.Issue277.AuthoringContext.AmbiguousGraphName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277AuthoringContextAmbiguousNameTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphTreeTest;
	using namespace CortexGraphIssue277AuthoringContextTest;

	UBlueprint* Blueprint = CreateTreeBlueprint(TEXT("BP_Issue277ContextAmbiguous"));
	if (!TestNotNull(TEXT("fixture Blueprint created"), Blueprint))
	{
		return false;
	}
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	UEdGraph* HostFn = AddTreeFunctionGraph(Blueprint, TEXT("HostFn"));
	if (!TestTrue(TEXT("the Blueprint has an EventGraph"), EventGraph != nullptr))
	{
		Blueprint->MarkAsGarbage();
		return false;
	}

	// A second root named EventGraph (a macro graph owned by another outer, so the name is free): two root graphs share the name.
	UEdGraph* DuplicateMacro = NewObject<UEdGraph>(HostFn, FName(TEXT("EventGraph")), RF_Transactional);
	DuplicateMacro->Schema = UEdGraphSchema_K2::StaticClass();
	DuplicateMacro->GraphGuid = FGuid::NewGuid();
	Blueprint->MacroGraphs.Add(DuplicateMacro);

	FCortexCommandRouter Router;
	RegisterTreeGraphDomain(Router);
	const FString AssetPath = Blueprint->GetPathName();

	auto CheckAmbiguity = [&](const TCHAR* Label, const FCortexCommandResult& Result)
	{
		TestFalse(FString::Printf(TEXT("%s: an ambiguous name is refused"), Label), Result.bSuccess);
		TestEqual(FString::Printf(TEXT("%s: the refusal is INVALID_FIELD"), Label), Result.ErrorCode, FString(CortexErrorCodes::InvalidField));
		const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
		if (!TestTrue(FString::Printf(TEXT("%s: the refusal lists candidates"), Label),
			Result.ErrorDetails.IsValid() && Result.ErrorDetails->TryGetArrayField(TEXT("candidates"), Candidates) && Candidates != nullptr))
		{
			return;
		}
		TestEqual(FString::Printf(TEXT("%s: both graphs are candidates"), Label), Candidates->Num(), 2);

		const TSharedPtr<FJsonObject> Ubergraph = FindTreeJsonObject(*Candidates, TEXT("graph_guid"), TreeGuidString(EventGraph));
		if (TestTrue(FString::Printf(TEXT("%s: the ubergraph is a candidate"), Label), Ubergraph.IsValid()))
		{
			TestEqual(TEXT("candidate graph_name"), Ubergraph->GetStringField(TEXT("graph_name")), FString(TEXT("EventGraph")));
			TestEqual(TEXT("candidate graph_kind"), Ubergraph->GetStringField(TEXT("graph_kind")), FString(TEXT("ubergraph")));
			TestTrue(TEXT("candidate carries subgraph_path"), Ubergraph->HasField(TEXT("subgraph_path")));
		}
		const TSharedPtr<FJsonObject> Macro = FindTreeJsonObject(*Candidates, TEXT("graph_guid"), TreeGuidString(DuplicateMacro));
		if (TestTrue(FString::Printf(TEXT("%s: the macro graph is a candidate"), Label), Macro.IsValid()))
		{
			TestEqual(TEXT("candidate graph_name"), Macro->GetStringField(TEXT("graph_name")), FString(TEXT("EventGraph")));
			TestEqual(TEXT("candidate graph_kind"), Macro->GetStringField(TEXT("graph_kind")), FString(TEXT("macro")));
			TestTrue(TEXT("candidate carries subgraph_path"), Macro->HasField(TEXT("subgraph_path")));
		}
	};

	CheckAmbiguity(TEXT("top-level graph_name"), ReadContext(Router, AssetPath,
		[](const TSharedPtr<FJsonObject>& Params) { Params->SetStringField(TEXT("graph_name"), TEXT("EventGraph")); }));
	CheckAmbiguity(TEXT("target.graph_name"), ReadContext(Router, AssetPath,
		[](const TSharedPtr<FJsonObject>& Params)
		{
			TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
			Target->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
			Params->SetObjectField(TEXT("target"), Target);
		}));

	// graph_ref still addresses either graph unambiguously by identity.
	{
		const FCortexCommandResult ByRef = ReadContext(Router, AssetPath,
			[&](const TSharedPtr<FJsonObject>& Params)
			{
				Params->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
				Params->SetObjectField(TEXT("target"), GraphRefTarget(EventGraph));
			});
		if (TestTrue(FString::Printf(TEXT("graph_ref disambiguates the name: %s"), *ByRef.ErrorMessage), ByRef.bSuccess))
		{
			TestEqual(TEXT("the graph_ref graph is the target"),
				ByRef.Data->GetObjectField(TEXT("target"))->GetStringField(TEXT("graph_guid")), TreeGuidString(EventGraph));
		}
	}

	Blueprint->MarkAsGarbage();
	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
