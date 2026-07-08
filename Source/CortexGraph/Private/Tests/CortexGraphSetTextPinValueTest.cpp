#include "Misc/AutomationTest.h"
#include "CortexCommandRouter.h"
#include "CortexGraphCommandHandler.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "ObjectTools.h"

namespace
{
UBlueprint* CreateTextPinBlueprint(const TCHAR* PackageName, const TCHAR* BlueprintName)
{
	UPackage* Package = CreatePackage(PackageName);
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		Package,
		FName(BlueprintName),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	return Blueprint;
}

FString AddPrintTextNode(FCortexCommandRouter& Router, const FString& AssetPath)
{
	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("asset_path"), AssetPath);
	AddParams->SetStringField(TEXT("node_class"), TEXT("UK2Node_CallFunction"));
	TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
	NodeParams->SetStringField(TEXT("function_name"), TEXT("KismetSystemLibrary.PrintText"));
	AddParams->SetObjectField(TEXT("params"), NodeParams);

	FCortexCommandResult AddResult = Router.Execute(TEXT("graph.add_node"), AddParams);
	if (!AddResult.bSuccess || !AddResult.Data.IsValid())
	{
		return FString();
	}

	FString NodeId;
	AddResult.Data->TryGetStringField(TEXT("node_id"), NodeId);
	return NodeId;
}

TSharedPtr<FJsonObject> MakeStringTableDescriptor(const FString& TableId, const FString& Key, const FString& Value)
{
	TSharedPtr<FJsonObject> StringTable = MakeShared<FJsonObject>();
	StringTable->SetStringField(TEXT("table_id"), TableId);
	StringTable->SetStringField(TEXT("key"), Key);

	TSharedPtr<FJsonObject> Text = MakeShared<FJsonObject>();
	Text->SetStringField(TEXT("type"), TEXT("FText"));
	Text->SetStringField(TEXT("source_kind"), TEXT("string_table"));
	Text->SetStringField(TEXT("value"), Value);
	Text->SetObjectField(TEXT("string_table"), StringTable);
	return Text;
}

void DeleteTestBlueprintAsset(const FString& AssetPath)
{
	UBlueprint* Blueprint = AssetPath.IsEmpty() ? nullptr : LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (Blueprint == nullptr)
	{
		return;
	}

	TArray<UObject*> ObjectsToDelete;
	ObjectsToDelete.Add(Blueprint);
	ObjectTools::DeleteObjectsUnchecked(ObjectsToDelete);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexGraphSetTextPinValueStringTableTest,
	"Cortex.Graph.SetPinValue.Text.StringTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexGraphSetTextPinValueStringTableTest::RunTest(const FString& Parameters)
{
	UStringTable* TestTable = NewObject<UStringTable>(
		GetTransientPackage(),
		FName(TEXT("TestStringTable_GraphTextMutation")));
	TestTable->GetMutableStringTable()->SetNamespace(TEXT("TestNS"));
	TestTable->GetMutableStringTable()->SetSourceString(TEXT("Mail.Button.Pay"), TEXT("Pay"));

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString PackageName = FString::Printf(TEXT("/Game/Temp/CortexGraphSetTextPinValueStringTable_%s"), *Suffix);
	const FString BlueprintName = FString::Printf(TEXT("BP_SetTextPinValueStringTable_%s"), *Suffix);
	UBlueprint* Blueprint = CreateTextPinBlueprint(*PackageName, *BlueprintName);
	TestNotNull(TEXT("Blueprint created"), Blueprint);
	if (Blueprint == nullptr)
	{
		TestTable->MarkAsGarbage();
		return false;
	}

	const FString AssetPath = Blueprint->GetPathName();
	FCortexCommandRouter Router;
	Router.RegisterDomain(TEXT("graph"), TEXT("Cortex Graph"), TEXT("1.0.1"),
		MakeShared<FCortexGraphCommandHandler>());

	const FString NodeId = AddPrintTextNode(Router, AssetPath);
	TestFalse(TEXT("PrintText node created"), NodeId.IsEmpty());

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), AssetPath);
	Params->SetStringField(TEXT("node_id"), NodeId);
	Params->SetStringField(TEXT("pin_name"), TEXT("InText"));
	Params->SetObjectField(TEXT("text"), MakeStringTableDescriptor(
		TestTable->GetStringTableId().ToString(),
		TEXT("Mail.Button.Pay"),
		TEXT("Pay")));

	const FCortexCommandResult Result = Router.Execute(TEXT("graph.set_pin_value"), Params);
	TestTrue(TEXT("StringTable text pin mutation succeeds"), Result.bSuccess);
	if (Result.bSuccess && Result.Data.IsValid())
	{
		TestTrue(TEXT("verification passed"), Result.Data->GetBoolField(TEXT("verification_passed")));
		TestTrue(TEXT("locator verified"), Result.Data->GetBoolField(TEXT("locator_verified")));

		const TSharedPtr<FJsonObject> Verified = Result.Data->GetObjectField(TEXT("verified_text"));
		TestEqual(TEXT("verified source_kind"), Verified->GetStringField(TEXT("source_kind")), TEXT("string_table"));
		TestEqual(TEXT("verified value"), Verified->GetStringField(TEXT("value")), TEXT("Pay"));
		TestEqual(TEXT("verified key"),
			Verified->GetObjectField(TEXT("string_table"))->GetStringField(TEXT("key")),
			TEXT("Mail.Button.Pay"));
	}

	TestTable->MarkAsGarbage();
	DeleteTestBlueprintAsset(AssetPath);
	return true;
}
