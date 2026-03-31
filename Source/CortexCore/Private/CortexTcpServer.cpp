
#include "CortexTcpServer.h"
#include "CortexCoreModule.h"
#include "CortexCommandRouter.h"
#include "CortexFileUtils.h"
#include "CortexSettings.h"
#include "Common/TcpListener.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Misc/ScopeLock.h"

FCortexTcpServer::FCortexTcpServer()
{
}

FCortexTcpServer::~FCortexTcpServer()
{
	Stop();
}

void FCortexTcpServer::CleanupStalePortFiles()
{
	TArray<FString> PortFiles;
	IFileManager::Get().FindFiles(
		PortFiles,
		*(FPaths::ProjectSavedDir() / TEXT("CortexPort-*.txt")),
		true,
		false);

	const uint32 OurPID = FPlatformProcess::GetCurrentProcessId();

	for (const FString& Filename : PortFiles)
	{
		// Parse PID from filename: CortexPort-{PID}.txt
		FString PIDString = Filename;
		if (!PIDString.RemoveFromStart(TEXT("CortexPort-")) ||
			!PIDString.RemoveFromEnd(TEXT(".txt")))
		{
			continue;
		}

		const uint32 FilePID = FCString::Atoi(*PIDString);
		if (FilePID == 0 || FilePID == OurPID)
		{
			continue;
		}

		// Only delete when we can positively determine the process is not running.
		if (FPlatformProcess::IsApplicationRunning(FilePID))
		{
			continue;
		}

		const FString FullPath = FPaths::ProjectSavedDir() / Filename;
		IFileManager::Get().Delete(*FullPath);
		IFileManager::Get().Delete(*(FullPath + TEXT(".tmp")));
		UE_LOG(LogCortex, Log, TEXT("Cleaned up stale port file: %s (PID %u no longer running)"),
			*Filename,
			FilePID);
	}
}

bool FCortexTcpServer::Start(int32 StartPort, FCommandDispatcher InDispatcher)
{
	if (bRunning)
	{
		UE_LOG(LogCortex, Warning, TEXT("TCP server is already running"));
		return false;
	}

	CleanupStalePortFiles();

	CommandDispatcher = MoveTemp(InDispatcher);

	for (int32 Port = StartPort; Port < StartPort + 100; ++Port)
	{
		FIPv4Endpoint ListenEndpoint(FIPv4Address::InternalLoopback, Port);

		// Disable SO_REUSEADDR so bind() fails when another editor already owns this port.
		// On Linux this can also reject TIME_WAIT ports; the auto-increment range mitigates it.
		Listener = MakeUnique<FTcpListener>(ListenEndpoint, FTimespan::FromMilliseconds(100), false);
		Listener->OnConnectionAccepted().BindRaw(this, &FCortexTcpServer::HandleConnectionAccepted);

		if (Listener->IsActive())
		{
			bRunning = true;
			BoundPort = Port;
			if (FSocket* ListenSocket = Listener->GetSocket())
			{
				TSharedRef<FInternetAddr> LocalAddress =
					ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
				ListenSocket->GetAddress(*LocalAddress);
				BoundPort = LocalAddress->GetPort();
			}

			// Write JSON port file for MCP server auto-discovery
			{
				TSharedRef<FJsonObject> PortInfo = MakeShared<FJsonObject>();
				PortInfo->SetNumberField(TEXT("port"), BoundPort);
				PortInfo->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
				PortInfo->SetStringField(TEXT("project"), FApp::GetProjectName());

				FString UProjectPath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
				UProjectPath.ReplaceInline(TEXT("\\"), TEXT("/"));
				PortInfo->SetStringField(TEXT("project_path"), UProjectPath);
				PortInfo->SetStringField(TEXT("started_at"), FDateTime::UtcNow().ToIso8601());

				FString JsonString;
				TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
					TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonString);
				FJsonSerializer::Serialize(PortInfo, Writer);

				const uint32 CurrentPID = FPlatformProcess::GetCurrentProcessId();
				PortFilePath = FPaths::ProjectSavedDir() / FString::Printf(TEXT("CortexPort-%u.txt"), CurrentPID);
				FCortexFileUtils::AtomicWriteFile(PortFilePath, JsonString);

				UE_LOG(LogCortex, Log, TEXT("Wrote port file: %s (port %d, pid %u)"),
					*PortFilePath,
					BoundPort,
					CurrentPID);
			}

			LastTickTime.Store(FPlatformTime::Seconds());

			TickDelegateHandle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([this](float DeltaTime) -> bool
				{
					if (bRunning)
					{
						const double Now = FPlatformTime::Seconds();
						const double PrevTick = LastTickTime.Load();
						const double Gap = Now - PrevTick;
						LastTickTime.Store(Now);

						if (Gap > StallWarningThresholdSeconds && PrevTick > 0.0)
						{
							UE_LOG(LogCortex, Warning,
								TEXT("Game thread stall detected: %.1fs since last tick. "
									 "Commands were queued but not processed during this period."),
								Gap);
						}

						{
							FScopeLock Lock(&PendingSocketsCS);
							for (FSocket* PendingSocket : PendingClientSockets)
							{
								ClientSockets.Add(PendingSocket);
								ReceiveBuffers.Add(PendingSocket, FString());
							}
							PendingClientSockets.Empty();
						}

						ProcessClientData();
						CheckDeferredTimeouts();
					}
					return bRunning;
				}),
				0.0f
			);

			UE_LOG(LogCortex, Log, TEXT("TCP server listening on 127.0.0.1:%d"), BoundPort);
			return true;
		}

		Listener.Reset();
	}

	UE_LOG(LogCortex, Error, TEXT("Failed to bind TCP server on ports %d-%d"),
		StartPort, StartPort + 99);
	return false;
}

