#include "Operations/CortexBPTextInventoryOps.h"

#include "CortexSafeFileContract.h"
#include "CortexCommandRouter.h"
#include "Operations/CortexBPAssetOps.h"
#include "Dom/JsonObject.h"
#include "Algo/StableSort.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Internationalization/Text.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

namespace
{
	/** Compact-JSON ceiling of one inline response, shared with the other bounded Cortex reads. */
	constexpr int32 MaxInlineResponseChars = 40000;

	namespace Location
	{
		const TCHAR* const ClassDefault = TEXT("class_default");
		const TCHAR* const Widget = TEXT("widget");
		const TCHAR* const Pin = TEXT("pin");
	}

	namespace SourceKind
	{
		const TCHAR* const StringTable = TEXT("string_table");
		const TCHAR* const Literal = TEXT("literal");
		const TCHAR* const CultureInvariant = TEXT("culture_invariant");
		const TCHAR* const Empty = TEXT("empty");
	}

	struct FTextEntryContext
	{
		TArray<TSharedPtr<FJsonObject>>& Out;
		const TCHAR* Location;
		FString Owner;
	};

	void EmitText(const FText& Text, const FString& Path, const FTextEntryContext& Context)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("location"), Context.Location);
		Entry->SetStringField(TEXT("owner"), Context.Owner);
		Entry->SetStringField(TEXT("path"), Path);
		Entry->SetStringField(TEXT("value"), Text.ToString());

		FName TableId;
		FString Key;
		const TCHAR* Kind = SourceKind::Literal;
		if (FTextInspector::GetTableIdAndKey(Text, TableId, Key))
		{
			Kind = SourceKind::StringTable;
			Entry->SetStringField(TEXT("table_id"), TableId.ToString());
			Entry->SetStringField(TEXT("key"), Key);
		}
		else if (Text.IsEmpty())
		{
			Kind = SourceKind::Empty;
		}
		else if (Text.IsCultureInvariant())
		{
			Kind = SourceKind::CultureInvariant;
		}
		Entry->SetStringField(TEXT("source_kind"), Kind);
		Context.Out.Add(Entry);
	}

	bool CanContainText(const FProperty* Property)
	{
		return CastField<FTextProperty>(Property) || CastField<FStructProperty>(Property)
			|| CastField<FArrayProperty>(Property) || CastField<FSetProperty>(Property)
			|| CastField<FMapProperty>(Property);
	}

	void VisitValue(const FProperty* Property, const void* ValuePtr, const FString& Path, const FTextEntryContext& Context);

	/** Visit every fixed-array slot of a property stored in a container (UObject or struct). */
	void VisitMember(const FProperty* Property, const void* Container, const FString& Path, const FTextEntryContext& Context)
	{
		if (!CanContainText(Property))
		{
			return;
		}
		for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
		{
			const FString ElementPath = Property->ArrayDim > 1 ? FString::Printf(TEXT("%s[%d]"), *Path, Index) : Path;
			VisitValue(Property, Property->ContainerPtrToValuePtr<void>(Container, Index), ElementPath, Context);
		}
	}

	void VisitValue(const FProperty* Property, const void* ValuePtr, const FString& Path, const FTextEntryContext& Context)
	{
		if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			EmitText(TextProperty->GetPropertyValue(ValuePtr), Path, Context);
		}
		else if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
			{
				VisitMember(*It, ValuePtr, Path + TEXT(".") + It->GetName(), Context);
			}
		}
		else if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			if (!CanContainText(ArrayProperty->Inner))
			{
				return;
			}
			FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				VisitValue(ArrayProperty->Inner, Helper.GetRawPtr(Index), FString::Printf(TEXT("%s[%d]"), *Path, Index), Context);
			}
		}
		else if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			if (!CanContainText(SetProperty->ElementProp))
			{
				return;
			}
			FScriptSetHelper Helper(SetProperty, ValuePtr);
			int32 Dense = 0;
			for (int32 Sparse = 0; Sparse < Helper.GetMaxIndex(); ++Sparse)
			{
				if (Helper.IsValidIndex(Sparse))
				{
					VisitValue(SetProperty->ElementProp, Helper.GetElementPtr(Sparse),
						FString::Printf(TEXT("%s[%d]"), *Path, Dense++), Context);
				}
			}
		}
		else if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			const bool bKeys = CanContainText(MapProperty->KeyProp);
			const bool bValues = CanContainText(MapProperty->ValueProp);
			if (!bKeys && !bValues)
			{
				return;
			}
			FScriptMapHelper Helper(MapProperty, ValuePtr);
			int32 Dense = 0;
			for (int32 Sparse = 0; Sparse < Helper.GetMaxIndex(); ++Sparse)
			{
				if (!Helper.IsValidIndex(Sparse))
				{
					continue;
				}
				const FString ElementPath = FString::Printf(TEXT("%s[%d]"), *Path, Dense++);
				if (bKeys)
				{
					VisitValue(MapProperty->KeyProp, Helper.GetKeyPtr(Sparse), ElementPath + TEXT(".key"), Context);
				}
				if (bValues)
				{
					VisitValue(MapProperty->ValueProp, Helper.GetValuePtr(Sparse), ElementPath + TEXT(".value"), Context);
				}
			}
		}
	}

	void VisitObject(const UObject* Object, const FTextEntryContext& Context)
	{
		for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
		{
			VisitMember(*It, Object, It->GetName(), Context);
		}
	}

	FString EntrySortKey(const TSharedPtr<FJsonObject>& Entry)
	{
		return Entry->GetStringField(TEXT("location")) + TEXT("\n") + Entry->GetStringField(TEXT("owner")) + TEXT("\n")
			+ Entry->GetStringField(TEXT("path"));
	}
}

