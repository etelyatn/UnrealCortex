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
    const TSharedPtr<FJsonObject>& Params,
    FDeferredResponseCallback DeferredCallback)
{
    (void)DeferredCallback;

    // Note: no longer static - virtual override of ICortexDomainHandler
    // DataTable operations
    if (Command == TEXT("create_datatable"))
    {
        return FCortexDataTableOps::CreateDataTable(Params);
    }
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
        return FCortexDataGameplayTagOps::ListGameplayTags(Params);
    }
    if (Command == TEXT("validate_gameplay_tag"))
    {
        return FCortexDataGameplayTagOps::ValidateGameplayTag(Params);
    }
    if (Command == TEXT("register_gameplay_tag"))
    {
        return FCortexDataGameplayTagOps::RegisterGameplayTag(Params);
    }
    if (Command == TEXT("register_gameplay_tags"))
    {
        return FCortexDataGameplayTagOps::RegisterGameplayTags(Params);
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
    if (Command == TEXT("create_data_asset"))
    {
        return FCortexDataAssetOps::CreateDataAsset(Params);
    }
    if (Command == TEXT("delete_data_asset"))
    {
        return FCortexDataAssetOps::DeleteDataAsset(Params);
    }

    // Localization operations
    if (Command == TEXT("list_string_tables"))
    {
        return FCortexDataLocalizationOps::ListStringTables(Params);
    }
    if (Command == TEXT("get_translations"))
    {
        return FCortexDataLocalizationOps::GetTranslations(Params);
    }
    if (Command == TEXT("set_translation"))
    {
        return FCortexDataLocalizationOps::SetTranslation(Params);
    }

    // Asset search
    if (Command == TEXT("search_assets"))
    {
        return FCortexDataAssetSearchOps::SearchAssets(Params);
    }

    // CurveTable operations
    if (Command == TEXT("list_curve_tables"))
    {
        return FCortexDataCurveTableOps::ListCurveTables(Params);
    }
    if (Command == TEXT("get_curve_table"))
    {
        return FCortexDataCurveTableOps::GetCurveTable(Params);
    }
    if (Command == TEXT("update_curve_table_row"))
    {
        return FCortexDataCurveTableOps::UpdateCurveTableRow(Params);
    }

    return FCortexCommandRouter::Error(
        CortexErrorCodes::UnknownCommand,
        FString::Printf(TEXT("Unknown data command: %s"), *Command)
    );
}

