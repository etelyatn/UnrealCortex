#include "CortexMaterialCommandHandler.h"
#include "CortexCommandRouter.h"

FCortexCommandResult FCortexMaterialCommandHandler::Execute(
	const FString& Command,
	const TSharedPtr<FJsonObject>& Params)
{
	return FCortexCommandRouter::Error(
		CortexErrorCodes::UnknownCommand,
		FString::Printf(TEXT("Unknown material command: %s"), *Command)
	);
}

TArray<FCortexCommandInfo> FCortexMaterialCommandHandler::GetSupportedCommands() const
{
	return {};
}
