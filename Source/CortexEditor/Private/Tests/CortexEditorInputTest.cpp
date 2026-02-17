#include "Misc/AutomationTest.h"
#include "CortexEditorCommandHandler.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInjectKeyNoPIETest,
	"Cortex.Editor.Input.InjectKey.ErrorWhenNoPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInjectKeyNoPIETest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("key"), TEXT("W"));
	Params->SetStringField(TEXT("action"), TEXT("tap"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("inject_key"), Params);

	TestFalse(TEXT("inject_key should fail without PIE"), Result.bSuccess);
	TestEqual(TEXT("Error should be PIE_NOT_ACTIVE"), Result.ErrorCode, TEXT("PIE_NOT_ACTIVE"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInjectInputSequenceNoPIETest,
	"Cortex.Editor.Input.InjectInputSequence.ErrorWhenNoPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInjectInputSequenceNoPIETest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Steps;
	TSharedPtr<FJsonObject> StepObj = MakeShared<FJsonObject>();
	StepObj->SetNumberField(TEXT("at_ms"), 0.0);
	StepObj->SetStringField(TEXT("kind"), TEXT("key"));
	Steps.Add(MakeShared<FJsonValueObject>(StepObj));
	Params->SetArrayField(TEXT("steps"), Steps);

	const FCortexCommandResult Result = Handler.Execute(TEXT("inject_input_sequence"), Params);

	TestFalse(TEXT("inject_input_sequence should fail without PIE"), Result.bSuccess);
	TestEqual(TEXT("Error should be PIE_NOT_ACTIVE"), Result.ErrorCode, TEXT("PIE_NOT_ACTIVE"));

	return true;
}
