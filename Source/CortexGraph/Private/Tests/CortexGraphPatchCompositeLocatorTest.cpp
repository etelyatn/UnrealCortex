#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "CortexGraphTestContentRoot.h"
#include "Operations/CortexGraphPatchOps.h"
#include "Operations/CortexGraphPatchState.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_Composite.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

/**
 * One graph-reference convention across discovery and mutation.
 *
 * `graph.get_authoring_context` publishes every composite child as a locator whose `graph_guid` is
 * the BOUND CHILD graph and whose `subgraph_path` is that child's root-relative path. A caller that
 * echoes that locator back must reach the child it names. The candidate instead resolved
 * `graph_guid` against top-level graphs only, so the published child locator was refused with
 * GRAPH_NOT_FOUND, and an omitted `subgraph_path` could not resolve a child by identity at all.
 *
 * The corrected rule is asserted from both sides: the locator taken verbatim from discovery addresses
 * the child in preview and apply, an omitted path resolves by identity alone, the flat top-level case
 * still works, and a GUID/path pair that disagrees is refused before any mutation.
 */
namespace CortexGraphPatchCompositeLocatorTest
{
UBlueprint* CompositeLocatorBlueprint(UPackage*& OutPackage, const TCHAR* Name)
{
	EnsureCortexGraphTestTempContentRoot();
	OutPackage = CreatePackage(*FString::Printf(TEXT("/Temp/%s"), Name));
	return FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), OutPackage, FName(Name), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
}

void CompositeLocatorCleanup(UPackage* Package, UBlueprint* Blueprint)
{
	if (GEditor && GEditor->Trans)
	{
		GEditor->Trans->Reset(FText::FromString(TEXT("CortexGraphPatchCompositeLocatorTestCleanup")));
	}
	if (Blueprint)
	{
		Blueprint->ClearFlags(RF_Standalone);
		Blueprint->MarkAsGarbage();
	}
	if (Package)
	{
		Package->ClearFlags(RF_Standalone);
		Package->MarkAsGarbage();
	}
}

/** A composite node whose bound graph becomes a child of the graph it is placed in. */
UK2Node_Composite* CompositeLocatorMakeChild(UEdGraph* ParentGraph)
{
	UK2Node_Composite* Composite = NewObject<UK2Node_Composite>(ParentGraph);
	Composite->CreateNewGuid();
	ParentGraph->AddNode(Composite, true, false);
	Composite->PostPlacedNewNode();
	return Composite;
}

FCortexCommandRouter CompositeLocatorRouter()
{
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"),
		MakeShared<FCortexGraphCommandHandler>());
	return Router;
}

/** The published discovery choice for one graph GUID, or null when it was not published. */
TSharedPtr<FJsonObject> CompositeLocatorFindChoice(const TSharedPtr<FJsonObject>& ContextData, const FGuid& Guid)
{
	if (!ContextData.IsValid() || !ContextData->HasField(TEXT("graph_choices"))) return nullptr;
	for (const TSharedPtr<FJsonValue>& Value : ContextData->GetArrayField(TEXT("graph_choices")))
	{
		const TSharedPtr<FJsonObject> Choice = Value->AsObject();
		if (Choice.IsValid() && Choice->GetStringField(TEXT("graph_guid")) == Guid.ToString())
		{
			return Choice;
		}
	}
	return nullptr;
}

/** The graph_ref a caller echoes back: exactly the published identity, kind and path. */
TSharedPtr<FJsonObject> CompositeLocatorGraphRef(const TSharedPtr<FJsonObject>& Choice)
{
	TSharedPtr<FJsonObject> Ref = MakeShared<FJsonObject>();
	Ref->SetStringField(TEXT("graph_guid"), Choice->GetStringField(TEXT("graph_guid")));
	if (Choice->HasField(TEXT("graph_kind")))
	{
		Ref->SetStringField(TEXT("graph_kind"), Choice->GetStringField(TEXT("graph_kind")));
	}
	if (Choice->HasField(TEXT("subgraph_path")))
	{
		Ref->SetStringField(TEXT("subgraph_path"), Choice->GetStringField(TEXT("subgraph_path")));
	}
	return Ref;
}

