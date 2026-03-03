#include "Misc/AutomationTest.h"
#include "CortexMaterialCommandHandler.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialDynPIEGateTest,
	"Cortex.Material.Dynamic.PIEGateReturnsError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialDynPIEGateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexMaterialCommandHandler Handler;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("actor_path"), TEXT("SomeActor"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("list_dynamic_instances"), Params);
	TestFalse(TEXT("Should fail when PIE not active"), Result.bSuccess);
	TestEqual(TEXT("Error code should be PIE_NOT_ACTIVE"), Result.ErrorCode, TEXT("PIE_NOT_ACTIVE"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialDynCreateNoPIETest,
	"Cortex.Material.Dynamic.CreateDynamicInstanceFailsWithoutPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialDynCreateNoPIETest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexMaterialCommandHandler Handler;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("actor_path"), TEXT("SomeActor"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("create_dynamic_instance"), Params);
	TestFalse(TEXT("Should fail without PIE"), Result.bSuccess);
	TestEqual(TEXT("Error code"), Result.ErrorCode, TEXT("PIE_NOT_ACTIVE"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialDynDestroyNoPIETest,
	"Cortex.Material.Dynamic.DestroyDynamicInstanceFailsWithoutPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialDynDestroyNoPIETest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexMaterialCommandHandler Handler;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("actor_path"), TEXT("SomeActor"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("destroy_dynamic_instance"), Params);
	TestFalse(TEXT("Should fail without PIE"), Result.bSuccess);
	TestEqual(TEXT("Error code"), Result.ErrorCode, TEXT("PIE_NOT_ACTIVE"));
	return true;
}
