#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "CortexMaterialCommandHandler.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Operations/CortexMaterialAssetOps.h"
#include "PackageTools.h"
#include "UObject/Package.h"

namespace
{
	FString NoLoadPackageFilename(const FString& PackageName)
	{
		return FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	}

	/** Unload any fixture packages the command under test reloaded, delete the saved files, and
	 *  refresh registry state. Package names only: the original UPackage pointers may be stale
	 *  by the time cleanup runs. */
	void NoLoadCleanup(const TArray<FString>& PackageNames, const TArray<FString>& Filenames)
	{
		TArray<UPackage*> Loaded;
		for (const FString& PackageName : PackageNames)
		{
			if (UPackage* Found = FindPackage(nullptr, *PackageName))
			{
				Loaded.Add(Found);
			}
		}
		if (Loaded.Num() > 0)
		{
			FText UnloadError;
			UPackageTools::UnloadPackages(Loaded, UnloadError, true);
		}

		for (const FString& Filename : Filenames)
		{
			IFileManager::Get().Delete(*Filename, false, true);
		}

		IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
		Registry.ScanModifiedAssetFiles(Filenames);
		Registry.WaitForCompletion();
	}
}

// get_material must count material instances from registry referencer data without synchronously
// loading every UMaterialInstanceConstant in the project (issue #224 item 1).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialInstanceCountNoLoadTest,
	"Cortex.Material.InstancesNoLoad.GetMaterialCount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialInstanceCountNoLoadTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	const FString MatName = FString::Printf(TEXT("M_NoLoad_%s"), *Suffix);
	const FString InstanceName = FString::Printf(TEXT("MI_NoLoad_%s"), *Suffix);
	const FString MatDir = FString::Printf(TEXT("/Game/Temp/CortexMatNoLoad_%s"), *Suffix);
	const FString MatPackageName = FString::Printf(TEXT("%s/%s"), *MatDir, *MatName);
	const FString InstancePackageName = FString::Printf(TEXT("%s/%s"), *MatDir, *InstanceName);
	const FString MatPath = FString::Printf(TEXT("%s.%s"), *MatPackageName, *MatName);

	// Create the material through the production command so the fixture is a real saved material.
	FCortexMaterialCommandHandler Handler;
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), MatDir);
	CreateParams->SetStringField(TEXT("name"), MatName);
	const FCortexCommandResult CreateResult = Handler.Execute(TEXT("create_material"), CreateParams);
	if (!TestTrue(TEXT("create_material should succeed"), CreateResult.bSuccess))
	{
		return false;
	}

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MatPath);
	if (!TestNotNull(TEXT("Material fixture loaded"), Material))
	{
		return false;
	}

	UPackage* InstancePackage = CreatePackage(*InstancePackageName);
	UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(
		InstancePackage, FName(*InstanceName), RF_Public | RF_Standalone);
	Instance->Parent = Material;

	const FString InstanceFilename = NoLoadPackageFilename(InstancePackageName);
	FCortexCommandResult SaveError;
	if (!TestTrue(
		TEXT("Instance fixture saved"),
		FCortexMaterialAssetOps::SaveNewAsset(Instance, InstanceFilename, SaveError)))
	{
		Instance->MarkAsGarbage();
		Material->MarkAsGarbage();
		return false;
	}

	const FString MatFilename = NoLoadPackageFilename(MatPackageName);
	IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
	Registry.ScanFilesSynchronous({MatFilename, InstanceFilename}, true);

	TArray<UPackage*> ToUnload = {Material->GetOutermost(), InstancePackage};
	FText UnloadError;
	if (!TestTrue(TEXT("Fixture packages unloaded"), UPackageTools::UnloadPackages(ToUnload, UnloadError, true)))
	{
		NoLoadCleanup({MatPackageName, InstancePackageName}, {MatFilename, InstanceFilename});
		return false;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), MatPath);
	const FCortexCommandResult Result = FCortexMaterialAssetOps::GetMaterial(Params);

	const bool bSucceeded = TestTrue(TEXT("get_material should succeed"), Result.bSuccess)
		&& Result.Data.IsValid();
	if (!bSucceeded)
	{
		NoLoadCleanup({MatPackageName, InstancePackageName}, {MatFilename, InstanceFilename});
		return false;
	}

	double InstanceCount = -1.0;
	Result.Data->TryGetNumberField(TEXT("instance_count"), InstanceCount);
	TestEqual(TEXT("instance_count counts the saved instance without loading it"), static_cast<int32>(InstanceCount), 1);

	// A material with no instances must report zero, not every instance in the project:
	// an empty referencer set feeds an exact-package filter, which must not degrade to "all".
	const FString NoInstSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	const FString NoInstMatName = FString::Printf(TEXT("M_NoInst_%s"), *NoInstSuffix);
	const FString NoInstMatDir = FString::Printf(TEXT("/Game/Temp/CortexMatNoLoad_%s"), *NoInstSuffix);
	const FString NoInstMatPath = FString::Printf(TEXT("%s/%s.%s"), *NoInstMatDir, *NoInstMatName, *NoInstMatName);
	TSharedPtr<FJsonObject> NoInstCreateParams = MakeShared<FJsonObject>();
	NoInstCreateParams->SetStringField(TEXT("asset_path"), NoInstMatDir);
	NoInstCreateParams->SetStringField(TEXT("name"), NoInstMatName);
	const FCortexCommandResult NoInstCreate = Handler.Execute(TEXT("create_material"), NoInstCreateParams);
	UObject* NoInstMaterial = nullptr;
	if (TestTrue(TEXT("instance-less material created"), NoInstCreate.bSuccess))
	{
		NoInstMaterial = LoadObject<UMaterial>(nullptr, *NoInstMatPath);
		TSharedPtr<FJsonObject> NoInstParams = MakeShared<FJsonObject>();
		NoInstParams->SetStringField(TEXT("asset_path"), NoInstMatPath);
		const FCortexCommandResult NoInstResult = FCortexMaterialAssetOps::GetMaterial(NoInstParams);
		if (TestTrue(TEXT("get_material on instance-less material succeeds"), NoInstResult.bSuccess)
			&& NoInstResult.Data.IsValid())
		{
			double NoInstCount = -1.0;
			NoInstResult.Data->TryGetNumberField(TEXT("instance_count"), NoInstCount);
			TestEqual(TEXT("instance_count is zero for an instance-less material"), static_cast<int32>(NoInstCount), 0);
		}
	}
	if (NoInstMaterial != nullptr)
	{
		NoInstMaterial->MarkAsGarbage();
	}

	TestNull(TEXT("Instance package stays unloaded"), FindPackage(nullptr, *InstancePackageName));
	TestNull(
		TEXT("Instance object stays unloaded"),
		FindObject<UMaterialInstanceConstant>(nullptr, *FString::Printf(TEXT("%s.%s"), *InstancePackageName, *InstanceName)));

	const FString NoInstPackageName = FString::Printf(TEXT("%s/%s"), *NoInstMatDir, *NoInstMatName);
	NoLoadCleanup(
		{MatPackageName, InstancePackageName, NoInstPackageName},
		{MatFilename, InstanceFilename, NoLoadPackageFilename(NoInstPackageName)});
	return true;
}

