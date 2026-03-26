#include "Misc/AutomationTest.h"
#include "Operations/CortexBPClassSettingsOps.h"
#include "CortexCommandRouter.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetReplicationBasicTest,
	"Cortex.Blueprint.ClassSettings.SetReplication.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSetReplicationBasicTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_SetReplicationBasicTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetBoolField(TEXT("replicates"), true);
	Params->SetBoolField(TEXT("replicate_movement"), true);
	Params->SetStringField(TEXT("net_dormancy"), TEXT("DORM_Awake"));
	Params->SetBoolField(TEXT("compile"), true);
	Params->SetBoolField(TEXT("save"), false);

	FCortexCommandResult Result = FCortexBPClassSettingsOps::SetReplicationSettings(Params);
	TestTrue(TEXT("SetReplicationSettings succeeded"), Result.bSuccess);

	FKismetEditorUtilities::CompileBlueprint(TestBP);
	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(TestBP->GeneratedClass);
	TestNotNull(TEXT("GeneratedClass exists"), GeneratedClass);
	if (GeneratedClass)
	{
		AActor* CDO = Cast<AActor>(GeneratedClass->GetDefaultObject(false));
		TestNotNull(TEXT("CDO is AActor"), CDO);
		if (CDO)
		{
			TestTrue(TEXT("bReplicates is true"), CDO->bReplicates);
			TestTrue(TEXT("bReplicateMovement is true"), CDO->bReplicateMovement);
			TestEqual(TEXT("NetDormancy is DORM_Awake"),
				CDO->NetDormancy, ENetDormancy::DORM_Awake);
		}
	}

	if (Result.Data.IsValid())
	{
		bool bReplicates = false;
		TestTrue(TEXT("Response has replicates"),
			Result.Data->TryGetBoolField(TEXT("replicates"), bReplicates));
		TestTrue(TEXT("replicates is true"), bReplicates);

		FString Dormancy;
		TestTrue(TEXT("Response has net_dormancy"),
			Result.Data->TryGetStringField(TEXT("net_dormancy"), Dormancy));
		TestEqual(TEXT("net_dormancy is DORM_Awake"), Dormancy, FString(TEXT("DORM_Awake")));
	}

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetReplicationNonActorTest,
	"Cortex.Blueprint.ClassSettings.SetReplication.NonActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSetReplicationNonActorTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		UActorComponent::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_SetReplicationNonActorTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetBoolField(TEXT("replicates"), true);

	FCortexCommandResult Result = FCortexBPClassSettingsOps::SetReplicationSettings(Params);
	TestFalse(TEXT("Returns failure for non-Actor Blueprint"), Result.bSuccess);
	TestEqual(TEXT("Error code is InvalidBlueprintType"), Result.ErrorCode, CortexErrorCodes::InvalidBlueprintType);

	TestBP->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSetReplicationInvalidDormancyTest,
	"Cortex.Blueprint.ClassSettings.SetReplication.InvalidDormancy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPSetReplicationInvalidDormancyTest::RunTest(const FString& Parameters)
{
	UBlueprint* TestBP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FName(TEXT("BP_SetReplicationBadDormancyTest")),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Test Blueprint created"), TestBP);
	if (!TestBP) { return false; }

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TestBP->GetPathName());
	Params->SetStringField(TEXT("net_dormancy"), TEXT("INVALID_VALUE"));
	Params->SetBoolField(TEXT("compile"), false);

	FCortexCommandResult Result = FCortexBPClassSettingsOps::SetReplicationSettings(Params);
	TestFalse(TEXT("Returns failure for invalid dormancy"), Result.bSuccess);
	TestEqual(TEXT("Error code is InvalidValue"), Result.ErrorCode, CortexErrorCodes::InvalidValue);

	TestBP->MarkAsGarbage();
	return true;
}
