#include "Operations/CortexUMGEmbeddedValueOps.h"
#include "CortexUMGUtils.h"
#include "CortexTypes.h"
#include "CortexSerializer.h"
#include "WidgetBlueprint.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"

namespace
{
/**
 * An editable property declared below UUserWidget: engine UserWidget/Widget members are excluded, while
 * Blueprint variables and native UPROPERTYs of the embedded class are included. Defaults-only properties
 * (CPF_DisableEditOnInstance) are included and flagged through IsInstanceEditable.
 */
bool IsEmbeddedValueProperty(const FProperty* Property)
{
    if (!Property->HasAnyPropertyFlags(CPF_Edit))
    {
        return false;
    }
    const UClass* Owner = Property->GetOwner<UClass>();
    return Owner && Owner != UUserWidget::StaticClass() && Owner->IsChildOf(UUserWidget::StaticClass());
}

bool IsInstanceEditable(const FProperty* Property)
{
    return !Property->HasAnyPropertyFlags(CPF_DisableEditOnInstance);
}

/** Embedded-value properties of the class, sorted by name. */
TArray<const FProperty*> CollectEmbeddedValueProperties(const UClass* WidgetClass)
{
    TArray<const FProperty*> Properties;
    for (TFieldIterator<FProperty> It(WidgetClass); It; ++It)
    {
        if (IsEmbeddedValueProperty(*It))
        {
            Properties.Add(*It);
        }
    }
    // TArray::Sort over pointer elements hands the predicate dereferenced elements.
    Properties.Sort([](const FProperty& A, const FProperty& B)
    {
        return A.GetName().Compare(B.GetName(), ESearchCase::CaseSensitive) < 0;
    });
    return Properties;
}

void SetInstanceIdentity(const TSharedPtr<FJsonObject>& Json, const UUserWidget* Instance)
{
    const UClass* WidgetClass = Instance->GetClass();
    Json->SetStringField(TEXT("widget_name"), Instance->GetName());
    Json->SetStringField(TEXT("class"), WidgetClass->GetName());
    if (const UBlueprint* Blueprint = Cast<UBlueprint>(WidgetClass->ClassGeneratedBy))
    {
        Json->SetStringField(TEXT("widget_blueprint"), Blueprint->GetPathName());
    }
}

void SetPropertyIdentity(const TSharedPtr<FJsonObject>& Json, const FProperty* Property)
{
    Json->SetStringField(TEXT("property"), Property->GetName());
    Json->SetStringField(TEXT("declared_in"), Property->GetOwner<UClass>()->GetPathName());
    Json->SetStringField(TEXT("type"), Property->GetCPPType());
    Json->SetBoolField(TEXT("instance_editable"), IsInstanceEditable(Property));
}

TSharedPtr<FJsonObject> BuildEmbeddedWidgetJson(UUserWidget* Instance)
{
    const UObject* ClassDefault = Instance->GetClass()->GetDefaultObject();

    TArray<TSharedPtr<FJsonValue>> PropertiesJson;
    for (const FProperty* Property : CollectEmbeddedValueProperties(Instance->GetClass()))
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        SetPropertyIdentity(Json, Property);
        Json->SetField(TEXT("value"),
            FCortexSerializer::PropertyToJson(Property, Property->ContainerPtrToValuePtr<void>(Instance)));
        Json->SetBoolField(TEXT("is_override"), !Property->Identical_InContainer(Instance, ClassDefault));
        PropertiesJson.Add(MakeShared<FJsonValueObject>(Json));
    }

    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    SetInstanceIdentity(Json, Instance);
    Json->SetArrayField(TEXT("properties"), PropertiesJson);
    return Json;
}

/** Template properties whose value differs from the class default, including defaults-only members. */
TSharedPtr<FJsonObject> BuildEmbeddedWidgetDiffJson(UUserWidget* Instance)
{
    const UObject* ClassDefault = Instance->GetClass()->GetDefaultObject();

    TArray<TSharedPtr<FJsonValue>> DifferencesJson;
    for (const FProperty* Property : CollectEmbeddedValueProperties(Instance->GetClass()))
    {
        if (Property->Identical_InContainer(Instance, ClassDefault))
        {
            continue;
        }
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        SetPropertyIdentity(Json, Property);
        Json->SetField(TEXT("instance_value"),
            FCortexSerializer::PropertyToJson(Property, Property->ContainerPtrToValuePtr<void>(Instance)));
        Json->SetField(TEXT("class_default_value"),
            FCortexSerializer::PropertyToJson(Property, Property->ContainerPtrToValuePtr<void>(ClassDefault)));
        DifferencesJson.Add(MakeShared<FJsonValueObject>(Json));
    }

    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    SetInstanceIdentity(Json, Instance);
    Json->SetNumberField(TEXT("difference_count"), DifferencesJson.Num());
    Json->SetArrayField(TEXT("differences"), DifferencesJson);
    return Json;
}

