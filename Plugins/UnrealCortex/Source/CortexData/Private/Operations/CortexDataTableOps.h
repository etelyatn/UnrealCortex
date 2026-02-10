
#pragma once

#include "CoreMinimal.h"
#include "CortexCommandRouter.h"

class UDataTable;
class UCompositeDataTable;

class FUDBDataTableOps
{
public:
	static FUDBCommandResult ListDatatables(const TSharedPtr<FJsonObject>& Params);
	static FUDBCommandResult GetDatatableSchema(const TSharedPtr<FJsonObject>& Params);
	static FUDBCommandResult QueryDatatable(const TSharedPtr<FJsonObject>& Params);
	static FUDBCommandResult GetDatatableRow(const TSharedPtr<FJsonObject>& Params);
	static FUDBCommandResult GetStructSchema(const TSharedPtr<FJsonObject>& Params);
	static FUDBCommandResult AddDatatableRow(const TSharedPtr<FJsonObject>& Params);
	static FUDBCommandResult UpdateDatatableRow(const TSharedPtr<FJsonObject>& Params);
	static FUDBCommandResult DeleteDatatableRow(const TSharedPtr<FJsonObject>& Params);
	static FUDBCommandResult ImportDatatableJson(const TSharedPtr<FJsonObject>& Params);
	static FUDBCommandResult SearchDatatableContent(const TSharedPtr<FJsonObject>& Params);
	static FUDBCommandResult GetDataCatalog(const TSharedPtr<FJsonObject>& Params);
	static FUDBCommandResult ResolveTags(const TSharedPtr<FJsonObject>& Params);

private:
	/** Load a DataTable by asset path, returns nullptr and sets OutError if not found */
	static UDataTable* LoadDataTable(const FString& TablePath, FUDBCommandResult& OutError);

	/** Get the parent/source tables from a UCompositeDataTable via reflection */
	static TArray<UDataTable*> GetParentTables(const UCompositeDataTable* CompositeTable);

	/** Build a JSON array of {name, path} entries for the parent tables */
	static TArray<TSharedPtr<FJsonValue>> GetParentTablesJsonArray(const UCompositeDataTable* CompositeTable);

	/** Find which source table actually owns a row (searches back-to-front for override semantics) */
	static UDataTable* FindSourceTableForRow(const UCompositeDataTable* CompositeTable, FName RowName);
};
