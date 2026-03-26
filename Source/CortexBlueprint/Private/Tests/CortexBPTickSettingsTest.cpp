#include "Misc/AutomationTest.h"
#include "Operations/CortexBPClassSettingsOps.h"
#include "CortexCommandRouter.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetTickSettingsBasicTest,
	"Cortex.Blueprint.ClassSettings.SetTickSettings.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSetTickSettingsBasicTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_SetTickBasicTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetBoolField(TEXT("start_with_tick_enabled"), true);
	Params->SetNumberField(TEXT("tick_interval"), 0.5);
	Params->SetBoolField(TEXT("compile"), true);
	Params->SetBoolField(TEXT("save"), false);

	FCortexCommandResult Result = FCortexBPClassSettingsOps::SetTickSettings(Params);
	TestTrue(TEXT("SetTickSettings succeeded"), Result.bSuccess);

	FKismetEditorUtilities::CompileBlueprint(TestBP);
	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(TestBP->GeneratedClass);
	TestNotNull(TEXT("GeneratedClass exists"), GeneratedClass);
	if (GeneratedClass)
	{
		AActor* CDO = Cast<AActor>(GeneratedClass->GetDefaultObject(false));
		TestNotNull(TEXT("CDO is AActor"), CDO);
		if (CDO)
		{
			TestTrue(TEXT("bCanEverTick is true"), CDO->PrimaryActorTick.bCanEverTick);
			TestTrue(TEXT("bStartWithTickEnabled is true"), CDO->PrimaryActorTick.bStartWithTickEnabled);
			TestEqual(TEXT("TickInterval is 0.5"), CDO->PrimaryActorTick.TickInterval, 0.5f);
		}
	}

	if (Result.Data.IsValid())
	{
		bool bTickEnabled = false;
		TestTrue(TEXT("Response has start_with_tick_enabled"),
			Result.Data->TryGetBoolField(TEXT("start_with_tick_enabled"), bTickEnabled));
		TestTrue(TEXT("start_with_tick_enabled is true"), bTickEnabled);

		double TickInterval = 0.0;
		TestTrue(TEXT("Response has tick_interval"),
			Result.Data->TryGetNumberField(TEXT("tick_interval"), TickInterval));
		TestEqual(TEXT("tick_interval is 0.5"), TickInterval, 0.5);
	}

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetTickSettingsNonActorTest,
	"Cortex.Blueprint.ClassSettings.SetTickSettings.NonActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSetTickSettingsNonActorTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		UActorComponent::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_SetTickNonActorTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetBoolField(TEXT("start_with_tick_enabled"), true);

	FCortexCommandResult Result = FCortexBPClassSettingsOps::SetTickSettings(Params);
	TestFalse(TEXT("Returns failure for non-Actor Blueprint"), Result.bSuccess);
	TestEqual(TEXT("Error code is InvalidBlueprintType"), Result.ErrorCode, CortexErrorCodes::InvalidBlueprintType);

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetTickSettingsPartialTest,
	"Cortex.Blueprint.ClassSettings.SetTickSettings.Partial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSetTickSettingsPartialTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_SetTickPartialTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetNumberField(TEXT("tick_interval"), 0.25);
	Params->SetBoolField(TEXT("compile"), true);
	Params->SetBoolField(TEXT("save"), false);

	FCortexCommandResult Result = FCortexBPClassSettingsOps::SetTickSettings(Params);
	TestTrue(TEXT("SetTickSettings with partial params succeeded"), Result.bSuccess);

	FKismetEditorUtilities::CompileBlueprint(TestBP);
	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(TestBP->GeneratedClass);
	if (GeneratedClass)
	{
		AActor* CDO = Cast<AActor>(GeneratedClass->GetDefaultObject(false));
		if (CDO)
		{
			TestEqual(TEXT("TickInterval is 0.25"), CDO->PrimaryActorTick.TickInterval, 0.25f);
		}
	}

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetTickSettingsDisableTest,
	"Cortex.Blueprint.ClassSettings.SetTickSettings.Disable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSetTickSettingsDisableTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_SetTickDisableTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	TSharedPtr<FJsonObject> EnableParams = MakeShared<FJsonObject>();
	EnableParams->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	EnableParams->SetBoolField(TEXT("start_with_tick_enabled"), true);
	EnableParams->SetBoolField(TEXT("compile"), true);
	EnableParams->SetBoolField(TEXT("save"), false);

	FCortexCommandResult EnableResult = FCortexBPClassSettingsOps::SetTickSettings(EnableParams);
	TestTrue(TEXT("Enable tick succeeded"), EnableResult.bSuccess);

	TSharedPtr<FJsonObject> DisableParams = MakeShared<FJsonObject>();
	DisableParams->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	DisableParams->SetBoolField(TEXT("start_with_tick_enabled"), false);
	DisableParams->SetBoolField(TEXT("compile"), true);
	DisableParams->SetBoolField(TEXT("save"), false);

	FCortexCommandResult DisableResult = FCortexBPClassSettingsOps::SetTickSettings(DisableParams);
	TestTrue(TEXT("Disable tick succeeded"), DisableResult.bSuccess);

	FKismetEditorUtilities::CompileBlueprint(TestBP);
	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(TestBP->GeneratedClass);
	if (GeneratedClass)
	{
		AActor* CDO = Cast<AActor>(GeneratedClass->GetDefaultObject(false));
		if (CDO)
		{
			TestFalse(TEXT("bStartWithTickEnabled is false"), CDO->PrimaryActorTick.bStartWithTickEnabled);
			TestTrue(TEXT("bCanEverTick still true"), CDO->PrimaryActorTick.bCanEverTick);
		}
	}

	TestBP->MarkAsGarbage();
	return true;
}
