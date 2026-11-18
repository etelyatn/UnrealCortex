#include "Misc/AutomationTest.h"
#include "CortexSerializer.h"
#include "CortexSerializerEnumTestTypes.h"
#include "CortexSerializerTypeSafetyTestTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameplayTagsManager.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace CortexSerializerTypeSafetyTest
{
	/** LogJson records this when an As* accessor falls back on a mismatched JSON type. */
	const TCHAR* const JsonTypeMismatchLog = TEXT("Json Value of type");

	/**
	 * Registers the test-only gameplay tags through the tag manager's runtime row insertion (the same path
	 * loose tag ini files and data tables use). CortexCore is an editor module, so UE_DEFINE_GAMEPLAY_TAG_STATIC
	 * is not allowed here: native tags must come from runtime modules and the manager ensures on anything else.
	 * The registration is session-scoped; the manager has no runtime API to remove a tag row.
	 */
	void RegisterTestGameplayTags()
	{
		static bool bRegistered = false;
		if (bRegistered)
		{
			return;
		}
		bRegistered = true;

		// The public runtime registration (callable after startup); unlike UE_DEFINE_GAMEPLAY_TAG_STATIC it does not
		// validate the module type, so it is valid from this editor module's tests.
		UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
		TagManager.AddNativeGameplayTag(FName(TEXT("Cortex.Test.Serializer.ContainerA")), TEXT("CortexSerializerTypeSafetyTest"));
		TagManager.AddNativeGameplayTag(FName(TEXT("Cortex.Test.Serializer.ContainerB")), TEXT("CortexSerializerTypeSafetyTest"));
	}

	/** Resolves a test gameplay tag, registering the test tags first. */
	FGameplayTag RequestTestGameplayTag(const TCHAR* TagName)
	{
		RegisterTestGameplayTags();
		return FGameplayTag::RequestGameplayTag(FName(TagName), /*bErrorIfNotFound*/ false);
	}

	/** Text representation of a property value, used to prove a refused write changed nothing. */
	FString ExportValueText(const FProperty* Property, const void* ValuePtr)
	{
		FString ValueText;
		Property->ExportTextItem_Direct(ValueText, ValuePtr, nullptr, nullptr, PPF_None);
		return ValueText;
	}

	bool AnyWarningMentions(const TArray<FString>& Warnings, const FString& Token)
	{
		for (const FString& Warning : Warnings)
		{
			if (Warning.Contains(Token, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * Walk an expanded instanced chain and report the nesting depth at which it degrades to one of
	 * CyclePaths, or INDEX_NONE when the chain never names one of those objects.
	 */
	int32 FindCyclePathDepth(
		const TSharedPtr<FJsonValue>& JsonValue,
		const TArray<FString>& CyclePaths,
		int32 Depth,
		int32 MaxDepth)
	{
		if (!JsonValue.IsValid() || Depth > MaxDepth)
		{
			return INDEX_NONE;
		}

		if (JsonValue->Type == EJson::String)
		{
			return CyclePaths.Contains(JsonValue->AsString()) ? Depth : INDEX_NONE;
		}

		const TSharedPtr<FJsonObject>* ObjectValue = nullptr;
		if (!JsonValue->TryGetObject(ObjectValue) || ObjectValue == nullptr)
		{
			return INDEX_NONE;
		}

		// Instanced objects serialize as {"_class": ..., "properties": {"Next": ...}}.
		const TSharedPtr<FJsonObject>* PropertiesValue = nullptr;
		if (!(*ObjectValue)->TryGetObjectField(TEXT("properties"), PropertiesValue) || PropertiesValue == nullptr)
		{
			return INDEX_NONE;
		}

		const TSharedPtr<FJsonValue> NextValue = (*PropertiesValue)->TryGetField(TEXT("Next"));
		return FindCyclePathDepth(NextValue, CyclePaths, Depth + 1, MaxDepth);
	}
}

// ============================================================================
// Test: Every scalar branch refuses a mismatched JSON type and leaves the value unchanged
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerTypeSafetyScalarMismatchTest,
	"Cortex.Core.Serializer.TypeSafety.ScalarMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerTypeSafetyScalarMismatchTest::RunTest(const FString& Parameters)
{
	// The buggy path reads the value through an unchecked As* accessor, which logs the mismatch.
	AddExpectedError(
		CortexSerializerTypeSafetyTest::JsonTypeMismatchLog,
		EAutomationExpectedErrorFlags::Contains,
		-1);

	UCortexSerializerTypeSafetyTestObject* Object = NewObject<UCortexSerializerTypeSafetyTestObject>(
		GetTransientPackage(), NAME_None, RF_Transient);

	// Non-default sentinels so a silent write to a default value is visible.
	Object->IntValue = 42;
	Object->Int64Value = 42;
	Object->FloatValue = 42.0f;
	Object->DoubleValue = 42.0;
	Object->ByteValue = 42;
	Object->bBoolValue = true;
	Object->StringValue = TEXT("sentinel");
	Object->NameValue = FName(TEXT("SentinelName"));

	struct FCase
	{
		FName PropertyName;
		FString Label;
		TSharedPtr<FJsonValue> Value;
		FString ReceivedTypeToken;
	};

	const TSharedPtr<FJsonValue> EmptyObject = MakeShared<FJsonValueObject>(MakeShared<FJsonObject>());

	TArray<FCase> Cases;
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, IntValue),
		TEXT("string into int32"),
		MakeShared<FJsonValueString>(TEXT("abc")),
		TEXT("string")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, IntValue),
		TEXT("object into int32"),
		EmptyObject,
		TEXT("object")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, Int64Value),
		TEXT("string into int64"),
		MakeShared<FJsonValueString>(TEXT("abc")),
		TEXT("string")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, Int64Value),
		TEXT("object into int64"),
		EmptyObject,
		TEXT("object")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, FloatValue),
		TEXT("string into float"),
		MakeShared<FJsonValueString>(TEXT("abc")),
		TEXT("string")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, FloatValue),
		TEXT("object into float"),
		EmptyObject,
		TEXT("object")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, DoubleValue),
		TEXT("string into double"),
		MakeShared<FJsonValueString>(TEXT("abc")),
		TEXT("string")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, DoubleValue),
		TEXT("object into double"),
		EmptyObject,
		TEXT("object")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, ByteValue),
		TEXT("string into byte"),
		MakeShared<FJsonValueString>(TEXT("abc")),
		TEXT("string")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, ByteValue),
		TEXT("object into byte"),
		EmptyObject,
		TEXT("object")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, bBoolValue),
		TEXT("number into bool"),
		MakeShared<FJsonValueNumber>(0.0),
		TEXT("number")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, bBoolValue),
		TEXT("string into bool"),
		MakeShared<FJsonValueString>(TEXT("abc")),
		TEXT("string")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, bBoolValue),
		TEXT("object into bool"),
		EmptyObject,
		TEXT("object")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, StringValue),
		TEXT("number into string"),
		MakeShared<FJsonValueNumber>(0.0),
		TEXT("number")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, StringValue),
		TEXT("object into string"),
		EmptyObject,
		TEXT("object")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, NameValue),
		TEXT("number into name"),
		MakeShared<FJsonValueNumber>(0.0),
		TEXT("number")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, NameValue),
		TEXT("object into name"),
		EmptyObject,
		TEXT("object")});

	for (const FCase& Case : Cases)
	{
		const FProperty* Property = UCortexSerializerTypeSafetyTestObject::StaticClass()->FindPropertyByName(Case.PropertyName);
		TestNotNull(*FString::Printf(TEXT("%s: property should exist"), *Case.Label), Property);
		if (Property == nullptr)
		{
			continue;
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
		const FString Before = CortexSerializerTypeSafetyTest::ExportValueText(Property, ValuePtr);

		TArray<FString> Warnings;
		const bool bWritten = FCortexSerializer::JsonToProperty(Case.Value, Property, ValuePtr, Object, Warnings);

		TestFalse(*FString::Printf(TEXT("%s should be refused"), *Case.Label), bWritten);
		TestTrue(*FString::Printf(TEXT("%s should report a warning"), *Case.Label), Warnings.Num() > 0);
		TestTrue(
			*FString::Printf(TEXT("%s warning should name the property '%s'"), *Case.Label, *Property->GetName()),
			CortexSerializerTypeSafetyTest::AnyWarningMentions(Warnings, Property->GetName()));
		TestTrue(
			*FString::Printf(TEXT("%s warning should name the received JSON type '%s'"), *Case.Label, *Case.ReceivedTypeToken),
			CortexSerializerTypeSafetyTest::AnyWarningMentions(Warnings, Case.ReceivedTypeToken));
		TestEqual(
			*FString::Printf(TEXT("%s should leave the value unchanged"), *Case.Label),
			CortexSerializerTypeSafetyTest::ExportValueText(Property, ValuePtr),
			Before);
	}

	Object->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: Numeric values outside a scalar's representable range are refused
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerTypeSafetyNumericRangeTest,
	"Cortex.Core.Serializer.TypeSafety.NumericRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerTypeSafetyNumericRangeTest::RunTest(const FString& Parameters)
{
	UCortexSerializerTypeSafetyTestObject* Object = NewObject<UCortexSerializerTypeSafetyTestObject>(
		GetTransientPackage(), NAME_None, RF_Transient);

	Object->IntValue = 42;
	Object->Int64Value = 42;
	Object->ByteValue = 42;

	struct FCase
	{
		FName PropertyName;
		FString Label;
		double Value;
	};

	TArray<FCase> Cases;
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, IntValue),
		TEXT("int32 above MAX_int32"),
		5000000000.0});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, IntValue),
		TEXT("int32 below MIN_int32"),
		-5000000000.0});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, Int64Value),
		TEXT("int64 above MAX_int64"),
		1.0e19});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, ByteValue),
		TEXT("byte above MAX_uint8"),
		300.0});

	for (const FCase& Case : Cases)
	{
		const FProperty* Property = UCortexSerializerTypeSafetyTestObject::StaticClass()->FindPropertyByName(Case.PropertyName);
		TestNotNull(*FString::Printf(TEXT("%s: property should exist"), *Case.Label), Property);
		if (Property == nullptr)
		{
			continue;
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
		const FString Before = CortexSerializerTypeSafetyTest::ExportValueText(Property, ValuePtr);

		TArray<FString> Warnings;
		const bool bWritten = FCortexSerializer::JsonToProperty(
			MakeShared<FJsonValueNumber>(Case.Value), Property, ValuePtr, Object, Warnings);

		TestFalse(*FString::Printf(TEXT("%s should be refused"), *Case.Label), bWritten);
		TestTrue(*FString::Printf(TEXT("%s should report a warning"), *Case.Label), Warnings.Num() > 0);
		TestEqual(
			*FString::Printf(TEXT("%s should leave the value unchanged"), *Case.Label),
			CortexSerializerTypeSafetyTest::ExportValueText(Property, ValuePtr),
			Before);
	}

	Object->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: Existing numeric-string coercion keeps working
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerTypeSafetyNumericStringTest,
	"Cortex.Core.Serializer.TypeSafety.NumericStringCoercion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerTypeSafetyNumericStringTest::RunTest(const FString& Parameters)
{
	UCortexSerializerTypeSafetyTestObject* Object = NewObject<UCortexSerializerTypeSafetyTestObject>(
		GetTransientPackage(), NAME_None, RF_Transient);

	const FProperty* IntProperty = UCortexSerializerTypeSafetyTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, IntValue));
	const FProperty* Int64Property = UCortexSerializerTypeSafetyTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, Int64Value));
	const FProperty* FloatProperty = UCortexSerializerTypeSafetyTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, FloatValue));
	const FProperty* DoubleProperty = UCortexSerializerTypeSafetyTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, DoubleValue));
	const FProperty* ByteProperty = UCortexSerializerTypeSafetyTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, ByteValue));
	TestNotNull(TEXT("IntValue property should exist"), IntProperty);
	TestNotNull(TEXT("Int64Value property should exist"), Int64Property);
	TestNotNull(TEXT("FloatValue property should exist"), FloatProperty);
	TestNotNull(TEXT("DoubleValue property should exist"), DoubleProperty);
	TestNotNull(TEXT("ByteValue property should exist"), ByteProperty);
	if (IntProperty == nullptr || Int64Property == nullptr || FloatProperty == nullptr
		|| DoubleProperty == nullptr || ByteProperty == nullptr)
	{
		Object->MarkAsGarbage();
		return true;
	}

	TArray<FString> Warnings;
	const bool bIntWritten = FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueString>(TEXT("5")),
		IntProperty,
		IntProperty->ContainerPtrToValuePtr<void>(Object),
		Object,
		Warnings);
	TestTrue(TEXT("Numeric string should write an int32"), bIntWritten);
	TestEqual(TEXT("int32 should be 5"), Object->IntValue, 5);

	const bool bInt64Written = FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueString>(TEXT("7")),
		Int64Property,
		Int64Property->ContainerPtrToValuePtr<void>(Object),
		Object,
		Warnings);
	TestTrue(TEXT("Numeric string should write an int64"), bInt64Written);
	TestEqual(TEXT("int64 should be 7"), Object->Int64Value, static_cast<int64>(7));

	const bool bFloatWritten = FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueString>(TEXT("0.5")),
		FloatProperty,
		FloatProperty->ContainerPtrToValuePtr<void>(Object),
		Object,
		Warnings);
	TestTrue(TEXT("Numeric string should write a float"), bFloatWritten);
	TestEqual(TEXT("float should be 0.5"), Object->FloatValue, 0.5f);

	const bool bDoubleWritten = FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueString>(TEXT("2.5")),
		DoubleProperty,
		DoubleProperty->ContainerPtrToValuePtr<void>(Object),
		Object,
		Warnings);
	TestTrue(TEXT("Numeric string should write a double"), bDoubleWritten);
	TestEqual(TEXT("double should be 2.5"), Object->DoubleValue, 2.5);

	const bool bByteWritten = FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueString>(TEXT("9")),
		ByteProperty,
		ByteProperty->ContainerPtrToValuePtr<void>(Object),
		Object,
		Warnings);
	TestTrue(TEXT("Numeric string should write a byte"), bByteWritten);
	TestEqual(TEXT("byte should be 9"), static_cast<int32>(Object->ByteValue), 9);

	TestEqual(TEXT("Valid numeric strings should not warn"), Warnings.Num(), 0);

	Object->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: Numeric enum writes must name a declared enumerator
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerTypeSafetyEnumRangeTest,
	"Cortex.Core.Serializer.TypeSafety.EnumOutOfRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerTypeSafetyEnumRangeTest::RunTest(const FString& Parameters)
{
	UCortexSerializerEnumTestObject* Object = NewObject<UCortexSerializerEnumTestObject>(
		GetTransientPackage(), NAME_None, RF_Transient);

	const FProperty* LevelProperty = UCortexSerializerEnumTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerEnumTestObject, Level));
	const FProperty* ByteLevelProperty = UCortexSerializerEnumTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerEnumTestObject, ByteLevel));
	TestNotNull(TEXT("Level property should exist"), LevelProperty);
	TestNotNull(TEXT("ByteLevel property should exist"), ByteLevelProperty);
	if (LevelProperty == nullptr || ByteLevelProperty == nullptr)
	{
		Object->MarkAsGarbage();
		return true;
	}

	Object->Level = ECortexSerializerSparseEnum::Low;
	Object->ByteLevel = CSSBE_First;

	// Declared gaps: 0, 1, 2, 4, 8 and 0, 10, 20. Every value below is inside the underlying
	// storage type but outside the enum, so it must be refused instead of stored.
	TArray<double> InvalidEnumValues;
	InvalidEnumValues.Add(3.0);
	InvalidEnumValues.Add(300.0);

	for (const double InvalidValue : InvalidEnumValues)
	{
		TArray<FString> Warnings;
		const bool bWritten = FCortexSerializer::JsonToProperty(
			MakeShared<FJsonValueNumber>(InvalidValue),
			LevelProperty,
			LevelProperty->ContainerPtrToValuePtr<void>(Object),
			Object,
			Warnings);

		TestFalse(*FString::Printf(TEXT("Enum value %f should be refused for Level"), InvalidValue), bWritten);
		TestTrue(*FString::Printf(TEXT("Enum value %f should report a warning"), InvalidValue), Warnings.Num() > 0);
		TestTrue(
			*FString::Printf(TEXT("Enum value %f warning should list valid values"), InvalidValue),
			CortexSerializerTypeSafetyTest::AnyWarningMentions(Warnings, TEXT("Valid")));
		TestTrue(
			TEXT("Level should stay Low after a refused write"),
			Object->Level == ECortexSerializerSparseEnum::Low);
	}

	for (const double InvalidValue : InvalidEnumValues)
	{
		TArray<FString> Warnings;
		const bool bWritten = FCortexSerializer::JsonToProperty(
			MakeShared<FJsonValueNumber>(InvalidValue),
			ByteLevelProperty,
			ByteLevelProperty->ContainerPtrToValuePtr<void>(Object),
			Object,
			Warnings);

		TestFalse(*FString::Printf(TEXT("Enum value %f should be refused for ByteLevel"), InvalidValue), bWritten);
		TestTrue(*FString::Printf(TEXT("Enum value %f should report a warning"), InvalidValue), Warnings.Num() > 0);
		TestTrue(
			TEXT("ByteLevel should stay CSSBE_First after a refused write"),
			Object->ByteLevel == CSSBE_First);
	}

	// Declared numeric values keep working.
	TArray<FString> Warnings;
	const bool bLevelWritten = FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueNumber>(4.0),
		LevelProperty,
		LevelProperty->ContainerPtrToValuePtr<void>(Object),
		Object,
		Warnings);
	TestTrue(TEXT("Declared enum value 4 should write Level"), bLevelWritten);
	TestTrue(TEXT("Level should be High"), Object->Level == ECortexSerializerSparseEnum::High);

	const bool bByteLevelWritten = FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueNumber>(20.0),
		ByteLevelProperty,
		ByteLevelProperty->ContainerPtrToValuePtr<void>(Object),
		Object,
		Warnings);
	TestTrue(TEXT("Declared enum value 20 should write ByteLevel"), bByteLevelWritten);
	TestTrue(TEXT("ByteLevel should be CSSBE_Second"), Object->ByteLevel == CSSBE_Second);
	TestEqual(TEXT("Declared enum values should not warn"), Warnings.Num(), 0);

	Object->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: A map with an unparsable key is refused and the map is unchanged
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerTypeSafetyMapKeyTest,
	"Cortex.Core.Serializer.TypeSafety.MapRejectsInvalidKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerTypeSafetyMapKeyTest::RunTest(const FString& Parameters)
{
	UCortexSerializerTypeSafetyTestObject* Object = NewObject<UCortexSerializerTypeSafetyTestObject>(
		GetTransientPackage(), NAME_None, RF_Transient);

	Object->IntKeyedMap.Add(1, 10);
	Object->IntKeyedMap.Add(2, 20);

	const FProperty* MapProperty = UCortexSerializerTypeSafetyTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, IntKeyedMap));
	TestNotNull(TEXT("IntKeyedMap property should exist"), MapProperty);
	if (MapProperty == nullptr)
	{
		Object->MarkAsGarbage();
		return true;
	}

	TSharedPtr<FJsonObject> MapJson = MakeShared<FJsonObject>();
	MapJson->SetNumberField(TEXT("abc"), 7.0);

	TArray<FString> Warnings;
	const bool bWritten = FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueObject>(MapJson),
		MapProperty,
		MapProperty->ContainerPtrToValuePtr<void>(Object),
		Object,
		Warnings);

	TestFalse(TEXT("Unparsable map key should be refused"), bWritten);
	TestTrue(TEXT("Unparsable map key should report a warning"), Warnings.Num() > 0);
	TestEqual(TEXT("Map should keep its original size"), Object->IntKeyedMap.Num(), 2);
	TestEqual(TEXT("Map key 1 should keep value 10"), Object->IntKeyedMap.FindRef(1), 10);
	TestEqual(TEXT("Map key 2 should keep value 20"), Object->IntKeyedMap.FindRef(2), 20);
	TestFalse(TEXT("No fabricated key 0 should be added"), Object->IntKeyedMap.Contains(0));

	Object->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: A map with a failing value conversion is refused and the map is unchanged
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerTypeSafetyMapValueTest,
	"Cortex.Core.Serializer.TypeSafety.MapRejectsInvalidValue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerTypeSafetyMapValueTest::RunTest(const FString& Parameters)
{
	AddExpectedError(
		CortexSerializerTypeSafetyTest::JsonTypeMismatchLog,
		EAutomationExpectedErrorFlags::Contains,
		-1);

	UCortexSerializerTypeSafetyTestObject* Object = NewObject<UCortexSerializerTypeSafetyTestObject>(
		GetTransientPackage(), NAME_None, RF_Transient);

	Object->StringKeyedMap.Add(TEXT("keep"), 1);

	const FProperty* MapProperty = UCortexSerializerTypeSafetyTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, StringKeyedMap));
	TestNotNull(TEXT("StringKeyedMap property should exist"), MapProperty);
	if (MapProperty == nullptr)
	{
		Object->MarkAsGarbage();
		return true;
	}

	TSharedPtr<FJsonObject> MapJson = MakeShared<FJsonObject>();
	MapJson->SetObjectField(TEXT("new"), MakeShared<FJsonObject>());

	TArray<FString> Warnings;
	const bool bWritten = FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueObject>(MapJson),
		MapProperty,
		MapProperty->ContainerPtrToValuePtr<void>(Object),
		Object,
		Warnings);

	TestFalse(TEXT("Failing map value should be refused"), bWritten);
	TestTrue(TEXT("Failing map value should report a warning"), Warnings.Num() > 0);
	TestEqual(TEXT("Map should keep its original size"), Object->StringKeyedMap.Num(), 1);
	TestEqual(TEXT("Original entry should keep value 1"), Object->StringKeyedMap.FindRef(TEXT("keep")), 1);
	TestFalse(TEXT("No partially converted entry should be added"), Object->StringKeyedMap.Contains(TEXT("new")));

	Object->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: A mutually referencing instanced pair serializes to a finite result
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerTypeSafetyInstancedCycleTest,
	"Cortex.Core.Serializer.TypeSafety.InstancedCycleTerminates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerTypeSafetyInstancedCycleTest::RunTest(const FString& Parameters)
{
	UCortexSerializerCycleNode* NodeA = NewObject<UCortexSerializerCycleNode>(
		GetTransientPackage(), NAME_None, RF_Transient);
	UCortexSerializerCycleNode* NodeB = NewObject<UCortexSerializerCycleNode>(
		NodeA, NAME_None, RF_Transient);

	// UPanelSlot::Content and UWidget::Slot form the same Instanced shape as these two objects.
	NodeA->Next = NodeB;
	NodeB->Next = NodeA;

	const FProperty* NextProperty = UCortexSerializerCycleNode::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerCycleNode, Next));
	TestNotNull(TEXT("Next property should exist"), NextProperty);
	if (NextProperty == nullptr)
	{
		NodeB->MarkAsGarbage();
		NodeA->MarkAsGarbage();
		return true;
	}

	const TSharedPtr<FJsonValue> Serialized = FCortexSerializer::PropertyToJson(
		NextProperty, NextProperty->ContainerPtrToValuePtr<void>(NodeA));
	TestTrue(TEXT("Cyclic instanced graph should serialize"), Serialized.IsValid());

	// The first node must still be expanded, not immediately degraded to a path string.
	const TSharedPtr<FJsonObject>* RootObject = nullptr;
	const bool bRootIsObject = Serialized.IsValid() && Serialized->TryGetObject(RootObject) && RootObject != nullptr;
	TestTrue(TEXT("Referenced node should expand to an object"), bRootIsObject);
	if (bRootIsObject)
	{
		FString RootClass;
		const bool bHasClass = (*RootObject)->TryGetStringField(TEXT("_class"), RootClass);
		TestTrue(TEXT("Expanded node should carry _class"), bHasClass);
		TestEqual(
			TEXT("Expanded node should be the referenced class"),
			RootClass,
			UCortexSerializerCycleNode::StaticClass()->GetName());
	}

	TArray<FString> CyclePaths;
	CyclePaths.Add(NodeA->GetPathName());
	CyclePaths.Add(NodeB->GetPathName());

	const int32 CycleDepth = CortexSerializerTypeSafetyTest::FindCyclePathDepth(Serialized, CyclePaths, 0, 32);
	TestTrue(
		TEXT("The cycle should be emitted as an object path string"),
		CycleDepth != INDEX_NONE);
	TestTrue(
		TEXT("Instanced expansion should be bounded by the serialization depth limit"),
		CycleDepth != INDEX_NONE && CycleDepth <= FCortexSerializationPolicy().MaxDepth);

	NodeB->MarkAsGarbage();
	NodeA->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: A second instance of the same class still expands (cycle tracking is per object)
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerTypeSafetyInstancedSameClassTest,
	"Cortex.Core.Serializer.TypeSafety.InstancedSameClassExpands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerTypeSafetyInstancedSameClassTest::RunTest(const FString& Parameters)
{
	UCortexSerializerCycleNode* NodeA = NewObject<UCortexSerializerCycleNode>(
		GetTransientPackage(), NAME_None, RF_Transient);
	UCortexSerializerCycleNode* NodeB = NewObject<UCortexSerializerCycleNode>(
		NodeA, NAME_None, RF_Transient);
	UCortexSerializerCycleNode* NodeC = NewObject<UCortexSerializerCycleNode>(
		NodeB, NAME_None, RF_Transient);

	// Finite chain A -> B -> C. Cycle tracking keyed on the class instead of the object would
	// wrongly degrade C (same class as B) to a path string.
	NodeA->Next = NodeB;
	NodeB->Next = NodeC;
	NodeC->Next = nullptr;

	const FProperty* NextProperty = UCortexSerializerCycleNode::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerCycleNode, Next));
	TestNotNull(TEXT("Next property should exist"), NextProperty);
	if (NextProperty == nullptr)
	{
		NodeC->MarkAsGarbage();
		NodeB->MarkAsGarbage();
		NodeA->MarkAsGarbage();
		return true;
	}

	const TSharedPtr<FJsonValue> Serialized = FCortexSerializer::PropertyToJson(
		NextProperty, NextProperty->ContainerPtrToValuePtr<void>(NodeA));

	const TSharedPtr<FJsonObject>* RootObject = nullptr;
	const bool bRootIsObject = Serialized.IsValid() && Serialized->TryGetObject(RootObject) && RootObject != nullptr;
	TestTrue(TEXT("First node should expand to an object"), bRootIsObject);

	if (bRootIsObject)
	{
		const TSharedPtr<FJsonObject>* RootProperties = nullptr;
		const bool bHasProperties = (*RootObject)->TryGetObjectField(TEXT("properties"), RootProperties)
			&& RootProperties != nullptr;
		TestTrue(TEXT("Expanded first node should expose its properties"), bHasProperties);

		if (bHasProperties)
		{
			const TSharedPtr<FJsonValue> InnerValue = (*RootProperties)->TryGetField(TEXT("Next"));
			TestTrue(
				TEXT("Second instance of the same class should still expand to an object"),
				InnerValue.IsValid() && InnerValue->Type == EJson::Object);

			const TSharedPtr<FJsonObject>* InnerObject = nullptr;
			if (InnerValue.IsValid() && InnerValue->TryGetObject(InnerObject) && InnerObject != nullptr)
			{
				FString InnerClass;
				const bool bHasClass = (*InnerObject)->TryGetStringField(TEXT("_class"), InnerClass);
				TestTrue(TEXT("Expanded second instance should carry _class"), bHasClass);
				TestEqual(
					TEXT("Expanded second instance should be the node class"),
					InnerClass,
					UCortexSerializerCycleNode::StaticClass()->GetName());

				const TSharedPtr<FJsonObject>* InnerProperties = nullptr;
				const bool bInnerHasProperties = (*InnerObject)->TryGetObjectField(TEXT("properties"), InnerProperties)
					&& InnerProperties != nullptr;
				TestTrue(TEXT("Expanded second instance should expose its properties"), bInnerHasProperties);
				const TSharedPtr<FJsonValue> Terminator = bInnerHasProperties
					? (*InnerProperties)->TryGetField(TEXT("Next"))
					: TSharedPtr<FJsonValue>();
				TestTrue(
					TEXT("A null instanced reference should serialize as null"),
					Terminator.IsValid() && Terminator->Type == EJson::Null);
			}
		}
	}

	NodeC->MarkAsGarbage();
	NodeB->MarkAsGarbage();
	NodeA->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: Reference-like properties refuse mismatched JSON types
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerTypeSafetyReferenceTest,
	"Cortex.Core.Serializer.TypeSafety.ReferenceMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerTypeSafetyReferenceTest::RunTest(const FString& Parameters)
{
	// The current path reads these values through AsString, which logs the mismatch before clearing.
	AddExpectedError(
		CortexSerializerTypeSafetyTest::JsonTypeMismatchLog,
		EAutomationExpectedErrorFlags::Contains,
		-1);

	UCortexSerializerTypeSafetyTestObject* Object = NewObject<UCortexSerializerTypeSafetyTestObject>(
		GetTransientPackage(), NAME_None, RF_Transient);

	// Non-default sentinels so a silent clear is visible.
	const FGameplayTag TagSentinel =
		CortexSerializerTypeSafetyTest::RequestTestGameplayTag(TEXT("Cortex.Test.Serializer.ContainerA"));
	TestTrue(TEXT("Test gameplay tag should be registered"), TagSentinel.IsValid());
	Object->GameplayTagValue = TagSentinel;
	Object->SoftPathValue = FSoftObjectPath(TEXT("/Game/CortexTest/Sentinel.Sentinel"));
	Object->ObjectReference = GetTransientPackage();
	Object->SoftObjectReference = TSoftObjectPtr<UObject>(FSoftObjectPath(TEXT("/Game/CortexTest/Sentinel.Sentinel")));

	struct FCase
	{
		FName PropertyName;
		FString Label;
		TSharedPtr<FJsonValue> Value;
		FString ReceivedTypeToken;
	};

	const TSharedPtr<FJsonValue> EmptyObject = MakeShared<FJsonValueObject>(MakeShared<FJsonObject>());

	TArray<FCase> Cases;
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, GameplayTagValue),
		TEXT("number into FGameplayTag"),
		MakeShared<FJsonValueNumber>(5.0),
		TEXT("number")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, GameplayTagValue),
		TEXT("object into FGameplayTag"),
		EmptyObject,
		TEXT("object")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, GameplayTagValue),
		TEXT("bool into FGameplayTag"),
		MakeShared<FJsonValueBoolean>(true),
		TEXT("boolean")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, SoftPathValue),
		TEXT("number into FSoftObjectPath"),
		MakeShared<FJsonValueNumber>(5.0),
		TEXT("number")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, SoftPathValue),
		TEXT("object into FSoftObjectPath"),
		EmptyObject,
		TEXT("object")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, SoftPathValue),
		TEXT("bool into FSoftObjectPath"),
		MakeShared<FJsonValueBoolean>(true),
		TEXT("boolean")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, ObjectReference),
		TEXT("number into object reference"),
		MakeShared<FJsonValueNumber>(5.0),
		TEXT("number")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, ObjectReference),
		TEXT("object into object reference"),
		EmptyObject,
		TEXT("object")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, ObjectReference),
		TEXT("bool into object reference"),
		MakeShared<FJsonValueBoolean>(true),
		TEXT("boolean")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, SoftObjectReference),
		TEXT("number into soft object reference"),
		MakeShared<FJsonValueNumber>(5.0),
		TEXT("number")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, SoftObjectReference),
		TEXT("object into soft object reference"),
		EmptyObject,
		TEXT("object")});
	Cases.Add(FCase{
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, SoftObjectReference),
		TEXT("bool into soft object reference"),
		MakeShared<FJsonValueBoolean>(true),
		TEXT("boolean")});

	for (const FCase& Case : Cases)
	{
		const FProperty* Property = UCortexSerializerTypeSafetyTestObject::StaticClass()->FindPropertyByName(Case.PropertyName);
		TestNotNull(*FString::Printf(TEXT("%s: property should exist"), *Case.Label), Property);
		if (Property == nullptr)
		{
			continue;
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
		const FString Before = CortexSerializerTypeSafetyTest::ExportValueText(Property, ValuePtr);

		TArray<FString> Warnings;
		const bool bWritten = FCortexSerializer::JsonToProperty(Case.Value, Property, ValuePtr, Object, Warnings);

		TestFalse(*FString::Printf(TEXT("%s should be refused"), *Case.Label), bWritten);
		TestTrue(*FString::Printf(TEXT("%s should report a warning"), *Case.Label), Warnings.Num() > 0);
		TestTrue(
			*FString::Printf(TEXT("%s warning should name the property '%s'"), *Case.Label, *Property->GetName()),
			CortexSerializerTypeSafetyTest::AnyWarningMentions(Warnings, Property->GetName()));
		TestTrue(
			*FString::Printf(TEXT("%s warning should name the received JSON type '%s'"), *Case.Label, *Case.ReceivedTypeToken),
			CortexSerializerTypeSafetyTest::AnyWarningMentions(Warnings, Case.ReceivedTypeToken));
		TestEqual(
			*FString::Printf(TEXT("%s should leave the value unchanged"), *Case.Label),
			CortexSerializerTypeSafetyTest::ExportValueText(Property, ValuePtr),
			Before);
	}

	const FString TransientPath = GetTransientPackage()->GetPathName();

	// A resolvable object path still applies, and JSON null still clears a hard object reference.
	if (const FProperty* ObjectProperty = UCortexSerializerTypeSafetyTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, ObjectReference)))
	{
		void* ObjectValuePtr = ObjectProperty->ContainerPtrToValuePtr<void>(Object);

		TArray<FString> Warnings;
		const bool bPathWritten = FCortexSerializer::JsonToProperty(
			MakeShared<FJsonValueString>(TransientPath), ObjectProperty, ObjectValuePtr, Object, Warnings);
		TestTrue(TEXT("Object reference accepts a resolvable path string"), bPathWritten);
		TestEqual(
			TEXT("Object reference resolves to the loaded object"),
			Object->ObjectReference.Get(),
			static_cast<UObject*>(GetTransientPackage()));
		TestEqual(TEXT("Valid object path should not warn"), Warnings.Num(), 0);

		Warnings.Empty();
		const bool bCleared = FCortexSerializer::JsonToProperty(
			MakeShared<FJsonValueNull>(), ObjectProperty, ObjectValuePtr, Object, Warnings);
		TestTrue(TEXT("JSON null clears an object reference"), bCleared);
		TestNull(TEXT("Object reference is null after the clear"), Object->ObjectReference.Get());
	}

	// A resolvable soft object path still applies.
	if (const FProperty* SoftObjectProperty = UCortexSerializerTypeSafetyTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, SoftObjectReference)))
	{
		TArray<FString> Warnings;
		const bool bWritten = FCortexSerializer::JsonToProperty(
			MakeShared<FJsonValueString>(TransientPath),
			SoftObjectProperty,
			SoftObjectProperty->ContainerPtrToValuePtr<void>(Object),
			Object,
			Warnings);
		TestTrue(TEXT("Soft object reference accepts a resolvable path string"), bWritten);
		TestTrue(
			TEXT("Soft object reference stores the path"),
			Object->SoftObjectReference.ToSoftObjectPath().ToString().Contains(TEXT("Transient")));
		TestEqual(TEXT("Valid soft object path should not warn"), Warnings.Num(), 0);
	}

	// A soft object path string still applies.
	if (const FProperty* SoftPathProperty = UCortexSerializerTypeSafetyTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, SoftPathValue)))
	{
		TArray<FString> Warnings;
		const bool bWritten = FCortexSerializer::JsonToProperty(
			MakeShared<FJsonValueString>(TEXT("/Game/CortexTest/Whatever.Whatever")),
			SoftPathProperty,
			SoftPathProperty->ContainerPtrToValuePtr<void>(Object),
			Object,
			Warnings);
		TestTrue(TEXT("Soft object path accepts a path string"), bWritten);
		TestTrue(
			TEXT("Soft object path stores the path"),
			Object->SoftPathValue.ToString().Contains(TEXT("Whatever")));
	}

	// A tag name still applies, and an empty string still clears the tag.
	if (const FProperty* TagProperty = UCortexSerializerTypeSafetyTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, GameplayTagValue)))
	{
		void* TagValuePtr = TagProperty->ContainerPtrToValuePtr<void>(Object);

		TArray<FString> Warnings;
		const bool bCleared = FCortexSerializer::JsonToProperty(
			MakeShared<FJsonValueString>(TEXT("")), TagProperty, TagValuePtr, Object, Warnings);
		TestTrue(TEXT("Empty tag string clears the gameplay tag"), bCleared);
		TestFalse(TEXT("Gameplay tag is invalid after the clear"), Object->GameplayTagValue.IsValid());

		// Native tags register with the tag manager on module load, so a tag name write always resolves.
		Warnings.Empty();
		const bool bWritten = FCortexSerializer::JsonToProperty(
			MakeShared<FJsonValueString>(TEXT("Cortex.Test.Serializer.ContainerA")),
			TagProperty,
			TagValuePtr,
			Object,
			Warnings);
		TestTrue(TEXT("Gameplay tag accepts a registered tag name"), bWritten);
		TestTrue(TEXT("Gameplay tag equals the registered tag"), Object->GameplayTagValue == TagSentinel);
		TestEqual(TEXT("Valid tag name should not warn"), Warnings.Num(), 0);
	}

	Object->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: A failing tag element leaves the previous container contents intact
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerTypeSafetyTagContainerTest,
	"Cortex.Core.Serializer.TypeSafety.GameplayTagContainerAllOrNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerTypeSafetyTagContainerTest::RunTest(const FString& Parameters)
{
	// The test tags are registered at runtime by the tag manager, so no registry skip guard is needed.
	const FGameplayTag AddedTag =
		CortexSerializerTypeSafetyTest::RequestTestGameplayTag(TEXT("Cortex.Test.Serializer.ContainerA"));
	const FGameplayTag ExistingTag =
		CortexSerializerTypeSafetyTest::RequestTestGameplayTag(TEXT("Cortex.Test.Serializer.ContainerB"));
	TestTrue(TEXT("Test gameplay tags should be registered"), AddedTag.IsValid() && ExistingTag.IsValid());

	UCortexSerializerTypeSafetyTestObject* Object = NewObject<UCortexSerializerTypeSafetyTestObject>(
		GetTransientPackage(), NAME_None, RF_Transient);

	// The sentinel tag differs from the tag the failing writes try to convert, so losing the previous
	// contents is visible even when the first element of the write is valid.
	Object->GameplayTagContainerValue.AddTag(ExistingTag);

	const FProperty* ContainerProperty = UCortexSerializerTypeSafetyTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, GameplayTagContainerValue));
	TestNotNull(TEXT("GameplayTagContainerValue property should exist"), ContainerProperty);
	if (ContainerProperty == nullptr)
	{
		Object->MarkAsGarbage();
		return true;
	}

	void* ValuePtr = ContainerProperty->ContainerPtrToValuePtr<void>(Object);

	struct FCase
	{
		FString Label;
		TSharedPtr<FJsonValue> Element;
	};

	TArray<FCase> Cases;
	Cases.Add(FCase{TEXT("number element"), MakeShared<FJsonValueNumber>(5.0)});
	Cases.Add(FCase{TEXT("unregistered tag element"), MakeShared<FJsonValueString>(TEXT("Not.A.Registered.Tag"))});

	for (const FCase& Case : Cases)
	{
		TArray<TSharedPtr<FJsonValue>> Elements;
		Elements.Add(MakeShared<FJsonValueString>(TEXT("Cortex.Test.Serializer.ContainerA")));
		Elements.Add(Case.Element);

		TArray<FString> Warnings;
		const bool bWritten = FCortexSerializer::JsonToProperty(
			MakeShared<FJsonValueArray>(Elements), ContainerProperty, ValuePtr, Object, Warnings);

		TestFalse(
			*FString::Printf(TEXT("A container write with a %s should be refused"), *Case.Label),
			bWritten);
		TestTrue(
			*FString::Printf(TEXT("A container write with a %s should report a warning"), *Case.Label),
			Warnings.Num() > 0);
		TestEqual(
			*FString::Printf(TEXT("The %s refusal should keep the container size"), *Case.Label),
			Object->GameplayTagContainerValue.Num(),
			1);
		TestTrue(
			*FString::Printf(TEXT("The %s refusal should keep the original tag"), *Case.Label),
			Object->GameplayTagContainerValue.HasTagExact(ExistingTag));
		TestFalse(
			*FString::Printf(TEXT("The %s refusal should not install the converted tag"), *Case.Label),
			Object->GameplayTagContainerValue.HasTagExact(AddedTag));
	}

	// A fully valid write still replaces the container.
	TArray<TSharedPtr<FJsonValue>> ValidElements;
	ValidElements.Add(MakeShared<FJsonValueString>(TEXT("Cortex.Test.Serializer.ContainerA")));

	TArray<FString> Warnings;
	const bool bValidWritten = FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueArray>(ValidElements), ContainerProperty, ValuePtr, Object, Warnings);
	TestTrue(TEXT("A fully valid tag array should be written"), bValidWritten);
	TestEqual(TEXT("Valid write leaves exactly one tag"), Object->GameplayTagContainerValue.Num(), 1);
	TestTrue(TEXT("Valid write stores the new tag"), Object->GameplayTagContainerValue.HasTagExact(AddedTag));
	TestFalse(TEXT("Valid write replaces the previous tag"), Object->GameplayTagContainerValue.HasTagExact(ExistingTag));
	TestEqual(TEXT("Valid tag array should not warn"), Warnings.Num(), 0);

	Object->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: Sparse struct-array writes keep omitted fields of retained elements
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerTypeSafetyArraySparseTest,
	"Cortex.Core.Serializer.TypeSafety.ArraySparseStructUpdatePreservesFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerTypeSafetyArraySparseTest::RunTest(const FString& Parameters)
{
	UCortexSerializerTypeSafetyTestObject* Object = NewObject<UCortexSerializerTypeSafetyTestObject>(
		GetTransientPackage(), NAME_None, RF_Transient);

	Object->StructArray.SetNum(2);
	Object->StructArray[0].RetainedField = TEXT("keep0");
	Object->StructArray[0].UpdatedField = TEXT("old0");
	Object->StructArray[1].RetainedField = TEXT("keep1");
	Object->StructArray[1].UpdatedField = TEXT("old1");

	const FProperty* ArrayProperty = UCortexSerializerTypeSafetyTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, StructArray));
	TestNotNull(TEXT("StructArray property should exist"), ArrayProperty);
	if (ArrayProperty == nullptr)
	{
		Object->MarkAsGarbage();
		return true;
	}

	void* ValuePtr = ArrayProperty->ContainerPtrToValuePtr<void>(Object);

	auto MakeElement = [](const TCHAR* RetainedField, const TCHAR* UpdatedField) -> TSharedPtr<FJsonValue>
	{
		TSharedPtr<FJsonObject> Element = MakeShared<FJsonObject>();
		if (RetainedField != nullptr)
		{
			Element->SetStringField(TEXT("RetainedField"), RetainedField);
		}
		if (UpdatedField != nullptr)
		{
			Element->SetStringField(TEXT("UpdatedField"), UpdatedField);
		}
		return MakeShared<FJsonValueObject>(Element);
	};

	TArray<FString> Warnings;

	// Same element count: only the updated field is provided, the retained field must survive.
	TArray<TSharedPtr<FJsonValue>> SparseElements;
	SparseElements.Add(MakeElement(nullptr, TEXT("new0")));
	SparseElements.Add(MakeElement(nullptr, TEXT("new1")));
	const bool bSparseWritten = FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueArray>(SparseElements), ArrayProperty, ValuePtr, Object, Warnings);
	TestTrue(TEXT("Sparse struct array write should succeed"), bSparseWritten);
	TestEqual(TEXT("Sparse write keeps the element count"), Object->StructArray.Num(), 2);
	if (Object->StructArray.Num() == 2)
	{
		TestEqual(TEXT("Element 0 keeps its omitted field"), Object->StructArray[0].RetainedField, TEXT("keep0"));
		TestEqual(TEXT("Element 0 applies the provided field"), Object->StructArray[0].UpdatedField, TEXT("new0"));
		TestEqual(TEXT("Element 1 keeps its omitted field"), Object->StructArray[1].RetainedField, TEXT("keep1"));
		TestEqual(TEXT("Element 1 applies the provided field"), Object->StructArray[1].UpdatedField, TEXT("new1"));
	}

	// Growth: retained elements keep their fields, the new element starts from defaults.
	TArray<TSharedPtr<FJsonValue>> GrownElements;
	GrownElements.Add(MakeElement(nullptr, TEXT("new0")));
	GrownElements.Add(MakeElement(nullptr, TEXT("new1")));
	GrownElements.Add(MakeElement(TEXT("added"), TEXT("added")));
	Warnings.Empty();
	const bool bGrown = FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueArray>(GrownElements), ArrayProperty, ValuePtr, Object, Warnings);
	TestTrue(TEXT("Growing the struct array should succeed"), bGrown);
	TestEqual(TEXT("Grown array has three elements"), Object->StructArray.Num(), 3);
	if (Object->StructArray.Num() == 3)
	{
		TestEqual(TEXT("Grown element 0 keeps its omitted field"), Object->StructArray[0].RetainedField, TEXT("keep0"));
		TestEqual(TEXT("Grown element 1 keeps its omitted field"), Object->StructArray[1].RetainedField, TEXT("keep1"));
		TestEqual(TEXT("Grown element 2 applies the provided field"), Object->StructArray[2].RetainedField, TEXT("added"));
	}

	// Shrink: dropped elements disappear and the retained element keeps its fields.
	TArray<TSharedPtr<FJsonValue>> ShrunkElements;
	ShrunkElements.Add(MakeElement(nullptr, TEXT("final")));
	Warnings.Empty();
	const bool bShrunk = FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueArray>(ShrunkElements), ArrayProperty, ValuePtr, Object, Warnings);
	TestTrue(TEXT("Shrinking the struct array should succeed"), bShrunk);
	TestEqual(TEXT("Shrunk array has one element"), Object->StructArray.Num(), 1);
	if (Object->StructArray.Num() == 1)
	{
		TestEqual(TEXT("Shrunk element keeps its omitted field"), Object->StructArray[0].RetainedField, TEXT("keep0"));
		TestEqual(TEXT("Shrunk element applies the provided field"), Object->StructArray[0].UpdatedField, TEXT("final"));
	}
	TestEqual(TEXT("Struct array writes should not warn"), Warnings.Num(), 0);

	Object->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: Fractional numeric JSON values truncate instead of rounding
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerTypeSafetyFractionalTest,
	"Cortex.Core.Serializer.TypeSafety.FractionalNumberTruncates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerTypeSafetyFractionalTest::RunTest(const FString& Parameters)
{
	UCortexSerializerTypeSafetyTestObject* Object = NewObject<UCortexSerializerTypeSafetyTestObject>(
		GetTransientPackage(), NAME_None, RF_Transient);

	const FProperty* IntProperty = UCortexSerializerTypeSafetyTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, IntValue));
	const FProperty* Int64Property = UCortexSerializerTypeSafetyTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, Int64Value));
	const FProperty* ByteProperty = UCortexSerializerTypeSafetyTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, ByteValue));
	TestNotNull(TEXT("IntValue property should exist"), IntProperty);
	TestNotNull(TEXT("Int64Value property should exist"), Int64Property);
	TestNotNull(TEXT("ByteValue property should exist"), ByteProperty);
	if (IntProperty == nullptr || Int64Property == nullptr || ByteProperty == nullptr)
	{
		Object->MarkAsGarbage();
		return true;
	}

	void* IntValuePtr = IntProperty->ContainerPtrToValuePtr<void>(Object);
	void* Int64ValuePtr = Int64Property->ContainerPtrToValuePtr<void>(Object);
	void* ByteValuePtr = ByteProperty->ContainerPtrToValuePtr<void>(Object);

	TArray<FString> Warnings;

	TestTrue(TEXT("1.6 should write an int32"), FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueNumber>(1.6), IntProperty, IntValuePtr, Object, Warnings));
	TestEqual(TEXT("int32 truncates 1.6 to 1"), Object->IntValue, 1);

	TestTrue(TEXT("-1.6 should write an int32"), FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueNumber>(-1.6), IntProperty, IntValuePtr, Object, Warnings));
	TestEqual(TEXT("int32 truncates -1.6 to -1"), Object->IntValue, -1);

	TestTrue(TEXT("1.6 should write an int64"), FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueNumber>(1.6), Int64Property, Int64ValuePtr, Object, Warnings));
	TestEqual(TEXT("int64 truncates 1.6 to 1"), Object->Int64Value, static_cast<int64>(1));

	TestTrue(TEXT("-1.6 should write an int64"), FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueNumber>(-1.6), Int64Property, Int64ValuePtr, Object, Warnings));
	TestEqual(TEXT("int64 truncates -1.6 to -1"), Object->Int64Value, static_cast<int64>(-1));

	TestTrue(TEXT("2.9 should write a byte"), FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueNumber>(2.9), ByteProperty, ByteValuePtr, Object, Warnings));
	TestEqual(TEXT("byte truncates 2.9 to 2"), static_cast<int32>(Object->ByteValue), 2);

	// A numeric string already truncates through the lexer; a JSON number must agree with it.
	TestTrue(TEXT("numeric string 1.6 should write an int32"), FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueString>(TEXT("1.6")), IntProperty, IntValuePtr, Object, Warnings));
	TestEqual(TEXT("int32 truncates the numeric string 1.6 to 1"), Object->IntValue, 1);
	TestEqual(TEXT("Truncating numeric writes should not warn"), Warnings.Num(), 0);

	// A fractional numeric enum value must not round into another enumerator: 1.5 would become
	// Medium (2), which is a declared value of ECortexSerializerSparseEnum.
	UCortexSerializerEnumTestObject* EnumObject = NewObject<UCortexSerializerEnumTestObject>(
		GetTransientPackage(), NAME_None, RF_Transient);
	EnumObject->Level = ECortexSerializerSparseEnum::Low;

	const FProperty* LevelProperty = UCortexSerializerEnumTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerEnumTestObject, Level));
	TestNotNull(TEXT("Level property should exist"), LevelProperty);
	if (LevelProperty != nullptr)
	{
		TArray<FString> EnumWarnings;
		const bool bEnumWritten = FCortexSerializer::JsonToProperty(
			MakeShared<FJsonValueNumber>(1.5),
			LevelProperty,
			LevelProperty->ContainerPtrToValuePtr<void>(EnumObject),
			EnumObject,
			EnumWarnings);

		TestFalse(TEXT("A fractional enum value should be refused"), bEnumWritten);
		TestTrue(TEXT("A fractional enum refusal should report a warning"), EnumWarnings.Num() > 0);
		TestTrue(
			TEXT("Level should stay Low after a fractional write"),
			EnumObject->Level == ECortexSerializerSparseEnum::Low);
	}

	EnumObject->MarkAsGarbage();
	Object->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: A staged instanced child that owns an instanced grandchild stays alive
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerTypeSafetyNestedInstancedTest,
	"Cortex.Core.Serializer.TypeSafety.NestedInstancedArrayKeepsChildren",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerTypeSafetyNestedInstancedTest::RunTest(const FString& Parameters)
{
	UCortexSerializerTypeSafetyTestObject* Object = NewObject<UCortexSerializerTypeSafetyTestObject>(
		GetTransientPackage(), NAME_None, RF_Transient);

	const FProperty* ArrayProperty = UCortexSerializerTypeSafetyTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, InstancedChildren));
	const FProperty* MapProperty = UCortexSerializerTypeSafetyTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, InstancedChildMap));
	TestNotNull(TEXT("InstancedChildren property should exist"), ArrayProperty);
	TestNotNull(TEXT("InstancedChildMap property should exist"), MapProperty);
	if (ArrayProperty == nullptr || MapProperty == nullptr)
	{
		Object->MarkAsGarbage();
		return true;
	}

	// One node whose instanced Next is another node created by the same conversion. This is the
	// UPanelWidget::Slots -> UPanelSlot::Content shape: the parent owns a child it created itself.
	TSharedPtr<FJsonObject> NestedNode = MakeShared<FJsonObject>();
	NestedNode->SetStringField(TEXT("_class"), TEXT("CortexSerializerCycleNode"));

	TSharedPtr<FJsonObject> ParentProperties = MakeShared<FJsonObject>();
	ParentProperties->SetObjectField(TEXT("Next"), NestedNode);

	TSharedPtr<FJsonObject> ParentNode = MakeShared<FJsonObject>();
	ParentNode->SetStringField(TEXT("_class"), TEXT("CortexSerializerCycleNode"));
	ParentNode->SetObjectField(TEXT("properties"), ParentProperties);

	TArray<FString> Warnings;

	TArray<TSharedPtr<FJsonValue>> Children;
	Children.Add(MakeShared<FJsonValueObject>(ParentNode));
	const bool bArrayWritten = FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueArray>(Children),
		ArrayProperty,
		ArrayProperty->ContainerPtrToValuePtr<void>(Object),
		Object,
		Warnings);

	TestTrue(TEXT("Nested instanced array write should succeed"), bArrayWritten);
	TestEqual(TEXT("The array write creates one child"), Object->InstancedChildren.Num(), 1);
	UCortexSerializerCycleNode* ArrayChild = Object->InstancedChildren.Num() == 1
		? Object->InstancedChildren[0].Get()
		: nullptr;
	TestNotNull(TEXT("The created array child should exist"), ArrayChild);
	if (ArrayChild != nullptr)
	{
		TestFalse(
			TEXT("The created array child must not be retired"),
			ArrayChild->HasAnyInternalFlags(EInternalObjectFlags::Garbage));

		UCortexSerializerCycleNode* ArrayGrandChild = ArrayChild->Next.Get();
		TestNotNull(TEXT("The created array child should own its nested child"), ArrayGrandChild);
		if (ArrayGrandChild != nullptr)
		{
			TestTrue(TEXT("The nested array child is a distinct object"), ArrayGrandChild != ArrayChild);
			TestFalse(
				TEXT("The nested array child must not be retired while its parent references it"),
				ArrayGrandChild->HasAnyInternalFlags(EInternalObjectFlags::Garbage));
		}
	}

	// The same parent/child shape through a map value.
	TSharedPtr<FJsonObject> MapJson = MakeShared<FJsonObject>();
	MapJson->SetObjectField(TEXT("Slot"), ParentNode);

	Warnings.Empty();
	const bool bMapWritten = FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueObject>(MapJson),
		MapProperty,
		MapProperty->ContainerPtrToValuePtr<void>(Object),
		Object,
		Warnings);

	TestTrue(TEXT("Nested instanced map write should succeed"), bMapWritten);
	TestEqual(TEXT("The map write creates one entry"), Object->InstancedChildMap.Num(), 1);
	UCortexSerializerCycleNode* MapChild = Object->InstancedChildMap.FindRef(TEXT("Slot")).Get();
	TestNotNull(TEXT("The created map child should exist"), MapChild);
	if (MapChild != nullptr)
	{
		TestFalse(
			TEXT("The created map child must not be retired"),
			MapChild->HasAnyInternalFlags(EInternalObjectFlags::Garbage));

		UCortexSerializerCycleNode* MapGrandChild = MapChild->Next.Get();
		TestNotNull(TEXT("The created map child should own its nested child"), MapGrandChild);
		if (MapGrandChild != nullptr)
		{
			TestTrue(TEXT("The nested map child is a distinct object"), MapGrandChild != MapChild);
			TestFalse(
				TEXT("The nested map child must not be retired while its parent references it"),
				MapGrandChild->HasAnyInternalFlags(EInternalObjectFlags::Garbage));
		}
	}

	// Cleanup: children before their nested children.
	for (int32 Index = 0; Index < Object->InstancedChildren.Num(); ++Index)
	{
		UCortexSerializerCycleNode* Child = Object->InstancedChildren[Index].Get();
		if (Child != nullptr)
		{
			if (UCortexSerializerCycleNode* GrandChild = Child->Next.Get())
			{
				GrandChild->MarkAsGarbage();
			}
			Child->MarkAsGarbage();
		}
	}

	for (const TPair<FString, TObjectPtr<UCortexSerializerCycleNode>>& Pair : Object->InstancedChildMap)
	{
		UCortexSerializerCycleNode* Child = Pair.Value.Get();
		if (Child != nullptr)
		{
			if (UCortexSerializerCycleNode* GrandChild = Child->Next.Get())
			{
				GrandChild->MarkAsGarbage();
			}
			Child->MarkAsGarbage();
		}
	}

	Object->MarkAsGarbage();
	return true;
}

