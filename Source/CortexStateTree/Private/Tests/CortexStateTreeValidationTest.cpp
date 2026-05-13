#include "Misc/AutomationTest.h"
#include "CortexStateTreeCommandHandler.h"
#include "CortexStateTreeTestUtils.h"
#include "CortexTypes.h"

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
