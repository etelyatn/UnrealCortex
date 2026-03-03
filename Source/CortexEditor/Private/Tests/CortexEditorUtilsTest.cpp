#include "Misc/AutomationTest.h"
#include "CortexPIEUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexPIEUtilsGetPIEWorldNullTest,
	"Cortex.Editor.Utils.GetPIEWorldReturnsNullWhenNotPlaying",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexPIEUtilsGetPIEWorldNullTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UWorld* World = FCortexPIEUtils::GetPIEWorld();
	TestNull(TEXT("PIE world should be null when not playing"), World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexPIEUtilsFindActorNullWorldTest,
	"Cortex.Editor.Utils.FindActorInPIEReturnsNullForNullWorld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexPIEUtilsFindActorNullWorldTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	AActor* Actor = FCortexPIEUtils::FindActorInPIE(nullptr, TEXT("SomeActor"));
	TestNull(TEXT("Should return null for null world"), Actor);

	Actor = FCortexPIEUtils::FindActorInPIE(nullptr, TEXT(""));
	TestNull(TEXT("Should return null for empty identifier"), Actor);

	return true;
}