void FCortexTcpServer::Stop()
{
	if (!bRunning)
	{
		return;
	}

	bRunning = false;
	BoundPort = 0;

	if (!PortFilePath.IsEmpty())
	{
		IFileManager::Get().Delete(*PortFilePath);
		IFileManager::Get().Delete(*(PortFilePath + TEXT(".tmp")));
		PortFilePath.Empty();
	}

	if (TickDelegateHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickDelegateHandle);
		TickDelegateHandle.Reset();
	}

	for (FSocket* Socket : ClientSockets)
	{
		if (Socket != nullptr)
		{
			Socket->Close();
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		}
	}
	ClientSockets.Empty();
	PendingClientSockets.Empty();
	ReceiveBuffers.Empty();
	PendingDeferred.Empty();
	NextDeferredId = 1;

	Listener.Reset();

	UE_LOG(LogCortex, Log, TEXT("TCP server stopped"));
}

void FCortexTcpServer::SetClientDisconnectCallback(FClientDisconnectCallback Callback)
{
	ClientDisconnectCallback = MoveTemp(Callback);
}

int32 FCortexTcpServer::GetBoundPort() const
{
	return BoundPort;
}

int32 FCortexTcpServer::GetClientCount() const
{
	return ClientSockets.Num();
}

bool FCortexTcpServer::IsRunning() const
{
	return bRunning;
}

bool FCortexTcpServer::HandleConnectionAccepted(FSocket* InClientSocket, const FIPv4Endpoint& ClientEndpoint)
{
	UE_LOG(LogCortex, Log, TEXT("Client connected from %s"), *ClientEndpoint.ToString());
	FScopeLock Lock(&PendingSocketsCS);
	PendingClientSockets.Add(InClientSocket);
	return true;
}

void FCortexTcpServer::ProcessClientData()
{
	if (ClientSockets.Num() == 0)
	{
		return;
	}

	// Iterate in reverse so we can safely remove disconnected clients
	for (int32 Index = ClientSockets.Num() - 1; Index >= 0; --Index)
	{
		FSocket* Socket = ClientSockets[Index];
		if (!ProcessSingleClient(Socket))
		{
			DestroyClientSocket(Socket);
			ClientSockets.RemoveAt(Index);
		}
	}
}

