#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveGraphTest,
	"Cortex.Blueprint.RemoveGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPRemoveGraphTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	const FString TestBPPath = TEXT("/Game/Temp/CortexBPTest_RemoveGraph/BP_RemoveGraphTest");

	// Setup: create a Blueprint
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("name"), TEXT("BP_RemoveGraphTest"));
		Params->SetStringField(TEXT("path"), TEXT("/Game/Temp/CortexBPTest_RemoveGraph"));
		Params->SetStringField(TEXT("type"), TEXT("Actor"));
		FCortexCommandResult R = Handler.Execute(TEXT("create"), Params);
		TestTrue(TEXT("Setup: create BP"), R.bSuccess);
	}

	// Setup: add a function graph
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("name"), TEXT("TestFunction"));
		FCortexCommandResult R = Handler.Execute(TEXT("add_function"), Params);
		TestTrue(TEXT("Setup: add function"), R.bSuccess);
	}

	// Test: remove function graph
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("name"), TEXT("TestFunction"));
		Params->SetBoolField(TEXT("compile"), false);

		FCortexCommandResult Result = Handler.Execute(TEXT("remove_graph"), Params);
		TestTrue(TEXT("remove_graph should succeed"), Result.bSuccess);

		if (Result.Data.IsValid())
		{
			const TSharedPtr<FJsonObject>* RemovedObj = nullptr;
			if (TestTrue(TEXT("Should have removed object"),
				Result.Data->TryGetObjectField(TEXT("removed"), RemovedObj)))
			{
				FString Name;
				(*RemovedObj)->TryGetStringField(TEXT("name"), Name);
				TestEqual(TEXT("Removed name"), Name, TEXT("TestFunction"));

				FString Type;
				(*RemovedObj)->TryGetStringField(TEXT("type"), Type);
				TestEqual(TEXT("Removed type"), Type, TEXT("Function"));

				double NodeCount = 0;
				(*RemovedObj)->TryGetNumberField(TEXT("node_count"), NodeCount);
				TestTrue(TEXT("node_count should be > 0"), NodeCount > 0.0);
			}
		}
	}

	// Verify: function no longer in get_info
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);

		FCortexCommandResult Result = Handler.Execute(TEXT("get_info"), Params);
		if (Result.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* FuncsArray = nullptr;
			if (Result.Data->TryGetArrayField(TEXT("functions"), FuncsArray))
			{
				bool bFoundTestFunc = false;
				for (const TSharedPtr<FJsonValue>& Val : *FuncsArray)
				{
					TSharedPtr<FJsonObject> Obj = Val->AsObject();
					if (Obj.IsValid())
					{
						FString FuncName;
						Obj->TryGetStringField(TEXT("name"), FuncName);
						if (FuncName == TEXT("TestFunction"))
						{
							bFoundTestFunc = true;
							break;
						}
					}
				}
				TestFalse(TEXT("TestFunction should no longer exist"), bFoundTestFunc);
			}
		}
	}

	// Cleanup
	UObject* CreatedBP = LoadObject<UBlueprint>(nullptr, *TestBPPath);
	if (CreatedBP)
	{
		CreatedBP->GetOutermost()->MarkAsGarbage();
	}

	return true;
}
