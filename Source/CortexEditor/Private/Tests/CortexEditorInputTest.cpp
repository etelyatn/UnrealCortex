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
	StepObj->SetStringField(TEXT("key"), TEXT("W"));
	StepObj->SetStringField(TEXT("action"), TEXT("tap"));
	Steps.Add(MakeShared<FJsonValueObject>(StepObj));
	Params->SetArrayField(TEXT("steps"), Steps);

	FDeferredResponseCallback Callback = [](FCortexCommandResult) {};
	const FCortexCommandResult Result = Handler.Execute(
		TEXT("inject_input_sequence"), Params, MoveTemp(Callback));

	TestFalse(TEXT("inject_input_sequence should fail without PIE"), Result.bSuccess);
	TestEqual(TEXT("Error should be PIE_NOT_ACTIVE"), Result.ErrorCode, TEXT("PIE_NOT_ACTIVE"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInjectKeyMissingKeyTest,
	"Cortex.Editor.Input.InjectKey.ErrorWhenMissingKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInjectKeyMissingKeyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	const FCortexCommandResult Result = Handler.Execute(TEXT("inject_key"), Params);

	TestFalse(TEXT("inject_key should fail without key param"), Result.bSuccess);
	TestEqual(TEXT("Error should be INVALID_FIELD"), Result.ErrorCode, TEXT("INVALID_FIELD"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInjectKeyInvalidKeyNameTest,
	"Cortex.Editor.Input.InjectKey.ErrorWhenInvalidKeyName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInjectKeyInvalidKeyNameTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("key"), TEXT("NotARealKey"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("inject_key"), Params);

	TestFalse(TEXT("inject_key should fail with invalid key name"), Result.bSuccess);
	TestEqual(TEXT("Error should be INVALID_FIELD"), Result.ErrorCode, TEXT("INVALID_FIELD"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInjectKeyInvalidActionTest,
	"Cortex.Editor.Input.InjectKey.ErrorWhenInvalidAction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInjectKeyInvalidActionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("key"), TEXT("W"));
	Params->SetStringField(TEXT("action"), TEXT("invalid_action"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("inject_key"), Params);

	TestFalse(TEXT("inject_key should fail with invalid action"), Result.bSuccess);
	TestEqual(TEXT("Error should be INVALID_FIELD"), Result.ErrorCode, TEXT("INVALID_FIELD"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInjectMouseMissingActionTest,
	"Cortex.Editor.Input.InjectMouse.ErrorWhenMissingAction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInjectMouseMissingActionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	const FCortexCommandResult Result = Handler.Execute(TEXT("inject_mouse"), Params);
	TestFalse(TEXT("inject_mouse should fail without action"), Result.bSuccess);
	TestEqual(TEXT("Error should be INVALID_FIELD"), Result.ErrorCode, TEXT("INVALID_FIELD"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInjectMouseInvalidButtonTest,
	"Cortex.Editor.Input.InjectMouse.ErrorWhenInvalidButton",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInjectMouseInvalidButtonTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("action"), TEXT("click"));
	Params->SetStringField(TEXT("button"), TEXT("invalid_button"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("inject_mouse"), Params);
	TestFalse(TEXT("inject_mouse should fail with invalid button"), Result.bSuccess);
	TestEqual(TEXT("Error should be INVALID_FIELD"), Result.ErrorCode, TEXT("INVALID_FIELD"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInjectMouseScrollMissingDeltaTest,
	"Cortex.Editor.Input.InjectMouse.ErrorWhenScrollMissingDelta",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInjectMouseScrollMissingDeltaTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("action"), TEXT("scroll"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("inject_mouse"), Params);
	TestFalse(TEXT("inject_mouse scroll should fail without delta"), Result.bSuccess);
	TestEqual(TEXT("Error should be INVALID_FIELD"), Result.ErrorCode, TEXT("INVALID_FIELD"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInjectInputActionMissingNameTest,
	"Cortex.Editor.Input.InjectInputAction.ErrorWhenMissingActionName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInjectInputActionMissingNameTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	const FCortexCommandResult Result = Handler.Execute(TEXT("inject_input_action"), Params);
	TestFalse(TEXT("inject_input_action should fail without action_name"), Result.bSuccess);
	TestEqual(TEXT("Error should be INVALID_FIELD"), Result.ErrorCode, TEXT("INVALID_FIELD"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorInjectSequenceInvalidKindTest,
	"Cortex.Editor.Input.InjectInputSequence.ErrorWhenInvalidStepKind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorInjectSequenceInvalidKindTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexEditorCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Steps;
	TSharedPtr<FJsonObject> StepObj = MakeShared<FJsonObject>();
	StepObj->SetNumberField(TEXT("at_ms"), 0.0);
	StepObj->SetStringField(TEXT("kind"), TEXT("invalid_kind"));
	Steps.Add(MakeShared<FJsonValueObject>(StepObj));
	Params->SetArrayField(TEXT("steps"), Steps);

	FDeferredResponseCallback Callback = [](FCortexCommandResult) {};
	const FCortexCommandResult Result = Handler.Execute(
		TEXT("inject_input_sequence"), Params, MoveTemp(Callback));

	TestFalse(TEXT("inject_input_sequence should fail with invalid kind"), Result.bSuccess);
	TestEqual(TEXT("Error should be INVALID_FIELD"), Result.ErrorCode, TEXT("INVALID_FIELD"));

	return true;
}