bool FCortexTcpServer::ProcessSingleClient(FSocket* InClientSocket)
{
	if (InClientSocket == nullptr)
	{
		return false;
	}

	// Check connection state
	ESocketConnectionState ConnectionState = InClientSocket->GetConnectionState();
	if (ConnectionState == SCS_ConnectionError)
	{
		UE_LOG(LogCortex, Log, TEXT("Client disconnected"));
		return false;
	}

	// Read available data
	uint32 PendingDataSize = 0;
	if (!InClientSocket->HasPendingData(PendingDataSize) || PendingDataSize == 0)
	{
		return true;
	}

	FString& ClientBuffer = ReceiveBuffers.FindOrAdd(InClientSocket);
	int32 TotalBytesRead = 0;

	// Loop until all pending data is read or 2MB limit is reached
	do
	{
		TArray<uint8> TempBuffer;
		TempBuffer.SetNumUninitialized(ReceiveBufferSize);
		int32 BytesRead = 0;

		if (!InClientSocket->Recv(TempBuffer.GetData(), ReceiveBufferSize - 1, BytesRead) || BytesRead <= 0)
		{
			break;
		}

		FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(TempBuffer.GetData()), BytesRead);
		ClientBuffer.Append(Converter.Get(), Converter.Length());
		TotalBytesRead += BytesRead;

		if (TotalBytesRead >= MaxMessageSize)
		{
			UE_LOG(LogCortex, Warning, TEXT("Client message exceeds MaxMessageSize (%d bytes), truncating"), MaxMessageSize);
			break;
		}

		PendingDataSize = 0;
	} while (InClientSocket->HasPendingData(PendingDataSize) && PendingDataSize > 0);

	// Process complete lines (delimited by \n)
	int32 NewlineIndex = INDEX_NONE;
	while (ClientBuffer.FindChar(TEXT('\n'), NewlineIndex))
	{
		FString Line = ClientBuffer.Left(NewlineIndex);
		ClientBuffer.RemoveAt(0, NewlineIndex + 1);

		Line.TrimStartAndEndInline();
		if (Line.IsEmpty())
		{
			continue;
		}

		// Parse JSON
		TSharedPtr<FJsonObject> RequestJson;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);

		if (!FJsonSerializer::Deserialize(Reader, RequestJson) || !RequestJson.IsValid())
		{
			UE_LOG(LogCortex, Warning, TEXT("Failed to parse JSON: %s"), *Line);
			FCortexCommandResult ParseError = FCortexCommandRouter::Error(
				TEXT("PARSE_ERROR"),
				TEXT("Failed to parse JSON request")
			);
			SendResponse(InClientSocket, FCortexCommandRouter::ResultToJson(ParseError, 0.0));
			continue;
		}

		// Extract command
		FString Command;
		if (!RequestJson->TryGetStringField(TEXT("command"), Command))
		{
			UE_LOG(LogCortex, Warning, TEXT("JSON missing 'command' field: %s"), *Line);
			FCortexCommandResult MissingCmd = FCortexCommandRouter::Error(
				TEXT("MISSING_COMMAND"),
				TEXT("JSON request missing 'command' field")
			);
			FString MissingCommandRequestId;
			RequestJson->TryGetStringField(TEXT("id"), MissingCommandRequestId);
			SendResponse(InClientSocket, FCortexCommandRouter::ResultToJson(MissingCmd, 0.0, MissingCommandRequestId));
			continue;
		}

		FString RequestId;
		RequestJson->TryGetStringField(TEXT("id"), RequestId);

		// Extract params (optional)
		const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
		TSharedPtr<FJsonObject> Params;
		if (RequestJson->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr != nullptr)
		{
			Params = *ParamsPtr;
		}

		// Verbose logging: log incoming command
		const bool bLogCommands = UCortexSettings::Get()->bLogCommands;
		if (bLogCommands)
		{
			FString ParamsString;
			if (Params.IsValid())
			{
				TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
					TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ParamsString);
				FJsonSerializer::Serialize(Params.ToSharedRef(), Writer);
			}
			constexpr int32 MaxParamsLength = 200;
			if (ParamsString.Len() > MaxParamsLength)
			{
				ParamsString = ParamsString.Left(MaxParamsLength) + TEXT("...");
			}
			UE_LOG(LogCortex, Log, TEXT("[Cortex] <- %s %s"), *Command, *ParamsString);
		}

		// Execute command with timing
		const double StartTime = FPlatformTime::Seconds();
		const int32 DeferredId = NextDeferredId++;
		FCortexCommandResult Result = CommandDispatcher(
			Command,
			Params,
			[this, DeferredId](FCortexCommandResult DeferredResult)
			{
				SendDeferredResponse(DeferredId, DeferredResult);
			});
		const double EndTime = FPlatformTime::Seconds();
		const double TimingMs = (EndTime - StartTime) * 1000.0;
		const double TimingSeconds = EndTime - StartTime;

		if (TimingSeconds > CommandTimeoutWarningSeconds)
		{
			UE_LOG(LogCortex, Warning, TEXT("Command '%s' took %.1fs (threshold: %.0fs)"), *Command, TimingSeconds, CommandTimeoutWarningSeconds);
		}

		// Verbose logging: log command result
		if (bLogCommands)
		{
			if (Result.bSuccess)
			{
				// Try to find a countable array in the result data
				int32 ResultCount = -1;
				if (Result.Data.IsValid())
				{
					for (const auto& Pair : Result.Data->Values)
					{
						if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Array)
						{
							ResultCount = Pair.Value->AsArray().Num();
							break;
						}
					}
				}

				if (ResultCount >= 0)
				{
					UE_LOG(LogCortex, Log, TEXT("[Cortex] -> SUCCESS (%.1fms, %d results)"), TimingMs, ResultCount);
				}
				else
				{
					UE_LOG(LogCortex, Log, TEXT("[Cortex] -> SUCCESS (%.1fms)"), TimingMs);
				}
			}
			else
			{
				UE_LOG(LogCortex, Log, TEXT("[Cortex] -> ERROR %s (%.1fms)"), *Result.ErrorCode, TimingMs);
			}
		}

		if (Result.bIsDeferred)
		{
			FCortexPendingDeferred Pending;
			Pending.ClientSocket = InClientSocket;
			Pending.RequestId = RequestId;
			Pending.StartTime = StartTime;
			Pending.TimeoutSeconds = DefaultDeferredTimeoutSeconds;
			PendingDeferred.Add(DeferredId, Pending);

			TSharedRef<FJsonObject> AckJson = MakeShared<FJsonObject>();
			if (!RequestId.IsEmpty())
			{
				AckJson->SetStringField(TEXT("id"), RequestId);
			}
			AckJson->SetStringField(TEXT("status"), TEXT("deferred"));
			AckJson->SetBoolField(TEXT("success"), true);
			TSharedPtr<FJsonObject> AckData = MakeShared<FJsonObject>();
			AckData->SetStringField(TEXT("status"), TEXT("deferred"));
			AckData->SetNumberField(TEXT("timeout_seconds"), Pending.TimeoutSeconds);
			AckJson->SetObjectField(TEXT("data"), AckData);

			FString AckString;
			TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> AckWriter =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&AckString);
			FJsonSerializer::Serialize(AckJson, AckWriter);

			SendResponse(InClientSocket, AckString);
			continue;
		}

		SendResponse(InClientSocket, FCortexCommandRouter::ResultToJson(Result, TimingMs, RequestId));
	}

	return true;
}

