
#include "CortexCommandRouter.h"
#include "ICortexDomainHandler.h"
#include "Misc/EngineVersion.h"
#include "Misc/App.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogUDBCommandHandler, Log, All);

FUDBCommandResult FUDBCommandHandler::Execute(const FString& Command, const TSharedPtr<FJsonObject>& Params)
{
	// Built-in commands (no namespace)
	if (Command == TEXT("ping"))
	{
		return HandlePing(Params);
	}
	if (Command == TEXT("get_status"))
	{
		return HandleGetStatus(Params);
	}
	if (Command == TEXT("get_capabilities"))
	{
		return HandleGetCapabilities(Params);
	}
	if (Command == TEXT("batch"))
	{
		return HandleBatch(Params);
	}

	// Namespace routing: "data.list_datatables" -> domain "data", command "list_datatables"
	FString Namespace;
	FString SubCommand;

	if (Command.Split(TEXT("."), &Namespace, &SubCommand))
	{
		for (const FRegisteredDomain& Domain : RegisteredDomains)
		{
			if (Domain.Namespace == Namespace)
			{
				return Domain.Handler->Execute(SubCommand, Params);
			}
		}

		return Error(UDBErrorCodes::UnknownCommand,
			FString::Printf(TEXT("Unknown domain: %s"), *Namespace));
	}

	// Fallback: try all domains without namespace (backward compat for Phase 1 tests)
	for (const FRegisteredDomain& Domain : RegisteredDomains)
	{
		for (const FCortexCommandInfo& CmdInfo : Domain.Handler->GetSupportedCommands())
		{
			if (CmdInfo.Name == Command)
			{
				return Domain.Handler->Execute(Command, Params);
			}
		}
	}

	// Legacy default handler (Phase 1 compat, removed in Task 23)
	if (DefaultHandler)
	{
		return DefaultHandler(Command, Params);
	}

	UE_LOG(LogUDBCommandHandler, Warning, TEXT("Unknown command: %s"), *Command);
	return Error(UDBErrorCodes::UnknownCommand, FString::Printf(TEXT("Unknown command: %s"), *Command));
}

FString FUDBCommandHandler::ResultToJson(const FUDBCommandResult& Result, double TimingMs)
{
	TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
	ResponseJson->SetBoolField(TEXT("success"), Result.bSuccess);

	if (Result.bSuccess)
	{
		if (Result.Data.IsValid())
		{
			ResponseJson->SetObjectField(TEXT("data"), Result.Data);
		}

		if (Result.Warnings.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> WarningsArray;
			for (const FString& Warning : Result.Warnings)
			{
				WarningsArray.Add(MakeShared<FJsonValueString>(Warning));
			}
			ResponseJson->SetArrayField(TEXT("warnings"), WarningsArray);
		}
	}
	else
	{
		TSharedRef<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
		ErrorObj->SetStringField(TEXT("code"), Result.ErrorCode);
		ErrorObj->SetStringField(TEXT("message"), Result.ErrorMessage);

		if (Result.ErrorDetails.IsValid())
		{
			ErrorObj->SetObjectField(TEXT("details"), Result.ErrorDetails);
		}

		ResponseJson->SetObjectField(TEXT("error"), ErrorObj);
	}

	ResponseJson->SetNumberField(TEXT("timing_ms"), TimingMs);

	FString OutputString;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
	FJsonSerializer::Serialize(ResponseJson, Writer);

	return OutputString;
}

FUDBCommandResult FUDBCommandHandler::Success(TSharedPtr<FJsonObject> Data)
{
	FUDBCommandResult Result;
	Result.bSuccess = true;
	Result.Data = MoveTemp(Data);
	return Result;
}

FUDBCommandResult FUDBCommandHandler::Error(const FString& Code, const FString& Message, TSharedPtr<FJsonObject> Details)
{
	FUDBCommandResult Result;
	Result.bSuccess = false;
	Result.ErrorCode = Code;
	Result.ErrorMessage = Message;
	Result.ErrorDetails = MoveTemp(Details);
	return Result;
}

void FUDBCommandHandler::SetDefaultHandler(FDefaultHandler InHandler)
{
	DefaultHandler = MoveTemp(InHandler);
}

void FUDBCommandHandler::RegisterDomain(
	const FString& Namespace,
	const FString& DisplayName,
	const FString& Version,
	TSharedPtr<ICortexDomainHandler> Handler)
{
	RegisteredDomains.Add({ Namespace, DisplayName, Version, Handler });
	UE_LOG(LogUDBCommandHandler, Log, TEXT("Registered domain: %s (%s v%s)"),
		*Namespace, *DisplayName, *Version);
}

const TArray<FRegisteredDomain>& FUDBCommandHandler::GetRegisteredDomains() const
{
	return RegisteredDomains;
}

FUDBCommandResult FUDBCommandHandler::HandlePing(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("message"), TEXT("pong"));
	return Success(Data);
}