TSharedPtr<FJsonObject> CompositeLocatorContextRequest(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& GraphRef)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	if (GraphRef.IsValid())
	{
		TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetObjectField(TEXT("graph_ref"), GraphRef);
		Params->SetObjectField(TEXT("target"), Target);
	}
	return Params;
}

/** A patch request addressing one graph_ref, adding a single resolvable node. */
TSharedPtr<FJsonObject> CompositeLocatorPatchRequest(
	UBlueprint* Blueprint,
	const TCHAR* PatchId,
	const TSharedPtr<FJsonObject>& GraphRef,
	const bool bDryRun)
{
	EnsureCortexGraphTestTempContentRoot();
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	Request->SetStringField(TEXT("patch_id"), PatchId);
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	Target->SetObjectField(TEXT("graph_ref"), GraphRef);
	Request->SetObjectField(TEXT("target"), Target);
	Request->SetObjectField(TEXT("expected_fingerprint"), FCortexGraphPatchState::ComputeFingerprint(Blueprint));
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("client_id"), TEXT("self0"));
	Node->SetStringField(TEXT("node_class"), TEXT("Self"));
	TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
	Position->SetNumberField(TEXT("x"), 240);
	Position->SetNumberField(TEXT("y"), 120);
	Node->SetObjectField(TEXT("position"), Position);
	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(MakeShared<FJsonValueObject>(Node));
	Request->SetArrayField(TEXT("nodes"), Nodes);
	Request->SetArrayField(TEXT("connections"), TArray<TSharedPtr<FJsonValue>>());
	Request->SetArrayField(TEXT("pin_updates"), TArray<TSharedPtr<FJsonValue>>());
	Request->SetBoolField(TEXT("dry_run"), bDryRun);
	Request->SetBoolField(TEXT("compile"), false);
	Request->SetBoolField(TEXT("save"), false);
	Request->SetBoolField(TEXT("allow_noop"), false);
	return Request;
}

