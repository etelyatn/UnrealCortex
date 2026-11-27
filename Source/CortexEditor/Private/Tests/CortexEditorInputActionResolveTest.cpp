#include "Misc/AutomationTest.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "HAL/ThreadSafeCounter.h"
#include "InputAction.h"
#include "Misc/Guid.h"
#include "Misc/OutputDevice.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Operations/CortexEditorInputOps.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
/** Counts warnings and errors whose text mentions Needle; unrelated engine log lines are ignored. */
class FCortexInputResolveLogCounter : public FOutputDevice
{
public:
	explicit FCortexInputResolveLogCounter(FString InNeedle)
		: Needle(MoveTemp(InNeedle))
	{
		GLog->AddOutputDevice(this);
	}

	virtual ~FCortexInputResolveLogCounter() override
	{
		GLog->RemoveOutputDevice(this);
	}

	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		if (Verbosity <= ELogVerbosity::Warning && FCString::Stristr(Message, *Needle) != nullptr)
		{
			Count.Increment();
		}
	}

	int32 GetCount() const
	{
		return Count.GetValue();
	}

private:
	FString Needle;
	FThreadSafeCounter Count;
};

bool UnloadInputResolveFixture(const FString& PackageName)
{
	UPackage* Package = FindPackage(nullptr, *PackageName);
	if (Package == nullptr)
	{
		return true;
	}
	TArray<UPackage*> ToUnload = { Package };
	FText UnloadError;
	return UPackageTools::UnloadPackages(ToUnload, UnloadError, true);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInputResolvesActionAssetFormsTest,
	"Cortex.Editor.Input.ResolvesActionAssetForms",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInputResolvesActionAssetFormsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	// --- A loaded transient action resolves by full object path and by short name. -----------------
	{
		const FString TransientName = FString::Printf(
			TEXT("CortexResolveFixture_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8));
		UInputAction* Transient = NewObject<UInputAction>(GetTransientPackage(), FName(*TransientName));
		if (!TestNotNull(TEXT("transient fixture created"), Transient))
		{
			return false;
		}

		FString Error;
		TestTrue(TEXT("a transient action resolves by full object path"),
			FCortexEditorInputOps::ResolveInputActionObject(Transient->GetPathName(), Error) == Transient);
		TestTrue(TEXT("a loaded action resolves by short name"),
			FCortexEditorInputOps::ResolveInputActionObject(TransientName, Error) == Transient);
		Transient->MarkAsGarbage();
	}

	// --- A saved asset resolves by package path and by object path after it is unloaded. -----------
	const FString PackageName = TEXT("/Game/Test/Issue276/IA_Issue276Probe");
	const FString AssetName = TEXT("IA_Issue276Probe");
	const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());

	IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();

	UPackage* Package = CreatePackage(*PackageName);
	UInputAction* Asset = NewObject<UInputAction>(Package, FName(*AssetName), RF_Public | RF_Standalone);
	if (!TestNotNull(TEXT("saved fixture created"), Asset))
	{
		return false;
	}
	Asset->ValueType = EInputActionValueType::Axis2D;

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	if (!TestTrue(TEXT("fixture package saved"), UPackage::SavePackage(Package, Asset, *PackageFilename, SaveArgs)))
	{
		Asset->MarkAsGarbage();
		return false;
	}
	Registry.ScanFilesSynchronous({ PackageFilename }, true);

	auto CleanupFixture = [&PackageName, &PackageFilename, &Registry]()
	{
		UnloadInputResolveFixture(PackageName);
		IFileManager::Get().Delete(*PackageFilename, false, true);
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(PackageFilename), false, true);
		Registry.ScanModifiedAssetFiles({ PackageFilename });
	};

	// Resolution forms are exercised while the package is not loaded, so the load path is covered.
	auto UnloadFixture = [this, &PackageName, &ObjectPath]() -> bool
	{
		if (!TestTrue(TEXT("fixture package unloads"), UnloadInputResolveFixture(PackageName)))
		{
			return false;
		}
		return TestNull(TEXT("fixture is no longer loaded"), FindObject<UInputAction>(nullptr, *ObjectPath));
	};

	if (!UnloadFixture())
	{
		CleanupFixture();
		return false;
	}

	FString Error;
	UInputAction* ByPackagePath = FCortexEditorInputOps::ResolveInputActionObject(PackageName, Error);
	if (TestNotNull(TEXT("an unloaded asset resolves by package path"), ByPackagePath))
	{
		TestEqual(TEXT("package path resolves to the asset's object path"), ByPackagePath->GetPathName(), ObjectPath);
		TestEqual(TEXT("the loaded asset keeps its saved value type"),
			static_cast<int32>(ByPackagePath->ValueType), static_cast<int32>(EInputActionValueType::Axis2D));
		TestTrue(TEXT("a successful resolve leaves no error text"), Error.IsEmpty());
	}

	if (!UnloadFixture())
	{
		CleanupFixture();
		return false;
	}

	UInputAction* ByObjectPath = FCortexEditorInputOps::ResolveInputActionObject(ObjectPath, Error);
	if (TestNotNull(TEXT("an unloaded asset resolves by full object path"), ByObjectPath))
	{
		TestEqual(TEXT("object path resolves to the same asset"), ByObjectPath->GetPathName(), ObjectPath);
	}

	// Once loaded, the package path also resolves without touching the disk again.
	TestTrue(TEXT("a loaded asset still resolves by package path"),
		FCortexEditorInputOps::ResolveInputActionObject(PackageName, Error) == ByObjectPath);

	if (IsValid(ByObjectPath))
	{
		ByObjectPath->MarkAsGarbage();
	}
	CleanupFixture();

	// --- A nonexistent asset returns null, names the accepted forms and logs nothing. ---------------
	{
		FCortexInputResolveLogCounter Counter(TEXT("IA_CortexIssue276Missing"));
		FString MissingError;

		TestNull(TEXT("a nonexistent package path resolves to null"),
			FCortexEditorInputOps::ResolveInputActionObject(TEXT("/Game/Nope/IA_CortexIssue276Missing"), MissingError));
		TestTrue(TEXT("the error lists the accepted package-path form"), MissingError.Contains(TEXT("package path")));
		TestTrue(TEXT("the error lists the accepted object-path form"), MissingError.Contains(TEXT("object path")));
		TestTrue(TEXT("the error names the requested action"), MissingError.Contains(TEXT("IA_CortexIssue276Missing")));

		TestNull(TEXT("a nonexistent object path resolves to null"),
			FCortexEditorInputOps::ResolveInputActionObject(TEXT("/Game/Nope/IA_CortexIssue276Missing.IA_CortexIssue276Missing"), MissingError));
		TestNull(TEXT("an unknown short name resolves to null"),
			FCortexEditorInputOps::ResolveInputActionObject(TEXT("IA_CortexIssue276Missing"), MissingError));

		TestEqual(TEXT("resolving a nonexistent action emits no warning or error"), Counter.GetCount(), 0);
	}

	return true;
}
