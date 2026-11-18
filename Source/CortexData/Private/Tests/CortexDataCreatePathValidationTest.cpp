#include "Misc/AutomationTest.h"
#include "CortexDataCommandHandler.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Tests/CortexTestDataAsset.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataCreatePathValidationTest,
	"Cortex.Data.CreatePathValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataCreatePathValidationTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	FCortexDataCommandHandler Handler;

	const auto CreateDataAsset = [&Handler](const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("class_name"), TEXT("CortexTestDataAsset"));
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		return Handler.Execute(TEXT("create_data_asset"), Params);
	};
	const auto CreateDataTable = [&Handler](const FString& TablePath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("table_path"), TablePath);
		Params->SetStringField(TEXT("row_struct"), TEXT("TableRowBase"));
		return Handler.Execute(TEXT("create_datatable"), Params);
	};

	// A bare name has no mount root and cannot be mapped to a file.
	const FString BareName = FString::Printf(TEXT("DA_NoRoot_%s"), *Suffix);
	const FCortexCommandResult BareAsset = CreateDataAsset(BareName);
	TestFalse(TEXT("create_data_asset refuses a path without a mount root"), BareAsset.bSuccess);
	TestEqual(TEXT("No-root DataAsset refusal is INVALID_FIELD"), BareAsset.ErrorCode, CortexErrorCodes::InvalidField);
	TestNull(TEXT("No package is created for the bare name"), FindPackage(nullptr, *BareName));

	const FString RelativeTable = FString::Printf(TEXT("Game/Temp/DT_NoRoot_%s"), *Suffix);
	const FCortexCommandResult RelativeResult = CreateDataTable(RelativeTable);
	TestFalse(TEXT("create_datatable refuses a path without a mount root"), RelativeResult.bSuccess);
	TestEqual(TEXT("No-root DataTable refusal is INVALID_FIELD"), RelativeResult.ErrorCode, CortexErrorCodes::InvalidField);
	TestNull(TEXT("No package is created for the relative path"), FindPackage(nullptr, *RelativeTable));

	// A double slash is not a valid long package name.
	const FString DoubleSlash = FString::Printf(TEXT("/Game/Temp//DA_DoubleSlash_%s"), *Suffix);
	const FCortexCommandResult DoubleSlashResult = CreateDataAsset(DoubleSlash);
	TestFalse(TEXT("create_data_asset refuses a double slash"), DoubleSlashResult.bSuccess);
	TestEqual(TEXT("Double-slash refusal is INVALID_FIELD"), DoubleSlashResult.ErrorCode, CortexErrorCodes::InvalidField);

	// Engine content is not a writable root.
	const FString EngineAsset = FString::Printf(TEXT("/Engine/CortexDataPathTest_%s/DA_Engine"), *Suffix);
	const FCortexCommandResult EngineAssetResult = CreateDataAsset(EngineAsset);
	TestFalse(TEXT("create_data_asset refuses /Engine"), EngineAssetResult.bSuccess);
	TestEqual(TEXT("Engine DataAsset refusal is INVALID_FIELD"), EngineAssetResult.ErrorCode, CortexErrorCodes::InvalidField);
	TestNull(TEXT("No DataAsset package is created under /Engine"), FindPackage(nullptr, *EngineAsset));

	const FString EngineTable = FString::Printf(TEXT("/Engine/CortexDataPathTest_%s/DT_Engine"), *Suffix);
	const FCortexCommandResult EngineTableResult = CreateDataTable(EngineTable);
	TestFalse(TEXT("create_datatable refuses /Engine"), EngineTableResult.bSuccess);
	TestEqual(TEXT("Engine DataTable refusal is INVALID_FIELD"), EngineTableResult.ErrorCode, CortexErrorCodes::InvalidField);
	TestNull(TEXT("No DataTable package is created under /Engine"), FindPackage(nullptr, *EngineTable));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataCreateSaveFailureTest,
	"Cortex.Data.Create.SaveFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataCreateSaveFailureTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	FCortexDataCommandHandler Handler;
	IFileManager& FileManager = IFileManager::Get();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	TArray<FString> Blockers;

	// A read-only directory at the package filename is not a package, so the name stays free, but
	// SavePackage refuses to replace it. The engine logs one error and one warning per refusal; that
	// refusal is the condition under test.
	AddExpectedError(TEXT("as it is read only"), EAutomationExpectedErrorFlags::Contains, 2);
	AddExpectedMessage(TEXT("Error saving"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);

	const auto BlockPackage = [this, &FileManager, &PlatformFile, &Blockers](const FString& PackagePath)
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackagePath, FPackageName::GetAssetPackageExtension());
		TestTrue(TEXT("Blocking directory created"), FileManager.MakeDirectory(*Filename, true));
		TestTrue(TEXT("Blocking directory made read-only"), PlatformFile.SetReadOnly(*Filename, true));
		Blockers.Add(Filename);
	};

	// create_datatable: a refused package save is a save failure.
	const FString TablePackagePath = FString::Printf(TEXT("/Game/Temp/DT_SaveFailure_%s"), *Suffix);
	BlockPackage(TablePackagePath);
	TSharedPtr<FJsonObject> TableParams = MakeShared<FJsonObject>();
	TableParams->SetStringField(TEXT("table_path"), TablePackagePath);
	TableParams->SetStringField(TEXT("row_struct"), TEXT("TableRowBase"));
	const FCortexCommandResult TableResult = Handler.Execute(TEXT("create_datatable"), TableParams);
	TestFalse(TEXT("create_datatable reports the refused save"), TableResult.bSuccess);
	TestEqual(TEXT("create_datatable uses SAVE_FAILED"), TableResult.ErrorCode, CortexErrorCodes::SaveFailed);
	TestFalse(TEXT("create_datatable leaves no package on disk"), FPackageName::DoesPackageExist(TablePackagePath));

	// create_data_asset: same condition, same code.
	const FString AssetPackagePath = FString::Printf(TEXT("/Game/Temp/DA_SaveFailure_%s"), *Suffix);
	BlockPackage(AssetPackagePath);
	TSharedPtr<FJsonObject> AssetParams = MakeShared<FJsonObject>();
	AssetParams->SetStringField(TEXT("class_name"), TEXT("CortexTestDataAsset"));
	AssetParams->SetStringField(TEXT("asset_path"), AssetPackagePath);
	const FCortexCommandResult AssetResult = Handler.Execute(TEXT("create_data_asset"), AssetParams);
	TestFalse(TEXT("create_data_asset reports the refused save"), AssetResult.bSuccess);
	TestEqual(TEXT("create_data_asset uses SAVE_FAILED"), AssetResult.ErrorCode, CortexErrorCodes::SaveFailed);
	TestFalse(TEXT("create_data_asset leaves no package on disk"), FPackageName::DoesPackageExist(AssetPackagePath));

	// Cleanup: the refused creates leave a transient object behind and a temp file in Saved/.
	for (const FString& PackagePath : { TablePackagePath, AssetPackagePath })
	{
		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *FPackageName::GetShortName(PackagePath));
		if (UObject* Asset = FindObject<UObject>(nullptr, *ObjectPath))
		{
			UPackage* Package = Asset->GetPackage();
			Asset->ClearFlags(RF_Public | RF_Standalone);
			Asset->MarkAsGarbage();
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
		}
	}
	for (const FString& Blocker : Blockers)
	{
		PlatformFile.SetReadOnly(*Blocker, false);
		FileManager.DeleteDirectory(*Blocker, false, true);
	}
	TArray<FString> TempFiles;
	FileManager.FindFiles(TempFiles, *(FPaths::ProjectSavedDir() / TEXT("*.tmp")), true, false);
	for (const FString& TempFile : TempFiles)
	{
		if (TempFile.Contains(TEXT("SaveFailure")))
		{
			FileManager.Delete(*(FPaths::ProjectSavedDir() / TempFile));
		}
	}

	return true;
}
