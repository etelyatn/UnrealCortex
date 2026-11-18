#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "CortexTypes.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
    struct FUMGPropertyHonestyFixture
    {
        UPackage* Package = nullptr;
        UWidgetBlueprint* WidgetBlueprint = nullptr;
        FString AssetPath;
        FCortexCommandRouter Router;

        FUMGPropertyHonestyFixture(const TCHAR* PackageName, const TCHAR* AssetName)
        {
            Package = CreatePackage(PackageName);
            WidgetBlueprint = NewObject<UWidgetBlueprint>(
                Package, AssetName, RF_Public | RF_Standalone | RF_Transactional);
            WidgetBlueprint->ParentClass = UUserWidget::StaticClass();
            WidgetBlueprint->WidgetTree = NewObject<UWidgetTree>(WidgetBlueprint, TEXT("WidgetTree"));
            AssetPath = WidgetBlueprint->GetPathName();

            Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.1"),
                MakeShared<FCortexUMGCommandHandler>());
        }

        ~FUMGPropertyHonestyFixture()
        {
            if (WidgetBlueprint != nullptr)
            {
                WidgetBlueprint->MarkAsGarbage();
            }
        }

        FCortexCommandResult AddWidget(const TCHAR* WidgetClass, const TCHAR* Name, const TCHAR* ParentName)
        {
            TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
            Params->SetStringField(TEXT("asset_path"), AssetPath);
            Params->SetStringField(TEXT("widget_class"), WidgetClass);
            Params->SetStringField(TEXT("name"), Name);
            if (ParentName != nullptr)
            {
                Params->SetStringField(TEXT("parent_name"), ParentName);
            }
            return Router.Execute(TEXT("umg.add_widget"), Params);
        }

        FCortexCommandResult SetProperty(
            const TCHAR* WidgetName,
            const TCHAR* PropertyPath,
            const TSharedPtr<FJsonValue>& Value)
        {
            TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
            Params->SetStringField(TEXT("asset_path"), AssetPath);
            Params->SetStringField(TEXT("widget_name"), WidgetName);
            Params->SetStringField(TEXT("property_path"), PropertyPath);
            if (Value.IsValid())
            {
                Params->SetField(TEXT("value"), Value);
            }
            return Router.Execute(TEXT("umg.set_property"), Params);
        }

        FCortexCommandResult GetProperty(const TCHAR* WidgetName, const TCHAR* PropertyPath)
        {
            TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
            Params->SetStringField(TEXT("asset_path"), AssetPath);
            Params->SetStringField(TEXT("widget_name"), WidgetName);
            Params->SetStringField(TEXT("property_path"), PropertyPath);
            return Router.Execute(TEXT("umg.get_property"), Params);
        }
    };

    /** Every string the response exposes: message, detail/data strings/arrays, and envelope warnings. */
    FString CollectSurfacedText(const FCortexCommandResult& Result)
    {
        FString Surface = Result.ErrorMessage;

        auto AppendObjectStrings = [&Surface](const TSharedPtr<FJsonObject>& Object)
        {
            if (!Object.IsValid())
            {
                return;
            }

            for (const auto& Pair : Object->Values)
            {
                const TSharedPtr<FJsonValue>& Value = Pair.Value;
                if (!Value.IsValid())
                {
                    continue;
                }
                if (Value->Type == EJson::String)
                {
                    Surface += TEXT("\n") + Value->AsString();
                }
                else if (Value->Type == EJson::Array)
                {
                    for (const TSharedPtr<FJsonValue>& Entry : Value->AsArray())
                    {
                        if (Entry.IsValid() && Entry->Type == EJson::String)
                        {
                            Surface += TEXT("\n") + Entry->AsString();
                        }
                    }
                }
            }
        };

        AppendObjectStrings(Result.ErrorDetails);
        AppendObjectStrings(Result.Data);

        for (const FString& Warning : Result.Warnings)
        {
            Surface += TEXT("\n") + Warning;
        }

        return Surface;
    }

    bool ReadStringProperty(
        FUMGPropertyHonestyFixture& Fixture,
        const TCHAR* WidgetName,
        const TCHAR* PropertyPath,
        FString& OutValue)
    {
        const FCortexCommandResult GetResult = Fixture.GetProperty(WidgetName, PropertyPath);
        if (!GetResult.bSuccess || !GetResult.Data.IsValid())
        {
            return false;
        }
        return GetResult.Data->TryGetStringField(TEXT("value"), OutValue);
    }

    bool ReadBoolProperty(
        FUMGPropertyHonestyFixture& Fixture,
        const TCHAR* WidgetName,
        const TCHAR* PropertyPath,
        bool& OutValue)
    {
        const FCortexCommandResult GetResult = Fixture.GetProperty(WidgetName, PropertyPath);
        if (!GetResult.bSuccess || !GetResult.Data.IsValid())
        {
            return false;
        }
        return GetResult.Data->TryGetBoolField(TEXT("value"), OutValue);
    }
}

