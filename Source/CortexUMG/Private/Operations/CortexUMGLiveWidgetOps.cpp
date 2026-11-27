#include "Operations/CortexUMGLiveWidgetOps.h"
#include "CortexEngineCompat.h"
#include "CortexPIEUtils.h"
#include "CortexSafeFileContract.h"
#include "CortexSerializer.h"
#include "CortexTypes.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/Image.h"
#include "Components/RichTextBlock.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateTypes.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"

namespace
{
constexpr int32 DefaultMaxInstances = 10;
constexpr int32 MaximumMaxInstances = 100;
constexpr int32 MaxReportedDifferences = 200;
constexpr int32 MaxLabelLength = 64;
constexpr int32 SnapshotFormatVersion = 1;

/** The capture selectors shared by get, snapshot and (read back from the snapshot file) diff. */
struct FLiveWidgetRequest
{
    FString WidgetClassPath;
    UClass* WidgetClass = nullptr;
    TArray<FString> Widgets;
    bool bHasWidgets = false;
    bool bIncludeProperties = true;
    int32 MaxInstances = DefaultMaxInstances;
};

struct FLiveReadback
{
    TArray<TSharedPtr<FJsonValue>> Instances;
    bool bTruncated = false;
};

using FFlatEntries = TMap<FString, TSharedPtr<FJsonValue>>;

FCortexCommandResult InvalidField(const FString& Message)
{
    return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, Message);
}

/** A present, non-null field; JSON null reads as absent so clients may pass explicit nulls. */
TSharedPtr<FJsonValue> GetPresentField(const TSharedPtr<FJsonObject>& Source, const TCHAR* Name)
{
    const TSharedPtr<FJsonValue> Value = Source->TryGetField(Name);
    return (Value.IsValid() && !Value->IsNull()) ? Value : nullptr;
}

/**
 * Widget Blueprint asset path, generated class path or native class name. Package loads are guarded by
 * DoesPackageExist so a missing path never produces a SkipPackage warning.
 */
UClass* ResolveWidgetClass(const FString& Input)
{
    UClass* Resolved = nullptr;
    if (!Input.Contains(TEXT("/")))
    {
        Resolved = FindFirstObjectSafe<UClass>(*Input, EFindFirstObjectOptions::NativeFirst);
        if (!Resolved && Input.Len() > 1 && Input[0] == TEXT('U'))
        {
            Resolved = FindFirstObjectSafe<UClass>(*Input.Mid(1), EFindFirstObjectOptions::NativeFirst);
        }
        return Resolved;
    }

    FString ObjectPath = Input;
    if (!ObjectPath.Contains(TEXT(".")))
    {
        ObjectPath = ObjectPath + TEXT(".") + FPackageName::GetShortName(ObjectPath);
    }

    if (UClass* LoadedClass = FindObject<UClass>(nullptr, *ObjectPath))
    {
        return LoadedClass;
    }
    if (const UBlueprint* LoadedBlueprint = FindObject<UBlueprint>(nullptr, *ObjectPath))
    {
        return LoadedBlueprint->GeneratedClass;
    }

    const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
    if (PackageName.StartsWith(TEXT("/Script/")))
    {
        return nullptr;
    }
    if (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName))
    {
        return nullptr;
    }

    UObject* Loaded = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn);
    if (UClass* LoadedClass = Cast<UClass>(Loaded))
    {
        return LoadedClass;
    }
    if (const UBlueprint* LoadedBlueprint = Cast<UBlueprint>(Loaded))
    {
        return LoadedBlueprint->GeneratedClass;
    }
    return nullptr;
}

