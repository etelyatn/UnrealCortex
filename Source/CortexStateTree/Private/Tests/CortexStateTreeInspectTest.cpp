#include "Misc/AutomationTest.h"
#include "CortexStateTreeCommandHandler.h"
#include "CortexStateTreeTestUtils.h"
#include "CortexSTTypes.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"

namespace
{
bool CreateTestStateTree(
	FAutomationTestBase& Test,
	FCortexStateTreeCommandHandler& Handler,
	const FString& AssetPath,
	const FString& RootName = TEXT("Root"))
{
	TSharedPtr<FJsonObject> CreateParams = CortexStateTreeTest::Params();
	CreateParams->SetStringField(TEXT("asset_path"), AssetPath);
	CreateParams->SetStringField(TEXT("schema_class"), CortexStateTreeTest::GetTestSchemaClassPath());
	CreateParams->SetStringField(TEXT("root_name"), RootName);
	CreateParams->SetBoolField(TEXT("save"), false);

	const FCortexCommandResult CreateResult = Handler.Execute(TEXT("create_asset"), CreateParams);
	Test.TestTrue(TEXT("create succeeds"), CreateResult.bSuccess);
	return CreateResult.bSuccess;
}

UStateTreeState* GetRootState(const FString& AssetPath)
{
	FCortexSTAssetContext Context;
	FCortexCommandResult Error;
	if (!CortexST::LoadAssetContext(AssetPath, Context, Error))
	{
		return nullptr;
	}

	return Context.EditorData != nullptr && Context.EditorData->SubTrees.Num() > 0
		? Context.EditorData->SubTrees[0]
		: nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeDumpTreeTest,
	"Cortex.StateTree.Inspect.DumpTree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeDumpTreeTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	const FString AssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_DumpTree"));

	if (!CreateTestStateTree(*this, Handler, AssetPath))
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	UStateTreeState* RootState = GetRootState(AssetPath);
	TestNotNull(TEXT("root state loads"), RootState);
	if (RootState != nullptr)
	{
		RootState->AddChildState(TEXT("Duplicate"));
		RootState->AddChildState(TEXT("Duplicate"));
	}

	TSharedPtr<FJsonObject> DumpParams = CortexStateTreeTest::Params();
	DumpParams->SetStringField(TEXT("asset_path"), AssetPath);
	DumpParams->SetBoolField(TEXT("include_transitions"), true);
	DumpParams->SetBoolField(TEXT("include_nodes"), true);

	const FCortexCommandResult DumpResult = Handler.Execute(TEXT("dump_tree"), DumpParams);
	TestTrue(TEXT("dump succeeds"), DumpResult.bSuccess);
	TestTrue(TEXT("dump includes data"), DumpResult.Data.IsValid());
	if (DumpResult.Data.IsValid())
	{
		FString ReturnedAssetPath;
		TestTrue(TEXT("dump includes top-level asset_path"),
			DumpResult.Data->TryGetStringField(TEXT("asset_path"), ReturnedAssetPath));
		TestEqual(TEXT("dump asset_path matches object path"), ReturnedAssetPath, AssetPath);
		TestTrue(TEXT("dump includes states array"),
			DumpResult.Data->HasTypedField<EJson::Array>(TEXT("states")));
		TestTrue(TEXT("dump includes validation object"),
			DumpResult.Data->HasTypedField<EJson::Object>(TEXT("validation")));
		TestTrue(TEXT("dump includes fingerprint object"),
			DumpResult.Data->HasTypedField<EJson::Object>(TEXT("fingerprint")));
		const TSharedPtr<FJsonObject>* Validation = nullptr;
		TestTrue(TEXT("dump returns validation payload"),
			DumpResult.Data->TryGetObjectField(TEXT("validation"), Validation) && Validation != nullptr && Validation->IsValid());
		if (Validation != nullptr && Validation->IsValid())
		{
			bool bValid = true;
			TestTrue(TEXT("dump returns validation.valid"), (*Validation)->TryGetBoolField(TEXT("valid"), bValid));
			TestTrue(TEXT("duplicate state paths remain structurally valid"), bValid);

			const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
			TestTrue(TEXT("dump returns validation warnings"),
				(*Validation)->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings != nullptr);
			if (Warnings != nullptr)
			{
				bool bFoundAmbiguousPathWarning = false;
				for (const TSharedPtr<FJsonValue>& WarningValue : *Warnings)
				{
					if (WarningValue.IsValid() && WarningValue->AsString().Contains(TEXT("Ambiguous state path: Root/Duplicate")))
					{
						bFoundAmbiguousPathWarning = true;
						break;
					}
				}

				TestTrue(TEXT("dump surfaces ambiguous path warning"), bFoundAmbiguousPathWarning);
			}
		}
	}

	CortexStateTreeTest::DeleteIfLoaded(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeGetStateByIdTest,
	"Cortex.StateTree.Inspect.GetState.ById",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeGetStateByIdTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	const FString AssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_GetStateById"));

	if (!CreateTestStateTree(*this, Handler, AssetPath))
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	UStateTreeState* RootState = GetRootState(AssetPath);
	TestNotNull(TEXT("root state loads"), RootState);
	if (RootState == nullptr)
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> GetParams = CortexStateTreeTest::Params();
	GetParams->SetStringField(TEXT("asset_path"), AssetPath);
	GetParams->SetStringField(TEXT("state_id"), RootState->ID.ToString(EGuidFormats::DigitsWithHyphens));

	const FCortexCommandResult GetResult = Handler.Execute(TEXT("get_state"), GetParams);
	TestTrue(TEXT("get_state by id succeeds"), GetResult.bSuccess);
	TestTrue(TEXT("get_state by id includes data"), GetResult.Data.IsValid());
	if (GetResult.Data.IsValid())
	{
		FString ReturnedId;
		FString ReturnedPath;
		FString ReturnedAssetPath;
		TestTrue(TEXT("state id is returned"),
			GetResult.Data->TryGetStringField(TEXT("id"), ReturnedId));
		TestEqual(TEXT("returned id matches root"), ReturnedId, RootState->ID.ToString(EGuidFormats::DigitsWithHyphens));
		TestTrue(TEXT("state path is returned"),
			GetResult.Data->TryGetStringField(TEXT("path"), ReturnedPath));
		TestEqual(TEXT("returned path matches root"), ReturnedPath, FString(TEXT("Root")));
		TestTrue(TEXT("get_state by id includes top-level asset_path"),
			GetResult.Data->TryGetStringField(TEXT("asset_path"), ReturnedAssetPath));
		TestEqual(TEXT("get_state by id asset_path matches object path"), ReturnedAssetPath, AssetPath);
		TestTrue(TEXT("get_state by id includes validation object"),
			GetResult.Data->HasTypedField<EJson::Object>(TEXT("validation")));
	}

	CortexStateTreeTest::DeleteIfLoaded(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeGetStateByPathTest,
	"Cortex.StateTree.Inspect.GetState.ByPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeGetStateByPathTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	const FString AssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_GetStateByPath"));

	if (!CreateTestStateTree(*this, Handler, AssetPath))
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> GetParams = CortexStateTreeTest::Params();
	GetParams->SetStringField(TEXT("asset_path"), AssetPath);
	GetParams->SetStringField(TEXT("state_path"), TEXT("Root"));

	const FCortexCommandResult GetResult = Handler.Execute(TEXT("get_state"), GetParams);
	TestTrue(TEXT("get_state succeeds"), GetResult.bSuccess);
	TestTrue(TEXT("get_state includes data"), GetResult.Data.IsValid());
	if (GetResult.Data.IsValid())
	{
		FString StatePath;
		FString ReturnedAssetPath;
		TestTrue(TEXT("state path is returned"),
			GetResult.Data->TryGetStringField(TEXT("path"), StatePath));
		TestEqual(TEXT("state_path resolves root"), StatePath, FString(TEXT("Root")));
		TestTrue(TEXT("get_state includes top-level asset_path"),
			GetResult.Data->TryGetStringField(TEXT("asset_path"), ReturnedAssetPath));
		TestEqual(TEXT("get_state asset_path matches object path"), ReturnedAssetPath, AssetPath);
		TestTrue(TEXT("get_state includes fingerprint object"),
			GetResult.Data->HasTypedField<EJson::Object>(TEXT("fingerprint")));
		TestTrue(TEXT("get_state includes validation object"),
			GetResult.Data->HasTypedField<EJson::Object>(TEXT("validation")));
	}

	CortexStateTreeTest::DeleteIfLoaded(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeGetStateDefaultsToRootTest,
	"Cortex.StateTree.Inspect.GetState.DefaultsToRoot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeGetStateDefaultsToRootTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	const FString AssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_GetStateDefaultRoot"));

	if (!CreateTestStateTree(*this, Handler, AssetPath))
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TSharedPtr<FJsonObject> GetParams = CortexStateTreeTest::Params();
	GetParams->SetStringField(TEXT("asset_path"), AssetPath);

	const FCortexCommandResult GetResult = Handler.Execute(TEXT("get_state"), GetParams);
	TestTrue(TEXT("get_state without selector succeeds"), GetResult.bSuccess);
	TestTrue(TEXT("get_state without selector includes data"), GetResult.Data.IsValid());
	if (GetResult.Data.IsValid())
	{
		FString ReturnedPath;
		TestTrue(TEXT("default root path is returned"),
			GetResult.Data->TryGetStringField(TEXT("path"), ReturnedPath));
		TestEqual(TEXT("missing selector resolves root"), ReturnedPath, FString(TEXT("Root")));
	}

	CortexStateTreeTest::DeleteIfLoaded(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeGetStateRejectsConflictingSelectorsTest,
	"Cortex.StateTree.Inspect.GetState.RejectsConflictingSelectors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeGetStateRejectsConflictingSelectorsTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	const FString AssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_GetStateConflict"));

	if (!CreateTestStateTree(*this, Handler, AssetPath))
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	UStateTreeState* RootState = GetRootState(AssetPath);
	TestNotNull(TEXT("root state loads"), RootState);

	TSharedPtr<FJsonObject> GetParams = CortexStateTreeTest::Params();
	GetParams->SetStringField(TEXT("asset_path"), AssetPath);
	GetParams->SetStringField(TEXT("state_id"), RootState != nullptr ? RootState->ID.ToString(EGuidFormats::DigitsWithHyphens) : FString());
	GetParams->SetStringField(TEXT("state_path"), TEXT("Root"));

	const FCortexCommandResult GetResult = Handler.Execute(TEXT("get_state"), GetParams);
	TestFalse(TEXT("get_state rejects conflicting selectors"), GetResult.bSuccess);
	TestEqual(TEXT("conflicting selectors use invalid field"), GetResult.ErrorCode, CortexErrorCodes::InvalidField);

	CortexStateTreeTest::DeleteIfLoaded(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeGetStateRejectsAmbiguousPathTest,
	"Cortex.StateTree.Inspect.GetState.RejectsAmbiguousPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeGetStateRejectsAmbiguousPathTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	const FString AssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_GetStateAmbiguous"));

	if (!CreateTestStateTree(*this, Handler, AssetPath))
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	UStateTreeState* RootState = GetRootState(AssetPath);
	TestNotNull(TEXT("root state loads"), RootState);
	if (RootState == nullptr)
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	UStateTreeState& FirstDuplicate = RootState->AddChildState(TEXT("Duplicate"));
	UStateTreeState& SecondDuplicate = RootState->AddChildState(TEXT("Duplicate"));

	TSharedPtr<FJsonObject> GetParams = CortexStateTreeTest::Params();
	GetParams->SetStringField(TEXT("asset_path"), AssetPath);
	GetParams->SetStringField(TEXT("state_path"), TEXT("Root/Duplicate"));

	const FCortexCommandResult GetResult = Handler.Execute(TEXT("get_state"), GetParams);
	TestFalse(TEXT("ambiguous path is rejected"), GetResult.bSuccess);
	TestEqual(TEXT("ambiguous path uses dedicated error"), GetResult.ErrorCode, CortexErrorCodes::AmbiguousStatePath);

	const TArray<TSharedPtr<FJsonValue>>* MatchingIds = nullptr;
	TestTrue(TEXT("ambiguous path returns matching ids"),
		GetResult.ErrorDetails.IsValid()
		&& GetResult.ErrorDetails->TryGetArrayField(TEXT("matching_state_ids"), MatchingIds)
		&& MatchingIds != nullptr);
	if (MatchingIds != nullptr)
	{
		TestEqual(TEXT("matching ids count"), MatchingIds->Num(), 2);

		TSet<FString> ReturnedIds;
		for (const TSharedPtr<FJsonValue>& Value : *MatchingIds)
		{
			ReturnedIds.Add(Value->AsString());
		}

		TestTrue(TEXT("first duplicate id returned"),
			ReturnedIds.Contains(FirstDuplicate.ID.ToString(EGuidFormats::DigitsWithHyphens)));
		TestTrue(TEXT("second duplicate id returned"),
			ReturnedIds.Contains(SecondDuplicate.ID.ToString(EGuidFormats::DigitsWithHyphens)));
	}

	CortexStateTreeTest::DeleteIfLoaded(AssetPath);
	return true;
}
