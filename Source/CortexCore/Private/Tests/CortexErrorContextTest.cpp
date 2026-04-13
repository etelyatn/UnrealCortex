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
	FCortexErrorContextAddArrayTruncationTest,
	"Cortex.Core.ErrorContext.AddArrayTruncation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexErrorContextAddArrayTruncationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCommandResult Result = FCortexCommandRouter::Error(
		TEXT("TYPE_MISMATCH"),
		TEXT("Type mismatch for property OpenSeq"));

	TArray<FString> Formats;
	for (int32 Index = 0; Index < 21; ++Index)
	{
		Formats.Add(FString::Printf(TEXT("format_%d"), Index));
	}
	Result.AddContext(TEXT("accepted_formats"), Formats);

	TestTrue(TEXT("ErrorDetails should exist"), Result.ErrorDetails.IsValid());

	const TArray<TSharedPtr<FJsonValue>>* FormatsArr = nullptr;
	TestTrue(TEXT("accepted_formats array should exist"),
		Result.ErrorDetails->TryGetArrayField(TEXT("accepted_formats"), FormatsArr));
	if (FormatsArr)
	{
		TestEqual(TEXT("Should cap array at 20"), FormatsArr->Num(), 20);
		if (FormatsArr->Num() == 20)
		{
			TestEqual(TEXT("First format should match"),
				(*FormatsArr)[0]->AsString(),
				Formats[0]);
			TestEqual(TEXT("Last retained format should match"),
				(*FormatsArr)[19]->AsString(),
				Formats[19]);
		}
	}

	TestTrue(TEXT("accepted_formats_truncated should exist"),
		Result.ErrorDetails->HasField(TEXT("accepted_formats_truncated")));
	TestTrue(TEXT("accepted_formats_truncated should be true"),
		Result.ErrorDetails->GetBoolField(TEXT("accepted_formats_truncated")));
	TestTrue(TEXT("accepted_formats_total should exist"),
		Result.ErrorDetails->HasField(TEXT("accepted_formats_total")));
	TestEqual(TEXT("accepted_formats_total should match original count"),
		static_cast<int32>(Result.ErrorDetails->GetNumberField(TEXT("accepted_formats_total"))),
		21);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexErrorContextRewriteClearsTruncationTest,
	"Cortex.Core.ErrorContext.RewriteClearsTruncation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexErrorContextRewriteClearsTruncationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCommandResult Result = FCortexCommandRouter::Error(
		TEXT("TYPE_MISMATCH"),
		TEXT("Type mismatch for property OpenSeq"));

	TArray<FString> LongFormats;
	for (int32 Index = 0; Index < 21; ++Index)
	{
		LongFormats.Add(FString::Printf(TEXT("long_%d"), Index));
	}
	Result.AddContext(TEXT("accepted_formats"), LongFormats);

	TArray<FString> ShortFormats = {
		TEXT("first"),
		TEXT("second"),
		TEXT("third")
	};
	Result.AddContext(TEXT("accepted_formats"), ShortFormats);

	const TArray<TSharedPtr<FJsonValue>>* FormatsArr = nullptr;
	TestTrue(TEXT("accepted_formats array should exist"),
		Result.ErrorDetails->TryGetArrayField(TEXT("accepted_formats"), FormatsArr));
	if (FormatsArr)
	{
		TestEqual(TEXT("Should have 3 formats after rewrite"), FormatsArr->Num(), 3);
		if (FormatsArr->Num() == 3)
		{
			TestEqual(TEXT("Format 0 should match"), (*FormatsArr)[0]->AsString(), ShortFormats[0]);
			TestEqual(TEXT("Format 1 should match"), (*FormatsArr)[1]->AsString(), ShortFormats[1]);
			TestEqual(TEXT("Format 2 should match"), (*FormatsArr)[2]->AsString(), ShortFormats[2]);
		}
	}

	TestFalse(TEXT("Should not retain truncated metadata after rewrite"),
		Result.ErrorDetails->HasField(TEXT("accepted_formats_truncated")));
	TestFalse(TEXT("Should not retain total metadata after rewrite"),
		Result.ErrorDetails->HasField(TEXT("accepted_formats_total")));

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
	FCortexErrorContextNumericContextTest,
	"Cortex.Core.ErrorContext.NumericContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexErrorContextNumericContextTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCommandResult Result = FCortexCommandRouter::Error(
		TEXT("TYPE_MISMATCH"),
		TEXT("Type mismatch"));

	Result.AddContext(TEXT("double_value"), 2.5);
	Result.AddContext(TEXT("int_value"), 7);

	TestTrue(TEXT("ErrorDetails should exist"), Result.ErrorDetails.IsValid());
	TestTrue(TEXT("double_value should exist"), Result.ErrorDetails->HasField(TEXT("double_value")));
	TestTrue(TEXT("int_value should exist"), Result.ErrorDetails->HasField(TEXT("int_value")));
	TestEqual(TEXT("double_value should match"),
		Result.ErrorDetails->GetNumberField(TEXT("double_value")),
		2.5);
	TestEqual(TEXT("int_value should match"),
		static_cast<int32>(Result.ErrorDetails->GetNumberField(TEXT("int_value"))),
		7);

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexErrorContextLazyInitTest,
	"Cortex.Core.ErrorContext.LazyInitFromDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexErrorContextLazyInitTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCommandResult Result;
	TestFalse(TEXT("ErrorDetails should be null initially"), Result.ErrorDetails.IsValid());

	Result.AddContext(TEXT("key"), TEXT("value"));

	TestTrue(TEXT("ErrorDetails should be auto-created"), Result.ErrorDetails.IsValid());
	TestEqual(TEXT("key should have correct value"),
		Result.ErrorDetails->GetStringField(TEXT("key")),
		TEXT("value"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexErrorContextAddArrayBoundaryTest,
	"Cortex.Core.ErrorContext.AddArrayBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexErrorContextAddArrayBoundaryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCommandResult Result = FCortexCommandRouter::Error(
		TEXT("TEST"), TEXT("boundary test"));

	TArray<FString> Formats;
	for (int32 Index = 0; Index < FCortexCommandResult::MaxContextArrayEntries; ++Index)
	{
		Formats.Add(FString::Printf(TEXT("format_%d"), Index));
	}
	Result.AddContext(TEXT("accepted_formats"), Formats);

	const TArray<TSharedPtr<FJsonValue>>* FormatsArr = nullptr;
	TestTrue(TEXT("accepted_formats array should exist"),
		Result.ErrorDetails->TryGetArrayField(TEXT("accepted_formats"), FormatsArr));
	if (FormatsArr)
	{
		TestEqual(TEXT("Should have exactly MaxContextArrayEntries"),
			FormatsArr->Num(), FCortexCommandResult::MaxContextArrayEntries);
	}

	TestFalse(TEXT("Exactly at limit should not be truncated"),
		Result.ErrorDetails->HasField(TEXT("accepted_formats_truncated")));
	TestFalse(TEXT("Exactly at limit should not have total"),
		Result.ErrorDetails->HasField(TEXT("accepted_formats_total")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexErrorContextAddNullJsonTest,
	"Cortex.Core.ErrorContext.AddNullJsonIgnored",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexErrorContextAddNullJsonTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexCommandResult Result;
	TSharedPtr<FJsonObject> NullJson;
	Result.AddContext(TEXT("should_not_exist"), NullJson);

	TestFalse(TEXT("ErrorDetails should remain null when adding null JSON"),
		Result.ErrorDetails.IsValid());

	return true;
}