/** Parses the capture selectors from command params or from a snapshot file's selector block. */
bool ParseRequest(const TSharedPtr<FJsonObject>& Source, FLiveWidgetRequest& OutRequest, FCortexCommandResult& OutError)
{
    if (!Source.IsValid())
    {
        OutError = InvalidField(TEXT("Missing params"));
        return false;
    }

    const TSharedPtr<FJsonValue> ClassValue = GetPresentField(Source, TEXT("widget_class"));
    FString WidgetClassPath;
    if (!ClassValue.IsValid() || !ClassValue->TryGetString(WidgetClassPath) || WidgetClassPath.IsEmpty())
    {
        OutError = InvalidField(TEXT("widget_class is required: a Widget Blueprint asset path, a generated class path or a native class name"));
        return false;
    }
    OutRequest.WidgetClassPath = WidgetClassPath;
    OutRequest.WidgetClass = ResolveWidgetClass(WidgetClassPath);
    if (!OutRequest.WidgetClass)
    {
        OutError = InvalidField(FString::Printf(
            TEXT("widget_class not found: %s (accepted: Widget Blueprint asset path, generated class path, native class name)"),
            *WidgetClassPath));
        return false;
    }
    if (!OutRequest.WidgetClass->IsChildOf(UUserWidget::StaticClass()))
    {
        OutError = InvalidField(FString::Printf(
            TEXT("widget_class must resolve to a UserWidget subclass: %s"), *OutRequest.WidgetClass->GetPathName()));
        return false;
    }

    if (const TSharedPtr<FJsonValue> WidgetsValue = GetPresentField(Source, TEXT("widgets")))
    {
        const TArray<TSharedPtr<FJsonValue>>* WidgetArray = nullptr;
        if (!WidgetsValue->TryGetArray(WidgetArray) || WidgetArray == nullptr || WidgetArray->Num() == 0)
        {
            OutError = InvalidField(TEXT("widgets must be a non-empty array of child widget names"));
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Entry : *WidgetArray)
        {
            FString Name;
            if (!Entry.IsValid() || !Entry->TryGetString(Name) || Name.IsEmpty())
            {
                OutError = InvalidField(TEXT("widgets must contain only non-empty widget name strings"));
                return false;
            }
            OutRequest.Widgets.AddUnique(Name);
        }
        OutRequest.bHasWidgets = true;
    }

    if (const TSharedPtr<FJsonValue> IncludeValue = GetPresentField(Source, TEXT("include_properties")))
    {
        if (!IncludeValue->TryGetBool(OutRequest.bIncludeProperties))
        {
            OutError = InvalidField(TEXT("include_properties must be a boolean"));
            return false;
        }
    }

    if (const TSharedPtr<FJsonValue> MaxValue = GetPresentField(Source, TEXT("max_instances")))
    {
        double MaxInstances = 0.0;
        if (!MaxValue->TryGetNumber(MaxInstances) || FMath::FloorToDouble(MaxInstances) != MaxInstances
            || MaxInstances < 1.0 || MaxInstances > static_cast<double>(MaximumMaxInstances))
        {
            OutError = InvalidField(FString::Printf(TEXT("max_instances must be an integer from 1 to %d"), MaximumMaxInstances));
            return false;
        }
        OutRequest.MaxInstances = static_cast<int32>(MaxInstances);
    }
    return true;
}

bool IsValidLabel(const FString& Label)
{
    if (Label.IsEmpty() || Label.Len() > MaxLabelLength || Label == TEXT(".") || Label == TEXT(".."))
    {
        return false;
    }
    for (int32 Index = 0; Index < Label.Len(); ++Index)
    {
        const TCHAR Character = Label[Index];
        const bool bAllowed = (Character >= TEXT('A') && Character <= TEXT('Z'))
            || (Character >= TEXT('a') && Character <= TEXT('z'))
            || (Character >= TEXT('0') && Character <= TEXT('9'))
            || Character == TEXT('_') || Character == TEXT('.') || Character == TEXT('-');
        if (!bAllowed)
        {
            return false;
        }
    }
    return true;
}

bool ParseLabel(const TSharedPtr<FJsonObject>& Params, FString& OutLabel, FCortexCommandResult& OutError)
{
    const TSharedPtr<FJsonValue> LabelValue = Params.IsValid() ? GetPresentField(Params, TEXT("label")) : nullptr;
    if (!LabelValue.IsValid() || !LabelValue->TryGetString(OutLabel) || !IsValidLabel(OutLabel))
    {
        OutError = InvalidField(FString::Printf(
            TEXT("label is required and must match [A-Za-z0-9_.-]{1,%d} (and not be '.' or '..')"), MaxLabelLength));
        return false;
    }
    return true;
}

FString SnapshotPath(const FString& Label)
{
    return FPaths::Combine(
        FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()),
        TEXT("Cortex"), TEXT("WidgetSnapshots"), Label + TEXT(".json"));
}

FString VisibilityName(const ESlateVisibility Visibility)
{
    return StaticEnum<ESlateVisibility>()->GetNameStringByValue(static_cast<int64>(Visibility));
}

TSharedPtr<FJsonObject> ColorJson(const FLinearColor& Color)
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetNumberField(TEXT("r"), Color.R);
    Json->SetNumberField(TEXT("g"), Color.G);
    Json->SetNumberField(TEXT("b"), Color.B);
    Json->SetNumberField(TEXT("a"), Color.A);
    return Json;
}

FString ResourcePath(const FSlateBrush& Brush)
{
    const UObject* Resource = Brush.GetResourceObject();
    return Resource ? Resource->GetPathName() : FString();
}

