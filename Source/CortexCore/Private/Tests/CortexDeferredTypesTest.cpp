#include "Misc/AutomationTest.h"
#include "CortexDeferredExec.h"
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
#include "HAL/PlatformProcess.h"

static int32 ParsePortFromPortFileText(const FString& PortText)
{
	FString Trimmed = PortText;
	Trimmed.TrimStartAndEndInline();

	if (Trimmed.StartsWith(TEXT("{")))
	{
		TSharedPtr<FJsonObject> JsonObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
		if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
		{
			int32 ParsedPort = 0;
			if (JsonObj->TryGetNumberField(TEXT("port"), ParsedPort))
			{
				return ParsedPort;
			}
		}
		return 0;
	}

	return FCString::Atoi(*Trimmed);
}

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
	FCortexDeferredExecRequiresCallbackTest,
	"Cortex.Core.Deferred.RunNextTick.RequiresCallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDeferredExecRequiresCallbackTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	bool bWorkRan = false;

	FCortexCommandResult Result = FCortexDeferredExec::RunNextTick(
		[&bWorkRan]()
		{
			bWorkRan = true;
			return FCortexCommandRouter::Success(MakeShared<FJsonObject>());
		},
		nullptr);

	TestFalse(TEXT("RunNextTick without callback should fail"), Result.bSuccess);
	TestFalse(TEXT("RunNextTick without callback should not be deferred"), Result.bIsDeferred);
	TestEqual(TEXT("Error should be INVALID_OPERATION"), Result.ErrorCode, CortexErrorCodes::InvalidOperation);
	TestFalse(TEXT("Work must not run without a callback"), bWorkRan);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDeferredExecRunsOnNextTickTest,
	"Cortex.Core.Deferred.RunNextTick.CallbackOnTicker",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDeferredExecRunsOnNextTickTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	bool bWorkRan = false;
	bool bCallbackRan = false;
	FString Status;

	bool bDeferredSuccess = false;
	FDeferredResponseCallback Callback = [&bCallbackRan, &bDeferredSuccess, &Status](FCortexCommandResult Result)
	{
		bCallbackRan = true;
		bDeferredSuccess = Result.bSuccess;
		if (Result.Data.IsValid())
		{
			Result.Data->TryGetStringField(TEXT("status"), Status);
		}
	};

	FCortexCommandResult Result = FCortexDeferredExec::RunNextTick(
		[&bWorkRan]()
		{
			bWorkRan = true;
			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("status"), TEXT("ran_next_tick"));
			return FCortexCommandRouter::Success(Data);
		},
		MoveTemp(Callback));

	TestTrue(TEXT("RunNextTick should return deferred placeholder"), Result.bIsDeferred);
	TestFalse(TEXT("Work should not run before the ticker advances"), bWorkRan);
	TestFalse(TEXT("Callback should not run before the ticker advances"), bCallbackRan);

	for (int32 Attempt = 0; Attempt < 5 && !bCallbackRan; ++Attempt)
	{
		FTSTicker::GetCoreTicker().Tick(0.016f);
	}

	TestTrue(TEXT("Work should run after ticker advances"), bWorkRan);
	TestTrue(TEXT("Callback should run after ticker advances"), bCallbackRan);
	TestTrue(TEXT("Deferred callback should receive success"), bDeferredSuccess);
	TestEqual(TEXT("Callback data should round-trip"), Status, TEXT("ran_next_tick"));
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
	Router.RegisterDomain(TEXT("test"), TEXT("Test"), TEXT("1.0.1"), DeferredHandler);

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
	const uint32 CurrentPID = FPlatformProcess::GetCurrentProcessId();
	const FString PortFilePath = FPaths::ProjectSavedDir() / FString::Printf(TEXT("CortexPort-%u.txt"), CurrentPID);
	const bool bReadPortFile = FFileHelper::LoadFileToString(PortText, *PortFilePath);
	TestTrue(TEXT("Should read CortexPort-{PID}.txt"), bReadPortFile);
	if (!bReadPortFile)
	{
		Server.Stop();
		return true;
	}
	const int32 TestPort = ParsePortFromPortFileText(PortText);
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
	Router.RegisterDomain(TEXT("test"), TEXT("Test"), TEXT("1.0.1"), Handler);

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
	const uint32 CurrentPID = FPlatformProcess::GetCurrentProcessId();
	const FString PortFilePath = FPaths::ProjectSavedDir() / FString::Printf(TEXT("CortexPort-%u.txt"), CurrentPID);
	const bool bReadPortFile = FFileHelper::LoadFileToString(PortText, *PortFilePath);
	TestTrue(TEXT("Should read CortexPort-{PID}.txt"), bReadPortFile);
	const int32 TestPort = ParsePortFromPortFileText(PortText);
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

class FTestPerCommandTimeoutHandler : public ICortexDomainHandler
{
public:
	virtual FCortexCommandResult Execute(
		const FString& Command,
		const TSharedPtr<FJsonObject>& Params,
		FDeferredResponseCallback DeferredCallback = nullptr) override
	{
		(void)Params;
		(void)DeferredCallback;
		FCortexCommandResult Result;
		Result.bIsDeferred = true;
		if (Command == TEXT("short_op"))
		{
			Result.DeferredTimeoutSeconds = 0.3;
			return Result;
		}
		if (Command == TEXT("long_op"))
		{
			Result.DeferredTimeoutSeconds = 600.0;
			return Result;
		}
		if (Command == TEXT("default_op"))
		{
			return Result;
		}
		return FCortexCommandRouter::Error(CortexErrorCodes::UnknownCommand, TEXT("Unknown"));
	}

