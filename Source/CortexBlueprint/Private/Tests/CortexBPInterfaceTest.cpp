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
	Params->SetStringField(TEXT("interface_path"), TEXT("BlendableInterface"));
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
	Params->SetStringField(TEXT("interface_path"), TEXT("BlendableInterface"));
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

// ============================================================================
// RemoveInterface Tests
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveInterfaceBasicTest,
	"Cortex.Blueprint.ClassSettings.RemoveInterface.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveInterfaceBasicTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_RemoveInterfaceBasicTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	// First add an interface
	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	AddParams->SetStringField(TEXT("interface_path"), TEXT("BlendableInterface"));
	AddParams->SetBoolField(TEXT("compile"), false);

	FCortexCommandResult AddResult = FCortexBPClassSettingsOps::AddInterface(AddParams);
	TestTrue(TEXT("Add succeeded"), AddResult.bSuccess);
	TestEqual(TEXT("Interface count is 1"), TestBP->ImplementedInterfaces.Num(), 1);

	// Now remove it
	TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
	RemoveParams->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	RemoveParams->SetStringField(TEXT("interface_path"), TEXT("BlendableInterface"));
	RemoveParams->SetBoolField(TEXT("compile"), true);

	FCortexCommandResult RemoveResult = FCortexBPClassSettingsOps::RemoveInterface(RemoveParams);
	TestTrue(TEXT("RemoveInterface succeeded"), RemoveResult.bSuccess);
	TestEqual(TEXT("Interface count is 0"), TestBP->ImplementedInterfaces.Num(), 0);

	if (RemoveResult.Data.IsValid())
	{
		FString InterfaceName;
		TestTrue(TEXT("Response has interface_name"),
			RemoveResult.Data->TryGetStringField(TEXT("interface_name"), InterfaceName));

		bool bCompiled = false;
		TestTrue(TEXT("Response has compiled"),
			RemoveResult.Data->TryGetBoolField(TEXT("compiled"), bCompiled));
	}

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPRemoveInterfaceNotImplementedTest,
	"Cortex.Blueprint.ClassSettings.RemoveInterface.NotImplemented",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPRemoveInterfaceNotImplementedTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_RemoveInterfaceNotImplTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("interface_path"), TEXT("BlendableInterface"));
	Params->SetBoolField(TEXT("compile"), false);

	FCortexCommandResult Result = FCortexBPClassSettingsOps::RemoveInterface(Params);
	TestFalse(TEXT("Returns failure when interface not implemented"), Result.bSuccess);
	TestEqual(TEXT("Error code is InvalidOperation"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);

	TestBP->MarkAsGarbage();
	return true;
}