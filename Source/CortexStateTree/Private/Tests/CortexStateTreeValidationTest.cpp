#include "Misc/AutomationTest.h"
#include "CortexSTCompat.h"
#include "CortexSTTypes.h"
#include "CortexStateTreeCommandHandler.h"
#include "CortexStateTreeTestUtils.h"
#include "CortexTypes.h"
#include "Operations/CortexSTAssetOps.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreeTypes.h"

namespace
{
UStateTreeState* GetRootState(UStateTree* StateTree)
{
	if (StateTree == nullptr)
	{
		return nullptr;
	}

	const UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(StateTree->EditorData);
	if (EditorData == nullptr || EditorData->SubTrees.Num() == 0)
	{
		return nullptr;
	}

	return EditorData->SubTrees[0];
}

bool GetValidationFlag(const FCortexCommandResult& Result, bool& bOutValid)
{
	if (!Result.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Validation = nullptr;
	return Result.Data->TryGetObjectField(TEXT("validation"), Validation)
		&& Validation != nullptr
		&& Validation->IsValid()
		&& (*Validation)->TryGetBoolField(TEXT("valid"), bOutValid);
}

bool GetFingerprintDirtyFlag(const TSharedPtr<FJsonObject>& Data, bool& bOutDirty)
{
	if (!Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Fingerprint = nullptr;
	return Data->TryGetObjectField(TEXT("fingerprint"), Fingerprint)
		&& Fingerprint != nullptr
		&& Fingerprint->IsValid()
		&& (*Fingerprint)->TryGetBoolField(TEXT("is_dirty"), bOutDirty);
}

bool GetDirectFingerprintDirtyFlag(const TSharedPtr<FJsonObject>& Fingerprint, bool& bOutDirty)
{
	return Fingerprint.IsValid() && Fingerprint->TryGetBoolField(TEXT("is_dirty"), bOutDirty);
}

void DeleteWithCurrentFingerprint(FCortexStateTreeCommandHandler& Handler, const FString& AssetPath)
{
	if (UStateTree* StateTree = LoadObject<UStateTree>(nullptr, *AssetPath))
	{
		TSharedPtr<FJsonObject> DeleteParams = CortexStateTreeTest::Params();
		DeleteParams->SetStringField(TEXT("asset_path"), AssetPath);
		DeleteParams->SetObjectField(TEXT("expected_fingerprint"), CortexST::MakeFingerprint(StateTree));
		(void)Handler.Execute(TEXT("delete_asset"), DeleteParams);
		return;
	}

	CortexStateTreeTest::DeleteIfLoaded(AssetPath);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeValidationCommandsTest,
	"Cortex.StateTree.Validation.Commands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeValidationCommandsTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	const FString AssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_Validation"));

	TSharedPtr<FJsonObject> CreateParams = CortexStateTreeTest::Params();
	CreateParams->SetStringField(TEXT("asset_path"), AssetPath);
	CreateParams->SetStringField(TEXT("schema_class"), CortexStateTreeTest::GetTestSchemaClassPath());

	const FCortexCommandResult Create = Handler.Execute(TEXT("create_asset"), CreateParams);
	TestTrue(TEXT("create succeeds"), Create.bSuccess);

	TSharedPtr<FJsonObject> CheckParams = CortexStateTreeTest::Params();
	CheckParams->SetStringField(TEXT("asset_path"), AssetPath);

	const FCortexCommandResult Check = Handler.Execute(TEXT("check_structure"), CheckParams);
	TestTrue(TEXT("check_structure succeeds"), Check.bSuccess);
	TestTrue(TEXT("check_structure returns validation"),
		Check.Data.IsValid() && Check.Data->HasTypedField<EJson::Object>(TEXT("validation")));

	TSharedPtr<FJsonObject> ValidateWithoutFingerprint = CortexStateTreeTest::Params();
	ValidateWithoutFingerprint->SetStringField(TEXT("asset_path"), AssetPath);

	const FCortexCommandResult ValidateFail = Handler.Execute(TEXT("validate_asset"), ValidateWithoutFingerprint);
	TestFalse(TEXT("validate_asset without fingerprint fails"), ValidateFail.bSuccess);
	TestEqual(TEXT("validate_asset without fingerprint uses stale precondition"),
		ValidateFail.ErrorCode,
		CortexErrorCodes::StalePrecondition);

	TSharedPtr<FJsonObject> ValidateParams = CortexStateTreeTest::Params();
	ValidateParams->SetStringField(TEXT("asset_path"), AssetPath);
	if (Create.Data.IsValid() && Create.Data->HasTypedField<EJson::Object>(TEXT("fingerprint")))
	{
		ValidateParams->SetObjectField(TEXT("expected_fingerprint"), Create.Data->GetObjectField(TEXT("fingerprint")));
	}

	const FCortexCommandResult Validate = Handler.Execute(TEXT("validate_asset"), ValidateParams);
	TestTrue(TEXT("validate_asset with fingerprint succeeds"), Validate.bSuccess);
	TestTrue(TEXT("validate_asset returns fingerprint"),
		Validate.Data.IsValid() && Validate.Data->HasTypedField<EJson::Object>(TEXT("fingerprint")));

	TSharedPtr<FJsonObject> CompileParams = CortexStateTreeTest::Params();
	CompileParams->SetStringField(TEXT("asset_path"), AssetPath);
	if (Validate.Data.IsValid() && Validate.Data->HasTypedField<EJson::Object>(TEXT("fingerprint")))
	{
		CompileParams->SetObjectField(TEXT("expected_fingerprint"), Validate.Data->GetObjectField(TEXT("fingerprint")));
	}

	const FCortexCommandResult Compile = Handler.Execute(TEXT("compile"), CompileParams);
	TestTrue(TEXT("compile with fingerprint succeeds"), Compile.bSuccess);
	TestTrue(TEXT("compile returns diagnostics"),
		Compile.Data.IsValid() && Compile.Data->HasTypedField<EJson::Array>(TEXT("diagnostics")));

	CortexStateTreeTest::DeleteIfLoaded(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeValidationNonGotoTransitionTest,
	"Cortex.StateTree.Validation.NonGotoTransition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeValidationNonGotoTransitionTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	const FString AssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_ValidationNonGoto"));

