#pragma once

#include "CoreMinimal.h"
#include "ICortexDomainHandler.h"

class FCortexEditorPIEState;
class FCortexEditorLogCapture;

class FCortexEditorCommandHandler : public ICortexDomainHandler
{
public:
	FCortexEditorCommandHandler();
	virtual ~FCortexEditorCommandHandler() override;

	virtual FCortexCommandResult Execute(
		const FString& Command,
		const TSharedPtr<FJsonObject>& Params,
		FDeferredResponseCallback DeferredCallback = nullptr
	) override;

	virtual TArray<FCortexCommandInfo> GetSupportedCommands() const override;

private:
	TSharedPtr<FCortexEditorPIEState> PIEState;
	TUniquePtr<FCortexEditorLogCapture> LogCapture;
};
