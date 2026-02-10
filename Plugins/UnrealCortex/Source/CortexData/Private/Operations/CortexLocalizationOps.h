
#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UStringTable;

class FUDBLocalizationOps
{
public:
	static FUDBCommandResult ListStringTables(const TSharedPtr<FJsonObject>& Params);
	static FUDBCommandResult GetTranslations(const TSharedPtr<FJsonObject>& Params);
	static FUDBCommandResult SetTranslation(const TSharedPtr<FJsonObject>& Params);

private:
	/** Load a StringTable by asset path, returns nullptr and sets OutError if not found */
	static UStringTable* LoadStringTable(const FString& TablePath, FUDBCommandResult& OutError);
};
