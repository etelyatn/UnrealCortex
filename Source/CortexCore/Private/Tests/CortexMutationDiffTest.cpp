#include "Misc/AutomationTest.h"
#include "CortexMutationDiff.h"
#include "CortexSerializer.h"
#include "Dom/JsonObject.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"

namespace
{
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
	FCortexSerializerNonDefaultPropsTest,
	"Cortex.Core.Serializer.NonDefaultPropertiesToJson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerNonDefaultPropsTest::RunTest(const FString& Parameters)
{
	UInputModifierNegate* Modifier = NewObject<UInputModifierNegate>(GetTransientPackage(), NAME_None, RF_Transient);
	Modifier->bX = false;

	const TSharedPtr<FJsonObject> Json = FCortexSerializer::NonDefaultPropertiesToJson(Modifier, 1);

	TestTrue(TEXT("Non-default JSON should be valid"), Json.IsValid());
	if (Json.IsValid())
	{
		TestTrue(TEXT("Non-default JSON should not be empty"), Json->Values.Num() > 0);
		TestTrue(TEXT("Changed property should be serialized"), Json->HasTypedField<EJson::Boolean>(TEXT("bX")));
		if (Json->HasTypedField<EJson::Boolean>(TEXT("bX")))
		{
			TestFalse(TEXT("Serialized bX should be false"), Json->GetBoolField(TEXT("bX")));
		}
	}

	Modifier->MarkAsGarbage();
	return true;
}
