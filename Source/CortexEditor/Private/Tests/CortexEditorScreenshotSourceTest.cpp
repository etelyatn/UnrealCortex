#include "Misc/AutomationTest.h"
#include "CortexEditorCommandHandler.h"
#include "CortexTypes.h"
#include "Editor.h"

/** `capture_screenshot` validates and publishes its `source` selector. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorCaptureScreenshotSourceDeclaredTest,
	"Cortex.Editor.CaptureScreenshot.SourceDeclared",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorCaptureScreenshotSourceDeclaredTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;
	const TArray<FCortexCommandInfo> Commands = Handler.GetSupportedCommands();
	const FCortexCommandInfo* Capture = Commands.FindByPredicate(
		[](const FCortexCommandInfo& Command) { return Command.Name == TEXT("capture_screenshot"); });
	if (!TestNotNull(TEXT("capture_screenshot is declared"), Capture))
	{
		return false;
	}

	const FCortexParamInfo* Source = Capture->Params.FindByPredicate(
		[](const FCortexParamInfo& Param) { return Param.Name == TEXT("source"); });
	if (TestNotNull(TEXT("capture_screenshot declares source"), Source))
	{
		TestFalse(TEXT("source is optional"), Source->bRequired);
		for (const TCHAR* Value : { TEXT("auto"), TEXT("editor"), TEXT("pie") })
		{
			TestTrue(*FString::Printf(TEXT("source description lists %s"), Value), Source->Description.Contains(Value));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorCaptureScreenshotSourceRejectsUnknownTest,
	"Cortex.Editor.CaptureScreenshot.SourceRejectsUnknown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorCaptureScreenshotSourceRejectsUnknownTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexEditorCommandHandler Handler;

	const TSharedPtr<FJsonObject> Bad = MakeShared<FJsonObject>();
	Bad->SetStringField(TEXT("source"), TEXT("game"));
	const FCortexCommandResult BadResult = Handler.Execute(TEXT("capture_screenshot"), Bad);
	TestFalse(TEXT("an unknown source is refused"), BadResult.bSuccess);
	TestEqual(TEXT("refusal code"), BadResult.ErrorCode, FString(TEXT("INVALID_FIELD")));
	TestTrue(TEXT("the refusal names the valid values"),
		BadResult.ErrorMessage.Contains(TEXT("auto")) && BadResult.ErrorMessage.Contains(TEXT("editor")) && BadResult.ErrorMessage.Contains(TEXT("pie")));

	const TSharedPtr<FJsonObject> Number = MakeShared<FJsonObject>();
	Number->SetNumberField(TEXT("source"), 1);
	const FCortexCommandResult NumberResult = Handler.Execute(TEXT("capture_screenshot"), Number);
	TestFalse(TEXT("a non-string source is refused"), NumberResult.bSuccess);
	TestEqual(TEXT("non-string refusal code"), NumberResult.ErrorCode, FString(TEXT("INVALID_FIELD")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorCaptureScreenshotSourcePIERequiresPIETest,
	"Cortex.Editor.CaptureScreenshot.SourcePIERequiresPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorCaptureScreenshotSourcePIERequiresPIETest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (GEditor == nullptr || GEditor->PlayWorld != nullptr)
	{
		AddError(TEXT("The source=pie refusal test requires an editor with no PIE session"));
		return false;
	}

	FCortexEditorCommandHandler Handler;
	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("source"), TEXT("pie"));
	const FCortexCommandResult Result = Handler.Execute(TEXT("capture_screenshot"), Params);
	TestFalse(TEXT("source=pie without PIE is refused"), Result.bSuccess);
	TestEqual(TEXT("refusal code"), Result.ErrorCode, FString(TEXT("PIE_NOT_ACTIVE")));
	return true;
}