TArray<FCortexCommandInfo> FCortexDataCommandHandler::GetSupportedCommands() const
{
    return {
        FCortexCommandInfo{ TEXT("create_datatable"), TEXT("Create a new DataTable asset") }
            .Required(TEXT("table_path"), TEXT("string"), TEXT("Target DataTable asset path"))
            .Required(TEXT("row_struct"), TEXT("string"), TEXT("Row struct type name")),
        FCortexCommandInfo{ TEXT("list_datatables"), TEXT("List all DataTables") }
            .Optional(TEXT("path_filter"), TEXT("string"), TEXT("Optional asset path prefix filter")),
        FCortexCommandInfo{ TEXT("get_datatable_schema"), TEXT("Get row struct schema") }
            .Required(TEXT("table_path"), TEXT("string"), TEXT("DataTable asset path"))
            .Optional(TEXT("include_inherited"), TEXT("boolean"), TEXT("Include inherited struct fields")),
        FCortexCommandInfo{ TEXT("query_datatable"), TEXT("Query rows with filtering") }
            .Required(TEXT("table_path"), TEXT("string"), TEXT("DataTable asset path"))
            .Optional(TEXT("row_name_pattern"), TEXT("string"), TEXT("Wildcard row-name filter"))
            .Optional(TEXT("row_names"), TEXT("array"), TEXT("Exact row names to fetch"))
            .Optional(TEXT("fields"), TEXT("array"), TEXT("Subset of fields to serialize"))
            .Optional(TEXT("limit"), TEXT("number"), TEXT("Maximum rows to return"))
            .Optional(TEXT("offset"), TEXT("number"), TEXT("Pagination offset")),
        FCortexCommandInfo{ TEXT("get_datatable_row"), TEXT("Get single row by name") }
            .Required(TEXT("table_path"), TEXT("string"), TEXT("DataTable asset path"))
            .Required(TEXT("row_name"), TEXT("string"), TEXT("Row identifier")),
        FCortexCommandInfo{ TEXT("get_struct_schema"), TEXT("Get schema for any UStruct") }
            .Required(TEXT("struct_name"), TEXT("string"), TEXT("Struct type name"))
            .Optional(TEXT("include_subtypes"), TEXT("boolean"), TEXT("Include known instanced-struct subtypes")),
        FCortexCommandInfo{ TEXT("add_datatable_row"), TEXT("Add new row") }
            .Required(TEXT("table_path"), TEXT("string"), TEXT("Target DataTable asset path"))
            .Required(TEXT("row_name"), TEXT("string"), TEXT("New row identifier"))
            .Required(TEXT("row_data"), TEXT("object"), TEXT("Row payload to insert")),
        FCortexCommandInfo{ TEXT("update_datatable_row"), TEXT("Update existing row") }
            .Required(TEXT("table_path"), TEXT("string"), TEXT("Target DataTable asset path"))
            .Required(TEXT("row_name"), TEXT("string"), TEXT("Existing row identifier"))
            .Required(TEXT("row_data"), TEXT("object"), TEXT("Partial row payload to merge"))
            .Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview changes without writing")),
        FCortexCommandInfo{ TEXT("delete_datatable_row"), TEXT("Delete row") }
            .Required(TEXT("table_path"), TEXT("string"), TEXT("Target DataTable asset path"))
            .Required(TEXT("row_name"), TEXT("string"), TEXT("Row identifier to delete")),
        FCortexCommandInfo{ TEXT("import_datatable_json"), TEXT("Bulk import rows") }
            .Required(TEXT("table_path"), TEXT("string"), TEXT("Target DataTable asset path"))
            .Required(TEXT("rows"), TEXT("array"), TEXT("Rows to import"))
            .Optional(TEXT("mode"), TEXT("string"), TEXT("Import mode"))
            .Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Validate without writing")),
        FCortexCommandInfo{ TEXT("search_datatable_content"), TEXT("Full-text search in tables") }
            .Required(TEXT("table_path"), TEXT("string"), TEXT("DataTable asset path"))
            .Required(TEXT("search_text"), TEXT("string"), TEXT("Case-insensitive search text (alias: query)"))
            .Optional(TEXT("fields"), TEXT("array"), TEXT("Fields to search"))
            .Optional(TEXT("preview_fields"), TEXT("array"), TEXT("Fields to include in match previews"))
            .Optional(TEXT("limit"), TEXT("number"), TEXT("Maximum matches to return")),
        FCortexCommandInfo{ TEXT("get_data_catalog"), TEXT("Discovery catalog of all data") },
        FCortexCommandInfo{ TEXT("resolve_tags"), TEXT("Look up rows by GameplayTag") }
            .Required(TEXT("table_path"), TEXT("string"), TEXT("Target DataTable asset path"))
            .Required(TEXT("tag_field"), TEXT("string"), TEXT("Field containing GameplayTags"))
            .Required(TEXT("tags"), TEXT("array"), TEXT("GameplayTags to resolve"))
            .Optional(TEXT("fields"), TEXT("array"), TEXT("Fields to include in results")),
        FCortexCommandInfo{ TEXT("list_gameplay_tags"), TEXT("List GameplayTags by prefix") }
            .Optional(TEXT("prefix"), TEXT("string"), TEXT("Optional tag prefix filter"))
            .Optional(TEXT("include_source_file"), TEXT("boolean"), TEXT("Include source file metadata")),
        FCortexCommandInfo{ TEXT("validate_gameplay_tag"), TEXT("Check if tag is registered") }
            .Required(TEXT("tag"), TEXT("string"), TEXT("GameplayTag to validate")),
        FCortexCommandInfo{ TEXT("register_gameplay_tag"), TEXT("Register single tag") }
            .Required(TEXT("tag"), TEXT("string"), TEXT("GameplayTag to register"))
            .Optional(TEXT("dev_comment"), TEXT("string"), TEXT("Optional developer comment"))
            .Optional(TEXT("source"), TEXT("string"), TEXT("Tag source name")),
        FCortexCommandInfo{ TEXT("register_gameplay_tags"), TEXT("Batch register tags") }
            .Required(TEXT("tags"), TEXT("array"), TEXT("GameplayTags to register")),
        FCortexCommandInfo{ TEXT("list_data_assets"), TEXT("List DataAssets") }
            .Optional(TEXT("class_name"), TEXT("string"), TEXT("Optional class filter"))
            .Optional(TEXT("path_filter"), TEXT("string"), TEXT("Optional asset path prefix")),
        FCortexCommandInfo{ TEXT("get_data_asset"), TEXT("Get DataAsset properties") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("DataAsset path")),
        FCortexCommandInfo{ TEXT("update_data_asset"), TEXT("Update DataAsset properties") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("DataAsset path"))
            .Required(TEXT("properties"), TEXT("object"), TEXT("Properties to update"))
            .Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview changes without writing")),
        FCortexCommandInfo{ TEXT("create_data_asset"), TEXT("Create new DataAsset") }
            .Required(TEXT("class_name"), TEXT("string"), TEXT("DataAsset class name"))
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("Target asset path"))
            .Optional(TEXT("properties"), TEXT("object"), TEXT("Initial property values")),
        FCortexCommandInfo{ TEXT("delete_data_asset"), TEXT("Delete DataAsset") }
            .Required(TEXT("asset_path"), TEXT("string"), TEXT("DataAsset path")),
        FCortexCommandInfo{ TEXT("list_string_tables"), TEXT("List StringTables") }
            .Optional(TEXT("path_filter"), TEXT("string"), TEXT("Optional asset path prefix")),
        FCortexCommandInfo{ TEXT("get_translations"), TEXT("Get StringTable entries") }
            .Required(TEXT("string_table_path"), TEXT("string"), TEXT("StringTable asset path"))
            .Optional(TEXT("key_pattern"), TEXT("string"), TEXT("Optional key filter")),
        FCortexCommandInfo{ TEXT("set_translation"), TEXT("Set StringTable entry") }
            .Required(TEXT("string_table_path"), TEXT("string"), TEXT("StringTable asset path"))
            .Required(TEXT("key"), TEXT("string"), TEXT("StringTable key"))
            .Required(TEXT("text"), TEXT("string"), TEXT("Localized text value")),
        FCortexCommandInfo{ TEXT("search_assets"), TEXT("Asset Registry search") }
            .Optional(TEXT("query"), TEXT("string"), TEXT("Search text"))
            .Optional(TEXT("class_names"), TEXT("array"), TEXT("Allowed asset classes"))
            .Optional(TEXT("path_prefixes"), TEXT("array"), TEXT("Allowed asset path prefixes"))
            .Optional(TEXT("limit"), TEXT("number"), TEXT("Maximum assets to return")),
        FCortexCommandInfo{ TEXT("list_curve_tables"), TEXT("List CurveTables") }
            .Optional(TEXT("path_filter"), TEXT("string"), TEXT("Optional asset path prefix")),
        FCortexCommandInfo{ TEXT("get_curve_table"), TEXT("Get curve rows") }
            .Required(TEXT("table_path"), TEXT("string"), TEXT("CurveTable asset path"))
            .Optional(TEXT("row_name"), TEXT("string"), TEXT("Optional row filter")),
        FCortexCommandInfo{ TEXT("update_curve_table_row"), TEXT("Update curve row") }
            .Required(TEXT("table_path"), TEXT("string"), TEXT("CurveTable asset path"))
            .Required(TEXT("row_name"), TEXT("string"), TEXT("Curve row identifier"))
            .Required(TEXT("keyframes"), TEXT("array"), TEXT("Curve keyframes to apply")),
    };
}
