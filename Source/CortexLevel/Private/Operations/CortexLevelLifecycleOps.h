#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class FCortexLevelLifecycleOps
{
public:
	static FCortexCommandResult ListTemplates(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult CreateLevel(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult OpenLevel(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult DuplicateLevel(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult RenameLevel(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult DeleteLevel(const TSharedPtr<FJsonObject>& Params);

private:
	static bool IsLevelCurrentlyOpen(const FString& ContentPath);
	static bool IsCurrentLevelDirty();
	static bool IsValidContentPath(const FString& Path);
	static bool DoesLevelExist(const FString& ContentPath);
};
