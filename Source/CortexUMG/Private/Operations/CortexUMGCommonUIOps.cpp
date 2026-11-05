#include "Operations/CortexUMGCommonUIOps.h"
#include "CortexTypes.h"
#include "CortexSerializer.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/DataTable.h"
#include "K2Node_Event.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
/** CommonUI classes resolved by path; any of them may be null when the CommonUI plugin is not loaded. */
struct FCommonUIClasses
{
    UClass* Activatable = nullptr;
    UClass* ButtonBase = nullptr;
    UClass* TextBlock = nullptr;
    UClass* RichTextBlock = nullptr;
    UClass* Border = nullptr;
    UClass* ButtonStyle = nullptr;
    UClass* TextStyle = nullptr;
    UClass* BorderStyle = nullptr;

    FCommonUIClasses()
    {
        Activatable = FindObject<UClass>(nullptr, TEXT("/Script/CommonUI.CommonActivatableWidget"));
        ButtonBase = FindObject<UClass>(nullptr, TEXT("/Script/CommonUI.CommonButtonBase"));
        TextBlock = FindObject<UClass>(nullptr, TEXT("/Script/CommonUI.CommonTextBlock"));
        RichTextBlock = FindObject<UClass>(nullptr, TEXT("/Script/CommonUI.CommonRichTextBlock"));
        Border = FindObject<UClass>(nullptr, TEXT("/Script/CommonUI.CommonBorder"));
        ButtonStyle = FindObject<UClass>(nullptr, TEXT("/Script/CommonUI.CommonButtonStyle"));
        TextStyle = FindObject<UClass>(nullptr, TEXT("/Script/CommonUI.CommonTextStyle"));
        BorderStyle = FindObject<UClass>(nullptr, TEXT("/Script/CommonUI.CommonBorderStyle"));
    }

    bool IsAvailable() const
    {
        return Activatable && ButtonBase && TextBlock && RichTextBlock && Border
            && ButtonStyle && TextStyle && BorderStyle;
    }
};

/** Parent classes (nearest first) that live in the CommonUI native module. */
TArray<TSharedPtr<FJsonValue>> CollectCommonUIBases(const UClass* ParentClass)
{
    TArray<TSharedPtr<FJsonValue>> Bases;
    for (const UClass* Class = ParentClass; Class; Class = Class->GetSuperClass())
    {
        if (Class->GetOutermost()->GetName().StartsWith(TEXT("/Script/CommonUI")))
        {
            Bases.Add(MakeShared<FJsonValueString>(Class->GetPathName()));
        }
    }
    return Bases;
}

UClass* ReadClassProperty(const UObject* Object, const TCHAR* PropertyName)
{
    const FClassProperty* Property = CastField<FClassProperty>(Object->GetClass()->FindPropertyByName(PropertyName));
    return Property ? Cast<UClass>(Property->GetObjectPropertyValue_InContainer(Object)) : nullptr;
}

/** Writes style (class path, empty when unset) and style_asset (owning Blueprint asset) of a style class. */
void WriteStyleReference(
    const UClass* StyleClass, const TSharedPtr<FJsonObject>& Json, TSet<FString>& StyleAssets)
{
    Json->SetStringField(TEXT("style"), StyleClass ? StyleClass->GetPathName() : FString());
    if (StyleClass)
    {
        if (const UBlueprint* StyleBlueprint = Cast<UBlueprint>(StyleClass->ClassGeneratedBy))
        {
            Json->SetStringField(TEXT("style_asset"), StyleBlueprint->GetPathName());
            StyleAssets.Add(StyleBlueprint->GetPathName());
        }
    }
}

TArray<TSharedPtr<FJsonValue>> SortedStringValues(const TSet<FString>& Strings)
{
    TArray<FString> Sorted = Strings.Array();
    Sorted.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const FString& String : Sorted)
    {
        Values.Add(MakeShared<FJsonValueString>(String));
    }
    return Values;
}

TSharedPtr<FJsonObject> BuildButtonJson(
    const UObject* Button, const FString& WidgetName, bool bIsSelf, TSet<FString>& StyleAssets)
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("widget_name"), WidgetName);
    Json->SetStringField(TEXT("class"), Button->GetClass()->GetName());
    if (bIsSelf)
    {
        Json->SetBoolField(TEXT("is_self"), true);
    }
    WriteStyleReference(ReadClassProperty(Button, TEXT("Style")), Json, StyleAssets);

    TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
    FString Table;
    FString Row;
    const FStructProperty* ActionProperty =
        CastField<FStructProperty>(Button->GetClass()->FindPropertyByName(TEXT("TriggeringInputAction")));
    if (ActionProperty && ActionProperty->Struct == FDataTableRowHandle::StaticStruct())
    {
        const FDataTableRowHandle* Handle = ActionProperty->ContainerPtrToValuePtr<FDataTableRowHandle>(Button);
        Table = Handle->DataTable ? Handle->DataTable->GetPathName() : FString();
        Row = Handle->RowName.IsNone() ? FString() : Handle->RowName.ToString();
    }
    Action->SetStringField(TEXT("table"), Table);
    Action->SetStringField(TEXT("row"), Row);
    Json->SetObjectField(TEXT("triggering_input_action"), Action);
    return Json;
}

