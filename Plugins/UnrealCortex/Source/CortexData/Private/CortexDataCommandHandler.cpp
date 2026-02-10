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
