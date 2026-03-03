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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialDynGetInstanceNoPIETest,
	"Cortex.Material.Dynamic.GetDynamicInstanceFailsWithoutPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialDynGetInstanceNoPIETest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexMaterialCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("actor_path"), TEXT("SomeActor"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("get_dynamic_instance"), Params);
	TestFalse(TEXT("Should fail without PIE"), Result.bSuccess);
	TestEqual(TEXT("Error code"), Result.ErrorCode, TEXT("PIE_NOT_ACTIVE"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialDynSetParamNoPIETest,
	"Cortex.Material.Dynamic.SetDynamicParameterFailsWithoutPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialDynSetParamNoPIETest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexMaterialCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("actor_path"), TEXT("SomeActor"));
	Params->SetStringField(TEXT("name"), TEXT("Roughness"));
	Params->SetStringField(TEXT("type"), TEXT("scalar"));
	Params->SetNumberField(TEXT("value"), 0.5);

	const FCortexCommandResult Result = Handler.Execute(TEXT("set_dynamic_parameter"), Params);
	TestFalse(TEXT("Should fail without PIE"), Result.bSuccess);
	TestEqual(TEXT("Error code"), Result.ErrorCode, TEXT("PIE_NOT_ACTIVE"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialDynGetParamNoPIETest,
	"Cortex.Material.Dynamic.GetDynamicParameterFailsWithoutPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialDynGetParamNoPIETest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexMaterialCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("actor_path"), TEXT("SomeActor"));
	Params->SetStringField(TEXT("name"), TEXT("Roughness"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("get_dynamic_parameter"), Params);
	TestFalse(TEXT("Should fail without PIE"), Result.bSuccess);
	TestEqual(TEXT("Error code"), Result.ErrorCode, TEXT("PIE_NOT_ACTIVE"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialDynListParamsNoPIETest,
	"Cortex.Material.Dynamic.ListDynamicParametersFailsWithoutPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialDynListParamsNoPIETest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexMaterialCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("actor_path"), TEXT("SomeActor"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("list_dynamic_parameters"), Params);
	TestFalse(TEXT("Should fail without PIE"), Result.bSuccess);
	TestEqual(TEXT("Error code"), Result.ErrorCode, TEXT("PIE_NOT_ACTIVE"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialDynSetParamsBatchNoPIETest,
	"Cortex.Material.Dynamic.SetDynamicParametersBatchFailsWithoutPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialDynSetParamsBatchNoPIETest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexMaterialCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("actor_path"), TEXT("SomeActor"));
	Params->SetArrayField(TEXT("parameters"), TArray<TSharedPtr<FJsonValue>>());

	const FCortexCommandResult Result = Handler.Execute(TEXT("set_dynamic_parameters"), Params);
	TestFalse(TEXT("Should fail without PIE"), Result.bSuccess);
	TestEqual(TEXT("Error code"), Result.ErrorCode, TEXT("PIE_NOT_ACTIVE"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMaterialDynResetParamNoPIETest,
	"Cortex.Material.Dynamic.ResetDynamicParameterFailsWithoutPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMaterialDynResetParamNoPIETest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexMaterialCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("actor_path"), TEXT("SomeActor"));
	Params->SetStringField(TEXT("name"), TEXT("Roughness"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("reset_dynamic_parameter"), Params);
	TestFalse(TEXT("Should fail without PIE"), Result.bSuccess);
	TestEqual(TEXT("Error code"), Result.ErrorCode, TEXT("PIE_NOT_ACTIVE"));
	return true;
}
