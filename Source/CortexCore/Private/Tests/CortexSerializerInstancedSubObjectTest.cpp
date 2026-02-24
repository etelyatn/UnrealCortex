#include "Misc/AutomationTest.h"
#include "CortexSerializer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "InputMappingContext.h"
#include "EnhancedActionKeyMapping.h"
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
	UInputMappingContext* IMC = NewObject<UInputMappingContext>();
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
	UInputMappingContext* IMC2 = NewObject<UInputMappingContext>();

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
