#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphDescribeNodeTest,
	"Cortex.Graph.DescribeNode.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
bool ParamListContainsName(const TSharedPtr<FJsonObject>& Contract, const FString& ListField, const FString& ParamName)
{
	const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
	if (!Contract.IsValid() || !Contract->TryGetArrayField(ListField, Params) || Params == nullptr)
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Params)
	{
		const TSharedPtr<FJsonObject>* ParamObj = nullptr;
		if (Value.IsValid() && Value->TryGetObject(ParamObj) && ParamObj && (*ParamObj).IsValid())
		{
			FString Name;
			if ((*ParamObj)->TryGetStringField(TEXT("name"), Name) && Name == ParamName)
			{
				return true;
			}
		}
	}
	return false;
}
}

bool FCortexGraphDescribeNodeTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"),
		MakeShared<FCortexGraphCommandHandler>());

	auto Describe = [&Router, this](const FString& NodeClass) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("node_class"), NodeClass);
		FCortexCommandResult Result = Router.Execute(TEXT("graph.describe_node"), Params);
		TestTrue(TEXT("describe_node succeeds"), Result.bSuccess);
		return Result.bSuccess && Result.Data.IsValid() ? Result.Data : nullptr;
	};

	{
		TSharedPtr<FJsonObject> Contract = Describe(TEXT("UK2Node_VariableGet"));
		TestTrue(TEXT("VariableGet contract present"), Contract.IsValid());
		if (Contract.IsValid())
		{
			TestEqual(TEXT("canonical class"), Contract->GetStringField(TEXT("node_class")), FString(TEXT("UK2Node_VariableGet")));
			TestTrue(TEXT("supported flag"), Contract->GetBoolField(TEXT("supported")));
			TestTrue(TEXT("prerequisites mention is_variable"),
				Contract->GetStringField(TEXT("prerequisites")).Contains(TEXT("is_variable=true")));
			TestTrue(TEXT("non-retryable errors include VARIABLE_NOT_FOUND"),
				Contract->GetStringField(TEXT("non_retryable_errors")).Contains(TEXT("VARIABLE_NOT_FOUND")));
		}
	}

	{
		TSharedPtr<FJsonObject> Contract = Describe(TEXT("UK2Node_SwitchEnum"));
		TestTrue(TEXT("SwitchEnum contract present"), Contract.IsValid());
		if (Contract.IsValid())
		{
			TestTrue(TEXT("SwitchEnum documents enum_name"),
				ParamListContainsName(Contract, TEXT("optional_params"), TEXT("enum_name")));
		}
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("node_class"), TEXT("UK2Node_NotASupportedClass"));
		FCortexCommandResult Result = Router.Execute(TEXT("graph.describe_node"), Params);
		TestFalse(TEXT("unsupported class fails"), Result.bSuccess);
	}

	// Contract shape must be type-stable: with construction params the probe path
	// allocates real pins, and expected_pins must STILL serialize as an array (the
	// same shape as the no-params contract) — a machine-readable contract cannot
	// type-switch the same field between array and object.
	auto DescribeWithParams = [&Router, this](const FString& NodeClass, const TSharedPtr<FJsonObject>& NodeParams) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("node_class"), NodeClass);
		Params->SetObjectField(TEXT("params"), NodeParams);
		FCortexCommandResult Result = Router.Execute(TEXT("graph.describe_node"), Params);
		TestTrue(TEXT("describe_node with params succeeds"), Result.bSuccess);
		return Result.bSuccess && Result.Data.IsValid() ? Result.Data : nullptr;
	};

	{
		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
		TSharedPtr<FJsonObject> Contract = DescribeWithParams(TEXT("UK2Node_CallFunction"), NodeParams);
		TestTrue(TEXT("CallFunction params contract present"), Contract.IsValid());
		if (Contract.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
			TestTrue(TEXT("expected_pins stays an array with params"), Contract->TryGetArrayField(TEXT("expected_pins"), Pins) && Pins != nullptr);
			TestTrue(TEXT("pins_allocated true with params"), Contract->GetBoolField(TEXT("pins_allocated")));
			if (Pins != nullptr)
			{
				TestTrue(TEXT("probe pins contain PrintString inputs"), Pins->Num() > 0);
			}
		}
	}

	{
		// Invalid construction params must not change the expected_pins shape either.
		TSharedPtr<FJsonObject> BadParams = MakeShared<FJsonObject>();
		BadParams->SetStringField(TEXT("function_name"), TEXT("Missing.Owner"));
		TSharedPtr<FJsonObject> Contract = DescribeWithParams(TEXT("UK2Node_CallFunction"), BadParams);
		TestTrue(TEXT("invalid-params contract present"), Contract.IsValid());
		if (Contract.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
			TestTrue(TEXT("expected_pins stays an array with invalid params"), Contract->TryGetArrayField(TEXT("expected_pins"), Pins) && Pins != nullptr);
		}
	}

	{
		// Empty params object: same stable array shape.
		TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Contract = DescribeWithParams(TEXT("UK2Node_IfThenElse"), EmptyParams);
		TestTrue(TEXT("empty-params contract present"), Contract.IsValid());
		if (Contract.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
			TestTrue(TEXT("expected_pins stays an array with empty params"), Contract->TryGetArrayField(TEXT("expected_pins"), Pins) && Pins != nullptr);
		}
	}

	return true;
}