void AddFontAndColor(const TSharedPtr<FJsonObject>& Style, const FSlateFontInfo& Font, const FSlateColor& Color)
{
    const UObject* FontObject = Font.FontObject.Get();
    Style->SetObjectField(TEXT("color"), ColorJson(Color.GetSpecifiedColor()));
    Style->SetStringField(TEXT("font_object"), FontObject ? FontObject->GetPathName() : FString());
    Style->SetStringField(TEXT("typeface"), Font.TypefaceFontName.ToString());
    Style->SetNumberField(TEXT("size"), Font.Size);
}

/** Stored style values of the widget kinds the design lists; false when the widget has none. */
bool BuildStyleJson(const UWidget* Widget, TSharedPtr<FJsonObject>& OutStyle)
{
    TSharedPtr<FJsonObject> Style = MakeShared<FJsonObject>();
    if (const UTextBlock* TextBlock = Cast<UTextBlock>(Widget))
    {
        AddFontAndColor(Style, TextBlock->GetFont(), TextBlock->GetColorAndOpacity());
    }
    else if (const UEditableText* EditableText = Cast<UEditableText>(Widget))
    {
        AddFontAndColor(Style, EditableText->GetFont(), EditableText->WidgetStyle.ColorAndOpacity);
    }
    else if (const UEditableTextBox* EditableTextBox = Cast<UEditableTextBox>(Widget))
    {
        const FEditableTextBoxStyle& TextBoxStyle = EditableTextBox->GetWidgetStyle();
        AddFontAndColor(Style, TextBoxStyle.TextStyle.Font, TextBoxStyle.TextStyle.ColorAndOpacity);
    }
    else if (const UImage* Image = Cast<UImage>(Widget))
    {
        const FSlateBrush& Brush = Image->GetBrush();
        TSharedPtr<FJsonObject> Size = MakeShared<FJsonObject>();
        Size->SetNumberField(TEXT("x"), Brush.ImageSize.X);
        Size->SetNumberField(TEXT("y"), Brush.ImageSize.Y);
        Style->SetStringField(TEXT("brush_resource"), ResourcePath(Brush));
        Style->SetObjectField(TEXT("image_size"), Size);
        Style->SetObjectField(TEXT("tint"), ColorJson(Image->GetColorAndOpacity()));
    }
    else if (const UButton* Button = Cast<UButton>(Widget))
    {
        const FButtonStyle& ButtonStyle = Button->GetStyle();
        Style->SetStringField(TEXT("normal"), ResourcePath(ButtonStyle.Normal));
        Style->SetStringField(TEXT("hovered"), ResourcePath(ButtonStyle.Hovered));
        Style->SetStringField(TEXT("pressed"), ResourcePath(ButtonStyle.Pressed));
    }
    else if (const UBorder* Border = Cast<UBorder>(Widget))
    {
        Style->SetStringField(TEXT("brush_resource"), ResourcePath(Border->Background));
    }
    else
    {
        return false;
    }
    OutStyle = Style;
    return true;
}

bool TryGetWidgetText(const UWidget* Widget, FText& OutText)
{
    if (const UTextBlock* TextBlock = Cast<UTextBlock>(Widget))
    {
        OutText = TextBlock->GetText();
        return true;
    }
    if (const URichTextBlock* RichText = Cast<URichTextBlock>(Widget))
    {
        OutText = RichText->GetText();
        return true;
    }
    if (const UEditableText* EditableText = Cast<UEditableText>(Widget))
    {
        OutText = EditableText->GetText();
        return true;
    }
    if (const UEditableTextBox* EditableTextBox = Cast<UEditableTextBox>(Widget))
    {
        OutText = EditableTextBox->GetText();
        return true;
    }
    return false;
}

/** Names of the property delegates (`<Name>Delegate`) that are bound on the widget, sorted, suffix dropped. */
TArray<FString> CollectBoundDelegates(const UWidget* Widget)
{
    TArray<FString> Bound;
    for (TFieldIterator<FDelegateProperty> It(Widget->GetClass()); It; ++It)
    {
        const FString Name = It->GetName();
        if (!Name.EndsWith(TEXT("Delegate")))
        {
            continue;
        }
        const FScriptDelegate* Delegate = It->ContainerPtrToValuePtr<FScriptDelegate>(Widget);
        if (Delegate && Delegate->IsBound())
        {
            Bound.Add(Name.LeftChop(8));
        }
    }
    Bound.Sort();
    return Bound;
}

