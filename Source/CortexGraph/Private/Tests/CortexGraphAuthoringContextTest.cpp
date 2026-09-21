#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "Operations/CortexGraphAuthoringContext.h"
#include "Operations/CortexGraphPatchState.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Blueprint/UserWidget.h"
#include "WidgetBlueprint.h"
#include "Dom/JsonObject.h"

#if WITH_EDITOR

namespace CortexGraphAuthoringTestHelpers
{
	void CleanupPackage(UPackage* Pkg, UObject* Asset = nullptr)
	{
		if (!Pkg) return;
		Pkg->SetDirtyFlag(false);
		if (Pkg->IsRooted())
		{
			Pkg->RemoveFromRoot();
		}
		Pkg->ClearFlags(RF_Standalone);

		if (Asset)
		{
			Asset->ClearFlags(RF_Standalone);
			Asset->MarkAsGarbage();
		}
		Pkg->MarkAsGarbage();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphAuthoringContextReadTest,
	"Cortex.Graph.Authoring.Context.Read",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphAuthoringContextReadTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UPackage* WidgetPkg = CreatePackage(TEXT("/Temp/WBP_TestWidget_T01"));
	UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(
		FKismetEditorUtilities::CreateBlueprint(
			UUserWidget::StaticClass(),
			WidgetPkg,
			FName("WBP_TestWidget_T01"),
			BPTYPE_Normal,
			UWidgetBlueprint::StaticClass(),
			UWidgetBlueprintGeneratedClass::StaticClass()
		)
	);
	TestNotNull(TEXT("WidgetBP created"), WidgetBP);
	if (!WidgetBP)
	{
		CortexGraphAuthoringTestHelpers::CleanupPackage(WidgetPkg);
		return false;
	}

	// 1. Create candidate 1: EventGraph (ubergraph)
	UEdGraph* EventGraph = FBlueprintEditorUtils::CreateNewGraph(
		WidgetBP, FName("EventGraph"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	EventGraph->Schema = UEdGraphSchema_K2::StaticClass();
	if (!EventGraph->GraphGuid.IsValid())
	{
		EventGraph->GraphGuid = FGuid::NewGuid();
	}
	FBlueprintEditorUtils::AddUbergraphPage(WidgetBP, EventGraph);

	// 2. Create candidate 2: Function graph
	UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
		WidgetBP, FName("MyTestFunction"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FuncGraph->Schema = UEdGraphSchema_K2::StaticClass();
	if (!FuncGraph->GraphGuid.IsValid())
	{
		FuncGraph->GraphGuid = FGuid::NewGuid();
	}
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(WidgetBP, FuncGraph, false, nullptr);

	// 3. Make EventGraph non-empty by adding a node
	UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(EventGraph);
	CallNode->CreateNewGuid();
	CallNode->FunctionReference.SetExternalMember(FName("PrintString"), UKismetSystemLibrary::StaticClass());
	EventGraph->AddNode(CallNode);
	CallNode->AllocateDefaultPins();

	const bool bDirtyBefore = WidgetPkg->IsDirty();
	const int32 EventGraphNodeCountBefore = EventGraph->Nodes.Num();

	// 4. Call FCortexGraphAuthoringContext::Read without target
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), WidgetBP->GetPathName());

	FCortexCommandResult Result = FCortexGraphAuthoringContext::Read(Params);
	TestTrue(TEXT("Read context succeeds"), Result.bSuccess);
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		CortexGraphAuthoringTestHelpers::CleanupPackage(WidgetPkg, WidgetBP);
		return false;
	}

	// Require canonical asset IDs and metadata
	TestEqual(TEXT("canonical asset_path"), Result.Data->GetStringField(TEXT("asset_path")), WidgetBP->GetPathName());
	TestEqual(TEXT("asset_class is WidgetBlueprint"), Result.Data->GetStringField(TEXT("asset_class")), FString(TEXT("WidgetBlueprint")));
	TestEqual(TEXT("parent_class is UserWidget"), Result.Data->GetStringField(TEXT("parent_class")), UUserWidget::StaticClass()->GetPathName());

	// Require bounded candidates and exact kind in graph_choices
	const TArray<TSharedPtr<FJsonValue>>* GraphChoices = nullptr;
	TestTrue(TEXT("graph_choices array exists"), Result.Data->TryGetArrayField(TEXT("graph_choices"), GraphChoices));
	if (GraphChoices)
	{
		TestTrue(TEXT("graph_choices has at least 2 candidate contexts"), GraphChoices->Num() >= 2);

		bool bFoundEventGraph = false;
		bool bFoundFuncGraph = false;

		for (const auto& Val : *GraphChoices)
		{
			const TSharedPtr<FJsonObject> Choice = Val->AsObject();
			if (!Choice.IsValid()) continue;

			const FString Name = Choice->GetStringField(TEXT("graph_name"));
			const FString Kind = Choice->GetStringField(TEXT("graph_kind"));
			const FString GuidStr = Choice->GetStringField(TEXT("graph_guid"));
			const bool bMutable = Choice->GetBoolField(TEXT("is_mutable"));

			FGuid ParsedGuid;
			TestTrue(TEXT("graph_guid is valid GUID string"), FGuid::Parse(GuidStr, ParsedGuid));

			if (Name == TEXT("EventGraph"))
			{
				bFoundEventGraph = true;
				TestEqual(TEXT("EventGraph kind is ubergraph"), Kind, FString(TEXT("ubergraph")));
				TestTrue(TEXT("EventGraph is mutable"), bMutable);
				TestEqual(TEXT("EventGraph guid matches"), GuidStr, EventGraph->GraphGuid.ToString());
			}
			else if (Name == TEXT("MyTestFunction"))
			{
				bFoundFuncGraph = true;
				TestEqual(TEXT("MyTestFunction kind is function"), Kind, FString(TEXT("function")));
				TestTrue(TEXT("MyTestFunction is mutable"), bMutable);
				TestEqual(TEXT("MyTestFunction guid matches"), GuidStr, FuncGraph->GraphGuid.ToString());
			}
		}

		TestTrue(TEXT("found EventGraph in choices"), bFoundEventGraph);
		TestTrue(TEXT("found MyTestFunction in choices"), bFoundFuncGraph);
	}

	// Require fingerprint with version=1 and nonempty hash
	const TSharedPtr<FJsonObject>* FingerprintObj = nullptr;
	TestTrue(TEXT("fingerprint exists in response"), Result.Data->TryGetObjectField(TEXT("fingerprint"), FingerprintObj));
	if (FingerprintObj && FingerprintObj->IsValid())
	{
		int32 Version = 0;
		TestTrue(TEXT("graph_authoring_version is present"), (*FingerprintObj)->TryGetNumberField(TEXT("graph_authoring_version"), Version));
		TestEqual(TEXT("graph_authoring_version equals 1"), Version, 1);

		FString Hash;
		TestTrue(TEXT("graph_authoring_hash is present"), (*FingerprintObj)->TryGetStringField(TEXT("graph_authoring_hash"), Hash));
		TestFalse(TEXT("graph_authoring_hash is nonempty"), Hash.IsEmpty());

		TestTrue(TEXT("package_saved_hash exists"), (*FingerprintObj)->HasField(TEXT("package_saved_hash")));
		TestTrue(TEXT("is_dirty exists"), (*FingerprintObj)->HasField(TEXT("is_dirty")));
		TestTrue(TEXT("dirty_epoch exists"), (*FingerprintObj)->HasField(TEXT("dirty_epoch")));
		TestTrue(TEXT("not_ready exists"), (*FingerprintObj)->HasField(TEXT("not_ready")));
	}

	// Require no mutation
	TestEqual(TEXT("read context leaves package dirtiness unchanged"), WidgetPkg->IsDirty(), bDirtyBefore);
	TestEqual(TEXT("read context leaves node count unchanged"), EventGraph->Nodes.Num(), EventGraphNodeCountBefore);

	// 5. Test with target graph_ref
	TSharedPtr<FJsonObject> TargetParams = MakeShared<FJsonObject>();
	TargetParams->SetStringField(TEXT("asset_path"), WidgetBP->GetPathName());
	TSharedPtr<FJsonObject> TargetRef = MakeShared<FJsonObject>();
	TargetRef->SetStringField(TEXT("graph_guid"), EventGraph->GraphGuid.ToString());
	TargetRef->SetStringField(TEXT("graph_kind"), TEXT("ubergraph"));
	TargetRef->SetStringField(TEXT("subgraph_path"), TEXT(""));
	TSharedPtr<FJsonObject> TargetWrapper = MakeShared<FJsonObject>();
	TargetWrapper->SetObjectField(TEXT("graph_ref"), TargetRef);
	TargetParams->SetObjectField(TEXT("target"), TargetWrapper);

	FCortexCommandResult TargetResult = FCortexGraphAuthoringContext::Read(TargetParams);
	TestTrue(TEXT("Read context with target succeeds"), TargetResult.bSuccess);
	if (TargetResult.bSuccess && TargetResult.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* ResolvedTarget = nullptr;
		TestTrue(TEXT("target returned in response"), TargetResult.Data->TryGetObjectField(TEXT("target"), ResolvedTarget));
		if (ResolvedTarget && ResolvedTarget->IsValid())
		{
			TestEqual(TEXT("resolved target graph_guid matches"), (*ResolvedTarget)->GetStringField(TEXT("graph_guid")), EventGraph->GraphGuid.ToString());
			TestEqual(TEXT("resolved target graph_kind matches"), (*ResolvedTarget)->GetStringField(TEXT("graph_kind")), FString(TEXT("ubergraph")));
			TestEqual(TEXT("resolved target graph_name matches"), (*ResolvedTarget)->GetStringField(TEXT("graph_name")), FString(TEXT("EventGraph")));
		}
	}

	CortexGraphAuthoringTestHelpers::CleanupPackage(WidgetPkg, WidgetBP);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphAuthoringContextFailingCasesTest,
	"Cortex.Graph.Authoring.Context.FailingCases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphAuthoringContextFailingCasesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UPackage* WidgetPkg = CreatePackage(TEXT("/Temp/WBP_TestWidget_T01_Fail"));
	UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(
		FKismetEditorUtilities::CreateBlueprint(
			UUserWidget::StaticClass(),
			WidgetPkg,
			FName("WBP_TestWidget_T01_Fail"),
			BPTYPE_Normal,
			UWidgetBlueprint::StaticClass(),
			UWidgetBlueprintGeneratedClass::StaticClass()
		)
	);
	TestNotNull(TEXT("WidgetBP created"), WidgetBP);
	if (!WidgetBP)
	{
		CortexGraphAuthoringTestHelpers::CleanupPackage(WidgetPkg);
		return false;
	}

	UEdGraph* EventGraph = FBlueprintEditorUtils::CreateNewGraph(
		WidgetBP, FName("EventGraph"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	EventGraph->Schema = UEdGraphSchema_K2::StaticClass();
	if (!EventGraph->GraphGuid.IsValid())
	{
		EventGraph->GraphGuid = FGuid::NewGuid();
	}
	FBlueprintEditorUtils::AddUbergraphPage(WidgetBP, EventGraph);

	// Case 1: Ambiguous flat name
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), WidgetBP->GetPathName());
		Params->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));

		FCortexCommandResult Result = FCortexGraphAuthoringContext::Read(Params);
		TestFalse(TEXT("ambiguous top-level flat graph_name fails"), Result.bSuccess);
		TestEqual(TEXT("ambiguous flat name error code"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), WidgetBP->GetPathName());
		TSharedPtr<FJsonObject> FlatTarget = MakeShared<FJsonObject>();
		FlatTarget->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
		Params->SetObjectField(TEXT("target"), FlatTarget);

		FCortexCommandResult Result = FCortexGraphAuthoringContext::Read(Params);
		TestFalse(TEXT("ambiguous flat graph_name in target fails"), Result.bSuccess);
		TestEqual(TEXT("ambiguous flat name in target error code"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// Case 2: Wrong GUID / kind
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), WidgetBP->GetPathName());
		TSharedPtr<FJsonObject> Ref = MakeShared<FJsonObject>();
		Ref->SetStringField(TEXT("graph_guid"), FGuid::NewGuid().ToString());
		Ref->SetStringField(TEXT("graph_kind"), TEXT("ubergraph"));
		TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetObjectField(TEXT("graph_ref"), Ref);
		Params->SetObjectField(TEXT("target"), Target);

		FCortexCommandResult Result = FCortexGraphAuthoringContext::Read(Params);
		TestFalse(TEXT("nonexistent GUID fails"), Result.bSuccess);
		TestEqual(TEXT("wrong GUID error code is GraphNotFound"), Result.ErrorCode, CortexErrorCodes::GraphNotFound);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), WidgetBP->GetPathName());
		TSharedPtr<FJsonObject> Ref = MakeShared<FJsonObject>();
		Ref->SetStringField(TEXT("graph_guid"), EventGraph->GraphGuid.ToString());
		Ref->SetStringField(TEXT("graph_kind"), TEXT("function")); // Actual is ubergraph
		TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetObjectField(TEXT("graph_ref"), Ref);
		Params->SetObjectField(TEXT("target"), Target);

		FCortexCommandResult Result = FCortexGraphAuthoringContext::Read(Params);
		TestFalse(TEXT("wrong graph_kind fails"), Result.bSuccess);
		TestEqual(TEXT("wrong kind error code is InvalidField"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// Case 3: Read-only delegate graph
	{
		UEdGraph* DelegateGraph = FBlueprintEditorUtils::CreateNewGraph(
			WidgetBP, FName("MyDelegate"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		DelegateGraph->Schema = UEdGraphSchema_K2::StaticClass();
		if (!DelegateGraph->GraphGuid.IsValid())
		{
			DelegateGraph->GraphGuid = FGuid::NewGuid();
		}
		WidgetBP->DelegateSignatureGraphs.Add(DelegateGraph);

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), WidgetBP->GetPathName());
		TSharedPtr<FJsonObject> Ref = MakeShared<FJsonObject>();
		Ref->SetStringField(TEXT("graph_guid"), DelegateGraph->GraphGuid.ToString());
		Ref->SetStringField(TEXT("graph_kind"), TEXT("delegate"));
		TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetObjectField(TEXT("graph_ref"), Ref);
		Params->SetObjectField(TEXT("target"), Target);

		FCortexCommandResult Result = FCortexGraphAuthoringContext::Read(Params);
		TestFalse(TEXT("read-only delegate graph target fails"), Result.bSuccess);
		TestEqual(TEXT("delegate target error code is InvalidOperation"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
	}

	// Case 4: Unsupported subpath
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), WidgetBP->GetPathName());
		TSharedPtr<FJsonObject> Ref = MakeShared<FJsonObject>();
		Ref->SetStringField(TEXT("graph_guid"), EventGraph->GraphGuid.ToString());
		Ref->SetStringField(TEXT("graph_kind"), TEXT("ubergraph"));
		Ref->SetStringField(TEXT("subgraph_path"), TEXT("Depth1.Depth2.Depth3.Depth4.Depth5")); // Exceeds MaxDepth (4)
		TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetObjectField(TEXT("graph_ref"), Ref);
		Params->SetObjectField(TEXT("target"), Target);

		FCortexCommandResult Result = FCortexGraphAuthoringContext::Read(Params);
		TestFalse(TEXT("unsupported subpath fails"), Result.bSuccess);
	}

	// Case 5: Unready class context
	{
		UPackage* UnreadyPkg = CreatePackage(TEXT("/Temp/BP_Unready_T01"));
		UBlueprint* UnreadyBP = NewObject<UBlueprint>(
			UnreadyPkg, UBlueprint::StaticClass(), FName("BP_Unready_T01"), RF_Public | RF_Standalone);
		// Note: UnreadyBP->ParentClass and GeneratedClass are nullptr!

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), UnreadyBP->GetPathName());

		FCortexCommandResult Result = FCortexGraphAuthoringContext::Read(Params);
		TestFalse(TEXT("unready class context fails"), Result.bSuccess);
		TestEqual(TEXT("unready class error code is InvalidOperation"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);

		CortexGraphAuthoringTestHelpers::CleanupPackage(UnreadyPkg, UnreadyBP);
	}

	CortexGraphAuthoringTestHelpers::CleanupPackage(WidgetPkg, WidgetBP);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphAuthoringFingerprintTest,
	"Cortex.Graph.Authoring.Context.Fingerprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphAuthoringFingerprintTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UPackage* Pkg = CreatePackage(TEXT("/Temp/BP_TestActor_T01_FP"));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		Pkg,
		FName("BP_TestActor_T01_FP"),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);
	TestNotNull(TEXT("Blueprint created"), Blueprint);
	if (!Blueprint)
	{
		CortexGraphAuthoringTestHelpers::CleanupPackage(Pkg);
		return false;
	}

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	TestNotNull(TEXT("EventGraph exists"), EventGraph);
	if (!EventGraph)
	{
		CortexGraphAuthoringTestHelpers::CleanupPackage(Pkg, Blueprint);
		return false;
	}

	UK2Node_CallFunction* ExistingNode = NewObject<UK2Node_CallFunction>(EventGraph);
	ExistingNode->CreateNewGuid();
	ExistingNode->NodePosX = 100;
	ExistingNode->NodePosY = 100;
	ExistingNode->FunctionReference.SetExternalMember(FName("PrintString"), UKismetSystemLibrary::StaticClass());
	EventGraph->AddNode(ExistingNode);
	ExistingNode->AllocateDefaultPins();

	// Case 6: Same-status edits with equal Blueprint status
	const bool bDirtyBefore = Blueprint->GetOutermost()->IsDirty();
	const auto Before = FCortexGraphPatchState::ComputeFingerprint(Blueprint);
	ExistingNode->NodePosX += 37; // Authored layout edit; do not save or change Status.
	const auto After = FCortexGraphPatchState::ComputeFingerprint(Blueprint);

	TestNotEqual(TEXT("same-status authoring edit changes guard"),
		Before->GetStringField(TEXT("graph_authoring_hash")),
		After->GetStringField(TEXT("graph_authoring_hash")));
	TestEqual(TEXT("hash read does not alter dirtiness"),
		Blueprint->GetOutermost()->IsDirty(), bDirtyBefore);

	// Stable hash on no-op
	const auto AfterNoOp = FCortexGraphPatchState::ComputeFingerprint(Blueprint);
	TestEqual(TEXT("no-op produces identical hash"),
		After->GetStringField(TEXT("graph_authoring_hash")),
		AfterNoOp->GetStringField(TEXT("graph_authoring_hash")));

	CortexGraphAuthoringTestHelpers::CleanupPackage(Pkg, Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphAuthoringPreconditionTest,
	"Cortex.Graph.Authoring.Context.Precondition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexGraphAuthoringPreconditionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	TSharedPtr<FJsonObject> Current = MakeShared<FJsonObject>();
	Current->SetStringField(TEXT("package_saved_hash"), TEXT("hash123"));
	Current->SetBoolField(TEXT("is_dirty"), false);
	Current->SetStringField(TEXT("dirty_epoch"), TEXT("1"));
	Current->SetBoolField(TEXT("not_ready"), false);
	Current->SetNumberField(TEXT("graph_authoring_version"), 1);
	Current->SetStringField(TEXT("graph_authoring_hash"), TEXT("valid_hash_456"));

	// 1. Missing version in Expected
	{
		TSharedPtr<FJsonObject> Expected = MakeShared<FJsonObject>();
		Expected->SetStringField(TEXT("package_saved_hash"), TEXT("hash123"));
		Expected->SetBoolField(TEXT("is_dirty"), false);
		Expected->SetStringField(TEXT("dirty_epoch"), TEXT("1"));
		Expected->SetBoolField(TEXT("not_ready"), false);
		Expected->SetStringField(TEXT("graph_authoring_hash"), TEXT("valid_hash_456"));

		FCortexCommandResult Error;
		TestFalse(TEXT("missing version fails"), FCortexGraphPatchState::ValidatePrecondition(Expected, Current, Error));
		TestEqual(TEXT("error code is StalePrecondition"), Error.ErrorCode, CortexErrorCodes::StalePrecondition);
	}

	// 2. Missing hash in Expected
	{
		TSharedPtr<FJsonObject> Expected = MakeShared<FJsonObject>();
		Expected->SetStringField(TEXT("package_saved_hash"), TEXT("hash123"));
		Expected->SetBoolField(TEXT("is_dirty"), false);
		Expected->SetStringField(TEXT("dirty_epoch"), TEXT("1"));
		Expected->SetBoolField(TEXT("not_ready"), false);
		Expected->SetNumberField(TEXT("graph_authoring_version"), 1);

		FCortexCommandResult Error;
		TestFalse(TEXT("missing hash fails"), FCortexGraphPatchState::ValidatePrecondition(Expected, Current, Error));
		TestEqual(TEXT("error code is StalePrecondition"), Error.ErrorCode, CortexErrorCodes::StalePrecondition);
	}

	// 3. Hash mismatch
	{
		TSharedPtr<FJsonObject> Expected = MakeShared<FJsonObject>();
		Expected->SetStringField(TEXT("package_saved_hash"), TEXT("hash123"));
		Expected->SetBoolField(TEXT("is_dirty"), false);
		Expected->SetStringField(TEXT("dirty_epoch"), TEXT("1"));
		Expected->SetBoolField(TEXT("not_ready"), false);
		Expected->SetNumberField(TEXT("graph_authoring_version"), 1);
		Expected->SetStringField(TEXT("graph_authoring_hash"), TEXT("stale_hash_789"));

		FCortexCommandResult Error;
		TestFalse(TEXT("mismatched hash fails"), FCortexGraphPatchState::ValidatePrecondition(Expected, Current, Error));
		TestEqual(TEXT("error code is StalePrecondition"), Error.ErrorCode, CortexErrorCodes::StalePrecondition);
	}

	// 4. Unknown field in Expected
	{
		TSharedPtr<FJsonObject> Expected = MakeShared<FJsonObject>();
		Expected->SetStringField(TEXT("package_saved_hash"), TEXT("hash123"));
		Expected->SetBoolField(TEXT("is_dirty"), false);
		Expected->SetStringField(TEXT("dirty_epoch"), TEXT("1"));
		Expected->SetBoolField(TEXT("not_ready"), false);
		Expected->SetNumberField(TEXT("graph_authoring_version"), 1);
		Expected->SetStringField(TEXT("graph_authoring_hash"), TEXT("valid_hash_456"));
		Expected->SetStringField(TEXT("unknown_caller_hash"), TEXT("extra"));

		FCortexCommandResult Error;
		TestFalse(TEXT("unknown field fails"), FCortexGraphPatchState::ValidatePrecondition(Expected, Current, Error));
	}

	// 5. Valid matching Expected
	{
		TSharedPtr<FJsonObject> Expected = MakeShared<FJsonObject>();
		Expected->SetStringField(TEXT("package_saved_hash"), TEXT("hash123"));
		Expected->SetBoolField(TEXT("is_dirty"), false);
		Expected->SetStringField(TEXT("dirty_epoch"), TEXT("1"));
		Expected->SetBoolField(TEXT("not_ready"), false);
		Expected->SetNumberField(TEXT("graph_authoring_version"), 1);
		Expected->SetStringField(TEXT("graph_authoring_hash"), TEXT("valid_hash_456"));

		FCortexCommandResult Error;
		TestTrue(TEXT("valid matching expected succeeds"), FCortexGraphPatchState::ValidatePrecondition(Expected, Current, Error));
	}

	return true;
}

#endif // WITH_EDITOR