	TSharedPtr<FJsonObject> CreateParams = CortexStateTreeTest::Params();
	CreateParams->SetStringField(TEXT("asset_path"), AssetPath);
	CreateParams->SetStringField(TEXT("schema_class"), CortexStateTreeTest::GetTestSchemaClassPath());

	const FCortexCommandResult Create = Handler.Execute(TEXT("create_asset"), CreateParams);
	TestTrue(TEXT("create succeeds"), Create.bSuccess);

	UStateTree* StateTree = LoadObject<UStateTree>(nullptr, *AssetPath);
	TestNotNull(TEXT("StateTree loads"), StateTree);
	UStateTreeState* RootState = GetRootState(StateTree);
	TestNotNull(TEXT("root state exists"), RootState);
	if (StateTree == nullptr || RootState == nullptr)
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	StateTree->Modify();
	RootState->Modify();
	FStateTreeTransition& CompletionTransition = CortexSTCompat::AddTransition(
		*RootState,
		EStateTreeTransitionTrigger::OnStateCompleted,
		EStateTreeTransitionType::Succeeded,
		nullptr);
	CompletionTransition.State.LinkType = EStateTreeTransitionType::Succeeded;
	CompletionTransition.State.ID.Invalidate();
	CompletionTransition.State.Name = NAME_None;
	StateTree->MarkPackageDirty();

	TSharedPtr<FJsonObject> CheckParams = CortexStateTreeTest::Params();
	CheckParams->SetStringField(TEXT("asset_path"), AssetPath);
	const FCortexCommandResult Check = Handler.Execute(TEXT("check_structure"), CheckParams);
	TestTrue(TEXT("check_structure succeeds"), Check.bSuccess);

	bool bValid = false;
	TestTrue(TEXT("check_structure returns validation.valid"), GetValidationFlag(Check, bValid));
	TestTrue(TEXT("non-goto completion transition stays valid in check_structure"), bValid);

	TSharedPtr<FJsonObject> ValidateParams = CortexStateTreeTest::Params();
	ValidateParams->SetStringField(TEXT("asset_path"), AssetPath);
	ValidateParams->SetObjectField(TEXT("expected_fingerprint"), CortexST::MakeFingerprint(StateTree));

	const FCortexCommandResult Validate = Handler.Execute(TEXT("validate_asset"), ValidateParams);
	TestTrue(TEXT("validate_asset succeeds"), Validate.bSuccess);
	bValid = false;
	TestTrue(TEXT("validate_asset returns validation.valid"), GetValidationFlag(Validate, bValid));
	TestTrue(TEXT("non-goto completion transition stays valid in validate_asset"), bValid);

