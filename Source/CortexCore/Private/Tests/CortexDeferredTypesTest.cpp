#include "Misc/AutomationTest.h"
#include "CortexTypes.h"
#include "CortexCommandRouter.h"
#include "CortexTcpServer.h"
#include "ICortexDomainHandler.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDeferredResultFieldsTest,
	"Cortex.Core.Deferred.ResultHasDeferredField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDeferredResultFieldsTest::RunTest(const FString& Parameters)
{
	FCortexCommandResult Result;
	TestFalse(TEXT("bIsDeferred should default to false"), Result.bIsDeferred);

	Result.bIsDeferred = true;
	TestTrue(TEXT("bIsDeferred should be settable to true"), Result.bIsDeferred);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDeferredCallbackTypeTest,
	"Cortex.Core.Deferred.CallbackTypeCompiles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDeferredCallbackTypeTest::RunTest(const FString& Parameters)
{
	bool bCallbackFired = false;
	FDeferredResponseCallback Callback = [&bCallbackFired](FCortexCommandResult Result)
	{
		bCallbackFired = true;
	};

	FCortexCommandResult DummyResult;
	DummyResult.bSuccess = true;
	Callback(DummyResult);

	TestTrue(TEXT("Deferred callback should fire"), bCallbackFired);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexEditorErrorCodesTest,
	"Cortex.Core.ErrorCodes.EditorDomain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexEditorErrorCodesTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("PIE_NOT_ACTIVE code should match"), CortexErrorCodes::PIENotActive, TEXT("PIE_NOT_ACTIVE"));
	TestEqual(TEXT("PIE_ALREADY_ACTIVE code should match"), CortexErrorCodes::PIEAlreadyActive, TEXT("PIE_ALREADY_ACTIVE"));
	TestEqual(TEXT("PIE_ALREADY_PAUSED code should match"), CortexErrorCodes::PIEAlreadyPaused, TEXT("PIE_ALREADY_PAUSED"));
	TestEqual(TEXT("PIE_NOT_PAUSED code should match"), CortexErrorCodes::PIENotPaused, TEXT("PIE_NOT_PAUSED"));
	TestEqual(TEXT("PIE_TRANSITION_IN_PROGRESS code should match"), CortexErrorCodes::PIETransitionInProgress, TEXT("PIE_TRANSITION_IN_PROGRESS"));
	TestEqual(TEXT("PIE_TERMINATED code should match"), CortexErrorCodes::PIETerminated, TEXT("PIE_TERMINATED"));
	TestEqual(TEXT("PIE_MODE_UNSUPPORTED code should match"), CortexErrorCodes::PIEModeUnsupported, TEXT("PIE_MODE_UNSUPPORTED"));
	TestEqual(TEXT("VIEWPORT_NOT_FOUND code should match"), CortexErrorCodes::ViewportNotFound, TEXT("VIEWPORT_NOT_FOUND"));
	TestEqual(TEXT("INPUT_ACTION_NOT_FOUND code should match"), CortexErrorCodes::InputActionNotFound, TEXT("INPUT_ACTION_NOT_FOUND"));
	TestEqual(TEXT("SCREENSHOT_FAILED code should match"), CortexErrorCodes::ScreenshotFailed, TEXT("SCREENSHOT_FAILED"));
	TestEqual(TEXT("CONSOLE_COMMAND_FAILED code should match"), CortexErrorCodes::ConsoleCommandFailed, TEXT("CONSOLE_COMMAND_FAILED"));
	TestEqual(TEXT("INVALID_TIME_SCALE code should match"), CortexErrorCodes::InvalidTimeScale, TEXT("INVALID_TIME_SCALE"));
	TestEqual(TEXT("GAME_MODE_NOT_FOUND code should match"), CortexErrorCodes::GameModeNotFound, TEXT("GAME_MODE_NOT_FOUND"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexRequestIdInResponseTest,
	"Cortex.Core.Deferred.RequestIdInResponse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexRequestIdInResponseTest::RunTest(const FString& Parameters)
{
	FCortexCommandResult Result;
	Result.bSuccess = true;
	Result.Data = MakeShared<FJsonObject>();
	Result.Data->SetStringField(TEXT("message"), TEXT("pong"));

	FString JsonStr = FCortexCommandRouter::ResultToJson(Result, 1.5, TEXT("req_001"));

	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
	const bool bParsed = FJsonSerializer::Deserialize(Reader, JsonObj);

	TestTrue(TEXT("Should parse as valid JSON"), bParsed);
	if (bParsed && JsonObj.IsValid())
	{
		FString Id;
		TestTrue(TEXT("Should have 'id' field"), JsonObj->TryGetStringField(TEXT("id"), Id));
		TestEqual(TEXT("'id' should match request"), Id, TEXT("req_001"));
	}

	FString JsonStrNoId = FCortexCommandRouter::ResultToJson(Result, 1.5);

	TSharedPtr<FJsonObject> JsonObjNoId;
	TSharedRef<TJsonReader<>> Reader2 = TJsonReaderFactory<>::Create(JsonStrNoId);
	const bool bParsed2 = FJsonSerializer::Deserialize(Reader2, JsonObjNoId);

	TestTrue(TEXT("Should parse as valid JSON (no id)"), bParsed2);
	if (bParsed2 && JsonObjNoId.IsValid())
	{
		TestFalse(TEXT("Should NOT have 'id' field when not provided"), JsonObjNoId->HasField(TEXT("id")));
	}

	return true;
}

class FTestDeferredHandler : public ICortexDomainHandler
{
public:
	FDeferredResponseCallback StoredCallback;
	bool bCommandReceived = false;

	virtual FCortexCommandResult Execute(
		const FString& Command,
		const TSharedPtr<FJsonObject>& Params,
		FDeferredResponseCallback DeferredCallback = nullptr) override
	{
		(void)Params;
		if (Command == TEXT("slow_op"))
		{
			bCommandReceived = true;
			StoredCallback = MoveTemp(DeferredCallback);

			FCortexCommandResult Result;
			Result.bIsDeferred = true;
			return Result;
		}

		return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Unknown"));
	}

	virtual TArray<FCortexCommandInfo> GetSupportedCommands() const override
	{
		return { { TEXT("slow_op"), TEXT("A slow operation") } };
	}
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDeferredResponseE2ETest,
	"Cortex.Core.Deferred.E2EFlow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDeferredResponseE2ETest::RunTest(const FString& Parameters)
{
	auto DeferredHandler = MakeShared<FTestDeferredHandler>();
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("test"), TEXT("Test"), TEXT("1.0.0"), DeferredHandler);

	FCortexTcpServer Server;
	const bool bStarted = Server.Start(
		0,
		[&Router](const FString& Command, const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback)
		{
			return Router.Execute(Command, Params, MoveTemp(DeferredCallback));
		});
	TestTrue(TEXT("Server should start"), bStarted);
	if (!bStarted)
	{
		return true;
	}

	FString PortText;
	const FString PortFilePath = FPaths::ProjectSavedDir() / TEXT("CortexPort.txt");
	const bool bReadPortFile = FFileHelper::LoadFileToString(PortText, *PortFilePath);
	TestTrue(TEXT("Should read CortexPort.txt"), bReadPortFile);
	if (!bReadPortFile)
	{
		Server.Stop();
		return true;
	}
	const int32 TestPort = FCString::Atoi(*PortText);
	TestTrue(TEXT("Port from file should be > 0"), TestPort > 0);
	if (TestPort <= 0)
	{
		Server.Stop();
		return true;
	}

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	FSocket* Client = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("DeferredTestClient"), false);
	TestNotNull(TEXT("Client socket should be created"), Client);
	if (Client == nullptr)
	{
		Server.Stop();
		return true;
	}

	FIPv4Endpoint Endpoint(FIPv4Address::InternalLoopback, TestPort);
	const bool bConnected = Client->Connect(*Endpoint.ToInternetAddr());
	TestTrue(TEXT("Client should connect"), bConnected);
	if (!bConnected)
	{
		SocketSubsystem->DestroySocket(Client);
		Server.Stop();
		return true;
	}
	FPlatformProcess::Sleep(0.1f);

	FString CommandLine = TEXT("{\"id\":\"req_001\",\"command\":\"test.slow_op\",\"params\":{}}\n");
	FTCHARToUTF8 Utf8(*CommandLine);
	int32 BytesSent = 0;
	const bool bSent = Client->Send(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), BytesSent);
	TestTrue(TEXT("Should send deferred command"), bSent);

	FString Response;
	uint8 Buffer[4096];
	int32 BytesRead = 0;
	for (int32 Attempt = 0; Attempt < 30; ++Attempt)
	{
		FTSTicker::GetCoreTicker().Tick(0.016f);
		FPlatformProcess::Sleep(0.05f);

		uint32 PendingData = 0;
		while (Client->HasPendingData(PendingData) && PendingData > 0)
		{
			if (Client->Recv(Buffer, sizeof(Buffer) - 1, BytesRead) && BytesRead > 0)
			{
				Buffer[BytesRead] = 0;
				FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Buffer), BytesRead);
				Response.Append(Conv.Get(), Conv.Length());
			}
			else
			{
				break;
			}
		}

		if (Response.Contains(TEXT("\n")))
		{
			break;
		}
	}

	TestTrue(TEXT("Should receive deferred ack"), Response.Contains(TEXT("deferred")));

	const int32 AckNewline = Response.Find(TEXT("\n"));
	const FString AckLine = AckNewline == INDEX_NONE ? Response : Response.Left(AckNewline);
	TSharedPtr<FJsonObject> AckJson;
	TSharedRef<TJsonReader<>> AckReader = TJsonReaderFactory<>::Create(AckLine);
	const bool bAckParsed = FJsonSerializer::Deserialize(AckReader, AckJson);
	TestTrue(TEXT("Ack should parse"), bAckParsed);
	if (bAckParsed && AckJson.IsValid())
	{
		FString AckId;
		FString Status;
		AckJson->TryGetStringField(TEXT("id"), AckId);
		AckJson->TryGetStringField(TEXT("status"), Status);
		TestEqual(TEXT("Ack should have matching id"), AckId, TEXT("req_001"));
		TestEqual(TEXT("Ack status should be deferred"), Status, TEXT("deferred"));
	}

	TestTrue(TEXT("Handler should receive deferred command"), DeferredHandler->bCommandReceived);
	if (DeferredHandler->StoredCallback)
	{
		FCortexCommandResult FinalResult;
		FinalResult.bSuccess = true;
		FinalResult.Data = MakeShared<FJsonObject>();
		FinalResult.Data->SetStringField(TEXT("status"), TEXT("done"));
		DeferredHandler->StoredCallback(FinalResult);
	}

	Response.Empty();
	for (int32 Attempt = 0; Attempt < 30; ++Attempt)
	{
		FTSTicker::GetCoreTicker().Tick(0.016f);
		FPlatformProcess::Sleep(0.05f);

		uint32 PendingData = 0;
		while (Client->HasPendingData(PendingData) && PendingData > 0)
		{
			if (Client->Recv(Buffer, sizeof(Buffer) - 1, BytesRead) && BytesRead > 0)
			{
				Buffer[BytesRead] = 0;
				FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Buffer), BytesRead);
				Response.Append(Conv.Get(), Conv.Length());
			}
			else
			{
				break;
			}
		}

		if (Response.Contains(TEXT("\n")))
		{
			break;
		}
	}

	TestTrue(TEXT("Should receive final response"), Response.Contains(TEXT("complete")));
	const int32 FinalNewline = Response.Find(TEXT("\n"));
	const FString FinalLine = FinalNewline == INDEX_NONE ? Response : Response.Left(FinalNewline);
	TSharedPtr<FJsonObject> FinalJson;
	TSharedRef<TJsonReader<>> FinalReader = TJsonReaderFactory<>::Create(FinalLine);
	const bool bFinalParsed = FJsonSerializer::Deserialize(FinalReader, FinalJson);
	TestTrue(TEXT("Final response should parse"), bFinalParsed);
	if (bFinalParsed && FinalJson.IsValid())
	{
		FString FinalId;
		FString FinalStatus;
		bool bSuccess = false;
		FinalJson->TryGetStringField(TEXT("id"), FinalId);
		FinalJson->TryGetStringField(TEXT("status"), FinalStatus);
		FinalJson->TryGetBoolField(TEXT("success"), bSuccess);
		TestEqual(TEXT("Final should have matching id"), FinalId, TEXT("req_001"));
		TestEqual(TEXT("Final status should be complete"), FinalStatus, TEXT("complete"));
		TestTrue(TEXT("Final should be success"), bSuccess);
	}

	SocketSubsystem->DestroySocket(Client);
	Server.Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDeferredTimeoutTest,
	"Cortex.Core.Deferred.Timeout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDeferredTimeoutTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	auto Handler = MakeShared<FTestDeferredHandler>();
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("test"), TEXT("Test"), TEXT("1.0.0"), Handler);

	FCortexTcpServer Server;
	const bool bStarted = Server.Start(
		0,
		[&Router](const FString& Command, const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback)
		{
			return Router.Execute(Command, Params, MoveTemp(DeferredCallback));
		});
	TestTrue(TEXT("Server should start"), bStarted);
	if (!bStarted)
	{
		return true;
	}

	FString PortText;
	const FString PortFilePath = FPaths::ProjectSavedDir() / TEXT("CortexPort.txt");
	const bool bReadPortFile = FFileHelper::LoadFileToString(PortText, *PortFilePath);
	TestTrue(TEXT("Should read CortexPort.txt"), bReadPortFile);
	const int32 TestPort = FCString::Atoi(*PortText);
	TestTrue(TEXT("Port from file should be > 0"), TestPort > 0);

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	FSocket* Client = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("DeferredTimeoutClient"), false);
	TestNotNull(TEXT("Client socket should be created"), Client);
	if (Client == nullptr || TestPort <= 0)
	{
		if (Client != nullptr)
		{
			SocketSubsystem->DestroySocket(Client);
		}
		Server.Stop();
		return true;
	}

	const FIPv4Endpoint Endpoint(FIPv4Address::InternalLoopback, TestPort);
	const bool bConnected = Client->Connect(*Endpoint.ToInternetAddr());
	TestTrue(TEXT("Client should connect"), bConnected);
	if (!bConnected)
	{
		SocketSubsystem->DestroySocket(Client);
		Server.Stop();
		return true;
	}

	const FString CommandLine = TEXT("{\"id\":\"timeout_test\",\"command\":\"test.slow_op\",\"params\":{}}\n");
	FTCHARToUTF8 Utf8(*CommandLine);
	int32 BytesSent = 0;
	const bool bSent = Client->Send(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), BytesSent);
	TestTrue(TEXT("Should send deferred command"), bSent);

	for (int32 Attempt = 0; Attempt < 10; ++Attempt)
	{
		FTSTicker::GetCoreTicker().Tick(0.016f);
		FPlatformProcess::Sleep(0.05f);
	}

	TestTrue(TEXT("Handler should have received command"), Handler->bCommandReceived);

	SocketSubsystem->DestroySocket(Client);
	Server.Stop();
	return true;
}
