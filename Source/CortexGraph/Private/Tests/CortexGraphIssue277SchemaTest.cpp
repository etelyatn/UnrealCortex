#include "Misc/AutomationTest.h"

#include "CortexGraphIssue277TestTypes.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

namespace CortexGraphIssue277SchemaTest
{
	const FCortexParamInfo* FindParam(const FCortexCommandInfo& Command, const TCHAR* Name)
	{
		return Command.Params.FindByPredicate([Name](const FCortexParamInfo& Param) { return Param.Name == Name; });
	}
}

/**
 * Issue 277 published declarations. The router rejects every undeclared param, so a new param or command that is not
 * in GetSupportedCommands is unusable; the descriptions are what an agent reads to discover the behaviour.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphIssue277SchemaDeclarationsTest,
	"Cortex.Graph.Issue277.Schema.Declarations",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphIssue277SchemaDeclarationsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphIssue277SchemaTest;

	const FCortexGraphCommandHandler Handler;
	const TArray<FCortexCommandInfo> Commands = Handler.GetSupportedCommands();
	auto FindCommand = [&Commands](const TCHAR* Name)
	{
		return Commands.FindByPredicate([Name](const FCortexCommandInfo& Info) { return Info.Name == Name; });
	};

	// Item 3: get_authoring_context accepts graph_name.
	if (const FCortexCommandInfo* Context = FindCommand(TEXT("get_authoring_context")))
	{
		const FCortexParamInfo* GraphName = FindParam(*Context, TEXT("graph_name"));
		if (TestNotNull(TEXT("get_authoring_context declares graph_name"), GraphName))
		{
			TestFalse(TEXT("graph_name is optional"), GraphName->bRequired);
			TestTrue(TEXT("graph_name documents the ambiguity candidates"), GraphName->Description.Contains(TEXT("candidates")));
		}
	}
	else
	{
		AddError(TEXT("get_authoring_context is registered"));
	}

	// Item 9: reads document the positions, add_node declares placement.
	if (const FCortexCommandInfo* GetSubgraph = FindCommand(TEXT("get_subgraph")))
	{
		const FCortexParamInfo* Compact = FindParam(*GetSubgraph, TEXT("compact"));
		const FString Published = GetSubgraph->Description + (Compact ? Compact->Description : FString());
		TestTrue(TEXT("get_subgraph says compact:false returns node positions"),
			Published.Contains(TEXT("position")) && Published.Contains(TEXT("compact")));
	}
	else
	{
		AddError(TEXT("get_subgraph is registered"));
	}
	if (const FCortexCommandInfo* AddNode = FindCommand(TEXT("add_node")))
	{
		const FCortexParamInfo* Placement = FindParam(*AddNode, TEXT("placement"));
		if (TestNotNull(TEXT("add_node declares placement"), Placement))
		{
			TestEqual(TEXT("placement is an object"), Placement->Type, FString(TEXT("object")));
			TestFalse(TEXT("placement is optional"), Placement->bRequired);
			TestTrue(TEXT("placement documents relative_to"), Placement->Description.Contains(TEXT("relative_to")));
			TestTrue(TEXT("placement documents side"), Placement->Description.Contains(TEXT("side")));
			TestTrue(TEXT("placement documents gap"), Placement->Description.Contains(TEXT("gap")));
			TestTrue(TEXT("placement documents its exclusivity with position"), Placement->Description.Contains(TEXT("position")));
		}
	}
	else
	{
		AddError(TEXT("add_node is registered"));
	}

	// Item 10: graph.split_getter is a declared write command.
	if (const FCortexCommandInfo* Split = FindCommand(TEXT("split_getter")))
	{
		for (const TCHAR* Required : { TEXT("asset_path"), TEXT("node_id") })
		{
			const FCortexParamInfo* Param = FindParam(*Split, Required);
			if (TestNotNull(FString::Printf(TEXT("split_getter declares %s"), Required), Param))
			{
				TestTrue(FString::Printf(TEXT("split_getter requires %s"), Required), Param->bRequired);
			}
		}
		for (const TCHAR* Optional : { TEXT("graph_name"), TEXT("dry_run") })
		{
			const FCortexParamInfo* Param = FindParam(*Split, Optional);
			if (TestNotNull(FString::Printf(TEXT("split_getter declares %s"), Optional), Param))
			{
				TestFalse(FString::Printf(TEXT("split_getter %s is optional"), Optional), Param->bRequired);
			}
		}
		TestTrue(TEXT("split_getter describes the consumer fan-out"), Split->Description.Contains(TEXT("consumer")));
	}
	else
	{
		AddError(TEXT("split_getter is registered"));
	}

	// Item 11: remove_nodes documents the override pre-flight.
	if (const FCortexCommandInfo* RemoveNodes = FindCommand(TEXT("remove_nodes")))
	{
		TestTrue(TEXT("remove_nodes documents override_events"), RemoveNodes->Description.Contains(TEXT("override_events")));
		TestTrue(TEXT("remove_nodes documents that native callers are not enumerable"), RemoveNodes->Description.Contains(TEXT("not_enumerable")));
	}
	else
	{
		AddError(TEXT("remove_nodes is registered"));
	}

	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
