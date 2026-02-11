
#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UStringTable;

class FCortexDataLocalizationOps
{
public:
	static FCortexCommandResult ListStringTables(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetTranslations(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult SetTranslation(const TSharedPtr<FJsonObject>& Params);

private:
	/** Load a StringTable by asset path, returns nullptr and sets OutError if not found */
	static UStringTable* LoadStringTable(const FString& TablePath, FCortexCommandResult& OutError);
};