TSharedPtr<FJsonObject> BuildWidgetJson(const UWidget* Widget)
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
    Json->SetStringField(TEXT("visibility"), VisibilityName(Widget->GetVisibility()));
    Json->SetBoolField(TEXT("is_enabled"), Widget->GetIsEnabled());
    Json->SetNumberField(TEXT("render_opacity"), Widget->GetRenderOpacity());

    FText Text;
    if (TryGetWidgetText(Widget, Text))
    {
        // The Slate widget, once built, holds the effective value (bindings included).
        Json->SetStringField(TEXT("text"), Text.ToString());
        Json->SetStringField(TEXT("text_source"), Widget->GetCachedWidget().IsValid() ? TEXT("slate") : TEXT("property"));
    }

    TArray<TSharedPtr<FJsonValue>> Bindings;
    for (const FString& Binding : CollectBoundDelegates(Widget))
    {
        Bindings.Add(MakeShared<FJsonValueString>(Binding));
    }
    Json->SetArrayField(TEXT("bindings"), Bindings);

    TSharedPtr<FJsonObject> Style;
    if (BuildStyleJson(Widget, Style))
    {
        Json->SetObjectField(TEXT("style"), Style);
    }
    return Json;
}

/** True when the property (or a container element) refers to a widget, or is a delegate. */
bool IsWidgetReferenceOrDelegate(const FProperty* Property)
{
    if (CastField<FDelegateProperty>(Property) || CastField<FMulticastDelegateProperty>(Property))
    {
        return true;
    }
    if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
    {
        return ObjectProperty->PropertyClass && ObjectProperty->PropertyClass->IsChildOf(UWidget::StaticClass());
    }
    if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
    {
        return IsWidgetReferenceOrDelegate(ArrayProperty->Inner);
    }
    if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
    {
        return IsWidgetReferenceOrDelegate(SetProperty->ElementProp);
    }
    if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
    {
        return IsWidgetReferenceOrDelegate(MapProperty->KeyProp) || IsWidgetReferenceOrDelegate(MapProperty->ValueProp);
    }
    return false;
}

/** The compiler-generated UberGraphFrame (FPointerToUberGraphFrame) is runtime bookkeeping, not widget state. */
bool IsUberGraphFrame(const FProperty* Property)
{
    const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
    return StructProperty && StructProperty->Struct
        && StructProperty->Struct->GetFName() == FName(TEXT("PointerToUberGraphFrame"));
}

/** Reflected properties declared below UUserWidget, minus widget references and delegates. */
TSharedPtr<FJsonObject> BuildPropertiesJson(const UUserWidget* Widget)
{
    TArray<const FProperty*> Properties;
    for (TFieldIterator<FProperty> It(Widget->GetClass()); It; ++It)
    {
        const UClass* Owner = It->GetOwner<UClass>();
        if (!Owner || Owner == UUserWidget::StaticClass() || !Owner->IsChildOf(UUserWidget::StaticClass())
            || IsWidgetReferenceOrDelegate(*It) || IsUberGraphFrame(*It))
        {
            continue;
        }
        Properties.Add(*It);
    }
    Properties.Sort([](const FProperty& A, const FProperty& B)
    {
        return A.GetName().Compare(B.GetName(), ESearchCase::CaseSensitive) < 0;
    });

    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    for (const FProperty* Property : Properties)
    {
        TSharedPtr<FJsonValue> Value = FCortexSerializer::PropertyToJson(Property, Property->ContainerPtrToValuePtr<void>(Widget));
        Json->SetField(Property->GetName(), Value.IsValid() ? Value : MakeShared<FJsonValueNull>());
    }
    return Json;
}

TSharedPtr<FJsonObject> BuildInstanceJson(const int32 Index, const UUserWidget* Instance, const FLiveWidgetRequest& Request)
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetNumberField(TEXT("index"), Index);
    Json->SetStringField(TEXT("name"), Instance->GetName());
    Json->SetStringField(TEXT("class"), Instance->GetClass()->GetName());
    Json->SetBoolField(TEXT("in_viewport"), Instance->IsInViewport());
    Json->SetStringField(TEXT("visibility"), VisibilityName(Instance->GetVisibility()));
    if (Request.bIncludeProperties)
    {
        Json->SetObjectField(TEXT("properties"), BuildPropertiesJson(Instance));
    }

    const UWidgetTree* Tree = Instance->WidgetTree;
    TArray<FString> Names = Request.Widgets;
    if (!Request.bHasWidgets && Tree)
    {
        Tree->ForEachWidget([&Names](UWidget* Child)
        {
            Names.Add(Child->GetName());
        });
        Names.Sort();
    }

    TSharedPtr<FJsonObject> WidgetsJson = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Missing;
    for (const FString& Name : Names)
    {
        // A name lookup that ignores bIsVariable; FNAME_Find avoids minting names for typos.
        const FName WidgetName(*Name, FNAME_Find);
        const UWidget* Child = (Tree && !WidgetName.IsNone()) ? Tree->FindWidget(WidgetName) : nullptr;
        if (Child)
        {
            WidgetsJson->SetObjectField(Name, BuildWidgetJson(Child));
        }
        else
        {
            Missing.Add(MakeShared<FJsonValueString>(Name));
        }
    }
    Json->SetObjectField(TEXT("widgets"), WidgetsJson);
    Json->SetArrayField(TEXT("missing"), Missing);
    return Json;
}

