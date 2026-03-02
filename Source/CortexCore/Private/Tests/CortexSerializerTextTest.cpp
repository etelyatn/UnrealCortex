#include "Misc/AutomationTest.h"
#include "CortexSerializer.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerTextToJsonLiteralTest,
	"Cortex.Core.Serializer.TextToJson.Literal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerTextToJsonLiteralTest::RunTest(const FString& Parameters)
{
	const FText LiteralText = FText::FromString(TEXT("Hello World"));
	const TSharedPtr<FJsonObject> Result = FCortexSerializer::TextToJson(LiteralText);

	TestTrue(TEXT("Result should be valid"), Result.IsValid());
	if (!Result.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("value should be 'Hello World'"),
		Result->GetStringField(TEXT("value")), TEXT("Hello World"));
	TestFalse(TEXT("Literal text should not include string_table"),
		Result->HasField(TEXT("string_table")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerTextToJsonStringTableTest,
	"Cortex.Core.Serializer.TextToJson.StringTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerTextToJsonStringTableTest::RunTest(const FString& Parameters)
{
	UStringTable* TestTable = NewObject<UStringTable>(
		GetTransientPackage(),
		FName(TEXT("TestStringTable_TextToJson")));
	TestTable->GetMutableStringTable()->SetNamespace(TEXT("TestNS"));
	TestTable->GetMutableStringTable()->SetSourceString(TEXT("TestKey"), TEXT("Test Value"));

	const FText TableText = FText::FromStringTable(TestTable->GetStringTableId(), TEXT("TestKey"));
	const TSharedPtr<FJsonObject> Result = FCortexSerializer::TextToJson(TableText);

	TestTrue(TEXT("Result should be valid"), Result.IsValid());
	if (!Result.IsValid())
	{
		TestTable->MarkAsGarbage();
		return false;
	}

	TestEqual(TEXT("value should be resolved string"),
		Result->GetStringField(TEXT("value")), TEXT("Test Value"));
	TestTrue(TEXT("Table-backed text should include string_table"),
		Result->HasField(TEXT("string_table")));

	const TSharedPtr<FJsonObject>* StringTableObject = nullptr;
	if (Result->TryGetObjectField(TEXT("string_table"), StringTableObject) && StringTableObject)
	{
		TestTrue(TEXT("table_id should include table name"),
			(*StringTableObject)->GetStringField(TEXT("table_id")).Contains(
				TEXT("TestStringTable"), ESearchCase::CaseSensitive));
		TestEqual(TEXT("key should be TestKey"),
			(*StringTableObject)->GetStringField(TEXT("key")), TEXT("TestKey"));
	}

	TestTable->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerTextToJsonEmptyTest,
	"Cortex.Core.Serializer.TextToJson.Empty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerTextToJsonEmptyTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Result = FCortexSerializer::TextToJson(FText::GetEmpty());

	TestTrue(TEXT("Result should be valid"), Result.IsValid());
	if (!Result.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("value should be empty string"),
		Result->GetStringField(TEXT("value")), TEXT(""));
	TestFalse(TEXT("Empty text should not include string_table"),
		Result->HasField(TEXT("string_table")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerFTextEnrichmentPatternTest,
	"Cortex.Core.Serializer.TextToJson.EnrichmentPattern",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerFTextEnrichmentPatternTest::RunTest(const FString& Parameters)
{
	const FText TestText = FText::FromString(TEXT("Enriched Text"));

	TSharedPtr<FJsonObject> PropertyObject = MakeShared<FJsonObject>();
	PropertyObject->SetStringField(TEXT("name"), TEXT("TestProp"));
	PropertyObject->SetStringField(TEXT("type"), TEXT("FText"));
	PropertyObject->SetStringField(TEXT("value"), TestText.ToString());

	const TSharedPtr<FJsonObject> TextJson = FCortexSerializer::TextToJson(TestText);
	if (TextJson->HasField(TEXT("string_table")))
	{
		PropertyObject->SetObjectField(TEXT("string_table"), TextJson->GetObjectField(TEXT("string_table")));
	}

	TestFalse(TEXT("Literal text should not add string_table sibling"),
		PropertyObject->HasField(TEXT("string_table")));
	TestEqual(TEXT("value should stay plain string"),
		PropertyObject->GetStringField(TEXT("value")), TEXT("Enriched Text"));
	return true;
}
