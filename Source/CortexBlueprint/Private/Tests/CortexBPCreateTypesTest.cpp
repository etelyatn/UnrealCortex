// Copyright Andrei Sudarikov. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CortexBPCommandHandler.h"
#include "CortexTypes.h"
#include "Misc/Guid.h"

DEFINE_LOG_CATEGORY_STATIC(LogCortexBPTypesTests, Log, All);

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCreateActorTypeTest,
	"Cortex.Blueprint.Create.Types.Actor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPCreateActorTypeTest::RunTest(const FString& Parameters)
{
	// Setup
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString BlueprintName = FString::Printf(TEXT("BP_TestActor_%s"), *Suffix);
	const FString BlueprintDir = FString::Printf(TEXT("/Game/Temp/CortexBPTest_Types_%s"), *Suffix);
	const FString BlueprintPath = FString::Printf(TEXT("%s/%s"), *BlueprintDir, *BlueprintName);
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), BlueprintName);
	Params->SetStringField(TEXT("path"), BlueprintDir);
	Params->SetStringField(TEXT("type"), TEXT("Actor"));

	// Execute
	FCortexCommandResult Result = Handler.Execute(TEXT("create"), Params);

	// Verify
	TestTrue(TEXT("Command should succeed"), Result.bSuccess);

	// Verify asset is loadable
	UBlueprint* LoadedBP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	TestNotNull(TEXT("Asset should be loadable"), LoadedBP);

	if (LoadedBP)
	{
		// Verify parent class is AActor
		UClass* ParentClass = LoadedBP->ParentClass;
		TestNotNull(TEXT("Parent class should exist"), ParentClass);
		if (ParentClass)
		{
			TestTrue(TEXT("Parent class should be or derive from AActor"),
				ParentClass->IsChildOf(AActor::StaticClass()));
		}

		// Cleanup
		LoadedBP->MarkAsGarbage();
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCreateComponentTypeTest,
	"Cortex.Blueprint.Create.Types.Component",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPCreateComponentTypeTest::RunTest(const FString& Parameters)
{
	// Setup
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString BlueprintName = FString::Printf(TEXT("BP_TestComponent_%s"), *Suffix);
	const FString BlueprintDir = FString::Printf(TEXT("/Game/Temp/CortexBPTest_Types_%s"), *Suffix);
	const FString BlueprintPath = FString::Printf(TEXT("%s/%s"), *BlueprintDir, *BlueprintName);
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), BlueprintName);
	Params->SetStringField(TEXT("path"), BlueprintDir);
	Params->SetStringField(TEXT("type"), TEXT("Component"));

	// Execute
	FCortexCommandResult Result = Handler.Execute(TEXT("create"), Params);

	// Verify
	TestTrue(TEXT("Command should succeed"), Result.bSuccess);

	// Verify asset is loadable
	UBlueprint* LoadedBP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	TestNotNull(TEXT("Asset should be loadable"), LoadedBP);

	if (LoadedBP)
	{
		// Verify parent class is UActorComponent
		UClass* ParentClass = LoadedBP->ParentClass;
		TestNotNull(TEXT("Parent class should exist"), ParentClass);
		if (ParentClass)
		{
			TestTrue(TEXT("Parent class should be or derive from UActorComponent"),
				ParentClass->IsChildOf(UActorComponent::StaticClass()));
		}

		// Cleanup
		LoadedBP->MarkAsGarbage();
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCreateFunctionLibraryTypeTest,
	"Cortex.Blueprint.Create.Types.FunctionLibrary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPCreateFunctionLibraryTypeTest::RunTest(const FString& Parameters)
{
	// Setup
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString BlueprintName = FString::Printf(TEXT("BP_TestFunctionLibrary_%s"), *Suffix);
	const FString BlueprintDir = FString::Printf(TEXT("/Game/Temp/CortexBPTest_Types_%s"), *Suffix);
	const FString BlueprintPath = FString::Printf(TEXT("%s/%s"), *BlueprintDir, *BlueprintName);
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), BlueprintName);
	Params->SetStringField(TEXT("path"), BlueprintDir);
	Params->SetStringField(TEXT("type"), TEXT("FunctionLibrary"));

	// Execute
	FCortexCommandResult Result = Handler.Execute(TEXT("create"), Params);

	// Verify
	TestTrue(TEXT("Command should succeed"), Result.bSuccess);

	// Verify asset is loadable
	UBlueprint* LoadedBP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	TestNotNull(TEXT("Asset should be loadable"), LoadedBP);

	if (LoadedBP)
	{
		// Verify parent class is UBlueprintFunctionLibrary
		UClass* ParentClass = LoadedBP->ParentClass;
		TestNotNull(TEXT("Parent class should exist"), ParentClass);
		if (ParentClass)
		{
			TestTrue(TEXT("Parent class should be or derive from UBlueprintFunctionLibrary"),
				ParentClass->IsChildOf(UBlueprintFunctionLibrary::StaticClass()));
		}

		// Cleanup
		LoadedBP->MarkAsGarbage();
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCreateInterfaceTypeTest,
	"Cortex.Blueprint.Create.Types.Interface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPCreateInterfaceTypeTest::RunTest(const FString& Parameters)
{
	// Setup
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString BlueprintName = FString::Printf(TEXT("BP_TestInterface_%s"), *Suffix);
	const FString BlueprintDir = FString::Printf(TEXT("/Game/Temp/CortexBPTest_Types_%s"), *Suffix);
	const FString BlueprintPath = FString::Printf(TEXT("%s/%s"), *BlueprintDir, *BlueprintName);
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), BlueprintName);
	Params->SetStringField(TEXT("path"), BlueprintDir);
	Params->SetStringField(TEXT("type"), TEXT("Interface"));

	// Execute
	FCortexCommandResult Result = Handler.Execute(TEXT("create"), Params);

	// Verify
	TestTrue(TEXT("Command should succeed"), Result.bSuccess);

	// Verify asset is loadable
	UBlueprint* LoadedBP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	TestNotNull(TEXT("Asset should be loadable"), LoadedBP);

	if (LoadedBP)
	{
		// Verify parent class is UInterface
		UClass* ParentClass = LoadedBP->ParentClass;
		TestNotNull(TEXT("Parent class should exist"), ParentClass);
		if (ParentClass)
		{
			TestTrue(TEXT("Parent class should be or derive from UInterface"),
				ParentClass->IsChildOf(UInterface::StaticClass()));
		}

		// Cleanup
		LoadedBP->MarkAsGarbage();
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPCreateWidgetTypeTest,
	"Cortex.Blueprint.Create.Types.Widget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPCreateWidgetTypeTest::RunTest(const FString& Parameters)
{
	// Setup
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString BlueprintName = FString::Printf(TEXT("WBP_TestWidget_%s"), *Suffix);
	const FString BlueprintDir = FString::Printf(TEXT("/Game/Temp/CortexBPTest_Types_%s"), *Suffix);
	const FString BlueprintPath = FString::Printf(TEXT("%s/%s"), *BlueprintDir, *BlueprintName);
	FCortexBPCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), BlueprintName);
	Params->SetStringField(TEXT("path"), BlueprintDir);
	Params->SetStringField(TEXT("type"), TEXT("Widget"));

	// Execute
	FCortexCommandResult Result = Handler.Execute(TEXT("create"), Params);

	// Verify
	TestTrue(TEXT("Command should succeed"), Result.bSuccess);

	// Verify asset is loadable
	UBlueprint* LoadedBP = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	TestNotNull(TEXT("Asset should be loadable"), LoadedBP);

	if (LoadedBP)
	{
		// Verify parent class is UserWidget (UMG)
		UClass* ParentClass = LoadedBP->ParentClass;
		TestNotNull(TEXT("Parent class should exist"), ParentClass);
		if (ParentClass)
		{
			// UserWidget class is dynamically resolved - check by name
			UClass* UserWidgetClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.UserWidget"));
			if (UserWidgetClass)
			{
				TestTrue(TEXT("Parent class should be or derive from UserWidget"),
					ParentClass->IsChildOf(UserWidgetClass));
			}
			else
			{
				AddInfo(TEXT("UserWidget class not available (UMG module may not be loaded) - skipping"));
			}
		}

		// Cleanup
		LoadedBP->MarkAsGarbage();
	}

	return true;
}
