#include "Misc/AutomationTest.h"
#include "Operations/CortexBPClassSettingsOps.h"
#include "CortexCommandRouter.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"

// ============================================================================
// AddInterface Tests
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAddInterfaceBasicTest,
	"Cortex.Blueprint.ClassSettings.AddInterface.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAddInterfaceBasicTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_AddInterfaceBasicTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	const int32 InterfaceCountBefore = TestBP->ImplementedInterfaces.Num();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("interface_path"), TEXT("ActorTickableInterface"));
	Params->SetBoolField(TEXT("compile"), true);

	FCortexCommandResult Result = FCortexBPClassSettingsOps::AddInterface(Params);
	TestTrue(TEXT("AddInterface succeeded"), Result.bSuccess);

	// Verify interface was added
	TestEqual(TEXT("Interface count increased by 1"),
		TestBP->ImplementedInterfaces.Num(), InterfaceCountBefore + 1);

	// Verify response fields
	if (Result.Data.IsValid())
	{
		FString InterfaceName;
		TestTrue(TEXT("Response has interface_name"),
			Result.Data->TryGetStringField(TEXT("interface_name"), InterfaceName));
		TestFalse(TEXT("interface_name not empty"), InterfaceName.IsEmpty());

		bool bCompiled = false;
		TestTrue(TEXT("Response has compiled"),
			Result.Data->TryGetBoolField(TEXT("compiled"), bCompiled));
	}

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAddInterfaceDuplicateTest,
	"Cortex.Blueprint.ClassSettings.AddInterface.Duplicate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAddInterfaceDuplicateTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_AddInterfaceDupTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("interface_path"), TEXT("ActorTickableInterface"));
	Params->SetBoolField(TEXT("compile"), false);

	FCortexCommandResult Result1 = FCortexBPClassSettingsOps::AddInterface(Params);
	TestTrue(TEXT("First add succeeded"), Result1.bSuccess);

	FCortexCommandResult Result2 = FCortexBPClassSettingsOps::AddInterface(Params);
	TestFalse(TEXT("Duplicate add fails"), Result2.bSuccess);
	TestEqual(TEXT("Error code is InvalidOperation"), Result2.ErrorCode, CortexErrorCodes::InvalidOperation);

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAddInterfaceInvalidClassTest,
	"Cortex.Blueprint.ClassSettings.AddInterface.InvalidClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAddInterfaceInvalidClassTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_AddInterfaceInvalidTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("interface_path"), TEXT("NonExistentInterface"));
	Params->SetBoolField(TEXT("compile"), false);

	FCortexCommandResult Result = FCortexBPClassSettingsOps::AddInterface(Params);
	TestFalse(TEXT("Returns failure for invalid interface"), Result.bSuccess);
	TestEqual(TEXT("Error code is ClassNotFound"), Result.ErrorCode, CortexErrorCodes::ClassNotFound);

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPAddInterfaceNotInterfaceTest,
	"Cortex.Blueprint.ClassSettings.AddInterface.NotAnInterface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPAddInterfaceNotInterfaceTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_AddInterfaceNotIfaceTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("interface_path"), TEXT("Actor"));
	Params->SetBoolField(TEXT("compile"), false);

	FCortexCommandResult Result = FCortexBPClassSettingsOps::AddInterface(Params);
	TestFalse(TEXT("Returns failure for non-interface class"), Result.bSuccess);
	TestEqual(TEXT("Error code is InvalidOperation"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);

	TestBP->MarkAsGarbage();
	return true;
}