/**
 * Every UUserWidget of the PIE world whose class is the requested class or a child, including widgets
 * that are not in the viewport. GetUniqueID is a reusable slot, so instances are ordered by FName number
 * (the per-class creation counter) and then by name.
 */
FLiveReadback ReadLiveWidgets(UWorld* World, const FLiveWidgetRequest& Request)
{
    TArray<UUserWidget*> Matches;
    for (TObjectIterator<UUserWidget> It; It; ++It)
    {
        UUserWidget* Widget = *It;
        if (!IsValid(Widget) || Widget->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
            || Widget->GetWorld() != World || !Widget->IsA(Request.WidgetClass))
        {
            continue;
        }
        Matches.Add(Widget);
    }
    Matches.Sort([](const UUserWidget& A, const UUserWidget& B)
    {
        const int32 NumberA = A.GetFName().GetNumber();
        const int32 NumberB = B.GetFName().GetNumber();
        if (NumberA != NumberB)
        {
            return NumberA < NumberB;
        }
        return A.GetName().Compare(B.GetName(), ESearchCase::CaseSensitive) < 0;
    });

    FLiveReadback Readback;
    Readback.bTruncated = Matches.Num() > Request.MaxInstances;
    const int32 Count = FMath::Min(Matches.Num(), Request.MaxInstances);
    Readback.Instances.Reserve(Count);
    for (int32 Index = 0; Index < Count; ++Index)
    {
        Readback.Instances.Add(MakeShared<FJsonValueObject>(BuildInstanceJson(Index, Matches[Index], Request)));
    }
    return Readback;
}

/** Escapes the path metacharacters so a literal key such as "a.b" cannot alias the nested path a -> b. */
FString EscapeKeySegment(const FString& Segment)
{
    FString Escaped;
    Escaped.Reserve(Segment.Len());
    for (int32 Index = 0; Index < Segment.Len(); ++Index)
    {
        const TCHAR Character = Segment[Index];
        if (Character == TEXT('\\') || Character == TEXT('.') || Character == TEXT('[') || Character == TEXT(']'))
        {
            Escaped.AppendChar(TEXT('\\'));
        }
        Escaped.AppendChar(Character);
    }
    return Escaped;
}

FString JoinKey(const FString& Prefix, const FString& Segment)
{
    return Prefix.IsEmpty() ? Segment : Prefix + TEXT(".") + Segment;
}

/** Entries of one instance under `[<index>].<field path>`; identity metadata (name, index) is not compared. */
void FlattenInstance(const int32 Index, const TSharedPtr<FJsonValue>& Instance, FFlatEntries& Out)
{
    const TSharedPtr<FJsonObject>* Object = nullptr;
    if (!Instance.IsValid() || !Instance->TryGetObject(Object) || !Object || !Object->IsValid())
    {
        return;
    }
    const FString Prefix = FString::Printf(TEXT("[%d]"), Index);
    for (const auto& Pair : (*Object)->Values)
    {
        const FString Field = CortexEngineCompat::JsonKeyToString(Pair.Key);
        if (Field == TEXT("name") || Field == TEXT("index"))
        {
            continue;
        }
        FCortexUMGLiveWidgetOps::FlattenValue(JoinKey(Prefix, EscapeKeySegment(Field)), Pair.Value, Out);
    }
}

int32 CountEntries(const TArray<TSharedPtr<FJsonValue>>& Instances)
{
    int32 Total = 0;
    for (int32 Index = 0; Index < Instances.Num(); ++Index)
    {
        FFlatEntries Entries;
        FlattenInstance(Index, Instances[Index], Entries);
        Total += Entries.Num();
    }
    return Total;
}