void FCortexTcpServer::SendResponse(FSocket* InClientSocket, const FString& ResponseString)
{
	if (InClientSocket == nullptr)
	{
		return;
	}

	FString ResponseWithNewline = ResponseString + TEXT("\n");
	FTCHARToUTF8 Utf8Response(*ResponseWithNewline);

	int32 BytesSent = 0;
	if (!InClientSocket->Send(
		reinterpret_cast<const uint8*>(Utf8Response.Get()),
		Utf8Response.Length(),
		BytesSent))
	{
		UE_LOG(LogCortex, Warning, TEXT("Failed to send response"));
	}
}

void FCortexTcpServer::DestroyClientSocket(FSocket* InClientSocket)
{
	if (InClientSocket == nullptr)
	{
		return;
	}

	ReceiveBuffers.Remove(InClientSocket);

	TArray<int32> DeferredIdsToRemove;
	for (const TPair<int32, FCortexPendingDeferred>& Pair : PendingDeferred)
	{
		if (Pair.Value.ClientSocket == InClientSocket)
		{
			DeferredIdsToRemove.Add(Pair.Key);
		}
	}
	for (const int32 DeferredId : DeferredIdsToRemove)
	{
		PendingDeferred.Remove(DeferredId);
	}

	InClientSocket->Close();
	ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(InClientSocket);

	check(IsInGameThread());
	if (ClientDisconnectCallback)
	{
		ClientDisconnectCallback();
	}
}

void FCortexTcpServer::SendDeferredResponse(int32 DeferredId, const FCortexCommandResult& Result)
{
	check(IsInGameThread());

	FCortexPendingDeferred* Pending = PendingDeferred.Find(DeferredId);
	if (Pending == nullptr || Pending->ClientSocket == nullptr)
	{
		return;
	}

	const double TimingMs = (FPlatformTime::Seconds() - Pending->StartTime) * 1000.0;
	FString FinalResponse = FCortexCommandRouter::ResultToJson(Result, TimingMs, Pending->RequestId);

	TSharedPtr<FJsonObject> ResponseJson;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FinalResponse);
	if (FJsonSerializer::Deserialize(Reader, ResponseJson) && ResponseJson.IsValid())
	{
		ResponseJson->SetStringField(TEXT("status"), TEXT("complete"));

		FString CompleteString;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&CompleteString);
		FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);
		SendResponse(Pending->ClientSocket, CompleteString);
	}
	else
	{
		SendResponse(Pending->ClientSocket, FinalResponse);
	}

	PendingDeferred.Remove(DeferredId);
}

void FCortexTcpServer::CheckDeferredTimeouts()
{
	TArray<int32> TimedOutDeferred;
	const double Now = FPlatformTime::Seconds();

	for (const TPair<int32, FCortexPendingDeferred>& Pair : PendingDeferred)
	{
		const FCortexPendingDeferred& Pending = Pair.Value;
		if (Now - Pending.StartTime >= Pending.TimeoutSeconds)
		{
			TimedOutDeferred.Add(Pair.Key);
		}
	}

	for (const int32 DeferredId : TimedOutDeferred)
	{
		FCortexCommandResult TimeoutResult = FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("Deferred command timed out"));
		SendDeferredResponse(DeferredId, TimeoutResult);
	}
}
