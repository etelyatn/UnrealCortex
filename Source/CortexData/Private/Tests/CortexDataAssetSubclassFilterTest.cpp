#include "Misc/AutomationTest.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "CortexTcpServer.h"
#include "CortexCommandRouter.h"
#include "CortexDataCommandHandler.h"
#include "Tests/CortexTestDataAsset.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "AssetRegistry/AssetRegistryModule.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataAssetSubclassFilterTest,
	"Cortex.Data.DataAssetSubclassFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

namespace
{
	bool SendCommandAndReadResponse(
		FSocket* Socket,
		const FString& JsonCommand,
		FString& OutResponse,
		float TimeoutSeconds = 3.0f)
	{
		FTCHARToUTF8 Utf8(*JsonCommand);
		int32 BytesSent = 0;
		if (!Socket->Send(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), BytesSent))
		{
			return false;
		}

		OutResponse.Empty();
		uint8 RecvBuffer[65536];
		int32 BytesRead = 0;
		const int32 MaxAttempts = FMath::CeilToInt(TimeoutSeconds / 0.05f);

		for (int32 Attempt = 0; Attempt < MaxAttempts; ++Attempt)
		{
			FTSTicker::GetCoreTicker().Tick(0.016f);
			FPlatformProcess::Sleep(0.05f);

			uint32 PendingDataSize = 0;
			while (Socket->HasPendingData(PendingDataSize) && PendingDataSize > 0)
			{
				if (Socket->Recv(RecvBuffer, sizeof(RecvBuffer) - 1, BytesRead) && BytesRead > 0)
				{
					RecvBuffer[BytesRead] = 0;
					FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(RecvBuffer), BytesRead);
					OutResponse.Append(Converter.Get(), Converter.Length());
				}
				else
				{
					break;
				}
			}

			if (OutResponse.Contains(TEXT("\n")))
			{
				OutResponse.TrimStartAndEndInline();
				return true;
			}
		}

		return false;
	}

	int32 ParseCountFromResponse(const FString& ResponseString)
	{
		TSharedPtr<FJsonObject> JsonObject;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseString);
		if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
		{
			return -1;
		}

		bool bSuccess = false;
		if (!JsonObject->TryGetBoolField(TEXT("success"), bSuccess) || !bSuccess)
		{
			return -1;
		}

		const TSharedPtr<FJsonObject>* DataObject = nullptr;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObject) || DataObject == nullptr)
		{
			return -1;
		}

		double Count = -1.0;
		if (!(*DataObject)->TryGetNumberField(TEXT("count"), Count))
		{
			return -1;
		}

		return static_cast<int32>(Count);
	}

	FString ParseResolvedClassFromResponse(const FString& ResponseString)
	{
		TSharedPtr<FJsonObject> JsonObject;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseString);
		if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
		{
			return FString();
		}

		const TSharedPtr<FJsonObject>* DataObject = nullptr;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObject) || DataObject == nullptr)
		{
			return FString();
		}

		FString ResolvedClass;
		(*DataObject)->TryGetStringField(TEXT("resolved_class"), ResolvedClass);
		return ResolvedClass;
	}
}

