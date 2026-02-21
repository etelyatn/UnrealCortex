#include "CortexCoreCommandHandler.h"
#include "CortexCommandRouter.h"

FCortexCommandResult FCortexCoreCommandHandler::Execute(
	const FString& Command,
	const TSharedPtr<FJsonObject>& Params,
	FDeferredResponseCallback DeferredCallback)
{
	(void)Params;
	(void)DeferredCallback;

	return FCortexCommandRouter::Error(
		CortexErrorCodes::UnknownCommand,
		FString::Printf(TEXT("Unknown core command: %s"), *Command)
	);
}

TArray<FCortexCommandInfo> FCortexCoreCommandHandler::GetSupportedCommands() const
{
	return {};
}
