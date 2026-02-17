#include "Misc/AutomationTest.h"
#include "CortexTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexLevelErrorCodesTest,
	"Cortex.Core.ErrorCodes.LevelDomain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexLevelErrorCodesTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("ACTOR_NOT_FOUND code should match"), CortexErrorCodes::ActorNotFound, TEXT("ACTOR_NOT_FOUND"));
	TestEqual(TEXT("AMBIGUOUS_ACTOR code should match"), CortexErrorCodes::AmbiguousActor, TEXT("AMBIGUOUS_ACTOR"));
	TestEqual(TEXT("CLASS_NOT_FOUND code should match"), CortexErrorCodes::ClassNotFound, TEXT("CLASS_NOT_FOUND"));
	TestEqual(TEXT("COMPONENT_NOT_FOUND code should match"), CortexErrorCodes::ComponentNotFound, TEXT("COMPONENT_NOT_FOUND"));
	TestEqual(TEXT("COMPONENT_REMOVE_DENIED code should match"), CortexErrorCodes::ComponentRemoveDenied, TEXT("COMPONENT_REMOVE_DENIED"));
	TestEqual(TEXT("PROPERTY_NOT_FOUND code should match"), CortexErrorCodes::PropertyNotFound, TEXT("PROPERTY_NOT_FOUND"));
	TestEqual(TEXT("SUBLEVEL_NOT_FOUND code should match"), CortexErrorCodes::SublevelNotFound, TEXT("SUBLEVEL_NOT_FOUND"));
	TestEqual(TEXT("DATA_LAYER_NOT_FOUND code should match"), CortexErrorCodes::DataLayerNotFound, TEXT("DATA_LAYER_NOT_FOUND"));
	TestEqual(TEXT("SPAWN_FAILED code should match"), CortexErrorCodes::SpawnFailed, TEXT("SPAWN_FAILED"));
	return true;
}
