#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexUMGCommandHandler.h"
#include "CortexTypes.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Border.h"
#include "Components/TextBlock.h"
#include "Components/SizeBox.h"
#include "Dom/JsonObject.h"

/**
 * Contract tests for the UMG setter commands.
 *
 * Every rejected call must fail AND leave the widget unchanged: a setter that reports success while
 * ignoring what the caller asked for is worse than a refusal, so each refusal is checked against the
 * value a preceding valid call established.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCortexUMGSetterContractTest,
    "Cortex.UMG.SetterContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexUMGSetterContractTest::RunTest(const FString& Parameters)
{
    UPackage* TestPackage = CreatePackage(TEXT("/Temp/CortexUMGSetterContractTest"));
    UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
        TestPackage, TEXT("WBP_SetterContract"), RF_Public | RF_Standalone | RF_Transactional);
    WBP->ParentClass = UUserWidget::StaticClass();
    WBP->WidgetTree = NewObject<UWidgetTree>(WBP, TEXT("WidgetTree"));

    const FString AssetPath = WBP->GetPathName();

    FCortexCommandRouter Router;
    Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.1"),
        MakeShared<FCortexUMGCommandHandler>());

    auto AddWidget = [&Router, &AssetPath](
        const TCHAR* WidgetClass, const TCHAR* Name, const TCHAR* ParentName)
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
    };

    TestTrue(TEXT("fixture root should be created"),
        AddWidget(TEXT("CanvasPanel"), TEXT("Root"), nullptr).bSuccess);
    TestTrue(TEXT("fixture border should be created"),
        AddWidget(TEXT("Border"), TEXT("FormBox"), TEXT("Root")).bSuccess);
    TestTrue(TEXT("fixture label should be created"),
        AddWidget(TEXT("TextBlock"), TEXT("TitleLabel"), TEXT("Root")).bSuccess);
    TestTrue(TEXT("fixture size box should be created"),
        AddWidget(TEXT("SizeBox"), TEXT("SizeHolder"), TEXT("Root")).bSuccess);
    TestTrue(TEXT("fixture box host should be created"),
        AddWidget(TEXT("VerticalBox"), TEXT("Column"), TEXT("Root")).bSuccess);
    TestTrue(TEXT("fixture box child should be created"),
        AddWidget(TEXT("TextBlock"), TEXT("ColumnLabel"), TEXT("Column")).bSuccess);

    UBorder* FormBox = Cast<UBorder>(WBP->WidgetTree->FindWidget(TEXT("FormBox")));
    UTextBlock* TitleLabel = Cast<UTextBlock>(WBP->WidgetTree->FindWidget(TEXT("TitleLabel")));
    USizeBox* SizeHolder = Cast<USizeBox>(WBP->WidgetTree->FindWidget(TEXT("SizeHolder")));
    UTextBlock* ColumnLabel = Cast<UTextBlock>(WBP->WidgetTree->FindWidget(TEXT("ColumnLabel")));
    TestNotNull(TEXT("FormBox should be a Border"), FormBox);
    TestNotNull(TEXT("TitleLabel should be a TextBlock"), TitleLabel);
    TestNotNull(TEXT("SizeHolder should be a SizeBox"), SizeHolder);
    TestNotNull(TEXT("ColumnLabel should be a TextBlock in a box slot"), ColumnLabel);

    // Alignment lives on box/overlay slots, not on a CanvasPanel slot, so it is read generically.
    auto ReadSlotAlignment = [](UWidget* Target) -> TOptional<EHorizontalAlignment>
    {
        if (Target == nullptr || Target->Slot == nullptr)
        {
            return {};
        }
        FProperty* Prop = Target->Slot->GetClass()->FindPropertyByName(TEXT("HorizontalAlignment"));
        if (Prop == nullptr)
        {
            return {};
        }
        return *Prop->ContainerPtrToValuePtr<EHorizontalAlignment>(Target->Slot);
    };

    auto ReadSlotVerticalAlignment = [](UWidget* Target) -> TOptional<EVerticalAlignment>
    {
        if (Target == nullptr || Target->Slot == nullptr)
        {
            return {};
        }
        FProperty* Prop = Target->Slot->GetClass()->FindPropertyByName(TEXT("VerticalAlignment"));
        if (Prop == nullptr)
        {
            return {};
        }
        return *Prop->ContainerPtrToValuePtr<EVerticalAlignment>(Target->Slot);
    };

    // ---------------------------------------------------------------- set_padding
    {
        TSharedPtr<FJsonObject> Padding = MakeShared<FJsonObject>();
        Padding->SetNumberField(TEXT("top"), 16.0);

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("widget_name"), TEXT("FormBox"));
        Params->SetObjectField(TEXT("padding"), Padding);

        const FCortexCommandResult Result = Router.Execute(TEXT("umg.set_padding"), Params);
        TestTrue(TEXT("nested padding form should succeed"), Result.bSuccess);
        if (FormBox)
        {
            TestTrue(TEXT("nested padding should apply top=16"),
                FMath::IsNearlyEqual(FormBox->GetPadding().Top, 16.0f));
        }
    }
    {
        // A flat edge key is not part of the contract and must not be silently dropped.
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("widget_name"), TEXT("FormBox"));
        Params->SetNumberField(TEXT("top"), 4.0);

        const FCortexCommandResult Result = Router.Execute(TEXT("umg.set_padding"), Params);
        TestFalse(TEXT("flat top key must be refused"), Result.bSuccess);
        TestEqual(TEXT("flat top key should be INVALID_FIELD"),
            Result.ErrorCode, CortexErrorCodes::InvalidField);
        if (FormBox)
        {
            TestTrue(TEXT("refused flat-padding call must not change padding"),
                FMath::IsNearlyEqual(FormBox->GetPadding().Top, 16.0f));
        }
    }
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("widget_name"), TEXT("FormBox"));

        const FCortexCommandResult Result = Router.Execute(TEXT("umg.set_padding"), Params);
        TestFalse(TEXT("missing required padding must be refused"), Result.bSuccess);
        TestEqual(TEXT("missing padding should be INVALID_FIELD"),
            Result.ErrorCode, CortexErrorCodes::InvalidField);
        if (FormBox)
        {
            TestTrue(TEXT("refused missing-padding call must not change padding"),
                FMath::IsNearlyEqual(FormBox->GetPadding().Top, 16.0f));
        }
    }
    {
        // Router-owned keys are not part of any command contract and must be tolerated.
        TSharedPtr<FJsonObject> Padding = MakeShared<FJsonObject>();
        Padding->SetNumberField(TEXT("top"), 8.0);

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("widget_name"), TEXT("FormBox"));
        Params->SetObjectField(TEXT("padding"), Padding);
        Params->SetBoolField(TEXT("_telemetry_parallel"), true);

        const FCortexCommandResult Result = Router.Execute(TEXT("umg.set_padding"), Params);
        TestTrue(TEXT("router-owned underscore keys must be accepted"), Result.bSuccess);
        if (FormBox)
        {
            TestTrue(TEXT("padding should still apply with a router-owned key present"),
                FMath::IsNearlyEqual(FormBox->GetPadding().Top, 8.0f));
        }
    }

    // ---------------------------------------------------------------- set_anchor
    UCanvasPanelSlot* FormBoxSlot = FormBox ? Cast<UCanvasPanelSlot>(FormBox->Slot) : nullptr;
    TestNotNull(TEXT("FormBox should be a CanvasPanel child"), FormBoxSlot);
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("widget_name"), TEXT("FormBox"));
        Params->SetStringField(TEXT("preset"), TEXT("Center"));

        const FCortexCommandResult Result = Router.Execute(TEXT("umg.set_anchor"), Params);
        TestTrue(TEXT("valid preset should succeed"), Result.bSuccess);
        if (FormBoxSlot)
        {
            const FAnchors Anchors = FormBoxSlot->GetAnchors();
            TestTrue(TEXT("Center preset should set min to 0.5"),
                FMath::IsNearlyEqual(Anchors.Minimum.X, 0.5f) && FMath::IsNearlyEqual(Anchors.Minimum.Y, 0.5f));
            TestTrue(TEXT("Center preset should set max to 0.5"),
                FMath::IsNearlyEqual(Anchors.Maximum.X, 0.5f) && FMath::IsNearlyEqual(Anchors.Maximum.Y, 0.5f));
        }
    }
    {
        // `anchor` is not a parameter of this command; the preset is named `preset`.
        TSharedPtr<FJsonObject> Alignment = MakeShared<FJsonObject>();
        Alignment->SetNumberField(TEXT("x"), 0.5);
        Alignment->SetNumberField(TEXT("y"), 0.5);

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("widget_name"), TEXT("FormBox"));
        Params->SetStringField(TEXT("anchor"), TEXT("BottomRight"));
        Params->SetObjectField(TEXT("alignment"), Alignment);

        const FVector2D AlignmentBefore = FormBoxSlot ? FormBoxSlot->GetAlignment() : FVector2D(0.0f, 0.0f);
        const FCortexCommandResult Result = Router.Execute(TEXT("umg.set_anchor"), Params);
        TestFalse(TEXT("unknown anchor field must be refused"), Result.bSuccess);
        TestEqual(TEXT("unknown anchor field should be INVALID_FIELD"),
            Result.ErrorCode, CortexErrorCodes::InvalidField);
        if (FormBoxSlot)
        {
            TestTrue(TEXT("refused anchor call must not change anchors"),
                FMath::IsNearlyEqual(FormBoxSlot->GetAnchors().Minimum.X, 0.5f));
            // The refused payload carried an alignment the old handler would have applied.
            TestTrue(TEXT("refused anchor call must not change slot alignment"),
                FMath::IsNearlyEqual(FormBoxSlot->GetAlignment().X, AlignmentBefore.X)
                && FMath::IsNearlyEqual(FormBoxSlot->GetAlignment().Y, AlignmentBefore.Y));
        }
    }
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("widget_name"), TEXT("FormBox"));
        Params->SetStringField(TEXT("preset"), TEXT("NotAPreset"));

        const FCortexCommandResult Result = Router.Execute(TEXT("umg.set_anchor"), Params);
        TestFalse(TEXT("unknown preset token must be refused"), Result.bSuccess);
        if (FormBoxSlot)
        {
            TestTrue(TEXT("refused preset token must not change anchors"),
                FMath::IsNearlyEqual(FormBoxSlot->GetAnchors().Minimum.X, 0.5f));
        }
    }
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("widget_name"), TEXT("FormBox"));

        const FCortexCommandResult Result = Router.Execute(TEXT("umg.set_anchor"), Params);
        TestFalse(TEXT("empty anchor request must be refused"), Result.bSuccess);
        if (FormBoxSlot)
        {
            TestTrue(TEXT("refused empty anchor call must not change anchors"),
                FMath::IsNearlyEqual(FormBoxSlot->GetAnchors().Minimum.X, 0.5f));
        }
    }
    {
        // The custom min/max form is supported and must keep working.
        TSharedPtr<FJsonObject> Min = MakeShared<FJsonObject>();
        Min->SetNumberField(TEXT("x"), 0.25);
        Min->SetNumberField(TEXT("y"), 0.25);
        TSharedPtr<FJsonObject> Max = MakeShared<FJsonObject>();
        Max->SetNumberField(TEXT("x"), 0.75);
        Max->SetNumberField(TEXT("y"), 0.75);

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("widget_name"), TEXT("FormBox"));
        Params->SetObjectField(TEXT("min"), Min);
        Params->SetObjectField(TEXT("max"), Max);

        const FCortexCommandResult Result = Router.Execute(TEXT("umg.set_anchor"), Params);
        TestTrue(TEXT("custom min/max anchor form must keep working"), Result.bSuccess);
        if (FormBoxSlot)
        {
            TestTrue(TEXT("custom min anchor should apply"),
                FMath::IsNearlyEqual(FormBoxSlot->GetAnchors().Minimum.X, 0.25f));
            TestTrue(TEXT("custom max anchor should apply"),
                FMath::IsNearlyEqual(FormBoxSlot->GetAnchors().Maximum.X, 0.75f));
        }
    }

    // ---------------------------------------------------------------- set_alignment
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("widget_name"), TEXT("ColumnLabel"));
        Params->SetStringField(TEXT("horizontal"), TEXT("Center"));

        TestTrue(TEXT("valid alignment should succeed"),
            Router.Execute(TEXT("umg.set_alignment"), Params).bSuccess);

        const TOptional<EHorizontalAlignment> Applied = ReadSlotAlignment(ColumnLabel);
        TestTrue(TEXT("horizontal alignment should apply to the box slot"),
            Applied.IsSet() && Applied.GetValue() == EHorizontalAlignment::HAlign_Center);
    }
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("widget_name"), TEXT("ColumnLabel"));

        const FCortexCommandResult Result = Router.Execute(TEXT("umg.set_alignment"), Params);
        TestFalse(TEXT("empty alignment request must be refused"), Result.bSuccess);
        const TOptional<EHorizontalAlignment> AfterRefusal = ReadSlotAlignment(ColumnLabel);
        TestTrue(TEXT("refused empty alignment must not change the slot"),
            AfterRefusal.IsSet() && AfterRefusal.GetValue() == EHorizontalAlignment::HAlign_Center);
    }
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("widget_name"), TEXT("ColumnLabel"));
        Params->SetStringField(TEXT("horizontal"), TEXT("Sideways"));

        const FCortexCommandResult Result = Router.Execute(TEXT("umg.set_alignment"), Params);
        TestFalse(TEXT("unknown alignment token must be refused"), Result.bSuccess);
        const TOptional<EHorizontalAlignment> AfterRefusal = ReadSlotAlignment(ColumnLabel);
        TestTrue(TEXT("refused alignment token must not change the slot"),
            AfterRefusal.IsSet() && AfterRefusal.GetValue() == EHorizontalAlignment::HAlign_Center);
    }
    {
        // An explicitly supplied empty token is not a valid alignment. Ignoring it while applying the
        // other axis would silently discard what the caller asked for.
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("widget_name"), TEXT("ColumnLabel"));
        Params->SetStringField(TEXT("horizontal"), TEXT(""));
        Params->SetStringField(TEXT("vertical"), TEXT("Bottom"));

        const TOptional<EHorizontalAlignment> HorizontalBefore = ReadSlotAlignment(ColumnLabel);
        const TOptional<EVerticalAlignment> VerticalBefore = ReadSlotVerticalAlignment(ColumnLabel);

        const FCortexCommandResult Result = Router.Execute(TEXT("umg.set_alignment"), Params);
        TestFalse(TEXT("supplied empty alignment token must be refused"), Result.bSuccess);
        TestEqual(TEXT("empty alignment token should be INVALID_PROPERTY_VALUE"),
            Result.ErrorCode, CortexErrorCodes::InvalidPropertyValue);
        const TOptional<EHorizontalAlignment> HorizontalAfter = ReadSlotAlignment(ColumnLabel);
        const TOptional<EVerticalAlignment> VerticalAfter = ReadSlotVerticalAlignment(ColumnLabel);
        TestTrue(TEXT("refused empty token must leave horizontal alignment unchanged"),
            HorizontalAfter.IsSet() == HorizontalBefore.IsSet()
            && (!HorizontalAfter.IsSet() || HorizontalAfter.GetValue() == HorizontalBefore.GetValue()));
        TestTrue(TEXT("refused empty token must leave vertical alignment unchanged"),
            VerticalAfter.IsSet() == VerticalBefore.IsSet()
            && (!VerticalAfter.IsSet() || VerticalAfter.GetValue() == VerticalBefore.GetValue()));
    }

    // ---------------------------------------------------------------- set_size
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("widget_name"), TEXT("SizeHolder"));
        Params->SetNumberField(TEXT("width"), 320.0);

        const FCortexCommandResult Result = Router.Execute(TEXT("umg.set_size"), Params);
        TestTrue(TEXT("valid width should succeed"), Result.bSuccess);
        if (SizeHolder)
        {
            TestTrue(TEXT("width override should apply"),
                FMath::IsNearlyEqual(SizeHolder->GetWidthOverride(), 320.0f));
        }
    }
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("widget_name"), TEXT("SizeHolder"));

        const FCortexCommandResult Result = Router.Execute(TEXT("umg.set_size"), Params);
        TestFalse(TEXT("empty size request must be refused"), Result.bSuccess);
        if (SizeHolder)
        {
            TestTrue(TEXT("refused size call must not change the size"),
                FMath::IsNearlyEqual(SizeHolder->GetWidthOverride(), 320.0f));
        }
    }

    // ---------------------------------------------------------------- set_text / set_font
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("widget_name"), TEXT("TitleLabel"));
        Params->SetStringField(TEXT("text"), TEXT("Hello"));

        TestTrue(TEXT("valid text should succeed"),
            Router.Execute(TEXT("umg.set_text"), Params).bSuccess);
    }
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("widget_name"), TEXT("TitleLabel"));

        const FCortexCommandResult Result = Router.Execute(TEXT("umg.set_text"), Params);
        TestFalse(TEXT("missing required text must be refused"), Result.bSuccess);
        TestEqual(TEXT("missing text should be INVALID_FIELD"),
            Result.ErrorCode, CortexErrorCodes::InvalidField);
        if (TitleLabel)
        {
            TestTrue(TEXT("refused set_text must not change the label"),
                TitleLabel->GetText().ToString() == TEXT("Hello"));
        }
    }
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("widget_name"), TEXT("TitleLabel"));
        Params->SetNumberField(TEXT("outline_size"), 3.0);

        TestTrue(TEXT("declared outline_size font form must keep working"),
            Router.Execute(TEXT("umg.set_font"), Params).bSuccess);
        if (TitleLabel)
        {
            TestEqual(TEXT("outline size should apply"),
                TitleLabel->GetFont().OutlineSettings.OutlineSize, 3);
        }
    }
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetStringField(TEXT("widget_name"), TEXT("TitleLabel"));

        const FCortexCommandResult Result = Router.Execute(TEXT("umg.set_font"), Params);
        TestFalse(TEXT("empty font request must be refused"), Result.bSuccess);
        if (TitleLabel)
        {
            TestEqual(TEXT("refused set_font must not change the font"),
                TitleLabel->GetFont().OutlineSettings.OutlineSize, 3);
        }
    }

    WBP->MarkAsGarbage();
    return true;
}
