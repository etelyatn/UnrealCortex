#include "Misc/AutomationTest.h"
#include "CortexMutationDiff.h"
#include "CortexSerializer.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Dom/JsonObject.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"

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
	UInputModifierNegate* TransientModifier = NewObject<UInputModifierNegate>(
		MappingContext, TEXT("TransientModifier"), RF_Transient);

	FCortexMutationDiff MutationDiff;
	const TSharedPtr<FJsonObject> Snapshot = MutationDiff.SnapshotObject(MappingContext);

	TestTrue(TEXT("Snapshot should be valid"), Snapshot.IsValid());
	if (!Snapshot.IsValid())
	{
		CleanupMutationDiffFixture(MappingContext, { PersistentModifier, TransientModifier });
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
		bool bFoundTransientModifier = false;
		for (const TSharedPtr<FJsonValue>& Entry : *SubObjectNames)
		{
			if (!Entry.IsValid())
			{
				continue;
			}

			bFoundPersistentModifier |= Entry->AsString() == PersistentModifier->GetName();
			bFoundTransientModifier |= Entry->AsString() == TransientModifier->GetName();
		}

		TestTrue(TEXT("Non-transient sub-object should be included"), bFoundPersistentModifier);
		TestFalse(TEXT("Transient sub-object should be excluded"), bFoundTransientModifier);
	}

	CleanupMutationDiffFixture(MappingContext, { PersistentModifier, TransientModifier });
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
	UInputModifierNegate* TransientModifier = NewObject<UInputModifierNegate>(
		MappingContext, TEXT("TransientModifier"), RF_Transient);

	FCortexMutationDiff MutationDiff;
	const TSharedPtr<FJsonObject> Snapshot = MutationDiff.SnapshotObject(MappingContext);

	TestTrue(TEXT("Snapshot should be valid"), Snapshot.IsValid());
	if (Snapshot.IsValid())
	{
		TestTrue(TEXT("Snapshot should include sub_object_count"),
			Snapshot->HasTypedField<EJson::Number>(TEXT("sub_object_count")));
		if (Snapshot->HasTypedField<EJson::Number>(TEXT("sub_object_count")))
		{
			TestEqual(TEXT("Only non-transient sub-objects should be counted"),
				static_cast<int32>(Snapshot->GetNumberField(TEXT("sub_object_count"))),
				1);
		}
	}

	CleanupMutationDiffFixture(MappingContext, { PersistentModifier, TransientModifier });
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
	FCortexMutationDiff MutationDiff;
	FScopedMutationCapture Capture(MutationDiff, Modifier, 0);

	Modifier->Scalar = FVector(2.0, 3.0, 4.0);

	const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Capture.ApplyDiff(Result);

	const TSharedPtr<FJsonObject>* ChangesJson = nullptr;
	TestTrue(TEXT("ApplyDiff should attach changes"), Result->TryGetObjectField(TEXT("changes"), ChangesJson));
	if (ChangesJson != nullptr && (*ChangesJson).IsValid())
	{
		const TSharedPtr<FJsonObject>* CurrentJson = nullptr;
		TestTrue(TEXT("Changes should include current snapshot"),
			(*ChangesJson)->TryGetObjectField(TEXT("current"), CurrentJson));
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
	FCortexSerializerNonDefaultPropsTest,
	"Cortex.Core.Serializer.NonDefaultPropertiesToJson",
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