// ============================================================================
// Test: Replacing an instanced array keeps children referenced from elsewhere
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerTypeSafetyReplacedArrayTest,
	"Cortex.Core.Serializer.TypeSafety.ReplacedArrayKeepsExternallyReferencedChild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerTypeSafetyReplacedArrayTest::RunTest(const FString& Parameters)
{
	UCortexSerializerTypeSafetyTestObject* Object = NewObject<UCortexSerializerTypeSafetyTestObject>(
		GetTransientPackage(), NAME_None, RF_Transient);

	// InstancedChildren = [Child], Child.Next = GrandChild, and the same owner references the grandchild
	// from another property. Replacing the array owns only its direct element.
	UCortexSerializerCycleNode* Child = NewObject<UCortexSerializerCycleNode>(Object, NAME_None, RF_Transient);
	UCortexSerializerCycleNode* GrandChild = NewObject<UCortexSerializerCycleNode>(Object, NAME_None, RF_Transient);
	Child->Next = GrandChild;

	Object->InstancedChildren.Add(Child);
	Object->ObjectReference = GrandChild;

	const FProperty* ArrayProperty = UCortexSerializerTypeSafetyTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerTypeSafetyTestObject, InstancedChildren));
	TestNotNull(TEXT("InstancedChildren property should exist"), ArrayProperty);
	if (ArrayProperty == nullptr)
	{
		GrandChild->MarkAsGarbage();
		Child->MarkAsGarbage();
		Object->MarkAsGarbage();
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> EmptyArray;
	TArray<FString> Warnings;
	const bool bWritten = FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueArray>(EmptyArray),
		ArrayProperty,
		ArrayProperty->ContainerPtrToValuePtr<void>(Object),
		Object,
		Warnings);

	TestTrue(TEXT("Replacing the array with an empty value should succeed"), bWritten);
	TestEqual(TEXT("The array is empty after the write"), Object->InstancedChildren.Num(), 0);
	TestTrue(
		TEXT("The replaced direct instanced element is retired"),
		Child->HasAnyInternalFlags(EInternalObjectFlags::Garbage));
	TestFalse(
		TEXT("The child referenced from another property must not be retired"),
		GrandChild->HasAnyInternalFlags(EInternalObjectFlags::Garbage));
	TestTrue(
		TEXT("The other property still references the surviving child"),
		Object->ObjectReference.Get() == GrandChild);

	GrandChild->MarkAsGarbage();
	Child->MarkAsGarbage();
	Object->MarkAsGarbage();
	return true;
}
