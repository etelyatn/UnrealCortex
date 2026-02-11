#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphConnectPinsTest,
	"Cortex.Graph.ConnectPins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphConnectPinsTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_CortexGraphTest_Connect")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);
	TestNotNull(TEXT("Test Blueprint should be created"), TestBP);

	if (TestBP == nullptr)
	{
		return true;
	}

	FString AssetPath = TestBP->GetPathName();

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.0"),
		MakeShared<FCortexGraphCommandHandler>());

	// Add two CallFunction nodes (PrintString)
	FString Node1Id;
	FString Node2Id;

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
		Params->SetObjectField(TEXT("params"), NodeParams);
		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestTrue(TEXT("add first node should succeed"), Result.bSuccess);
		if (Result.Data.IsValid())
		{
			Result.Data->TryGetStringField(TEXT("node_id"), Node1Id);
		}
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
		Params->SetObjectField(TEXT("params"), NodeParams);
		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestTrue(TEXT("add second node should succeed"), Result.bSuccess);
		if (Result.Data.IsValid())
		{
			Result.Data->TryGetStringField(TEXT("node_id"), Node2Id);
		}
	}

	TestFalse(TEXT("Node1 ID should not be empty"), Node1Id.IsEmpty());
	TestFalse(TEXT("Node2 ID should not be empty"), Node2Id.IsEmpty());

	// Test: connect exec pin — Node1 "then" -> Node2 "execute"
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("source_node"), Node1Id);
		Params->SetStringField(TEXT("source_pin"), TEXT("then"));
		Params->SetStringField(TEXT("target_node"), Node2Id);
		Params->SetStringField(TEXT("target_pin"), TEXT("execute"));

		FCortexCommandResult Result = Router.Execute(TEXT("graph.connect"), Params);
		TestTrue(TEXT("connect should succeed"), Result.bSuccess);

		if (Result.Data.IsValid())
		{
			bool bConnected = false;
			Result.Data->TryGetBoolField(TEXT("connected"), bConnected);
			TestTrue(TEXT("connected should be true"), bConnected);
		}
	}

	// Test: reconnecting same pins should fail with CONNECTION_EXISTS
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("source_node"), Node1Id);
		Params->SetStringField(TEXT("source_pin"), TEXT("then"));
		Params->SetStringField(TEXT("target_node"), Node2Id);
		Params->SetStringField(TEXT("target_pin"), TEXT("execute"));

		FCortexCommandResult Result = Router.Execute(TEXT("graph.connect"), Params);
		TestFalse(TEXT("Reconnecting same pins should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be CONNECTION_EXISTS"),
			Result.ErrorCode, CortexErrorCodes::ConnectionExists);
	}

	// Verify connection via get_node
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_id"), Node1Id);

		FCortexCommandResult Result = Router.Execute(TEXT("graph.get_node"), Params);
		TestTrue(TEXT("get_node should succeed"), Result.bSuccess);

		if (Result.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* PinsArray = nullptr;
			Result.Data->TryGetArrayField(TEXT("pins"), PinsArray);
			if (PinsArray != nullptr)
			{
				bool bFoundConnectedThen = false;
				for (const TSharedPtr<FJsonValue>& PinVal : *PinsArray)
				{
					const TSharedPtr<FJsonObject>* PinObj = nullptr;
					if (PinVal->TryGetObject(PinObj))
					{
						FString PinName;
						(*PinObj)->TryGetStringField(TEXT("name"), PinName);
						if (PinName == TEXT("then"))
						{
							bool bIsConnected = false;
							(*PinObj)->TryGetBoolField(TEXT("is_connected"), bIsConnected);
							bFoundConnectedThen = bIsConnected;
							break;
						}
					}
				}
				TestTrue(TEXT("'then' pin should now be connected"), bFoundConnectedThen);
			}
		}
	}

	// Test: disconnect
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_id"), Node1Id);
		Params->SetStringField(TEXT("pin_name"), TEXT("then"));

		FCortexCommandResult Result = Router.Execute(TEXT("graph.disconnect"), Params);
		TestTrue(TEXT("disconnect should succeed"), Result.bSuccess);

		if (Result.Data.IsValid())
		{
			bool bDisconnected = false;
			Result.Data->TryGetBoolField(TEXT("disconnected"), bDisconnected);
			TestTrue(TEXT("disconnected should be true"), bDisconnected);
		}
	}

	// Verify disconnection via get_node
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_id"), Node1Id);

		FCortexCommandResult Result = Router.Execute(TEXT("graph.get_node"), Params);
		TestTrue(TEXT("get_node should succeed after disconnect"), Result.bSuccess);
		if (Result.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* PinsArray = nullptr;
			Result.Data->TryGetArrayField(TEXT("pins"), PinsArray);
			if (PinsArray != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& PinVal : *PinsArray)
				{
					const TSharedPtr<FJsonObject>* PinObj = nullptr;
					if (PinVal->TryGetObject(PinObj))
					{
						FString PinName;
						(*PinObj)->TryGetStringField(TEXT("name"), PinName);
						if (PinName == TEXT("then"))
						{
							bool bIsConnected = false;
							(*PinObj)->TryGetBoolField(TEXT("is_connected"), bIsConnected);
							TestFalse(TEXT("'then' pin should now be disconnected"), bIsConnected);
							break;
						}
					}
				}
			}
		}
	}

	// Test: connect with bad pin name
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("source_node"), Node1Id);
		Params->SetStringField(TEXT("source_pin"), TEXT("nonexistent_pin"));
		Params->SetStringField(TEXT("target_node"), Node2Id);
		Params->SetStringField(TEXT("target_pin"), TEXT("execute"));

		FCortexCommandResult Result = Router.Execute(TEXT("graph.connect"), Params);
		TestFalse(TEXT("connect with bad pin should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be PIN_NOT_FOUND"), Result.ErrorCode, CortexErrorCodes::PinNotFound);
	}

	// Cleanup
	TestBP->MarkAsGarbage();

	return true;
}
