#include "Misc/AutomationTest.h"
#include "CortexSerializer.h"
#include "CortexSerializerTextTestTypes.h"
#include "Dom/JsonValue.h"
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
	TestTable->GetMutableStringTable()->SetSourceString(TEXT("TestKey"), TEXT("Test Value"), TEXT(""));

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerJsonToTextStringTableTest,
	"Cortex.Core.Serializer.JsonToText.StringTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerJsonToTextStringTableTest::RunTest(const FString& Parameters)
{
	UStringTable* TestTable = NewObject<UStringTable>(
		GetTransientPackage(),
		FName(TEXT("TestStringTable_JsonToText")));
	TestTable->GetMutableStringTable()->SetNamespace(TEXT("TestNS"));
	TestTable->GetMutableStringTable()->SetSourceString(TEXT("TestKey"), TEXT("Test Value"), TEXT(""));

	TSharedPtr<FJsonObject> StringTableObject = MakeShared<FJsonObject>();
	StringTableObject->SetStringField(TEXT("table_id"), TestTable->GetStringTableId().ToString());
	StringTableObject->SetStringField(TEXT("key"), TEXT("TestKey"));

	TSharedPtr<FJsonObject> TextObject = MakeShared<FJsonObject>();
	TextObject->SetStringField(TEXT("value"), TEXT("Test Value"));
	TextObject->SetObjectField(TEXT("string_table"), StringTableObject);

	UCortexSerializerTextTestObject* TestObject = NewObject<UCortexSerializerTextTestObject>();
	TArray<FString> Warnings;
	const bool bSuccess = FCortexSerializer::JsonToProperty(
		MakeShared<FJsonValueObject>(TextObject),
		UCortexSerializerTextTestObject::StaticClass()->FindPropertyByName(TEXT("Title")),
		UCortexSerializerTextTestObject::StaticClass()->FindPropertyByName(TEXT("Title"))->ContainerPtrToValuePtr<void>(TestObject),
		TestObject,
		Warnings);

	TestTrue(TEXT("Table-backed FText should deserialize"), bSuccess);
	TestEqual(TEXT("No warnings should be reported"), Warnings.Num(), 0);
	TestEqual(TEXT("Text resolves through table"), TestObject->Title.ToString(), TEXT("Test Value"));

	FName TableId;
	FString Key;
	TestTrue(TEXT("Deserialized text keeps string table metadata"),
		FTextInspector::GetTableIdAndKey(TestObject->Title, TableId, Key));
	TestEqual(TEXT("Table id matches"), TableId, TestTable->GetStringTableId());
	TestEqual(TEXT("Key matches"), Key, TEXT("TestKey"));

	TestObject->MarkAsGarbage();
	TestTable->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerTextDescriptorNormalizeLiteralTest,
	"Cortex.Core.Serializer.TextDescriptor.NormalizeLiteral",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerTextDescriptorNormalizeLiteralTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("type"), TEXT("FText"));
	Input->SetStringField(TEXT("source_kind"), TEXT("literal"));
	Input->SetStringField(TEXT("value"), TEXT("Literal Pay"));

	TArray<FString> Errors;
	TSharedPtr<FJsonObject> Normalized;
	FText Text;
	const bool bOk = FCortexSerializer::NormalizeTextDescriptor(
		MakeShared<FJsonValueObject>(Input),
		Normalized,
		&Text,
		Errors);

	TestTrue(TEXT("literal descriptor normalizes"), bOk);
	TestEqual(TEXT("no errors"), Errors.Num(), 0);
	TestEqual(TEXT("type is FText"), Normalized->GetStringField(TEXT("type")), TEXT("FText"));
	TestEqual(TEXT("source kind is literal"), Normalized->GetStringField(TEXT("source_kind")), TEXT("literal"));
	TestEqual(TEXT("value preserved"), Normalized->GetStringField(TEXT("value")), TEXT("Literal Pay"));
	TestFalse(TEXT("literal has no string table"), Normalized->HasField(TEXT("string_table")));
	TestEqual(TEXT("FText value resolves"), Text.ToString(), TEXT("Literal Pay"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerTextDescriptorNormalizeHistoricalStringTableTest,
	"Cortex.Core.Serializer.TextDescriptor.NormalizeHistoricalStringTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerTextDescriptorNormalizeHistoricalStringTableTest::RunTest(const FString& Parameters)
{
	UStringTable* TestTable = NewObject<UStringTable>(
		GetTransientPackage(),
		FName(TEXT("TestStringTable_NormalizeHistorical")));
	TestTable->GetMutableStringTable()->SetNamespace(TEXT("TestNS"));
	TestTable->GetMutableStringTable()->SetSourceString(TEXT("PayKey"), TEXT("Pay"), TEXT(""));

	TSharedPtr<FJsonObject> StringTable = MakeShared<FJsonObject>();
	StringTable->SetStringField(TEXT("table_id"), TestTable->GetStringTableId().ToString());
	StringTable->SetStringField(TEXT("key"), TEXT("PayKey"));

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("value"), TEXT("Pay"));
	Input->SetObjectField(TEXT("string_table"), StringTable);

	TArray<FString> Errors;
	TSharedPtr<FJsonObject> Normalized;
	FText Text;
	const bool bOk = FCortexSerializer::NormalizeTextDescriptor(
		MakeShared<FJsonValueObject>(Input),
		Normalized,
		&Text,
		Errors);

	TestTrue(TEXT("historical string_table shape normalizes"), bOk);
	TestEqual(TEXT("source kind inferred"), Normalized->GetStringField(TEXT("source_kind")), TEXT("string_table"));
	TestEqual(TEXT("type inferred"), Normalized->GetStringField(TEXT("type")), TEXT("FText"));
	TestEqual(TEXT("value preserved"), Normalized->GetStringField(TEXT("value")), TEXT("Pay"));

	const TSharedPtr<FJsonObject>* NormalizedStringTable = nullptr;
	TestTrue(TEXT("normalized output has string_table"),
		Normalized->TryGetObjectField(TEXT("string_table"), NormalizedStringTable));
	TestEqual(TEXT("key preserved"),
		(*NormalizedStringTable)->GetStringField(TEXT("key")), TEXT("PayKey"));

	FName TableId;
	FString Key;
	TestTrue(TEXT("FText keeps table metadata"), FTextInspector::GetTableIdAndKey(Text, TableId, Key));
	TestEqual(TEXT("table id preserved"), TableId, TestTable->GetStringTableId());
	TestEqual(TEXT("key preserved in FText"), Key, TEXT("PayKey"));

	TestTable->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerTextDescriptorRejectUnsupportedSourceKindTest,
	"Cortex.Core.Serializer.TextDescriptor.RejectUnsupportedSourceKind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerTextDescriptorRejectUnsupportedSourceKindTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("type"), TEXT("FText"));
	Input->SetStringField(TEXT("source_kind"), TEXT("format"));
	Input->SetStringField(TEXT("value"), TEXT("{Count} items"));

	TArray<FString> Errors;
	TSharedPtr<FJsonObject> Normalized;
	const bool bOk = FCortexSerializer::NormalizeTextDescriptor(
		MakeShared<FJsonValueObject>(Input),
		Normalized,
		nullptr,
		Errors);

	TestFalse(TEXT("unsupported format source kind is rejected"), bOk);
	TestTrue(TEXT("error mentions source_kind"),
		Errors.Num() > 0 && Errors[0].Contains(TEXT("source_kind")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexSerializerStructToJsonStringTableTextTest,
	"Cortex.Core.Serializer.JsonToText.StructToJsonStringTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexSerializerStructToJsonStringTableTextTest::RunTest(const FString& Parameters)
{
	UStringTable* TestTable = NewObject<UStringTable>(
		GetTransientPackage(),
		FName(TEXT("TestStringTable_StructToJsonText")));
	TestTable->GetMutableStringTable()->SetNamespace(TEXT("TestNS"));
	TestTable->GetMutableStringTable()->SetSourceString(TEXT("TestKey"), TEXT("Test Value"), TEXT(""));

	UCortexSerializerTextTestObject* TestObject = NewObject<UCortexSerializerTextTestObject>();
	TestObject->Title = FText::FromStringTable(TestTable->GetStringTableId(), TEXT("TestKey"));

	const TSharedPtr<FJsonObject> Result = FCortexSerializer::StructToJson(
		UCortexSerializerTextTestObject::StaticClass(),
		TestObject);

	TestTrue(TEXT("Struct JSON should be valid"), Result.IsValid());
	const TSharedPtr<FJsonObject>* TitleObject = nullptr;
	TestTrue(TEXT("FText property should serialize as object"),
		Result.IsValid() && Result->TryGetObjectField(TEXT("Title"), TitleObject));
	if (TitleObject != nullptr && (*TitleObject).IsValid())
	{
		TestEqual(TEXT("FText object should include type"),
			(*TitleObject)->GetStringField(TEXT("type")), TEXT("FText"));
		TestTrue(TEXT("FText object should include source_kind"),
			(*TitleObject)->HasTypedField<EJson::String>(TEXT("source_kind")));
		TestEqual(TEXT("FText object should include value"),
			(*TitleObject)->GetStringField(TEXT("value")), TEXT("Test Value"));
		TestTrue(TEXT("FText object should include string_table"),
			(*TitleObject)->HasField(TEXT("string_table")));

		const TSharedPtr<FJsonObject>* StringTableObject = nullptr;
		if ((*TitleObject)->TryGetObjectField(TEXT("string_table"), StringTableObject) && StringTableObject != nullptr)
		{
			TestEqual(TEXT("Serialized table id matches"),
				(*StringTableObject)->GetStringField(TEXT("table_id")),
				TestTable->GetStringTableId().ToString());
			TestEqual(TEXT("Serialized key matches"),
				(*StringTableObject)->GetStringField(TEXT("key")),
				TEXT("TestKey"));
		}
	}

	TestObject->MarkAsGarbage();
	TestTable->MarkAsGarbage();
	return true;
}
