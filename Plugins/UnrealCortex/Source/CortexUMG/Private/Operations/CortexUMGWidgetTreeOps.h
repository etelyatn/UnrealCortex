#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UWidget;

class FCortexUMGWidgetTreeOps
{
public:
    static FCortexCommandResult AddWidget(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult RemoveWidget(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult Reparent(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult GetTree(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult GetWidget(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult ListWidgetClasses(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult DuplicateWidget(const TSharedPtr<FJsonObject>& Params);

private:
    static UClass* ResolveWidgetClass(const FString& ClassName);
    static TSharedPtr<FJsonObject> BuildWidgetTreeJson(UWidget* Widget);
};
