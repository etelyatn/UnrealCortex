#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexUMGWidgetPropertyOps
{
public:
    static FCortexCommandResult SetColor(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SetText(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SetFont(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SetBrush(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SetPadding(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SetAnchor(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SetAlignment(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SetSize(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SetVisibility(const TSharedPtr<FJsonObject>& Params);

    static FCortexCommandResult SetProperty(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult GetProperty(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult GetSchema(const TSharedPtr<FJsonObject>& Params);

private:
    static bool ParseColor(const FString& ColorString, FLinearColor& OutColor);
};
