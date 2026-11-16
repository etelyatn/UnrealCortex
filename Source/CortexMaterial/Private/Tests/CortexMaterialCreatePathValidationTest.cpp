#include "Misc/AutomationTest.h"
#include "CortexMaterialCommandHandler.h"
#include "CortexTypes.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Materials/MaterialParameterCollection.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialCreatePathValidationTest,
	"Cortex.Material.CreatePathValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialCreatePathValidationTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	const FString Dir = FString::Printf(TEXT("/Game/Temp/CortexMatPathTest_%s"), *Suffix);
	FCortexMaterialCommandHandler Handler;

	// A trailing slash on the directory is normalized, as create_material and create_instance do.
	const FString CollName = FString::Printf(TEXT("MPC_Slash_%s"), *Suffix);
	TSharedPtr<FJsonObject> SlashParams = MakeShared<FJsonObject>();
	SlashParams->SetStringField(TEXT("asset_path"), Dir + TEXT("/"));
	SlashParams->SetStringField(TEXT("name"), CollName);
	const FCortexCommandResult SlashResult = Handler.Execute(TEXT("create_collection"), SlashParams);
	TestTrue(TEXT("create_collection accepts a trailing slash"), SlashResult.bSuccess);
	FString ReturnedPath;
	if (SlashResult.Data.IsValid())
	{
		SlashResult.Data->TryGetStringField(TEXT("asset_path"), ReturnedPath);
	}
	TestEqual(TEXT("Returned path has no double slash"), ReturnedPath, Dir + TEXT("/") + CollName);
	UMaterialParameterCollection* Created = FindObject<UMaterialParameterCollection>(
		nullptr, *FString::Printf(TEXT("%s/%s.%s"), *Dir, *CollName, *CollName));
	TestNotNull(TEXT("Collection exists at the normalized path"), Created);
	if (Created)
	{
		Created->MarkAsGarbage();
	}

	// A path without a mount root cannot be mapped to a file.
	TSharedPtr<FJsonObject> InstanceParams = MakeShared<FJsonObject>();
	InstanceParams->SetStringField(TEXT("asset_path"), TEXT("Game/Mats"));
	InstanceParams->SetStringField(TEXT("name"), TEXT("MI_NoRoot"));
	InstanceParams->SetStringField(TEXT("parent_material"), TEXT("/Engine/EngineMaterials/WorldGridMaterial"));
	const FCortexCommandResult NoRoot = Handler.Execute(TEXT("create_instance"), InstanceParams);
	TestFalse(TEXT("create_instance refuses a path without a mount root"), NoRoot.bSuccess);
	TestEqual(TEXT("No-root refusal is INVALID_FIELD"), NoRoot.ErrorCode, CortexErrorCodes::InvalidField);
	TestNull(TEXT("No package is created for the no-root path"), FindPackage(nullptr, TEXT("Game/Mats/MI_NoRoot")));

	// Invalid object name characters are refused instead of being split into package and object.
	TSharedPtr<FJsonObject> DotParams = MakeShared<FJsonObject>();
	DotParams->SetStringField(TEXT("asset_path"), Dir);
	DotParams->SetStringField(TEXT("name"), TEXT("MPC_Bad.Name"));
	const FCortexCommandResult Dot = Handler.Execute(TEXT("create_collection"), DotParams);
	TestFalse(TEXT("create_collection refuses a name with '.'"), Dot.bSuccess);
	TestEqual(TEXT("Invalid-name refusal is INVALID_FIELD"), Dot.ErrorCode, CortexErrorCodes::InvalidField);
	TestNull(TEXT("No package is created for the invalid name"),
		FindPackage(nullptr, *FString::Printf(TEXT("%s/MPC_Bad"), *Dir)));

	// Engine content is not a writable root.
	const FString EnginePackage = FString::Printf(TEXT("/Engine/CortexMatPathTest_%s/MPC_Engine"), *Suffix);
	TSharedPtr<FJsonObject> EngineParams = MakeShared<FJsonObject>();
	EngineParams->SetStringField(TEXT("asset_path"), FPackageName::GetLongPackagePath(EnginePackage));
	EngineParams->SetStringField(TEXT("name"), TEXT("MPC_Engine"));
	const FCortexCommandResult Engine = Handler.Execute(TEXT("create_collection"), EngineParams);
	TestFalse(TEXT("create_collection refuses /Engine"), Engine.bSuccess);
	TestEqual(TEXT("Engine refusal is INVALID_FIELD"), Engine.ErrorCode, CortexErrorCodes::InvalidField);
	TestNull(TEXT("No package is created under /Engine"), FindPackage(nullptr, *EnginePackage));

	return true;
}
