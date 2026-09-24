// Direct tests for FCortexBPToolbarExtension::IsBlueprintInterfaceForPayload.
// Fixture: a transient Blueprint Interface hosted under a unique, registered, non-/Game mount.

#include "Misc/AutomationTest.h"
#include "CortexBPToolbarExtension.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Interface.h"

namespace CortexBPToolbarClassificationTest
{
	const TCHAR* const InterfaceRoot = TEXT("/CortexToolbarInterfaceTest/");
	const TCHAR* const InterfaceBackingDirectory = TEXT("CortexToolbarInterfaceTest");
	const TCHAR* const InterfaceAssetName = TEXT("BPI_CortexToolbarTest");

	FString InterfacePackageName()
	{
		return FString(InterfaceRoot) + InterfaceAssetName;
	}

	/** Owns the non-/Game content mount and its backing directory for the fixture's entire scope. */
	struct FScopedInterfaceMount
	{
		FScopedInterfaceMount(const FString& InRoot, const FString& InDirectory)
			: Root(InRoot)
			, Directory(InDirectory)
		{
			IFileManager::Get().MakeDirectory(*Directory, true);
			bRegistered = IFileManager::Get().DirectoryExists(*Directory);
			if (bRegistered)
			{
				FPackageName::RegisterMountPoint(Root, Directory);
			}
		}

		~FScopedInterfaceMount()
		{
			if (bRegistered)
			{
				FPackageName::UnRegisterMountPoint(Root, Directory);
				IFileManager::Get().DeleteDirectory(*Directory, false, true);
			}
		}

		bool IsValid() const { return bRegistered; }

		FString Root;
		FString Directory;

	private:
		bool bRegistered = false;
	};

	/** Creates and releases the fixed-name generated interface so the test is repeatable. */
	void RunInterfaceClassificationFixture(FAutomationTestBase& Test)
	{
		const FString Directory = FPaths::ProjectSavedDir() / InterfaceBackingDirectory;
		FScopedInterfaceMount Mount(InterfaceRoot, Directory);
		if (!Test.TestTrue(TEXT("Interface classification fixture mount must be created"), Mount.IsValid()))
		{
			return;
		}

		const FString AssetName = InterfaceAssetName;
		UPackage* Package = CreatePackage(*InterfacePackageName());
		UBlueprint* Blueprint = Package
			? FKismetEditorUtilities::CreateBlueprint(
				UInterface::StaticClass(),
				Package,
				FName(*AssetName),
				BPTYPE_Interface,
				UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass(),
				NAME_None)
			: nullptr;

		Test.TestNotNull(TEXT("Blueprint Interface fixture package must be created"), Package);
		Test.TestNotNull(TEXT("Blueprint Interface fixture must be created"), Blueprint);

		if (Blueprint)
		{
			FKismetEditorUtilities::CompileBlueprint(Blueprint);

			UClass* GeneratedClass = Blueprint->GeneratedClass;
			Test.TestNotNull(TEXT("Blueprint Interface generated class must exist"), GeneratedClass);
			if (GeneratedClass)
			{
				Test.TestTrue(TEXT("Non-/Game Blueprint Interface must classify as Blueprint"),
					FCortexBPToolbarExtension::IsBlueprintInterfaceForPayload(GeneratedClass));
			}
		}

		Test.TestFalse(TEXT("Native UInterface must classify as native"),
			FCortexBPToolbarExtension::IsBlueprintInterfaceForPayload(UInterface::StaticClass()));

		if (Blueprint)
		{
			Blueprint->MarkAsGarbage();
		}
		if (Package)
		{
			Package->MarkAsGarbage();
		}
		CollectGarbage(RF_NoFlags);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPToolbarInterfaceClassificationTest,
	"Cortex.Blueprint.Toolbar.InterfaceClassification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPToolbarInterfaceClassificationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	CortexBPToolbarClassificationTest::RunInterfaceClassificationFixture(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPToolbarInterfaceNameReuseTest,
	"Cortex.Blueprint.Toolbar.InterfaceNameReuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPToolbarInterfaceNameReuseTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexBPToolbarClassificationTest;

	RunInterfaceClassificationFixture(*this);
	const FString PackageName = InterfacePackageName();
	const FString BlueprintPath = PackageName + TEXT(".") + InterfaceAssetName;
	TestNull(TEXT("Fixture package must be reclaimed before its name is reused"),
		FindObject<UPackage>(nullptr, *PackageName));
	TestNull(TEXT("Fixture Blueprint must be reclaimed before its name is reused"),
		FindObject<UBlueprint>(nullptr, *BlueprintPath));
	RunInterfaceClassificationFixture(*this);
	return true;
}
