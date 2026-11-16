#include "Misc/AutomationTest.h"
#include "CortexDataCommandHandler.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
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
