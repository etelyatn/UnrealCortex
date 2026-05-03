#include "Misc/AutomationTest.h"
#include "CortexLogCapture.h"

namespace
{
void EmitWarningForCapture(FCortexLogCapture& Capture, const TCHAR* Message)
{
	Capture.Serialize(Message, ELogVerbosity::Warning, FName(TEXT("LogCore")));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLogCaptureMatchTest,
	"Cortex.Core.LogCapture.CapturesInstancedStructWarning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLogCaptureMatchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexLogCapture Capture;

	EmitWarningForCapture(
		Capture,
		TEXT("Unable to find serialized UScriptStruct -> Advance 37 bytes in the archive and reset to empty FInstancedStruct. SerializedProperty:/Script/TestModule.FTestStruct:Items.Items LinkerRoot:/Game/Test/DA_Test"));

	TestEqual(TEXT("Should capture one warning"), Capture.GetWarnings().Num(), 1);
	TestTrue(
		TEXT("Warning should contain the expected message"),
		Capture.GetWarnings()[0].Contains(TEXT("Unable to find serialized UScriptStruct")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLogCaptureIgnoreUnrelatedTest,
	"Cortex.Core.LogCapture.IgnoresUnrelatedWarnings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLogCaptureIgnoreUnrelatedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexLogCapture Capture;

	EmitWarningForCapture(Capture, TEXT("Some other warning that should not be captured"));
	EmitWarningForCapture(Capture, TEXT("A second unrelated warning"));

	TestEqual(TEXT("Should capture zero warnings"), Capture.GetWarnings().Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLogCaptureFilterTest,
	"Cortex.Core.LogCapture.FiltersWarningsByAssetPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLogCaptureFilterTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexLogCapture Capture;

	EmitWarningForCapture(
		Capture,
		TEXT("Unable to find serialized UScriptStruct -> Advance 37 bytes in the archive and reset to empty FInstancedStruct. SerializedProperty:/Script/M.S:F.F LinkerRoot:/Game/Test/DA_Target"));
	EmitWarningForCapture(
		Capture,
		TEXT("Unable to find serialized UScriptStruct -> Advance 98 bytes in the archive and reset to empty FInstancedStruct. SerializedProperty:/Script/M.S:F.F LinkerRoot:/Game/Test/DA_Other"));

	TestEqual(TEXT("Total warnings should be 2"), Capture.GetWarnings().Num(), 2);
	TestEqual(
		TEXT("Filtered to DA_Target should be 1"),
		Capture.GetWarnings(TEXT("/Game/Test/DA_Target")).Num(),
		1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLogCaptureScopeTest,
	"Cortex.Core.LogCapture.StopsCaptureAfterDestruction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLogCaptureScopeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	TArray<FString> CapturedDuringScope;
	{
		FCortexLogCapture Capture;
		EmitWarningForCapture(
			Capture,
			TEXT("Unable to find serialized UScriptStruct -> Advance 37 bytes in the archive and reset to empty FInstancedStruct. SerializedProperty:/Script/M.S:F LinkerRoot:/Game/Test/X"));
		CapturedDuringScope = Capture.GetWarnings();
	}

	TestEqual(TEXT("Should have captured 1 warning during scope"), CapturedDuringScope.Num(), 1);
	return true;
}