// list_instances with parent_material must filter from registry referencer data without loading
// each instance (issue #224 item 2). Neither the material nor the instance may be loaded.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialListInstancesParentNoLoadTest,
	"Cortex.Material.InstancesNoLoad.ListInstancesParentFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialListInstancesParentNoLoadTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	const FString MatName = FString::Printf(TEXT("M_ParentNoLoad_%s"), *Suffix);
	const FString InstanceName = FString::Printf(TEXT("MI_ParentNoLoad_%s"), *Suffix);
	const FString MatDir = FString::Printf(TEXT("/Game/Temp/CortexMatNoLoad_%s"), *Suffix);
	const FString MatPackageName = FString::Printf(TEXT("%s/%s"), *MatDir, *MatName);
	const FString InstancePackageName = FString::Printf(TEXT("%s/%s"), *MatDir, *InstanceName);
	const FString MatPath = FString::Printf(TEXT("%s.%s"), *MatPackageName, *MatName);
	const FString InstancePath = FString::Printf(TEXT("%s.%s"), *InstancePackageName, *InstanceName);

	FCortexMaterialCommandHandler Handler;
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), MatDir);
	CreateParams->SetStringField(TEXT("name"), MatName);
	const FCortexCommandResult CreateResult = Handler.Execute(TEXT("create_material"), CreateParams);
	if (!TestTrue(TEXT("create_material should succeed"), CreateResult.bSuccess))
	{
		return false;
	}

	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MatPath);
	if (!TestNotNull(TEXT("Material fixture loaded"), Material))
	{
		return false;
	}

	UPackage* InstancePackage = CreatePackage(*InstancePackageName);
	UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(
		InstancePackage, FName(*InstanceName), RF_Public | RF_Standalone);
	Instance->Parent = Material;

	const FString InstanceFilename = NoLoadPackageFilename(InstancePackageName);
	FCortexCommandResult SaveError;
	if (!TestTrue(
		TEXT("Instance fixture saved"),
		FCortexMaterialAssetOps::SaveNewAsset(Instance, InstanceFilename, SaveError)))
	{
		Instance->MarkAsGarbage();
		Material->MarkAsGarbage();
		return false;
	}

	const FString MatFilename = NoLoadPackageFilename(MatPackageName);
	IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
	Registry.ScanFilesSynchronous({MatFilename, InstanceFilename}, true);

	TArray<UPackage*> ToUnload = {Material->GetOutermost(), InstancePackage};
	FText UnloadError;
	if (!TestTrue(TEXT("Fixture packages unloaded"), UPackageTools::UnloadPackages(ToUnload, UnloadError, true)))
	{
		NoLoadCleanup({MatPackageName, InstancePackageName}, {MatFilename, InstanceFilename});
		return false;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("parent_material"), MatPath);
	const FCortexCommandResult Result = FCortexMaterialAssetOps::ListInstances(Params);

	const bool bSucceeded = TestTrue(TEXT("list_instances should succeed"), Result.bSuccess)
		&& Result.Data.IsValid();
	if (!bSucceeded)
	{
		NoLoadCleanup({MatPackageName, InstancePackageName}, {MatFilename, InstanceFilename});
		return false;
	}

	double Count = -1.0;
	Result.Data->TryGetNumberField(TEXT("count"), Count);
	TestEqual(TEXT("list_instances counts the saved instance"), static_cast<int32>(Count), 1);

	bool bFound = false;
	const TArray<TSharedPtr<FJsonValue>>* InstancesArray = nullptr;
	if (Result.Data->TryGetArrayField(TEXT("instances"), InstancesArray) && InstancesArray != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& Entry : *InstancesArray)
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			if (Entry.IsValid() && Entry->TryGetObject(EntryObject) && EntryObject != nullptr)
			{
				FString AssetPath;
				(*EntryObject)->TryGetStringField(TEXT("asset_path"), AssetPath);
				bFound |= AssetPath == InstancePath;
			}
		}
	}
	TestTrue(TEXT("list_instances returns the saved instance"), bFound);

	TestNull(TEXT("Instance package stays unloaded"), FindPackage(nullptr, *InstancePackageName));
	TestNull(TEXT("Material package stays unloaded"), FindPackage(nullptr, *MatPackageName));

	NoLoadCleanup({MatPackageName, InstancePackageName}, {MatFilename, InstanceFilename});
	return true;
}