/**
 * `umg.set_property` previously reported `set:true` while writing nothing when `value` was absent.
 * A missing required parameter is INVALID_FIELD and the widget keeps its previous value.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGSetPropertyMissingValueTest,
    "Cortex.UMG.SetProperty.MissingValue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGSetPropertyMissingValueTest::RunTest(const FString& Parameters)
{
    FUMGPropertyHonestyFixture Fixture(
        TEXT("/Temp/CortexUMGSetPropertyMissingValueTest"), TEXT("WBP_MissingValue"));
    TestTrue(TEXT("fixture panel should be created"), Fixture.AddWidget(TEXT("CanvasPanel"), TEXT("Root"), nullptr).bSuccess);
    TestTrue(TEXT("fixture border should be created"), Fixture.AddWidget(TEXT("Border"), TEXT("FormBox"), TEXT("Root")).bSuccess);

    // Establish a non-default value so a silent no-op is observable.
    const FCortexCommandResult Baseline = Fixture.SetProperty(
        TEXT("FormBox"), TEXT("bIsEnabled"), MakeShared<FJsonValueBoolean>(false));
    TestTrue(TEXT("baseline set_property should succeed"), Baseline.bSuccess);

    bool bBefore = true;
    TestTrue(TEXT("baseline value should be readable"), ReadBoolProperty(Fixture, TEXT("FormBox"), TEXT("bIsEnabled"), bBefore));
    TestFalse(TEXT("baseline value should be false"), bBefore);

    const FCortexCommandResult SetResult = Fixture.SetProperty(TEXT("FormBox"), TEXT("bIsEnabled"), nullptr);
    TestFalse(TEXT("set_property without value must be refused"), SetResult.bSuccess);
    TestEqual(
        *FString::Printf(TEXT("set_property without value must be INVALID_FIELD (got %s: %s)"),
            *SetResult.ErrorCode, *SetResult.ErrorMessage),
        SetResult.ErrorCode,
        CortexErrorCodes::InvalidField);

    bool bSet = false;
    if (SetResult.Data.IsValid())
    {
        SetResult.Data->TryGetBoolField(TEXT("set"), bSet);
    }
    TestFalse(TEXT("A refused set_property must not report set:true"), bSet);

    bool bAfter = true;
    TestTrue(TEXT("value should still be readable after the refusal"),
        ReadBoolProperty(Fixture, TEXT("FormBox"), TEXT("bIsEnabled"), bAfter));
    TestFalse(TEXT("A refused set_property must not change the widget"), bAfter);

    return true;
}

/**
 * An unknown enum name must not be a silent or opaque failure: the valid names travel with the
 * refusal so the caller can correct the request.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGSetPropertyUnknownEnumWarningsTest,
    "Cortex.UMG.SetProperty.UnknownEnumWarnings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGSetPropertyUnknownEnumWarningsTest::RunTest(const FString& Parameters)
{
    FUMGPropertyHonestyFixture Fixture(
        TEXT("/Temp/CortexUMGSetPropertyUnknownEnumWarningsTest"), TEXT("WBP_UnknownEnum"));
    TestTrue(TEXT("fixture panel should be created"), Fixture.AddWidget(TEXT("CanvasPanel"), TEXT("Root"), nullptr).bSuccess);
    TestTrue(TEXT("fixture border should be created"), Fixture.AddWidget(TEXT("Border"), TEXT("FormBox"), TEXT("Root")).bSuccess);

    const FCortexCommandResult SetResult = Fixture.SetProperty(
        TEXT("FormBox"), TEXT("Visibility"), MakeShared<FJsonValueString>(TEXT("NotASlateVisibility")));
    TestFalse(TEXT("An unknown enum name must be refused"), SetResult.bSuccess);
    TestEqual(
        *FString::Printf(TEXT("An unknown enum name is INVALID_PROPERTY_VALUE (got %s: %s)"),
            *SetResult.ErrorCode, *SetResult.ErrorMessage),
        SetResult.ErrorCode,
        CortexErrorCodes::InvalidPropertyValue);

    const FString Surface = CollectSurfacedText(SetResult);
    TestTrue(
        FString::Printf(TEXT("The refusal must surface the valid enum names (got: %s)"), *Surface),
        Surface.Contains(TEXT("Collapsed")) && Surface.Contains(TEXT("HitTestInvisible")));

    FString VisibilityAfter;
    TestTrue(TEXT("Visibility should still be readable"),
        ReadStringProperty(Fixture, TEXT("FormBox"), TEXT("Visibility"), VisibilityAfter));
    TestEqual(TEXT("A refused enum write must not change the widget"), VisibilityAfter, FString(TEXT("Visible")));

    return true;
}

/**
 * A serializer warning that does not fail the write must still reach the caller instead of being
 * dropped, so a partially-understood value is never reported as a clean success.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGSetPropertyStructFieldWarningsTest,
    "Cortex.UMG.SetProperty.StructFieldWarnings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGSetPropertyStructFieldWarningsTest::RunTest(const FString& Parameters)
{
    FUMGPropertyHonestyFixture Fixture(
        TEXT("/Temp/CortexUMGSetPropertyStructFieldWarningsTest"), TEXT("WBP_StructWarnings"));
    TestTrue(TEXT("fixture panel should be created"), Fixture.AddWidget(TEXT("CanvasPanel"), TEXT("Root"), nullptr).bSuccess);
    TestTrue(TEXT("fixture border should be created"), Fixture.AddWidget(TEXT("Border"), TEXT("FormBox"), TEXT("Root")).bSuccess);

    TSharedPtr<FJsonObject> PaddingValue = MakeShared<FJsonObject>();
    PaddingValue->SetNumberField(TEXT("NoSuchPaddingField"), 7.0);

    const FCortexCommandResult SetResult = Fixture.SetProperty(
        TEXT("FormBox"), TEXT("Padding"), MakeShared<FJsonValueObject>(PaddingValue));

    TestTrue(
        FString::Printf(TEXT("An unknown struct field is a warning, not a failure (got %s: %s)"),
            *SetResult.ErrorCode, *SetResult.ErrorMessage),
        SetResult.bSuccess);

    const FString Surface = CollectSurfacedText(SetResult);
    TestTrue(
        FString::Printf(TEXT("The serializer warning must reach the caller (got: %s)"), *Surface),
        Surface.Contains(TEXT("NoSuchPaddingField")));

    return true;
}

/**
 * Legacy `PropertyToJson` expansion of `UWidget::Slot` recurses through Slot->Content->Slot, so this
 * read must terminate with a bounded value. Kept under its own test name because the current code
 * overflows the stack.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGGetPropertySlotBoundedTest,
    "Cortex.UMG.GetProperty.SlotBounded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGGetPropertySlotBoundedTest::RunTest(const FString& Parameters)
{
    FUMGPropertyHonestyFixture Fixture(
        TEXT("/Temp/CortexUMGGetPropertySlotBoundedTest"), TEXT("WBP_SlotBounded"));
    TestTrue(TEXT("fixture root should be created"), Fixture.AddWidget(TEXT("CanvasPanel"), TEXT("Root"), nullptr).bSuccess);
    TestTrue(TEXT("fixture child should be created"),
        Fixture.AddWidget(TEXT("TextBlock"), TEXT("Label"), TEXT("Root")).bSuccess);

    const FCortexCommandResult GetResult = Fixture.GetProperty(TEXT("Label"), TEXT("Slot"));
    TestTrue(
        FString::Printf(TEXT("get_property Slot must succeed (got %s: %s)"),
            *GetResult.ErrorCode, *GetResult.ErrorMessage),
        GetResult.bSuccess);

    if (!GetResult.Data.IsValid())
    {
        AddError(TEXT("get_property Slot must return data"));
        return true;
    }

    const TSharedPtr<FJsonValue> SlotValue = GetResult.Data->TryGetField(TEXT("value"));
    TestTrue(TEXT("get_property Slot must return a value field"), SlotValue.IsValid());
    if (SlotValue.IsValid())
    {
        TestFalse(TEXT("get_property Slot must not return null"), SlotValue->IsNull());
        TestTrue(TEXT("get_property Slot must return an object or an identity string"),
            SlotValue->Type == EJson::Object || SlotValue->Type == EJson::String);
    }

    const FString Serialized = FCortexCommandRouter::ResultToJson(GetResult, 0.0);
    TestTrue(TEXT("The serialized response must not be empty"), Serialized.Len() > 0);
    TestTrue(
        FString::Printf(TEXT("The serialized response must be bounded (got %d chars)"), Serialized.Len()),
        Serialized.Len() < 1000000);

    return true;
}
