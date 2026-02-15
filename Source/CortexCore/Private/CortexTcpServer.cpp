
#include "CortexTcpServer.h"
#include "CortexCoreModule.h"
#include "CortexCommandRouter.h"
#include "CortexSettings.h"
#include "Common/TcpListener.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

FCortexTcpServer::FCortexTcpServer()
{
}

FCortexTcpServer::~FCortexTcpServer()
{
	Stop();
}

bool FCortexTcpServer::Start(int32 StartPort, FCommandDispatcher InDispatcher)
{
	if (bRunning)
	{
		UE_LOG(LogCortex, Warning, TEXT("TCP server is already running"));
		return false;
	}

	CommandDispatcher = MoveTemp(InDispatcher);

	for (int32 Port = StartPort; Port < StartPort + 100; ++Port)
	{
		FIPv4Endpoint ListenEndpoint(FIPv4Address::InternalLoopback, Port);

		Listener = MakeUnique<FTcpListener>(ListenEndpoint);
		Listener->OnConnectionAccepted().BindRaw(this, &FCortexTcpServer::HandleConnectionAccepted);

		if (Listener->IsActive())
		{
			bRunning = true;

			// Write port file for MCP server auto-discovery
			FString PortFilePath = FPaths::ProjectSavedDir() / TEXT("CortexPort.txt");
			FFileHelper::SaveStringToFile(FString::FromInt(Port), *PortFilePath);
			UE_LOG(LogCortex, Log, TEXT("Wrote port file: %s (port %d)"), *PortFilePath, Port);

			TickDelegateHandle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([this](float DeltaTime) -> bool
				{
					if (bRunning)
					{
						ProcessClientData();
					}
					return bRunning;
				}),
				0.0f
			);

			UE_LOG(LogCortex, Log, TEXT("TCP server listening on 127.0.0.1:%d"), Port);
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

	// Delete port file
	FString PortFilePath = FPaths::ProjectSavedDir() / TEXT("CortexPort.txt");
	IFileManager::Get().Delete(*PortFilePath);

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
	ReceiveBuffers.Empty();

	Listener.Reset();

	UE_LOG(LogCortex, Log, TEXT("TCP server stopped"));
}

bool FCortexTcpServer::IsRunning() const
{
	return bRunning;
}

bool FCortexTcpServer::HandleConnectionAccepted(FSocket* InClientSocket, const FIPv4Endpoint& ClientEndpoint)
{
	UE_LOG(LogCortex, Log, TEXT("Client connected from %s (total clients: %d)"), *ClientEndpoint.ToString(), ClientSockets.Num() + 1);
	ClientSockets.Add(InClientSocket);
	ReceiveBuffers.Add(InClientSocket, FString());
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
			SendResponse(InClientSocket, FCortexCommandRouter::ResultToJson(MissingCmd, 0.0));
			continue;
		}

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
		FCortexCommandResult Result = CommandDispatcher(Command, Params);
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

		SendResponse(InClientSocket, FCortexCommandRouter::ResultToJson(Result, TimingMs));
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
	InClientSocket->Close();
	ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(InClientSocket);
}
