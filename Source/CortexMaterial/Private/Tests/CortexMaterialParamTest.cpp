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
		const TSharedPtr<FJsonObject>* ParametersObj = nullptr;
		TestTrue(TEXT("Should have parameters object"),
			Result.Data->TryGetObjectField(TEXT("parameters"), ParametersObj));

		if (ParametersObj && (*ParametersObj).IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* ScalarParams = nullptr;
			TestTrue(TEXT("Should have scalar array"),
				(*ParametersObj)->TryGetArrayField(TEXT("scalar"), ScalarParams));

			const TArray<TSharedPtr<FJsonValue>>* VectorParams = nullptr;
			TestTrue(TEXT("Should have vector array"),
				(*ParametersObj)->TryGetArrayField(TEXT("vector"), VectorParams));

			const TArray<TSharedPtr<FJsonValue>>* TextureParams = nullptr;
			TestTrue(TEXT("Should have texture array"),
				(*ParametersObj)->TryGetArrayField(TEXT("texture"), TextureParams));
		}
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialSetParamTest,
	"Cortex.Material.Param.SetParameter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialSetParamTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestSetParent_%s"), *Suffix);
	const FString InstName = FString::Printf(TEXT("MI_TestSet_%s"), *Suffix);
	const FString Dir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_Set_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *Dir, *MatName);
	const FString InstPath = FString::Printf(TEXT("%s/%s"), *Dir, *InstName);

	FCortexMaterialCommandHandler Handler;

	// Create material + instance
	TSharedPtr<FJsonObject> MatParams = MakeShared<FJsonObject>();
	MatParams->SetStringField(TEXT("asset_path"), Dir);
	MatParams->SetStringField(TEXT("name"), MatName);
	Handler.Execute(TEXT("create_material"), MatParams);

	TSharedPtr<FJsonObject> InstParams = MakeShared<FJsonObject>();
	InstParams->SetStringField(TEXT("asset_path"), Dir);
	InstParams->SetStringField(TEXT("name"), InstName);
	InstParams->SetStringField(TEXT("parent_material"), MatPath);
	Handler.Execute(TEXT("create_instance"), InstParams);

	// Set a scalar parameter (even if it doesn't exist, instance should accept it)
	TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
	SetParams->SetStringField(TEXT("asset_path"), InstPath);
	SetParams->SetStringField(TEXT("parameter_name"), TEXT("TestScalar"));
	SetParams->SetStringField(TEXT("parameter_type"), TEXT("scalar"));
	SetParams->SetNumberField(TEXT("value"), 0.5);
	FCortexCommandResult Result = Handler.Execute(TEXT("set_parameter"), SetParams);

	TestTrue(TEXT("set_parameter should succeed"), Result.bSuccess);

	// Cleanup
	UObject* InstAsset = LoadObject<UMaterialInstanceConstant>(nullptr, *InstPath);
	if (InstAsset) InstAsset->MarkAsGarbage();
	UObject* MatAsset = LoadObject<UMaterial>(nullptr, *MatPath);
	if (MatAsset) MatAsset->MarkAsGarbage();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialSetParamsBatchTest,
	"Cortex.Material.Param.SetParameters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialSetParamsBatchTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestBatchParent_%s"), *Suffix);
	const FString InstName = FString::Printf(TEXT("MI_TestBatch_%s"), *Suffix);
	const FString Dir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_Batch_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *Dir, *MatName);
	const FString InstPath = FString::Printf(TEXT("%s/%s"), *Dir, *InstName);

	FCortexMaterialCommandHandler Handler;

	// Create material + instance
	TSharedPtr<FJsonObject> MatParams = MakeShared<FJsonObject>();
	MatParams->SetStringField(TEXT("asset_path"), Dir);
	MatParams->SetStringField(TEXT("name"), MatName);
	Handler.Execute(TEXT("create_material"), MatParams);

	TSharedPtr<FJsonObject> InstParams = MakeShared<FJsonObject>();
	InstParams->SetStringField(TEXT("asset_path"), Dir);
	InstParams->SetStringField(TEXT("name"), InstName);
	InstParams->SetStringField(TEXT("parent_material"), MatPath);
	Handler.Execute(TEXT("create_instance"), InstParams);

	// Set multiple parameters in batch
	TArray<TSharedPtr<FJsonValue>> ParamsArray;

	TSharedRef<FJsonObject> Param1 = MakeShared<FJsonObject>();
	Param1->SetStringField(TEXT("parameter_name"), TEXT("TestScalar"));
	Param1->SetStringField(TEXT("parameter_type"), TEXT("scalar"));
	Param1->SetNumberField(TEXT("value"), 0.75);
	ParamsArray.Add(MakeShared<FJsonValueObject>(Param1));

	TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
	SetParams->SetStringField(TEXT("asset_path"), InstPath);
	SetParams->SetArrayField(TEXT("parameters"), ParamsArray);
	FCortexCommandResult Result = Handler.Execute(TEXT("set_parameters"), SetParams);

	TestTrue(TEXT("set_parameters should succeed"), Result.bSuccess);

	// Cleanup
	UObject* InstAsset = LoadObject<UMaterialInstanceConstant>(nullptr, *InstPath);
	if (InstAsset) InstAsset->MarkAsGarbage();
	UObject* MatAsset = LoadObject<UMaterial>(nullptr, *MatPath);
	if (MatAsset) MatAsset->MarkAsGarbage();

	return true;
}
