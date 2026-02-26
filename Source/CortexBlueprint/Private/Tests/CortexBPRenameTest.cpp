#include "Misc/AutomationTest.h"
#include "Operations/CortexBPAssetOps.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRenameBasicTest,
	"Cortex.Blueprint.Rename.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPRenameBasicTest::RunTest(const FString& Parameters)
{
	const FString SourcePath = TEXT("/Game/Temp/CortexBPTest_Rename/BP_Rename_Source");
	const FString DestPath = TEXT("/Game/Temp/CortexBPTest_Rename/BP_Rename_Dest");

	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("name"), TEXT("BP_Rename_Source"));
	CreateParams->SetStringField(TEXT("path"), TEXT("/Game/Temp/CortexBPTest_Rename"));
	CreateParams->SetStringField(TEXT("type"), TEXT("Actor"));
	const FCortexCommandResult CreateResult = FCortexBPAssetOps::Create(CreateParams);
	TestTrue(TEXT("Source BP created"), CreateResult.bSuccess);

	TSharedPtr<FJsonObject> RenameParams = MakeShared<FJsonObject>();
	RenameParams->SetStringField(TEXT("source_path"), SourcePath);
	RenameParams->SetStringField(TEXT("dest_path"), DestPath);
	const FCortexCommandResult RenameResult = FCortexBPAssetOps::Rename(RenameParams);
	TestTrue(TEXT("Rename succeeded"), RenameResult.bSuccess);

	FString LoadError;
	UBlueprint* RenamedBP = FCortexBPAssetOps::LoadBlueprint(DestPath, LoadError);
	TestNotNull(TEXT("Blueprint loadable at new path"), RenamedBP);

	TSharedPtr<FJsonObject> DeleteParams = MakeShared<FJsonObject>();
	DeleteParams->SetStringField(TEXT("asset_path"), DestPath);
	FCortexBPAssetOps::Delete(DeleteParams);

	return true;
}
