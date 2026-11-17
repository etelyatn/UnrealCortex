#include "Misc/AutomationTest.h"
#include "CortexSerializer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "InputMappingContext.h"
#include "EnhancedActionKeyMapping.h"
#include "InputAction.h"
#include "InputModifiers.h"

// ============================================================================
// Test: Deserialize single instanced sub-object with _class discriminator
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerInstancedSubObjectTest,
	"Cortex.Core.Serializer.InstancedSubObject.Single",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerInstancedSubObjectTest::RunTest(const FString& Parameters)
{
	// FEnhancedActionKeyMapping has UPROPERTY(Instanced) TArray<TObjectPtr<UInputModifier>> Modifiers
	// We'll test setting a single element via the instanced sub-object path.
	// Use a UInputMappingContext as Outer so sub-objects are properly owned.
	UInputMappingContext* IMC = NewObject<UInputMappingContext>(GetTransientPackage(),
		NAME_None, RF_Transient);
	UObject* Outer = IMC;

	// Work with a local FEnhancedActionKeyMapping struct on the stack
	FEnhancedActionKeyMapping Mapping;

	// Build JSON for a single modifier: {"_class": "InputModifierNegate"}
	TSharedPtr<FJsonObject> ModifierJson = MakeShared<FJsonObject>();
	ModifierJson->SetStringField(TEXT("_class"), TEXT("InputModifierNegate"));
	TSharedPtr<FJsonValue> ModifierValue = MakeShared<FJsonValueObject>(ModifierJson);

	// Find the Modifiers property on FEnhancedActionKeyMapping
	const UStruct* MappingStruct = FEnhancedActionKeyMapping::StaticStruct();
	const FProperty* ModifiersProp = MappingStruct->FindPropertyByName(TEXT("Modifiers"));
	TestNotNull(TEXT("Should find Modifiers property"), ModifiersProp);
	if (ModifiersProp == nullptr)
	{
		IMC->MarkAsGarbage();
		return true;
	}

	// Verify CPF_InstancedReference is set on inner property
	const FArrayProperty* ArrayProp = CastField<FArrayProperty>(ModifiersProp);
	TestNotNull(TEXT("Modifiers should be array property"), ArrayProp);
	if (ArrayProp == nullptr)
	{
		IMC->MarkAsGarbage();
		return true;
	}
	TestTrue(TEXT("Inner should have CPF_InstancedReference"),
		ArrayProp->Inner->HasAllPropertyFlags(CPF_InstancedReference));

	// Build JSON array with one modifier
	TArray<TSharedPtr<FJsonValue>> ModifiersArray;
	ModifiersArray.Add(ModifierValue);
	TSharedPtr<FJsonValue> ArrayValue = MakeShared<FJsonValueArray>(ModifiersArray);

	// Deserialize into the mapping's Modifiers array
	void* ValuePtr = ModifiersProp->ContainerPtrToValuePtr<void>(&Mapping);
	TArray<FString> Warnings;
	const bool bResult = FCortexSerializer::JsonToProperty(ArrayValue, ModifiersProp, ValuePtr, Outer, Warnings);

	TestTrue(TEXT("Deserialization should succeed"), bResult);
	TestEqual(TEXT("No warnings"), Warnings.Num(), 0);
	TestEqual(TEXT("Should have 1 modifier"), Mapping.Modifiers.Num(), 1);

	if (Mapping.Modifiers.Num() > 0)
	{
		UInputModifier* Modifier = Mapping.Modifiers[0];
		TestNotNull(TEXT("Modifier should not be null"), Modifier);
		if (Modifier)
		{
			TestTrue(TEXT("Should be UInputModifierNegate"),
				Modifier->IsA<UInputModifierNegate>());
			TestEqual(TEXT("Outer should be the transient UObject"),
				Modifier->GetOuter(), Outer);
		}
	}

	// Cleanup: mark sub-objects as garbage before the outer
	for (UInputModifier* Mod : Mapping.Modifiers)
	{
		if (Mod)
		{
			Mod->MarkAsGarbage();
		}
	}
	IMC->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: Instanced sub-object with custom properties
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerInstancedSubObjectPropsTest,
	"Cortex.Core.Serializer.InstancedSubObject.WithProperties",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerInstancedSubObjectPropsTest::RunTest(const FString& Parameters)
{
	// Use IMC as outer so instanced sub-objects are properly owned
	UInputMappingContext* IMC = NewObject<UInputMappingContext>(GetTransientPackage(),
		NAME_None, RF_Transient);
	UObject* Outer = IMC;

	// Work with a local FEnhancedActionKeyMapping struct on the stack
	FEnhancedActionKeyMapping Mapping;

	// Build JSON: Negate modifier with only bX=true, bY=false, bZ=false
	TSharedPtr<FJsonObject> PropsJson = MakeShared<FJsonObject>();
	PropsJson->SetBoolField(TEXT("bX"), true);
	PropsJson->SetBoolField(TEXT("bY"), false);
	PropsJson->SetBoolField(TEXT("bZ"), false);

	TSharedPtr<FJsonObject> ModifierJson = MakeShared<FJsonObject>();
	ModifierJson->SetStringField(TEXT("_class"), TEXT("InputModifierNegate"));
	ModifierJson->SetObjectField(TEXT("properties"), PropsJson);

	TArray<TSharedPtr<FJsonValue>> ModifiersArray;
	ModifiersArray.Add(MakeShared<FJsonValueObject>(ModifierJson));

	const FProperty* ModifiersProp = FEnhancedActionKeyMapping::StaticStruct()->FindPropertyByName(TEXT("Modifiers"));
	void* ValuePtr = ModifiersProp->ContainerPtrToValuePtr<void>(&Mapping);
	TArray<FString> Warnings;

	const bool bResult = FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueArray>(ModifiersArray), ModifiersProp, ValuePtr, Outer, Warnings);

	TestTrue(TEXT("Should succeed"), bResult);
	TestEqual(TEXT("Should have 1 modifier"), Mapping.Modifiers.Num(), 1);

	if (Mapping.Modifiers.Num() > 0)
	{
		UInputModifierNegate* Negate = Cast<UInputModifierNegate>(Mapping.Modifiers[0]);
		TestNotNull(TEXT("Should be Negate modifier"), Negate);
		if (Negate)
		{
			TestTrue(TEXT("bX should be true"), Negate->bX);
			TestFalse(TEXT("bY should be false"), Negate->bY);
			TestFalse(TEXT("bZ should be false"), Negate->bZ);
		}
	}

	// Cleanup
	for (UInputModifier* Mod : Mapping.Modifiers)
	{
		if (Mod)
		{
			Mod->MarkAsGarbage();
		}
	}
	IMC->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: Array of mixed instanced sub-object types
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerInstancedSubObjectArrayTest,
	"Cortex.Core.Serializer.InstancedSubObject.MixedArray",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerInstancedSubObjectArrayTest::RunTest(const FString& Parameters)
{
	// Use IMC as outer so instanced sub-objects are properly owned
	UInputMappingContext* IMC = NewObject<UInputMappingContext>(GetTransientPackage(),
		NAME_None, RF_Transient);
	UObject* Outer = IMC;

	// Work with a local FEnhancedActionKeyMapping struct on the stack
	FEnhancedActionKeyMapping Mapping;

	// SwizzleAxis + Negate (typical WASD config)
	TSharedPtr<FJsonObject> SwizzleJson = MakeShared<FJsonObject>();
	SwizzleJson->SetStringField(TEXT("_class"), TEXT("InputModifierSwizzleAxis"));

	TSharedPtr<FJsonObject> NegateJson = MakeShared<FJsonObject>();
	NegateJson->SetStringField(TEXT("_class"), TEXT("InputModifierNegate"));

	TArray<TSharedPtr<FJsonValue>> ModifiersArray;
	ModifiersArray.Add(MakeShared<FJsonValueObject>(SwizzleJson));
	ModifiersArray.Add(MakeShared<FJsonValueObject>(NegateJson));

	const FProperty* ModifiersProp = FEnhancedActionKeyMapping::StaticStruct()->FindPropertyByName(TEXT("Modifiers"));
	TestNotNull(TEXT("Should find Modifiers property"), ModifiersProp);
	if (ModifiersProp == nullptr)
	{
		IMC->MarkAsGarbage();
		return true;
	}

	void* ValuePtr = ModifiersProp->ContainerPtrToValuePtr<void>(&Mapping);
	TArray<FString> Warnings;

	const bool bResult = FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueArray>(ModifiersArray), ModifiersProp, ValuePtr, Outer, Warnings);

	TestTrue(TEXT("Should succeed"), bResult);
	TestEqual(TEXT("Should have 2 modifiers"), Mapping.Modifiers.Num(), 2);

	if (Mapping.Modifiers.Num() == 2)
	{
		TestTrue(TEXT("First should be SwizzleAxis"),
			Mapping.Modifiers[0]->IsA<UInputModifierSwizzleAxis>());
		TestTrue(TEXT("Second should be Negate"),
			Mapping.Modifiers[1]->IsA<UInputModifierNegate>());

		// Both should have IMC as outer
		TestEqual(TEXT("First outer is IMC"),
			Mapping.Modifiers[0]->GetOuter(), Outer);
		TestEqual(TEXT("Second outer is IMC"),
			Mapping.Modifiers[1]->GetOuter(), Outer);
	}

	// Cleanup
	for (UInputModifier* Mod : Mapping.Modifiers)
	{
		if (Mod)
		{
			Mod->MarkAsGarbage();
		}
	}
	IMC->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: Overwriting instanced array cleans up old sub-objects
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerInstancedSubObjectCleanupTest,
	"Cortex.Core.Serializer.InstancedSubObject.Cleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerInstancedSubObjectCleanupTest::RunTest(const FString& Parameters)
{
	UInputMappingContext* IMC = NewObject<UInputMappingContext>(GetTransientPackage(),
		NAME_None, RF_Transient);
	UObject* Outer = IMC;

	// Work with a local FEnhancedActionKeyMapping struct on the stack
	FEnhancedActionKeyMapping Mapping;

	const FProperty* ModifiersProp = FEnhancedActionKeyMapping::StaticStruct()->FindPropertyByName(TEXT("Modifiers"));
	TestNotNull(TEXT("Should find Modifiers property"), ModifiersProp);
	if (ModifiersProp == nullptr)
	{
		IMC->MarkAsGarbage();
		return true;
	}

	void* ValuePtr = ModifiersProp->ContainerPtrToValuePtr<void>(&Mapping);

	// First: set two modifiers
	TSharedPtr<FJsonObject> Mod1 = MakeShared<FJsonObject>();
	Mod1->SetStringField(TEXT("_class"), TEXT("InputModifierNegate"));
	TSharedPtr<FJsonObject> Mod2 = MakeShared<FJsonObject>();
	Mod2->SetStringField(TEXT("_class"), TEXT("InputModifierSwizzleAxis"));

	TArray<TSharedPtr<FJsonValue>> FirstArray;
	FirstArray.Add(MakeShared<FJsonValueObject>(Mod1));
	FirstArray.Add(MakeShared<FJsonValueObject>(Mod2));

	TArray<FString> Warnings;
	FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueArray>(FirstArray), ModifiersProp, ValuePtr, Outer, Warnings);

	TestEqual(TEXT("Should have 2 modifiers after first set"), Mapping.Modifiers.Num(), 2);
	if (Mapping.Modifiers.Num() != 2)
	{
		IMC->MarkAsGarbage();
		return true;
	}

	UInputModifier* OldMod0 = Mapping.Modifiers[0];
	UInputModifier* OldMod1 = Mapping.Modifiers[1];

	// Second: overwrite with one modifier
	TSharedPtr<FJsonObject> Mod3 = MakeShared<FJsonObject>();
	Mod3->SetStringField(TEXT("_class"), TEXT("InputModifierDeadZone"));

	TArray<TSharedPtr<FJsonValue>> SecondArray;
	SecondArray.Add(MakeShared<FJsonValueObject>(Mod3));

	Warnings.Empty();
	FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueArray>(SecondArray), ModifiersProp, ValuePtr, Outer, Warnings);

	TestEqual(TEXT("Should have 1 modifier after overwrite"), Mapping.Modifiers.Num(), 1);
	if (Mapping.Modifiers.Num() > 0)
	{
		TestTrue(TEXT("New modifier should be DeadZone"),
			Mapping.Modifiers[0]->IsA<UInputModifierDeadZone>());
	}

	// Old sub-objects should be marked for garbage
	TestTrue(TEXT("Old modifier 0 should be garbage"),
		OldMod0->IsUnreachable() || !OldMod0->IsValidLowLevel() || OldMod0->HasAnyInternalFlags(EInternalObjectFlags::Garbage));
	TestTrue(TEXT("Old modifier 1 should be garbage"),
		OldMod1->IsUnreachable() || !OldMod1->IsValidLowLevel() || OldMod1->HasAnyInternalFlags(EInternalObjectFlags::Garbage));

	// Cleanup remaining modifier
	for (UInputModifier* Mod : Mapping.Modifiers)
	{
		if (Mod)
		{
			Mod->MarkAsGarbage();
		}
	}
	IMC->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: Round-trip — serialize instanced sub-objects then deserialize back
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerInstancedSubObjectRoundTripTest,
	"Cortex.Core.Serializer.InstancedSubObject.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerInstancedSubObjectRoundTripTest::RunTest(const FString& Parameters)
{
	// Create mapping with modifiers programmatically
	UInputMappingContext* IMC = NewObject<UInputMappingContext>(GetTransientPackage(),
		NAME_None, RF_Transient);
	FEnhancedActionKeyMapping Mapping;

	UInputModifierNegate* Negate = NewObject<UInputModifierNegate>(IMC);
	Negate->bX = true;
	Negate->bY = false;
	Negate->bZ = true;
	Mapping.Modifiers.Add(Negate);

	UInputModifierSwizzleAxis* Swizzle = NewObject<UInputModifierSwizzleAxis>(IMC);
	Mapping.Modifiers.Add(Swizzle);

	// Serialize: PropertyToJson should produce {"_class": "...", "properties": {...}}
	const FProperty* ModifiersProp = FEnhancedActionKeyMapping::StaticStruct()->FindPropertyByName(TEXT("Modifiers"));
	const void* ReadPtr = ModifiersProp->ContainerPtrToValuePtr<void>(&Mapping);
	TSharedPtr<FJsonValue> Serialized = FCortexSerializer::PropertyToJson(ModifiersProp, ReadPtr);

	TestNotNull(TEXT("Serialized should not be null"), Serialized.Get());
	TestEqual(TEXT("Should be array"), Serialized->Type, EJson::Array);

	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	TestTrue(TEXT("Should get array"), Serialized->TryGetArray(Arr));
	if (Arr)
	{
		TestEqual(TEXT("Array should have 2 elements"), Arr->Num(), 2);

		// First element should be an object with _class
		if (Arr->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* FirstObj = nullptr;
			TestTrue(TEXT("First element should be object"), (*Arr)[0]->TryGetObject(FirstObj));
			if (FirstObj)
			{
				FString FirstClass;
				TestTrue(TEXT("Should have _class"), (*FirstObj)->TryGetStringField(TEXT("_class"), FirstClass));
				TestEqual(TEXT("First class should be InputModifierNegate"), FirstClass, TEXT("InputModifierNegate"));
			}
		}
	}

	// Deserialize back into a fresh mapping
	FEnhancedActionKeyMapping Mapping2;
	UInputMappingContext* IMC2 = NewObject<UInputMappingContext>(GetTransientPackage(),
		NAME_None, RF_Transient);

	void* WritePtr = ModifiersProp->ContainerPtrToValuePtr<void>(&Mapping2);
	TArray<FString> Warnings;
	const bool bResult = FCortexSerializer::JsonToProperty(Serialized, ModifiersProp, WritePtr, IMC2, Warnings);

	TestTrue(TEXT("Deserialize should succeed"), bResult);
	TestEqual(TEXT("Should have 2 modifiers"), Mapping2.Modifiers.Num(), 2);

	if (Mapping2.Modifiers.Num() == 2)
	{
		UInputModifierNegate* RestoredNegate = Cast<UInputModifierNegate>(Mapping2.Modifiers[0]);
		TestNotNull(TEXT("First should be Negate"), RestoredNegate);
		if (RestoredNegate)
		{
			TestTrue(TEXT("bX should be true"), RestoredNegate->bX);
			TestFalse(TEXT("bY should be false"), RestoredNegate->bY);
			TestTrue(TEXT("bZ should be true"), RestoredNegate->bZ);
		}

		TestTrue(TEXT("Second should be SwizzleAxis"),
			Mapping2.Modifiers[1]->IsA<UInputModifierSwizzleAxis>());
	}

	for (UInputModifier* Mod : Mapping2.Modifiers)
	{
		if (Mod) { Mod->MarkAsGarbage(); }
	}
	IMC->MarkAsGarbage();
	IMC2->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: Invalid _class name produces error
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerInstancedSubObjectInvalidClassTest,
	"Cortex.Core.Serializer.InstancedSubObject.InvalidClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerInstancedSubObjectInvalidClassTest::RunTest(const FString& Parameters)
{
	UInputMappingContext* IMC = NewObject<UInputMappingContext>(GetTransientPackage(),
		NAME_None, RF_Transient);
	UObject* Outer = IMC;

	// Work with a local FEnhancedActionKeyMapping struct on the stack
	FEnhancedActionKeyMapping Mapping;

	TSharedPtr<FJsonObject> BadMod = MakeShared<FJsonObject>();
	BadMod->SetStringField(TEXT("_class"), TEXT("NonExistentModifierClass"));

	TArray<TSharedPtr<FJsonValue>> ModifiersArray;
	ModifiersArray.Add(MakeShared<FJsonValueObject>(BadMod));

	const FProperty* ModifiersProp = FEnhancedActionKeyMapping::StaticStruct()->FindPropertyByName(TEXT("Modifiers"));
	TestNotNull(TEXT("Should find Modifiers property"), ModifiersProp);
	if (ModifiersProp == nullptr)
	{
		IMC->MarkAsGarbage();
		return true;
	}

	void* ValuePtr = ModifiersProp->ContainerPtrToValuePtr<void>(&Mapping);
	TArray<FString> Warnings;

	FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueArray>(ModifiersArray), ModifiersProp, ValuePtr, Outer, Warnings);

	TestTrue(TEXT("Should have warnings for invalid class"), Warnings.Num() > 0);
	if (Warnings.Num() > 0)
	{
		TestTrue(TEXT("Warning should mention class name"),
			Warnings[0].Contains(TEXT("NonExistentModifierClass")));
	}

	IMC->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: A failing element leaves the previous elements and sub-objects intact
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerInstancedSubObjectArrayFailureTest,
	"Cortex.Core.Serializer.InstancedSubObject.ArrayFailurePreservesState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerInstancedSubObjectArrayFailureTest::RunTest(const FString& Parameters)
{
	UInputMappingContext* IMC = NewObject<UInputMappingContext>(GetTransientPackage(),
		NAME_None, RF_Transient);

	FEnhancedActionKeyMapping Mapping;

	UInputModifierNegate* ExistingNegate = NewObject<UInputModifierNegate>(IMC);
	ExistingNegate->bX = true;
	ExistingNegate->bY = false;
	ExistingNegate->bZ = true;
	Mapping.Modifiers.Add(ExistingNegate);

	UInputModifierSwizzleAxis* ExistingSwizzle = NewObject<UInputModifierSwizzleAxis>(IMC);
	Mapping.Modifiers.Add(ExistingSwizzle);

	const FProperty* ModifiersProp = FEnhancedActionKeyMapping::StaticStruct()->FindPropertyByName(TEXT("Modifiers"));
	TestNotNull(TEXT("Should find Modifiers property"), ModifiersProp);
	if (ModifiersProp == nullptr)
	{
		IMC->MarkAsGarbage();
		return true;
	}

	void* ValuePtr = ModifiersProp->ContainerPtrToValuePtr<void>(&Mapping);

	// First element converts; second names a class that does not exist.
	TSharedPtr<FJsonObject> ValidJson = MakeShared<FJsonObject>();
	ValidJson->SetStringField(TEXT("_class"), TEXT("InputModifierNegate"));

	TSharedPtr<FJsonObject> InvalidJson = MakeShared<FJsonObject>();
	InvalidJson->SetStringField(TEXT("_class"), TEXT("NonExistentModifierClass"));

	TArray<TSharedPtr<FJsonValue>> NewArray;
	NewArray.Add(MakeShared<FJsonValueObject>(ValidJson));
	NewArray.Add(MakeShared<FJsonValueObject>(InvalidJson));

	TArray<FString> Warnings;
	const bool bResult = FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueArray>(NewArray), ModifiersProp, ValuePtr, IMC, Warnings);

	TestFalse(TEXT("Write with one failing element should be refused"), bResult);
	TestTrue(TEXT("Refusal should report a warning"), Warnings.Num() > 0);
	TestEqual(TEXT("Array length should be preserved"), Mapping.Modifiers.Num(), 2);
	if (Mapping.Modifiers.Num() == 2)
	{
		TestTrue(TEXT("Element 0 should still be the original sub-object"),
			Mapping.Modifiers[0].Get() == ExistingNegate);
		TestTrue(TEXT("Element 1 should still be the original sub-object"),
			Mapping.Modifiers[1].Get() == ExistingSwizzle);
	}
	TestFalse(TEXT("Original sub-object 0 should not be retired"),
		ExistingNegate->HasAnyInternalFlags(EInternalObjectFlags::Garbage));
	TestFalse(TEXT("Original sub-object 1 should not be retired"),
		ExistingSwizzle->HasAnyInternalFlags(EInternalObjectFlags::Garbage));
	TestTrue(TEXT("Original sub-object values should be intact"),
		ExistingNegate->bX && !ExistingNegate->bY && ExistingNegate->bZ);
	TestTrue(TEXT("Original sub-object class should be intact"),
		ExistingSwizzle->IsA<UInputModifierSwizzleAxis>());

	// The buggy path leaves a substitute sub-object in element 0; retire anything created.
	for (UInputModifier* Modifier : Mapping.Modifiers)
	{
		if (Modifier != nullptr && Modifier != ExistingNegate && Modifier != ExistingSwizzle)
		{
			Modifier->MarkAsGarbage();
		}
	}
	ExistingNegate->MarkAsGarbage();
	ExistingSwizzle->MarkAsGarbage();
	IMC->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: A sparse mapping update keeps the fields it does not mention
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerInstancedSparseMappingTest,
	"Cortex.Core.Serializer.InstancedSubObject.SparseMappingUpdatePreservesFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerInstancedSparseMappingTest::RunTest(const FString& Parameters)
{
	UInputMappingContext* IMC = NewObject<UInputMappingContext>(GetTransientPackage(), NAME_None, RF_Transient);

	UInputAction* Action = NewObject<UInputAction>(IMC);
	const UInputAction* ExpectedAction = Action;
	UInputModifierNegate* OriginalModifier = NewObject<UInputModifierNegate>(IMC);

	FEnhancedActionKeyMapping Mapping;
	Mapping.Action = Action;
	Mapping.Key = EKeys::A;
	Mapping.Modifiers.Add(OriginalModifier);

	// The mapping array is private: UE 5.7 moved it into the DefaultKeyMappings struct, so locate it
	// through reflection and address it with script containers.
	FArrayProperty* MappingsProperty = nullptr;
	void* MappingsValuePtr = nullptr;

	if (FStructProperty* DefaultKeyMappings = CastField<FStructProperty>(
		UInputMappingContext::StaticClass()->FindPropertyByName(TEXT("DefaultKeyMappings"))))
	{
		MappingsProperty = CastField<FArrayProperty>(
			DefaultKeyMappings->Struct->FindPropertyByName(TEXT("Mappings")));
		if (MappingsProperty != nullptr)
		{
			void* MappingData = DefaultKeyMappings->ContainerPtrToValuePtr<void>(IMC);
			MappingsValuePtr = MappingsProperty->ContainerPtrToValuePtr<void>(MappingData);
		}
	}

	if (MappingsProperty == nullptr)
	{
		MappingsProperty = CastField<FArrayProperty>(
			UInputMappingContext::StaticClass()->FindPropertyByName(TEXT("Mappings")));
		if (MappingsProperty != nullptr)
		{
			MappingsValuePtr = MappingsProperty->ContainerPtrToValuePtr<void>(IMC);
		}
	}

	TestNotNull(TEXT("Mappings property should exist"), MappingsProperty);
	if (MappingsProperty == nullptr || MappingsValuePtr == nullptr)
	{
		OriginalModifier->MarkAsGarbage();
		Action->MarkAsGarbage();
		IMC->MarkAsGarbage();
		return true;
	}

	{
		FScriptArrayHelper SeedHelper(MappingsProperty, MappingsValuePtr);
		const int32 SeedIndex = SeedHelper.AddValue();
		*reinterpret_cast<FEnhancedActionKeyMapping*>(SeedHelper.GetRawPtr(SeedIndex)) = Mapping;
	}

	// 1. Sparse update: only the key is provided, so the action and the modifier must survive.
	TSharedPtr<FJsonObject> KeyJson = MakeShared<FJsonObject>();
	KeyJson->SetStringField(TEXT("KeyName"), TEXT("B"));
	TSharedPtr<FJsonObject> SparseMapping = MakeShared<FJsonObject>();
	SparseMapping->SetObjectField(TEXT("Key"), KeyJson);

	TArray<TSharedPtr<FJsonValue>> SparseArray;
	SparseArray.Add(MakeShared<FJsonValueObject>(SparseMapping));

	TArray<FString> Warnings;
	const bool bSparseWritten = FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueArray>(SparseArray), MappingsProperty, MappingsValuePtr, IMC, Warnings);

	TestTrue(TEXT("Sparse mapping write should succeed"), bSparseWritten);
	{
		FScriptArrayHelper LiveHelper(MappingsProperty, MappingsValuePtr);
		TestEqual(TEXT("Sparse write keeps one mapping"), LiveHelper.Num(), 1);
		if (LiveHelper.Num() == 1)
		{
			const FEnhancedActionKeyMapping& LiveMapping =
				*reinterpret_cast<FEnhancedActionKeyMapping*>(LiveHelper.GetRawPtr(0));
			TestTrue(TEXT("Sparse write applies the provided key"), LiveMapping.Key == EKeys::B);
			TestTrue(TEXT("Sparse write keeps the action"), LiveMapping.Action.Get() == ExpectedAction);
			TestEqual(TEXT("Sparse write keeps the modifier count"), LiveMapping.Modifiers.Num(), 1);
			if (LiveMapping.Modifiers.Num() == 1)
			{
				TestTrue(
					TEXT("Sparse write keeps the same modifier object"),
					LiveMapping.Modifiers[0].Get() == OriginalModifier);
				TestFalse(
					TEXT("Sparse write must not retire the retained modifier"),
					OriginalModifier->HasAnyInternalFlags(EInternalObjectFlags::Garbage));
			}
		}
	}

	// 2. Replacing Modifiers retires the previous owned modifier at commit and keeps the rest.
	TSharedPtr<FJsonObject> NewModifierJson = MakeShared<FJsonObject>();
	NewModifierJson->SetStringField(TEXT("_class"), TEXT("InputModifierNegate"));
	TArray<TSharedPtr<FJsonValue>> ReplacementModifiers;
	ReplacementModifiers.Add(MakeShared<FJsonValueObject>(NewModifierJson));

	TSharedPtr<FJsonObject> ReplacementMapping = MakeShared<FJsonObject>();
	ReplacementMapping->SetArrayField(TEXT("Modifiers"), ReplacementModifiers);

	TArray<TSharedPtr<FJsonValue>> ReplacementArray;
	ReplacementArray.Add(MakeShared<FJsonValueObject>(ReplacementMapping));

	Warnings.Empty();
	const bool bReplaced = FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueArray>(ReplacementArray), MappingsProperty, MappingsValuePtr, IMC, Warnings);

	TestTrue(TEXT("Modifier replacement write should succeed"), bReplaced);
	UInputModifier* ReplacementModifier = nullptr;
	{
		FScriptArrayHelper LiveHelper(MappingsProperty, MappingsValuePtr);
		TestEqual(TEXT("Replacement write keeps one mapping"), LiveHelper.Num(), 1);
		if (LiveHelper.Num() == 1)
		{
			const FEnhancedActionKeyMapping& LiveMapping =
				*reinterpret_cast<FEnhancedActionKeyMapping*>(LiveHelper.GetRawPtr(0));
			TestTrue(TEXT("Replacement write keeps the action"), LiveMapping.Action.Get() == ExpectedAction);
			TestTrue(TEXT("Replacement write keeps the provided key"), LiveMapping.Key == EKeys::B);
			TestEqual(TEXT("Replacement write installs one modifier"), LiveMapping.Modifiers.Num(), 1);
			if (LiveMapping.Modifiers.Num() == 1)
			{
				ReplacementModifier = LiveMapping.Modifiers[0];
				TestTrue(
					TEXT("Replacement modifier is a new Negate"),
					ReplacementModifier != nullptr && ReplacementModifier->IsA<UInputModifierNegate>());
				TestTrue(TEXT("Replacement modifier is not the original"), ReplacementModifier != OriginalModifier);
			}
		}
	}
	TestTrue(
		TEXT("Replacing the modifier retires the previous owned modifier"),
		OriginalModifier->HasAnyInternalFlags(EInternalObjectFlags::Garbage));

	// 3. A failing element leaves the mapping untouched and retires nothing it still references.
	const UInputAction* ActionBeforeFailure = nullptr;
	FKey KeyBeforeFailure;
	UInputModifier* ModifierBeforeFailure = nullptr;
	{
		FScriptArrayHelper LiveHelper(MappingsProperty, MappingsValuePtr);
		if (LiveHelper.Num() == 1)
		{
			const FEnhancedActionKeyMapping& LiveMapping =
				*reinterpret_cast<FEnhancedActionKeyMapping*>(LiveHelper.GetRawPtr(0));
			ActionBeforeFailure = LiveMapping.Action.Get();
			KeyBeforeFailure = LiveMapping.Key;
			if (LiveMapping.Modifiers.Num() == 1)
			{
				ModifierBeforeFailure = LiveMapping.Modifiers[0];
			}
		}
	}

	TSharedPtr<FJsonObject> KeyUpdateJson = MakeShared<FJsonObject>();
	KeyUpdateJson->SetStringField(TEXT("KeyName"), TEXT("C"));
	TSharedPtr<FJsonObject> FirstMapping = MakeShared<FJsonObject>();
	FirstMapping->SetObjectField(TEXT("Key"), KeyUpdateJson);

	TSharedPtr<FJsonObject> InvalidModifierJson = MakeShared<FJsonObject>();
	InvalidModifierJson->SetStringField(TEXT("_class"), TEXT("NonExistentModifierClass"));
	TArray<TSharedPtr<FJsonValue>> InvalidModifiers;
	InvalidModifiers.Add(MakeShared<FJsonValueObject>(InvalidModifierJson));
	TSharedPtr<FJsonObject> SecondMapping = MakeShared<FJsonObject>();
	SecondMapping->SetArrayField(TEXT("Modifiers"), InvalidModifiers);

	TArray<TSharedPtr<FJsonValue>> FailingArray;
	FailingArray.Add(MakeShared<FJsonValueObject>(FirstMapping));
	FailingArray.Add(MakeShared<FJsonValueObject>(SecondMapping));

	Warnings.Empty();
	const bool bFailingWritten = FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueArray>(FailingArray), MappingsProperty, MappingsValuePtr, IMC, Warnings);

	TestFalse(TEXT("A write with a failing element should be refused"), bFailingWritten);
	TestTrue(TEXT("The refusal should report a warning"), Warnings.Num() > 0);
	{
		FScriptArrayHelper LiveHelper(MappingsProperty, MappingsValuePtr);
		TestEqual(TEXT("The refusal keeps one mapping"), LiveHelper.Num(), 1);
		if (LiveHelper.Num() == 1)
		{
			const FEnhancedActionKeyMapping& LiveMapping =
				*reinterpret_cast<FEnhancedActionKeyMapping*>(LiveHelper.GetRawPtr(0));
			TestTrue(TEXT("The refusal keeps the action"), LiveMapping.Action.Get() == ActionBeforeFailure);
			TestTrue(TEXT("The refusal keeps the key"), LiveMapping.Key == KeyBeforeFailure);
			TestEqual(TEXT("The refusal keeps the modifier count"), LiveMapping.Modifiers.Num(), 1);
			if (LiveMapping.Modifiers.Num() == 1)
			{
				TestTrue(
					TEXT("The refusal keeps the same modifier object"),
					LiveMapping.Modifiers[0].Get() == ModifierBeforeFailure);
			}
		}
	}
	if (ModifierBeforeFailure != nullptr)
	{
		TestFalse(
			TEXT("The refusal must not retire the modifier the live mapping references"),
			ModifierBeforeFailure->HasAnyInternalFlags(EInternalObjectFlags::Garbage));
	}

	{
		FScriptArrayHelper LiveHelper(MappingsProperty, MappingsValuePtr);
		for (int32 Index = 0; Index < LiveHelper.Num(); ++Index)
		{
			FEnhancedActionKeyMapping& LiveMapping =
				*reinterpret_cast<FEnhancedActionKeyMapping*>(LiveHelper.GetRawPtr(Index));
			for (UInputModifier* Modifier : LiveMapping.Modifiers)
			{
				if (Modifier != nullptr)
				{
					Modifier->MarkAsGarbage();
				}
			}
		}
	}
	OriginalModifier->MarkAsGarbage();
	Action->MarkAsGarbage();
	IMC->MarkAsGarbage();
	return true;
}
