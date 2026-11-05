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
 * An instance-editable property declared below UUserWidget: engine UserWidget/Widget members are
 * excluded, while Blueprint variables and native UPROPERTYs of the embedded class are included.
 */
bool IsEmbeddedValueProperty(const FProperty* Property)
{
    if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_DisableEditOnInstance))
    {
        return false;
    }
    const UClass* Owner = Property->GetOwner<UClass>();
    return Owner && Owner != UUserWidget::StaticClass() && Owner->IsChildOf(UUserWidget::StaticClass());
}

TSharedPtr<FJsonObject> BuildEmbeddedWidgetJson(UUserWidget* Instance)
{
    const UClass* WidgetClass = Instance->GetClass();
    const UObject* ClassDefault = WidgetClass->GetDefaultObject();

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

    TArray<TSharedPtr<FJsonValue>> PropertiesJson;
    for (const FProperty* Property : Properties)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("property"), Property->GetName());
        Json->SetStringField(TEXT("declared_in"), Property->GetOwner<UClass>()->GetPathName());
        Json->SetStringField(TEXT("type"), Property->GetCPPType());
        Json->SetField(TEXT("value"),
            FCortexSerializer::PropertyToJson(Property, Property->ContainerPtrToValuePtr<void>(Instance)));
        Json->SetBoolField(TEXT("is_override"), !Property->Identical_InContainer(Instance, ClassDefault));
        PropertiesJson.Add(MakeShared<FJsonValueObject>(Json));
    }

    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("widget_name"), Instance->GetName());
    Json->SetStringField(TEXT("class"), WidgetClass->GetName());
    if (const UBlueprint* Blueprint = Cast<UBlueprint>(WidgetClass->ClassGeneratedBy))
    {
        Json->SetStringField(TEXT("widget_blueprint"), Blueprint->GetPathName());
    }
    Json->SetArrayField(TEXT("properties"), PropertiesJson);
    return Json;
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

    TArray<UWidget*> AllWidgets;
    WBP->WidgetTree->GetAllWidgets(AllWidgets);

    TArray<UUserWidget*> Instances;
    FString WidgetName;
    if (Params->TryGetStringField(TEXT("widget_name"), WidgetName))
    {
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
        for (UWidget* Widget : AllWidgets)
        {
            if (UUserWidget* UserWidget = Cast<UUserWidget>(Widget))
            {
                Instances.Add(UserWidget);
            }
        }
    }
    Instances.Sort([](const UUserWidget& A, const UUserWidget& B)
    {
        return A.GetName().Compare(B.GetName(), ESearchCase::CaseSensitive) < 0;
    });

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
