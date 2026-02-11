
#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UDataTable;
class UCompositeDataTable;

class FCortexDataTableOps
{
public:
	static FCortexCommandResult ListDatatables(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetDatatableSchema(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult QueryDatatable(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetDatatableRow(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetStructSchema(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult AddDatatableRow(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult UpdateDatatableRow(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult DeleteDatatableRow(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult ImportDatatableJson(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult SearchDatatableContent(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult GetDataCatalog(const TSharedPtr<FJsonObject>& Params);
	static FCortexCommandResult ResolveTags(const TSharedPtr<FJsonObject>& Params);

private:
	/** Load a DataTable by asset path, returns nullptr and sets OutError if not found */
	static UDataTable* LoadDataTable(const FString& TablePath, FCortexCommandResult& OutError);

	/** Get the parent/source tables from a UCompositeDataTable via reflection */
	static TArray<UDataTable*> GetParentTables(const UCompositeDataTable* CompositeTable);

	/** Build a JSON array of {name, path} entries for the parent tables */
	static TArray<TSharedPtr<FJsonValue>> GetParentTablesJsonArray(const UCompositeDataTable* CompositeTable);

	/** Find which source table actually owns a row (searches back-to-front for override semantics) */
	static UDataTable* FindSourceTableForRow(const UCompositeDataTable* CompositeTable, FName RowName);
};
