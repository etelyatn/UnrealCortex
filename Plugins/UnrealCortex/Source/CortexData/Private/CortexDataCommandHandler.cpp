#include "CortexDataCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexDataTableOps.h"
#include "Operations/CortexGameplayTagOps.h"
#include "Operations/CortexDataAssetOps.h"
#include "Operations/CortexLocalizationOps.h"
#include "Operations/CortexAssetSearchOps.h"
#include "Operations/CortexCurveTableOps.h"

FCortexCommandResult FCortexDataCommandHandler::Execute(
    const FString& Command,
    const TSharedPtr<FJsonObject>& Params)
{
    // Note: no longer static - virtual override of ICortexDomainHandler
    // DataTable operations
    if (Command == TEXT("list_datatables"))
    {
        return FCortexDataTableOps::ListDatatables(Params);
    }
    if (Command == TEXT("get_datatable_schema"))
    {
        return FCortexDataTableOps::GetDatatableSchema(Params);
    }
    if (Command == TEXT("query_datatable"))
    {
        return FCortexDataTableOps::QueryDatatable(Params);
    }
    if (Command == TEXT("get_datatable_row"))
    {
        return FCortexDataTableOps::GetDatatableRow(Params);
    }
    if (Command == TEXT("get_struct_schema"))
    {
        return FCortexDataTableOps::GetStructSchema(Params);
    }
    if (Command == TEXT("add_datatable_row"))
    {
        return FCortexDataTableOps::AddDatatableRow(Params);
    }
    if (Command == TEXT("update_datatable_row"))
    {
        return FCortexDataTableOps::UpdateDatatableRow(Params);
    }
    if (Command == TEXT("delete_datatable_row"))
    {
        return FCortexDataTableOps::DeleteDatatableRow(Params);
    }
    if (Command == TEXT("import_datatable_json"))
    {
        return FCortexDataTableOps::ImportDatatableJson(Params);
    }
    if (Command == TEXT("search_datatable_content"))
    {
        return FCortexDataTableOps::SearchDatatableContent(Params);
    }
    if (Command == TEXT("get_data_catalog"))
    {
        return FCortexDataTableOps::GetDataCatalog(Params);
    }
    if (Command == TEXT("resolve_tags"))
    {
        return FCortexDataTableOps::ResolveTags(Params);
    }

    // GameplayTag operations
    if (Command == TEXT("list_gameplay_tags"))
    {
        return FCortexGameplayTagOps::ListGameplayTags(Params);
    }
    if (Command == TEXT("validate_gameplay_tag"))
    {
        return FCortexGameplayTagOps::ValidateGameplayTag(Params);
    }
    if (Command == TEXT("register_gameplay_tag"))
    {
        return FCortexGameplayTagOps::RegisterGameplayTag(Params);
    }
    if (Command == TEXT("register_gameplay_tags"))
    {
        return FCortexGameplayTagOps::RegisterGameplayTags(Params);
    }

    // DataAsset operations
    if (Command == TEXT("list_data_assets"))
    {
        return FCortexDataAssetOps::ListDataAssets(Params);
    }
    if (Command == TEXT("get_data_asset"))
    {
        return FCortexDataAssetOps::GetDataAsset(Params);
    }
    if (Command == TEXT("update_data_asset"))
    {
        return FCortexDataAssetOps::UpdateDataAsset(Params);
    }

    // Localization operations
    if (Command == TEXT("list_string_tables"))
    {
        return FCortexLocalizationOps::ListStringTables(Params);
    }
    if (Command == TEXT("get_translations"))
    {
        return FCortexLocalizationOps::GetTranslations(Params);
    }
    if (Command == TEXT("set_translation"))
    {
        return FCortexLocalizationOps::SetTranslation(Params);
    }

    // Asset search
    if (Command == TEXT("search_assets"))
    {
        return FCortexAssetSearchOps::SearchAssets(Params);
    }

    // CurveTable operations
    if (Command == TEXT("list_curve_tables"))
    {
        return FCortexCurveTableOps::ListCurveTables(Params);
    }
    if (Command == TEXT("get_curve_table"))
    {
        return FCortexCurveTableOps::GetCurveTable(Params);
    }
    if (Command == TEXT("update_curve_table_row"))
    {
        return FCortexCurveTableOps::UpdateCurveTableRow(Params);
    }

    return FCortexCommandRouter::Error(
        CortexErrorCodes::UnknownCommand,
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
