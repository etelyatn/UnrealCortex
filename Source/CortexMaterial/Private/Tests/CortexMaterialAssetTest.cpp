#include "Misc/AutomationTest.h"
#include "CortexMaterialCommandHandler.h"
#include "CortexTypes.h"
#include "Misc/Guid.h"
#include "Materials/Material.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialCreateTest,
	"Cortex.Material.Asset.CreateMaterial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialCreateTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestCreate_%s"), *Suffix);
	const FString MatDir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *MatDir, *MatName);

	FCortexMaterialCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), MatDir);
	Params->SetStringField(TEXT("name"), MatName);

	FCortexCommandResult Result = Handler.Execute(TEXT("create_material"), Params);

	TestTrue(TEXT("create_material should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		FString AssetPath;
		Result.Data->TryGetStringField(TEXT("asset_path"), AssetPath);
		TestEqual(TEXT("asset_path should match"), AssetPath, MatPath);

		bool bCreated = false;
		Result.Data->TryGetBoolField(TEXT("created"), bCreated);
		TestTrue(TEXT("created should be true"), bCreated);
	}

	// Verify loadable
	UObject* LoadedAsset = LoadObject<UMaterial>(nullptr, *MatPath);
	TestNotNull(TEXT("Material should be loadable"), LoadedAsset);

	// Cleanup
	if (LoadedAsset)
	{
		LoadedAsset->MarkAsGarbage();
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialCreateMissingParamsTest,
	"Cortex.Material.Asset.CreateMaterial.MissingParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialCreateMissingParamsTest::RunTest(const FString& Parameters)
{
	FCortexMaterialCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	FCortexCommandResult Result = Handler.Execute(TEXT("create_material"), Params);

	TestFalse(TEXT("Should fail with missing params"), Result.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_FIELD"),
		Result.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialCreateDuplicateTest,
	"Cortex.Material.Asset.CreateMaterial.Duplicate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialCreateDuplicateTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestDup_%s"), *Suffix);
	const FString MatDir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_Dup_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *MatDir, *MatName);

	FCortexMaterialCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), MatDir);
	Params->SetStringField(TEXT("name"), MatName);

	FCortexCommandResult Result1 = Handler.Execute(TEXT("create_material"), Params);
	TestTrue(TEXT("First create should succeed"), Result1.bSuccess);

	FCortexCommandResult Result2 = Handler.Execute(TEXT("create_material"), Params);
	TestFalse(TEXT("Duplicate create should fail"), Result2.bSuccess);
	TestEqual(TEXT("Error should be ASSET_ALREADY_EXISTS"),
		Result2.ErrorCode, CortexErrorCodes::AssetAlreadyExists);

	// Cleanup
	UObject* LoadedAsset = LoadObject<UMaterial>(nullptr, *MatPath);
	if (LoadedAsset)
	{
		LoadedAsset->MarkAsGarbage();
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialListTest,
	"Cortex.Material.Asset.ListMaterials",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialListTest::RunTest(const FString& Parameters)
{
	FCortexMaterialCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	FCortexCommandResult Result = Handler.Execute(TEXT("list_materials"), Params);

	TestTrue(TEXT("list_materials should succeed"), Result.bSuccess);
	TestTrue(TEXT("Result should have data"), Result.Data.IsValid());

	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* MaterialsArray = nullptr;
		TestTrue(TEXT("Data should have materials array"),
			Result.Data->TryGetArrayField(TEXT("materials"), MaterialsArray));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialGetTest,
	"Cortex.Material.Asset.GetMaterial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialGetTest::RunTest(const FString& Parameters)
{
	// Create a material first
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestGet_%s"), *Suffix);
	const FString MatDir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_Get_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *MatDir, *MatName);

	FCortexMaterialCommandHandler Handler;
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), MatDir);
	CreateParams->SetStringField(TEXT("name"), MatName);
	Handler.Execute(TEXT("create_material"), CreateParams);

	// Get material
	TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
	GetParams->SetStringField(TEXT("asset_path"), MatPath);
	FCortexCommandResult Result = Handler.Execute(TEXT("get_material"), GetParams);

	TestTrue(TEXT("get_material should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		FString Name;
		Result.Data->TryGetStringField(TEXT("name"), Name);
		TestEqual(TEXT("name should match"), Name, MatName);

		FString Domain;
		Result.Data->TryGetStringField(TEXT("material_domain"), Domain);
		TestFalse(TEXT("material_domain should be populated"), Domain.IsEmpty());

		FString BlendMode;
		Result.Data->TryGetStringField(TEXT("blend_mode"), BlendMode);
		TestFalse(TEXT("blend_mode should be populated"), BlendMode.IsEmpty());

		FString ShadingModel;
		Result.Data->TryGetStringField(TEXT("shading_model"), ShadingModel);
		TestFalse(TEXT("shading_model should be populated"), ShadingModel.IsEmpty());
	}

	// Cleanup
	UObject* LoadedAsset = LoadObject<UMaterial>(nullptr, *MatPath);
	if (LoadedAsset)
	{
		LoadedAsset->MarkAsGarbage();
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialGetNotFoundTest,
	"Cortex.Material.Asset.GetMaterial.NotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialGetNotFoundTest::RunTest(const FString& Parameters)
{
	FCortexMaterialCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistent/M_Fake"));

	FCortexCommandResult Result = Handler.Execute(TEXT("get_material"), Params);

	TestFalse(TEXT("Should fail for non-existent material"), Result.bSuccess);
	TestEqual(TEXT("Error code should be MATERIAL_NOT_FOUND"),
		Result.ErrorCode, CortexErrorCodes::MaterialNotFound);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialDeleteTest,
	"Cortex.Material.Asset.DeleteMaterial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialDeleteTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestDel_%s"), *Suffix);
	const FString MatDir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_Del_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *MatDir, *MatName);

	FCortexMaterialCommandHandler Handler;

	// Create
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), MatDir);
	CreateParams->SetStringField(TEXT("name"), MatName);
	Handler.Execute(TEXT("create_material"), CreateParams);

	// Delete
	TSharedPtr<FJsonObject> DeleteParams = MakeShared<FJsonObject>();
	DeleteParams->SetStringField(TEXT("asset_path"), MatPath);
	FCortexCommandResult Result = Handler.Execute(TEXT("delete_material"), DeleteParams);

	TestTrue(TEXT("delete_material should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		bool bDeleted = false;
		Result.Data->TryGetBoolField(TEXT("deleted"), bDeleted);
		TestTrue(TEXT("deleted should be true"), bDeleted);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialDeleteNotFoundTest,
	"Cortex.Material.Asset.DeleteMaterial.NotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialDeleteNotFoundTest::RunTest(const FString& Parameters)
{
	FCortexMaterialCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistent/M_Fake"));

	FCortexCommandResult Result = Handler.Execute(TEXT("delete_material"), Params);

	TestFalse(TEXT("Should fail for non-existent"), Result.bSuccess);
	TestEqual(TEXT("Error should be MATERIAL_NOT_FOUND"),
		Result.ErrorCode, CortexErrorCodes::MaterialNotFound);

	return true;
}