FUDBCommandResult FUDBCommandHandler::HandleBatch(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* CommandsArray = nullptr;
	if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("commands"), CommandsArray) || CommandsArray == nullptr)
	{
		return Error(UDBErrorCodes::InvalidField, TEXT("Missing required param: commands (array)"));
	}

	if (CommandsArray->Num() > MaxBatchSize)
	{
		return Error(
			UDBErrorCodes::BatchLimitExceeded,
			FString::Printf(TEXT("Batch size %d exceeds maximum of %d"), CommandsArray->Num(), MaxBatchSize)
		);
	}

	const double BatchStartTime = FPlatformTime::Seconds();

	TArray<TSharedPtr<FJsonValue>> ResultsArray;

	for (int32 Index = 0; Index < CommandsArray->Num(); ++Index)
	{
		TSharedRef<FJsonObject> EntryResult = MakeShared<FJsonObject>();
		EntryResult->SetNumberField(TEXT("index"), Index);

		const TSharedPtr<FJsonValue>& CmdVal = (*CommandsArray)[Index];
		const TSharedPtr<FJsonObject>* CmdObj = nullptr;

		if (!CmdVal.IsValid() || !CmdVal->TryGetObject(CmdObj) || CmdObj == nullptr)
		{
			EntryResult->SetStringField(TEXT("command"), TEXT(""));
			EntryResult->SetBoolField(TEXT("success"), false);
			EntryResult->SetStringField(TEXT("error_code"), UDBErrorCodes::InvalidField);
			EntryResult->SetStringField(TEXT("error_message"), TEXT("Invalid command entry (not an object)"));
			EntryResult->SetNumberField(TEXT("timing_ms"), 0.0);
			ResultsArray.Add(MakeShared<FJsonValueObject>(EntryResult));
			continue;
		}

		FString SubCommand;
		(*CmdObj)->TryGetStringField(TEXT("command"), SubCommand);
		EntryResult->SetStringField(TEXT("command"), SubCommand);

		// Block nested batch
		if (SubCommand == TEXT("batch"))
		{
			EntryResult->SetBoolField(TEXT("success"), false);
			EntryResult->SetStringField(TEXT("error_code"), UDBErrorCodes::BatchRecursionBlocked);
			EntryResult->SetStringField(TEXT("error_message"), TEXT("Nested batch commands are not allowed"));
			EntryResult->SetNumberField(TEXT("timing_ms"), 0.0);
			ResultsArray.Add(MakeShared<FJsonValueObject>(EntryResult));
			continue;
		}

		TSharedPtr<FJsonObject> SubParams;
		const TSharedPtr<FJsonObject>* SubParamsPtr = nullptr;
		if ((*CmdObj)->TryGetObjectField(TEXT("params"), SubParamsPtr) && SubParamsPtr != nullptr)
		{
			SubParams = *SubParamsPtr;
		}
		else
		{
			SubParams = MakeShared<FJsonObject>();
		}

		const double CmdStartTime = FPlatformTime::Seconds();
		FUDBCommandResult SubResult = Execute(SubCommand, SubParams);
		const double CmdElapsed = (FPlatformTime::Seconds() - CmdStartTime) * 1000.0;

		EntryResult->SetBoolField(TEXT("success"), SubResult.bSuccess);
		EntryResult->SetNumberField(TEXT("timing_ms"), CmdElapsed);

		if (SubResult.bSuccess)
		{
			if (SubResult.Data.IsValid())
			{
				EntryResult->SetObjectField(TEXT("data"), SubResult.Data);
			}
		}
		else
		{
			EntryResult->SetStringField(TEXT("error_code"), SubResult.ErrorCode);
			EntryResult->SetStringField(TEXT("error_message"), SubResult.ErrorMessage);
		}

		ResultsArray.Add(MakeShared<FJsonValueObject>(EntryResult));
	}

	const double BatchElapsed = (FPlatformTime::Seconds() - BatchStartTime) * 1000.0;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("results"), ResultsArray);
	Data->SetNumberField(TEXT("count"), ResultsArray.Num());
	Data->SetNumberField(TEXT("total_timing_ms"), BatchElapsed);

	return Success(Data);
}

FUDBCommandResult FUDBCommandHandler::HandleGetStatus(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("connected"), true);
	Data->SetStringField(TEXT("plugin_version"), TEXT("0.1.0"));

	// Engine version
	Data->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());

	// Project name
	Data->SetStringField(TEXT("project_name"), FApp::GetProjectName());

	// Subsystem availability
	TSharedPtr<FJsonObject> Subsystems = MakeShared<FJsonObject>();
	Subsystems->SetBoolField(TEXT("asset_registry"), IAssetRegistry::Get() != nullptr);
	Subsystems->SetBoolField(TEXT("gameplay_tags"), true);
	Subsystems->SetBoolField(TEXT("localization"), true);
	Data->SetObjectField(TEXT("subsystems"), Subsystems);

	return Success(Data);
}

FUDBCommandResult FUDBCommandHandler::HandleGetCapabilities(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("plugin_version"), TEXT("0.1.0"));

	TSharedPtr<FJsonObject> Domains = MakeShared<FJsonObject>();

	for (const FRegisteredDomain& Domain : RegisteredDomains)
	{
		TSharedPtr<FJsonObject> DomainObj = MakeShared<FJsonObject>();
		DomainObj->SetStringField(TEXT("name"), Domain.DisplayName);
		DomainObj->SetStringField(TEXT("version"), Domain.Version);

		TArray<TSharedPtr<FJsonValue>> CommandArray;
		for (const FCortexCommandInfo& CmdInfo : Domain.Handler->GetSupportedCommands())
		{
			TSharedPtr<FJsonObject> CmdObj = MakeShared<FJsonObject>();
			CmdObj->SetStringField(TEXT("name"), CmdInfo.Name);
			CmdObj->SetStringField(TEXT("description"), CmdInfo.Description);
			CommandArray.Add(MakeShared<FJsonValueObject>(CmdObj));
		}
		DomainObj->SetArrayField(TEXT("commands"), CommandArray);

		Domains->SetObjectField(Domain.Namespace, DomainObj);
	}

	Data->SetObjectField(TEXT("domains"), Domains);
	return Success(Data);
}