void FCortexBPTextInventoryOps::CollectTextEntries(UBlueprint* Blueprint, TArray<TSharedPtr<FJsonObject>>& OutEntries)
{
	if (!Blueprint)
	{
		return;
	}

	const int32 FirstNewEntry = OutEntries.Num();

	// Generated-class CDO.
	if (UClass* GeneratedClass = Blueprint->GeneratedClass)
	{
		if (const UObject* CDO = GeneratedClass->GetDefaultObject())
		{
			VisitObject(CDO, FTextEntryContext{OutEntries, Location::ClassDefault, TEXT("CDO")});
		}
	}

	// Widget Blueprint widgets: reflection only, CortexBlueprint has no UMG dependency (see CortexBPAnalysisOps).
	const UClass* WidgetBlueprintClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.WidgetBlueprint"));
	if (WidgetBlueprintClass && Blueprint->IsA(WidgetBlueprintClass))
	{
		const FObjectProperty* WidgetTreeProperty =
			CastField<FObjectProperty>(Blueprint->GetClass()->FindPropertyByName(TEXT("WidgetTree")));
		UObject* WidgetTree = WidgetTreeProperty ? WidgetTreeProperty->GetObjectPropertyValue_InContainer(Blueprint) : nullptr;
		if (WidgetTree)
		{
			ForEachObjectWithOuter(WidgetTree, [&OutEntries](UObject* Widget)
			{
				if (Widget && !Widget->HasAnyFlags(RF_ClassDefaultObject))
				{
					VisitObject(Widget, FTextEntryContext{OutEntries, Location::Widget, Widget->GetName()});
				}
			});
		}
	}

	// Text pin defaults in every graph.
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (const UEdGraph* Graph : Graphs)
	{
		if (!Graph)
		{
			continue;
		}
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			const FTextEntryContext Context{OutEntries, Location::Pin,
				FString::Printf(TEXT("%s/%s"), *Graph->GetName(), *Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens))};
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text)
				{
					EmitText(Pin->DefaultTextValue, Pin->PinName.ToString(), Context);
				}
			}
		}
	}

	TArrayView<TSharedPtr<FJsonObject>> NewEntries(OutEntries.GetData() + FirstNewEntry, OutEntries.Num() - FirstNewEntry);
	Algo::StableSortBy(NewEntries, [](const TSharedPtr<FJsonObject>& Entry) { return EntrySortKey(Entry); },
		[](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
}

FCortexCommandResult FCortexBPTextInventoryOps::ExportTextInventory(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: asset_path"));
	}

	// An unusable out_path is refused before any asset work.
	FString OutPath;
	FCortexResolvedFilePath ResolvedOutPath;
	const bool bFileOutput = Params->TryGetStringField(TEXT("out_path"), OutPath);
	if (bFileOutput)
	{
		FString PathErrorCode;
		FString PathErrorMessage;
		if (!FCortexSafeFileContract::ResolveWritePath(OutPath, ResolvedOutPath, PathErrorCode, PathErrorMessage))
		{
			return FCortexCommandRouter::Error(PathErrorCode, PathErrorMessage);
		}
	}

	FString LoadError;
	UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
	if (!Blueprint)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::AssetNotFound, LoadError);
	}

	TArray<TSharedPtr<FJsonObject>> Entries;
	CollectTextEntries(Blueprint, Entries);

	int32 Counts[4] = {0, 0, 0, 0};
	TArray<TSharedPtr<FJsonValue>> EntryValues;
	EntryValues.Reserve(Entries.Num());
	for (const TSharedPtr<FJsonObject>& Entry : Entries)
	{
		const FString Kind = Entry->GetStringField(TEXT("source_kind"));
		if (Kind == SourceKind::StringTable)
		{
			++Counts[0];
		}
		else if (Kind == SourceKind::Literal)
		{
			++Counts[1];
		}
		else if (Kind == SourceKind::CultureInvariant)
		{
			++Counts[2];
		}
		else
		{
			++Counts[3];
		}
		EntryValues.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> CountsJson = MakeShared<FJsonObject>();
	CountsJson->SetNumberField(TEXT("total"), Entries.Num());
	CountsJson->SetNumberField(SourceKind::StringTable, Counts[0]);
	CountsJson->SetNumberField(SourceKind::Literal, Counts[1]);
	CountsJson->SetNumberField(SourceKind::CultureInvariant, Counts[2]);
	CountsJson->SetNumberField(SourceKind::Empty, Counts[3]);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetObjectField(TEXT("counts"), CountsJson);
	Data->SetArrayField(TEXT("entries"), EntryValues);

	if (bFileOutput)
	{
		const FCortexJsonFileWriteResult WriteResult = FCortexSafeFileContract::WriteJsonReportAtomic(ResolvedOutPath, Data.ToSharedRef());
		if (!WriteResult.bWritten)
		{
			return FCortexCommandRouter::Error(
				WriteResult.ErrorCode.IsEmpty() ? CortexErrorCodes::SaveFailed : WriteResult.ErrorCode,
				WriteResult.ErrorMessage);
		}
		TSharedPtr<FJsonObject> FileData = MakeShared<FJsonObject>();
		FileData->SetStringField(TEXT("asset_path"), AssetPath);
		FileData->SetStringField(TEXT("out_path"), ResolvedOutPath.AbsolutePath);
		FileData->SetObjectField(TEXT("counts"), CountsJson);
		return FCortexCommandRouter::Success(FileData);
	}

	FString CompactText;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&CompactText);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);
	if (CompactText.Len() > MaxInlineResponseChars)
	{
		TSharedPtr<FJsonObject> Bounded = MakeShared<FJsonObject>();
		Bounded->SetStringField(TEXT("_error"), TEXT("RESPONSE_TOO_LARGE"));
		Bounded->SetBoolField(TEXT("reader_complete"), false);
		Bounded->SetStringField(TEXT("asset_path"), AssetPath);
		Bounded->SetObjectField(TEXT("counts"), CountsJson);
		Bounded->SetNumberField(TEXT("max_response_chars"), MaxInlineResponseChars);
		Bounded->SetStringField(TEXT("hint"), TEXT("Pass out_path to write the full inventory to a file."));
		return FCortexCommandRouter::Success(Bounded);
	}

	return FCortexCommandRouter::Success(Data);
}
