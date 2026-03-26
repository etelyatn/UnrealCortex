#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphDelegateNodeTest,
	"Cortex.Graph.DelegateNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphDelegateNodeTest::RunTest(const FString& Parameters)
{
	// Setup: Create a transient Blueprint for testing
	UPackage* TestPackage = NewObject<UPackage>(nullptr, TEXT("/Temp/CortexGraphDelegateNodeTest"), RF_Transient);
	TestPackage->SetPackageFlags(PKG_PlayInEditor);

	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		TestPackage,
		TEXT("BP_DelegateNodeTest"),
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

	// ---- Test 1: AddDelegate with external class delegate ----
	// Use OnTakeAnyDamage (inline multicast delegate, not sparse).
	// Avoid OnDestroyed — it is a sparse delegate (DECLARE_DYNAMIC_MULTICAST_SPARSE_DELEGATE)
	// whose FMulticastSparseDelegateProperty may not match FindFField<FMulticastDelegateProperty>
	// depending on UE cast flag inheritance behavior.
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_AddDelegate"));
		TSharedPtr<FJsonObject> NParams = MakeShared<FJsonObject>();
		NParams->SetStringField(TEXT("delegate_name"), TEXT("OnTakeAnyDamage"));
		NParams->SetStringField(TEXT("delegate_class"), TEXT("Actor"));
		Params->SetObjectField(TEXT("params"), NParams);

		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestTrue(TEXT("AddDelegate with OnTakeAnyDamage should succeed"), Result.bSuccess);

		if (Result.bSuccess && Result.Data.IsValid())
		{
			TestTrue(TEXT("AddDelegate should have node_id"), Result.Data->HasField(TEXT("node_id")));

			// Verify pins: should have execute, then, self, Delegate
			const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
			TestTrue(TEXT("AddDelegate should have pins"), Result.Data->TryGetArrayField(TEXT("pins"), Pins));
			if (Pins)
			{
				TestTrue(TEXT("AddDelegate should have at least 4 pins"), Pins->Num() >= 4);
			}

			// Verify display name contains "Bind Event"
			FString DisplayName;
			if (Result.Data->TryGetStringField(TEXT("display_name"), DisplayName))
			{
				TestTrue(TEXT("AddDelegate display name should contain 'Bind Event'"),
					DisplayName.Contains(TEXT("Bind Event")));
			}
		}
	}

	// ---- Test 2: RemoveDelegate with external class delegate ----
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_RemoveDelegate"));
		TSharedPtr<FJsonObject> NParams = MakeShared<FJsonObject>();
		NParams->SetStringField(TEXT("delegate_name"), TEXT("OnTakeAnyDamage"));
		NParams->SetStringField(TEXT("delegate_class"), TEXT("Actor"));
		Params->SetObjectField(TEXT("params"), NParams);

		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestTrue(TEXT("RemoveDelegate with OnTakeAnyDamage should succeed"), Result.bSuccess);

		if (Result.bSuccess && Result.Data.IsValid())
		{
			FString DisplayName;
			if (Result.Data->TryGetStringField(TEXT("display_name"), DisplayName))
			{
				TestTrue(TEXT("RemoveDelegate display name should contain 'Unbind'"),
					DisplayName.Contains(TEXT("Unbind")));
			}
		}
	}

	// ---- Test 3: ClearDelegate with external class delegate ----
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_ClearDelegate"));
		TSharedPtr<FJsonObject> NParams = MakeShared<FJsonObject>();
		NParams->SetStringField(TEXT("delegate_name"), TEXT("OnTakeAnyDamage"));
		NParams->SetStringField(TEXT("delegate_class"), TEXT("Actor"));
		Params->SetObjectField(TEXT("params"), NParams);

		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestTrue(TEXT("ClearDelegate with OnTakeAnyDamage should succeed"), Result.bSuccess);

		if (Result.bSuccess && Result.Data.IsValid())
		{
			FString DisplayName;
			if (Result.Data->TryGetStringField(TEXT("display_name"), DisplayName))
			{
				TestTrue(TEXT("ClearDelegate display name should contain 'Unbind all'"),
					DisplayName.Contains(TEXT("Unbind all")));
			}

			// ClearDelegate should have exactly 3 pins: execute, then, self
			// It must NOT have a Delegate pin (unlike AddDelegate/RemoveDelegate)
			const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
			if (Result.Data->TryGetArrayField(TEXT("pins"), Pins) && Pins)
			{
				TestEqual(TEXT("ClearDelegate should have exactly 3 pins"), Pins->Num(), 3);
			}
		}
	}

	// ---- Test 4: CreateDelegate with function name ----
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_CreateDelegate"));
		TSharedPtr<FJsonObject> NParams = MakeShared<FJsonObject>();
		NParams->SetStringField(TEXT("function_name"), TEXT("MyCustomHandler"));
		Params->SetObjectField(TEXT("params"), NParams);

		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestTrue(TEXT("CreateDelegate should succeed"), Result.bSuccess);

		if (Result.bSuccess && Result.Data.IsValid())
		{
			TestTrue(TEXT("CreateDelegate should have node_id"), Result.Data->HasField(TEXT("node_id")));

			// CreateDelegate is a pure node with self + OutputDelegate pins
			const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
			TestTrue(TEXT("CreateDelegate should have pins"), Result.Data->TryGetArrayField(TEXT("pins"), Pins));
			if (Pins)
			{
				TestTrue(TEXT("CreateDelegate should have at least 2 pins"), Pins->Num() >= 2);
			}
		}
	}

	// ---- Test 5: CreateDelegate without params (no function_name) should still succeed ----
	// CreateDelegate can be created without function_name; it gets set later or via connection
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_CreateDelegate"));

		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestTrue(TEXT("CreateDelegate without function_name should succeed"), Result.bSuccess);
	}

	// ---- Error Tests ----

	// Test 6: AddDelegate missing delegate_name should fail
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_AddDelegate"));

		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestFalse(TEXT("AddDelegate without delegate_name should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// Test 7: AddDelegate with invalid delegate class should fail
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_AddDelegate"));
		TSharedPtr<FJsonObject> NParams = MakeShared<FJsonObject>();
		NParams->SetStringField(TEXT("delegate_name"), TEXT("OnTakeAnyDamage"));
		NParams->SetStringField(TEXT("delegate_class"), TEXT("NonExistentClass"));
		Params->SetObjectField(TEXT("params"), NParams);

		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestFalse(TEXT("AddDelegate with invalid class should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// Test 8: AddDelegate with non-delegate property should fail
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_AddDelegate"));
		TSharedPtr<FJsonObject> NParams = MakeShared<FJsonObject>();
		NParams->SetStringField(TEXT("delegate_name"), TEXT("bHidden"));  // bool, not a delegate
		NParams->SetStringField(TEXT("delegate_class"), TEXT("Actor"));
		Params->SetObjectField(TEXT("params"), NParams);

		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestFalse(TEXT("AddDelegate with non-delegate property should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// Test 9: AddDelegate with nonexistent delegate name should fail
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_AddDelegate"));
		TSharedPtr<FJsonObject> NParams = MakeShared<FJsonObject>();
		NParams->SetStringField(TEXT("delegate_name"), TEXT("NonExistentDelegate"));
		NParams->SetStringField(TEXT("delegate_class"), TEXT("Actor"));
		Params->SetObjectField(TEXT("params"), NParams);

		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestFalse(TEXT("AddDelegate with nonexistent delegate should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// Test 10: RemoveDelegate missing delegate_name should fail
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_RemoveDelegate"));

		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestFalse(TEXT("RemoveDelegate without delegate_name should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// Test 11: ClearDelegate missing delegate_name should fail
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_ClearDelegate"));

		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestFalse(TEXT("ClearDelegate without delegate_name should fail"), Result.bSuccess);
		TestEqual(TEXT("Error should be INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// ---- Test 12: Self-context delegate (Blueprint's own event dispatcher) ----
	// Add a multicast delegate variable to the Blueprint, compile, then bind to it
	{
		// Add event dispatcher (multicast delegate) to BP
		FEdGraphPinType DelegatePinType;
		DelegatePinType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
		FBlueprintEditorUtils::AddMemberVariable(TestBP, TEXT("OnCustomEvent"), DelegatePinType);
		FKismetEditorUtilities::CompileBlueprint(TestBP);

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_AddDelegate"));
		TSharedPtr<FJsonObject> NParams = MakeShared<FJsonObject>();
		NParams->SetStringField(TEXT("delegate_name"), TEXT("OnCustomEvent"));
		// No delegate_class — self-context
		Params->SetObjectField(TEXT("params"), NParams);

		FCortexCommandResult Result = Router.Execute(TEXT("graph.add_node"), Params);
		TestTrue(TEXT("AddDelegate self-context should succeed"), Result.bSuccess);

		if (Result.bSuccess && Result.Data.IsValid())
		{
			// Should have at least 4 pins like external AddDelegate
			const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
			TestTrue(TEXT("Self-context AddDelegate should have pins"),
				Result.Data->TryGetArrayField(TEXT("pins"), Pins));
			if (Pins)
			{
				TestTrue(TEXT("Self-context AddDelegate should have at least 4 pins"),
					Pins->Num() >= 4);
			}
		}
	}

	TestBP->MarkAsGarbage();
	TestPackage->MarkAsGarbage();

	return true;
}