	virtual TArray<FCortexCommandInfo> GetSupportedCommands() const override
	{
		return { { TEXT("short_op"), TEXT("short") }, { TEXT("long_op"), TEXT("long") }, { TEXT("default_op"), TEXT("default") } };
	}
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDeferredPerCommandTimeoutTest,
	"Cortex.Core.Deferred.PerCommandTimeout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDeferredPerCommandTimeoutTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	auto Handler = MakeShared<FTestPerCommandTimeoutHandler>();
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("test"), TEXT("Test"), TEXT("1.0.1"), Handler);

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
	const FString PortFilePath = FPaths::ProjectSavedDir() / FString::Printf(TEXT("CortexPort-%u.txt"), FPlatformProcess::GetCurrentProcessId());
	FFileHelper::LoadFileToString(PortText, *PortFilePath);
	const int32 TestPort = ParsePortFromPortFileText(PortText);
	TestTrue(TEXT("Port from file should be > 0"), TestPort > 0);
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	FSocket* Client = TestPort > 0 ? SocketSubsystem->CreateSocket(NAME_Stream, TEXT("DeferredPerCommandTimeoutClient"), false) : nullptr;
	if (Client == nullptr || !Client->Connect(*FIPv4Endpoint(FIPv4Address::InternalLoopback, TestPort).ToInternetAddr()))
	{
		AddError(TEXT("Could not connect a client to the test server"));
		if (Client != nullptr)
		{
			SocketSubsystem->DestroySocket(Client);
		}
		Server.Stop();
		return true;
	}
	FPlatformProcess::Sleep(0.1f);

	FString Pending;
	// Sends one command and returns the next complete response line (ticking the server), empty if none arrives in MaxSeconds.
	auto SendCommand = [&](const FString& Id, const FString& Command)
	{
		const FString Line = FString::Printf(TEXT("{\"id\":\"%s\",\"command\":\"test.%s\",\"params\":{}}\n"), *Id, *Command);
		FTCHARToUTF8 Utf8(*Line);
		int32 BytesSent = 0;
		Client->Send(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), BytesSent);
	};
	auto ReadLine = [&](double MaxSeconds) -> TSharedPtr<FJsonObject>
	{
		const double Deadline = FPlatformTime::Seconds() + MaxSeconds;
		uint8 Buffer[4096];
		while (FPlatformTime::Seconds() < Deadline)
		{
			int32 Newline = INDEX_NONE;
			if (Pending.FindChar(TEXT('\n'), Newline))
			{
				const FString Line = Pending.Left(Newline);
				Pending.RightChopInline(Newline + 1);
				TSharedPtr<FJsonObject> Json;
				FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Line), Json);
				return Json;
			}
			FTSTicker::GetCoreTicker().Tick(0.016f);
			FPlatformProcess::Sleep(0.05f);
			uint32 PendingData = 0;
			while (Client->HasPendingData(PendingData) && PendingData > 0)
			{
				int32 BytesRead = 0;
				if (!Client->Recv(Buffer, sizeof(Buffer) - 1, BytesRead) || BytesRead <= 0)
				{
					break;
				}
				FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Buffer), BytesRead);
				Pending.Append(Conv.Get(), Conv.Length());
			}
		}
		return nullptr;
	};
	auto AckTimeout = [](const TSharedPtr<FJsonObject>& Ack) -> double
	{
		const TSharedPtr<FJsonObject>* Data = nullptr;
		double Value = -1.0;
		if (Ack.IsValid() && Ack->TryGetObjectField(TEXT("data"), Data))
		{
			(*Data)->TryGetNumberField(TEXT("timeout_seconds"), Value);
		}
		return Value;
	};

	SendCommand(TEXT("long"), TEXT("long_op"));
	TestEqual(TEXT("A handler-provided deferred timeout is advertised in the ack"), AckTimeout(ReadLine(3.0)), 600.0);

	SendCommand(TEXT("default"), TEXT("default_op"));
	TestEqual(TEXT("Commands without a handler timeout keep the 30 s default"), AckTimeout(ReadLine(3.0)), 30.0);

	SendCommand(TEXT("short"), TEXT("short_op"));
	TestEqual(TEXT("Short op ack advertises its own timeout"), AckTimeout(ReadLine(3.0)), 0.3);
	TSharedPtr<FJsonObject> Final = ReadLine(3.0);
	TestTrue(TEXT("The short op is timed out by its own bound, not the 30 s default"), Final.IsValid());
	if (Final.IsValid())
	{
		FString Id;
		FString Status;
		FString Code;
		Final->TryGetStringField(TEXT("id"), Id);
		Final->TryGetStringField(TEXT("status"), Status);
		const TSharedPtr<FJsonObject>* ErrorObj = nullptr;
		if (Final->TryGetObjectField(TEXT("error"), ErrorObj))
		{
			(*ErrorObj)->TryGetStringField(TEXT("code"), Code);
		}
		TestEqual(TEXT("Timed-out response id"), Id, TEXT("short"));
		TestEqual(TEXT("Timed-out response code"), Code, CortexErrorCodes::InvalidOperation);
	}
	TestFalse(TEXT("The long and default ops are still pending, not timed out"), ReadLine(0.2).IsValid());

	SocketSubsystem->DestroySocket(Client);
	Server.Stop();
	return true;
}