FString InstanceName(const TSharedPtr<FJsonValue>& Instance)
{
    const TSharedPtr<FJsonObject>* Object = nullptr;
    FString Name;
    if (Instance.IsValid() && Instance->TryGetObject(Object) && Object && Object->IsValid())
    {
        (*Object)->TryGetStringField(TEXT("name"), Name);
    }
    return Name;
}

TSharedPtr<FJsonValue> InstanceSummary(const int32 Index, const TSharedPtr<FJsonValue>& Instance)
{
    FFlatEntries Entries;
    FlattenInstance(Index, Instance, Entries);
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetNumberField(TEXT("index"), Index);
    Json->SetStringField(TEXT("name"), InstanceName(Instance));
    Json->SetNumberField(TEXT("entry_count"), Entries.Num());
    return MakeShared<FJsonValueObject>(Json);
}
}

void FCortexUMGLiveWidgetOps::FlattenValue(
    const FString& Key, const TSharedPtr<FJsonValue>& Value, TMap<FString, TSharedPtr<FJsonValue>>& Out)
{
    if (!Value.IsValid())
    {
        Out.Add(Key, MakeShared<FJsonValueNull>());
        return;
    }
    switch (Value->Type)
    {
    case EJson::Object:
    {
        const TSharedPtr<FJsonObject>* Object = nullptr;
        if (Value->TryGetObject(Object) && Object && Object->IsValid() && (*Object)->Values.Num() > 0)
        {
            for (const auto& Pair : (*Object)->Values)
            {
                FlattenValue(JoinKey(Key, EscapeKeySegment(CortexEngineCompat::JsonKeyToString(Pair.Key))), Pair.Value, Out);
            }
        }
        else
        {
            // An empty object is a leaf: it must compare equal only to another empty object.
            Out.Add(Key, MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
        }
        break;
    }
    case EJson::Array:
    {
        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        if (Value->TryGetArray(Array) && Array && Array->Num() > 0)
        {
            for (int32 Index = 0; Index < Array->Num(); ++Index)
            {
                FlattenValue(FString::Printf(TEXT("%s[%d]"), *Key, Index), (*Array)[Index], Out);
            }
        }
        else
        {
            // An empty array is a leaf: it must compare equal only to another empty array.
            Out.Add(Key, MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>()));
        }
        break;
    }
    default:
        Out.Add(Key, Value);
        break;
    }
}

