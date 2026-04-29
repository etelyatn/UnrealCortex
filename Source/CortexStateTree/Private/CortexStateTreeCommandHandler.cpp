#include "CortexStateTreeCommandHandler.h"
#include "CortexStateTreeModule.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"

namespace
{
    constexpr const TCHAR* StateTreeDomainVersion = TEXT("1.0.0");
}

FCortexCommandResult FCortexStateTreeCommandHandler::Execute(
    const FString& Command,
    const TSharedPtr<FJsonObject>& Params,
    FDeferredResponseCallback DeferredCallback)
{
    if (Command == TEXT("get_status"))
    {
        return GetStatus(Params);
    }

    return FCortexCommandRouter::Error(
        CortexErrorCodes::UnknownCommand,
        FString::Printf(TEXT("Unknown statetree command: %s"), *Command)
    );
}

TArray<FCortexCommandInfo> FCortexStateTreeCommandHandler::GetSupportedCommands() const
{
    return {
        FCortexCommandInfo{ TEXT("get_status"), TEXT("Domain registration diagnostic. Returns version, command count, and registered status.") },
    };
}

FCortexCommandResult FCortexStateTreeCommandHandler::GetStatus(const TSharedPtr<FJsonObject>& Params) const
{
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("domain"), TEXT("statetree"));
    Data->SetStringField(TEXT("version"), StateTreeDomainVersion);
    Data->SetBoolField(TEXT("registered"), true);
    Data->SetNumberField(TEXT("command_count"), GetSupportedCommands().Num());

    FCortexCommandResult Result;
    Result.bSuccess = true;
    Result.Data = Data;
    return Result;
}
