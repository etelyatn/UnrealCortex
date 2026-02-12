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
