#include "Misc/AutomationTest.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "CortexBPCommandHandler.h"
#include "CortexTypes.h"
#include "Misc/Guid.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCreateCustomParentActorTest,
	"Cortex.Blueprint.Create.CustomParent.Actor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPCreateCustomParentActorTest::RunTest(const FString& Parameters)
{
	// Setup — use short class name
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString BlueprintName = FString::Printf(TEXT("BP_TestCustomParent_%s"), *Suffix);
	const FString BlueprintDir = FString::Printf(TEXT("/Game/Temp/CortexBPTest_CustomParent_%s"), *Suffix);
	const FString BlueprintPath = FString::Printf(TEXT("%s/%s"), *BlueprintDir, *BlueprintName);
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), BlueprintName);
	Params->SetStringField(TEXT("path"), BlueprintDir);
	Params->SetStringField(TEXT("parent_class"), TEXT("CortexBenchmarkActor"));

	// Execute
	FCortexCommandResult Result = Handler.Execute(TEXT("create"), Params);

	// Verify
	TestTrue(TEXT("Command should succeed"), Result.bSuccess);
	TestTrue(TEXT("Result should have data"), Result.Data.IsValid());

	if (Result.Data.IsValid())
	{
		FString ParentClassName;
		Result.Data->TryGetStringField(TEXT("parent_class"), ParentClassName);
		TestEqual(TEXT("Parent class should be CortexBenchmarkActor"), ParentClassName, TEXT("CortexBenchmarkActor"));
	}

	// Verify asset is loadable and has correct parent
	UBlueprint* LoadedBP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	TestNotNull(TEXT("Asset should be loadable"), LoadedBP);

	if (LoadedBP)
	{
		UClass* ParentClass = LoadedBP->ParentClass;
		TestNotNull(TEXT("Parent class should exist"), ParentClass);
		if (ParentClass)
		{
			TestTrue(TEXT("Parent class should derive from AActor"),
				ParentClass->IsChildOf(AActor::StaticClass()));
			TestEqual(TEXT("Parent class name should match"),
				ParentClass->GetName(), TEXT("CortexBenchmarkActor"));
		}

		// Cleanup
		LoadedBP->MarkAsGarbage();
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCreateCustomParentFullPathTest,
	"Cortex.Blueprint.Create.CustomParent.FullPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPCreateCustomParentFullPathTest::RunTest(const FString& Parameters)
{
	// Setup — use full script path
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString BlueprintName = FString::Printf(TEXT("BP_TestCustomParentFP_%s"), *Suffix);
	const FString BlueprintDir = FString::Printf(TEXT("/Game/Temp/CortexBPTest_CustomParent_%s"), *Suffix);
	const FString BlueprintPath = FString::Printf(TEXT("%s/%s"), *BlueprintDir, *BlueprintName);
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), BlueprintName);
	Params->SetStringField(TEXT("path"), BlueprintDir);
	Params->SetStringField(TEXT("parent_class"), TEXT("/Script/CortexSandbox.CortexBenchmarkActor"));

	// Execute
	FCortexCommandResult Result = Handler.Execute(TEXT("create"), Params);

	// Verify
	TestTrue(TEXT("Command should succeed"), Result.bSuccess);

	// Verify asset has correct parent
	UBlueprint* LoadedBP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	TestNotNull(TEXT("Asset should be loadable"), LoadedBP);

	if (LoadedBP)
	{
		UClass* ParentClass = LoadedBP->ParentClass;
		TestNotNull(TEXT("Parent class should exist"), ParentClass);
		if (ParentClass)
		{
			TestTrue(TEXT("Parent class should derive from AActor"),
				ParentClass->IsChildOf(AActor::StaticClass()));
			TestEqual(TEXT("Parent class name should match"),
				ParentClass->GetName(), TEXT("CortexBenchmarkActor"));
		}

		// Cleanup
		LoadedBP->MarkAsGarbage();
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCreateCustomParentInvalidTest,
	"Cortex.Blueprint.Create.CustomParent.Invalid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPCreateCustomParentInvalidTest::RunTest(const FString& Parameters)
{
	// Setup — use invalid class name
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString BlueprintName = FString::Printf(TEXT("BP_TestCustomParentInv_%s"), *Suffix);
	const FString BlueprintDir = FString::Printf(TEXT("/Game/Temp/CortexBPTest_CustomParent_%s"), *Suffix);
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), BlueprintName);
	Params->SetStringField(TEXT("path"), BlueprintDir);
	Params->SetStringField(TEXT("parent_class"), TEXT("NonExistentClass_ZZZZZ"));

	// Execute
	FCortexCommandResult Result = Handler.Execute(TEXT("create"), Params);

	// Verify — should fail with InvalidParentClass
	TestFalse(TEXT("Command should fail"), Result.bSuccess);
	TestEqual(TEXT("Error code should be InvalidParentClass"),
		Result.ErrorCode, CortexErrorCodes::InvalidParentClass);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCreateCustomParentOverridesTypeTest,
	"Cortex.Blueprint.Create.CustomParent.OverridesType",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPCreateCustomParentOverridesTypeTest::RunTest(const FString& Parameters)
{
	// Setup — provide both type and parent_class; parent_class should win
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString BlueprintName = FString::Printf(TEXT("BP_TestCustomParentOvr_%s"), *Suffix);
	const FString BlueprintDir = FString::Printf(TEXT("/Game/Temp/CortexBPTest_CustomParent_%s"), *Suffix);
	const FString BlueprintPath = FString::Printf(TEXT("%s/%s"), *BlueprintDir, *BlueprintName);
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), BlueprintName);
	Params->SetStringField(TEXT("path"), BlueprintDir);
	Params->SetStringField(TEXT("type"), TEXT("Component")); // Should be overridden
	Params->SetStringField(TEXT("parent_class"), TEXT("CortexBenchmarkActor"));

	// Execute
	FCortexCommandResult Result = Handler.Execute(TEXT("create"), Params);

	// Verify — parent_class wins over type
	TestTrue(TEXT("Command should succeed"), Result.bSuccess);

	UBlueprint* LoadedBP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	TestNotNull(TEXT("Asset should be loadable"), LoadedBP);

	if (LoadedBP)
	{
		UClass* ParentClass = LoadedBP->ParentClass;
		TestNotNull(TEXT("Parent class should exist"), ParentClass);
		if (ParentClass)
		{
			// parent_class should override type — so it should be CortexBenchmarkActor (Actor), not Component
			TestTrue(TEXT("Parent class should derive from AActor (not Component)"),
				ParentClass->IsChildOf(AActor::StaticClass()));
			TestEqual(TEXT("Parent class name should be CortexBenchmarkActor"),
				ParentClass->GetName(), TEXT("CortexBenchmarkActor"));
		}

		// Cleanup
		LoadedBP->MarkAsGarbage();
	}

	return true;
}
