#include "Misc/AutomationTest.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "CortexAdoptTestHelpers.h"
#include "CortexBPCommandHandler.h"
#include "CortexBPEmbeddedTestTypes.h"
#include "CortexBPReparentTestSupport.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Operations/CortexBPReparentOps.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
	/** An inner Widget Blueprint embedded twice in an outer one. */
	struct FEmbeddedFixture
	{
		UBlueprint* Inner = nullptr;
		UBlueprint* Outer = nullptr;
		UWidgetTree* OuterTree = nullptr;

		bool IsValid() const { return Inner != nullptr && Outer != nullptr && OuterTree != nullptr; }

		UUserWidget* Instance(const TCHAR* Name) const
		{
			return Cast<UUserWidget>(OuterTree->FindWidget(FName(Name)));
		}
	};

	UBlueprint* MakeWidgetBlueprint(const TCHAR* Name, UClass* Parent)
	{
		UClass* WidgetBlueprintClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.WidgetBlueprint"));
		return WidgetBlueprintClass != nullptr ? CortexAdoptTest::MakeBlueprint(Name, Parent, nullptr, WidgetBlueprintClass) : nullptr;
	}

	UWidgetTree* WidgetTreeOf(UBlueprint* WidgetBlueprint)
	{
		const FObjectProperty* Property = CastField<FObjectProperty>(WidgetBlueprint->GetClass()->FindPropertyByName(TEXT("WidgetTree")));
		return Property != nullptr ? Cast<UWidgetTree>(Property->GetObjectPropertyValue_InContainer(WidgetBlueprint)) : nullptr;
	}

	/** Inner (parent InnerParent) embedded as EmbA and EmbB in Outer, both compiled. */
	FEmbeddedFixture MakeFixture(UClass* InnerParent)
	{
		FEmbeddedFixture Fixture;
		Fixture.Inner = MakeWidgetBlueprint(TEXT("WBP_EmbInner"), InnerParent);
		Fixture.Outer = MakeWidgetBlueprint(TEXT("WBP_EmbOuter"), UUserWidget::StaticClass());
		Fixture.OuterTree = Fixture.Outer != nullptr ? WidgetTreeOf(Fixture.Outer) : nullptr;
		if (!Fixture.IsValid() || Fixture.Inner->GeneratedClass == nullptr)
		{
			return FEmbeddedFixture();
		}
		UCanvasPanel* Root = Fixture.OuterTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
		Fixture.OuterTree->RootWidget = Root;
		UClass* InnerClass = Fixture.Inner->GeneratedClass;
		for (const TCHAR* Name : {TEXT("EmbA"), TEXT("EmbB")})
		{
			Root->AddChild(Fixture.OuterTree->ConstructWidget<UUserWidget>(InnerClass, FName(Name)));
		}
		FKismetEditorUtilities::CompileBlueprint(Fixture.Outer);
		return Fixture;
	}

	void DiscardFixture(const FEmbeddedFixture& Fixture)
	{
		CortexAdoptTest::Discard(Fixture.Outer);
		CortexAdoptTest::Discard(Fixture.Inner);
	}

	FString StringValue(const UObject* Object, const TCHAR* Property)
	{
		const FStrProperty* StrProperty = CastField<FStrProperty>(Object->GetClass()->FindPropertyByName(Property));
		return StrProperty != nullptr ? StrProperty->GetPropertyValue_InContainer(Object) : FString(TEXT("<missing>"));
	}

	int32 IntValue(const UObject* Object, const TCHAR* Property)
	{
		const FIntProperty* IntProperty = CastField<FIntProperty>(Object->GetClass()->FindPropertyByName(Property));
		return IntProperty != nullptr ? IntProperty->GetPropertyValue_InContainer(Object) : INDEX_NONE;
	}

	void SetInt(UObject* Object, const TCHAR* Property, int32 Value)
	{
		if (const FIntProperty* IntProperty = CastField<FIntProperty>(Object->GetClass()->FindPropertyByName(Property)))
		{
			IntProperty->SetPropertyValue_InContainer(Object, Value);
		}
	}

	TSharedPtr<FJsonObject> FindByField(const TSharedPtr<FJsonObject>& Data, const TCHAR* Array, const TCHAR* Field, const FString& Value)
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (Data.IsValid() && Data->TryGetArrayField(Array, Items) && Items != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Item : *Items)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				FString Found;
				if (Item->TryGetObject(Object) && Object != nullptr && (*Object)->TryGetStringField(Field, Found) && Found == Value)
				{
					return *Object;
				}
			}
		}
		return nullptr;
	}

	TArray<FString> PropertyNames(const TSharedPtr<FJsonObject>& Entry)
	{
		TArray<FString> Names;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Entry.IsValid() && Entry->TryGetArrayField(TEXT("properties"), Values) && Values != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				Names.Add(Value->AsString());
			}
		}
		return Names;
	}

	bool ReadBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		bool bValue = false;
		return Object.IsValid() && Object->TryGetBoolField(Field, bValue) && bValue;
	}

	/** Label and Gain change; OldOnly changes from its zero default (a default that moves away from zero must be seen too). */
	FCortexCommandResult SetDefaults(const UBlueprint* Inner, bool bResync, bool bCompile)
	{
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetStringField(TEXT("Label"), TEXT("new"));
		Properties->SetNumberField(TEXT("Gain"), 5);
		Properties->SetNumberField(TEXT("OldOnly"), 4);
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Inner->GetPathName());
		Params->SetObjectField(TEXT("properties"), Properties);
		Params->SetBoolField(TEXT("compile"), bCompile);
		Params->SetBoolField(TEXT("save"), false);
		if (bResync)
		{
			Params->SetBoolField(TEXT("resync_embedded"), true);
		}
		FCortexBPCommandHandler Handler;
		return Handler.Execute(TEXT("set_class_defaults"), Params);
	}

	void SetString(UObject* Object, const TCHAR* Property, const FString& Value)
	{
		if (const FStrProperty* StrProperty = CastField<FStrProperty>(Object->GetClass()->FindPropertyByName(Property)))
		{
			StrProperty->SetPropertyValue_InContainer(Object, Value);
		}
	}

	struct FPostRunHookScope
	{
		explicit FPostRunHookScope(TFunction<void(UBlueprint*)> Hook)
		{
			FCortexBPReparentOps::SetPostRunHookForTest(MoveTemp(Hook));
		}
		~FPostRunHookScope()
		{
			FCortexBPReparentOps::ClearPostRunHookForTest();
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPEmbeddedSetClassDefaultsReportsTest,
	"Cortex.Blueprint.EmbeddedTemplates.SetClassDefaultsReports",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPEmbeddedSetClassDefaultsReportsTest::RunTest(const FString& Parameters)
{
	const FEmbeddedFixture Fixture = MakeFixture(UCortexBPEmbeddedWidgetBase::StaticClass());
	if (!TestTrue(TEXT("Two-Widget-Blueprint fixture"), Fixture.IsValid()))
	{
		DiscardFixture(Fixture);
		return false;
	}
	UUserWidget* A = Fixture.Instance(TEXT("EmbA"));
	UUserWidget* B = Fixture.Instance(TEXT("EmbB"));
	if (!TestNotNull(TEXT("EmbA"), A) || !TestNotNull(TEXT("EmbB"), B))
	{
		DiscardFixture(Fixture);
		return false;
	}
	TestEqual(TEXT("The template starts with the native default"), StringValue(A, TEXT("Label")), FString(TEXT("native")));
	SetInt(B, TEXT("Gain"), 99); // an intentional override on one template

	const FCortexCommandResult Result = SetDefaults(Fixture.Inner, /*bResync=*/false, /*bCompile=*/false);
	if (TestTrue(TEXT("set_class_defaults succeeds"), Result.bSuccess) && Result.Data.IsValid())
	{
		const TSharedPtr<FJsonObject> EntryA = FindByField(Result.Data, TEXT("embedded_instances"), TEXT("widget_name"), TEXT("EmbA"));
		const TSharedPtr<FJsonObject> EntryB = FindByField(Result.Data, TEXT("embedded_instances"), TEXT("widget_name"), TEXT("EmbB"));
		if (TestNotNull(TEXT("EmbA is reported"), EntryA.Get()))
		{
			TestEqual(TEXT("asset_path"), EntryA->GetStringField(TEXT("asset_path")), Fixture.Outer->GetPathName());
			TestTrue(TEXT("Every default is stale on EmbA: defaults-only Label, instance-editable Gain, and OldOnly that moved away from zero"),
				PropertyNames(EntryA) == TArray<FString>({TEXT("Gain"), TEXT("Label"), TEXT("OldOnly")}));
			TestFalse(TEXT("Not resynced without the opt-in"), ReadBool(EntryA, TEXT("resynced")));
		}
		if (TestNotNull(TEXT("EmbB is reported"), EntryB.Get()))
		{
			TestTrue(TEXT("Label and OldOnly are stale on EmbB; its Gain override is not"), PropertyNames(EntryB) == TArray<FString>({TEXT("Label"), TEXT("OldOnly")}));
		}
	}
	TestEqual(TEXT("Without the opt-in the template keeps the old default"), StringValue(A, TEXT("Label")), FString(TEXT("native")));
	TestEqual(TEXT("Gain untouched"), IntValue(A, TEXT("Gain")), 1);
	TestEqual(TEXT("Override untouched"), IntValue(B, TEXT("Gain")), 99);

	// A Blueprint that is not a Widget Blueprint reports nothing.
	UBlueprint* Actor = CortexAdoptTest::MakeBlueprint(TEXT("EmbeddedPlainActor"), AActor::StaticClass());
	if (TestNotNull(TEXT("Actor Blueprint"), Actor))
	{
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetNumberField(TEXT("InitialLifeSpan"), 3);
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Actor->GetPathName());
		Params->SetObjectField(TEXT("properties"), Properties);
		Params->SetBoolField(TEXT("compile"), false);
		Params->SetBoolField(TEXT("save"), false);
		FCortexBPCommandHandler Handler;
		const FCortexCommandResult ActorResult = Handler.Execute(TEXT("set_class_defaults"), Params);
		if (TestTrue(TEXT("Actor set_class_defaults succeeds"), ActorResult.bSuccess) && ActorResult.Data.IsValid())
		{
			TestFalse(TEXT("No embedded_instances for a non-widget Blueprint"), ActorResult.Data->HasField(TEXT("embedded_instances")));
		}
		CortexAdoptTest::Discard(Actor);
	}

	DiscardFixture(Fixture);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPEmbeddedSetClassDefaultsResyncTest,
	"Cortex.Blueprint.EmbeddedTemplates.SetClassDefaultsResync",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPEmbeddedSetClassDefaultsResyncTest::RunTest(const FString& Parameters)
{
	const FEmbeddedFixture Fixture = MakeFixture(UCortexBPEmbeddedWidgetBase::StaticClass());
	if (!TestTrue(TEXT("Two-Widget-Blueprint fixture"), Fixture.IsValid()))
	{
		DiscardFixture(Fixture);
		return false;
	}
	UUserWidget* A = Fixture.Instance(TEXT("EmbA"));
	UUserWidget* B = Fixture.Instance(TEXT("EmbB"));
	if (!TestNotNull(TEXT("EmbA"), A) || !TestNotNull(TEXT("EmbB"), B))
	{
		DiscardFixture(Fixture);
		return false;
	}
	SetInt(B, TEXT("Gain"), 99);
	Fixture.Outer->GetOutermost()->ClearDirtyFlag();

	const FCortexCommandResult Result = SetDefaults(Fixture.Inner, /*bResync=*/true, /*bCompile=*/false);
	if (TestTrue(TEXT("set_class_defaults succeeds"), Result.bSuccess) && Result.Data.IsValid())
	{
		const TSharedPtr<FJsonObject> EntryA = FindByField(Result.Data, TEXT("embedded_instances"), TEXT("widget_name"), TEXT("EmbA"));
		const TSharedPtr<FJsonObject> EntryB = FindByField(Result.Data, TEXT("embedded_instances"), TEXT("widget_name"), TEXT("EmbB"));
		TestTrue(TEXT("EmbA resynced"), ReadBool(EntryA, TEXT("resynced")));
		TestTrue(TEXT("EmbB resynced"), ReadBool(EntryB, TEXT("resynced")));
	}

	// The engine details-panel rule: a value equal to the old default follows the new default, an override never does.
	TestEqual(TEXT("EmbA Label follows the new default"), StringValue(A, TEXT("Label")), FString(TEXT("new")));
	TestEqual(TEXT("EmbA Gain follows the new default"), IntValue(A, TEXT("Gain")), 5);
	TestEqual(TEXT("EmbB Label follows the new default"), StringValue(B, TEXT("Label")), FString(TEXT("new")));
	TestEqual(TEXT("EmbB's intentional Gain override survives"), IntValue(B, TEXT("Gain")), 99);
	TestEqual(TEXT("EmbA OldOnly follows the new default (moved away from zero)"), IntValue(A, TEXT("OldOnly")), 4);
	TestTrue(TEXT("The outer Widget Blueprint is marked modified"), Fixture.Outer->GetOutermost()->IsDirty());

	// A second call finds nothing left to resync.
	const FCortexCommandResult Again = SetDefaults(Fixture.Inner, /*bResync=*/true, /*bCompile=*/false);
	if (TestTrue(TEXT("Second set_class_defaults succeeds"), Again.bSuccess) && Again.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		TestTrue(TEXT("embedded_instances is reported"), Again.Data->TryGetArrayField(TEXT("embedded_instances"), Items) && Items != nullptr);
		TestEqual(TEXT("Nothing is stale any more"), Items != nullptr ? Items->Num() : -1, 0);
	}

	DiscardFixture(Fixture);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPEmbeddedReparentDroppedOverridesTest,
	"Cortex.Blueprint.EmbeddedTemplates.ReparentDroppedOverrides",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPEmbeddedReparentDroppedOverridesTest::RunTest(const FString& Parameters)
{
	using namespace CortexReparentTest;

	const FEmbeddedFixture Fixture = MakeFixture(UCortexBPEmbeddedWidgetBase::StaticClass());
	if (!TestTrue(TEXT("Two-Widget-Blueprint fixture"), Fixture.IsValid()))
	{
		DiscardFixture(Fixture);
		return false;
	}
	UUserWidget* A = Fixture.Instance(TEXT("EmbA"));
	if (!TestNotNull(TEXT("EmbA"), A))
	{
		DiscardFixture(Fixture);
		return false;
	}
	// EmbA overrides a property the new parent does not declare (its class default is zero); EmbB keeps every default.
	SetInt(A, TEXT("OldOnly"), 7);

	TSharedPtr<FJsonObject> Params = ReparentParams(Fixture.Inner, UCortexBPEmbeddedWidgetOther::StaticClass()->GetPathName());
	Params->SetBoolField(TEXT("resync_embedded"), true);
	FCortexCommandResult Result;
	{
		// A delta-reinstancing compile already moves untouched templates to the new default, so a stale template is simulated: EmbA's Label is
		// put back to the old class default after the compile, which the report must find and the resync must repair.
		FPostRunHookScope Hook([&Fixture](UBlueprint*)
		{
			if (UUserWidget* Instance = Fixture.Instance(TEXT("EmbA")))
			{
				SetString(Instance, TEXT("Label"), TEXT("native"));
			}
		});
		Result = FCortexBPReparentOps::Execute(Params);
	}
	if (TestTrue(TEXT("Reparent succeeds"), Result.bSuccess) && Result.Data.IsValid())
	{
		const TSharedPtr<FJsonObject> Dropped = FindByField(Result.Data, TEXT("dropped_overrides"), TEXT("widget_name"), TEXT("EmbA"));
		if (TestNotNull(TEXT("EmbA's override of the dropped property is reported"), Dropped.Get()))
		{
			TestEqual(TEXT("asset_path"), Dropped->GetStringField(TEXT("asset_path")), Fixture.Outer->GetPathName());
			TestEqual(TEXT("property"), Dropped->GetStringField(TEXT("property")), FString(TEXT("OldOnly")));
			TestEqual(TEXT("instance_value"), Dropped->GetStringField(TEXT("instance_value")), FString(TEXT("7")));
		}
		TestNull(TEXT("EmbB has no override to drop"), FindByField(Result.Data, TEXT("dropped_overrides"), TEXT("widget_name"), TEXT("EmbB")).Get());

		const TSharedPtr<FJsonObject> Stale = FindByField(Result.Data, TEXT("embedded_instances"), TEXT("widget_name"), TEXT("EmbA"));
		if (TestNotNull(TEXT("The template left behind is reported in embedded_instances"), Stale.Get()))
		{
			TestTrue(TEXT("Label is listed"), PropertyNames(Stale).Contains(TEXT("Label")));
			TestTrue(TEXT("The opt-in resynced it"), ReadBool(Stale, TEXT("resynced")));
		}
	}
	TestTrue(TEXT("The Blueprint is reparented"), Fixture.Inner->ParentClass == UCortexBPEmbeddedWidgetOther::StaticClass());

	const UObject* Cdo = Fixture.Inner->GeneratedClass->GetDefaultObject();
	const FString ClassDefault = StringValue(Cdo, TEXT("Label"));
	TestNotEqual(TEXT("The new class default is not the old one"), ClassDefault, FString(TEXT("native")));
	for (const TCHAR* Name : {TEXT("EmbA"), TEXT("EmbB")})
	{
		if (const UUserWidget* Instance = Fixture.Instance(Name))
		{
			TestEqual(*FString::Printf(TEXT("%s Label equals the class default"), Name), StringValue(Instance, TEXT("Label")), ClassDefault);
		}
		else
		{
			AddError(FString::Printf(TEXT("%s is gone after the reparent"), Name));
		}
	}

	DiscardFixture(Fixture);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPEmbeddedSetClassDefaultsCompiledTest,
	"Cortex.Blueprint.EmbeddedTemplates.SetClassDefaultsCompiled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPEmbeddedSetClassDefaultsCompiledTest::RunTest(const FString& Parameters)
{
	const FEmbeddedFixture Fixture = MakeFixture(UCortexBPEmbeddedWidgetBase::StaticClass());
	if (!TestTrue(TEXT("Two-Widget-Blueprint fixture"), Fixture.IsValid()))
	{
		DiscardFixture(Fixture);
		return false;
	}
	UUserWidget* B = Fixture.Instance(TEXT("EmbB"));
	if (!TestNotNull(TEXT("EmbB"), B))
	{
		DiscardFixture(Fixture);
		return false;
	}
	SetInt(B, TEXT("Gain"), 99);

	// The default path: compile:true. The compile reinstances the templates, so they are looked up again afterwards.
	const FCortexCommandResult Result = SetDefaults(Fixture.Inner, /*bResync=*/true, /*bCompile=*/true);
	if (TestTrue(TEXT("set_class_defaults with compile succeeds"), Result.bSuccess) && Result.Data.IsValid())
	{
		TestTrue(TEXT("The Blueprint compiled"), ReadBool(Result.Data, TEXT("compiled")));
		// The CDO is written before the compile, so the compile's delta reinstancing keeps the templates' old values: they are stale and reported.
		const TSharedPtr<FJsonObject> EntryA = FindByField(Result.Data, TEXT("embedded_instances"), TEXT("widget_name"), TEXT("EmbA"));
		if (TestNotNull(TEXT("EmbA is reported after a compiled set_class_defaults"), EntryA.Get()))
		{
			TestTrue(TEXT("EmbA resynced"), ReadBool(EntryA, TEXT("resynced")));
		}
	}
	const UUserWidget* A = Fixture.Instance(TEXT("EmbA"));
	B = Fixture.Instance(TEXT("EmbB"));
	if (TestNotNull(TEXT("EmbA after the compile"), A) && TestNotNull(TEXT("EmbB after the compile"), B))
	{
		TestEqual(TEXT("EmbA Label follows the new default"), StringValue(A, TEXT("Label")), FString(TEXT("new")));
		TestEqual(TEXT("EmbA Gain follows the new default"), IntValue(A, TEXT("Gain")), 5);
		TestEqual(TEXT("EmbA OldOnly follows the new default"), IntValue(A, TEXT("OldOnly")), 4);
		TestEqual(TEXT("EmbB Label follows the new default"), StringValue(B, TEXT("Label")), FString(TEXT("new")));
		TestEqual(TEXT("EmbB's intentional Gain override survives"), IntValue(B, TEXT("Gain")), 99);
	}

	DiscardFixture(Fixture);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPEmbeddedReparentPendingCompileTest,
	"Cortex.Blueprint.EmbeddedTemplates.ReparentPendingCompile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPEmbeddedReparentPendingCompileTest::RunTest(const FString& Parameters)
{
	using namespace CortexReparentTest;

	const FEmbeddedFixture Fixture = MakeFixture(UCortexBPEmbeddedWidgetBase::StaticClass());
	if (!TestTrue(TEXT("Two-Widget-Blueprint fixture"), Fixture.IsValid()))
	{
		DiscardFixture(Fixture);
		return false;
	}
	UUserWidget* A = Fixture.Instance(TEXT("EmbA"));
	if (!TestNotNull(TEXT("EmbA"), A))
	{
		DiscardFixture(Fixture);
		return false;
	}
	SetInt(A, TEXT("OldOnly"), 7);

	// compile:false leaves the class as it was: the override would be lost silently at the next compile, so it is reported now.
	TSharedPtr<FJsonObject> Params = ReparentParams(Fixture.Inner, UCortexBPEmbeddedWidgetOther::StaticClass()->GetPathName());
	Params->SetBoolField(TEXT("compile"), false);
	const FCortexCommandResult Result = FCortexBPReparentOps::Execute(Params);
	if (TestTrue(TEXT("Reparent without compile succeeds"), Result.bSuccess) && Result.Data.IsValid())
	{
		TestTrue(TEXT("The response says the class is not recompiled"), ReadBool(Result.Data, TEXT("embedded_report_pending_compile")));
		TestFalse(TEXT("embedded_instances is not claimed"), Result.Data->HasField(TEXT("embedded_instances")));
		const TSharedPtr<FJsonObject> Dropped = FindByField(Result.Data, TEXT("dropped_overrides"), TEXT("widget_name"), TEXT("EmbA"));
		if (TestNotNull(TEXT("EmbA's override of the property the new parent lacks is reported"), Dropped.Get()))
		{
			TestEqual(TEXT("property"), Dropped->GetStringField(TEXT("property")), FString(TEXT("OldOnly")));
			TestEqual(TEXT("instance_value"), Dropped->GetStringField(TEXT("instance_value")), FString(TEXT("7")));
		}
	}

	DiscardFixture(Fixture);
	return true;
}
