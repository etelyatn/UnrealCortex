#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexTypes.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCreateOverOtherAssetClassTest,
	"Cortex.Blueprint.CreateOverOtherAssetClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPCreateOverOtherAssetClassTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	const FString Dir = FString::Printf(TEXT("/Game/Temp/CortexBPOtherClass_%s"), *Suffix);
	const FString OccupiedName = TEXT("DT_Items");

	UPackage* TablePackage = CreatePackage(*FString::Printf(TEXT("%s/%s"), *Dir, *OccupiedName));
	UDataTable* Table = NewObject<UDataTable>(TablePackage, *OccupiedName, RF_Public | RF_Standalone);
	Table->RowStruct = FTableRowBase::StaticStruct();
	UBlueprint* Source = nullptr;
	ON_SCOPE_EXIT
	{
		if (Source)
		{
			Source->MarkAsGarbage();
		}
		Table->MarkAsGarbage();
		TablePackage->MarkAsGarbage();
	};

	FCortexBPCommandHandler Handler;

	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("name"), OccupiedName);
	CreateParams->SetStringField(TEXT("path"), Dir);
	CreateParams->SetStringField(TEXT("type"), TEXT("Actor"));
	const FCortexCommandResult Create = Handler.Execute(TEXT("create"), CreateParams);
	TestFalse(TEXT("create refuses a name held by a DataTable"), Create.bSuccess);
	TestEqual(TEXT("create refusal is ASSET_ALREADY_EXISTS"), Create.ErrorCode, CortexErrorCodes::AssetAlreadyExists);
	TestTrue(TEXT("The DataTable is untouched after create"),
		FindObject<UDataTable>(TablePackage, *OccupiedName) == Table && IsValid(Table));

	const FString SourceName = FString::Printf(TEXT("BP_Source_%s"), *Suffix);
	TSharedPtr<FJsonObject> SourceParams = MakeShared<FJsonObject>();
	SourceParams->SetStringField(TEXT("name"), SourceName);
	SourceParams->SetStringField(TEXT("path"), Dir);
	SourceParams->SetStringField(TEXT("type"), TEXT("Actor"));
	if (!TestTrue(TEXT("Setup: source Blueprint is created"), Handler.Execute(TEXT("create"), SourceParams).bSuccess))
	{
		return false;
	}
	const FString SourcePath = FString::Printf(TEXT("%s/%s"), *Dir, *SourceName);
	Source = FindObject<UBlueprint>(nullptr, *FString::Printf(TEXT("%s.%s"), *SourcePath, *SourceName));

	TSharedPtr<FJsonObject> DuplicateParams = MakeShared<FJsonObject>();
	DuplicateParams->SetStringField(TEXT("asset_path"), SourcePath);
	DuplicateParams->SetStringField(TEXT("new_name"), OccupiedName);
	DuplicateParams->SetStringField(TEXT("new_path"), Dir);
	const FCortexCommandResult Duplicate = Handler.Execute(TEXT("duplicate"), DuplicateParams);
	TestFalse(TEXT("duplicate refuses a name held by a DataTable"), Duplicate.bSuccess);
	TestEqual(TEXT("duplicate refusal is ASSET_ALREADY_EXISTS"), Duplicate.ErrorCode, CortexErrorCodes::AssetAlreadyExists);
	TestTrue(TEXT("The DataTable is untouched after duplicate"),
		FindObject<UDataTable>(TablePackage, *OccupiedName) == Table && IsValid(Table));

	return true;
}