bool FCortexUMGLiveWidgetOps::LeavesEqual(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
{
    if (!A.IsValid() || !B.IsValid())
    {
        return A.IsValid() == B.IsValid();
    }
    if (A->Type != B->Type)
    {
        return false;
    }
    switch (A->Type)
    {
    case EJson::String:
        return A->AsString().Equals(B->AsString(), ESearchCase::CaseSensitive);
    case EJson::Number:
    {
        const double NumberA = A->AsNumber();
        const double NumberB = B->AsNumber();
        return NumberA == NumberB || (FMath::IsNaN(NumberA) && FMath::IsNaN(NumberB));
    }
    case EJson::Boolean:
        return A->AsBool() == B->AsBool();
    case EJson::Null:
    case EJson::Object:
    case EJson::Array:
        // Same type; containers only reach here as the empty-container leaves FlattenValue emits.
        return true;
    default:
        return false;
    }
}

FCortexCommandResult FCortexUMGLiveWidgetOps::GetLiveWidgets(const TSharedPtr<FJsonObject>& Params)
{
    FLiveWidgetRequest Request;
    FCortexCommandResult Error;
    if (!ParseRequest(Params, Request, Error))
    {
        return Error;
    }

    UWorld* World = FCortexPIEUtils::GetPIEWorld();
    if (!World)
    {
        return FCortexPIEUtils::PIENotActiveError();
    }

    FLiveReadback Readback = ReadLiveWidgets(World, Request);
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("count"), Readback.Instances.Num());
    Data->SetArrayField(TEXT("instances"), Readback.Instances);
    Data->SetBoolField(TEXT("truncated"), Readback.bTruncated);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGLiveWidgetOps::SnapshotLiveWidgets(const TSharedPtr<FJsonObject>& Params)
{
    FString Label;
    FCortexCommandResult Error;
    if (!ParseLabel(Params, Label, Error))
    {
        return Error;
    }
    FLiveWidgetRequest Request;
    if (!ParseRequest(Params, Request, Error))
    {
        return Error;
    }

    UWorld* World = FCortexPIEUtils::GetPIEWorld();
    if (!World)
    {
        return FCortexPIEUtils::PIENotActiveError();
    }

    FCortexResolvedFilePath Path;
    FString PathErrorCode;
    FString PathErrorMessage;
    if (!FCortexSafeFileContract::ResolveWritePath(SnapshotPath(Label), Path, PathErrorCode, PathErrorMessage))
    {
        return FCortexCommandRouter::Error(PathErrorCode, PathErrorMessage);
    }

    const FLiveReadback Readback = ReadLiveWidgets(World, Request);

    TSharedRef<FJsonObject> File = MakeShared<FJsonObject>();
    File->SetNumberField(TEXT("format_version"), SnapshotFormatVersion);
    File->SetStringField(TEXT("widget_class"), Request.WidgetClassPath);
    if (Request.bHasWidgets)
    {
        TArray<TSharedPtr<FJsonValue>> WidgetNames;
        for (const FString& Name : Request.Widgets)
        {
            WidgetNames.Add(MakeShared<FJsonValueString>(Name));
        }
        File->SetArrayField(TEXT("widgets"), WidgetNames);
    }
    File->SetBoolField(TEXT("include_properties"), Request.bIncludeProperties);
    File->SetNumberField(TEXT("max_instances"), Request.MaxInstances);
    File->SetStringField(TEXT("captured_at"), FDateTime::UtcNow().ToIso8601());
    File->SetBoolField(TEXT("truncated"), Readback.bTruncated);
    File->SetArrayField(TEXT("instances"), Readback.Instances);

    const FCortexJsonFileWriteResult WriteResult = FCortexSafeFileContract::WriteJsonReportAtomic(Path, File);
    if (!WriteResult.bWritten)
    {
        return FCortexCommandRouter::Error(
            WriteResult.ErrorCode.IsEmpty() ? CortexErrorCodes::SaveFailed : WriteResult.ErrorCode,
            WriteResult.ErrorMessage);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("label"), Label);
    Data->SetStringField(TEXT("path"), Path.AbsolutePath);
    Data->SetNumberField(TEXT("instance_count"), Readback.Instances.Num());
    Data->SetNumberField(TEXT("entry_count"), CountEntries(Readback.Instances));
    Data->SetBoolField(TEXT("truncated"), Readback.bTruncated);
    return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexUMGLiveWidgetOps::DiffLiveWidgets(const TSharedPtr<FJsonObject>& Params)
{
    FString Label;
    FCortexCommandResult Error;
    if (!ParseLabel(Params, Label, Error))
    {
        return Error;
    }

    FCortexResolvedFilePath Path;
    FString PathErrorCode;
    FString PathErrorMessage;
    if (!FCortexSafeFileContract::ResolveReadPath(SnapshotPath(Label), Path, PathErrorCode, PathErrorMessage))
    {
        return FCortexCommandRouter::Error(PathErrorCode, FString::Printf(TEXT("Widget snapshot '%s': %s"), *Label, *PathErrorMessage));
    }
    FString FileText;
    if (!FCortexSafeFileContract::ReadTextFile(Path, FileText, PathErrorCode, PathErrorMessage))
    {
        return FCortexCommandRouter::Error(PathErrorCode, PathErrorMessage);
    }

    TSharedPtr<FJsonObject> Snapshot;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileText);
    if (!FJsonSerializer::Deserialize(Reader, Snapshot) || !Snapshot.IsValid())
    {
        return InvalidField(FString::Printf(TEXT("Widget snapshot '%s' is not valid JSON: %s"), *Label, *Path.AbsolutePath));
    }
    double FormatVersion = 0.0;
    const TArray<TSharedPtr<FJsonValue>>* SnapshotInstances = nullptr;
    bool bSnapshotTruncated = false;
    // Typed checks: TryGet*Field would also accept numeric or boolean strings.
    const TSharedPtr<FJsonValue> VersionField = Snapshot->TryGetField(TEXT("format_version"));
    const TSharedPtr<FJsonValue> TruncatedField = Snapshot->TryGetField(TEXT("truncated"));
    if (!VersionField.IsValid() || VersionField->Type != EJson::Number
        || !VersionField->TryGetNumber(FormatVersion) || FormatVersion != static_cast<double>(SnapshotFormatVersion)
        || !TruncatedField.IsValid() || TruncatedField->Type != EJson::Boolean
        || !TruncatedField->TryGetBool(bSnapshotTruncated)
        || !Snapshot->TryGetArrayField(TEXT("instances"), SnapshotInstances) || SnapshotInstances == nullptr)
    {
        return InvalidField(FString::Printf(
            TEXT("Widget snapshot '%s' has an unsupported format (expected format_version exactly %d, a boolean truncated and an instances array)"),
            *Label, SnapshotFormatVersion));
    }
    for (const TSharedPtr<FJsonValue>& Entry : *SnapshotInstances)
    {
        if (!Entry.IsValid() || Entry->Type != EJson::Object)
        {
            return InvalidField(FString::Printf(TEXT("Widget snapshot '%s' is malformed: every instances entry must be a JSON object"), *Label));
        }
    }

    // Re-read with the snapshot's own selectors; the diff command accepts nothing but the label.
    FLiveWidgetRequest Request;
    if (!ParseRequest(Snapshot, Request, Error))
    {
        return Error;
    }

    UWorld* World = FCortexPIEUtils::GetPIEWorld();
    if (!World)
    {
        return FCortexPIEUtils::PIENotActiveError();
    }

    const FLiveReadback Live = ReadLiveWidgets(World, Request);

    int32 TotalEntries = 0;
    int32 DifferingEntries = 0;
    TArray<TSharedPtr<FJsonValue>> Differences;
    bool bDifferencesTruncated = false;

    const int32 PairedCount = FMath::Min(SnapshotInstances->Num(), Live.Instances.Num());
    for (int32 Index = 0; Index < PairedCount; ++Index)
    {
        FFlatEntries Before;
        FFlatEntries After;
        FlattenInstance(Index, (*SnapshotInstances)[Index], Before);
        FlattenInstance(Index, Live.Instances[Index], After);

        TArray<FString> Keys;
        Before.GenerateKeyArray(Keys);
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : After)
        {
            if (!Before.Contains(Entry.Key))
            {
                Keys.Add(Entry.Key);
            }
        }
        Keys.Sort();

        for (const FString& Key : Keys)
        {
            ++TotalEntries;
            const TSharedPtr<FJsonValue>* BeforeValue = Before.Find(Key);
            const TSharedPtr<FJsonValue>* AfterValue = After.Find(Key);
            if (BeforeValue && AfterValue && FCortexUMGLiveWidgetOps::LeavesEqual(*BeforeValue, *AfterValue))
            {
                continue;
            }
            ++DifferingEntries;
            if (Differences.Num() >= MaxReportedDifferences)
            {
                bDifferencesTruncated = true;
                continue;
            }
            TSharedPtr<FJsonObject> Difference = MakeShared<FJsonObject>();
            Difference->SetStringField(TEXT("key"), Key);
            if (BeforeValue)
            {
                Difference->SetField(TEXT("before"), *BeforeValue);
            }
            if (AfterValue)
            {
                Difference->SetField(TEXT("after"), *AfterValue);
            }
            Differences.Add(MakeShared<FJsonValueObject>(Difference));
        }
    }

    // Instances without a partner count wholly as differing entries and are named in their own lists.
    TArray<TSharedPtr<FJsonValue>> AddedInstances;
    TArray<TSharedPtr<FJsonValue>> RemovedInstances;
    for (int32 Index = PairedCount; Index < Live.Instances.Num(); ++Index)
    {
        const TSharedPtr<FJsonValue> Summary = InstanceSummary(Index, Live.Instances[Index]);
        const int32 EntryCount = static_cast<int32>(Summary->AsObject()->GetNumberField(TEXT("entry_count")));
        TotalEntries += EntryCount;
        DifferingEntries += EntryCount;
        AddedInstances.Add(Summary);
    }
    for (int32 Index = PairedCount; Index < SnapshotInstances->Num(); ++Index)
    {
        const TSharedPtr<FJsonValue> Summary = InstanceSummary(Index, (*SnapshotInstances)[Index]);
        const int32 EntryCount = static_cast<int32>(Summary->AsObject()->GetNumberField(TEXT("entry_count")));
        TotalEntries += EntryCount;
        DifferingEntries += EntryCount;
        RemovedInstances.Add(Summary);
    }

    const bool bComplete = !bSnapshotTruncated && !Live.bTruncated;
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("label"), Label);
    Data->SetNumberField(TEXT("total_entries"), TotalEntries);
    Data->SetNumberField(TEXT("differing_entries"), DifferingEntries);
    Data->SetBoolField(TEXT("identical"), bComplete && DifferingEntries == 0);
    Data->SetBoolField(TEXT("complete"), bComplete);
    Data->SetStringField(TEXT("summary"), FString::Printf(TEXT("%d of %d entries differ"), DifferingEntries, TotalEntries));
    Data->SetArrayField(TEXT("differences"), Differences);
    Data->SetBoolField(TEXT("differences_truncated"), bDifferencesTruncated);
    Data->SetArrayField(TEXT("added_instances"), AddedInstances);
    Data->SetArrayField(TEXT("removed_instances"), RemovedInstances);
    return FCortexCommandRouter::Success(Data);
}
