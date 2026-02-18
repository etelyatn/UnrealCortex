#include "Misc/AutomationTest.h"
#include "CortexReflectCommandHandler.h"
#include "CortexTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectUsagesBasicTest,
	"Cortex.Reflect.Usages.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectUsagesBasicTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("symbol"), TEXT("JumpMaxHoldTime"));
	Params->SetStringField(TEXT("class_name"), TEXT("ACharacter"));

	FCortexCommandResult Result = Handler.Execute(TEXT("find_usages"), Params);

	TestTrue(TEXT("find_usages should succeed"), Result.bSuccess);

	if (Result.Data.IsValid())
	{
		TestTrue(TEXT("Should have symbol field"),
			Result.Data->HasField(TEXT("symbol")));
		TestTrue(TEXT("Should have usages array"),
			Result.Data->HasField(TEXT("usages")));
		TestTrue(TEXT("Should have total_usages field"),
			Result.Data->HasField(TEXT("total_usages")));
		TestTrue(TEXT("Should have total_classes field"),
			Result.Data->HasField(TEXT("total_classes")));
		TestTrue(TEXT("Should have defined_in field"),
			Result.Data->HasField(TEXT("defined_in")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectUsagesSymbolNotFoundTest,
	"Cortex.Reflect.Usages.SymbolNotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectUsagesSymbolNotFoundTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("symbol"), TEXT("NonExistentSymbolXYZ"));
	Params->SetStringField(TEXT("class_name"), TEXT("ACharacter"));

	FCortexCommandResult Result = Handler.Execute(TEXT("find_usages"), Params);

	TestFalse(TEXT("find_usages with unknown symbol should fail"), Result.bSuccess);
	TestEqual(TEXT("Should be SYMBOL_NOT_FOUND"),
		Result.ErrorCode, CortexErrorCodes::SymbolNotFound);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectUsagesClassNotFoundTest,
	"Cortex.Reflect.Usages.ClassNotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectUsagesClassNotFoundTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("symbol"), TEXT("Health"));
	Params->SetStringField(TEXT("class_name"), TEXT("ADoesNotExistFoo"));

	FCortexCommandResult Result = Handler.Execute(TEXT("find_usages"), Params);

	TestFalse(TEXT("find_usages with unknown class should fail"), Result.bSuccess);
	TestEqual(TEXT("Should be CLASS_NOT_FOUND"),
		Result.ErrorCode, CortexErrorCodes::ClassNotFound);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectUsagesMissingSymbolTest,
	"Cortex.Reflect.Usages.MissingSymbol",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectUsagesMissingSymbolTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	// symbol intentionally omitted
	Params->SetStringField(TEXT("class_name"), TEXT("ACharacter"));

	FCortexCommandResult Result = Handler.Execute(TEXT("find_usages"), Params);

	TestFalse(TEXT("find_usages without symbol should fail"), Result.bSuccess);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReflectUsagesMissingClassNameTest,
	"Cortex.Reflect.Usages.MissingClassName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReflectUsagesMissingClassNameTest::RunTest(const FString& Parameters)
{
	FCortexReflectCommandHandler Handler;
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("symbol"), TEXT("Health"));
	// class_name intentionally omitted

	FCortexCommandResult Result = Handler.Execute(TEXT("find_usages"), Params);

	TestFalse(TEXT("find_usages without class_name should fail"), Result.bSuccess);

	return true;
}
