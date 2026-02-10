
#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UCurveTable;

class FUDBCurveTableOps
{
public:
	static FUDBCommandResult ListCurveTables(const TSharedPtr<FJsonObject>& Params);
	static FUDBCommandResult GetCurveTable(const TSharedPtr<FJsonObject>& Params);
	static FUDBCommandResult UpdateCurveTableRow(const TSharedPtr<FJsonObject>& Params);

private:
	/** Load a CurveTable by asset path, returns nullptr and sets OutError if not found */
	static UCurveTable* LoadCurveTable(const FString& TablePath, FUDBCommandResult& OutError);
};
