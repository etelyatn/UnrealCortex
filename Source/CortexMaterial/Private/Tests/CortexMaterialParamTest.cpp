#include "Misc/AutomationTest.h"
#include "CortexMaterialCommandHandler.h"
#include "CortexTypes.h"
#include "Misc/Guid.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialListParamsTest,
	"Cortex.Material.Param.ListParameters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialListParamsTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestParams_%s"), *Suffix);
	const FString Dir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_Params_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *Dir, *MatName);

	FCortexMaterialCommandHandler Handler;

	// Create material
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), Dir);
	CreateParams->SetStringField(TEXT("name"), MatName);
	Handler.Execute(TEXT("create_material"), CreateParams);

	// List parameters
	TSharedPtr<FJsonObject> ListParams = MakeShared<FJsonObject>();
	ListParams->SetStringField(TEXT("asset_path"), MatPath);
	FCortexCommandResult Result = Handler.Execute(TEXT("list_parameters"), ListParams);

	TestTrue(TEXT("list_parameters should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* ScalarParams = nullptr;
		TestTrue(TEXT("Should have scalar array"),
			Result.Data->TryGetArrayField(TEXT("scalar"), ScalarParams));

		const TArray<TSharedPtr<FJsonValue>>* VectorParams = nullptr;
		TestTrue(TEXT("Should have vector array"),
			Result.Data->TryGetArrayField(TEXT("vector"), VectorParams));

		const TArray<TSharedPtr<FJsonValue>>* TextureParams = nullptr;
		TestTrue(TEXT("Should have texture array"),
			Result.Data->TryGetArrayField(TEXT("texture"), TextureParams));
	}

	// Cleanup
	UObject* MatAsset = LoadObject<UMaterial>(nullptr, *MatPath);
	if (MatAsset) MatAsset->MarkAsGarbage();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialGetParamNotFoundTest,
	"Cortex.Material.Param.GetParameter.NotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialGetParamNotFoundTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestGetParam_%s"), *Suffix);
	const FString Dir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_GetParam_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *Dir, *MatName);

	FCortexMaterialCommandHandler Handler;

	// Create material
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), Dir);
	CreateParams->SetStringField(TEXT("name"), MatName);
	Handler.Execute(TEXT("create_material"), CreateParams);

	// Get non-existent parameter
	TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
	GetParams->SetStringField(TEXT("asset_path"), MatPath);
	GetParams->SetStringField(TEXT("parameter_name"), TEXT("NonExistentParam"));
	FCortexCommandResult Result = Handler.Execute(TEXT("get_parameter"), GetParams);

	TestFalse(TEXT("Should fail for non-existent parameter"), Result.bSuccess);
	TestEqual(TEXT("Error should be PARAMETER_NOT_FOUND"),
		Result.ErrorCode, CortexErrorCodes::ParameterNotFound);

	// Cleanup
	UObject* MatAsset = LoadObject<UMaterial>(nullptr, *MatPath);
	if (MatAsset) MatAsset->MarkAsGarbage();

	return true;
}