bool FCortexDataAssetSubclassFilterTest::RunTest(const FString& Parameters)
{
	UPackage* BasePackage = CreatePackage(TEXT("/Game/Temp/CortexTest_DA_Base"));
	UCortexTestDataAsset* BaseAsset = NewObject<UCortexTestDataAsset>(
		BasePackage,
		UCortexTestDataAsset::StaticClass(),
		TEXT("CortexTest_DA_Base"),
		RF_Public | RF_Standalone);
	FAssetRegistryModule::AssetCreated(BaseAsset);

	UPackage* DerivedPackage = CreatePackage(TEXT("/Game/Temp/CortexTest_DA_Derived"));
	UCortexDerivedTestDataAsset* DerivedAsset = NewObject<UCortexDerivedTestDataAsset>(
		DerivedPackage,
		UCortexDerivedTestDataAsset::StaticClass(),
		TEXT("CortexTest_DA_Derived"),
		RF_Public | RF_Standalone);
	FAssetRegistryModule::AssetCreated(DerivedAsset);

	const int32 TestPort = 18745;
	FCortexCommandRouter CommandHandler;
	CommandHandler.RegisterDomain(TEXT("data"), TEXT("Cortex Data"), TEXT("1.0.0"),
		MakeShared<FCortexDataCommandHandler>());
	FCortexTcpServer Server;
	const bool bStarted = Server.Start(TestPort,
		[&CommandHandler](const FString& Command, const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback)
		{
			return CommandHandler.Execute(Command, Params, MoveTemp(DeferredCallback));
		});
	TestTrue(TEXT("Server should start successfully"), bStarted);

	if (!bStarted)
	{
		BaseAsset->MarkAsGarbage();
		DerivedAsset->MarkAsGarbage();
		return true;
	}

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	FSocket* ClientSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("CortexTestClient"), false);
	TestNotNull(TEXT("Client socket should be created"), ClientSocket);

	if (ClientSocket == nullptr)
	{
		Server.Stop();
		BaseAsset->MarkAsGarbage();
		DerivedAsset->MarkAsGarbage();
		return true;
	}

	FIPv4Endpoint ServerEndpoint(FIPv4Address::InternalLoopback, TestPort);
	const bool bConnected = ClientSocket->Connect(*ServerEndpoint.ToInternetAddr());
	TestTrue(TEXT("Client should connect to server"), bConnected);

	if (!bConnected)
	{
		SocketSubsystem->DestroySocket(ClientSocket);
		Server.Stop();
		BaseAsset->MarkAsGarbage();
		DerivedAsset->MarkAsGarbage();
		return true;
	}

	FPlatformProcess::Sleep(0.1f);

	const FString PathFilter = TEXT("/Game/Temp/CortexTest_");

	FString Response1;
	const FString Cmd1 = FString::Printf(
		TEXT("{\"command\":\"data.list_data_assets\",\"params\":{\"path_filter\":\"%s\"}}\n"),
		*PathFilter);
	const bool bGotResponse1 = SendCommandAndReadResponse(ClientSocket, Cmd1, Response1);
	TestTrue(TEXT("Should receive response for baseline query"), bGotResponse1);

	const int32 BaselineCount = bGotResponse1 ? ParseCountFromResponse(Response1) : -1;
	TestEqual(TEXT("Baseline should find 2 test assets (base + derived)"), BaselineCount, 2);

	FString Response2;
	const FString Cmd2 = FString::Printf(
		TEXT("{\"command\":\"data.list_data_assets\",\"params\":{\"class_filter\":\"CortexTestDataAsset\",\"path_filter\":\"%s\"}}\n"),
		*PathFilter);
	const bool bGotResponse2 = SendCommandAndReadResponse(ClientSocket, Cmd2, Response2);
	TestTrue(TEXT("Should receive response for base test class filter"), bGotResponse2);

	if (bGotResponse2)
	{
		const int32 BaseClassCount = ParseCountFromResponse(Response2);
		TestEqual(TEXT("Filtering by base class 'CortexTestDataAsset' should return same count as no filter"),
			BaseClassCount, BaselineCount);

		const FString ResolvedClass = ParseResolvedClassFromResponse(Response2);
		TestEqual(TEXT("resolved_class should be 'CortexTestDataAsset'"), ResolvedClass, TEXT("CortexTestDataAsset"));
	}

	FString Response3;
	const FString Cmd3 = FString::Printf(
		TEXT("{\"command\":\"data.list_data_assets\",\"params\":{\"class_filter\":\"CortexDerivedTestDataAsset\",\"path_filter\":\"%s\"}}\n"),
		*PathFilter);
	const bool bGotResponse3 = SendCommandAndReadResponse(ClientSocket, Cmd3, Response3);
	TestTrue(TEXT("Should receive response for derived test class filter"), bGotResponse3);

	if (bGotResponse3)
	{
		const int32 DerivedCount = ParseCountFromResponse(Response3);
		TestEqual(TEXT("Filtering by 'CortexDerivedTestDataAsset' should return 1 (subclass only)"), DerivedCount, 1);

		const FString ResolvedClass = ParseResolvedClassFromResponse(Response3);
		TestEqual(TEXT("resolved_class should be 'CortexDerivedTestDataAsset'"), ResolvedClass, TEXT("CortexDerivedTestDataAsset"));
	}

	FString Response4;
	const bool bGotResponse4 = SendCommandAndReadResponse(
		ClientSocket,
		TEXT("{\"command\":\"data.list_data_assets\",\"params\":{\"class_filter\":\"NonExistentClass_CortexTest\"}}\n"),
		Response4);
	TestTrue(TEXT("Should receive response for non-existent class filter"), bGotResponse4);

	if (bGotResponse4)
	{
		const int32 NonExistentCount = ParseCountFromResponse(Response4);
		TestEqual(TEXT("Non-existent class filter should return 0 results"), NonExistentCount, 0);

		const FString ResolvedClass = ParseResolvedClassFromResponse(Response4);
		TestTrue(TEXT("resolved_class should be empty for non-existent class"), ResolvedClass.IsEmpty());
	}

	FString Response5;
	const bool bGotResponse5 = SendCommandAndReadResponse(
		ClientSocket,
		TEXT("{\"command\":\"data.list_data_assets\",\"params\":{\"class_filter\":\"Actor\"}}\n"),
		Response5);
	TestTrue(TEXT("Should receive response for non-DataAsset class filter"), bGotResponse5);

	if (bGotResponse5)
	{
		const int32 NonDataAssetCount = ParseCountFromResponse(Response5);
		TestEqual(TEXT("Non-DataAsset class filter should return 0 results"), NonDataAssetCount, 0);

		const FString ResolvedClass = ParseResolvedClassFromResponse(Response5);
		TestTrue(TEXT("resolved_class should be empty for non-DataAsset class"), ResolvedClass.IsEmpty());
	}

	SocketSubsystem->DestroySocket(ClientSocket);
	Server.Stop();

	BaseAsset->MarkAsGarbage();
	DerivedAsset->MarkAsGarbage();

	return true;
}
