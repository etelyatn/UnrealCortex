#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "CortexCoreModule.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"

namespace
{
/** A never-saved, dirty /Game package that is removed again when the guard goes out of scope. */
struct FDirtyTestPackage
{
	FString PackageName;
	UPackage* Package = nullptr;

	FDirtyTestPackage()
	{
		PackageName = FString::Printf(TEXT("/Game/__CortexDirtyPackagesTest_%s"), *FGuid::NewGuid().ToString().Right(8));
		Package = CreatePackage(*PackageName);
		if (Package != nullptr)
		{
			Package->SetDirtyFlag(true);
		}
	}

	~FDirtyTestPackage()
	{
		if (Package != nullptr)
		{
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
		}
	}
};

FCortexCommandRouter& GetRouter()
{
	return FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore")).GetCommandRouter();
}

TSharedPtr<FJsonObject> FindEntry(const TArray<TSharedPtr<FJsonValue>>& Entries, const FString& PackageName)
{
	for (const TSharedPtr<FJsonValue>& Value : Entries)
	{
		const TSharedPtr<FJsonObject>* Entry = nullptr;
		FString Name;
		if (Value.IsValid() && Value->TryGetObject(Entry) && Entry != nullptr
			&& (*Entry)->TryGetStringField(TEXT("package"), Name) && Name == PackageName)
		{
			return *Entry;
		}
	}
	return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCoreDirtyPackagesListedTest,
	"Cortex.Core.DirtyPackages.ListsDirtyPackage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCoreDirtyPackagesListedTest::RunTest(const FString& Parameters)
{
	FDirtyTestPackage Fixture;
	if (!TestNotNull(TEXT("Fixture package"), Fixture.Package))
	{
		return false;
	}

	const FCortexCommandResult Result = GetRouter().Execute(TEXT("core.list_dirty_packages"), MakeShared<FJsonObject>());
	TestTrue(TEXT("list_dirty_packages succeeds"), Result.bSuccess);
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Packages = nullptr;
	if (!TestTrue(TEXT("packages array present"), Result.Data->TryGetArrayField(TEXT("packages"), Packages)) || Packages == nullptr)
	{
		return false;
	}
	TestEqual(TEXT("count matches the packages array"),
		static_cast<int32>(Result.Data->GetNumberField(TEXT("count"))), Packages->Num());

	const TSharedPtr<FJsonObject> Entry = FindEntry(*Packages, Fixture.PackageName);
	if (TestTrue(TEXT("dirty fixture package is listed"), Entry.IsValid()))
	{
		FString File;
		TestTrue(TEXT("entry has a file field"), Entry->TryGetStringField(TEXT("file"), File));
		TestTrue(TEXT("never-saved package has an empty file"), File.IsEmpty());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCoreDirtyPackagesCleanAbsentTest,
	"Cortex.Core.DirtyPackages.CleanPackageAbsent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCoreDirtyPackagesCleanAbsentTest::RunTest(const FString& Parameters)
{
	FDirtyTestPackage Fixture;
	if (!TestNotNull(TEXT("Fixture package"), Fixture.Package))
	{
		return false;
	}

	// Cleaning the dirty flag stands in for a save: the package must leave the listing.
	Fixture.Package->SetDirtyFlag(false);

	const FCortexCommandResult Result = GetRouter().Execute(TEXT("core.list_dirty_packages"), MakeShared<FJsonObject>());
	TestTrue(TEXT("list_dirty_packages succeeds"), Result.bSuccess);
	const TArray<TSharedPtr<FJsonValue>>* Packages = nullptr;
	if (Result.bSuccess && Result.Data.IsValid() && TestTrue(TEXT("packages array present"), Result.Data->TryGetArrayField(TEXT("packages"), Packages)))
	{
		TestFalse(TEXT("clean fixture package is not listed"), FindEntry(*Packages, Fixture.PackageName).IsValid());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCoreDirtyPackagesExcludesTransientTest,
	"Cortex.Core.DirtyPackages.ExcludesTransientAndScript",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCoreDirtyPackagesExcludesTransientTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString().Right(8);
	UPackage* TransientPackage = CreatePackage(*FString::Printf(TEXT("/Game/__CortexDirtyTransient_%s"), *Suffix));
	if (!TestNotNull(TEXT("Transient package"), TransientPackage))
	{
		return false;
	}
	TransientPackage->SetFlags(RF_Transient);
	TransientPackage->SetDirtyFlag(true);

	const FCortexCommandResult Result = GetRouter().Execute(TEXT("core.list_dirty_packages"), MakeShared<FJsonObject>());

	TransientPackage->SetDirtyFlag(false);
	TransientPackage->ClearFlags(RF_Transient);
	TransientPackage->MarkAsGarbage();

	TestTrue(TEXT("list_dirty_packages succeeds"), Result.bSuccess);
	const TArray<TSharedPtr<FJsonValue>>* Packages = nullptr;
	if (Result.bSuccess && Result.Data.IsValid() && TestTrue(TEXT("packages array present"), Result.Data->TryGetArrayField(TEXT("packages"), Packages)))
	{
		TestFalse(TEXT("an RF_Transient package is excluded"), FindEntry(*Packages, TransientPackage->GetName()).IsValid());
		for (const TSharedPtr<FJsonValue>& Value : *Packages)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			FString Name;
			if (Value->TryGetObject(Entry) && Entry != nullptr && (*Entry)->TryGetStringField(TEXT("package"), Name))
			{
				TestFalse(TEXT("no script package is listed"), Name.StartsWith(TEXT("/Script/")));
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCoreDirtyPackagesSavedFileTest,
	"Cortex.Core.DirtyPackages.SavedPackageHasAbsoluteFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCoreDirtyPackagesSavedFileTest::RunTest(const FString& Parameters)
{
	const FString PackageName = TEXT("/Game/Data/DT_TestSimple");
	if (!FPackageName::DoesPackageExist(PackageName))
	{
		AddInfo(TEXT("Skipped: /Game/Data/DT_TestSimple is not present in this project."));
		return true;
	}

	UPackage* Package = LoadPackage(nullptr, *PackageName, LOAD_None);
	if (!TestNotNull(TEXT("Saved package loads"), Package))
	{
		return false;
	}

	const bool bWasDirty = Package->IsDirty();
	Package->SetDirtyFlag(true);
	const FCortexCommandResult Result = GetRouter().Execute(TEXT("core.list_dirty_packages"), MakeShared<FJsonObject>());
	Package->SetDirtyFlag(bWasDirty);

	TestTrue(TEXT("list_dirty_packages succeeds"), Result.bSuccess);
	const TArray<TSharedPtr<FJsonValue>>* Packages = nullptr;
	if (Result.bSuccess && Result.Data.IsValid() && TestTrue(TEXT("packages array present"), Result.Data->TryGetArrayField(TEXT("packages"), Packages)))
	{
		const TSharedPtr<FJsonObject> Entry = FindEntry(*Packages, PackageName);
		if (TestTrue(TEXT("dirty saved package is listed"), Entry.IsValid()))
		{
			FString File;
			TestTrue(TEXT("entry has a file field"), Entry->TryGetStringField(TEXT("file"), File));
			TestFalse(TEXT("file is an absolute path"), FPaths::IsRelative(File));
			TestTrue(TEXT("file names the asset on disk"), File.EndsWith(TEXT("/Content/Data/DT_TestSimple.uasset")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCoreDirtyPackagesListsTempTest,
	"Cortex.Core.DirtyPackages.ListsNeverSavedTempPackage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCoreDirtyPackagesListsTempTest::RunTest(const FString& Parameters)
{
	// A new unsaved level lives in /Temp/Untitled_N; it must be listed (and block a non-forced shutdown).
	UPackage* TempPackage = CreatePackage(*FString::Printf(TEXT("/Temp/__CortexDirtyTemp_%s"), *FGuid::NewGuid().ToString().Right(8)));
	if (!TestNotNull(TEXT("Temp package"), TempPackage))
	{
		return false;
	}
	TempPackage->SetDirtyFlag(true);

	const FCortexCommandResult Result = GetRouter().Execute(TEXT("core.list_dirty_packages"), MakeShared<FJsonObject>());

	TempPackage->SetDirtyFlag(false);
	TempPackage->MarkAsGarbage();

	TestTrue(TEXT("list_dirty_packages succeeds"), Result.bSuccess);
	const TArray<TSharedPtr<FJsonValue>>* Packages = nullptr;
	if (Result.bSuccess && Result.Data.IsValid() && TestTrue(TEXT("packages array present"), Result.Data->TryGetArrayField(TEXT("packages"), Packages)))
	{
		const TSharedPtr<FJsonObject> Entry = FindEntry(*Packages, TempPackage->GetName());
		if (TestTrue(TEXT("a dirty /Temp/ package is listed"), Entry.IsValid()))
		{
			FString File;
			TestTrue(TEXT("entry has a file field"), Entry->TryGetStringField(TEXT("file"), File));
			TestTrue(TEXT("a never-saved package has an empty file"), File.IsEmpty());
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexCoreDirtyPackagesRejectsParamsTest,
	"Cortex.Core.DirtyPackages.RejectsUnknownParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexCoreDirtyPackagesRejectsParamsTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetBoolField(TEXT("include_transient"), true);

	const FCortexCommandResult Result = GetRouter().Execute(TEXT("core.list_dirty_packages"), Params);
	TestFalse(TEXT("undeclared param is refused"), Result.bSuccess);
	TestEqual(TEXT("refusal code"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	return true;
}
