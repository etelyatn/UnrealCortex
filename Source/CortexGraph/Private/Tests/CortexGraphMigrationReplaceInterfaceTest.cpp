// replace_interface preview (#122 Task 7): the per-function plan with a stable validation hash, a change-free preview and every
// refusal, each leaving the asset exactly as it was.

#include "Misc/AutomationTest.h"

#include "CortexGraphReplaceInterfaceTestSupport.h"
#include "Operations/CortexGraphInterfaceMigration.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphMigrationReplaceInterfacePreviewTest,
	"Cortex.Graph.Authoring.Migration.ReplaceInterface.Preview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphMigrationReplaceInterfacePreviewTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexGraphReplaceInterfaceTest;

	FScope Scope;
	UBlueprint* const Interface = MakeInterface(Scope, TEXT("BPI_Issue122Preview"));
	FImplementer Impl;
	if (!TestNotNull(TEXT("BPI"), Interface) || !TestTrue(TEXT("Implementer"), MakeImplementer(Scope, Interface, TEXT("BP_Issue122Preview"), Impl)))
	{
		return false;
	}
	const FString OldPath = Interface->GetPathName();
	const FString StructureBefore = StructureDigest(Impl.Blueprint);
	const bool bDirtyBefore = Impl.Blueprint->GetOutermost()->IsDirty();
	const FGuid GraphGuidBefore = Impl.FunctionGraph->GraphGuid;

	// The preview publishes the per-function plan and a stable validation hash, and changes nothing.
	FString FirstHash;
	{
		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult Error;
		const TSharedPtr<FJsonObject> Migration = MigrationJson(OldPath, NativePath(), FullFunctionMap());
		TestTrue(FString::Printf(TEXT("The preview succeeds: %s"), *Error.ErrorMessage),
			FCortexGraphPatchOps::Preflight(Impl.Blueprint, Request(Impl.Blueprint, TEXT("00000000-0000-0000-0000-000000122101"), Migration), Prepared, Error));
		TestTrue(TEXT("The preview is a dry run"), Prepared.bDryRun && Prepared.bIsInterfaceReplacement());
		TestFalse(TEXT("A validation hash is published"), Prepared.ValidationHash.IsEmpty());
		TestTrue(TEXT("The preview announces a change"), Prepared.bChanged && !Prepared.bFullyReused);
		FirstHash = Prepared.ValidationHash;

		FCortexGraphMigrationInterfacePlan Plan;
		FCortexCommandResult PlanError;
		if (TestTrue(TEXT("The durable plan round-trips"), FCortexGraphMigrationInterfacePlan::FromJson(Prepared.InterfacePlan, Plan, PlanError)))
		{
			TestEqual(TEXT("Both functions are planned"), Plan.Functions.Num(), 2);
			TestEqual(TEXT("The new interface is the native class"), Plan.NewInterface, UCortexIssue122GraphInterface::StaticClass()->GetPathName());
			TestEqual(TEXT("Nothing is dropped"), Plan.DroppedStubs.Num(), 0);
			TestEqual(TEXT("No new stub is needed"), Plan.NewStubFunctions.Num(), 0);
			for (const FCortexGraphInterfaceFunctionPlan& Function : Plan.Functions)
			{
				if (Function.From == TEXT("GetValue"))
				{
					TestEqual(TEXT("GetValue is a function graph"), Function.Implementation, FString(TEXT("function_graph")));
					TestEqual(TEXT("GetValue source graph"), Function.SourceGraphGuid, Impl.FunctionGraphGuid.ToString());
					TestEqual(TEXT("GetValue stale entry"), Function.SourceEntryGuid, Impl.FunctionEntryGuid.ToString());
					TestEqual(TEXT("GetValue stale result"), Function.SourceResultGuid, Impl.FunctionResultGuid.ToString());
					TestTrue(TEXT("GetValue returns a value"), Function.bToHasResult && !Function.bToIsEvent);
				}
				else
				{
					TestEqual(TEXT("Notify is an event"), Function.Implementation, FString(TEXT("event")));
					TestEqual(TEXT("Notify stale entry"), Function.SourceEntryGuid, Impl.EventGuid.ToString());
					TestTrue(TEXT("Notify is event-style"), Function.bToIsEvent);
				}
				TestFalse(TEXT("A per-function entry identity is derived"), Function.EntryIdentity.IsEmpty());
			}
			TestTrue(TEXT("Two mapped functions get distinct identities"),
				Plan.Functions[0].EntryIdentity != Plan.Functions[1].EntryIdentity);
		}
		const TSharedPtr<FJsonObject> Inventory = FCortexGraphInterfaceMigration::MakeInventory(Prepared.InterfacePlan);
		if (TestTrue(TEXT("The inventory is published"), Inventory.IsValid()))
		{
			const TArray<TSharedPtr<FJsonValue>>* Functions = nullptr;
			TestTrue(TEXT("The inventory lists the functions"), Inventory->TryGetArrayField(TEXT("functions"), Functions) && Functions->Num() == 2);
			TestTrue(TEXT("The inventory lists the caller and override refusals"),
				Inventory->HasField(TEXT("in_asset_callers")) && Inventory->HasField(TEXT("derived_overrides")) && Inventory->HasField(TEXT("dropped_stubs")));
		}
	}
	{
		FCortexGraphPreparedPatch Again;
		FCortexCommandResult Error;
		TestTrue(TEXT("A second preview succeeds"),
			FCortexGraphPatchOps::Preflight(Impl.Blueprint, Request(Impl.Blueprint, TEXT("00000000-0000-0000-0000-000000122101"),
				MigrationJson(OldPath, NativePath(), FullFunctionMap())), Again, Error));
		TestEqual(TEXT("The validation hash is stable for an identical request"), Again.ValidationHash, FirstHash);
	}
	TestEqual(TEXT("The preview changed no graph, interface or node"), StructureDigest(Impl.Blueprint), StructureBefore);
	TestEqual(TEXT("The preview did not touch the dirty flag"), Impl.Blueprint->GetOutermost()->IsDirty(), bDirtyBefore);
	TestEqual(TEXT("The interface list is unchanged"), Impl.Blueprint->ImplementedInterfaces.Num(), 1);
	TestTrue(TEXT("The graph is still the description's graph"),
		Impl.Blueprint->ImplementedInterfaces[0].Graphs.Num() == 1 && Impl.Blueprint->ImplementedInterfaces[0].Graphs[0]->GraphGuid == GraphGuidBefore);

	// A refusal changes nothing.
	const auto ExpectRefusal = [&](const TCHAR* Label, UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Migration,
		const FString& ExpectedCode, const TCHAR* ExpectedText)
	{
		const FString Before = StructureDigest(Blueprint);
		const bool bBefore = Blueprint->GetOutermost()->IsDirty();
		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult Error;
		const bool bPreviewed = FCortexGraphPatchOps::Preflight(Blueprint, Request(Blueprint, TEXT("00000000-0000-0000-0000-000000122102"), Migration), Prepared, Error);
		TestFalse(FString::Printf(TEXT("%s is refused"), Label), bPreviewed);
		TestEqual(FString::Printf(TEXT("%s: error code [%s]"), Label, *Error.ErrorMessage), Error.ErrorCode, ExpectedCode);
		TestTrue(FString::Printf(TEXT("%s: the refusal names '%s' [%s]"), Label, ExpectedText, *Error.ErrorMessage), Error.ErrorMessage.Contains(ExpectedText));
		TestEqual(FString::Printf(TEXT("%s: nothing changed"), Label), StructureDigest(Blueprint), Before);
		TestEqual(FString::Printf(TEXT("%s: the dirty flag is untouched"), Label), Blueprint->GetOutermost()->IsDirty(), bBefore);
	};

	{
		UBlueprint* const Unused = MakeInterface(Scope, TEXT("BPI_Issue122Unused"));
		if (TestNotNull(TEXT("Unused BPI"), Unused))
		{
			ExpectRefusal(TEXT("An old interface the Blueprint does not implement"), Impl.Blueprint,
				MigrationJson(Unused->GetPathName(), NativePath(), FullFunctionMap()), CortexErrorCodes::InvalidOperation, TEXT("does not implement"));
		}
	}
	{
		// The new interface is already implemented: a second BPI whose function the Blueprint also implements.
		UBlueprint* const Other = MakeInterface(Scope, TEXT("BPI_Issue122Other"), TEXT("OtherValue"), TEXT("OtherNotify"));
		FImplementer Both;
		if (TestNotNull(TEXT("Other BPI"), Other) && TestTrue(TEXT("Both-implementer"), MakeImplementer(Scope, Interface, TEXT("BP_Issue122Both"), Both))
			&& TestTrue(TEXT("The second interface is implemented"),
				FBlueprintEditorUtils::ImplementNewInterface(Both.Blueprint, FTopLevelAssetPath(Other->GeneratedClass))))
		{
			ExpectRefusal(TEXT("A new interface that is already implemented"), Both.Blueprint,
				MigrationJson(Interface->GetPathName(), Other->GetPathName(), FullFunctionMap()), CortexErrorCodes::InvalidOperation, TEXT("already implements"));
		}
	}
	{
		TArray<TSharedPtr<FJsonValue>> OnlyNotify;
		OnlyNotify.Add(MapEntry(TEXT("Notify"), nullptr, EventPinMap()));
		ExpectRefusal(TEXT("An implemented old function missing from function_map"), Impl.Blueprint,
			MigrationJson(OldPath, NativePath(), OnlyNotify), CortexErrorCodes::InvalidOperation, TEXT("GetValue"));
	}
	{
		TArray<TSharedPtr<FJsonValue>> Map;
		Map.Add(MapEntry(TEXT("GetValue"), TEXT("NoSuchFunction"), FunctionPinMap()));
		Map.Add(MapEntry(TEXT("Notify"), nullptr, EventPinMap()));
		ExpectRefusal(TEXT("A function that is not on the new interface"), Impl.Blueprint,
			MigrationJson(OldPath, NativePath(), Map), CortexErrorCodes::InvalidField, TEXT("NoSuchFunction"));
	}
	{
		TArray<TSharedPtr<FJsonValue>> Map;
		Map.Add(MapEntry(TEXT("GetValue"), TEXT("Notify"), EventPinMap()));
		Map.Add(MapEntry(TEXT("Notify"), TEXT("GetValue"), FunctionPinMap()));
		ExpectRefusal(TEXT("A function mapped across implementation styles"), Impl.Blueprint,
			MigrationJson(OldPath, NativePath(), Map), CortexErrorCodes::InvalidOperation, TEXT("style"));
	}
	{
		TArray<TSharedPtr<FJsonValue>> Map;
		Map.Add(MapEntry(TEXT("GetValue"), nullptr, FunctionPinMap(/*bWithResult=*/false)));
		Map.Add(MapEntry(TEXT("Notify"), nullptr, EventPinMap()));
		ExpectRefusal(TEXT("An incomplete pin_map"), Impl.Blueprint,
			MigrationJson(OldPath, NativePath(), Map), CortexErrorCodes::InvalidField, TEXT("ReturnValue"));
	}
	{
		FImplementer Caller;
		if (TestTrue(TEXT("Caller implementer"), MakeImplementer(Scope, Interface, TEXT("BP_Issue122Caller"), Caller)))
		{
			AddMessageCaller(Caller, Interface->GeneratedClass);
			ExpectRefusal(TEXT("An in-asset message caller of the old interface"), Caller.Blueprint,
				MigrationJson(OldPath, NativePath(), FullFunctionMap()), CortexErrorCodes::InvalidOperation, TEXT("K2Node_Message"));
		}
	}
	{
		FImplementer Parent;
		if (TestTrue(TEXT("Parent implementer"), MakeImplementer(Scope, Interface, TEXT("BP_Issue122Parent"), Parent)))
		{
			UBlueprint* const Child = FKismetEditorUtilities::CreateBlueprint(
				Parent.Blueprint->GeneratedClass, CreatePackage(*FString::Printf(TEXT("/Game/Temp/BP_Issue122Child_%s"), *UniqueSuffix())),
				FName(TEXT("BP_Issue122Child")), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
			if (TestNotNull(TEXT("Derived Blueprint"), Child))
			{
				Scope.Blueprints.Add(Child);
				UEdGraph* const ChildEvents = FBlueprintEditorUtils::FindEventGraph(Child);
				UK2Node_Event* const Override = NewObject<UK2Node_Event>(ChildEvents);
				Override->EventReference.SetExternalMember(TEXT("Notify"), Interface->GeneratedClass);
				Override->bOverrideFunction = true;
				Override->CreateNewGuid();
				Override->AllocateDefaultPins();
				ChildEvents->AddNode(Override, true, false);
				ExpectRefusal(TEXT("A derived Blueprint overriding a mapped function"), Parent.Blueprint,
					MigrationJson(OldPath, NativePath(), FullFunctionMap()), CortexErrorCodes::InvalidOperation, TEXT("BP_Issue122Child"));
			}
		}
	}
	{
		FImplementer Big;
		if (TestTrue(TEXT("Big implementer"), MakeImplementer(Scope, Interface, TEXT("BP_Issue122Big"), Big)))
		{
			for (int32 Index = 0; Index < FCortexGraphPatchOps::MaxScannedNodes + 16; ++Index)
			{
				UK2Node_ExecutionSequence* Filler = NewObject<UK2Node_ExecutionSequence>(Big.EventGraph);
				Filler->CreateNewGuid();
				Filler->AllocateDefaultPins();
				Big.EventGraph->AddNode(Filler, true, false);
			}
			ExpectRefusal(TEXT("A Blueprint beyond max_scanned_nodes"), Big.Blueprint,
				MigrationJson(OldPath, NativePath(), FullFunctionMap()), CortexErrorCodes::LimitExceeded, TEXT("max_scanned_nodes"));
		}
	}
	{
		// A request carrying 'target' is refused (the interface swap addresses the whole Blueprint).
		TSharedPtr<FJsonObject> WithTarget = Request(Impl.Blueprint, TEXT("00000000-0000-0000-0000-000000122103"),
			MigrationJson(OldPath, NativePath(), FullFunctionMap()));
		WithTarget->SetObjectField(TEXT("target"), MakeShared<FJsonObject>());
		WithTarget->SetArrayField(TEXT("nodes"), TArray<TSharedPtr<FJsonValue>>());
		FCortexGraphPreparedPatch Prepared;
		FCortexCommandResult Error;
		TestFalse(TEXT("A target is refused"), FCortexGraphPatchOps::Preflight(Impl.Blueprint, WithTarget, Prepared, Error));
		TestEqual(TEXT("... as INVALID_FIELD"), Error.ErrorCode, FString(CortexErrorCodes::InvalidField));
	}
	TestEqual(TEXT("Every refusal left the first fixture exactly as it was"), StructureDigest(Impl.Blueprint), StructureBefore);
	return true;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
