#include "Misc/AutomationTest.h"
#include "CortexCoreModule.h"
#include "CortexConversionTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexConversionDelegateExistsTest,
	"Cortex.Core.ConversionDelegate.Exists",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexConversionDelegateExistsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));

	bool bReceived = false;
	FDelegateHandle Handle = Core.OnConversionRequested().AddLambda(
		[&bReceived](const FCortexConversionPayload& Payload)
		{
			bReceived = true;
		});

	FCortexConversionPayload Payload;
	Payload.BlueprintPath = TEXT("/Game/Test/BP_Test");
	Payload.BlueprintName = TEXT("BP_Test");
	Core.OnConversionRequested().Broadcast(Payload);

	TestTrue(TEXT("Delegate should fire"), bReceived);

	Core.OnConversionRequested().Remove(Handle);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSerializationUnboundHandlerTest,
	"Cortex.Core.Serialization.UnboundHandler",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSerializationUnboundHandlerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));

	Core.ClearSerializationHandler();

	FCortexSerializationRequest Request;
	Request.BlueprintPath = TEXT("/Game/Test/BP_Test");
	Request.Scope = ECortexConversionScope::EntireBlueprint;

	bool bCallbackFired = false;
	bool bSuccess = true;
	FString ResultJson;

	Core.RequestSerialization(Request,
		FOnSerializationComplete::CreateLambda(
			[&](const FCortexSerializationResult& SerResult)
			{
				bCallbackFired = true;
				bSuccess = SerResult.bSuccess;
				ResultJson = SerResult.JsonPayload;
			}));

	TestTrue(TEXT("Callback should fire even when unbound"), bCallbackFired);
	TestFalse(TEXT("Should report failure when no handler bound"), bSuccess);
	TestTrue(TEXT("Error JSON should mention no handler"), ResultJson.Contains(TEXT("error")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexSerializationBoundHandlerTest,
	"Cortex.Core.Serialization.BoundHandler",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexSerializationBoundHandlerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexCoreModule& Core = FModuleManager::GetModuleChecked<FCortexCoreModule>(TEXT("CortexCore"));

	// Clear any existing handler (e.g. from CortexBlueprint) before binding test handler
	Core.ClearSerializationHandler();

	Core.SetSerializationHandler(FOnCortexSerializationRequested::CreateLambda(
		[](const FCortexSerializationRequest& Req, FOnSerializationComplete Callback)
		{
			FCortexSerializationResult HandlerResult;
			HandlerResult.bSuccess = true;
			HandlerResult.JsonPayload = TEXT("{\"test\":\"ok\"}");
			Callback.Execute(HandlerResult);
		}));

	FCortexSerializationRequest Request;
	Request.BlueprintPath = TEXT("/Game/Test/BP_Test");
	Request.Scope = ECortexConversionScope::EntireBlueprint;

	bool bSuccess = false;
	FString ResultJson;

	Core.RequestSerialization(Request,
		FOnSerializationComplete::CreateLambda(
			[&](const FCortexSerializationResult& SerResult)
			{
				bSuccess = SerResult.bSuccess;
				ResultJson = SerResult.JsonPayload;
			}));

	TestTrue(TEXT("Should report success when handler bound"), bSuccess);
	TestTrue(TEXT("Should return handler JSON"), ResultJson.Contains(TEXT("test")));

	Core.ClearSerializationHandler();
	return true;
}
