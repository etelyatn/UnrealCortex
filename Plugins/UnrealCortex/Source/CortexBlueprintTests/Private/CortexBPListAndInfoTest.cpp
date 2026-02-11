// Copyright Andrei Sudarikov. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "CortexBPCommandHandler.h"
#include "CortexTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPListTest,
	"Cortex.Blueprint.List.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPListTest::RunTest(const FString& Parameters)
{
	// Setup - Create test Blueprint first
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("name"), TEXT("BP_TestList"));
	CreateParams->SetStringField(TEXT("path"), TEXT("/Game/Temp/CortexBPTest_List"));
	CreateParams->SetStringField(TEXT("type"), TEXT("Actor"));

	FCortexCommandResult CreateResult = Handler.Execute(TEXT("create"), CreateParams);
	TestTrue(TEXT("Blueprint creation should succeed"), CreateResult.bSuccess);

	// Execute list command
	TSharedPtr<FJsonObject> ListParams = MakeShared<FJsonObject>();
	FCortexCommandResult ListResult = Handler.Execute(TEXT("list"), ListParams);

	// Verify
	TestTrue(TEXT("List command should succeed"), ListResult.bSuccess);
	TestTrue(TEXT("Result data should be valid"), ListResult.Data.IsValid());

	if (ListResult.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* BlueprintsArray;
		if (ListResult.Data->TryGetArrayField(TEXT("blueprints"), BlueprintsArray))
		{
			TestTrue(TEXT("Blueprints array should not be empty"), BlueprintsArray->Num() > 0);

			// Find our test Blueprint
			bool bFoundTestBP = false;
			for (const TSharedPtr<FJsonValue>& BPValue : *BlueprintsArray)
			{
				const TSharedPtr<FJsonObject>& BPObj = BPValue->AsObject();
				FString AssetPath;
				if (BPObj->TryGetStringField(TEXT("asset_path"), AssetPath))
				{
					if (AssetPath.Contains(TEXT("BP_TestList")))
					{
						bFoundTestBP = true;

						// Verify required fields
						FString Name;
						BPObj->TryGetStringField(TEXT("name"), Name);
						TestEqual(TEXT("Name should match"), Name, TEXT("BP_TestList"));

						FString Type;
						BPObj->TryGetStringField(TEXT("type"), Type);
						TestEqual(TEXT("Type should be Actor"), Type, TEXT("Actor"));

						break;
					}
				}
			}

			TestTrue(TEXT("Should find test Blueprint in list"), bFoundTestBP);
		}
	}

	// Cleanup
	UObject* LoadedAsset = LoadObject<UBlueprint>(nullptr, TEXT("/Game/Temp/CortexBPTest_List/BP_TestList"));
	if (LoadedAsset)
	{
		LoadedAsset->MarkAsGarbage();
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPListFilterPathTest,
	"Cortex.Blueprint.List.FilterPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPListFilterPathTest::RunTest(const FString& Parameters)
{
	// Setup - Create test Blueprint in specific path
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("name"), TEXT("BP_TestListFilter"));
	CreateParams->SetStringField(TEXT("path"), TEXT("/Game/Temp/CortexBPTest_ListFilter"));
	CreateParams->SetStringField(TEXT("type"), TEXT("Actor"));

	FCortexCommandResult CreateResult = Handler.Execute(TEXT("create"), CreateParams);
	TestTrue(TEXT("Blueprint creation should succeed"), CreateResult.bSuccess);

	// Execute list with path filter
	TSharedPtr<FJsonObject> ListParams = MakeShared<FJsonObject>();
	ListParams->SetStringField(TEXT("path"), TEXT("/Game/Temp/CortexBPTest_ListFilter"));
	FCortexCommandResult ListResult = Handler.Execute(TEXT("list"), ListParams);

	// Verify
	TestTrue(TEXT("List command should succeed"), ListResult.bSuccess);

	if (ListResult.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* BlueprintsArray;
		if (ListResult.Data->TryGetArrayField(TEXT("blueprints"), BlueprintsArray))
		{
			// All returned Blueprints should be in the filter path
			for (const TSharedPtr<FJsonValue>& BPValue : *BlueprintsArray)
			{
				const TSharedPtr<FJsonObject>& BPObj = BPValue->AsObject();
				FString AssetPath;
				BPObj->TryGetStringField(TEXT("asset_path"), AssetPath);
				TestTrue(TEXT("Asset path should match filter"), AssetPath.StartsWith(TEXT("/Game/Temp/CortexBPTest_ListFilter")));
			}
		}
	}

	// Cleanup
	UObject* LoadedAsset = LoadObject<UBlueprint>(nullptr, TEXT("/Game/Temp/CortexBPTest_ListFilter/BP_TestListFilter"));
	if (LoadedAsset)
	{
		LoadedAsset->MarkAsGarbage();
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGetInfoTest,
	"Cortex.Blueprint.GetInfo.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPGetInfoTest::RunTest(const FString& Parameters)
{
	// Use pre-built BP_ComplexActor (has variables, functions, graphs)
	FCortexBPCommandHandler Handler;

	TSharedPtr<FJsonObject> InfoParams = MakeShared<FJsonObject>();
	InfoParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Blueprints/BP_ComplexActor"));
	FCortexCommandResult InfoResult = Handler.Execute(TEXT("get_info"), InfoParams);

	// Verify
	TestTrue(TEXT("GetInfo command should succeed"), InfoResult.bSuccess);
	TestTrue(TEXT("Result data should be valid"), InfoResult.Data.IsValid());

	if (InfoResult.Data.IsValid())
	{
		// Verify basic fields
		FString Name;
		InfoResult.Data->TryGetStringField(TEXT("name"), Name);
		TestEqual(TEXT("Name should match"), Name, TEXT("BP_ComplexActor"));

		FString AssetPath;
		InfoResult.Data->TryGetStringField(TEXT("asset_path"), AssetPath);
		TestEqual(TEXT("Asset path should match"), AssetPath, TEXT("/Game/Blueprints/BP_ComplexActor"));

		FString Type;
		InfoResult.Data->TryGetStringField(TEXT("type"), Type);
		TestEqual(TEXT("Type should be Actor"), Type, TEXT("Actor"));

		FString ParentClass;
		InfoResult.Data->TryGetStringField(TEXT("parent_class"), ParentClass);
		TestEqual(TEXT("Parent class should be Actor"), ParentClass, TEXT("Actor"));

		// Verify variables array (BP_ComplexActor has Health, DisplayName, bIsActive)
		const TArray<TSharedPtr<FJsonValue>>* VariablesArray;
		if (TestTrue(TEXT("Variables array should exist"), InfoResult.Data->TryGetArrayField(TEXT("variables"), VariablesArray)))
		{
			TestTrue(TEXT("Should have at least 3 variables"), VariablesArray->Num() >= 3);
		}

		// Verify functions array (BP_ComplexActor has CalculateDamage)
		const TArray<TSharedPtr<FJsonValue>>* FunctionsArray;
		if (TestTrue(TEXT("Functions array should exist"), InfoResult.Data->TryGetArrayField(TEXT("functions"), FunctionsArray)))
		{
			TestTrue(TEXT("Should have at least 1 function"), FunctionsArray->Num() >= 1);
		}

		// Verify graphs array
		const TArray<TSharedPtr<FJsonValue>>* GraphsArray;
		if (TestTrue(TEXT("Graphs array should exist"), InfoResult.Data->TryGetArrayField(TEXT("graphs"), GraphsArray)))
		{
			TestTrue(TEXT("Should have at least one graph"), GraphsArray->Num() > 0);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPGetInfoMissingAssetTest,
	"Cortex.Blueprint.GetInfo.MissingAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPGetInfoMissingAssetTest::RunTest(const FString& Parameters)
{
	// Setup
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistent/BP_DoesNotExist"));

	// Execute
	FCortexCommandResult Result = Handler.Execute(TEXT("get_info"), Params);

	// Verify
	TestFalse(TEXT("Command should fail"), Result.bSuccess);
	TestEqual(TEXT("Should return BLUEPRINT_NOT_FOUND"), Result.ErrorCode, CortexErrorCodes::BlueprintNotFound);

	return true;
}