	CortexStateTreeTest::DeleteIfLoaded(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexStateTreeCompileMarksDirtyWithStableCompiledDataTest,
	"Cortex.StateTree.Validation.CompileDirtyWithStableCompiledData",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexStateTreeCompileMarksDirtyWithStableCompiledDataTest::RunTest(const FString& Parameters)
{
	FCortexStateTreeCommandHandler Handler;
	const FString AssetPath = CortexStateTreeTest::MakeAssetPath(TEXT("ST_CompileDirtyStable"));

	TSharedPtr<FJsonObject> CreateParams = CortexStateTreeTest::Params();
	CreateParams->SetStringField(TEXT("asset_path"), AssetPath);
	CreateParams->SetStringField(TEXT("schema_class"), CortexStateTreeTest::GetTestSchemaClassPath());

	const FCortexCommandResult Create = Handler.Execute(TEXT("create_asset"), CreateParams);
	TestTrue(TEXT("create succeeds"), Create.bSuccess);

	UStateTree* StateTree = LoadObject<UStateTree>(nullptr, *AssetPath);
	TestNotNull(TEXT("StateTree loads"), StateTree);
	UStateTreeState* RootState = GetRootState(StateTree);
	TestNotNull(TEXT("root state exists"), RootState);
	if (StateTree == nullptr || RootState == nullptr)
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	StateTree->Modify();
	RootState->Modify();
	UStateTreeState& ChildState = RootState->AddChildState(TEXT("Child"), EStateTreeStateType::State);
	ChildState.Modify();
	StateTree->MarkPackageDirty();

	TSharedPtr<FJsonObject> InitialCompileParams = CortexStateTreeTest::Params();
	InitialCompileParams->SetStringField(TEXT("asset_path"), AssetPath);
	InitialCompileParams->SetObjectField(TEXT("expected_fingerprint"), CortexST::MakeFingerprint(StateTree));
	const FCortexCommandResult InitialCompile = Handler.Execute(TEXT("compile"), InitialCompileParams);
	TestTrue(TEXT("initial compile establishes stable compiled data"), InitialCompile.bSuccess);
	if (!InitialCompile.bSuccess)
	{
		DeleteWithCurrentFingerprint(Handler, AssetPath);
		return false;
	}

	const FCortexCommandResult SaveResult = FCortexSTAssetOps::SaveAsset(AssetPath);
	TestTrue(TEXT("save fixture succeeds"), SaveResult.bSuccess);
	if (!SaveResult.bSuccess)
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TestFalse(TEXT("saved fixture package is clean"), StateTree->GetOutermost()->IsDirty());
	TestTrue(TEXT("compiled fixture is ready before modify"), StateTree->IsReadyToRun());
	const uint32 StableCompiledHash = StateTree->LastCompiledEditorDataHash;
	TestNotEqual(TEXT("compiled fixture has stable hash"), StableCompiledHash, 0u);

	StateTree->Modify();
	TestTrue(TEXT("Modify alone marks package dirty"), StateTree->GetOutermost()->IsDirty());

	TSharedPtr<FJsonObject> DirtyFingerprint = CortexST::MakeFingerprint(StateTree);
	bool bModifyDirty = false;
	TestTrue(TEXT("fingerprint after Modify exists"), GetDirectFingerprintDirtyFlag(DirtyFingerprint, bModifyDirty));
	TestTrue(TEXT("Modify alone returns dirty fingerprint"), bModifyDirty);

	const FCortexCommandResult SaveAfterModify = FCortexSTAssetOps::SaveAsset(AssetPath);
	TestTrue(TEXT("save after Modify succeeds"), SaveAfterModify.bSuccess);
	if (!SaveAfterModify.bSuccess)
	{
		CortexStateTreeTest::DeleteIfLoaded(AssetPath);
		return false;
	}

	TestFalse(TEXT("fixture is clean again before compile command"), StateTree->GetOutermost()->IsDirty());
	TestTrue(TEXT("fixture stays ready before compile command"), StateTree->IsReadyToRun());
	TestEqual(TEXT("compiled hash stays stable before compile command"), StateTree->LastCompiledEditorDataHash, StableCompiledHash);

	const TSharedPtr<FJsonObject> ExpectedFingerprint = CortexST::MakeFingerprint(StateTree);
	bool bExpectedDirty = true;
	TestTrue(TEXT("pre-compile fingerprint present"), GetDirectFingerprintDirtyFlag(ExpectedFingerprint, bExpectedDirty));
	TestFalse(TEXT("pre-compile fingerprint is clean"), bExpectedDirty);

	TSharedPtr<FJsonObject> CompileParams = CortexStateTreeTest::Params();
	CompileParams->SetStringField(TEXT("asset_path"), AssetPath);
	CompileParams->SetObjectField(TEXT("expected_fingerprint"), ExpectedFingerprint);

	const FCortexCommandResult Compile = Handler.Execute(TEXT("compile"), CompileParams);
	TestTrue(TEXT("compile succeeds with stable compiled data"), Compile.bSuccess);
	TestTrue(TEXT("compile keeps asset ready"), StateTree->IsReadyToRun());
	TestEqual(TEXT("compile keeps compiled hash stable"), StateTree->LastCompiledEditorDataHash, StableCompiledHash);
	TestTrue(TEXT("compile marks package dirty even with stable compiled data"), StateTree->GetOutermost()->IsDirty());

	bool bReturnedDirty = false;
	TestTrue(TEXT("compile returns fingerprint with dirty flag"), GetFingerprintDirtyFlag(Compile.Data, bReturnedDirty));
	TestTrue(TEXT("compile returns dirty fingerprint with stable compiled data"), bReturnedDirty);

	DeleteWithCurrentFingerprint(Handler, AssetPath);
	return true;
}