UEdGraphNode* CompositeLocatorFindNode(UBlueprint* Blueprint, const FGuid& Guid)
{
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == Guid) return Node;
		}
	}
	return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPatchCompositeLocatorDiscoveryPairTest,
	"Cortex.Graph.Authoring.Patch.CompositeLocatorDiscoveryPair",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphPatchCompositeLocatorDiscoveryPairTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphPatchCompositeLocatorTest;

	UPackage* Package = nullptr;
	UBlueprint* Blueprint = CompositeLocatorBlueprint(Package, TEXT("BP_PatchCompositeLocator_T20"));
	TestNotNull(TEXT("composite locator fixture Blueprint created"), Blueprint);
	if (!Blueprint)
	{
		CompositeLocatorCleanup(Package, Blueprint);
		return false;
	}

	UEdGraph* RootGraph = Blueprint->UbergraphPages[0];
	UK2Node_Composite* Outer = CompositeLocatorMakeChild(RootGraph);
	UEdGraph* ChildA = Outer ? Outer->BoundGraph : nullptr;
	TestNotNull(TEXT("first-level composite bound graph created"), ChildA);
	if (!ChildA)
	{
		CompositeLocatorCleanup(Package, Blueprint);
		return false;
	}
	UK2Node_Composite* Inner = CompositeLocatorMakeChild(ChildA);
	UEdGraph* ChildB = Inner ? Inner->BoundGraph : nullptr;
	TestNotNull(TEXT("second-level composite bound graph created"), ChildB);
	if (!ChildB)
	{
		CompositeLocatorCleanup(Package, Blueprint);
		return false;
	}

	FCortexCommandRouter Router = CompositeLocatorRouter();
	const FCortexCommandResult Context = Router.Execute(TEXT("graph.get_authoring_context"),
		CompositeLocatorContextRequest(Blueprint, nullptr));
	TestTrue(FString::Printf(TEXT("the authoring context is published: %s"), *Context.ErrorMessage), Context.bSuccess);
	if (!Context.bSuccess || !Context.Data.IsValid())
	{
		CompositeLocatorCleanup(Package, Blueprint);
		return false;
	}
	const TSharedPtr<FJsonObject> ChildChoice = CompositeLocatorFindChoice(Context.Data, ChildB->GraphGuid);
	TestTrue(TEXT("the nested child graph is published as a graph choice"), ChildChoice.IsValid());
	if (!ChildChoice.IsValid())
	{
		CompositeLocatorCleanup(Package, Blueprint);
		return false;
	}
	// The published locator identifies the BOUND CHILD, with the child's root-relative path.
	TestTrue(TEXT("the published child locator carries a root-relative subgraph_path"),
		ChildChoice->HasField(TEXT("subgraph_path")));
	TestEqual(TEXT("the published child path is the two-level root-relative path"),
		ChildChoice->GetStringField(TEXT("subgraph_path")),
		FString::Printf(TEXT("%s.%s"), *ChildA->GetName(), *ChildB->GetName()));
	const TSharedPtr<FJsonObject> VerbatimRef = CompositeLocatorGraphRef(ChildChoice);

	// --- the published identity resolves the child ---
	const FCortexCommandResult IdentityOnly = Router.Execute(TEXT("graph.get_authoring_context"),
		CompositeLocatorContextRequest(Blueprint, VerbatimRef));
	TestTrue(FString::Printf(TEXT("the verbatim child locator resolves: %s"), *IdentityOnly.ErrorMessage),
		IdentityOnly.bSuccess);
	if (IdentityOnly.bSuccess && IdentityOnly.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* Resolved = nullptr;
		TestTrue(TEXT("the resolved target is published"),
			IdentityOnly.Data->TryGetObjectField(TEXT("target"), Resolved) && Resolved && Resolved->IsValid());
		if (Resolved && Resolved->IsValid())
		{
			TestEqual(TEXT("the resolved target is the bound child, not the root graph"),
				(*Resolved)->GetStringField(TEXT("graph_guid")), ChildB->GraphGuid.ToString());
			TestEqual(TEXT("the resolved target publishes the child's own path"),
				(*Resolved)->GetStringField(TEXT("subgraph_path")),
				FString::Printf(TEXT("%s.%s"), *ChildA->GetName(), *ChildB->GetName()));
		}
	}

	// --- a child identity with no path resolves by identity alone ---
	TSharedPtr<FJsonObject> IdentityOnlyRef = MakeShared<FJsonObject>();
	IdentityOnlyRef->SetStringField(TEXT("graph_guid"), ChildB->GraphGuid.ToString());
	const FCortexCommandResult ByGuidOnly = Router.Execute(TEXT("graph.get_authoring_context"),
		CompositeLocatorContextRequest(Blueprint, IdentityOnlyRef));
	TestTrue(FString::Printf(TEXT("a child GUID with no subgraph_path resolves by identity alone: %s"), *ByGuidOnly.ErrorMessage),
		ByGuidOnly.bSuccess);

	// --- a GUID/path pair that disagrees is refused before any mutation ---
	const int32 RootNodesBefore = RootGraph->Nodes.Num();
	const FString FingerprintBefore = FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash"));
	{
		TSharedPtr<FJsonObject> Mismatched = MakeShared<FJsonObject>();
		Mismatched->SetStringField(TEXT("graph_guid"), ChildA->GraphGuid.ToString());
		Mismatched->SetStringField(TEXT("subgraph_path"), FString::Printf(TEXT("%s.%s"), *ChildA->GetName(), *ChildB->GetName()));
		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult Error;
		TestFalse(TEXT("a GUID that names one graph with another graph's path is refused"),
			FCortexGraphPatchOps::Preflight(Blueprint, CompositeLocatorPatchRequest(Blueprint,
				TEXT("00000000-0000-0000-0000-000000002001"), Mismatched, true), Prepared, Error));
		TestEqual(TEXT("the identity/path mismatch uses the shared invalid-field code"),
			Error.ErrorCode, FString(CortexErrorCodes::InvalidField));
		TestTrue(TEXT("the mismatch refusal names the actual path of the named graph"),
			Error.ErrorMessage.Contains(ChildB->GetName()));
		TestEqual(TEXT("the mismatch refusal mutates no root node"), RootGraph->Nodes.Num(), RootNodesBefore);
		TestEqual(TEXT("the mismatch refusal leaves the authoring hash unchanged"),
			FCortexGraphPatchState::ComputeFingerprint(Blueprint)->GetStringField(TEXT("graph_authoring_hash")), FingerprintBefore);
	}

	// --- the flat top-level locator still addresses the root graph ---
	{
		TSharedPtr<FJsonObject> RootRef = MakeShared<FJsonObject>();
		RootRef->SetStringField(TEXT("graph_guid"), RootGraph->GraphGuid.ToString());
		RootRef->SetStringField(TEXT("graph_kind"), TEXT("ubergraph"));
		TSharedPtr<FJsonObject> Request = CompositeLocatorPatchRequest(
			Blueprint, TEXT("00000000-0000-0000-0000-000000002002"), RootRef, true);
		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult Error;
		TestTrue(FString::Printf(TEXT("the flat top-level locator previews: %s"), *Error.ErrorMessage),
			FCortexGraphPatchOps::Preflight(Blueprint, Request, Prepared, Error));
		Request->SetBoolField(TEXT("dry_run"), false);
		Request->SetStringField(TEXT("expected_validation_hash"), Prepared.ValidationHash);
		FCortexGraphPatchOutcome Outcome;
		const bool bApplied = FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error);
		TestTrue(FString::Printf(TEXT("the flat top-level locator applies: %s"), *Error.ErrorMessage), bApplied);
		TestEqual(TEXT("the flat apply reports the root graph"),
			Outcome.Locators.GraphGuid.ToString(), RootGraph->GraphGuid.ToString());
	}

	// --- the child locator taken verbatim from discovery addresses the child graph ---
	{
		const int32 ChildBNodesBefore = ChildB->Nodes.Num();
		TSharedPtr<FJsonObject> Request = CompositeLocatorPatchRequest(
			Blueprint, TEXT("00000000-0000-0000-0000-000000002003"), VerbatimRef, true);
		FCortexGraphPreparedPatch Preview;
		FCortexCommandResult Error;
		TestTrue(FString::Printf(TEXT("the verbatim child locator previews: %s"), *Error.ErrorMessage),
			FCortexGraphPatchOps::Preflight(Blueprint, Request, Preview, Error));
		TestEqual(TEXT("the preview plans against the bound child"), Preview.GraphGuid, ChildB->GraphGuid.ToString());
		TestEqual(TEXT("the preview keeps the child's own path"), Preview.SubgraphPath,
			FString::Printf(TEXT("%s.%s"), *ChildA->GetName(), *ChildB->GetName()));
		Request->SetBoolField(TEXT("dry_run"), false);
		Request->SetStringField(TEXT("expected_validation_hash"), Preview.ValidationHash);

		const int32 RootBeforeChildApply = RootGraph->Nodes.Num();
		FCortexGraphPatchOutcome Outcome;
		const bool bApplied = FCortexGraphPatchOps::Execute(Blueprint, Request, Outcome, Error);
		TestTrue(FString::Printf(TEXT("the verbatim child locator applies: %s"), *Error.ErrorMessage), bApplied);
		TestEqual(TEXT("the child locator reports the bound child"),
			Outcome.Locators.GraphGuid.ToString(), ChildB->GraphGuid.ToString());
		TestEqual(TEXT("the child apply adds exactly one node to the bound child"),
			ChildB->Nodes.Num(), ChildBNodesBefore + 1);
		TestEqual(TEXT("the child apply leaves the root graph untouched"),
			RootGraph->Nodes.Num(), RootBeforeChildApply);
		const FGuid* AddedGuid = Outcome.Locators.NodeGuidByClientId.Find(TEXT("self0"));
		TestTrue(TEXT("the child apply publishes the planned identity"), AddedGuid != nullptr);
		if (AddedGuid)
		{
			UEdGraphNode* Added = CompositeLocatorFindNode(Blueprint, *AddedGuid);
			TestNotNull(TEXT("the planned node landed in the graph tree"), Added);
			TestTrue(TEXT("the planned node is a bound-child node, not a root node"),
				Added != nullptr && Added->GetGraph() == ChildB);
		}
	}

	CompositeLocatorCleanup(Package, Blueprint);
	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
