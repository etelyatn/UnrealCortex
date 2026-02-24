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
