#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPinConnectionTest,
	"Cortex.Graph.PinConnectionData",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphPinConnectionTest::RunTest(const FString& Parameters)
{
	UPackage* TestPackage = NewObject<UPackage>(nullptr, TEXT("/Temp/CortexGraphPinConnectionTest"), RF_Transient);
	TestPackage->SetPackageFlags(PKG_PlayInEditor);

	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		TestPackage,
		TEXT("BP_PinConnectionTest"),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP)
	{
		return false;
	}

	const FString AssetPath = TestBP->GetPathName();

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.0"), MakeShared<FCortexGraphCommandHandler>());

	FString NodeIdA;
	FString NodeIdB;

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
		TSharedPtr<FJsonObject> NP = MakeShared<FJsonObject>();
		NP->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
		Params->SetObjectField(TEXT("params"), NP);
		const FCortexCommandResult R = Router.Execute(TEXT("graph.add_node"), Params);
		TestTrue(TEXT("add_node A should succeed"), R.bSuccess);
		if (R.Data.IsValid())
		{
			R.Data->TryGetStringField(TEXT("node_id"), NodeIdA);
		}
	}

	TestFalse(TEXT("NodeIdA should not be empty"), NodeIdA.IsEmpty());
	if (NodeIdA.IsEmpty())
	{
		TestBP->MarkAsGarbage();
		return false;
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
		TSharedPtr<FJsonObject> NP = MakeShared<FJsonObject>();
		NP->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
		Params->SetObjectField(TEXT("params"), NP);
		const FCortexCommandResult R = Router.Execute(TEXT("graph.add_node"), Params);
		TestTrue(TEXT("add_node B should succeed"), R.bSuccess);
		if (R.Data.IsValid())
		{
			R.Data->TryGetStringField(TEXT("node_id"), NodeIdB);
		}
	}

	TestFalse(TEXT("NodeIdB should not be empty"), NodeIdB.IsEmpty());
	if (NodeIdB.IsEmpty())
	{
		TestBP->MarkAsGarbage();
		return false;
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("source_node"), NodeIdA);
		Params->SetStringField(TEXT("source_pin"), TEXT("then"));
		Params->SetStringField(TEXT("target_node"), NodeIdB);
		Params->SetStringField(TEXT("target_pin"), TEXT("execute"));
		const FCortexCommandResult R = Router.Execute(TEXT("graph.connect"), Params);
		TestTrue(TEXT("connect should succeed"), R.bSuccess);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_id"), NodeIdA);
		const FCortexCommandResult R = Router.Execute(TEXT("graph.get_node"), Params);
		TestTrue(TEXT("get_node A should succeed"), R.bSuccess);

		if (R.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* PinsArray = nullptr;
			R.Data->TryGetArrayField(TEXT("pins"), PinsArray);
			TestNotNull(TEXT("Should have pins array"), PinsArray);

			bool bFoundThenPin = false;
			if (PinsArray)
			{
				for (const TSharedPtr<FJsonValue>& PinVal : *PinsArray)
				{
					const TSharedPtr<FJsonObject>* PinObj = nullptr;
					if (!PinVal->TryGetObject(PinObj) || !PinObj)
					{
						continue;
					}

					FString PinName;
					(*PinObj)->TryGetStringField(TEXT("name"), PinName);
					if (PinName != TEXT("then"))
					{
						continue;
					}

					bFoundThenPin = true;

					const TArray<TSharedPtr<FJsonValue>>* ConnArray = nullptr;
					TestTrue(
						TEXT("'then' pin should have connected_to array"),
						(*PinObj)->TryGetArrayField(TEXT("connections"), ConnArray)
					);

					if (ConnArray)
					{
						TestEqual(TEXT("connected_to should have 1 entry"), ConnArray->Num(), 1);
						if (ConnArray->Num() > 0)
						{
							const TSharedPtr<FJsonObject>* ConnObj = nullptr;
							(*ConnArray)[0]->TryGetObject(ConnObj);
							if (TestNotNull(TEXT("Connection entry should be object"), ConnObj))
							{
								FString ConnNodeId;
								(*ConnObj)->TryGetStringField(TEXT("node_id"), ConnNodeId);
								TestEqual(TEXT("connected node_id should be B"), ConnNodeId, NodeIdB);

								FString ConnPin;
								(*ConnObj)->TryGetStringField(TEXT("pin"), ConnPin);
								TestEqual(TEXT("connected pin should be 'execute'"), ConnPin, FString(TEXT("execute")));
							}
						}
					}
				}
			}

			TestTrue(TEXT("Should find 'then' pin"), bFoundThenPin);
		}
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_id"), NodeIdB);
		const FCortexCommandResult R = Router.Execute(TEXT("graph.get_node"), Params);
		TestTrue(TEXT("get_node B should succeed"), R.bSuccess);

		if (R.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* PinsArray = nullptr;
			R.Data->TryGetArrayField(TEXT("pins"), PinsArray);

			bool bFoundExecPin = false;
			if (PinsArray)
			{
				for (const TSharedPtr<FJsonValue>& PinVal : *PinsArray)
				{
					const TSharedPtr<FJsonObject>* PinObj = nullptr;
					if (!PinVal->TryGetObject(PinObj) || !PinObj)
					{
						continue;
					}

					FString PinName;
					(*PinObj)->TryGetStringField(TEXT("name"), PinName);
					if (PinName != TEXT("execute"))
					{
						continue;
					}

					bFoundExecPin = true;

					const TArray<TSharedPtr<FJsonValue>>* ConnArray = nullptr;
					TestTrue(
						TEXT("'execute' pin should have connected_to array"),
						(*PinObj)->TryGetArrayField(TEXT("connections"), ConnArray)
					);

					if (ConnArray && ConnArray->Num() > 0)
					{
						const TSharedPtr<FJsonObject>* ConnObj = nullptr;
						(*ConnArray)[0]->TryGetObject(ConnObj);
						if (ConnObj)
						{
							FString ConnNodeId;
							(*ConnObj)->TryGetStringField(TEXT("node_id"), ConnNodeId);
							TestEqual(TEXT("connected node_id should be A"), ConnNodeId, NodeIdA);
						}
					}
				}
			}

			TestTrue(TEXT("Should find 'execute' pin"), bFoundExecPin);
		}
	}

	TestBP->MarkAsGarbage();
	return true;
}
