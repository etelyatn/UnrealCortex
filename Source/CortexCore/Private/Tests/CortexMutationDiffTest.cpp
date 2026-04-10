#include "Misc/AutomationTest.h"
#include "CortexMutationDiff.h"
#include "CortexSerializer.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Dom/JsonObject.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputAction.h"
#include "PlayerMappableKeySettings.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace
{
	const FString ExpectedChangedMarker = TEXT("<changed>");

	void CleanupMutationDiffFixture(UInputMappingContext* MappingContext, const TArray<UObject*>& SubObjects)
	{
		for (UObject* SubObject : SubObjects)
		{
			if (SubObject != nullptr)
			{
				SubObject->MarkAsGarbage();
			}
		}

		if (MappingContext != nullptr)
		{
			MappingContext->MarkAsGarbage();
		}
	}

	bool FindInstancedTemplateMismatchCandidate(UClass*& OutOwnerClass, FObjectProperty*& OutProperty, FString& OutDescription)
	{
		OutOwnerClass = nullptr;
		OutProperty = nullptr;
		OutDescription.Reset();

		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* OwnerClass = *It;
			if (OwnerClass == nullptr || !OwnerClass->IsNative())
			{
				continue;
			}

			if (OwnerClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				continue;
			}

			if (OwnerClass->IsChildOf<AActor>())
			{
				continue;
			}

			const UObject* OwnerCDO = OwnerClass->GetDefaultObject(false);
			if (OwnerCDO == nullptr)
			{
				continue;
			}

			for (TFieldIterator<FObjectProperty> PropIt(OwnerClass, EFieldIteratorFlags::IncludeSuper); PropIt; ++PropIt)
			{
				FObjectProperty* Property = *PropIt;
				if (Property == nullptr ||
					!Property->HasAllPropertyFlags(CPF_InstancedReference) ||
					Property->HasAnyPropertyFlags(CPF_Transient))
				{
					continue;
				}

				const UObject* TemplateObject = Property->GetObjectPropertyValue_InContainer(OwnerCDO);
				if (TemplateObject == nullptr)
				{
					continue;
				}

				const TSharedPtr<FJsonObject> TemplateDiff =
					FCortexSerializer::NonDefaultPropertiesToJson(TemplateObject, 2);
				if (!TemplateDiff.IsValid() || TemplateDiff->Values.Num() == 0)
				{
					continue;
				}

				OutOwnerClass = OwnerClass;
				OutProperty = Property;
				OutDescription = FString::Printf(TEXT("%s.%s"), *OwnerClass->GetName(), *Property->GetName());
				return true;
			}
		}

		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMutationDiffSubObjectWalkTest,
	"Cortex.Core.MutationDiff.SubObjectWalk",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMutationDiffSubObjectWalkTest::RunTest(const FString& Parameters)
{
	UInputMappingContext* MappingContext = NewObject<UInputMappingContext>(
		GetTransientPackage(), NAME_None, RF_Transient);
	UInputModifierNegate* PersistentModifier = NewObject<UInputModifierNegate>(
		MappingContext, TEXT("PersistentModifier"));
	UInputModifierScalar* NestedModifier = NewObject<UInputModifierScalar>(
		PersistentModifier, TEXT("NestedModifier"));
	UInputModifierNegate* TransientModifier = NewObject<UInputModifierNegate>(
		MappingContext, TEXT("TransientModifier"), RF_Transient);

	const TSharedPtr<FJsonObject> Snapshot = FCortexMutationDiff::SnapshotObject(MappingContext);

	TestTrue(TEXT("Snapshot should be valid"), Snapshot.IsValid());
	if (!Snapshot.IsValid())
	{
		CleanupMutationDiffFixture(MappingContext, { PersistentModifier, NestedModifier, TransientModifier });
		return true;
	}

	TestTrue(TEXT("Snapshot should include class"), Snapshot->HasTypedField<EJson::String>(TEXT("class")));
	if (Snapshot->HasTypedField<EJson::String>(TEXT("class")))
	{
		TestEqual(TEXT("Snapshot class should match object class"),
			Snapshot->GetStringField(TEXT("class")),
			MappingContext->GetClass()->GetName());
	}

	const TArray<TSharedPtr<FJsonValue>>* SubObjectNames = nullptr;
	TestTrue(TEXT("Snapshot should include sub_object_names"),
		Snapshot->TryGetArrayField(TEXT("sub_object_names"), SubObjectNames));
	if (SubObjectNames != nullptr)
	{
		bool bFoundPersistentModifier = false;
		bool bFoundNestedModifier = false;
		bool bFoundTransientModifier = false;
		for (const TSharedPtr<FJsonValue>& Entry : *SubObjectNames)
		{
			if (!Entry.IsValid())
			{
				continue;
			}

			bFoundPersistentModifier |= Entry->AsString() == PersistentModifier->GetName();
			bFoundNestedModifier |= Entry->AsString() == NestedModifier->GetName();
			bFoundTransientModifier |= Entry->AsString() == TransientModifier->GetName();
		}

		TestTrue(TEXT("Non-transient sub-object should be included"), bFoundPersistentModifier);
		TestTrue(TEXT("Nested non-transient sub-object should be included"), bFoundNestedModifier);
		TestFalse(TEXT("Transient sub-object should be excluded"), bFoundTransientModifier);
	}

	CleanupMutationDiffFixture(MappingContext, { PersistentModifier, NestedModifier, TransientModifier });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMutationDiffSubObjectCountTest,
	"Cortex.Core.MutationDiff.SubObjectCount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMutationDiffSubObjectCountTest::RunTest(const FString& Parameters)
{
	UInputMappingContext* MappingContext = NewObject<UInputMappingContext>(
		GetTransientPackage(), NAME_None, RF_Transient);
	UInputModifierNegate* PersistentModifier = NewObject<UInputModifierNegate>(
		MappingContext, TEXT("PersistentModifier"));
	UInputModifierScalar* NestedModifier = NewObject<UInputModifierScalar>(
		PersistentModifier, TEXT("NestedModifier"));
	UInputModifierNegate* TransientModifier = NewObject<UInputModifierNegate>(
		MappingContext, TEXT("TransientModifier"), RF_Transient);

	const TSharedPtr<FJsonObject> Snapshot = FCortexMutationDiff::SnapshotObject(MappingContext);

	TestTrue(TEXT("Snapshot should be valid"), Snapshot.IsValid());
	if (Snapshot.IsValid())
	{
		TestTrue(TEXT("Snapshot should include sub_object_count"),
			Snapshot->HasTypedField<EJson::Number>(TEXT("sub_object_count")));
		if (Snapshot->HasTypedField<EJson::Number>(TEXT("sub_object_count")))
		{
			TestEqual(TEXT("Only non-transient sub-objects should be counted"),
				static_cast<int32>(Snapshot->GetNumberField(TEXT("sub_object_count"))),
				2);
		}
	}

	CleanupMutationDiffFixture(MappingContext, { PersistentModifier, NestedModifier, TransientModifier });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexScopedMutationCapturePreservesDepthTest,
	"Cortex.Core.MutationDiff.ScopedCapturePreservesDepth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexScopedMutationCapturePreservesDepthTest::RunTest(const FString& Parameters)
{
	UInputModifierScalar* Modifier = NewObject<UInputModifierScalar>(GetTransientPackage(), NAME_None, RF_Transient);
	FScopedMutationCapture Capture(Modifier, 0);

	Modifier->Scalar = FVector(2.0, 3.0, 4.0);

	const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Capture.ApplyDiff(Result);

	const TSharedPtr<FJsonObject>* ChangesJson = nullptr;
	TestTrue(TEXT("ApplyDiff should attach changes"), Result->TryGetObjectField(TEXT("changes"), ChangesJson));
	if (ChangesJson != nullptr && (*ChangesJson).IsValid())
	{
		const TSharedPtr<FJsonObject>* PreviousJson = nullptr;
		TestTrue(TEXT("Changes should include previous snapshot"),
			(*ChangesJson)->TryGetObjectField(TEXT("previous"), PreviousJson));
		const TSharedPtr<FJsonObject>* CurrentJson = nullptr;
		TestTrue(TEXT("Changes should include current snapshot"),
			(*ChangesJson)->TryGetObjectField(TEXT("current"), CurrentJson));
		if (PreviousJson != nullptr && (*PreviousJson).IsValid())
		{
			TestTrue(TEXT("Previous snapshot should include class"),
				(*PreviousJson)->HasTypedField<EJson::String>(TEXT("class")));
		}
		if (CurrentJson != nullptr && (*CurrentJson).IsValid())
		{
			const TSharedPtr<FJsonObject>* PropertiesJson = nullptr;
			TestTrue(TEXT("Current snapshot should include non-default properties"),
				(*CurrentJson)->TryGetObjectField(TEXT("non_default_properties"), PropertiesJson));
			if (PropertiesJson != nullptr && (*PropertiesJson).IsValid())
			{
				TestTrue(TEXT("Depth 0 should emit changed marker for nested struct property"),
					(*PropertiesJson)->HasTypedField<EJson::String>(TEXT("Scalar")));
				if ((*PropertiesJson)->HasTypedField<EJson::String>(TEXT("Scalar")))
				{
					TestEqual(TEXT("Nested struct property should use changed marker"),
						(*PropertiesJson)->GetStringField(TEXT("Scalar")),
						ExpectedChangedMarker);
				}
			}
		}
	}

	Modifier->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexScopedMutationCaptureRemovedShapeTest,
	"Cortex.Core.MutationDiff.ScopedCaptureRemovedShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexScopedMutationCaptureRemovedShapeTest::RunTest(const FString& Parameters)
{
	UInputModifierNegate* Modifier = NewObject<UInputModifierNegate>(GetTransientPackage(), NAME_None, RF_Transient);
	FScopedMutationCapture Capture(Modifier, 1);

	const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Capture.ApplyRemoved(Result);

	const TSharedPtr<FJsonObject>* RemovedJson = nullptr;
	TestTrue(TEXT("ApplyRemoved should attach removed snapshot"),
		Result->TryGetObjectField(TEXT("removed"), RemovedJson));
	if (RemovedJson != nullptr && (*RemovedJson).IsValid())
	{
		TestTrue(TEXT("Removed snapshot should include class"),
			(*RemovedJson)->HasTypedField<EJson::String>(TEXT("class")));
		TestTrue(TEXT("Removed snapshot should include sub_object_count"),
			(*RemovedJson)->HasTypedField<EJson::Number>(TEXT("sub_object_count")));
	}

	Modifier->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerNonDefaultPropsTest,
	"Cortex.Core.Serializer.NonDefaultPropertiesToJson.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerNonDefaultPropsTest::RunTest(const FString& Parameters)
{
	USceneCaptureComponent2D* CaptureComponent = NewObject<USceneCaptureComponent2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	const USceneCaptureComponent2D* DefaultComponent =
		CaptureComponent->GetClass()->GetDefaultObject<USceneCaptureComponent2D>();

	CaptureComponent->FOVAngle = DefaultComponent->FOVAngle + 5.0f;
	CaptureComponent->bCameraCutThisFrame = true;

	const TSharedPtr<FJsonObject> Json = FCortexSerializer::NonDefaultPropertiesToJson(CaptureComponent, 1);
	const TSharedPtr<FJsonObject> JsonDepthZero = FCortexSerializer::NonDefaultPropertiesToJson(CaptureComponent, 0);

	TestTrue(TEXT("Non-default JSON should be valid"), Json.IsValid());
	if (Json.IsValid())
	{
		TestTrue(TEXT("Non-default JSON should not be empty"), Json->Values.Num() > 0);
		TestTrue(TEXT("Changed non-transient property should be serialized"),
			Json->HasTypedField<EJson::Number>(TEXT("FOVAngle")));
		if (Json->HasTypedField<EJson::Number>(TEXT("FOVAngle")))
		{
			TestEqual(TEXT("Serialized FOVAngle should match changed value"),
				Json->GetNumberField(TEXT("FOVAngle")),
				static_cast<double>(CaptureComponent->FOVAngle));
		}
		TestFalse(TEXT("Transient properties should be skipped"), Json->HasField(TEXT("bCameraCutThisFrame")));
	}

	TestTrue(TEXT("Depth 0 JSON should be valid"), JsonDepthZero.IsValid());
	if (JsonDepthZero.IsValid())
	{
		TestTrue(TEXT("Depth 0 should emit changed marker instead of serializing value"),
			JsonDepthZero->HasTypedField<EJson::String>(TEXT("FOVAngle")));
		if (JsonDepthZero->HasTypedField<EJson::String>(TEXT("FOVAngle")))
		{
			TestEqual(TEXT("Depth 0 marker should match expected changed marker"),
				JsonDepthZero->GetStringField(TEXT("FOVAngle")),
				ExpectedChangedMarker);
		}
		TestFalse(TEXT("Transient properties should still be skipped at depth 0"),
			JsonDepthZero->HasField(TEXT("bCameraCutThisFrame")));
	}

	CaptureComponent->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerNonDefaultInstancedObjectPropsTest,
	"Cortex.Core.Serializer.NonDefaultPropertiesToJson.InstancedObject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerNonDefaultInstancedObjectPropsTest::RunTest(const FString& Parameters)
{
	UInputAction* Action = NewObject<UInputAction>(GetTransientPackage(), NAME_None, RF_Transient);
	FObjectProperty* SettingsProperty = CastField<FObjectProperty>(
		UInputAction::StaticClass()->FindPropertyByName(TEXT("PlayerMappableKeySettings")));
	TestNotNull(TEXT("Should find PlayerMappableKeySettings property"), SettingsProperty);
	if (SettingsProperty == nullptr)
	{
		Action->MarkAsGarbage();
		return true;
	}

	UPlayerMappableKeySettings* Settings = NewObject<UPlayerMappableKeySettings>(Action);
	Settings->Name = TEXT("TestMapping");
	SettingsProperty->SetObjectPropertyValue_InContainer(Action, Settings);

	const TSharedPtr<FJsonObject> Json = FCortexSerializer::NonDefaultPropertiesToJson(Action, 2);
	const TSharedPtr<FJsonObject> JsonDepthOne = FCortexSerializer::NonDefaultPropertiesToJson(Action, 1);

	TestTrue(TEXT("Instanced object JSON should be valid"), Json.IsValid());
	if (Json.IsValid())
	{
		const TSharedPtr<FJsonObject>* SettingsJson = nullptr;
		TestTrue(TEXT("Instanced object property should be serialized as object"),
			Json->TryGetObjectField(TEXT("PlayerMappableKeySettings"), SettingsJson));
		if (SettingsJson != nullptr && (*SettingsJson).IsValid())
		{
			TestTrue(TEXT("Instanced object should serialize only changed inner fields"),
				(*SettingsJson)->HasTypedField<EJson::String>(TEXT("Name")));
			TestFalse(TEXT("Instanced object should not fully serialize default-only fields"),
				(*SettingsJson)->HasField(TEXT("DisplayCategory")));
		}
	}

	TestTrue(TEXT("Depth-limited instanced object JSON should be valid"), JsonDepthOne.IsValid());
	if (JsonDepthOne.IsValid())
	{
		const TSharedPtr<FJsonObject>* SettingsDepthOneJson = nullptr;
		TestTrue(TEXT("Depth-limited instanced object should still serialize as nested object"),
			JsonDepthOne->TryGetObjectField(TEXT("PlayerMappableKeySettings"), SettingsDepthOneJson));
		if (SettingsDepthOneJson != nullptr && (*SettingsDepthOneJson).IsValid())
		{
			TestTrue(TEXT("Inner changed field should degrade to marker at depth 0"),
				(*SettingsDepthOneJson)->HasTypedField<EJson::String>(TEXT("Name")));
			if ((*SettingsDepthOneJson)->HasTypedField<EJson::String>(TEXT("Name")))
			{
				TestEqual(TEXT("Instanced object changed marker should match expected marker"),
					(*SettingsDepthOneJson)->GetStringField(TEXT("Name")),
				ExpectedChangedMarker);
			}
		}
	}

	Settings->MarkAsGarbage();
	Action->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerInstancedObjectUsesOwningTemplateTest,
	"Cortex.Core.Serializer.NonDefaultPropertiesToJson.InstancedObjectUsesOwningTemplate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerInstancedObjectUsesOwningTemplateTest::RunTest(const FString& Parameters)
{
	UClass* OwnerClass = nullptr;
	FObjectProperty* Property = nullptr;
	FString CandidateDescription;
	TestTrue(TEXT("Should find an instanced object template that differs from its nested class CDO"),
		FindInstancedTemplateMismatchCandidate(OwnerClass, Property, CandidateDescription));
	if (OwnerClass == nullptr || Property == nullptr)
	{
		return true;
	}

	UObject* DuplicatedOwner = DuplicateObject<UObject>(OwnerClass->GetDefaultObject(), GetTransientPackage());
	TestNotNull(TEXT("Should duplicate owner CDO for serializer comparison"), DuplicatedOwner);
	if (DuplicatedOwner == nullptr)
	{
		return true;
	}

	TestNotNull(TEXT("Duplicated owner should preserve the instanced object property"),
		Property->GetObjectPropertyValue_InContainer(DuplicatedOwner));

	const TSharedPtr<FJsonObject> Json = FCortexSerializer::NonDefaultPropertiesToJson(DuplicatedOwner, 2);
	TestTrue(TEXT("Serializer output should be valid"), Json.IsValid());
	if (Json.IsValid())
	{
		TestFalse(*FString::Printf(TEXT("Matching owning template should not produce a false positive for %s"), *CandidateDescription),
			Json->HasField(Property->GetName()));
	}

	DuplicatedOwner->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMutationDiffSnapshotNullObjectTest,
	"Cortex.Core.MutationDiff.SnapshotNullObject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMutationDiffSnapshotNullObjectTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const TSharedPtr<FJsonObject> Snapshot = FCortexMutationDiff::SnapshotObject(nullptr);
	TestFalse(TEXT("Snapshot of null object should return invalid pointer"), Snapshot.IsValid());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexMutationDiffCompareNullPreSnapshotTest,
	"Cortex.Core.MutationDiff.CompareNullPreSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexMutationDiffCompareNullPreSnapshotTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UInputModifierNegate* Modifier = NewObject<UInputModifierNegate>(GetTransientPackage(), NAME_None, RF_Transient);
	const TSharedPtr<FJsonObject> Diff = FCortexMutationDiff::CompareSnapshots(Modifier, nullptr, 1);

	TestTrue(TEXT("Diff should be valid"), Diff.IsValid());
	if (Diff.IsValid())
	{
		TestTrue(TEXT("Diff should have previous key"), Diff->HasField(TEXT("previous")));
		TestTrue(TEXT("previous should be null JSON value"),
			Diff->GetField<EJson::None>(TEXT("previous"))->Type == EJson::Null);

		const TSharedPtr<FJsonObject>* CurrentJson = nullptr;
		TestTrue(TEXT("Diff should have current key as object"),
			Diff->TryGetObjectField(TEXT("current"), CurrentJson));
	}

	Modifier->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexScopedMutationCaptureGarbageCollectedTest,
	"Cortex.Core.MutationDiff.ScopedCaptureGarbageCollected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexScopedMutationCaptureGarbageCollectedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UInputModifierNegate* Modifier = NewObject<UInputModifierNegate>(GetTransientPackage(), NAME_None, RF_Transient);
	FScopedMutationCapture Capture(Modifier, 1);

	Modifier->MarkAsGarbage();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

	const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Capture.ApplyDiff(Result);

	TestFalse(TEXT("Should not have changes key when object is GC'd"),
		Result->HasField(TEXT("changes")));
	TestTrue(TEXT("Should have changes_error key"),
		Result->HasField(TEXT("changes_error")));
	TestEqual(TEXT("changes_error should indicate destruction"),
		Result->GetStringField(TEXT("changes_error")),
		TEXT("object_destroyed_before_diff"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerNonDefaultNullObjectTest,
	"Cortex.Core.Serializer.NonDefaultPropertiesToJson.NullObject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerNonDefaultNullObjectTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const TSharedPtr<FJsonObject> Json = FCortexSerializer::NonDefaultPropertiesToJson(nullptr, 1);
	TestTrue(TEXT("Null object should return valid empty JSON"), Json.IsValid());
	TestEqual(TEXT("Null object should return empty JSON"), Json->Values.Num(), 0);

	return true;
}
