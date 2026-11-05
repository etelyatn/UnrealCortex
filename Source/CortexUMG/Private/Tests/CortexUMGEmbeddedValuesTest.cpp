// umg.get_embedded_widget_values exports, per embedded UserWidget instance, every instance-editable
// property declared below UUserWidget (Blueprint variables and native UPROPERTYs alike) with its
// value and whether the instance overrides the class default.

#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "CortexUMGCommandHandler.h"
#include "CortexUMGEmbeddedValueTestTypes.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace
{
	TStrongObjectPtr<UWidgetBlueprint> CreateEmbeddedValueBlueprint(UClass* ParentClass, const TCHAR* Prefix)
	{
		UPackage* Package = CreatePackage(*(FString(TEXT("/Temp/")) + Prefix +
			FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		const FName Name = FName(*FPackageName::GetLongPackageAssetName(Package->GetName()));
		TStrongObjectPtr<UWidgetBlueprint> Blueprint(CastChecked<UWidgetBlueprint>(
			FKismetEditorUtilities::CreateBlueprint(ParentClass, Package, Name, BPTYPE_Normal,
				UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass(), NAME_None)));
		Blueprint->SetFlags(RF_Transactional);
		Blueprint->WidgetTree->RootWidget =
			Blueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
		return Blueprint;
	}

	void RetireBlueprint(TStrongObjectPtr<UWidgetBlueprint>& Blueprint)
	{
		if (Blueprint.IsValid())
		{
			UPackage* Package = Blueprint->GetPackage();
			ResetLoaders(Package);
			Package->ClearDirtyFlag();
			Blueprint->ClearFlags(RF_Standalone);
			Blueprint->MarkAsGarbage();
			Package->MarkAsGarbage();
			Blueprint.Reset();
		}
	}

	TSharedPtr<FJsonObject> FindWidgetRecord(const FCortexCommandResult& Result, const FString& WidgetName)
	{
		const TArray<TSharedPtr<FJsonValue>>* Widgets = nullptr;
		if (!Result.Data.IsValid() || !Result.Data->TryGetArrayField(TEXT("widgets"), Widgets))
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Widgets)
		{
			const TSharedPtr<FJsonObject> Record = Value->AsObject();
			if (Record.IsValid() && Record->GetStringField(TEXT("widget_name")) == WidgetName)
			{
				return Record;
			}
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> FindProperty(const TSharedPtr<FJsonObject>& WidgetRecord, const FString& PropertyName)
	{
		if (!WidgetRecord.IsValid())
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Value : WidgetRecord->GetArrayField(TEXT("properties")))
		{
			const TSharedPtr<FJsonObject> Property = Value->AsObject();
			if (Property.IsValid() && Property->GetStringField(TEXT("property")) == PropertyName)
			{
				return Property;
			}
		}
		return nullptr;
	}

	FString PropertyNames(const TSharedPtr<FJsonObject>& WidgetRecord)
	{
		TArray<FString> Names;
		for (const TSharedPtr<FJsonValue>& Value : WidgetRecord->GetArrayField(TEXT("properties")))
		{
			Names.Add(Value->AsObject()->GetStringField(TEXT("property")));
		}
		return FString::Join(Names, TEXT(","));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGEmbeddedWidgetValuesTest,
	"Cortex.UMG.EmbeddedWidgetValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGEmbeddedWidgetValuesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	TStrongObjectPtr<UWidgetBlueprint> Inner =
		CreateEmbeddedValueBlueprint(UCortexUMGEmbeddedValueTestWidget::StaticClass(), TEXT("CortexEmbeddedInner_"));

	FEdGraphPinType FloatType;
	FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
	FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
	FEdGraphPinType ColorType;
	ColorType.PinCategory = UEdGraphSchema_K2::PC_Struct;
	ColorType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
	const bool bVariablesAdded =
		FBlueprintEditorUtils::AddMemberVariable(Inner.Get(), TEXT("Speed"), FloatType, TEXT("1.0")) &&
		FBlueprintEditorUtils::AddMemberVariable(Inner.Get(), TEXT("Tint"), ColorType) &&
		FBlueprintEditorUtils::AddMemberVariable(Inner.Get(), TEXT("HiddenGain"), FloatType, TEXT("9.0"));
	if (!TestTrue(TEXT("inner variables added"), bVariablesAdded))
	{
		RetireBlueprint(Inner);
		return false;
	}
	FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Inner.Get(), TEXT("Speed"), false);
	FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Inner.Get(), TEXT("Tint"), false);
	FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Inner.Get(), TEXT("HiddenGain"), true);
	FKismetEditorUtilities::CompileBlueprint(Inner.Get());
	UClass* InnerClass = Inner->GeneratedClass;
	if (!TestNotNull(TEXT("inner generated class"), InnerClass))
	{
		RetireBlueprint(Inner);
		return false;
	}

	TStrongObjectPtr<UWidgetBlueprint> Outer =
		CreateEmbeddedValueBlueprint(UUserWidget::StaticClass(), TEXT("CortexEmbeddedOuter_"));
	UCanvasPanel* Root = Cast<UCanvasPanel>(Outer->WidgetTree->RootWidget);
	UUserWidget* InstanceA = Outer->WidgetTree->ConstructWidget<UUserWidget>(InnerClass, TEXT("InnerA"));
	UUserWidget* InstanceB = Outer->WidgetTree->ConstructWidget<UUserWidget>(InnerClass, TEXT("InnerB"));
	UTextBlock* Label = Outer->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Label"));
	Root->AddChild(InstanceB);
	Root->AddChild(InstanceA);
	Root->AddChild(Label);

	FProperty* SpeedProperty = InnerClass->FindPropertyByName(TEXT("Speed"));
	if (!TestNotNull(TEXT("Speed property"), SpeedProperty))
	{
		RetireBlueprint(Outer);
		RetireBlueprint(Inner);
		return false;
	}
	*SpeedProperty->ContainerPtrToValuePtr<float>(InstanceA) = 3.0f;

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.1"),
		MakeShared<FCortexUMGCommandHandler>());

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), Outer->GetPathName());
	const FCortexCommandResult All = Router.Execute(TEXT("umg.get_embedded_widget_values"), Params);
	if (TestTrue(TEXT("get_embedded_widget_values succeeds"), All.bSuccess && All.Data.IsValid()))
	{
		const TArray<TSharedPtr<FJsonValue>>& Widgets = All.Data->GetArrayField(TEXT("widgets"));
		TestEqual(TEXT("only the two embedded user widgets are reported"), Widgets.Num(), 2);
		if (Widgets.Num() == 2)
		{
			TestEqual(TEXT("widgets sorted by name (first)"),
				Widgets[0]->AsObject()->GetStringField(TEXT("widget_name")), FString(TEXT("InnerA")));
			TestEqual(TEXT("widgets sorted by name (second)"),
				Widgets[1]->AsObject()->GetStringField(TEXT("widget_name")), FString(TEXT("InnerB")));
		}

		const TSharedPtr<FJsonObject> RecordA = FindWidgetRecord(All, TEXT("InnerA"));
		const TSharedPtr<FJsonObject> RecordB = FindWidgetRecord(All, TEXT("InnerB"));
		if (TestTrue(TEXT("both records found"), RecordA.IsValid() && RecordB.IsValid()))
		{
			TestEqual(TEXT("properties sorted and limited to instance-editable declared members"),
				PropertyNames(RecordA), FString(TEXT("NativeGain,Speed,Tint")));
			TestEqual(TEXT("second instance lists the same properties"),
				PropertyNames(RecordB), PropertyNames(RecordA));

			TestNull(TEXT("DisableEditOnInstance Blueprint variable is absent"), FindProperty(RecordA, TEXT("HiddenGain")).Get());
			TestNull(TEXT("EditDefaultsOnly native property is absent"), FindProperty(RecordA, TEXT("NativeDefaultsOnly")).Get());
			const TArray<FString> EngineProperties = { TEXT("Padding"), TEXT("bIsEnabled"), TEXT("Visibility"), TEXT("ColorAndOpacity") };
			for (const FString& EngineProperty : EngineProperties)
			{
				TestNull(*FString::Printf(TEXT("engine property %s is absent"), *EngineProperty),
					FindProperty(RecordA, EngineProperty).Get());
			}

			const TSharedPtr<FJsonObject> SpeedA = FindProperty(RecordA, TEXT("Speed"));
			const TSharedPtr<FJsonObject> SpeedB = FindProperty(RecordB, TEXT("Speed"));
			if (TestTrue(TEXT("Speed listed on both"), SpeedA.IsValid() && SpeedB.IsValid()))
			{
				TestTrue(TEXT("overridden Speed is_override"), SpeedA->GetBoolField(TEXT("is_override")));
				TestFalse(TEXT("default Speed is not an override"), SpeedB->GetBoolField(TEXT("is_override")));
				TestEqual(TEXT("overridden Speed value"), SpeedA->GetNumberField(TEXT("value")), 3.0);
				TestEqual(TEXT("default Speed value"), SpeedB->GetNumberField(TEXT("value")), 1.0);
				TestEqual(TEXT("Speed declared_in the inner generated class"),
					SpeedA->GetStringField(TEXT("declared_in")), InnerClass->GetPathName());
				TestTrue(TEXT("Speed declares a type"), SpeedA->HasTypedField<EJson::String>(TEXT("type")));
			}

			const TSharedPtr<FJsonObject> Tint = FindProperty(RecordA, TEXT("Tint"));
			if (TestTrue(TEXT("Tint listed"), Tint.IsValid()))
			{
				TestFalse(TEXT("untouched Tint is not an override"), Tint->GetBoolField(TEXT("is_override")));
				TestTrue(TEXT("Tint serializes as an object"), Tint->HasTypedField<EJson::Object>(TEXT("value")));
			}

			const TSharedPtr<FJsonObject> Gain = FindProperty(RecordA, TEXT("NativeGain"));
			if (TestTrue(TEXT("native-declared NativeGain is listed"), Gain.IsValid()))
			{
				TestEqual(TEXT("NativeGain declared_in the native class"), Gain->GetStringField(TEXT("declared_in")),
					UCortexUMGEmbeddedValueTestWidget::StaticClass()->GetPathName());
				TestFalse(TEXT("native default is not an override"), Gain->GetBoolField(TEXT("is_override")));
				TestEqual(TEXT("native default value"), Gain->GetNumberField(TEXT("value")), 2.0);
			}
		}

		const FCortexCommandResult Again = Router.Execute(TEXT("umg.get_embedded_widget_values"), Params);
		TestTrue(TEXT("second read succeeds"), Again.bSuccess && Again.Data.IsValid());
		if (Again.bSuccess && Again.Data.IsValid())
		{
			TestEqual(TEXT("repeat read is identical"),
				FString::FromInt(Again.Data->GetArrayField(TEXT("widgets")).Num()), FString(TEXT("2")));
			TestEqual(TEXT("repeat read lists the same properties"),
				PropertyNames(FindWidgetRecord(Again, TEXT("InnerA"))), PropertyNames(FindWidgetRecord(All, TEXT("InnerA"))));
		}
	}

	TSharedPtr<FJsonObject> OneParams = MakeShared<FJsonObject>();
	OneParams->SetStringField(TEXT("asset_path"), Outer->GetPathName());
	OneParams->SetStringField(TEXT("widget_name"), TEXT("InnerB"));
	const FCortexCommandResult One = Router.Execute(TEXT("umg.get_embedded_widget_values"), OneParams);
	if (TestTrue(TEXT("widget_name filtered read succeeds"), One.bSuccess && One.Data.IsValid()))
	{
		const TArray<TSharedPtr<FJsonValue>>& Widgets = One.Data->GetArrayField(TEXT("widgets"));
		TestEqual(TEXT("widget_name filter returns exactly one widget"), Widgets.Num(), 1);
		if (Widgets.Num() == 1)
		{
			TestEqual(TEXT("filtered widget is InnerB"),
				Widgets[0]->AsObject()->GetStringField(TEXT("widget_name")), FString(TEXT("InnerB")));
		}
	}

	OneParams->SetStringField(TEXT("widget_name"), TEXT("NoSuchWidget"));
	const FCortexCommandResult Unknown = Router.Execute(TEXT("umg.get_embedded_widget_values"), OneParams);
	TestFalse(TEXT("unknown widget fails"), Unknown.bSuccess);
	TestEqual(TEXT("unknown widget error code"), Unknown.ErrorCode, CortexErrorCodes::WidgetNotFound);

	OneParams->SetStringField(TEXT("widget_name"), TEXT("Label"));
	const FCortexCommandResult NotUserWidget = Router.Execute(TEXT("umg.get_embedded_widget_values"), OneParams);
	TestFalse(TEXT("non-UserWidget target fails"), NotUserWidget.bSuccess);
	TestEqual(TEXT("non-UserWidget error code"), NotUserWidget.ErrorCode, CortexErrorCodes::TypeMismatch);

	RetireBlueprint(Outer);
	RetireBlueprint(Inner);
	return true;
}
