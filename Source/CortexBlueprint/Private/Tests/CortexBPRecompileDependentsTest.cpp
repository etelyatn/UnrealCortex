#include "Misc/AutomationTest.h"
#include "Operations/CortexBPCleanupOps.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "Modules/ModuleManager.h"
#include "PackageTools.h"
#include "UObject/SavePackage.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

namespace
{
	UPackage* CreateWritableRecompileTestPackage(const TCHAR* Name)
	{
		return CreatePackage(*FString::Printf(
			TEXT("/Game/Temp/%s_%s"),
			Name,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8)));
	}

	bool IsPackageUnderRoot(const FString& PackageName, const FString& Root)
	{
		return PackageName == Root || PackageName.StartsWith(Root + TEXT("/"));
	}

	struct FScopedReadOnlyMountedRoot
	{
		FString Root;
		FString PhysicalDir;

		FScopedReadOnlyMountedRoot()
		{
			Root = FString::Printf(
				TEXT("/CortexReadOnly%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8));
			PhysicalDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("CortexReadOnlyBlueprintTests") / Root.RightChop(1));
			IFileManager::Get().MakeDirectory(*PhysicalDir, true);
			FPackageName::RegisterMountPoint(Root + TEXT("/"), PhysicalDir / TEXT(""));
		}

		~FScopedReadOnlyMountedRoot()
		{
			for (TObjectIterator<UObject> It; It; ++It)
			{
				UObject* Asset = *It;
				if (!Asset || !Asset->IsAsset())
				{
					continue;
				}

				UPackage* Package = Asset->GetOutermost();
				if (Package && IsPackageUnderRoot(Package->GetName(), Root))
				{
					FAssetRegistryModule::AssetDeleted(Asset);
					Asset->MarkAsGarbage();
				}
			}

			for (TObjectIterator<UPackage> It; It; ++It)
			{
				UPackage* Package = *It;
				if (Package && IsPackageUnderRoot(Package->GetName(), Root))
				{
					Package->SetDirtyFlag(false);
					FAssetRegistryModule::PackageDeleted(Package);
					Package->MarkAsGarbage();
				}
			}
			CollectGarbage(RF_NoFlags);

			IAssetRegistry& AssetRegistry =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			AssetRegistry.WaitForCompletion();
			FlushAsyncLoading();
			FTSTicker::GetCoreTicker().Tick(0.0f);

			FPackageName::UnRegisterMountPoint(Root + TEXT("/"), PhysicalDir / TEXT(""));
			IFileManager::Get().DeleteDirectory(*PhysicalDir, false, true);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRecompileDependentsTest,
	"Cortex.Blueprint.RecompileDependents.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPRecompileDependentsTest::RunTest(const FString& Parameters)
{
	UBlueprint* ParentBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		CreateWritableRecompileTestPackage(TEXT("BP_RecompileParent")),
		FName(TEXT("BP_RecompileParent")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Parent BP created"), ParentBP);
	if (!ParentBP)
	{
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(ParentBP);

	UBlueprint* ChildBP = FKismetEditorUtilities::CreateBlueprint(
		ParentBP->GeneratedClass,
		CreateWritableRecompileTestPackage(TEXT("BP_RecompileChild")),
		FName(TEXT("BP_RecompileChild")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Child BP created"), ChildBP);
	if (!ChildBP)
	{
		ParentBP->MarkAsGarbage();
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(ChildBP);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), ParentBP->GetPathName());
	const FCortexCommandResult Result = FCortexBPCleanupOps::RecompileDependents(Params);
	TestTrue(TEXT("recompile_dependents succeeded"), Result.bSuccess);
	TestTrue(TEXT("Child blueprint up to date"), ChildBP->Status == BS_UpToDate || ChildBP->Status == BS_UpToDateWithWarnings);

	ChildBP->MarkAsGarbage();
	ParentBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRecompileDependentsRejectsNonWritableTargetTest,
	"Cortex.Blueprint.RecompileDependents.RejectsNonWritableTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPRecompileDependentsRejectsNonWritableTargetTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Engine/NotWritable/BP_Target"));

	const FCortexCommandResult Result = FCortexBPCleanupOps::RecompileDependents(Params);
	TestFalse(TEXT("recompile_dependents rejects non-writable target"), Result.bSuccess);
	TestEqual(TEXT("Error code is INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRecompileDependentsRejectsNonWritableDependentTest,
	"Cortex.Blueprint.RecompileDependents.RejectsNonWritableDependent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPRecompileDependentsRejectsNonWritableDependentTest::RunTest(const FString& Parameters)
{
	FScopedReadOnlyMountedRoot ReadOnlyRoot;

	UBlueprint* ParentBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		CreateWritableRecompileTestPackage(TEXT("BP_RecompileParentWritable")),
		FName(TEXT("BP_RecompileParentWritable")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Parent BP created"), ParentBP);
	if (!ParentBP)
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(ParentBP);

	UBlueprint* ChildBP = FKismetEditorUtilities::CreateBlueprint(
		ParentBP->GeneratedClass,
		CreatePackage(*(ReadOnlyRoot.Root / TEXT("BP_RecompileReadOnlyChild"))),
		FName(TEXT("BP_RecompileReadOnlyChild")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Read-only child BP created"), ChildBP);
	if (!ChildBP)
	{
		ParentBP->MarkAsGarbage();
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(ChildBP);
	ChildBP->Status = BS_Dirty;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), ParentBP->GetPathName());
	const FCortexCommandResult Result = FCortexBPCleanupOps::RecompileDependents(Params);

	TestFalse(TEXT("recompile_dependents rejects non-writable dependent"), Result.bSuccess);
	TestEqual(TEXT("Error code is INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	TestEqual(TEXT("Child was not compiled"), ChildBP->Status, EBlueprintStatus::BS_Dirty);

	ParentBP->MarkAsGarbage();
	return true;
}

namespace
{
	FString RecompileFixturePackageFilename(const FString& PackageName)
	{
		return FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	}

	bool RecompileSaveFixture(UBlueprint* Blueprint)
	{
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(
			Blueprint->GetOutermost(), Blueprint,
			*RecompileFixturePackageFilename(Blueprint->GetOutermost()->GetName()), SaveArgs);
	}

	void RecompileFixtureCleanup(const TArray<FString>& PackageNames, const TArray<FString>& Filenames)
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

		IAssetRegistry& AssetRegistry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		AssetRegistry.ScanModifiedAssetFiles(Filenames);
		AssetRegistry.WaitForCompletion();
	}
}

// Dependents saved on disk but not loaded must still be discovered (registry referencers) and
// recompiled — the state after an editor restart (issue #224 item 6).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRecompileDependentsRegistryTest,
	"Cortex.Blueprint.RecompileDependentsRegistry.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPRecompileDependentsRegistryTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);

	UBlueprint* ParentBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		CreateWritableRecompileTestPackage(TEXT("BP_RecompileRegistryParent")),
		FName(*FString::Printf(TEXT("BP_RecompileRegistryParent_%s"), *Suffix)),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("Parent BP created"), ParentBP))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(ParentBP);

	UBlueprint* ChildBP = FKismetEditorUtilities::CreateBlueprint(
		ParentBP->GeneratedClass,
		CreateWritableRecompileTestPackage(TEXT("BP_RecompileRegistryChild")),
		FName(*FString::Printf(TEXT("BP_RecompileRegistryChild_%s"), *Suffix)),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("Child BP created"), ChildBP))
	{
		ParentBP->MarkAsGarbage();
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(ChildBP);

	const FString ParentPath = ParentBP->GetPathName();
	const FString ChildPath = ChildBP->GetPathName();
	const FString ParentPackageName = ParentBP->GetOutermost()->GetName();
	const FString ChildPackageName = ChildBP->GetOutermost()->GetName();
	const TArray<FString> Filenames = {
		RecompileFixturePackageFilename(ParentPackageName),
		RecompileFixturePackageFilename(ChildPackageName)};

	if (!TestTrue(TEXT("Parent fixture saved"), RecompileSaveFixture(ParentBP))
		|| !TestTrue(TEXT("Child fixture saved"), RecompileSaveFixture(ChildBP)))
	{
		RecompileFixtureCleanup({ParentPackageName, ChildPackageName}, Filenames);
		return false;
	}

	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	AssetRegistry.ScanFilesSynchronous(Filenames, true);

	TArray<UPackage*> ToUnload = {ParentBP->GetOutermost(), ChildBP->GetOutermost()};
	FText UnloadError;
	if (!TestTrue(TEXT("Fixture packages unloaded"), UPackageTools::UnloadPackages(ToUnload, UnloadError, true)))
	{
		RecompileFixtureCleanup({ParentPackageName, ChildPackageName}, Filenames);
		return false;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), ParentPath);
	const FCortexCommandResult Result = FCortexBPCleanupOps::RecompileDependents(Params);

	const bool bSucceeded = TestTrue(TEXT("recompile_dependents succeeds with unloaded dependents"), Result.bSuccess)
		&& Result.Data.IsValid();
	if (!bSucceeded)
	{
		RecompileFixtureCleanup({ParentPackageName, ChildPackageName}, Filenames);
		return false;
	}

	double DependentCount = -1.0;
	double RecompiledCount = -1.0;
	Result.Data->TryGetNumberField(TEXT("dependent_count"), DependentCount);
	Result.Data->TryGetNumberField(TEXT("recompiled_count"), RecompiledCount);
	TestEqual(TEXT("Unloaded child discovered as a dependent"), static_cast<int32>(DependentCount), 1);
	TestEqual(TEXT("Unloaded child recompiled"), static_cast<int32>(RecompiledCount), 1);

	UBlueprint* ReloadedChild = FindObject<UBlueprint>(nullptr, *ChildPath);
	if (TestNotNull(TEXT("Child loaded for recompile"), ReloadedChild))
	{
		TestTrue(
			TEXT("Child blueprint up to date after recompile"),
			ReloadedChild->Status == BS_UpToDate || ReloadedChild->Status == BS_UpToDateWithWarnings);
	}

	RecompileFixtureCleanup({ParentPackageName, ChildPackageName}, Filenames);
	return true;
}

// A dependent that fails to compile must fail the command (issue #224 item 6): success with an
// all-failed results array is dishonest.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRecompileDependentsFailureHonestTest,
	"Cortex.Blueprint.RecompileDependentsFailure.Honest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPRecompileDependentsFailureHonestTest::RunTest(const FString& Parameters)
{
#if WITH_DEV_AUTOMATION_TESTS
	UBlueprint* ParentBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		CreateWritableRecompileTestPackage(TEXT("BP_RecompileFailParent")),
		FName(TEXT("BP_RecompileFailParent")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("Parent BP created"), ParentBP))
	{
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(ParentBP);

	UBlueprint* ChildBP = FKismetEditorUtilities::CreateBlueprint(
		ParentBP->GeneratedClass,
		CreateWritableRecompileTestPackage(TEXT("BP_RecompileFailChild")),
		FName(TEXT("BP_RecompileFailChild")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("Child BP created"), ChildBP))
	{
		ParentBP->MarkAsGarbage();
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(ChildBP);

	FCortexBPCleanupOps::SetRecompileDependentsPostCompileTestHook(
		[](UBlueprint* HookBP)
		{
			if (HookBP)
			{
				HookBP->Status = BS_Error;
			}
		});

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), ParentBP->GetPathName());
	const FCortexCommandResult Result = FCortexBPCleanupOps::RecompileDependents(Params);
	FCortexBPCleanupOps::SetRecompileDependentsPostCompileTestHook(nullptr);

	TestFalse(TEXT("Failed dependent compile fails the command"), Result.bSuccess);
	TestEqual(TEXT("Error code is CompileFailed"), Result.ErrorCode, CortexErrorCodes::CompileFailed);
	if (Result.ErrorDetails.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
		Result.ErrorDetails->TryGetArrayField(TEXT("results"), ResultsArray);
		TestTrue(TEXT("Failure details carry per-dependent results"), ResultsArray != nullptr && ResultsArray->Num() == 1);
	}

	ChildBP->MarkAsGarbage();
	ParentBP->MarkAsGarbage();
	return true;
#else
	AddInfo(TEXT("WITH_DEV_AUTOMATION_TESTS disabled; skipping dependent compile failure honesty test."));
	return true;
#endif
}
