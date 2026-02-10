#include "CortexDataCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexDataTableOps.h"
#include "Operations/CortexGameplayTagOps.h"
#include "Operations/CortexDataAssetOps.h"
#include "Operations/CortexLocalizationOps.h"
#include "Operations/CortexAssetSearchOps.h"
#include "Operations/CortexCurveTableOps.h"

FUDBCommandResult FCortexDataCommandHandler::Execute(
    const FString& Command,
    const TSharedPtr<FJsonObject>& Params)
{
    // Note: no longer static - virtual override of ICortexDomainHandler
    // DataTable operations
    if (Command == TEXT("list_datatables"))
    {
        return FUDBDataTableOps::ListDatatables(Params);
    }
    if (Command == TEXT("get_datatable_schema"))
    {
        return FUDBDataTableOps::GetDatatableSchema(Params);
    }
    if (Command == TEXT("query_datatable"))
    {
        return FUDBDataTableOps::QueryDatatable(Params);
    }
    if (Command == TEXT("get_datatable_row"))
    {
        return FUDBDataTableOps::GetDatatableRow(Params);
    }
    if (Command == TEXT("get_struct_schema"))
    {
        return FUDBDataTableOps::GetStructSchema(Params);
    }
    if (Command == TEXT("add_datatable_row"))
    {
        return FUDBDataTableOps::AddDatatableRow(Params);
    }
    if (Command == TEXT("update_datatable_row"))
    {
        return FUDBDataTableOps::UpdateDatatableRow(Params);
    }
    if (Command == TEXT("delete_datatable_row"))
    {
        return FUDBDataTableOps::DeleteDatatableRow(Params);
    }
    if (Command == TEXT("import_datatable_json"))
    {
        return FUDBDataTableOps::ImportDatatableJson(Params);
    }
    if (Command == TEXT("search_datatable_content"))
    {
        return FUDBDataTableOps::SearchDatatableContent(Params);
    }
    if (Command == TEXT("get_data_catalog"))
    {
        return FUDBDataTableOps::GetDataCatalog(Params);
    }
    if (Command == TEXT("resolve_tags"))
    {
        return FUDBDataTableOps::ResolveTags(Params);
    }

    // GameplayTag operations
    if (Command == TEXT("list_gameplay_tags"))
    {
        return FUDBGameplayTagOps::ListGameplayTags(Params);
    }
    if (Command == TEXT("validate_gameplay_tag"))
    {
        return FUDBGameplayTagOps::ValidateGameplayTag(Params);
    }
    if (Command == TEXT("register_gameplay_tag"))
    {
        return FUDBGameplayTagOps::RegisterGameplayTag(Params);
    }
    if (Command == TEXT("register_gameplay_tags"))
    {
        return FUDBGameplayTagOps::RegisterGameplayTags(Params);
    }

    // DataAsset operations
    if (Command == TEXT("list_data_assets"))
    {
        return FUDBDataAssetOps::ListDataAssets(Params);
    }
    if (Command == TEXT("get_data_asset"))
    {
        return FUDBDataAssetOps::GetDataAsset(Params);
    }
    if (Command == TEXT("update_data_asset"))
    {
        return FUDBDataAssetOps::UpdateDataAsset(Params);
    }

    // Localization operations
    if (Command == TEXT("list_string_tables"))
    {
        return FUDBLocalizationOps::ListStringTables(Params);
    }
    if (Command == TEXT("get_translations"))
    {
        return FUDBLocalizationOps::GetTranslations(Params);
    }
    if (Command == TEXT("set_translation"))
    {
        return FUDBLocalizationOps::SetTranslation(Params);
    }

    // Asset search
    if (Command == TEXT("search_assets"))
    {
        return FUDBAssetSearchOps::SearchAssets(Params);
    }

    // CurveTable operations
    if (Command == TEXT("list_curve_tables"))
    {
        return FUDBCurveTableOps::ListCurveTables(Params);
    }
    if (Command == TEXT("get_curve_table"))
    {
        return FUDBCurveTableOps::GetCurveTable(Params);
    }
    if (Command == TEXT("update_curve_table_row"))
    {
        return FUDBCurveTableOps::UpdateCurveTableRow(Params);
    }

    return FUDBCommandHandler::Error(
        UDBErrorCodes::UnknownCommand,
        FString::Printf(TEXT("Unknown data command: %s"), *Command)
    );
}

TArray<FCortexCommandInfo> FCortexDataCommandHandler::GetSupportedCommands() const
{
    return {
        { TEXT("list_datatables"), TEXT("List all DataTables") },
        { TEXT("get_datatable_schema"), TEXT("Get row struct schema") },
        { TEXT("query_datatable"), TEXT("Query rows with filtering") },
        { TEXT("get_datatable_row"), TEXT("Get single row by name") },
        { TEXT("get_struct_schema"), TEXT("Get schema for any UStruct") },
        { TEXT("add_datatable_row"), TEXT("Add new row") },
        { TEXT("update_datatable_row"), TEXT("Update existing row") },
        { TEXT("delete_datatable_row"), TEXT("Delete row") },
        { TEXT("import_datatable_json"), TEXT("Bulk import rows") },
        { TEXT("search_datatable_content"), TEXT("Full-text search in tables") },
        { TEXT("get_data_catalog"), TEXT("Discovery catalog of all data") },
        { TEXT("resolve_tags"), TEXT("Look up rows by GameplayTag") },
        { TEXT("list_gameplay_tags"), TEXT("List GameplayTags by prefix") },
        { TEXT("validate_gameplay_tag"), TEXT("Check if tag is registered") },
        { TEXT("register_gameplay_tag"), TEXT("Register single tag") },
        { TEXT("register_gameplay_tags"), TEXT("Batch register tags") },
        { TEXT("list_data_assets"), TEXT("List DataAssets") },
        { TEXT("get_data_asset"), TEXT("Get DataAsset properties") },
        { TEXT("update_data_asset"), TEXT("Update DataAsset properties") },
        { TEXT("list_string_tables"), TEXT("List StringTables") },
        { TEXT("get_translations"), TEXT("Get StringTable entries") },
        { TEXT("set_translation"), TEXT("Set StringTable entry") },
        { TEXT("search_assets"), TEXT("Asset Registry search") },
        { TEXT("list_curve_tables"), TEXT("List CurveTables") },
        { TEXT("get_curve_table"), TEXT("Get curve rows") },
        { TEXT("update_curve_table_row"), TEXT("Update curve row") },
    };
}
