// blueprint.export_text_inventory lists every FText of a Blueprint (CDO incl. struct arrays, widget
// tree widgets, text pin defaults) with a stable sort, source kind and string table identity.

#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexBPTextInventoryTestTypes.h"
#include "CortexEngineCompat.h"
#include "CortexTypes.h"
#include "Operations/CortexBPTextInventoryOps.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

namespace
{
	TSharedPtr<FJsonObject> FindEntry(
		const TArray<TSharedPtr<FJsonValue>>& Entries,
		const FString& Location,
		const FString& Owner,
		const FString& Path)
	{
		for (const TSharedPtr<FJsonValue>& Value : Entries)
		{
			const TSharedPtr<FJsonObject> Entry = Value->AsObject();
			if (Entry.IsValid()
				&& Entry->GetStringField(TEXT("location")) == Location
				&& Entry->GetStringField(TEXT("owner")) == Owner
				&& Entry->GetStringField(TEXT("path")) == Path)
			{
				return Entry;
			}
		}
		return nullptr;
	}

	FString SerializeCompact(const TSharedPtr<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return Text;
	}

	void RetireInventoryPackage(UObject* Asset)
	{
		if (Asset)
		{
			UPackage* Package = Asset->GetOutermost();
			Package->ClearDirtyFlag();
			Package->MarkAsGarbage();
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPExportTextInventoryTest,
	"Cortex.Blueprint.ExportTextInventory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPExportTextInventoryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8);
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/CortexTextInventory_%s"), *Suffix));
	TStrongObjectPtr<UBlueprint> Blueprint(FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), Package, FName(*FPackageName::GetLongPackageAssetName(Package->GetName())),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), NAME_None));
	if (!TestNotNull(TEXT("actor Blueprint created"), Blueprint.Get()))
	{
		return false;
	}

	FEdGraphPinType TextType;
	TextType.PinCategory = UEdGraphSchema_K2::PC_Text;
	FEdGraphPinType RowArrayType;
	RowArrayType.PinCategory = UEdGraphSchema_K2::PC_Struct;
	RowArrayType.PinSubCategoryObject = FCortexBPTextInventoryTestRow::StaticStruct();
	RowArrayType.ContainerType = EPinContainerType::Array;
	const bool bVariablesAdded =
		FBlueprintEditorUtils::AddMemberVariable(Blueprint.Get(), TEXT("LiteralText"), TextType) &&
		FBlueprintEditorUtils::AddMemberVariable(Blueprint.Get(), TEXT("TableText"), TextType) &&
		FBlueprintEditorUtils::AddMemberVariable(Blueprint.Get(), TEXT("Rows"), RowArrayType);
	if (!TestTrue(TEXT("variables added"), bVariablesAdded))
	{
		RetireInventoryPackage(Blueprint.Get());
		return false;
	}

	// PrintText graph node with a literal text pin default.
	UEdGraph* EventGraph = Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;
	if (!TestNotNull(TEXT("event graph"), EventGraph))
	{
		RetireInventoryPackage(Blueprint.Get());
		return false;
	}
	UK2Node_CallFunction* PrintNode = NewObject<UK2Node_CallFunction>(EventGraph);
	PrintNode->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintText")));
	EventGraph->AddNode(PrintNode, false, false);
	PrintNode->CreateNewGuid();
	PrintNode->AllocateDefaultPins();
	UEdGraphPin* InTextPin = PrintNode->FindPin(TEXT("InText"), EGPD_Input);
	if (!TestNotNull(TEXT("PrintText InText pin"), InTextPin))
	{
		RetireInventoryPackage(Blueprint.Get());
		return false;
	}
	InTextPin->DefaultTextValue = NSLOCTEXT("CortexInventory", "PinKey", "Pin Literal");
	const FString PinOwner = FString::Printf(TEXT("%s/%s"), *EventGraph->GetName(),
		*PrintNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));

	FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
	UClass* GeneratedClass = Blueprint->GeneratedClass;
	if (!TestNotNull(TEXT("generated class"), GeneratedClass))
	{
		RetireInventoryPackage(Blueprint.Get());
		return false;
	}
	UObject* CDO = GeneratedClass->GetDefaultObject();

	UStringTable* Table = NewObject<UStringTable>(GetTransientPackage(),
		FName(*FString::Printf(TEXT("CortexInventoryTable_%s"), *Suffix)));
	Table->GetMutableStringTable()->SetNamespace(TEXT("CortexInventoryNS"));
	CortexEngineCompat::SetStringTableSourceString(*Table->GetMutableStringTable(), TEXT("TableKey"), TEXT("Table Value"));

	FTextProperty* LiteralProperty = CastField<FTextProperty>(GeneratedClass->FindPropertyByName(TEXT("LiteralText")));
	FTextProperty* TableProperty = CastField<FTextProperty>(GeneratedClass->FindPropertyByName(TEXT("TableText")));
	FArrayProperty* RowsProperty = CastField<FArrayProperty>(GeneratedClass->FindPropertyByName(TEXT("Rows")));
	FStructProperty* RowStruct = RowsProperty ? CastField<FStructProperty>(RowsProperty->Inner) : nullptr;
	if (!TestTrue(TEXT("variable properties resolved"), LiteralProperty && TableProperty && RowsProperty && RowStruct))
	{
		RetireInventoryPackage(Blueprint.Get());
		return false;
	}
	LiteralProperty->SetPropertyValue_InContainer(CDO, NSLOCTEXT("CortexInventory", "LiteralKey", "Literal Value"));
	TableProperty->SetPropertyValue_InContainer(CDO, FText::FromStringTable(Table->GetStringTableId(), TEXT("TableKey")));
	{
		FScriptArrayHelper Rows(RowsProperty, RowsProperty->ContainerPtrToValuePtr<void>(CDO));
		Rows.AddValues(2);
		reinterpret_cast<FCortexBPTextInventoryTestRow*>(Rows.GetRawPtr(0))->Label =
			NSLOCTEXT("CortexInventory", "RowKey", "Row Literal");
		reinterpret_cast<FCortexBPTextInventoryTestRow*>(Rows.GetRawPtr(1))->Label =
			FText::AsCultureInvariant(TEXT("Row Invariant"));
	}

	// Widget Blueprint with a literal TextBlock (created reflectively: CortexBlueprint has no UMG dependency).
	FCortexBPCommandHandler Handler;
	const FString WidgetName = FString::Printf(TEXT("WBP_TextInventory_%s"), *Suffix);
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("name"), WidgetName);
	CreateParams->SetStringField(TEXT("path"), FString::Printf(TEXT("/Game/Temp/CortexTextInventory_%s"), *Suffix));
	CreateParams->SetStringField(TEXT("type"), TEXT("Widget"));
	const FCortexCommandResult CreateResult = Handler.Execute(TEXT("create"), CreateParams);
	FString WidgetAssetPath;
	if (CreateResult.bSuccess && CreateResult.Data.IsValid())
	{
		CreateResult.Data->TryGetStringField(TEXT("asset_path"), WidgetAssetPath);
	}
	UBlueprint* WidgetBlueprint = WidgetAssetPath.IsEmpty() ? nullptr : LoadObject<UBlueprint>(nullptr, *WidgetAssetPath);
	if (!TestNotNull(TEXT("Widget Blueprint created"), WidgetBlueprint))
	{
		RetireInventoryPackage(Blueprint.Get());
		return false;
	}
	const FObjectProperty* WidgetTreeProperty =
		CastField<FObjectProperty>(WidgetBlueprint->GetClass()->FindPropertyByName(TEXT("WidgetTree")));
	UObject* WidgetTree = WidgetTreeProperty ? WidgetTreeProperty->GetObjectPropertyValue_InContainer(WidgetBlueprint) : nullptr;
	UClass* TextBlockClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.TextBlock"));
	if (!TestTrue(TEXT("widget tree and TextBlock class"), WidgetTree && TextBlockClass))
	{
		RetireInventoryPackage(WidgetBlueprint);
		RetireInventoryPackage(Blueprint.Get());
		return false;
	}
	UObject* Caption = NewObject<UObject>(WidgetTree, TextBlockClass, TEXT("Caption"), RF_Transient);
	FTextProperty* CaptionText = CastField<FTextProperty>(TextBlockClass->FindPropertyByName(TEXT("Text")));
	if (!TestTrue(TEXT("TextBlock text property"), Caption && CaptionText))
	{
		RetireInventoryPackage(WidgetBlueprint);
		RetireInventoryPackage(Blueprint.Get());
		return false;
	}
	CaptionText->SetPropertyValue_InContainer(Caption, NSLOCTEXT("CortexInventory", "CaptionKey", "Caption Literal"));

	// Actor Blueprint inventory.
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
	const FCortexCommandResult First = Handler.Execute(TEXT("export_text_inventory"), Params);
	if (TestTrue(TEXT("export_text_inventory succeeds"), First.bSuccess && First.Data.IsValid()))
	{
		const TArray<TSharedPtr<FJsonValue>>& Entries = First.Data->GetArrayField(TEXT("entries"));

		const TSharedPtr<FJsonObject> Literal = FindEntry(Entries, TEXT("class_default"), TEXT("CDO"), TEXT("LiteralText"));
		if (TestTrue(TEXT("CDO literal entry"), Literal.IsValid()))
		{
			TestEqual(TEXT("literal source_kind"), Literal->GetStringField(TEXT("source_kind")), FString(TEXT("literal")));
			TestEqual(TEXT("literal value"), Literal->GetStringField(TEXT("value")), FString(TEXT("Literal Value")));
			TestFalse(TEXT("literal has no table_id"), Literal->HasField(TEXT("table_id")));
		}

		const TSharedPtr<FJsonObject> TableEntry = FindEntry(Entries, TEXT("class_default"), TEXT("CDO"), TEXT("TableText"));
		if (TestTrue(TEXT("CDO string table entry"), TableEntry.IsValid()))
		{
			TestEqual(TEXT("string table source_kind"),
				TableEntry->GetStringField(TEXT("source_kind")), FString(TEXT("string_table")));
			TestEqual(TEXT("string table value"), TableEntry->GetStringField(TEXT("value")), FString(TEXT("Table Value")));
			TestEqual(TEXT("table_id"), TableEntry->GetStringField(TEXT("table_id")),
				Table->GetStringTableId().ToString());
			TestEqual(TEXT("key"), TableEntry->GetStringField(TEXT("key")), FString(TEXT("TableKey")));
		}

		const TSharedPtr<FJsonObject> RowLiteral = FindEntry(Entries, TEXT("class_default"), TEXT("CDO"), TEXT("Rows[0].Label"));
		if (TestTrue(TEXT("struct array element 0 entry"), RowLiteral.IsValid()))
		{
			TestEqual(TEXT("row 0 source_kind"), RowLiteral->GetStringField(TEXT("source_kind")), FString(TEXT("literal")));
			TestEqual(TEXT("row 0 value"), RowLiteral->GetStringField(TEXT("value")), FString(TEXT("Row Literal")));
		}
		const TSharedPtr<FJsonObject> RowInvariant = FindEntry(Entries, TEXT("class_default"), TEXT("CDO"), TEXT("Rows[1].Label"));
		if (TestTrue(TEXT("struct array element 1 entry"), RowInvariant.IsValid()))
		{
			TestEqual(TEXT("row 1 source_kind"),
				RowInvariant->GetStringField(TEXT("source_kind")), FString(TEXT("culture_invariant")));
		}

		const TSharedPtr<FJsonObject> PinEntry = FindEntry(Entries, TEXT("pin"), PinOwner, TEXT("InText"));
		if (TestTrue(TEXT("pin default entry"), PinEntry.IsValid()))
		{
			TestEqual(TEXT("pin source_kind"), PinEntry->GetStringField(TEXT("source_kind")), FString(TEXT("literal")));
			TestEqual(TEXT("pin value"), PinEntry->GetStringField(TEXT("value")), FString(TEXT("Pin Literal")));
		}

		// Sorted by (location, owner, path); counts add up to the entries.
		bool bSorted = true;
		for (int32 Index = 1; Index < Entries.Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Prev = Entries[Index - 1]->AsObject();
			const TSharedPtr<FJsonObject> Next = Entries[Index]->AsObject();
			const FString PrevKey = Prev->GetStringField(TEXT("location")) + TEXT("\n") +
				Prev->GetStringField(TEXT("owner")) + TEXT("\n") + Prev->GetStringField(TEXT("path"));
			const FString NextKey = Next->GetStringField(TEXT("location")) + TEXT("\n") +
				Next->GetStringField(TEXT("owner")) + TEXT("\n") + Next->GetStringField(TEXT("path"));
			bSorted &= PrevKey.Compare(NextKey, ESearchCase::CaseSensitive) <= 0;
		}
		TestTrue(TEXT("entries sorted by location, owner, path"), bSorted);

		const TSharedPtr<FJsonObject>* Counts = nullptr;
		if (TestTrue(TEXT("counts object"), First.Data->TryGetObjectField(TEXT("counts"), Counts)))
		{
			int32 Total = 0;
			TestTrue(TEXT("total count"), (*Counts)->TryGetNumberField(TEXT("total"), Total));
			TestEqual(TEXT("total equals entries"), Total, Entries.Num());
			int32 Summed = 0;
			for (const TCHAR* Kind : {TEXT("string_table"), TEXT("literal"), TEXT("culture_invariant"), TEXT("empty")})
			{
				int32 Count = -1;
				TestTrue(*FString::Printf(TEXT("count for %s"), Kind), (*Counts)->TryGetNumberField(Kind, Count));
				Summed += FMath::Max(Count, 0);
			}
			TestEqual(TEXT("kind counts sum to total"), Summed, Total);
			int32 StringTableCount = 0;
			(*Counts)->TryGetNumberField(TEXT("string_table"), StringTableCount);
			TestEqual(TEXT("exactly one string table entry"), StringTableCount, 1);
			int32 InvariantCount = 0;
			(*Counts)->TryGetNumberField(TEXT("culture_invariant"), InvariantCount);
			TestEqual(TEXT("exactly one culture invariant entry"), InvariantCount, 1);
		}

		const FCortexCommandResult Second = Handler.Execute(TEXT("export_text_inventory"), Params);
		TestTrue(TEXT("second call succeeds"), Second.bSuccess && Second.Data.IsValid());
		if (Second.Data.IsValid())
		{
			TestEqual(TEXT("second call is identical"), SerializeCompact(Second.Data), SerializeCompact(First.Data));
		}

		// The reusable collector yields the same entries as the command.
		TArray<TSharedPtr<FJsonObject>> Collected;
		FCortexBPTextInventoryOps::CollectTextEntries(Blueprint.Get(), Collected);
		TestEqual(TEXT("CollectTextEntries matches command entry count"), Collected.Num(), Entries.Num());
	}

	// Widget Blueprint inventory.
	TSharedPtr<FJsonObject> WidgetParams = MakeShared<FJsonObject>();
	WidgetParams->SetStringField(TEXT("asset_path"), WidgetAssetPath);
	const FCortexCommandResult WidgetResult = Handler.Execute(TEXT("export_text_inventory"), WidgetParams);
	if (TestTrue(TEXT("widget export succeeds"), WidgetResult.bSuccess && WidgetResult.Data.IsValid()))
	{
		const TSharedPtr<FJsonObject> CaptionEntry = FindEntry(
			WidgetResult.Data->GetArrayField(TEXT("entries")), TEXT("widget"), TEXT("Caption"), TEXT("Text"));
		if (TestTrue(TEXT("widget Text entry"), CaptionEntry.IsValid()))
		{
			TestEqual(TEXT("widget source_kind"), CaptionEntry->GetStringField(TEXT("source_kind")), FString(TEXT("literal")));
			TestEqual(TEXT("widget value"), CaptionEntry->GetStringField(TEXT("value")), FString(TEXT("Caption Literal")));
		}
	}

	// Unknown asset.
	TSharedPtr<FJsonObject> MissingParams = MakeShared<FJsonObject>();
	MissingParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/CortexTextInventory_DoesNotExist/BP_Missing"));
	const FCortexCommandResult Missing = Handler.Execute(TEXT("export_text_inventory"), MissingParams);
	TestFalse(TEXT("unknown asset fails"), Missing.bSuccess);
	TestEqual(TEXT("unknown asset error code"), Missing.ErrorCode, CortexErrorCodes::AssetNotFound);

	RetireInventoryPackage(WidgetBlueprint);
	RetireInventoryPackage(Blueprint.Get());
	return true;
}