TSharedPtr<FJsonObject> BuildStyledWidgetJson(
    const UWidget* Widget, const UClass* StyleClass, TSet<FString>& StyleAssets)
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("widget_name"), Widget->GetName());
    Json->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
    WriteStyleReference(StyleClass, Json, StyleAssets);
    return Json;
}

void SortByWidgetName(TArray<TSharedPtr<FJsonValue>>& Records)
{
    Records.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
    {
        return A->AsObject()->GetStringField(TEXT("widget_name"))
            .Compare(B->AsObject()->GetStringField(TEXT("widget_name")), ESearchCase::CaseSensitive) < 0;
    });
}

/** True when the Blueprint implements the named BlueprintImplementableEvent as an event node or a function override. */
bool ImplementsOverride(UBlueprint* Blueprint, const FName& FunctionName)
{
    for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (Graph && Graph->GetFName() == FunctionName)
        {
            return true;
        }
    }
    TArray<UEdGraph*> Graphs;
    Blueprint->GetAllGraphs(Graphs);
    for (const UEdGraph* Graph : Graphs)
    {
        if (!Graph)
        {
            continue;
        }
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            const UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
            if (Event && Event->bOverrideFunction && Event->EventReference.GetMemberName() == FunctionName)
            {
                return true;
            }
        }
    }
    return false;
}

FCortexCommandResult DescribeWidgetBlueprint(
    UWidgetBlueprint* WBP, const FString& AssetPath, const FCommonUIClasses& Classes)
{
    const bool bAvailable = Classes.IsAvailable();
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetBoolField(TEXT("commonui_available"), bAvailable);
    Data->SetStringField(TEXT("asset_kind"), TEXT("widget"));
    Data->SetArrayField(TEXT("commonui_bases"), CollectCommonUIBases(WBP->ParentClass));

    TSharedPtr<FJsonObject> Activatable = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Overrides;
    TArray<TSharedPtr<FJsonValue>> Buttons;
    TArray<TSharedPtr<FJsonValue>> TextWidgets;
    TArray<TSharedPtr<FJsonValue>> BorderWidgets;
    TSet<FString> StyleAssets;

    if (bAvailable)
    {
        const UObject* ClassDefault = WBP->GeneratedClass ? WBP->GeneratedClass->GetDefaultObject() : nullptr;

        if (WBP->ParentClass && WBP->ParentClass->IsChildOf(Classes.Activatable))
        {
            if (ClassDefault)
            {
                TArray<const FProperty*> Properties;
                for (TFieldIterator<FProperty> It(Classes.Activatable, EFieldIteratorFlags::ExcludeSuper); It; ++It)
                {
                    if (It->HasAnyPropertyFlags(CPF_Edit))
                    {
                        Properties.Add(*It);
                    }
                }
                Properties.Sort([](const FProperty& A, const FProperty& B)
                {
                    return A.GetName().Compare(B.GetName(), ESearchCase::CaseSensitive) < 0;
                });
                for (const FProperty* Property : Properties)
                {
                    Activatable->SetField(Property->GetName(),
                        FCortexSerializer::PropertyToJson(Property, Property->ContainerPtrToValuePtr<void>(ClassDefault)));
                }
            }

            const TCHAR* const OverrideNames[] = {
                TEXT("BP_GetDesiredFocusTarget"), TEXT("BP_GetDesiredInputConfig"), TEXT("BP_OnActivated"),
                TEXT("BP_OnDeactivated"), TEXT("BP_OnHandleBackAction") };
            for (const TCHAR* OverrideName : OverrideNames)
            {
                if (ImplementsOverride(WBP, FName(OverrideName)))
                {
                    Overrides.Add(MakeShared<FJsonValueString>(OverrideName));
                }
            }
        }

        // A Blueprint parented to a CommonUI button is itself the button.
        if (ClassDefault && WBP->ParentClass && WBP->ParentClass->IsChildOf(Classes.ButtonBase))
        {
            Buttons.Add(MakeShared<FJsonValueObject>(BuildButtonJson(ClassDefault, WBP->GetName(), true, StyleAssets)));
        }

        TArray<UWidget*> Widgets;
        WBP->WidgetTree->GetAllWidgets(Widgets);
        for (const UWidget* Widget : Widgets)
        {
            if (!Widget)
            {
                continue;
            }
            const UClass* WidgetClass = Widget->GetClass();
            if (WidgetClass->IsChildOf(Classes.ButtonBase))
            {
                Buttons.Add(MakeShared<FJsonValueObject>(BuildButtonJson(Widget, Widget->GetName(), false, StyleAssets)));
            }
            else if (WidgetClass->IsChildOf(Classes.TextBlock))
            {
                TextWidgets.Add(MakeShared<FJsonValueObject>(
                    BuildStyledWidgetJson(Widget, ReadClassProperty(Widget, TEXT("Style")), StyleAssets)));
            }
            else if (WidgetClass->IsChildOf(Classes.RichTextBlock))
            {
                TextWidgets.Add(MakeShared<FJsonValueObject>(BuildStyledWidgetJson(
                    Widget, ReadClassProperty(Widget, TEXT("DefaultTextStyleOverrideClass")), StyleAssets)));
            }
            else if (WidgetClass->IsChildOf(Classes.Border))
            {
                BorderWidgets.Add(MakeShared<FJsonValueObject>(
                    BuildStyledWidgetJson(Widget, ReadClassProperty(Widget, TEXT("Style")), StyleAssets)));
            }
        }
        SortByWidgetName(Buttons);
        SortByWidgetName(TextWidgets);
        SortByWidgetName(BorderWidgets);
    }

    Data->SetObjectField(TEXT("activatable"), Activatable);
    Data->SetArrayField(TEXT("overrides"), Overrides);
    Data->SetArrayField(TEXT("buttons"), Buttons);
    Data->SetArrayField(TEXT("text_widgets"), TextWidgets);
    Data->SetArrayField(TEXT("border_widgets"), BorderWidgets);
    Data->SetArrayField(TEXT("style_assets"), SortedStringValues(StyleAssets));
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult DescribeStyleBlueprint(
    UBlueprint* Blueprint, const FString& AssetPath, const FCommonUIClasses& Classes, const TCHAR* StyleKind)
{
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetBoolField(TEXT("commonui_available"), true);
    Data->SetStringField(TEXT("asset_kind"), TEXT("style"));
    Data->SetStringField(TEXT("style_kind"), StyleKind);
    Data->SetArrayField(TEXT("commonui_bases"), CollectCommonUIBases(Blueprint->ParentClass));

    TArray<TSharedPtr<FJsonValue>> Referenced;
    TSet<FString> StyleAssets;
    if (const UObject* ClassDefault = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr)
    {
        TArray<const FClassProperty*> Properties;
        for (TFieldIterator<FClassProperty> It(Blueprint->GeneratedClass); It; ++It)
        {
            const UClass* MetaClass = It->MetaClass;
            if (MetaClass && (MetaClass->IsChildOf(Classes.ButtonStyle) || MetaClass->IsChildOf(Classes.TextStyle)
                    || MetaClass->IsChildOf(Classes.BorderStyle)))
            {
                Properties.Add(*It);
            }
        }
        Properties.Sort([](const FClassProperty& A, const FClassProperty& B)
        {
            return A.GetName().Compare(B.GetName(), ESearchCase::CaseSensitive) < 0;
        });
        for (const FClassProperty* Property : Properties)
        {
            const UClass* StyleClass = Cast<UClass>(Property->GetObjectPropertyValue_InContainer(ClassDefault));
            if (!StyleClass)
            {
                continue;
            }
            TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("property"), Property->GetName());
            WriteStyleReference(StyleClass, Json, StyleAssets);
            Referenced.Add(MakeShared<FJsonValueObject>(Json));
        }
    }
    Data->SetArrayField(TEXT("referenced_styles"), Referenced);
    Data->SetArrayField(TEXT("style_assets"), SortedStringValues(StyleAssets));
    return FCortexCommandRouter::Success(Data);
}
}

