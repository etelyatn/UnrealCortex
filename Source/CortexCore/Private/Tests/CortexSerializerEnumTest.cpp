#include "Misc/AutomationTest.h"
#include "CortexSerializer.h"
#include "CortexSerializerEnumTestTypes.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerEnumReadByValueTest,
	"Cortex.Core.Serializer.Enum.ReadsNameByValue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerEnumReadByValueTest::RunTest(const FString& Parameters)
{
	UCortexSerializerEnumTestObject* Object = NewObject<UCortexSerializerEnumTestObject>();
	Object->Level = ECortexSerializerSparseEnum::High;
	Object->ByteLevel = CSSBE_Second;

	const FProperty* LevelProperty = UCortexSerializerEnumTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerEnumTestObject, Level));
	const FProperty* ByteLevelProperty = UCortexSerializerEnumTestObject::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UCortexSerializerEnumTestObject, ByteLevel));
	TestNotNull(TEXT("Level property should exist"), LevelProperty);
	TestNotNull(TEXT("ByteLevel property should exist"), ByteLevelProperty);
	if (!LevelProperty || !ByteLevelProperty)
	{
		return false;
	}

	// High has value 4 but index 3; reading by index would report index 4 ("Extreme").
	const TSharedPtr<FJsonValue> LevelJson = FCortexSerializer::PropertyToJson(
		LevelProperty, LevelProperty->ContainerPtrToValuePtr<void>(Object));
	TestTrue(TEXT("Level should serialize"), LevelJson.IsValid());
	if (LevelJson.IsValid())
	{
		TestEqual(TEXT("Enum class value should read back by value"), LevelJson->AsString(), TEXT("High"));
	}

	// CSSBE_Second has value 20, far past the last index.
	const TSharedPtr<FJsonValue> ByteLevelJson = FCortexSerializer::PropertyToJson(
		ByteLevelProperty, ByteLevelProperty->ContainerPtrToValuePtr<void>(Object));
	TestTrue(TEXT("ByteLevel should serialize"), ByteLevelJson.IsValid());
	if (ByteLevelJson.IsValid())
	{
		TestEqual(TEXT("Byte enum value should read back by value"), ByteLevelJson->AsString(), TEXT("CSSBE_Second"));
	}

	// The name that was read must write the same value back.
	if (LevelJson.IsValid())
	{
		Object->Level = ECortexSerializerSparseEnum::None;
		TArray<FString> Warnings;
		const bool bWritten = FCortexSerializer::JsonToProperty(
			LevelJson, LevelProperty, LevelProperty->ContainerPtrToValuePtr<void>(Object), Object, Warnings);
		TestTrue(TEXT("Read name should write back"), bWritten);
		TestTrue(TEXT("Round trip should preserve the value"), Object->Level == ECortexSerializerSparseEnum::High);
	}

	return true;
}
