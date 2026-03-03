#include "Misc/AutomationTest.h"
#include "CortexEditorUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorUtilsGetPIEWorldNullTest,
	"Cortex.Editor.Utils.GetPIEWorldReturnsNullWhenNotPlaying",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorUtilsGetPIEWorldNullTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UWorld* World = FCortexEditorUtils::GetPIEWorld();
	TestNull(TEXT("PIE world should be null when not playing"), World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorUtilsFindActorNullWorldTest,
	"Cortex.Editor.Utils.FindActorInPIEReturnsNullForNullWorld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorUtilsFindActorNullWorldTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	AActor* Actor = FCortexEditorUtils::FindActorInPIE(nullptr, TEXT("SomeActor"));
	TestNull(TEXT("Should return null for null world"), Actor);

	Actor = FCortexEditorUtils::FindActorInPIE(nullptr, TEXT(""));
	TestNull(TEXT("Should return null for empty identifier"), Actor);

	return true;
}
