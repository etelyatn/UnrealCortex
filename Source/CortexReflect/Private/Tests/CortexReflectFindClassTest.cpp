#include "Misc/AutomationTest.h"
#include "CortexReflectCommandHandler.h"
#include "CortexTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectFindClassCppTest,
	"Cortex.Reflect.FindClass.CppClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectFindClassCppTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class_name"), TEXT("AActor"));

	FCortexCommandResult Result = Handler.Execute(TEXT("class_detail"), Params);

	TestTrue(TEXT("class_detail for AActor should succeed"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		FString Name;
		Result.Data->TryGetStringField(TEXT("name"), Name);
		TestEqual(TEXT("Name should be AActor"), Name, TEXT("AActor"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectFindClassCppNoPrefix,
	"Cortex.Reflect.FindClass.CppClassNoPrefix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectFindClassCppNoPrefix::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class_name"), TEXT("Actor"));

	FCortexCommandResult Result = Handler.Execute(TEXT("class_detail"), Params);

	TestTrue(TEXT("class_detail for Actor should succeed"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		FString Name;
		Result.Data->TryGetStringField(TEXT("name"), Name);
		TestEqual(TEXT("Name should be AActor"), Name, TEXT("AActor"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectFindClassBlueprintTest,
	"Cortex.Reflect.FindClass.BlueprintAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectFindClassBlueprintTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class_name"), TEXT("/Game/Blueprints/BP_SimpleActor"));

	FCortexCommandResult Result = Handler.Execute(TEXT("class_detail"), Params);

	TestTrue(TEXT("class_detail for BP_SimpleActor should succeed"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		FString Type;
		Result.Data->TryGetStringField(TEXT("type"), Type);
		TestEqual(TEXT("Type should be blueprint"), Type, TEXT("blueprint"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectFindClassNotFoundTest,
	"Cortex.Reflect.FindClass.NotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectFindClassNotFoundTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("class_name"), TEXT("ADoesNotExistClass"));

	FCortexCommandResult Result = Handler.Execute(TEXT("class_detail"), Params);

	TestFalse(TEXT("class_detail for missing class should fail"), Result.bSuccess);
	TestEqual(TEXT("Error code should be CLASS_NOT_FOUND"),
		Result.ErrorCode, CortexErrorCodes::ClassNotFound);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectFindClassMissingParamTest,
	"Cortex.Reflect.FindClass.MissingParam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectFindClassMissingParamTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	FCortexCommandResult Result = Handler.Execute(TEXT("class_detail"), Params);

	TestFalse(TEXT("class_detail without class_name should fail"), Result.bSuccess);
	TestEqual(TEXT("Error code should be INVALID_FIELD"),
		Result.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}