/** Every embedded UserWidget of the blueprint's designer tree, sorted by name. */
TArray<UUserWidget*> CollectEmbeddedUserWidgets(const UWidgetBlueprint* WBP)
{
    TArray<UWidget*> AllWidgets;
    WBP->WidgetTree->GetAllWidgets(AllWidgets);

    TArray<UUserWidget*> Instances;
    for (UWidget* Widget : AllWidgets)
    {
        if (UUserWidget* UserWidget = Cast<UUserWidget>(Widget))
        {
            Instances.Add(UserWidget);
        }
    }
    Instances.Sort([](const UUserWidget& A, const UUserWidget& B)
    {
        return A.GetName().Compare(B.GetName(), ESearchCase::CaseSensitive) < 0;
    });
    return Instances;
}
}

FCortexCommandResult FCortexUMGEmbeddedValueOps::GetEmbeddedWidgetValues(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    FCortexCommandResult LoadError;
    UWidgetBlueprint* WBP = CortexUMGUtils::LoadWidgetBlueprint(AssetPath, LoadError);
    if (!WBP)
    {
        return LoadError;
    }

    TArray<UUserWidget*> Instances;
    FString WidgetName;
    if (Params->TryGetStringField(TEXT("widget_name"), WidgetName))
    {
        TArray<UWidget*> AllWidgets;
        WBP->WidgetTree->GetAllWidgets(AllWidgets);
        UWidget* const* Found = AllWidgets.FindByPredicate([&WidgetName](const UWidget* Widget)
        {
            return Widget && Widget->GetName().Equals(WidgetName, ESearchCase::CaseSensitive);
        });
        if (!Found)
        {
            return FCortexCommandRouter::Error(CortexErrorCodes::WidgetNotFound,
                FString::Printf(TEXT("Widget not found: %s"), *WidgetName));
        }
        UUserWidget* UserWidget = Cast<UUserWidget>(*Found);
        if (!UserWidget)
        {
            return FCortexCommandRouter::Error(CortexErrorCodes::TypeMismatch,
                FString::Printf(TEXT("Widget %s is a %s, not an embedded UserWidget"),
                    *WidgetName, *(*Found)->GetClass()->GetName()));
        }
        Instances.Add(UserWidget);
    }
    else
    {
        Instances = CollectEmbeddedUserWidgets(WBP);
    }

    TArray<TSharedPtr<FJsonValue>> WidgetsJson;
    for (UUserWidget* Instance : Instances)
    {
        WidgetsJson.Add(MakeShared<FJsonValueObject>(BuildEmbeddedWidgetJson(Instance)));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetNumberField(TEXT("widget_count"), WidgetsJson.Num());
    Data->SetArrayField(TEXT("widgets"), WidgetsJson);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGEmbeddedValueOps::GetEmbeddedWidgetDiff(const TSharedPtr<FJsonObject>& Params)
{
    const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    FCortexCommandResult LoadError;
    UWidgetBlueprint* WBP = CortexUMGUtils::LoadWidgetBlueprint(AssetPath, LoadError);
    if (!WBP)
    {
        return LoadError;
    }

    TArray<TSharedPtr<FJsonValue>> WidgetsJson;
    int32 DifferenceCount = 0;
    for (UUserWidget* Instance : CollectEmbeddedUserWidgets(WBP))
    {
        const TSharedPtr<FJsonObject> Json = BuildEmbeddedWidgetDiffJson(Instance);
        DifferenceCount += static_cast<int32>(Json->GetNumberField(TEXT("difference_count")));
        WidgetsJson.Add(MakeShared<FJsonValueObject>(Json));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetNumberField(TEXT("widget_count"), WidgetsJson.Num());
    Data->SetNumberField(TEXT("difference_count"), DifferenceCount);
    Data->SetArrayField(TEXT("widgets"), WidgetsJson);
    return FCortexCommandRouter::Success(Data);
}
