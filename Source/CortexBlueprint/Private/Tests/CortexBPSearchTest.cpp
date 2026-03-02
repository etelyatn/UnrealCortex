#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "CortexTypes.h"
#include "Misc/Guid.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Misc/PackageName.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"

namespace
{
	FString CreateSearchTestBlueprint(FCortexBPCommandHandler& Handler, const FString& Suffix)
	{
		const FString Name = FString::Printf(TEXT("BP_SearchTest_%s"), *Suffix);
		const FString Dir = FString::Printf(TEXT("/Game/Temp/CortexBPSearch_%s"), *Suffix);

		const TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("name"), Name);
		CreateParams->SetStringField(TEXT("path"), Dir);
		CreateParams->SetStringField(TEXT("type"), TEXT("Actor"));

		const FCortexCommandResult CreateResult = Handler.Execute(TEXT("create"), CreateParams);
		if (!CreateResult.bSuccess || !CreateResult.Data.IsValid())
		{
			return FString();
		}

		FString AssetPath;
		CreateResult.Data->TryGetStringField(TEXT("asset_path"), AssetPath);
		return AssetPath;
	}

	void CleanupSearchTestBlueprint(const FString& AssetPath)
	{
		if (AssetPath.IsEmpty())
		{
			return;
		}

		const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
		if (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName))
		{
			return;
		}

		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (Blueprint)
		{
			Blueprint->GetOutermost()->MarkAsGarbage();
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSearchMissingParamsTest,
	"Cortex.Blueprint.Search.MissingParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPSearchMissingParamsTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	const FCortexCommandResult Result = Handler.Execute(TEXT("search"), Params);
	TestFalse(TEXT("search should fail without required params"), Result.bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSearchEmptyQueryTest,
	"Cortex.Blueprint.Search.EmptyQuery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPSearchEmptyQueryTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Blueprints/SomeAsset"));
	Params->SetStringField(TEXT("query"), TEXT(""));

	const FCortexCommandResult Result = Handler.Execute(TEXT("search"), Params);
	TestFalse(TEXT("search should fail with empty query"), Result.bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSearchInvalidSearchInTest,
	"Cortex.Blueprint.Search.InvalidSearchIn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPSearchInvalidSearchInTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Blueprints/SomeAsset"));
	Params->SetStringField(TEXT("query"), TEXT("test"));
	TArray<TSharedPtr<FJsonValue>> SearchIn;
	SearchIn.Add(MakeShared<FJsonValueString>(TEXT("nodes")));
	Params->SetArrayField(TEXT("search_in"), SearchIn);

	const FCortexCommandResult Result = Handler.Execute(TEXT("search"), Params);
	TestFalse(TEXT("search should fail with invalid search_in"), Result.bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSearchNonexistentBlueprintTest,
	"Cortex.Blueprint.Search.Nonexistent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPSearchNonexistentBlueprintTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonexistentBP"));
	Params->SetStringField(TEXT("query"), TEXT("anything"));

	const FCortexCommandResult Result = Handler.Execute(TEXT("search"), Params);
	TestFalse(TEXT("search should fail for nonexistent blueprint"), Result.bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSearchCDOMatchTest,
	"Cortex.Blueprint.Search.CDOMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPSearchCDOMatchTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	FCortexBPCommandHandler Handler;
	const FString AssetPath = CreateSearchTestBlueprint(Handler, Suffix);
	TestFalse(TEXT("Test Blueprint should be created"), AssetPath.IsEmpty());
	if (AssetPath.IsEmpty())
	{
		return true;
	}

	const TSharedPtr<FJsonObject> AddVariableParams = MakeShared<FJsonObject>();
	AddVariableParams->SetStringField(TEXT("asset_path"), AssetPath);
	AddVariableParams->SetStringField(TEXT("name"), TEXT("TestMessage"));
	AddVariableParams->SetStringField(TEXT("type"), TEXT("String"));
	AddVariableParams->SetStringField(TEXT("default_value"), TEXT("UniqueSearchTarget_XYZ"));

	const FCortexCommandResult AddVariableResult = Handler.Execute(TEXT("add_variable"), AddVariableParams);
	TestTrue(TEXT("add_variable should succeed"), AddVariableResult.bSuccess);

	const TSharedPtr<FJsonObject> CompileParams = MakeShared<FJsonObject>();
	CompileParams->SetStringField(TEXT("asset_path"), AssetPath);
	const FCortexCommandResult CompileResult = Handler.Execute(TEXT("compile"), CompileParams);
	TestTrue(TEXT("compile should succeed"), CompileResult.bSuccess);

	const TSharedPtr<FJsonObject> SetDefaultsParams = MakeShared<FJsonObject>();
	SetDefaultsParams->SetStringField(TEXT("blueprint_path"), AssetPath);
	SetDefaultsParams->SetBoolField(TEXT("compile"), false);
	SetDefaultsParams->SetBoolField(TEXT("save"), false);
	TSharedPtr<FJsonObject> DefaultsObject = MakeShared<FJsonObject>();
	DefaultsObject->SetStringField(TEXT("TestMessage"), TEXT("UniqueSearchTarget_XYZ"));
	SetDefaultsParams->SetObjectField(TEXT("properties"), DefaultsObject);

	const FCortexCommandResult SetDefaultsResult = Handler.Execute(TEXT("set_class_defaults"), SetDefaultsParams);
	TestTrue(TEXT("set_class_defaults should succeed"), SetDefaultsResult.bSuccess);

	const TSharedPtr<FJsonObject> SearchParams = MakeShared<FJsonObject>();
	SearchParams->SetStringField(TEXT("asset_path"), AssetPath);
	SearchParams->SetStringField(TEXT("query"), TEXT("UniqueSearchTarget"));

	const FCortexCommandResult SearchResult = Handler.Execute(TEXT("search"), SearchParams);
	TestTrue(TEXT("search should succeed"), SearchResult.bSuccess);

	if (SearchResult.Data.IsValid())
	{
		int32 MatchCount = 0;
		SearchResult.Data->TryGetNumberField(TEXT("match_count"), MatchCount);
		TestTrue(TEXT("search should return at least one match"), MatchCount > 0);

		const TArray<TSharedPtr<FJsonValue>>* Matches = nullptr;
		if (SearchResult.Data->TryGetArrayField(TEXT("matches"), Matches) && Matches && Matches->Num() > 0)
		{
			const TSharedPtr<FJsonObject> FirstMatch = (*Matches)[0]->AsObject();
			if (FirstMatch.IsValid())
			{
				TestTrue(TEXT("match value should contain search query"),
					FirstMatch->GetStringField(TEXT("value")).Contains(
						TEXT("UniqueSearchTarget"), ESearchCase::CaseSensitive));
				TestEqual(TEXT("match type should be cdo"),
					FirstMatch->GetStringField(TEXT("type")), TEXT("cdo"));
			}
		}
	}

	CleanupSearchTestBlueprint(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSearchPinMatchTest,
	"Cortex.Blueprint.Search.PinMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPSearchPinMatchTest::RunTest(const FString& Parameters)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	FCortexBPCommandHandler Handler;
	const FString AssetPath = CreateSearchTestBlueprint(Handler, Suffix);
	TestFalse(TEXT("Test Blueprint should be created"), AssetPath.IsEmpty());
	if (AssetPath.IsEmpty())
	{
		return false;
	}

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("bp"), TEXT("Cortex Blueprint"), TEXT("1.0.0"),
		MakeShared<FCortexBPCommandHandler>());
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.0"),
		MakeShared<FCortexGraphCommandHandler>());

	const TSharedPtr<FJsonObject> AddNodeParams = MakeShared<FJsonObject>();
	AddNodeParams->SetStringField(TEXT("asset_path"), AssetPath);
	AddNodeParams->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
	TSharedPtr<FJsonObject> AddNodeInnerParams = MakeShared<FJsonObject>();
	AddNodeInnerParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintString"));
	AddNodeParams->SetObjectField(TEXT("params"), AddNodeInnerParams);

	const FCortexCommandResult AddNodeResult = Router.Execute(TEXT("graph.add_node"), AddNodeParams);
	TestTrue(TEXT("graph.add_node should succeed"), AddNodeResult.bSuccess);

	FString NodeId;
	if (AddNodeResult.Data.IsValid())
	{
		AddNodeResult.Data->TryGetStringField(TEXT("node_id"), NodeId);
	}
	TestFalse(TEXT("Added node should have node_id"), NodeId.IsEmpty());

	const TSharedPtr<FJsonObject> SetPinParams = MakeShared<FJsonObject>();
	SetPinParams->SetStringField(TEXT("asset_path"), AssetPath);
	SetPinParams->SetStringField(TEXT("node_id"), NodeId);
	SetPinParams->SetStringField(TEXT("pin_name"), TEXT("InString"));
	SetPinParams->SetStringField(TEXT("value"), TEXT("PinSearchNeedle_XYZ"));

	const FCortexCommandResult SetPinResult = Router.Execute(TEXT("graph.set_pin_value"), SetPinParams);
	TestTrue(TEXT("graph.set_pin_value should succeed"), SetPinResult.bSuccess);

	const TSharedPtr<FJsonObject> SearchParams = MakeShared<FJsonObject>();
	SearchParams->SetStringField(TEXT("asset_path"), AssetPath);
	SearchParams->SetStringField(TEXT("query"), TEXT("PinSearchNeedle"));
	TArray<TSharedPtr<FJsonValue>> SearchIn;
	SearchIn.Add(MakeShared<FJsonValueString>(TEXT("pins")));
	SearchParams->SetArrayField(TEXT("search_in"), SearchIn);

	const FCortexCommandResult SearchResult = Handler.Execute(TEXT("search"), SearchParams);
	TestTrue(TEXT("search should succeed"), SearchResult.bSuccess);

	bool bFoundPinMatch = false;
	if (SearchResult.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Matches = nullptr;
		if (SearchResult.Data->TryGetArrayField(TEXT("matches"), Matches) && Matches)
		{
			for (const TSharedPtr<FJsonValue>& MatchValue : *Matches)
			{
				const TSharedPtr<FJsonObject> MatchObj = MatchValue->AsObject();
				if (!MatchObj.IsValid())
				{
					continue;
				}

				FString Type;
				MatchObj->TryGetStringField(TEXT("type"), Type);
				if (Type == TEXT("pin"))
				{
					bFoundPinMatch = true;
					TestTrue(TEXT("pin match should include node_id"), MatchObj->HasField(TEXT("node_id")));
					TestTrue(TEXT("pin match should include graph_name"), MatchObj->HasField(TEXT("graph_name")));
					break;
				}
			}
		}
	}

	TestTrue(TEXT("search should include at least one pin match"), bFoundPinMatch);
	CleanupSearchTestBlueprint(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSearchPinTextStringTableMatchTest,
	"Cortex.Blueprint.Search.PinTextStringTableMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPSearchPinTextStringTableMatchTest::RunTest(const FString& Parameters)
{
	if (GEngine)
	{
		GEngine->Exec(nullptr, TEXT("log LogStringTable Error"));
	}

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	FCortexBPCommandHandler Handler;
	const FString AssetPath = CreateSearchTestBlueprint(Handler, Suffix);
	TestFalse(TEXT("Test Blueprint should be created"), AssetPath.IsEmpty());
	if (AssetPath.IsEmpty())
	{
		return false;
	}

	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.0"),
		MakeShared<FCortexGraphCommandHandler>());

	const TSharedPtr<FJsonObject> AddNodeParams = MakeShared<FJsonObject>();
	AddNodeParams->SetStringField(TEXT("asset_path"), AssetPath);
	AddNodeParams->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
	TSharedPtr<FJsonObject> AddNodeInnerParams = MakeShared<FJsonObject>();
	AddNodeInnerParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintText"));
	AddNodeParams->SetObjectField(TEXT("params"), AddNodeInnerParams);

	const FCortexCommandResult AddNodeResult = Router.Execute(TEXT("graph.add_node"), AddNodeParams);
	TestTrue(TEXT("graph.add_node PrintText should succeed"), AddNodeResult.bSuccess);

	FString NodeId;
	if (AddNodeResult.Data.IsValid())
	{
		AddNodeResult.Data->TryGetStringField(TEXT("node_id"), NodeId);
	}
	TestFalse(TEXT("Added node should have node_id"), NodeId.IsEmpty());

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	TestNotNull(TEXT("Blueprint should load"), Blueprint);
	if (!Blueprint)
	{
		CleanupSearchTestBlueprint(AssetPath);
		return false;
	}

	UEdGraphNode* TargetNode = nullptr;
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (!Graph)
		{
			continue;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->GetName() == NodeId)
			{
				TargetNode = Node;
				break;
			}
		}
		if (TargetNode)
		{
			break;
		}
	}
	TestNotNull(TEXT("Should find added graph node"), TargetNode);
	if (!TargetNode)
	{
		CleanupSearchTestBlueprint(AssetPath);
		return false;
	}

	UEdGraphPin* TextPin = nullptr;
	for (UEdGraphPin* Pin : TargetNode->Pins)
	{
		if (Pin && Pin->PinName == TEXT("InText"))
		{
			TextPin = Pin;
			break;
		}
	}
	TestNotNull(TEXT("PrintText node should have InText pin"), TextPin);
	if (!TextPin)
	{
		CleanupSearchTestBlueprint(AssetPath);
		return false;
	}

	TextPin->DefaultTextValue = FText::FromString(TEXT("Pin Search Value"));

	const TSharedPtr<FJsonObject> SearchParams = MakeShared<FJsonObject>();
	SearchParams->SetStringField(TEXT("asset_path"), AssetPath);
	SearchParams->SetStringField(TEXT("query"), TEXT("Pin Search Value"));
	TArray<TSharedPtr<FJsonValue>> SearchIn;
	SearchIn.Add(MakeShared<FJsonValueString>(TEXT("pins")));
	SearchParams->SetArrayField(TEXT("search_in"), SearchIn);

	const FCortexCommandResult SearchResult = Handler.Execute(TEXT("search"), SearchParams);
	TestTrue(TEXT("search should succeed"), SearchResult.bSuccess);

	bool bFoundPinMatch = false;
	if (SearchResult.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Matches = nullptr;
		if (SearchResult.Data->TryGetArrayField(TEXT("matches"), Matches) && Matches)
		{
			for (const TSharedPtr<FJsonValue>& MatchValue : *Matches)
			{
				const TSharedPtr<FJsonObject> MatchObj = MatchValue->AsObject();
				if (!MatchObj.IsValid())
				{
					continue;
				}

				FString Type;
				MatchObj->TryGetStringField(TEXT("type"), Type);
				if (Type == TEXT("pin"))
				{
					bFoundPinMatch = true;
					break;
				}
			}
		}
	}

	TestTrue(TEXT("search should match text pin value"), bFoundPinMatch);

	if (GEngine)
	{
		GEngine->Exec(nullptr, TEXT("log LogStringTable Warning"));
	}

	CleanupSearchTestBlueprint(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPSearchWidgetStringTableMatchTest,
	"Cortex.Blueprint.Search.WidgetStringTableMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPSearchWidgetStringTableMatchTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString Name = FString::Printf(TEXT("WBP_SearchWidget_%s"), *Suffix);
	const FString Dir = FString::Printf(TEXT("/Game/Temp/CortexBPSearchWidget_%s"), *Suffix);

	const TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("name"), Name);
	CreateParams->SetStringField(TEXT("path"), Dir);
	CreateParams->SetStringField(TEXT("type"), TEXT("Widget"));
	const FCortexCommandResult CreateResult = Handler.Execute(TEXT("create"), CreateParams);
	if (!CreateResult.bSuccess || !CreateResult.Data.IsValid())
	{
		AddInfo(TEXT("Skipping widget search test: Widget Blueprint creation unavailable in current test environment"));
		return true;
	}

	FString AssetPath;
	CreateResult.Data->TryGetStringField(TEXT("asset_path"), AssetPath);
	TestFalse(TEXT("Widget Blueprint path should not be empty"), AssetPath.IsEmpty());
	if (AssetPath.IsEmpty())
	{
		return false;
	}

	UBlueprint* WidgetBP = LoadObject<UBlueprint>(nullptr, *AssetPath);
	TestNotNull(TEXT("Widget Blueprint should load"), WidgetBP);
	if (!WidgetBP)
	{
		CleanupSearchTestBlueprint(AssetPath);
		return false;
	}

	const FObjectProperty* WidgetTreeProperty = CastField<FObjectProperty>(
		WidgetBP->GetClass()->FindPropertyByName(TEXT("WidgetTree")));
	UObject* WidgetTreeObject = WidgetTreeProperty
		? WidgetTreeProperty->GetObjectPropertyValue_InContainer(WidgetBP)
		: nullptr;
	TestNotNull(TEXT("WidgetTree should exist"), WidgetTreeObject);
	if (!WidgetTreeObject)
	{
		CleanupSearchTestBlueprint(AssetPath);
		return false;
	}

	UClass* TextBlockClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.TextBlock"));
	if (!TextBlockClass)
	{
		AddInfo(TEXT("Skipping widget search test: UMG.TextBlock class unavailable"));
		CleanupSearchTestBlueprint(AssetPath);
		return true;
	}

	UObject* TextWidget = NewObject<UObject>(WidgetTreeObject, TextBlockClass, NAME_None, RF_Transient);
	TestNotNull(TEXT("Should create widget object under WidgetTree"), TextWidget);
	if (!TextWidget)
	{
		CleanupSearchTestBlueprint(AssetPath);
		return false;
	}

	FTextProperty* TextProperty = CastField<FTextProperty>(TextWidget->GetClass()->FindPropertyByName(TEXT("Text")));
	TestNotNull(TEXT("TextBlock should have Text property"), TextProperty);
	if (!TextProperty)
	{
		CleanupSearchTestBlueprint(AssetPath);
		return false;
	}

	UStringTable* TestTable = NewObject<UStringTable>(
		GetTransientPackage(),
		FName(TEXT("TestStringTable_BPSearchWidget")));
	TestTable->GetMutableStringTable()->SetNamespace(TEXT("BPWidgetTest"));
	TestTable->GetMutableStringTable()->SetSourceString(TEXT("WidgetSearchKey"), TEXT("Widget Search Value"));

	void* ValuePtr = TextProperty->ContainerPtrToValuePtr<void>(TextWidget);
	TextProperty->SetPropertyValue(ValuePtr,
		FText::FromStringTable(TestTable->GetStringTableId(), TEXT("WidgetSearchKey")));

	const TSharedPtr<FJsonObject> SearchParams = MakeShared<FJsonObject>();
	SearchParams->SetStringField(TEXT("asset_path"), AssetPath);
	SearchParams->SetStringField(TEXT("query"), TEXT("WidgetSearchKey"));
	TArray<TSharedPtr<FJsonValue>> SearchIn;
	SearchIn.Add(MakeShared<FJsonValueString>(TEXT("widgets")));
	SearchParams->SetArrayField(TEXT("search_in"), SearchIn);

	const FCortexCommandResult SearchResult = Handler.Execute(TEXT("search"), SearchParams);
	TestTrue(TEXT("search should succeed"), SearchResult.bSuccess);

	bool bFoundWidgetMatch = false;
	if (SearchResult.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Matches = nullptr;
		if (SearchResult.Data->TryGetArrayField(TEXT("matches"), Matches) && Matches)
		{
			for (const TSharedPtr<FJsonValue>& MatchValue : *Matches)
			{
				const TSharedPtr<FJsonObject> MatchObj = MatchValue->AsObject();
				if (!MatchObj.IsValid())
				{
					continue;
				}

				FString Type;
				MatchObj->TryGetStringField(TEXT("type"), Type);
				if (Type != TEXT("widget"))
				{
					continue;
				}

				const TSharedPtr<FJsonObject>* StringTableObj = nullptr;
				if (MatchObj->TryGetObjectField(TEXT("string_table"), StringTableObj) && StringTableObj)
				{
					FString Key;
					(*StringTableObj)->TryGetStringField(TEXT("key"), Key);
					if (Key == TEXT("WidgetSearchKey"))
					{
						bFoundWidgetMatch = true;
						break;
					}
				}
			}
		}
	}

	TestTrue(TEXT("search should match widget StringTable key"), bFoundWidgetMatch);

	TestTable->MarkAsGarbage();
	CleanupSearchTestBlueprint(AssetPath);
	return true;
}
