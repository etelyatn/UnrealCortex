// Direct test for FCortexBPToolbarExtension::IsBlueprintInterfaceForPayload.
// Fixture: a transient Blueprint Interface hosted under a unique, registered, non-/Game mount.

#include "Misc/AutomationTest.h"
#include "CortexBPToolbarExtension.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Interface.h"

namespace CortexBPToolbarClassificationTest
{
	const TCHAR* const InterfaceRoot = TEXT("/CortexToolbarInterfaceTest/");

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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPToolbarInterfaceClassificationTest,
	"Cortex.Blueprint.Toolbar.InterfaceClassification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPToolbarInterfaceClassificationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	using namespace CortexBPToolbarClassificationTest;

	const FString Dir = FPaths::ProjectSavedDir() / TEXT("CortexToolbarInterfaceTest");
	FScopedInterfaceMount Mount(InterfaceRoot, Dir);
	if (!TestTrue(TEXT("Interface classification fixture mount must be created"), Mount.IsValid()))
	{
		return false;
	}

	const FString AssetName = TEXT("BPI_CortexToolbarTest");
	UPackage* Package = CreatePackage(*(InterfaceRoot + AssetName));
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

	TestNotNull(TEXT("Blueprint Interface fixture package must be created"), Package);
	TestNotNull(TEXT("Blueprint Interface fixture must be created"), Blueprint);

	if (Blueprint)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);

		UClass* GeneratedClass = Blueprint->GeneratedClass;
		TestNotNull(TEXT("Blueprint Interface generated class must exist"), GeneratedClass);
		if (GeneratedClass)
		{
			TestTrue(TEXT("Non-/Game Blueprint Interface must classify as Blueprint"),
				FCortexBPToolbarExtension::IsBlueprintInterfaceForPayload(GeneratedClass));
		}
	}

	TestFalse(TEXT("Native UInterface must classify as native"),
		FCortexBPToolbarExtension::IsBlueprintInterfaceForPayload(UInterface::StaticClass()));

	// Cleanup is unconditional: this test never returns before reaching it (and the mount above
	// cleans itself up on every exit path, including early failure returns).
	if (Blueprint)
	{
		Blueprint->MarkAsGarbage();
	}
	if (Package)
	{
		Package->MarkAsGarbage();
	}

	return true;
}
