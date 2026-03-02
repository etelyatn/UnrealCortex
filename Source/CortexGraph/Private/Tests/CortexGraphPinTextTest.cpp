#include "Misc/AutomationTest.h"
#include "Operations/CortexGraphNodeOps.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPinTextSerializationTest,
	"Cortex.Graph.Pin.TextSerialization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphPinTextSerializationTest::RunTest(const FString& Parameters)
{
	UEdGraph* TestGraph = NewObject<UEdGraph>(GetTransientPackage());
	TestGraph->Schema = UEdGraphSchema_K2::StaticClass();
	UEdGraphNode* TestNode = NewObject<UEdGraphNode>(TestGraph);
	TestGraph->AddNode(TestNode, false, false);

	UEdGraphPin* TextPin = TestNode->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Text, TEXT("TestTextPin"));
	TextPin->DefaultTextValue = FText::FromString(TEXT("Literal pin text"));

	TSharedRef<FJsonObject> PinJson = FCortexGraphNodeOps::SerializePin(TextPin, true);

	TestTrue(TEXT("Should have default_text_value field"), PinJson->HasField(TEXT("default_text_value")));

	const TSharedPtr<FJsonObject>* TextObject = nullptr;
	if (PinJson->TryGetObjectField(TEXT("default_text_value"), TextObject) && TextObject)
	{
		TestEqual(TEXT("value should match literal text"),
			(*TextObject)->GetStringField(TEXT("value")), TEXT("Literal pin text"));
		TestFalse(TEXT("Literal text should not include string_table"),
			(*TextObject)->HasField(TEXT("string_table")));
	}

	TestNode->DestroyNode();
	TestGraph->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPinTextStringTableTest,
	"Cortex.Graph.Pin.TextStringTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphPinTextStringTableTest::RunTest(const FString& Parameters)
{
	UStringTable* TestTable = NewObject<UStringTable>(
		GetTransientPackage(),
		FName(TEXT("TestStringTable_PinText")));
	TestTable->GetMutableStringTable()->SetNamespace(TEXT("TestNS"));
	TestTable->GetMutableStringTable()->SetSourceString(TEXT("PinKey"), TEXT("Pin Table Value"));

	UEdGraph* TestGraph = NewObject<UEdGraph>(GetTransientPackage());
	TestGraph->Schema = UEdGraphSchema_K2::StaticClass();
	UEdGraphNode* TestNode = NewObject<UEdGraphNode>(TestGraph);
	TestGraph->AddNode(TestNode, false, false);

	UEdGraphPin* TextPin = TestNode->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Text, TEXT("TestTextPin"));
	TextPin->DefaultTextValue = FText::FromStringTable(TestTable->GetStringTableId(), TEXT("PinKey"));

	TSharedRef<FJsonObject> PinJson = FCortexGraphNodeOps::SerializePin(TextPin, true);

	const TSharedPtr<FJsonObject>* TextObject = nullptr;
	if (PinJson->TryGetObjectField(TEXT("default_text_value"), TextObject) && TextObject)
	{
		TestTrue(TEXT("Table-backed text should include string_table"),
			(*TextObject)->HasField(TEXT("string_table")));

		const TSharedPtr<FJsonObject>* StringTableObject = nullptr;
		if ((*TextObject)->TryGetObjectField(TEXT("string_table"), StringTableObject) && StringTableObject)
		{
			TestEqual(TEXT("key should match PinKey"),
				(*StringTableObject)->GetStringField(TEXT("key")), TEXT("PinKey"));
		}
	}

	TestNode->DestroyNode();
	TestGraph->MarkAsGarbage();
	TestTable->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphPinNonDetailedNoTextTest,
	"Cortex.Graph.Pin.NonDetailedNoText",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphPinNonDetailedNoTextTest::RunTest(const FString& Parameters)
{
	UEdGraph* TestGraph = NewObject<UEdGraph>(GetTransientPackage());
	TestGraph->Schema = UEdGraphSchema_K2::StaticClass();
	UEdGraphNode* TestNode = NewObject<UEdGraphNode>(TestGraph);
	TestGraph->AddNode(TestNode, false, false);

	UEdGraphPin* TextPin = TestNode->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Text, TEXT("TestTextPin"));
	TextPin->DefaultTextValue = FText::FromString(TEXT("Some text"));

	TSharedRef<FJsonObject> PinJson = FCortexGraphNodeOps::SerializePin(TextPin, false);

	TestFalse(TEXT("Non-detailed response should not include default_text_value"),
		PinJson->HasField(TEXT("default_text_value")));
	TestFalse(TEXT("Non-detailed response should not include default_value"),
		PinJson->HasField(TEXT("default_value")));

	TestNode->DestroyNode();
	TestGraph->MarkAsGarbage();
	return true;
}
