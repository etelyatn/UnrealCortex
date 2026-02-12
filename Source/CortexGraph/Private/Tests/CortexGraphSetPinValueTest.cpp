#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSetPinValueTest,
	"Cortex.Graph.SetPinValue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphSetPinValueTest::RunTest(const FString& Parameters)
{
	// Setup: Create a transient Blueprint for testing
	UPackage* TestPackage = NewObject<UPackage>(nullptr, TEXT("/Temp/CortexGraphSetPinValueTest"), RF_Transient);
	TestPackage->SetPackageFlags(PKG_PlayInEditor);

	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		TestPackage,
		TEXT("BP_SetPinValueTest"),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);
	TestNotNull(TEXT("Test Blueprint should be created"), TestBP);
	if (TestBP == nullptr) return false;

	FString AssetPath = TestBP->GetPathName();

	// Register handler
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.0"),
		MakeShared<FCortexGraphCommandHandler>());

	// Add a Delay node
	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("asset_path"), AssetPath);
	AddParams->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
	TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
	NodeParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.Delay"));
	AddParams->SetObjectField(TEXT("params"), NodeParams);

	FCortexCommandResult AddResult = Router.Execute(TEXT("graph.add_node"), AddParams);
	TestTrue(TEXT("add_node should succeed"), AddResult.bSuccess);

	FString NodeId;
	if (AddResult.bSuccess && AddResult.Data.IsValid())
	{
		AddResult.Data->TryGetStringField(TEXT("node_id"), NodeId);
	}
	TestFalse(TEXT("Should have node_id"), NodeId.IsEmpty());

	// Test: Set Duration pin to 5.0
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_id"), NodeId);
		Params->SetStringField(TEXT("pin_name"), TEXT("Duration"));
		Params->SetStringField(TEXT("value"), TEXT("5.0"));

		FCortexCommandResult Result = Router.Execute(TEXT("graph.set_pin_value"), Params);
		TestTrue(TEXT("set_pin_value should succeed"), Result.bSuccess);

		if (Result.bSuccess && Result.Data.IsValid())
		{
			FString SetValue;
			Result.Data->TryGetStringField(TEXT("value"), SetValue);
			TestEqual(TEXT("Value should be 5.0"), SetValue, TEXT("5.0"));
		}
	}

	// Test: Verify the value was set by getting node info
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_id"), NodeId);

		FCortexCommandResult Result = Router.Execute(TEXT("graph.get_node"), Params);
		TestTrue(TEXT("get_node should succeed"), Result.bSuccess);

		if (Result.bSuccess && Result.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
			if (Result.Data->TryGetArrayField(TEXT("pins"), Pins))
			{
				bool bFoundDurationPin = false;
				for (const TSharedPtr<FJsonValue>& PinValue : *Pins)
				{
					const TSharedPtr<FJsonObject>* PinObj = nullptr;
					if (PinValue->TryGetObject(PinObj) && PinObj)
					{
						FString PinName;
						(*PinObj)->TryGetStringField(TEXT("name"), PinName);
						if (PinName == TEXT("Duration"))
						{
							bFoundDurationPin = true;
							FString DefaultValue;
							(*PinObj)->TryGetStringField(TEXT("default_value"), DefaultValue);
							TestEqual(TEXT("Duration pin should have value 5.0"), DefaultValue, TEXT("5.0"));
							break;
						}
					}
				}
				TestTrue(TEXT("Should find Duration pin"), bFoundDurationPin);
			}
		}
	}

	// Test: Add a PrintString node
	TSharedPtr<FJsonObject> AddPrintParams = MakeShared<FJsonObject>();
	AddPrintParams->SetStringField(TEXT("asset_path"), AssetPath);
	AddPrintParams->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
	TSharedPtr<FJsonObject> PrintNodeParams = MakeShared<FJsonObject>();
	PrintNodeParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
	AddPrintParams->SetObjectField(TEXT("params"), PrintNodeParams);

	FCortexCommandResult AddPrintResult = Router.Execute(TEXT("graph.add_node"), AddPrintParams);
	TestTrue(TEXT("add_node PrintString should succeed"), AddPrintResult.bSuccess);

	FString PrintNodeId;
	if (AddPrintResult.bSuccess && AddPrintResult.Data.IsValid())
	{
		AddPrintResult.Data->TryGetStringField(TEXT("node_id"), PrintNodeId);
	}

	// Test: Set InString pin to "Hello World"
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_id"), PrintNodeId);
		Params->SetStringField(TEXT("pin_name"), TEXT("InString"));
		Params->SetStringField(TEXT("value"), TEXT("Hello World"));

		FCortexCommandResult Result = Router.Execute(TEXT("graph.set_pin_value"), Params);
		TestTrue(TEXT("set_pin_value InString should succeed"), Result.bSuccess);
	}

	// Test: Missing asset_path
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("node_id"), NodeId);
		Params->SetStringField(TEXT("pin_name"), TEXT("Duration"));
		Params->SetStringField(TEXT("value"), TEXT("1.0"));

		FCortexCommandResult Result = Router.Execute(TEXT("graph.set_pin_value"), Params);
		TestFalse(TEXT("set_pin_value without asset_path should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// Test: Invalid pin name
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_id"), NodeId);
		Params->SetStringField(TEXT("pin_name"), TEXT("NonExistentPin"));
		Params->SetStringField(TEXT("value"), TEXT("1.0"));

		FCortexCommandResult Result = Router.Execute(TEXT("graph.set_pin_value"), Params);
		TestFalse(TEXT("set_pin_value with invalid pin should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be PIN_NOT_FOUND"), Result.ErrorCode, CortexErrorCodes::PinNotFound);
	}

	// Test: Try to set value on output pin (should fail)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_id"), NodeId);
		Params->SetStringField(TEXT("pin_name"), TEXT("then"));
		Params->SetStringField(TEXT("value"), TEXT("test"));

		FCortexCommandResult Result = Router.Execute(TEXT("graph.set_pin_value"), Params);
		TestFalse(TEXT("set_pin_value on output pin should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be INVALID_OPERATION"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
	}

	TestBP->MarkAsGarbage();

	return true;
}
