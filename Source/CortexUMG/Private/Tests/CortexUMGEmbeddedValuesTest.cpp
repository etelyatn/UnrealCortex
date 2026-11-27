// umg.get_embedded_widget_values exports, per embedded UserWidget instance, every editable property
// declared below UUserWidget (Blueprint variables and native UPROPERTYs alike, defaults-only ones flagged
// instance_editable=false) with its value and whether the instance overrides the class default.
// umg.get_embedded_widget_diff lists only the properties whose template value differs from the class default.

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

	// Inner: native test parent plus Blueprint variables Speed (instance editable, default 1.0), Tint
	// (instance editable) and HiddenGain (defaults-only, default 9.0). Outer embeds InnerA (Speed 3.0),
	// InnerB and a plain TextBlock Label.
	struct FEmbeddedFixture
	{
		TStrongObjectPtr<UWidgetBlueprint> Inner;
		TStrongObjectPtr<UWidgetBlueprint> Outer;
		UClass* InnerClass = nullptr;
		UUserWidget* InstanceA = nullptr;
		UUserWidget* InstanceB = nullptr;

		~FEmbeddedFixture()
		{
			RetireBlueprint(Outer);
			RetireBlueprint(Inner);
		}
	};

	bool BuildEmbeddedFixture(FAutomationTestBase& Test, FEmbeddedFixture& Fixture)
	{
		Fixture.Inner = CreateEmbeddedValueBlueprint(
			UCortexUMGEmbeddedValueTestWidget::StaticClass(), TEXT("CortexEmbeddedInner_"));

		FEdGraphPinType FloatType;
		FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
		FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
		FEdGraphPinType ColorType;
		ColorType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		ColorType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
		const bool bVariablesAdded =
			FBlueprintEditorUtils::AddMemberVariable(Fixture.Inner.Get(), TEXT("Speed"), FloatType, TEXT("1.0")) &&
			FBlueprintEditorUtils::AddMemberVariable(Fixture.Inner.Get(), TEXT("Tint"), ColorType) &&
			FBlueprintEditorUtils::AddMemberVariable(Fixture.Inner.Get(), TEXT("HiddenGain"), FloatType, TEXT("9.0"));
		if (!Test.TestTrue(TEXT("inner variables added"), bVariablesAdded))
		{
			return false;
		}
		FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Fixture.Inner.Get(), TEXT("Speed"), false);
		FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Fixture.Inner.Get(), TEXT("Tint"), false);
		FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Fixture.Inner.Get(), TEXT("HiddenGain"), true);
		FKismetEditorUtilities::CompileBlueprint(Fixture.Inner.Get());
		Fixture.InnerClass = Fixture.Inner->GeneratedClass;
		if (!Test.TestNotNull(TEXT("inner generated class"), Fixture.InnerClass))
		{
			return false;
		}

		Fixture.Outer = CreateEmbeddedValueBlueprint(UUserWidget::StaticClass(), TEXT("CortexEmbeddedOuter_"));
		UCanvasPanel* Root = Cast<UCanvasPanel>(Fixture.Outer->WidgetTree->RootWidget);
		Fixture.InstanceA = Fixture.Outer->WidgetTree->ConstructWidget<UUserWidget>(Fixture.InnerClass, TEXT("InnerA"));
		Fixture.InstanceB = Fixture.Outer->WidgetTree->ConstructWidget<UUserWidget>(Fixture.InnerClass, TEXT("InnerB"));
		UTextBlock* Label = Fixture.Outer->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Label"));
		Root->AddChild(Fixture.InstanceB);
		Root->AddChild(Fixture.InstanceA);
		Root->AddChild(Label);

		FProperty* SpeedProperty = Fixture.InnerClass->FindPropertyByName(TEXT("Speed"));
		if (!Test.TestNotNull(TEXT("Speed property"), SpeedProperty))
		{
			return false;
		}
		*SpeedProperty->ContainerPtrToValuePtr<float>(Fixture.InstanceA) = 3.0f;
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGEmbeddedWidgetValuesTest,
	"Cortex.UMG.EmbeddedWidgetValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGEmbeddedWidgetValuesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FEmbeddedFixture Fixture;
	if (!BuildEmbeddedFixture(*this, Fixture))
	{
		return false;
	}
	TStrongObjectPtr<UWidgetBlueprint>& Inner = Fixture.Inner;
	TStrongObjectPtr<UWidgetBlueprint>& Outer = Fixture.Outer;
	UClass* InnerClass = Fixture.InnerClass;

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
			TestEqual(TEXT("properties sorted: every editable declared member, defaults-only included"),
				PropertyNames(RecordA), FString(TEXT("HiddenGain,NativeDefaultsOnly,NativeGain,Speed,Tint")));
			TestEqual(TEXT("second instance lists the same properties"),
				PropertyNames(RecordB), PropertyNames(RecordA));

			const TArray<FString> DefaultsOnly = { TEXT("HiddenGain"), TEXT("NativeDefaultsOnly") };
			for (const FString& Name : DefaultsOnly)
			{
				const TSharedPtr<FJsonObject> Property = FindProperty(RecordA, Name);
				if (TestTrue(*FString::Printf(TEXT("defaults-only %s is listed"), *Name), Property.IsValid()))
				{
					TestFalse(*FString::Printf(TEXT("defaults-only %s is not instance editable"), *Name),
						Property->GetBoolField(TEXT("instance_editable")));
					TestFalse(*FString::Printf(TEXT("untouched defaults-only %s is not an override"), *Name),
						Property->GetBoolField(TEXT("is_override")));
				}
			}
			const TSharedPtr<FJsonObject> HiddenGain = FindProperty(RecordA, TEXT("HiddenGain"));
			if (HiddenGain.IsValid())
			{
				TestEqual(TEXT("HiddenGain value"), HiddenGain->GetNumberField(TEXT("value")), 9.0);
			}
			const TSharedPtr<FJsonObject> NativeDefaultsOnly = FindProperty(RecordA, TEXT("NativeDefaultsOnly"));
			if (NativeDefaultsOnly.IsValid())
			{
				TestEqual(TEXT("NativeDefaultsOnly value"), NativeDefaultsOnly->GetNumberField(TEXT("value")), 5.0);
			}
			for (const FString& Name : { FString(TEXT("Speed")), FString(TEXT("Tint")), FString(TEXT("NativeGain")) })
			{
				const TSharedPtr<FJsonObject> Property = FindProperty(RecordA, Name);
				if (TestTrue(*FString::Printf(TEXT("%s is listed"), *Name), Property.IsValid()))
				{
					TestTrue(*FString::Printf(TEXT("%s is instance editable"), *Name),
						Property->GetBoolField(TEXT("instance_editable")));
				}
			}

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexUMGEmbeddedWidgetDiffTest,
	"Cortex.UMG.EmbeddedWidgetDiff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexUMGEmbeddedWidgetDiffTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FEmbeddedFixture Fixture;
	if (!BuildEmbeddedFixture(*this, Fixture))
	{
		return false;
	}

	// A defaults-only native property and a defaults-only Blueprint variable are overridden on templates.
	FProperty* NativeDefaultsOnly = Fixture.InnerClass->FindPropertyByName(TEXT("NativeDefaultsOnly"));
	FProperty* HiddenGain = Fixture.InnerClass->FindPropertyByName(TEXT("HiddenGain"));
	if (!TestTrue(TEXT("defaults-only properties found"), NativeDefaultsOnly && HiddenGain))
	{
		return false;
	}
	*NativeDefaultsOnly->ContainerPtrToValuePtr<int32>(Fixture.InstanceA) = 7;
	*HiddenGain->ContainerPtrToValuePtr<float>(Fixture.InstanceB) = 10.0f;

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("umg"), TEXT("Cortex UMG"), TEXT("1.0.1"),
		MakeShared<FCortexUMGCommandHandler>());

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), Fixture.Outer->GetPathName());
	const FCortexCommandResult Diff = Router.Execute(TEXT("umg.get_embedded_widget_diff"), Params);
	if (!TestTrue(TEXT("get_embedded_widget_diff succeeds"), Diff.bSuccess && Diff.Data.IsValid()))
	{
		return false;
	}

	TestEqual(TEXT("only the two embedded user widgets are reported"), Diff.Data->GetIntegerField(TEXT("widget_count")), 2);
	TestEqual(TEXT("total difference count"), Diff.Data->GetIntegerField(TEXT("difference_count")), 3);

	auto FindDifferences = [](const FCortexCommandResult& Result, const FString& WidgetName)
		-> const TArray<TSharedPtr<FJsonValue>>*
	{
		const TSharedPtr<FJsonObject> Record = FindWidgetRecord(Result, WidgetName);
		return Record.IsValid() ? &Record->GetArrayField(TEXT("differences")) : nullptr;
	};
	auto FindDifference = [&FindDifferences](const FCortexCommandResult& Result, const FString& WidgetName,
		const FString& PropertyName) -> TSharedPtr<FJsonObject>
	{
		if (const TArray<TSharedPtr<FJsonValue>>* Differences = FindDifferences(Result, WidgetName))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Differences)
			{
				if (Value->AsObject()->GetStringField(TEXT("property")) == PropertyName)
				{
					return Value->AsObject();
				}
			}
		}
		return nullptr;
	};

	const TArray<TSharedPtr<FJsonValue>>* DifferencesA = FindDifferences(Diff, TEXT("InnerA"));
	const TArray<TSharedPtr<FJsonValue>>* DifferencesB = FindDifferences(Diff, TEXT("InnerB"));
	if (!TestTrue(TEXT("both embedded widgets reported"), DifferencesA && DifferencesB))
	{
		return false;
	}
	TestEqual(TEXT("InnerA lists exactly its two differing properties"), DifferencesA->Num(), 2);
	TestEqual(TEXT("InnerB lists exactly its one differing property"), DifferencesB->Num(), 1);
	TestNull(TEXT("plain TextBlock is not reported"), FindWidgetRecord(Diff, TEXT("Label")).Get());
	TestNull(TEXT("unchanged Tint is not reported"), FindDifference(Diff, TEXT("InnerA"), TEXT("Tint")).Get());
	TestNull(TEXT("unchanged NativeGain is not reported"), FindDifference(Diff, TEXT("InnerA"), TEXT("NativeGain")).Get());
	TestNull(TEXT("unchanged Speed on InnerB is not reported"), FindDifference(Diff, TEXT("InnerB"), TEXT("Speed")).Get());

	const TSharedPtr<FJsonObject> SpeedDiff = FindDifference(Diff, TEXT("InnerA"), TEXT("Speed"));
	if (TestTrue(TEXT("Speed difference listed"), SpeedDiff.IsValid()))
	{
		TestEqual(TEXT("Speed instance_value"), SpeedDiff->GetNumberField(TEXT("instance_value")), 3.0);
		TestEqual(TEXT("Speed class_default_value"), SpeedDiff->GetNumberField(TEXT("class_default_value")), 1.0);
		TestTrue(TEXT("Speed is instance editable"), SpeedDiff->GetBoolField(TEXT("instance_editable")));
		TestEqual(TEXT("Speed declared_in the inner generated class"),
			SpeedDiff->GetStringField(TEXT("declared_in")), Fixture.InnerClass->GetPathName());
		TestTrue(TEXT("Speed declares a type"), SpeedDiff->HasTypedField<EJson::String>(TEXT("type")));
	}

	const TSharedPtr<FJsonObject> NativeDiff = FindDifference(Diff, TEXT("InnerA"), TEXT("NativeDefaultsOnly"));
	if (TestTrue(TEXT("defaults-only native property difference listed"), NativeDiff.IsValid()))
	{
		TestEqual(TEXT("NativeDefaultsOnly instance_value"), NativeDiff->GetNumberField(TEXT("instance_value")), 7.0);
		TestEqual(TEXT("NativeDefaultsOnly class_default_value"),
			NativeDiff->GetNumberField(TEXT("class_default_value")), 5.0);
		TestFalse(TEXT("NativeDefaultsOnly is not instance editable"), NativeDiff->GetBoolField(TEXT("instance_editable")));
		TestEqual(TEXT("NativeDefaultsOnly declared_in the native class"),
			NativeDiff->GetStringField(TEXT("declared_in")),
			UCortexUMGEmbeddedValueTestWidget::StaticClass()->GetPathName());
	}

	const TSharedPtr<FJsonObject> HiddenDiff = FindDifference(Diff, TEXT("InnerB"), TEXT("HiddenGain"));
	if (TestTrue(TEXT("defaults-only Blueprint variable difference listed"), HiddenDiff.IsValid()))
	{
		TestEqual(TEXT("HiddenGain instance_value"), HiddenDiff->GetNumberField(TEXT("instance_value")), 10.0);
		TestEqual(TEXT("HiddenGain class_default_value"), HiddenDiff->GetNumberField(TEXT("class_default_value")), 9.0);
		TestFalse(TEXT("HiddenGain is not instance editable"), HiddenDiff->GetBoolField(TEXT("instance_editable")));
	}

	// An outer Widget Blueprint with no embedded widgets reports an empty diff.
	TStrongObjectPtr<UWidgetBlueprint> Plain =
		CreateEmbeddedValueBlueprint(UUserWidget::StaticClass(), TEXT("CortexEmbeddedPlain_"));
	Params->SetStringField(TEXT("asset_path"), Plain->GetPathName());
	const FCortexCommandResult Empty = Router.Execute(TEXT("umg.get_embedded_widget_diff"), Params);
	if (TestTrue(TEXT("diff of a blueprint without embedded widgets succeeds"), Empty.bSuccess && Empty.Data.IsValid()))
	{
		TestEqual(TEXT("no embedded widgets"), Empty.Data->GetIntegerField(TEXT("widget_count")), 0);
		TestEqual(TEXT("no differences"), Empty.Data->GetIntegerField(TEXT("difference_count")), 0);
	}
	RetireBlueprint(Plain);

	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Cortex/DoesNotExist/WBP_NoSuchEmbeddedOuter"));
	const FCortexCommandResult Missing = Router.Execute(TEXT("umg.get_embedded_widget_diff"), Params);
	TestFalse(TEXT("unknown asset fails"), Missing.bSuccess);
	return true;
}
