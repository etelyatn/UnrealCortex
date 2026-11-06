#include "Misc/AutomationTest.h"

#include "CortexGraphTreeTestTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphGetSubgraphByGuidTest,
	"Cortex.Graph.GetSubgraph.ByGuid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphGetSubgraphByGuidTest::RunTest(const FString& Parameters)
{
	using namespace CortexGraphTreeTest;

	FNestedFixture Fixture;
	if (!TestTrue(TEXT("nested fixture builds"), BuildTreeNestedFixture(TEXT("BP_GetSubgraphByGuid"), Fixture)))
	{
		return false;
	}

	const FString AssetPath = Fixture.Blueprint->GetPathName();
	FCortexCommandRouter Router;
	RegisterTreeGraphDomain(Router);

	auto GetSubgraph = [&](const TFunctionRef<void(const TSharedPtr<FJsonObject>&)>& Fill)
	{
		TSharedPtr<FJsonObject> Params = MakeTreeParams(AssetPath);
		Fill(Params);
		return Router.Execute(TEXT("graph.get_subgraph"), Params);
	};
	auto Candidates = [](const FCortexCommandResult& Result, const TCHAR* Field) -> TArray<TSharedPtr<FJsonValue>>
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Result.ErrorDetails.IsValid() && Result.ErrorDetails->TryGetArrayField(Field, Array) && Array)
		{
			return *Array;
		}
		return {};
	};

	// ── A nested graph by graph_guid ──
	{
		const FCortexCommandResult Result = GetSubgraph([&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), TreeGuidString(Fixture.T1));
		});
		if (TestTrue(TEXT("T1 by graph_guid succeeds"), Result.bSuccess) && Result.Data.IsValid())
		{
			TestEqual(TEXT("graph_name is the nested graph's name"), Result.Data->GetStringField(TEXT("graph_name")), FString(TEXT("Transition")));
			TestEqual(TEXT("graph_guid echoed"), Result.Data->GetStringField(TEXT("graph_guid")), TreeGuidString(Fixture.T1));
			TestEqual(TEXT("graph_kind reported"), Result.Data->GetStringField(TEXT("graph_kind")), FString(TEXT("nested")));
			TestEqual(TEXT("graph_path reported"), Result.Data->GetStringField(TEXT("graph_path")), FString(TEXT("HostFn/Transition")));
			const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
			if (TestTrue(TEXT("nodes returned"), Result.Data->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes))
			{
				TestEqual(TEXT("T1 holds exactly the inner host node"), Nodes->Num(), 1);
				if (Nodes->Num() == 1)
				{
					TestEqual(TEXT("node id"), (*Nodes)[0]->AsObject()->GetStringField(TEXT("node_id")), Fixture.InnerHost->GetName());
				}
			}
		}

		const FCortexCommandResult Second = GetSubgraph([&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), TreeGuidString(Fixture.T2));
		});
		if (TestTrue(TEXT("T2 by graph_guid succeeds"), Second.bSuccess) && Second.Data.IsValid())
		{
			TestEqual(TEXT("the second Transition is a different graph"), Second.Data->GetNumberField(TEXT("node_count")), 0.0);
		}

		const FCortexCommandResult Single = GetSubgraph([&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), TreeGuidString(Fixture.T1));
			P->SetStringField(TEXT("node_id"), Fixture.InnerHost->GetName());
		});
		if (TestTrue(TEXT("single node by graph_guid succeeds"), Single.bSuccess) && Single.Data.IsValid())
		{
			TestEqual(TEXT("single node carries graph_guid"), Single.Data->GetStringField(TEXT("graph_guid")), TreeGuidString(Fixture.T1));
		}

		const FCortexCommandResult Composite = GetSubgraph([&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), TreeGuidString(Fixture.Composite->BoundGraph));
		});
		if (TestTrue(TEXT("composite body by graph_guid succeeds"), Composite.bSuccess) && Composite.Data.IsValid())
		{
			TestEqual(TEXT("composite graph_kind"), Composite.Data->GetStringField(TEXT("graph_kind")), FString(TEXT("composite")));
			TestEqual(TEXT("composite subgraph_path"), Composite.Data->GetStringField(TEXT("subgraph_path")), FString(TEXT("Inner")));
		}
	}

	// ── graph_name keeps its root meaning; nested-only names list candidates ──
	{
		const FCortexCommandResult Result = GetSubgraph([&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_name"), TEXT("Transition"));
		});
		TestFalse(TEXT("a name matching only nested graphs is not resolved"), Result.bSuccess);
		TestEqual(TEXT("refusal code"), Result.ErrorCode, CortexErrorCodes::GraphNotFound);
		const TArray<TSharedPtr<FJsonValue>> Nested = Candidates(Result, TEXT("nested_candidates"));
		if (TestEqual(TEXT("both Transition graphs are offered"), Nested.Num(), 2))
		{
			const TSharedPtr<FJsonObject> Left = FindTreeJsonObject(Nested, TEXT("graph_guid"), TreeGuidString(Fixture.T1));
			const TSharedPtr<FJsonObject> Right = FindTreeJsonObject(Nested, TEXT("graph_guid"), TreeGuidString(Fixture.T2));
			if (TestTrue(TEXT("both candidates carry their graph_guid"), Left.IsValid() && Right.IsValid()))
			{
				TestEqual(TEXT("candidate graph_path"), Left->GetStringField(TEXT("graph_path")), FString(TEXT("HostFn/Transition")));
				TestEqual(TEXT("candidate graph_path 2"), Right->GetStringField(TEXT("graph_path")), FString(TEXT("HostFn/Transition~2")));
				TestEqual(TEXT("candidate kind"), Left->GetStringField(TEXT("kind")), FString(TEXT("nested")));
			}
		}

		const FCortexCommandResult Missing = GetSubgraph([&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_name"), TEXT("NoSuchGraphAnywhere"));
		});
		TestEqual(TEXT("an unknown name still refuses GRAPH_NOT_FOUND"), Missing.ErrorCode, CortexErrorCodes::GraphNotFound);
		TestEqual(TEXT("an unknown name lists no candidates"), Candidates(Missing, TEXT("nested_candidates")).Num(), 0);
	}

	// ── Disagreements and unknown identities refuse ──
	{
		const FCortexCommandResult NameMismatch = GetSubgraph([&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), TreeGuidString(Fixture.T1));
			P->SetStringField(TEXT("graph_name"), TEXT("Grandchild"));
		});
		TestFalse(TEXT("graph_guid with a disagreeing graph_name is refused"), NameMismatch.bSuccess);
		TestEqual(TEXT("name mismatch code"), NameMismatch.ErrorCode, CortexErrorCodes::InvalidField);

		const FCortexCommandResult PathMismatch = GetSubgraph([&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), TreeGuidString(Fixture.T1));
			P->SetStringField(TEXT("subgraph_path"), TEXT("Inner"));
		});
		TestEqual(TEXT("graph_guid with a disagreeing subgraph_path is refused"), PathMismatch.ErrorCode, CortexErrorCodes::InvalidField);

		const FCortexCommandResult Agreeing = GetSubgraph([&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), TreeGuidString(Fixture.T1));
			P->SetStringField(TEXT("graph_name"), TEXT("Transition"));
		});
		TestTrue(TEXT("an agreeing graph_name is accepted"), Agreeing.bSuccess);

		const FCortexCommandResult Unknown = GetSubgraph([&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), FGuid::NewGuid().ToString());
		});
		TestFalse(TEXT("an unknown graph_guid is refused"), Unknown.bSuccess);
		TestEqual(TEXT("unknown guid code"), Unknown.ErrorCode, CortexErrorCodes::GraphNotFound);

		const FCortexCommandResult Malformed = GetSubgraph([&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), TEXT("not-a-guid"));
		});
		TestEqual(TEXT("a malformed graph_guid is INVALID_FIELD"), Malformed.ErrorCode, CortexErrorCodes::InvalidField);

		const FCortexCommandResult UnknownPath = GetSubgraph([&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_path"), TEXT("HostFn/Nope"));
		});
		TestEqual(TEXT("an unknown graph_path is GRAPH_NOT_FOUND"), UnknownPath.ErrorCode, CortexErrorCodes::GraphNotFound);

		const FCortexCommandResult GuidPathMismatch = GetSubgraph([&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), TreeGuidString(Fixture.T1));
			P->SetStringField(TEXT("graph_path"), TEXT("HostFn/Inner"));
		});
		TestEqual(TEXT("graph_guid with a disagreeing graph_path is refused"), GuidPathMismatch.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// ── Root graph by graph_guid equals by graph_name ──
	{
		const FCortexCommandResult ByName = GetSubgraph([&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_name"), TEXT("HostFn"));
			P->SetBoolField(TEXT("include_edges"), true);
		});
		const FCortexCommandResult ByGuid = GetSubgraph([&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), TreeGuidString(Fixture.HostFn));
			P->SetBoolField(TEXT("include_edges"), true);
		});
		if (TestTrue(TEXT("root by name succeeds"), ByName.bSuccess) && TestTrue(TEXT("root by guid succeeds"), ByGuid.bSuccess))
		{
			const TArray<TSharedPtr<FJsonValue>>* NamedNodes = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* GuidNodes = nullptr;
			ByName.Data->TryGetArrayField(TEXT("nodes"), NamedNodes);
			ByGuid.Data->TryGetArrayField(TEXT("nodes"), GuidNodes);
			if (TestTrue(TEXT("both carry nodes"), NamedNodes && GuidNodes))
			{
				TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
				A->SetArrayField(TEXT("nodes"), *NamedNodes);
				TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();
				B->SetArrayField(TEXT("nodes"), *GuidNodes);
				TestEqual(TEXT("same nodes either way"), TreeToCondensedString(B), TreeToCondensedString(A));
			}
			TestEqual(TEXT("same graph_name either way"), ByGuid.Data->GetStringField(TEXT("graph_name")), ByName.Data->GetStringField(TEXT("graph_name")));
			TestFalse(TEXT("an existing caller's response carries no graph_guid"), ByName.Data->HasField(TEXT("graph_guid")));
			TestFalse(TEXT("an existing caller's response carries no graph_kind"), ByName.Data->HasField(TEXT("graph_kind")));
		}
	}
	Fixture.Blueprint->MarkAsGarbage();

	// ── A GUID shared by two graphs is never guessed; graph_path resolves each ──
	{
		FNestedFixture Shared;
		if (!TestTrue(TEXT("shared-guid fixture builds"), BuildTreeNestedFixture(TEXT("BP_GetSubgraphSharedGuid"), Shared)))
		{
			return false;
		}
		Shared.T2->GraphGuid = Shared.T1->GraphGuid;
		const FString SharedAsset = Shared.Blueprint->GetPathName();
		const FString SharedGuid = TreeGuidString(Shared.T1);

		auto SharedCall = [&](const TFunctionRef<void(const TSharedPtr<FJsonObject>&)>& Fill)
		{
			TSharedPtr<FJsonObject> Params = MakeTreeParams(SharedAsset);
			Fill(Params);
			return Router.Execute(TEXT("graph.get_subgraph"), Params);
		};

		const FCortexCommandResult Ambiguous = SharedCall([&](const TSharedPtr<FJsonObject>& P) { P->SetStringField(TEXT("graph_guid"), SharedGuid); });
		TestFalse(TEXT("a shared GUID is refused"), Ambiguous.bSuccess);
		TestEqual(TEXT("shared GUID code"), Ambiguous.ErrorCode, CortexErrorCodes::InvalidField);
		const TArray<TSharedPtr<FJsonValue>> Colliding = Candidates(Ambiguous, TEXT("colliding_graphs"));
		if (TestEqual(TEXT("both colliding graphs are listed"), Colliding.Num(), 2))
		{
			TestTrue(TEXT("first path listed"), FindTreeJsonObject(Colliding, TEXT("graph_path"), TEXT("HostFn/Transition")).IsValid());
			TestTrue(TEXT("second path listed"), FindTreeJsonObject(Colliding, TEXT("graph_path"), TEXT("HostFn/Transition~2")).IsValid());
		}
		TestTrue(TEXT("the message names both paths"),
			Ambiguous.ErrorMessage.Contains(TEXT("HostFn/Transition")) && Ambiguous.ErrorMessage.Contains(TEXT("HostFn/Transition~2")));

		const FCortexCommandResult First = SharedCall([&](const TSharedPtr<FJsonObject>& P) { P->SetStringField(TEXT("graph_path"), TEXT("HostFn/Transition")); });
		const FCortexCommandResult Other = SharedCall([&](const TSharedPtr<FJsonObject>& P) { P->SetStringField(TEXT("graph_path"), TEXT("HostFn/Transition~2")); });
		if (TestTrue(TEXT("graph_path resolves the first"), First.bSuccess) && TestTrue(TEXT("graph_path resolves the second"), Other.bSuccess))
		{
			TestEqual(TEXT("first path is T1 (one node)"), First.Data->GetNumberField(TEXT("node_count")), 1.0);
			TestEqual(TEXT("second path is T2 (no nodes)"), Other.Data->GetNumberField(TEXT("node_count")), 0.0);
		}

		const FCortexCommandResult Agreeing = SharedCall([&](const TSharedPtr<FJsonObject>& P)
		{
			P->SetStringField(TEXT("graph_guid"), SharedGuid);
			P->SetStringField(TEXT("graph_path"), TEXT("HostFn/Transition~2"));
		});
		TestTrue(TEXT("graph_guid plus the graph_path of one colliding graph resolves it"), Agreeing.bSuccess);
		Shared.Blueprint->MarkAsGarbage();
	}

	return true;
}
