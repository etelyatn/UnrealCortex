#include "Misc/AutomationTest.h"
#include "CortexMaterialCommandHandler.h"
#include "CortexTypes.h"
#include "Misc/Guid.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"

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
	FCortexMaterialGetReferencedCollectionsTest,
	"Cortex.Material.Asset.GetMaterial.ReferencedCollections",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialGetReferencedCollectionsTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestRefCol_%s"), *Suffix);
	const FString MatDir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_RefCol_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *MatDir, *MatName);

	FCortexMaterialCommandHandler Handler;

	TArray<FString> MpcPaths;
	for (int32 i = 0; i < 3; i++)
	{
		const FString MpcName = FString::Printf(TEXT("MPC_Test%d_%s"), i, *Suffix);
		const FString MpcDir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_MPC%d_%s"), i, *Suffix);

		TSharedPtr<FJsonObject> CreateMpcParams = MakeShared<FJsonObject>();
		CreateMpcParams->SetStringField(TEXT("asset_path"), MpcDir);
		CreateMpcParams->SetStringField(TEXT("name"), MpcName);
		FCortexCommandResult MpcResult = Handler.Execute(TEXT("create_collection"), CreateMpcParams);
		TestTrue(TEXT("create_collection should succeed"), MpcResult.bSuccess);

		FString MpcPath;
		if (MpcResult.Data.IsValid())
		{
			MpcResult.Data->TryGetStringField(TEXT("asset_path"), MpcPath);
		}
		MpcPaths.Add(MpcPath);
	}

	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), MatDir);
	CreateParams->SetStringField(TEXT("name"), MatName);
	Handler.Execute(TEXT("create_material"), CreateParams);

	for (int32 i = 0; i < 3; i++)
	{
		TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
		AddParams->SetStringField(TEXT("asset_path"), MatPath);
		AddParams->SetStringField(TEXT("expression_class"), TEXT("MaterialExpressionCollectionParameter"));
		FCortexCommandResult AddResult = Handler.Execute(TEXT("add_node"), AddParams);
		TestTrue(TEXT("add_node should succeed"), AddResult.bSuccess);

		FString NodeId;
		if (AddResult.Data.IsValid())
		{
			AddResult.Data->TryGetStringField(TEXT("node_id"), NodeId);
		}

		TSharedPtr<FJsonObject> SetPropParams = MakeShared<FJsonObject>();
		SetPropParams->SetStringField(TEXT("asset_path"), MatPath);
		SetPropParams->SetStringField(TEXT("node_id"), NodeId);
		SetPropParams->SetStringField(TEXT("property_name"), TEXT("Collection"));
		SetPropParams->SetStringField(TEXT("value"), MpcPaths[i]);
		Handler.Execute(TEXT("set_node_property"), SetPropParams);
	}

	TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
	GetParams->SetStringField(TEXT("asset_path"), MatPath);
	FCortexCommandResult Result = Handler.Execute(TEXT("get_material"), GetParams);

	TestTrue(TEXT("get_material should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Collections = nullptr;
		TestTrue(TEXT("referenced_collections should exist"),
			Result.Data->TryGetArrayField(TEXT("referenced_collections"), Collections));

		if (Collections)
		{
			TestEqual(TEXT("should reference 3 distinct collections"), Collections->Num(), 3);

			if (Collections->Num() > 0)
			{
				TSharedPtr<FJsonObject> FirstCol = (*Collections)[0]->AsObject();
				TestNotNull(TEXT("collection entry should be an object"), FirstCol.Get());
				if (FirstCol.IsValid())
				{
					TestTrue(TEXT("collection should have name field"),
						FirstCol->HasField(TEXT("name")));
					TestTrue(TEXT("collection should have asset_path field"),
						FirstCol->HasField(TEXT("asset_path")));
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
		TestTrue(TEXT("sm6_warnings should exist"),
			Result.Data->TryGetArrayField(TEXT("sm6_warnings"), Warnings));

		if (Warnings)
		{
			TestTrue(TEXT("should have at least one SM6 warning"), Warnings->Num() > 0);
		}
	}

	UObject* LoadedMat = LoadObject<UMaterial>(nullptr, *MatPath);
	if (LoadedMat) LoadedMat->MarkAsGarbage();

	for (const FString& MpcPath : MpcPaths)
	{
		UObject* LoadedMpc = LoadObject<UObject>(nullptr, *MpcPath);
		if (LoadedMpc) LoadedMpc->MarkAsGarbage();
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialGetInstanceReferencedCollectionsTest,
	"Cortex.Material.Asset.GetInstance.ReferencedCollections",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialGetInstanceReferencedCollectionsTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestInstRefCol_%s"), *Suffix);
	const FString MatDir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_InstRefCol_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *MatDir, *MatName);

	FCortexMaterialCommandHandler Handler;

	const FString MpcName = FString::Printf(TEXT("MPC_TestInst_%s"), *Suffix);
	const FString MpcDir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_MPCInst_%s"), *Suffix);
	TSharedPtr<FJsonObject> CreateMpcParams = MakeShared<FJsonObject>();
	CreateMpcParams->SetStringField(TEXT("asset_path"), MpcDir);
	CreateMpcParams->SetStringField(TEXT("name"), MpcName);
	FCortexCommandResult MpcResult = Handler.Execute(TEXT("create_collection"), CreateMpcParams);
	TestTrue(TEXT("create_collection should succeed"), MpcResult.bSuccess);

	FString MpcPath;
	if (MpcResult.Data.IsValid())
	{
		MpcResult.Data->TryGetStringField(TEXT("asset_path"), MpcPath);
	}

	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), MatDir);
	CreateParams->SetStringField(TEXT("name"), MatName);
	Handler.Execute(TEXT("create_material"), CreateParams);

	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("asset_path"), MatPath);
	AddParams->SetStringField(TEXT("expression_class"), TEXT("MaterialExpressionCollectionParameter"));
	FCortexCommandResult AddResult = Handler.Execute(TEXT("add_node"), AddParams);

	FString NodeId;
	if (AddResult.Data.IsValid())
	{
		AddResult.Data->TryGetStringField(TEXT("node_id"), NodeId);
	}

	TSharedPtr<FJsonObject> SetPropParams = MakeShared<FJsonObject>();
	SetPropParams->SetStringField(TEXT("asset_path"), MatPath);
	SetPropParams->SetStringField(TEXT("node_id"), NodeId);
	SetPropParams->SetStringField(TEXT("property_name"), TEXT("Collection"));
	SetPropParams->SetStringField(TEXT("value"), MpcPath);
	Handler.Execute(TEXT("set_node_property"), SetPropParams);

	const FString InstName = FString::Printf(TEXT("MI_TestInstRefCol_%s"), *Suffix);
	const FString InstDir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_InstRefColInst_%s"), *Suffix);
	TSharedPtr<FJsonObject> CreateInstParams = MakeShared<FJsonObject>();
	CreateInstParams->SetStringField(TEXT("asset_path"), InstDir);
	CreateInstParams->SetStringField(TEXT("name"), InstName);
	CreateInstParams->SetStringField(TEXT("parent_material"), MatPath);
	FCortexCommandResult InstResult = Handler.Execute(TEXT("create_instance"), CreateInstParams);
	TestTrue(TEXT("create_instance should succeed"), InstResult.bSuccess);

	FString InstPath;
	if (InstResult.Data.IsValid())
	{
		InstResult.Data->TryGetStringField(TEXT("asset_path"), InstPath);
	}

	TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
	GetParams->SetStringField(TEXT("asset_path"), InstPath);
	FCortexCommandResult Result = Handler.Execute(TEXT("get_instance"), GetParams);

	TestTrue(TEXT("get_instance should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Collections = nullptr;
		TestTrue(TEXT("referenced_collections should exist"),
			Result.Data->TryGetArrayField(TEXT("referenced_collections"), Collections));

		if (Collections)
		{
			TestEqual(TEXT("should reference 1 collection"), Collections->Num(), 1);
		}

		const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
		TestTrue(TEXT("sm6_warnings should exist"),
			Result.Data->TryGetArrayField(TEXT("sm6_warnings"), Warnings));

		if (Warnings)
		{
			TestEqual(TEXT("sm6_warnings should be empty for 1 collection"), Warnings->Num(), 0);
		}
	}

	UObject* LoadedInst = LoadObject<UObject>(nullptr, *InstPath);
	if (LoadedInst) LoadedInst->MarkAsGarbage();

	UObject* LoadedMat = LoadObject<UMaterial>(nullptr, *MatPath);
	if (LoadedMat) LoadedMat->MarkAsGarbage();

	UObject* LoadedMpc = LoadObject<UObject>(nullptr, *MpcPath);
	if (LoadedMpc) LoadedMpc->MarkAsGarbage();

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialCreateInstanceTest,
	"Cortex.Material.Asset.CreateInstance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialCreateInstanceTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestInstParent_%s"), *Suffix);
	const FString InstName = FString::Printf(TEXT("MI_TestInst_%s"), *Suffix);
	const FString Dir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_Inst_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *Dir, *MatName);
	const FString InstPath = FString::Printf(TEXT("%s/%s"), *Dir, *InstName);

	FCortexMaterialCommandHandler Handler;

	// Create parent material
	TSharedPtr<FJsonObject> MatParams = MakeShared<FJsonObject>();
	MatParams->SetStringField(TEXT("asset_path"), Dir);
	MatParams->SetStringField(TEXT("name"), MatName);
	Handler.Execute(TEXT("create_material"), MatParams);

	// Create instance
	TSharedPtr<FJsonObject> InstParams = MakeShared<FJsonObject>();
	InstParams->SetStringField(TEXT("asset_path"), Dir);
	InstParams->SetStringField(TEXT("name"), InstName);
	InstParams->SetStringField(TEXT("parent_material"), MatPath);
	FCortexCommandResult Result = Handler.Execute(TEXT("create_instance"), InstParams);

	TestTrue(TEXT("create_instance should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		FString ResultPath;
		Result.Data->TryGetStringField(TEXT("asset_path"), ResultPath);
		TestEqual(TEXT("asset_path should match"), ResultPath, InstPath);

		FString ParentPath;
		Result.Data->TryGetStringField(TEXT("parent_material"), ParentPath);
		TestEqual(TEXT("parent should match"), ParentPath, MatPath);
	}

	// Cleanup
	UObject* InstAsset = LoadObject<UMaterialInstanceConstant>(nullptr, *InstPath);
	if (InstAsset) InstAsset->MarkAsGarbage();
	UObject* MatAsset = LoadObject<UMaterial>(nullptr, *MatPath);
	if (MatAsset) MatAsset->MarkAsGarbage();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialGetInstanceTest,
	"Cortex.Material.Asset.GetInstance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialGetInstanceTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestGetInst_%s"), *Suffix);
	const FString InstName = FString::Printf(TEXT("MI_TestGetInst_%s"), *Suffix);
	const FString Dir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_GetInst_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *Dir, *MatName);
	const FString InstPath = FString::Printf(TEXT("%s/%s"), *Dir, *InstName);

	FCortexMaterialCommandHandler Handler;

	// Create parent + instance
	TSharedPtr<FJsonObject> MatParams = MakeShared<FJsonObject>();
	MatParams->SetStringField(TEXT("asset_path"), Dir);
	MatParams->SetStringField(TEXT("name"), MatName);
	Handler.Execute(TEXT("create_material"), MatParams);

	TSharedPtr<FJsonObject> InstParams = MakeShared<FJsonObject>();
	InstParams->SetStringField(TEXT("asset_path"), Dir);
	InstParams->SetStringField(TEXT("name"), InstName);
	InstParams->SetStringField(TEXT("parent_material"), MatPath);
	Handler.Execute(TEXT("create_instance"), InstParams);

	// Get instance
	TSharedPtr<FJsonObject> GetParams = MakeShared<FJsonObject>();
	GetParams->SetStringField(TEXT("asset_path"), InstPath);
	FCortexCommandResult Result = Handler.Execute(TEXT("get_instance"), GetParams);

	TestTrue(TEXT("get_instance should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		FString Name;
		Result.Data->TryGetStringField(TEXT("name"), Name);
		TestEqual(TEXT("name should match"), Name, InstName);

		FString ParentMat;
		Result.Data->TryGetStringField(TEXT("parent_material"), ParentMat);
		TestFalse(TEXT("parent_material should be populated"), ParentMat.IsEmpty());

		const TSharedPtr<FJsonObject>* Overrides = nullptr;
		TestTrue(TEXT("Should have overrides object"),
			Result.Data->TryGetObjectField(TEXT("overrides"), Overrides));
	}

	// Cleanup
	UObject* InstAsset = LoadObject<UMaterialInstanceConstant>(nullptr, *InstPath);
	if (InstAsset) InstAsset->MarkAsGarbage();
	UObject* MatAsset = LoadObject<UMaterial>(nullptr, *MatPath);
	if (MatAsset) MatAsset->MarkAsGarbage();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialListInstancesTest,
	"Cortex.Material.Asset.ListInstances",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialListInstancesTest::RunTest(const FString& Parameters)
{
	FCortexMaterialCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	FCortexCommandResult Result = Handler.Execute(TEXT("list_instances"), Params);

	TestTrue(TEXT("list_instances should succeed"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* InstancesArray = nullptr;
		TestTrue(TEXT("Data should have instances array"),
			Result.Data->TryGetArrayField(TEXT("instances"), InstancesArray));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialSetPropertyDomainTest,
	"Cortex.Material.Asset.SetMaterialProperty.Domain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialSetPropertyDomainTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestSetDomain_%s"), *Suffix);
	const FString MatDir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_SetDomain_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *MatDir, *MatName);

	FCortexMaterialCommandHandler Handler;

	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), MatDir);
	CreateParams->SetStringField(TEXT("name"), MatName);
	FCortexCommandResult CreateResult = Handler.Execute(TEXT("create_material"), CreateParams);
	TestTrue(TEXT("Material creation should succeed"), CreateResult.bSuccess);

	TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
	SetParams->SetStringField(TEXT("asset_path"), MatPath);
	SetParams->SetStringField(TEXT("property_name"), TEXT("MaterialDomain"));
	SetParams->SetStringField(TEXT("value"), TEXT("MD_PostProcess"));
	FCortexCommandResult SetResult = Handler.Execute(TEXT("set_material_property"), SetParams);

	TestTrue(TEXT("set_material_property should succeed"), SetResult.bSuccess);

	if (SetResult.Data.IsValid())
	{
		bool bUpdated = false;
		SetResult.Data->TryGetBoolField(TEXT("updated"), bUpdated);
		TestTrue(TEXT("updated should be true"), bUpdated);

		FString ResultPath;
		SetResult.Data->TryGetStringField(TEXT("asset_path"), ResultPath);
		TestEqual(TEXT("asset_path should match"), ResultPath, MatPath);
	}

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MatPath);
	TestNotNull(TEXT("Should load material"), Material);
	if (Material)
	{
		TestEqual(TEXT("MaterialDomain should be PostProcess"),
			static_cast<int32>(Material->MaterialDomain),
			static_cast<int32>(MD_PostProcess));
		Material->MarkAsGarbage();
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialSetPropertyBlendModeTest,
	"Cortex.Material.Asset.SetMaterialProperty.BlendMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialSetPropertyBlendModeTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestSetBlend_%s"), *Suffix);
	const FString MatDir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_SetBlend_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *MatDir, *MatName);

	FCortexMaterialCommandHandler Handler;

	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), MatDir);
	CreateParams->SetStringField(TEXT("name"), MatName);
	Handler.Execute(TEXT("create_material"), CreateParams);

	TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
	SetParams->SetStringField(TEXT("asset_path"), MatPath);
	SetParams->SetStringField(TEXT("property_name"), TEXT("BlendMode"));
	SetParams->SetStringField(TEXT("value"), TEXT("BLEND_Masked"));
	FCortexCommandResult SetResult = Handler.Execute(TEXT("set_material_property"), SetParams);

	TestTrue(TEXT("set_material_property BlendMode should succeed"), SetResult.bSuccess);

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MatPath);
	TestNotNull(TEXT("Should load material"), Material);
	if (Material)
	{
		TestEqual(TEXT("BlendMode should be Masked"),
			static_cast<int32>(Material->BlendMode),
			static_cast<int32>(BLEND_Masked));
		Material->MarkAsGarbage();
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialSetPropertyInvalidTest,
	"Cortex.Material.Asset.SetMaterialProperty.InvalidProperty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialSetPropertyInvalidTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString MatName = FString::Printf(TEXT("M_TestSetInvalid_%s"), *Suffix);
	const FString MatDir = FString::Printf(TEXT("/Game/Temp/CortexMatTest_SetInvalid_%s"), *Suffix);
	const FString MatPath = FString::Printf(TEXT("%s/%s"), *MatDir, *MatName);

	FCortexMaterialCommandHandler Handler;

	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), MatDir);
	CreateParams->SetStringField(TEXT("name"), MatName);
	Handler.Execute(TEXT("create_material"), CreateParams);

	TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
	SetParams->SetStringField(TEXT("asset_path"), MatPath);
	SetParams->SetStringField(TEXT("property_name"), TEXT("NonExistentProperty"));
	SetParams->SetStringField(TEXT("value"), TEXT("SomeValue"));
	FCortexCommandResult SetResult = Handler.Execute(TEXT("set_material_property"), SetParams);

	TestFalse(TEXT("Should fail for invalid property"), SetResult.bSuccess);
	TestEqual(TEXT("Error code should be InvalidField"),
		SetResult.ErrorCode, CortexErrorCodes::InvalidField);

	UObject* LoadedAsset = LoadObject<UMaterial>(nullptr, *MatPath);
	if (LoadedAsset)
	{
		LoadedAsset->MarkAsGarbage();
	}

	return true;
}
