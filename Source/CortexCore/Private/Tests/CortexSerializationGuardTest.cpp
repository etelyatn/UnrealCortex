#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "CortexCoreModule.h"
#include "CortexCommandRouter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
bool ParseResponse(const FString& Json, TSharedPtr<FJsonObject>& OutObject)
{
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	return FJsonSerializer::Deserialize(Reader, OutObject);
}

TSharedPtr<FJsonObject> BuildMalformedObject()
{
	// An array whose single element is a null TSharedPtr<FJsonValue> — dereferenced by
	// FJsonSerializer::Serialize it crashes the editor (SharedPointer.h null deref).
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> BrokenArray;
	BrokenArray.Add(nullptr);
	Data->SetArrayField(TEXT("broken"), BrokenArray);
	return Data;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializationGuardSuccessFallbackTest,
	"Cortex.Core.Serialization.GuardFallsBackOnMalformedSuccessData",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializationGuardSuccessFallbackTest::RunTest(const FString& Parameters)
{
	FCortexCommandResult Result;
	Result.bSuccess = true;
	Result.Data = BuildMalformedObject();

	const FString Json = FCortexCommandRouter::ResultToJson(Result, 12.5, TEXT("serial-test"));

	TSharedPtr<FJsonObject> Parsed;
	TestTrue(TEXT("Response parses as JSON"), ParseResponse(Json, Parsed) && Parsed.IsValid());
	if (!Parsed.IsValid())
	{
		return true;
	}

	TestEqual(TEXT("Request id preserved"), Parsed->GetStringField(TEXT("id")), TEXT("serial-test"));
	TestFalse(TEXT("Fallback is a failure"), Parsed->GetBoolField(TEXT("success")));

	const TSharedPtr<FJsonObject>* ErrorObj = nullptr;
	TestTrue(TEXT("Error object present"), Parsed->TryGetObjectField(TEXT("error"), ErrorObj) && ErrorObj != nullptr);
	if (ErrorObj == nullptr)
	{
		return true;
	}
	TestEqual(TEXT("Error code is SERIALIZATION_ERROR"),
		(*ErrorObj)->GetStringField(TEXT("code")), CortexErrorCodes::SerializationError);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializationGuardErrorDetailsFallbackTest,
	"Cortex.Core.Serialization.GuardFallsBackOnMalformedErrorDetails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializationGuardErrorDetailsFallbackTest::RunTest(const FString& Parameters)
{
	FCortexCommandResult Result;
	Result.bSuccess = false;
	Result.ErrorCode = TEXT("INVALID_VALUE");
	Result.ErrorMessage = TEXT("boom");
	Result.ErrorDetails = BuildMalformedObject();

	const FString Json = FCortexCommandRouter::ResultToJson(Result, 3.0, TEXT(""));

	TSharedPtr<FJsonObject> Parsed;
	TestTrue(TEXT("Response parses as JSON"), ParseResponse(Json, Parsed) && Parsed.IsValid());
	if (!Parsed.IsValid())
	{
		return true;
	}

	TestFalse(TEXT("Fallback is a failure"), Parsed->GetBoolField(TEXT("success")));

	const TSharedPtr<FJsonObject>* ErrorObj = nullptr;
	TestTrue(TEXT("Error object present"), Parsed->TryGetObjectField(TEXT("error"), ErrorObj) && ErrorObj != nullptr);
	if (ErrorObj == nullptr)
	{
		return true;
	}
	TestEqual(TEXT("Error code is SERIALIZATION_ERROR"),
		(*ErrorObj)->GetStringField(TEXT("code")), CortexErrorCodes::SerializationError);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializationGuardValidResponseTest,
	"Cortex.Core.Serialization.GuardPreservesValidResponse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializationGuardValidResponseTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("value"), TEXT("ok"));

	FCortexCommandResult Result;
	Result.bSuccess = true;
	Result.Data = Data;

	const FString Json = FCortexCommandRouter::ResultToJson(Result, 1.5, TEXT("valid-test"));

	TSharedPtr<FJsonObject> Parsed;
	TestTrue(TEXT("Response parses as JSON"), ParseResponse(Json, Parsed) && Parsed.IsValid());
	if (!Parsed.IsValid())
	{
		return true;
	}

	TestTrue(TEXT("Valid response succeeds"), Parsed->GetBoolField(TEXT("success")));
	TestEqual(TEXT("Request id preserved"), Parsed->GetStringField(TEXT("id")), TEXT("valid-test"));
	TestEqual(TEXT("Timing preserved"), Parsed->GetNumberField(TEXT("timing_ms")), 1.5);

	const TSharedPtr<FJsonObject>* DataObj = nullptr;
	TestTrue(TEXT("Data object present"), Parsed->TryGetObjectField(TEXT("data"), DataObj) && DataObj != nullptr);
	if (DataObj != nullptr)
	{
		TestEqual(TEXT("Data value preserved"), (*DataObj)->GetStringField(TEXT("value")), TEXT("ok"));
	}

	return true;
}
