#include "Misc/AutomationTest.h"
#include "CortexTypes.h"
#include "CortexCommandRouter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexErrorContextAddStringTest,
	"Cortex.Core.ErrorContext.AddStringContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexErrorContextAddStringTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCommandResult Result = FCortexCommandRouter::Error(
		TEXT("TYPE_MISMATCH"),
		TEXT("Property OpenSeq expects UActorSequenceComponent"));

	Result.AddContext(TEXT("expected_type"), TEXT("TObjectPtr<UActorSequenceComponent>"));

	TestTrue(TEXT("ErrorDetails should exist"), Result.ErrorDetails.IsValid());
	TestEqual(TEXT("expected_type field"),
		Result.ErrorDetails->GetStringField(TEXT("expected_type")),
		TEXT("TObjectPtr<UActorSequenceComponent>"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexErrorContextAddArrayTest,
	"Cortex.Core.ErrorContext.AddArrayContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexErrorContextAddArrayTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCommandResult Result = FCortexCommandRouter::Error(
		TEXT("TYPE_MISMATCH"),
		TEXT("Type mismatch for property OpenSeq"));

	TArray<FString> Formats = {
		TEXT("bare SCS variable name (e.g. 'DoorOpenSeq')"),
		TEXT("full CDO path (e.g. '/Game/.../BP_Lift.BP_Lift_C:DoorOpenSeq_GEN_VARIABLE')"),
		TEXT("null (to clear)")
	};
	Result.AddContext(TEXT("accepted_formats"), Formats);

	TestTrue(TEXT("ErrorDetails should exist"), Result.ErrorDetails.IsValid());

	const TArray<TSharedPtr<FJsonValue>>* FormatsArr = nullptr;
	TestTrue(TEXT("accepted_formats array should exist"),
		Result.ErrorDetails->TryGetArrayField(TEXT("accepted_formats"), FormatsArr));
	if (FormatsArr)
	{
		TestEqual(TEXT("Should have 3 formats"), FormatsArr->Num(), 3);
		if (FormatsArr->Num() == 3)
		{
			TestEqual(TEXT("Format 0 should match"),
				(*FormatsArr)[0]->AsString(),
				Formats[0]);
			TestEqual(TEXT("Format 1 should match"),
				(*FormatsArr)[1]->AsString(),
				Formats[1]);
			TestEqual(TEXT("Format 2 should match"),
				(*FormatsArr)[2]->AsString(),
				Formats[2]);
		}
	}
	TestFalse(TEXT("Should not be truncated"),
		Result.ErrorDetails->HasField(TEXT("accepted_formats_truncated")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexErrorContextAddJsonTest,
	"Cortex.Core.ErrorContext.AddJsonContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexErrorContextAddJsonTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCommandResult Result = FCortexCommandRouter::Error(
		TEXT("TYPE_MISMATCH"),
		TEXT("Type mismatch"));

	TSharedPtr<FJsonObject> NestedObj = MakeShared<FJsonObject>();
	NestedObj->SetStringField(TEXT("class"), TEXT("StaticMeshComponent"));
	NestedObj->SetNumberField(TEXT("property_count"), 42);
	Result.AddContext(TEXT("target_info"), NestedObj);

	TestTrue(TEXT("ErrorDetails should exist"), Result.ErrorDetails.IsValid());

	const TSharedPtr<FJsonObject>* TargetInfo = nullptr;
	TestTrue(TEXT("target_info object should exist"),
		Result.ErrorDetails->TryGetObjectField(TEXT("target_info"), TargetInfo));
	if (TargetInfo && TargetInfo->IsValid())
	{
		TestEqual(TEXT("target_info.class should match"),
			(*TargetInfo)->GetStringField(TEXT("class")),
			TEXT("StaticMeshComponent"));
		TestEqual(TEXT("target_info.property_count should match"),
			static_cast<int32>((*TargetInfo)->GetNumberField(TEXT("property_count"))),
			42);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexErrorContextPreservesExistingTest,
	"Cortex.Core.ErrorContext.PreservesExistingDetails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexErrorContextPreservesExistingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	TSharedPtr<FJsonObject> ExistingDetails = MakeShared<FJsonObject>();
	ExistingDetails->SetStringField(TEXT("existing_key"), TEXT("existing_value"));

	FCortexCommandResult Result = FCortexCommandRouter::Error(
		TEXT("TYPE_MISMATCH"),
		TEXT("Type mismatch"),
		ExistingDetails);

	Result.AddContext(TEXT("new_key"), TEXT("new_value"));

	TestEqual(TEXT("existing_key preserved"),
		Result.ErrorDetails->GetStringField(TEXT("existing_key")),
		TEXT("existing_value"));
	TestEqual(TEXT("new_key added"),
		Result.ErrorDetails->GetStringField(TEXT("new_key")),
		TEXT("new_value"));

	return true;
}
