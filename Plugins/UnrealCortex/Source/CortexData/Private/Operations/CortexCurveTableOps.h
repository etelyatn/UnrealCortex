
#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UCurveTable;

class FCortexDataCurveTableOps
{
public:
	static FCortexCommandResult ListCurveTables(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetCurveTable(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult UpdateCurveTableRow(const TSharedPtr<FJsonObject>& Params);

private:
	/** Load a CurveTable by asset path, returns nullptr and sets OutError if not found */
	static UCurveTable* LoadCurveTable(const FString& TablePath, FCortexCommandResult& OutError);
};