FCortexCommandResult FCortexUMGCommonUIOps::GetCommonUIInfo(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

    const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
    if (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName))
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound,
            FString::Printf(TEXT("Blueprint package not found: %s"), *AssetPath));
    }
    UBlueprint* Blueprint = Cast<UBlueprint>(StaticLoadObject(UBlueprint::StaticClass(), nullptr, *AssetPath));
    if (!Blueprint)
    {
        return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound,
            FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
    }

    const FCommonUIClasses Classes;
    if (UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Blueprint))
    {
        if (!WBP->WidgetTree)
        {
            return FCortexCommandRouter::Error(CortexErrorCodes::BlueprintNotFound,
                FString::Printf(TEXT("Widget blueprint has no WidgetTree: %s"), *AssetPath));
        }
        return DescribeWidgetBlueprint(WBP, AssetPath, Classes);
    }

    if (Classes.IsAvailable() && Blueprint->ParentClass)
    {
        const TCHAR* StyleKind = nullptr;
        if (Blueprint->ParentClass->IsChildOf(Classes.ButtonStyle))
        {
            StyleKind = TEXT("button");
        }
        else if (Blueprint->ParentClass->IsChildOf(Classes.TextStyle))
        {
            StyleKind = TEXT("text");
        }
        else if (Blueprint->ParentClass->IsChildOf(Classes.BorderStyle))
        {
            StyleKind = TEXT("border");
        }
        if (StyleKind)
        {
            return DescribeStyleBlueprint(Blueprint, AssetPath, Classes, StyleKind);
        }
    }

    return FCortexCommandRouter::Error(CortexErrorCodes::TypeMismatch,
        FString::Printf(TEXT("%s is neither a Widget Blueprint nor a CommonUI style Blueprint"), *AssetPath));
}
