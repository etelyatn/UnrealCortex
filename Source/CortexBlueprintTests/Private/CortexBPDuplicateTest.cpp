#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "GameFramework/Actor.h"
#include "EdGraphSchema_K2.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPDuplicateTest,
	"Cortex.Blueprint.Duplicate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPDuplicateTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;

	// Setup: create a Blueprint with a variable via bp.create
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("name"), TEXT("BP_DupSource"));
		Params->SetStringField(TEXT("path"), TEXT("/Game/Temp/CortexBPTest_Dup"));
		Params->SetStringField(TEXT("type"), TEXT("Actor"));
		Handler.Execute(TEXT("create"), Params);
	}

	// Add a variable to the source Blueprint directly
	UObject* LoadedObj = StaticLoadObject(
		UBlueprint::StaticClass(), nullptr,
		TEXT("/Game/Temp/CortexBPTest_Dup/BP_DupSource"));
	UBlueprint* SourceBP = Cast<UBlueprint>(LoadedObj);
	TestNotNull(TEXT("Source BP should exist"), SourceBP);

	if (SourceBP != nullptr)
	{
		FEdGraphPinType FloatType;
		FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
		FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
		FBlueprintEditorUtils::AddMemberVariable(SourceBP, FName("Health"), FloatType);
	}

	// Test: duplicate
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/CortexBPTest_Dup/BP_DupSource"));
		Params->SetStringField(TEXT("new_name"), TEXT("BP_DupCopy"));

		FCortexCommandResult Result = Handler.Execute(TEXT("duplicate"), Params);
		TestTrue(TEXT("duplicate should succeed"), Result.bSuccess);

		if (Result.Data.IsValid())
		{
			bool bDuplicated = false;
			Result.Data->TryGetBoolField(TEXT("duplicated"), bDuplicated);
			TestTrue(TEXT("duplicated should be true"), bDuplicated);

			FString NewPath;
			Result.Data->TryGetStringField(TEXT("new_asset_path"), NewPath);
			TestEqual(TEXT("New path should match"),
				NewPath, TEXT("/Game/Temp/CortexBPTest_Dup/BP_DupCopy"));
		}
	}

	// Verify the copy exists and has the Health variable
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/CortexBPTest_Dup/BP_DupCopy"));

		FCortexCommandResult Result = Handler.Execute(TEXT("get_info"), Params);
		TestTrue(TEXT("get_info on copy should succeed"), Result.bSuccess);

		if (Result.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* VarsArray = nullptr;
			if (Result.Data->TryGetArrayField(TEXT("variables"), VarsArray) && VarsArray != nullptr)
			{
				bool bFoundHealth = false;
				for (const TSharedPtr<FJsonValue>& VarVal : *VarsArray)
				{
					TSharedPtr<FJsonObject> VarObj = VarVal->AsObject();
					if (VarObj.IsValid())
					{
						FString VarName;
						VarObj->TryGetStringField(TEXT("name"), VarName);
						if (VarName == TEXT("Health"))
						{
							bFoundHealth = true;
							break;
						}
					}
				}
				TestTrue(TEXT("Copy should have Health variable"), bFoundHealth);
			}
		}
	}

	// Cleanup
	UObject* CreatedSource = LoadObject<UBlueprint>(nullptr, TEXT("/Game/Temp/CortexBPTest_Dup/BP_DupSource"));
	if (CreatedSource)
	{
		CreatedSource->MarkAsGarbage();
	}

	UObject* CreatedCopy = LoadObject<UBlueprint>(nullptr, TEXT("/Game/Temp/CortexBPTest_Dup/BP_DupCopy"));
	if (CreatedCopy)
	{
		CreatedCopy->MarkAsGarbage();
	}

	return true;
}
