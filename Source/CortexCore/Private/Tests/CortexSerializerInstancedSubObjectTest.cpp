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
