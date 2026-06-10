#include "CoreMinimal.h"
#include "CortexCommandRouter.h"
#include "CortexDataCommandHandler.h"
#include "CortexTypes.h"
#include "Tests/CortexDataLocalizationTestTypes.h"
#include "Tests/CortexTestDataAsset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/CompositeDataTable.h"
#include "Engine/DataTable.h"
#include "HAL/FileManager.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/OutputDevice.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformProcess.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectHash.h"

namespace
{
	const TCHAR* ExportTestRoot = TEXT("/Game/CortexExportTests");

	FCortexCommandRouter CreateDataExportTestRouter()
	{
		FCortexCommandRouter Router;
		Router.RegisterDomain(
			TEXT("data"),
			TEXT("Cortex Data"),
			TEXT("1.0.1"),
			MakeShared<FCortexDataCommandHandler>());
		return Router;
	}

	FCortexDataLocalizationTestRow MakeExportRow(const FString& Title, const FString& StepText)
	{
		FCortexDataLocalizationTestRow Row;
		Row.Title = FText::FromString(Title);
		Row.row_name = FString::Printf(TEXT("Data %s"), *Title);

		FCortexDataLocalizationStepTestRow Step;
		Step.Description = FText::FromString(StepText);
		Row.Steps.Add(Step);

		return Row;
	}

	FString PackagePathForAsset(const FString& RunId, const FString& AssetName)
	{
		return FString::Printf(TEXT("%s/%s/%s"), ExportTestRoot, *RunId, *AssetName);
	}

	bool SupportedCommandNamesContain(const TArray<FCortexCommandInfo>& Commands, const FString& CommandName)
	{
		for (const FCortexCommandInfo& Command : Commands)
		{
			if (Command.Name == CommandName)
			{
				return true;
			}
		}

		return false;
	}

	void DeletePackageFile(const FString& PackageName)
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackageName,
			FPackageName::GetAssetPackageExtension());

		if (IFileManager::Get().FileExists(*Filename))
		{
			IFileManager::Get().Delete(*Filename);
		}
	}

	void CleanupLoadedPackage(const FString& PackageName)
	{
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			TArray<UObject*> PackageObjects;
			GetObjectsWithPackage(Package, PackageObjects, false);
			for (UObject* Object : PackageObjects)
			{
				if (Object == nullptr)
				{
					continue;
				}

				FAssetRegistryModule::AssetDeleted(Object);
				Object->ClearFlags(RF_Public | RF_Standalone);
				Object->MarkAsGarbage();
			}

			Package->ClearFlags(RF_Public | RF_Standalone);
			Package->MarkAsGarbage();
		}
	}

	void CleanupPackageByName(const FString& PackageName)
	{
		CleanupLoadedPackage(PackageName);
		DeletePackageFile(PackageName);
	}

	class FCortexExportSkipPackageWarningCapture final : public FOutputDevice
	{
	public:
		int32 SkipPackageWarnings = 0;

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			(void)Category;

			if (Verbosity != ELogVerbosity::Warning || V == nullptr)
			{
				return;
			}

			const FString Message(V);
			if (Message.Contains(TEXT("SkipPackage")))
			{
				++SkipPackageWarnings;
			}
		}
	};

	class FCortexDataExportTestFixture
	{
	public:
		FCortexDataExportTestFixture()
			: RunId(FString::Printf(TEXT("Run_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
		{
		}

		~FCortexDataExportTestFixture()
		{
			Cleanup();
		}

		UDataTable* CreateRegularDataTable()
		{
			UDataTable* Table = CreateAsset<UDataTable>(TEXT("DT_CortexExportRows"));
			if (Table == nullptr)
			{
				return nullptr;
			}

			Table->RowStruct = FCortexDataLocalizationTestRow::StaticStruct();

			Table->AddRow(TEXT("zeta"), MakeExportRow(TEXT("Zeta"), TEXT("Third inserted")));
			Table->AddRow(TEXT("alpha"), MakeExportRow(TEXT("Alpha"), TEXT("Second inserted")));
			Table->AddRow(TEXT("middle"), MakeExportRow(TEXT("Middle"), TEXT("First inserted")));

			return Table;
		}

		UCompositeDataTable* CreateCompositeDataTable()
		{
			UDataTable* ParentA = CreateAsset<UDataTable>(TEXT("DT_CortexExportParentA"));
			if (ParentA == nullptr)
			{
				return nullptr;
			}

			ParentA->RowStruct = FCortexDataLocalizationTestRow::StaticStruct();
			ParentA->AddRow(TEXT("shared"), MakeExportRow(TEXT("Base Shared"), TEXT("Base parent")));
			ParentA->AddRow(TEXT("alpha"), MakeExportRow(TEXT("Alpha Parent"), TEXT("Base only")));

			UDataTable* ParentB = CreateAsset<UDataTable>(TEXT("DT_CortexExportParentB"));
			if (ParentB == nullptr)
			{
				return nullptr;
			}

			ParentB->RowStruct = FCortexDataLocalizationTestRow::StaticStruct();
			ParentB->AddRow(TEXT("shared"), MakeExportRow(TEXT("Override Shared"), TEXT("Override parent")));
			ParentB->AddRow(TEXT("beta"), MakeExportRow(TEXT("Beta Parent"), TEXT("Override only")));

			UCompositeDataTable* Composite = CreateAsset<UCompositeDataTable>(TEXT("DT_CortexExportComposite"));
			if (Composite == nullptr)
			{
				return nullptr;
			}

			Composite->RowStruct = FCortexDataLocalizationTestRow::StaticStruct();

			TArray<UDataTable*> Parents;
			Parents.Add(ParentA);
			Parents.Add(ParentB);
			Composite->AppendParentTables(Parents);

			return Composite;
		}

		UStringTable* CreateStringTable()
		{
			UStringTable* Table = CreateAsset<UStringTable>(TEXT("ST_CortexExportText"));
			if (Table == nullptr)
			{
				return nullptr;
			}

			Table->GetMutableStringTable()->SetNamespace(TEXT("CortexExportTests"));
			Table->GetMutableStringTable()->SetSourceString(TEXT("zeta.key"), TEXT("Zeta text"));
			Table->GetMutableStringTable()->SetSourceString(TEXT("alpha.key"), TEXT("Alpha text"));
			Table->GetMutableStringTable()->SetSourceString(TEXT("middle.key"), TEXT("Middle text"));
			Table->GetMutableStringTable()->SetSourceString(TEXT("ignored.other"), TEXT("Ignored text"));
			return Table;
		}

		UCortexTestDataAsset* CreateDataAsset(const FString& AssetName = TEXT("DA_CortexExportFixture"))
		{
		UCortexTestDataAsset* Asset = CreateAsset<UCortexTestDataAsset>(AssetName);
			if (Asset == nullptr)
			{
				return nullptr;
			}

			Asset->TestProperty = TEXT("Editable export value");
			Asset->TestNumber = 42;
			Asset->Nested.Visible = TEXT("Nested export value");
			Asset->Nested.Internal = TEXT("Nested internal value");
			Asset->NestedArray.Add(Asset->Nested);
			Asset->ExportTransientProperty = TEXT("Transient value");
			Asset->TransientExportBlocked = TEXT("Non-editable transient value");
#if WITH_EDITORONLY_DATA
			Asset->ExportEditorOnlyProperty = TEXT("Editor-only value");
#endif
			Asset->ExportInternalProperty = TEXT("Internal value");
			Asset->EditableExportAllowed = TEXT("Editable export allowed value");
			return Asset;
		}

		UDataTable* CreateSchemaAnnotatedDataTable()
		{
			UDataTable* Table = CreateAsset<UDataTable>(TEXT("DT_CortexSchemaAnnotated"));
			if (Table == nullptr)
			{
				return nullptr;
			}

			Table->RowStruct = FCortexSchemaAnnotatedRow::StaticStruct();

			FCortexSchemaAnnotatedRow Row;
			Row.Nested.Child.Label = TEXT("row");
			Row.Payload.InitializeAs<FCortexSchemaInstancedDerived>();
			Table->AddRow(TEXT("alpha"), Row);
			return Table;
		}

		UDataTable* CreateSchemaUnannotatedDataTable()
		{
			UDataTable* Table = CreateAsset<UDataTable>(TEXT("DT_CortexSchemaUnannotated"));
			if (Table == nullptr)
			{
				return nullptr;
			}

			Table->RowStruct = FCortexSchemaUnannotatedRow::StaticStruct();

			FCortexSchemaUnannotatedRow Row;
			Row.Payload.InitializeAs<FCortexSchemaInstancedDerived>();
			Table->AddRow(TEXT("alpha"), Row);
			return Table;
		}

		UCortexDerivedTestDataAsset* CreateDerivedSchemaDataAsset(const FString& AssetName = TEXT("DA_SchemaDerived"))
		{
			UCortexDerivedTestDataAsset* Asset = CreateAsset<UCortexDerivedTestDataAsset>(AssetName);
			if (Asset == nullptr)
			{
				return nullptr;
			}

			Asset->TestProperty = TEXT("base editable");
			Asset->DerivedOnlyProperty = TEXT("derived editable");
			return Asset;
		}

		UCortexSchemaInstancedDataAsset* CreateSchemaInstancedDataAsset(const FString& AssetName = TEXT("DA_SchemaInstanced"))
		{
			UCortexSchemaInstancedDataAsset* Asset = CreateAsset<UCortexSchemaInstancedDataAsset>(AssetName);
			if (Asset == nullptr)
			{
				return nullptr;
			}

			Asset->Payload.InitializeAs<FCortexSchemaInstancedDerived>();
			return Asset;
		}

		FString MakeSavedOutputPath(const FString& FileName) const
		{
			return FPaths::Combine(GetSavedRunDir(), FileName);
		}

		FString MakeSavedOutputDir(const FString& DirName) const
		{
			return FPaths::Combine(GetSavedRunDir(), DirName);
		}

		bool TryReadJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutJson, FString& OutError) const
		{
			FString Contents;
			if (!FFileHelper::LoadFileToString(Contents, *FilePath))
			{
				OutError = FString::Printf(TEXT("Could not read JSON file: %s"), *FilePath);
				return false;
			}

			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Contents);
			if (!FJsonSerializer::Deserialize(Reader, OutJson) || !OutJson.IsValid())
			{
				OutError = FString::Printf(TEXT("Could not parse JSON file: %s"), *FilePath);
				return false;
			}

			return true;
		}

		bool TryReadFileBytes(const FString& FilePath, TArray<uint8>& OutBytes, FString& OutError) const
		{
			if (!FFileHelper::LoadFileToArray(OutBytes, *FilePath))
			{
				OutError = FString::Printf(TEXT("Could not read file bytes: %s"), *FilePath);
				return false;
			}

			return true;
		}

		void Cleanup()
		{
			IFileManager::Get().DeleteDirectory(*GetSavedRunDir(), false, true);

			for (int32 Index = CreatedPackageNames.Num() - 1; Index >= 0; --Index)
			{
				CleanupPackageByName(CreatedPackageNames[Index]);
			}
			CreatedPackageNames.Empty();
		}

	private:
		FString GetSavedRunDir() const
		{
			return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CortexExportTests"), RunId);
		}

		template <typename AssetType>
		AssetType* CreateAsset(const FString& AssetName)
		{
			const FString PackageName = PackagePathForAsset(RunId, AssetName);
			if (FindPackage(nullptr, *PackageName) != nullptr || FPackageName::DoesPackageExist(PackageName))
			{
				return nullptr;
			}

			UPackage* Package = CreatePackage(*PackageName);
			AssetType* Asset = NewObject<AssetType>(
				Package,
				AssetType::StaticClass(),
				FName(*AssetName),
				RF_Public | RF_Standalone);

			FAssetRegistryModule::AssetCreated(Asset);

			CreatedPackageNames.Add(PackageName);
			return Asset;
		}

		FString RunId;
		TArray<FString> CreatedPackageNames;
	};

	void AddStringArrayField(TSharedRef<FJsonObject> Params, const FString& FieldName, const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		Params->SetArrayField(FieldName, JsonValues);
	}

	TArray<FString> GetRowNamesFromExport(const TArray<TSharedPtr<FJsonValue>>& Rows)
	{
		TArray<FString> RowNames;
		for (const TSharedPtr<FJsonValue>& RowValue : Rows)
		{
			const TSharedPtr<FJsonObject>* RowObject = nullptr;
			if (RowValue.IsValid() && RowValue->TryGetObject(RowObject) && RowObject != nullptr && RowObject->IsValid())
			{
				RowNames.Add((*RowObject)->GetStringField(TEXT("row_name")));
			}
		}

		return RowNames;
	}

	TSharedPtr<FJsonObject> GetRowDataFromExportRow(const TSharedPtr<FJsonObject>& RowObject)
	{
		if (!RowObject.IsValid())
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* RowDataObject = nullptr;
		if (!RowObject->TryGetObjectField(TEXT("row_data"), RowDataObject) || RowDataObject == nullptr || !RowDataObject->IsValid())
		{
			return nullptr;
		}

		return *RowDataObject;
	}

	FString GetExportedTextValue(const TSharedPtr<FJsonObject>& RowObject, const FString& FieldName)
	{
		if (!RowObject.IsValid())
		{
			return TEXT("");
		}

		const TSharedPtr<FJsonObject>* TextObject = nullptr;
		if (!RowObject->TryGetObjectField(FieldName, TextObject) || TextObject == nullptr || !TextObject->IsValid())
		{
			return TEXT("");
		}

		FString Value;
		(*TextObject)->TryGetStringField(TEXT("value"), Value);
		return Value;
	}

	TArray<TSharedPtr<FJsonObject>> GetDataAssetEntries(const TSharedPtr<FJsonObject>& FileJson)
	{
		TArray<TSharedPtr<FJsonObject>> Entries;
		if (!FileJson.IsValid())
		{
			return Entries;
		}

		const TArray<TSharedPtr<FJsonValue>>* DataAssets = nullptr;
		if (!FileJson->TryGetArrayField(TEXT("data_assets"), DataAssets) || DataAssets == nullptr)
		{
			return Entries;
		}

		for (const TSharedPtr<FJsonValue>& EntryValue : *DataAssets)
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			if (EntryValue.IsValid() && EntryValue->TryGetObject(EntryObject) && EntryObject != nullptr && EntryObject->IsValid())
			{
				Entries.Add(*EntryObject);
			}
		}

		return Entries;
	}

	TArray<TSharedPtr<FJsonObject>> GetOmittedAssetEntries(const TSharedPtr<FJsonObject>& FileJson)
	{
		TArray<TSharedPtr<FJsonObject>> Entries;
		if (!FileJson.IsValid())
		{
			return Entries;
		}

		const TArray<TSharedPtr<FJsonValue>>* OmittedAssets = nullptr;
		if (!FileJson->TryGetArrayField(TEXT("omitted_assets"), OmittedAssets) || OmittedAssets == nullptr)
		{
			return Entries;
		}

		for (const TSharedPtr<FJsonValue>& EntryValue : *OmittedAssets)
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			if (EntryValue.IsValid() && EntryValue->TryGetObject(EntryObject) && EntryObject != nullptr && EntryObject->IsValid())
			{
				Entries.Add(*EntryObject);
			}
		}

		return Entries;
	}

	TArray<TSharedPtr<FJsonObject>> GetObjectArrayEntries(const TSharedPtr<FJsonObject>& FileJson, const FString& FieldName)
	{
		TArray<TSharedPtr<FJsonObject>> Entries;
		if (!FileJson.IsValid())
		{
			return Entries;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!FileJson->TryGetArrayField(FieldName, Values) || Values == nullptr)
		{
			return Entries;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			if (Value.IsValid() && Value->TryGetObject(EntryObject) && EntryObject != nullptr && EntryObject->IsValid())
			{
				Entries.Add(*EntryObject);
			}
		}

		return Entries;
	}

	TSharedPtr<FJsonObject> FindEntryByStringField(
		const TArray<TSharedPtr<FJsonObject>>& Entries,
		const FString& FieldName,
		const FString& ExpectedValue)
	{
		for (const TSharedPtr<FJsonObject>& Entry : Entries)
		{
			FString Value;
			if (Entry.IsValid() && Entry->TryGetStringField(FieldName, Value) && Value == ExpectedValue)
			{
				return Entry;
			}
		}

		return nullptr;
	}

	TSharedPtr<FJsonObject> FindSchemaStructEntry(const TSharedPtr<FJsonObject>& FileJson, const FString& StructName)
	{
		return FindEntryByStringField(GetObjectArrayEntries(FileJson, TEXT("structs")), TEXT("struct_name"), StructName);
	}

	TSharedPtr<FJsonObject> FindSchemaClassEntry(const TSharedPtr<FJsonObject>& FileJson, const FString& ClassName)
	{
		return FindEntryByStringField(GetObjectArrayEntries(FileJson, TEXT("data_asset_classes")), TEXT("class_name"), ClassName);
	}

	TSharedPtr<FJsonObject> FindSchemaStringTableEntry(const TSharedPtr<FJsonObject>& FileJson, const FString& StringTablePath)
	{
		return FindEntryByStringField(GetObjectArrayEntries(FileJson, TEXT("string_tables")), TEXT("string_table_path"), StringTablePath);
	}

	TSharedPtr<FJsonObject> FindFieldSchema(const TSharedPtr<FJsonObject>& SchemaObject, const FString& FieldName)
	{
		if (!SchemaObject.IsValid())
		{
			return nullptr;
		}

		const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
		if (!SchemaObject->TryGetArrayField(TEXT("fields"), Fields) || Fields == nullptr)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& FieldValue : *Fields)
		{
			const TSharedPtr<FJsonObject>* FieldObject = nullptr;
			if (FieldValue.IsValid() && FieldValue->TryGetObject(FieldObject) && FieldObject != nullptr && FieldObject->IsValid())
			{
				FString Name;
				if ((*FieldObject)->TryGetStringField(TEXT("name"), Name) && Name == FieldName)
				{
					return *FieldObject;
				}
			}
		}

		return nullptr;
	}

	bool StructCatalogContains(const TSharedPtr<FJsonObject>& FileJson, const FString& StructName)
	{
		return FindSchemaStructEntry(FileJson, StructName).IsValid();
	}

	bool SchemaEntryHasProperty(const TSharedPtr<FJsonObject>& FileJson, const FString& ClassName, const FString& PropertyName)
	{
		const TSharedPtr<FJsonObject> ClassEntry = FindSchemaClassEntry(FileJson, ClassName);
		if (!ClassEntry.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!ClassEntry->TryGetArrayField(TEXT("properties"), Properties) || Properties == nullptr)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& PropertyValue : *Properties)
		{
			const TSharedPtr<FJsonObject>* PropertyObject = nullptr;
			if (PropertyValue.IsValid() && PropertyValue->TryGetObject(PropertyObject) && PropertyObject != nullptr && PropertyObject->IsValid())
			{
				FString Name;
				if ((*PropertyObject)->TryGetStringField(TEXT("name"), Name) && Name == PropertyName)
				{
					return true;
				}
			}
		}

		return false;
	}

	bool WarningsContainSubstring(const TSharedPtr<FJsonObject>& JsonObject, const FString& Needle)
	{
		if (!JsonObject.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
		if (!JsonObject->TryGetArrayField(TEXT("warnings"), Warnings) || Warnings == nullptr)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& WarningValue : *Warnings)
		{
			FString Warning;
			if (WarningValue.IsValid() && WarningValue->TryGetString(Warning) && Warning.Contains(Needle))
			{
				return true;
			}
		}

		return false;
	}

	FString MakeMissingDataAssetObjectPath(const UDataAsset* Asset)
	{
		const FString MissingAssetName = TEXT("DA_CortexExportMissing");
		const FString MissingPackageName = FPaths::Combine(FPaths::GetPath(Asset->GetOutermost()->GetName()), MissingAssetName);
		return FString::Printf(TEXT("%s.%s"), *MissingPackageName, *MissingAssetName);
	}

	TSharedRef<FJsonObject> MakeBulkDataTableItem(
		const FString& Name,
		const FString& TablePath,
		const FString& OutPath)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("type"), TEXT("datatable"));
		if (!Name.IsEmpty())
		{
			Item->SetStringField(TEXT("name"), Name);
		}
		Item->SetStringField(TEXT("table_path"), TablePath);
		if (!OutPath.IsEmpty())
		{
			Item->SetStringField(TEXT("out_path"), OutPath);
		}
		return Item;
	}

	TSharedRef<FJsonObject> MakeBulkStringTableItem(
		const FString& Name,
		const FString& StringTablePath,
		const FString& OutPath)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("type"), TEXT("string_table"));
		if (!Name.IsEmpty())
		{
			Item->SetStringField(TEXT("name"), Name);
		}
		Item->SetStringField(TEXT("string_table_path"), StringTablePath);
		if (!OutPath.IsEmpty())
		{
			Item->SetStringField(TEXT("out_path"), OutPath);
		}
		return Item;
	}

	TSharedRef<FJsonObject> MakeBulkDataAssetsItem(
		const FString& Name,
		const FString& OutPath,
		const TArray<FString>& AssetPaths,
		bool bIncludeProperties,
		bool bAllowPartial)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("type"), TEXT("data_assets"));
		if (!Name.IsEmpty())
		{
			Item->SetStringField(TEXT("name"), Name);
		}
		if (!OutPath.IsEmpty())
		{
			Item->SetStringField(TEXT("out_path"), OutPath);
		}
		Item->SetStringField(TEXT("class_name"), TEXT("CortexTestDataAsset"));
		AddStringArrayField(Item, TEXT("asset_paths"), AssetPaths);
		Item->SetBoolField(TEXT("include_properties"), bIncludeProperties);
		Item->SetBoolField(TEXT("allow_partial"), bAllowPartial);
		return Item;
	}

	void AddBulkItem(TArray<TSharedPtr<FJsonValue>>& Items, const TSharedRef<FJsonObject>& Item)
	{
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	TArray<TSharedPtr<FJsonObject>> GetBulkItemSummaries(const TSharedPtr<FJsonObject>& Data)
	{
		TArray<TSharedPtr<FJsonObject>> Summaries;
		if (!Data.IsValid())
		{
			return Summaries;
		}

		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Data->TryGetArrayField(TEXT("items"), Items) || Items == nullptr)
		{
			return Summaries;
		}

		for (const TSharedPtr<FJsonValue>& ItemValue : *Items)
		{
			const TSharedPtr<FJsonObject>* ItemObject = nullptr;
			if (ItemValue.IsValid() && ItemValue->TryGetObject(ItemObject) && ItemObject != nullptr && ItemObject->IsValid())
			{
				Summaries.Add(*ItemObject);
			}
		}

		return Summaries;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportFixtureSmokeTest,
	"Cortex.Data.Export.FixtureSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportFixtureSmokeTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;

	UDataTable* RegularTable = Fixture.CreateRegularDataTable();
	TestNotNull(TEXT("regular DataTable fixture is created"), RegularTable);
	if (RegularTable != nullptr)
	{
		TestEqual(TEXT("regular fixture has deterministic row count"), RegularTable->GetRowMap().Num(), 3);
	}

	UCompositeDataTable* CompositeTable = Fixture.CreateCompositeDataTable();
	TestNotNull(TEXT("CompositeDataTable fixture is created"), CompositeTable);
	if (CompositeTable != nullptr)
	{
		const uint8* SharedRowData = CompositeTable->FindRowUnchecked(TEXT("shared"));
		TestNotNull(TEXT("composite fixture has overridden shared row"), SharedRowData);
		if (SharedRowData != nullptr)
		{
			const FCortexDataLocalizationTestRow* SharedRow = reinterpret_cast<const FCortexDataLocalizationTestRow*>(SharedRowData);
			TestEqual(TEXT("later composite parent overrides earlier parent"), SharedRow->Title.ToString(), TEXT("Override Shared"));
		}
	}

	UStringTable* StringTable = Fixture.CreateStringTable();
	TestNotNull(TEXT("StringTable fixture is created"), StringTable);
	if (StringTable != nullptr)
	{
		FString SourceString;
		const bool bFoundAlpha = StringTable->GetStringTable()->GetSourceString(TEXT("alpha.key"), SourceString);
		TestTrue(TEXT("StringTable fixture contains out-of-order alpha key"), bFoundAlpha);
		TestEqual(TEXT("StringTable alpha key has deterministic value"), SourceString, TEXT("Alpha text"));
	}

	UCortexTestDataAsset* DataAsset = Fixture.CreateDataAsset();
	TestNotNull(TEXT("DataAsset fixture is created"), DataAsset);
	if (DataAsset != nullptr)
	{
		TestEqual(TEXT("DataAsset editable string is populated"), DataAsset->TestProperty, TEXT("Editable export value"));
		TestEqual(TEXT("DataAsset editable number is populated"), DataAsset->TestNumber, 42);
		TestEqual(TEXT("DataAsset transient blocked field is populated"), DataAsset->ExportTransientProperty, TEXT("Transient value"));
		TestEqual(TEXT("DataAsset non-editable transient blocked field is populated"), DataAsset->TransientExportBlocked, TEXT("Non-editable transient value"));
#if WITH_EDITORONLY_DATA
		TestEqual(TEXT("DataAsset editor-only blocked field is populated"), DataAsset->ExportEditorOnlyProperty, TEXT("Editor-only value"));
#endif
		TestEqual(TEXT("DataAsset internal blocked field is populated"), DataAsset->ExportInternalProperty, TEXT("Internal value"));
		TestEqual(TEXT("DataAsset editable export field is populated"), DataAsset->EditableExportAllowed, TEXT("Editable export allowed value"));
	}

	const FString ProbePath = Fixture.MakeSavedOutputPath(TEXT("probe.json"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ProbePath), true);
	TestTrue(TEXT("probe JSON file writes to Saved/CortexExportTests"),
		FFileHelper::SaveStringToFile(TEXT("{\"ok\":true}"), *ProbePath));

	TSharedPtr<FJsonObject> ParsedProbe;
	FString ParseError;
	TestTrue(TEXT("fixture helper parses written JSON from disk"), Fixture.TryReadJsonFile(ProbePath, ParsedProbe, ParseError));
	if (!ParseError.IsEmpty())
	{
		AddError(ParseError);
	}
	if (ParsedProbe.IsValid())
	{
		TestTrue(TEXT("parsed probe JSON contains expected bool"), ParsedProbe->GetBoolField(TEXT("ok")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportCommandsRegisteredTest,
	"Cortex.Data.Export.CommandsRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportCommandsRegisteredTest::RunTest(const FString& Parameters)
{
	FCortexDataCommandHandler Handler;
	const TArray<FCortexCommandInfo> Commands = Handler.GetSupportedCommands();

	TestTrue(TEXT("export_datatable_json is advertised"),
		SupportedCommandNamesContain(Commands, TEXT("export_datatable_json")));
	TestTrue(TEXT("export_string_table_json is advertised"),
		SupportedCommandNamesContain(Commands, TEXT("export_string_table_json")));
	TestTrue(TEXT("export_data_assets_json is advertised"),
		SupportedCommandNamesContain(Commands, TEXT("export_data_assets_json")));
	TestTrue(TEXT("export_bulk_json is advertised"),
		SupportedCommandNamesContain(Commands, TEXT("export_bulk_json")));

	FCortexCommandRouter Router = CreateDataExportTestRouter();
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("table_path"), TEXT("/Game/CortexExportTests/Missing.Missing"));
	Params->SetStringField(TEXT("out_path"), FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CortexExportTests"), TEXT("registered.json")));

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_datatable_json"), Params);
	TestFalse(TEXT("export_datatable_json is registered and validates the missing table"), Result.bSuccess);
	TestEqual(TEXT("registered command returns domain error, not UnknownCommand"), Result.ErrorCode, CortexErrorCodes::TableNotFound);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataSchemaExportCommandsRegisteredTest,
	"Cortex.Data.Export.Schema.CommandsRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataSchemaExportCommandsRegisteredTest::RunTest(const FString& Parameters)
{
	FCortexDataCommandHandler Handler;
	const TArray<FCortexCommandInfo> Commands = Handler.GetSupportedCommands();

	TestTrue(TEXT("export_schema_json is advertised"),
		SupportedCommandNamesContain(Commands, TEXT("export_schema_json")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataSchemaExportSummaryOmitsPayloadTest,
	"Cortex.Data.Export.Schema.SummaryOmitsPayload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataSchemaExportSummaryOmitsPayloadTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	FCortexCommandRouter Router = CreateDataExportTestRouter();

	UDataTable* Table = Fixture.CreateSchemaAnnotatedDataTable();
	TestNotNull(TEXT("schema table fixture is created"), Table);
	if (Table == nullptr)
	{
		return false;
	}

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("out_path"), Fixture.MakeSavedOutputPath(TEXT("schema-summary.json")));

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_schema_json"), Params);
	TestTrue(TEXT("schema export succeeds"), Result.bSuccess);
	TestTrue(TEXT("schema summary returns data"), Result.Data.IsValid());
	if (!Result.Data.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("summary omits datatables payload"), !Result.Data->HasField(TEXT("datatables")));
	TestTrue(TEXT("summary omits structs payload"), !Result.Data->HasField(TEXT("structs")));
	TestTrue(TEXT("summary has counts"), Result.Data->HasTypedField<EJson::Object>(TEXT("counts")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataSchemaExportWritesCanonicalSnapshotTest,
	"Cortex.Data.Export.Schema.WritesCanonicalSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataSchemaExportWritesCanonicalSnapshotTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	FCortexCommandRouter Router = CreateDataExportTestRouter();

	UDataTable* Table = Fixture.CreateSchemaAnnotatedDataTable();
	UStringTable* StringTable = Fixture.CreateStringTable();
	UCortexDerivedTestDataAsset* DataAsset = Fixture.CreateDerivedSchemaDataAsset(TEXT("DA_SchemaDerived"));
	TestNotNull(TEXT("schema DataTable exists"), Table);
	TestNotNull(TEXT("schema StringTable exists"), StringTable);
	TestNotNull(TEXT("schema DataAsset exists"), DataAsset);
	if (Table == nullptr || StringTable == nullptr || DataAsset == nullptr)
	{
		return false;
	}

	const FString OutPath = Fixture.MakeSavedOutputPath(TEXT("schema-canonical.json"));
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("out_path"), OutPath);

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_schema_json"), Params);
	TestTrue(TEXT("schema export succeeds"), Result.bSuccess);

	TSharedPtr<FJsonObject> FileJson;
	FString ParseError;
	TestTrue(TEXT("schema file parses"), Fixture.TryReadJsonFile(OutPath, FileJson, ParseError));
	if (!ParseError.IsEmpty())
	{
		AddError(ParseError);
	}
	if (FileJson.IsValid())
	{
		TestEqual(TEXT("schema version is 1"), static_cast<int32>(FileJson->GetNumberField(TEXT("schema_version"))), 1);
		TestTrue(TEXT("datatables array exists"), FileJson->HasTypedField<EJson::Array>(TEXT("datatables")));
		TestTrue(TEXT("structs array exists"), FileJson->HasTypedField<EJson::Array>(TEXT("structs")));
		TestTrue(TEXT("data_asset_classes array exists"), FileJson->HasTypedField<EJson::Array>(TEXT("data_asset_classes")));
		TestTrue(TEXT("string_tables array exists"), FileJson->HasTypedField<EJson::Array>(TEXT("string_tables")));
		TestFalse(TEXT("gameplay tags section is absent in v1"), FileJson->HasField(TEXT("gameplay_tags")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataSchemaExportSelectorDatatablesTest,
	"Cortex.Data.Export.Schema.Selectors.Datatables",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataSchemaExportSelectorDatatablesTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	FCortexCommandRouter Router = CreateDataExportTestRouter();

	UDataTable* Table = Fixture.CreateSchemaAnnotatedDataTable();
	TestNotNull(TEXT("schema table exists"), Table);
	if (Table == nullptr)
	{
		return false;
	}

	const FString OutPath = Fixture.MakeSavedOutputPath(TEXT("schema-datatable-selector.json"));
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("out_path"), OutPath);
	AddStringArrayField(Params, TEXT("datatable_paths"), TArray<FString>{ Table->GetPathName() });

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_schema_json"), Params);
	TestTrue(TEXT("schema export succeeds"), Result.bSuccess);

	TSharedPtr<FJsonObject> FileJson;
	FString ParseError;
	TestTrue(TEXT("schema file parses"), Fixture.TryReadJsonFile(OutPath, FileJson, ParseError));
	if (FileJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonObject>> Datatables = GetObjectArrayEntries(FileJson, TEXT("datatables"));
		TestEqual(TEXT("selector export writes one DataTable entry"), Datatables.Num(), 1);
		if (Datatables.Num() == 1)
		{
			TestEqual(TEXT("DataTable entry keeps table path"), Datatables[0]->GetStringField(TEXT("table_path")), Table->GetPathName());
			TestEqual(TEXT("DataTable entry keeps row struct"), Datatables[0]->GetStringField(TEXT("row_struct")), TEXT("FCortexSchemaAnnotatedRow"));
			TestTrue(TEXT("DataTable entry emits inline fields"), Datatables[0]->HasTypedField<EJson::Array>(TEXT("fields")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataSchemaExportSelectorStructsTest,
	"Cortex.Data.Export.Schema.Selectors.Structs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataSchemaExportSelectorStructsTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	FCortexCommandRouter Router = CreateDataExportTestRouter();
	const FString OutPath = Fixture.MakeSavedOutputPath(TEXT("schema-struct-selector.json"));

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("out_path"), OutPath);
	AddStringArrayField(Params, TEXT("struct_names"), TArray<FString>{ TEXT("FCortexSchemaParentStruct") });

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_schema_json"), Params);
	TestTrue(TEXT("explicit struct export succeeds"), Result.bSuccess);

	TSharedPtr<FJsonObject> FileJson;
	FString ParseError;
	TestTrue(TEXT("explicit struct schema file parses"), Fixture.TryReadJsonFile(OutPath, FileJson, ParseError));
	if (FileJson.IsValid())
	{
		TestTrue(TEXT("explicit struct entry exists"), StructCatalogContains(FileJson, TEXT("FCortexSchemaParentStruct")));
		TestTrue(TEXT("nested struct closure exists"), StructCatalogContains(FileJson, TEXT("FCortexSchemaLeafStruct")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataSchemaExportSelectorDataAssetClassesTest,
	"Cortex.Data.Export.Schema.Selectors.DataAssetClasses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataSchemaExportSelectorDataAssetClassesTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	FCortexCommandRouter Router = CreateDataExportTestRouter();

	UCortexDerivedTestDataAsset* DataAsset = Fixture.CreateDerivedSchemaDataAsset();
	TestNotNull(TEXT("derived DataAsset exists"), DataAsset);
	if (DataAsset == nullptr)
	{
		return false;
	}

	const FString OutPath = Fixture.MakeSavedOutputPath(TEXT("schema-dataasset-selector.json"));
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("out_path"), OutPath);
	AddStringArrayField(Params, TEXT("data_asset_classes"), TArray<FString>{ DataAsset->GetClass()->GetPathName() });

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_schema_json"), Params);
	TestTrue(TEXT("data asset class selector succeeds"), Result.bSuccess);

	TSharedPtr<FJsonObject> FileJson;
	FString ParseError;
	TestTrue(TEXT("data asset class selector file parses"), Fixture.TryReadJsonFile(OutPath, FileJson, ParseError));
	if (FileJson.IsValid())
	{
		TestTrue(TEXT("derived class entry exists"), FindSchemaClassEntry(FileJson, TEXT("CortexDerivedTestDataAsset")).IsValid());
		TestTrue(TEXT("include_inherited=true keeps base property"),
			SchemaEntryHasProperty(FileJson, TEXT("CortexDerivedTestDataAsset"), TEXT("TestProperty")));
		TestTrue(TEXT("include_inherited=true keeps derived property"),
			SchemaEntryHasProperty(FileJson, TEXT("CortexDerivedTestDataAsset"), TEXT("DerivedOnlyProperty")));
	}

	const FString NoInheritedOutPath = Fixture.MakeSavedOutputPath(TEXT("schema-dataasset-selector-no-inherited.json"));
	Params->SetStringField(TEXT("out_path"), NoInheritedOutPath);
	Params->SetBoolField(TEXT("include_inherited"), false);

	const FCortexCommandResult NoInheritedResult = Router.Execute(TEXT("data.export_schema_json"), Params);
	TestTrue(TEXT("include_inherited=false succeeds"), NoInheritedResult.bSuccess);

	TSharedPtr<FJsonObject> NoInheritedFileJson;
	ParseError.Reset();
	TestTrue(TEXT("include_inherited=false schema file parses"), Fixture.TryReadJsonFile(NoInheritedOutPath, NoInheritedFileJson, ParseError));
	if (NoInheritedFileJson.IsValid())
	{
		TestFalse(TEXT("include_inherited=false omits base property"),
			SchemaEntryHasProperty(NoInheritedFileJson, TEXT("CortexDerivedTestDataAsset"), TEXT("TestProperty")));
		TestTrue(TEXT("include_inherited=false keeps derived property"),
			SchemaEntryHasProperty(NoInheritedFileJson, TEXT("CortexDerivedTestDataAsset"), TEXT("DerivedOnlyProperty")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataSchemaExportSelectorStringTablesTest,
	"Cortex.Data.Export.Schema.Selectors.StringTables",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataSchemaExportSelectorStringTablesTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	FCortexCommandRouter Router = CreateDataExportTestRouter();

	UStringTable* StringTable = Fixture.CreateStringTable();
	TestNotNull(TEXT("schema StringTable exists"), StringTable);
	if (StringTable == nullptr)
	{
		return false;
	}

	const FString OutPath = Fixture.MakeSavedOutputPath(TEXT("schema-string-table-selector.json"));
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("out_path"), OutPath);
	AddStringArrayField(Params, TEXT("string_table_paths"), TArray<FString>{ StringTable->GetPathName() });

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_schema_json"), Params);
	TestTrue(TEXT("string table selector succeeds"), Result.bSuccess);

	TSharedPtr<FJsonObject> FileJson;
	FString ParseError;
	TestTrue(TEXT("string table selector file parses"), Fixture.TryReadJsonFile(OutPath, FileJson, ParseError));
	if (FileJson.IsValid())
	{
		const TSharedPtr<FJsonObject> Entry = FindSchemaStringTableEntry(FileJson, StringTable->GetPathName());
		TestTrue(TEXT("string table metadata entry exists"), Entry.IsValid());
		if (Entry.IsValid())
		{
			TestTrue(TEXT("string table metadata includes entry_count"), Entry->HasTypedField<EJson::Number>(TEXT("entry_count")));
			TestFalse(TEXT("string table metadata omits raw entries"), Entry->HasField(TEXT("entries")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataSchemaExportInstancedStructCoverageTest,
	"Cortex.Data.Export.Schema.InstancedStructCoverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataSchemaExportInstancedStructCoverageTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	FCortexCommandRouter Router = CreateDataExportTestRouter();

	UDataTable* AnnotatedTable = Fixture.CreateSchemaAnnotatedDataTable();
	UDataTable* UnannotatedTable = Fixture.CreateSchemaUnannotatedDataTable();
	UCortexSchemaInstancedDataAsset* InstancedAsset = Fixture.CreateSchemaInstancedDataAsset();
	TestNotNull(TEXT("annotated table exists"), AnnotatedTable);
	TestNotNull(TEXT("unannotated table exists"), UnannotatedTable);
	TestNotNull(TEXT("instanced asset exists"), InstancedAsset);
	if (AnnotatedTable == nullptr || UnannotatedTable == nullptr || InstancedAsset == nullptr)
	{
		return false;
	}

	const FString OutPath = Fixture.MakeSavedOutputPath(TEXT("schema-instanced-structs.json"));
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("out_path"), OutPath);

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_schema_json"), Params);
	TestTrue(TEXT("instanced struct schema export succeeds"), Result.bSuccess);

	TSharedPtr<FJsonObject> FileJson;
	FString ParseError;
	TestTrue(TEXT("instanced struct schema file parses"), Fixture.TryReadJsonFile(OutPath, FileJson, ParseError));
	if (FileJson.IsValid())
	{
		TestTrue(TEXT("annotated instanced base schema exists"), StructCatalogContains(FileJson, TEXT("FCortexSchemaInstancedBase")));
		TestTrue(TEXT("annotated instanced subtype schema exists"), StructCatalogContains(FileJson, TEXT("FCortexSchemaInstancedDerived")));
		TestTrue(TEXT("unannotated snapshot is partial"), FileJson->GetBoolField(TEXT("partial")));
		const TSharedPtr<FJsonObject> UnannotatedStruct = FindSchemaStructEntry(FileJson, TEXT("FCortexSchemaUnannotatedRow"));
		const TSharedPtr<FJsonObject> PayloadField = FindFieldSchema(UnannotatedStruct, TEXT("Payload"));
		TestTrue(TEXT("unannotated field marks partial"), PayloadField.IsValid() && PayloadField->GetBoolField(TEXT("partial")));
		TestTrue(TEXT("unannotated warnings mention BaseStruct"), WarningsContainSubstring(FileJson, TEXT("BaseStruct")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataSchemaExportInvalidPathRejectedTest,
	"Cortex.Data.Export.Schema.InvalidPathRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataSchemaExportInvalidPathRejectedTest::RunTest(const FString& Parameters)
{
	FCortexCommandRouter Router = CreateDataExportTestRouter();

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("out_path"), TEXT("../../schema-invalid.json"));

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_schema_json"), Params);
	TestFalse(TEXT("invalid out_path is rejected"), Result.bSuccess);
	TestEqual(TEXT("invalid out_path uses InvalidField"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataSchemaExportMalformedSelectorsRejectedTest,
	"Cortex.Data.Export.Schema.MalformedSelectorsRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataSchemaExportMalformedSelectorsRejectedTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	FCortexCommandRouter Router = CreateDataExportTestRouter();

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("out_path"), Fixture.MakeSavedOutputPath(TEXT("schema-bad-selectors.json")));

	TArray<TSharedPtr<FJsonValue>> BadSelectors;
	BadSelectors.Add(MakeShared<FJsonValueNumber>(42.0));
	Params->SetArrayField(TEXT("datatable_paths"), BadSelectors);

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_schema_json"), Params);
	TestFalse(TEXT("malformed selector array is rejected"), Result.bSuccess);
	TestEqual(TEXT("malformed selector uses InvalidField"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataSchemaExportDeterministicRepeatedRunsTest,
	"Cortex.Data.Export.Schema.DeterministicRepeatedRuns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataSchemaExportDeterministicRepeatedRunsTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	FCortexCommandRouter Router = CreateDataExportTestRouter();

	UDataTable* Table = Fixture.CreateSchemaAnnotatedDataTable();
	UStringTable* StringTable = Fixture.CreateStringTable();
	UCortexDerivedTestDataAsset* DataAsset = Fixture.CreateDerivedSchemaDataAsset();
	TestNotNull(TEXT("schema DataTable exists"), Table);
	TestNotNull(TEXT("schema StringTable exists"), StringTable);
	TestNotNull(TEXT("schema DataAsset exists"), DataAsset);
	if (Table == nullptr || StringTable == nullptr || DataAsset == nullptr)
	{
		return false;
	}

	const FString FirstOutPath = Fixture.MakeSavedOutputPath(TEXT("schema-first.json"));
	const FString SecondOutPath = Fixture.MakeSavedOutputPath(TEXT("schema-second.json"));

	auto ExecuteSchemaExport = [&Router](const FString& OutPath)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("out_path"), OutPath);
		return Router.Execute(TEXT("data.export_schema_json"), Params);
	};

	const FCortexCommandResult FirstResult = ExecuteSchemaExport(FirstOutPath);
	const FCortexCommandResult SecondResult = ExecuteSchemaExport(SecondOutPath);
	TestTrue(TEXT("first schema export succeeds"), FirstResult.bSuccess);
	TestTrue(TEXT("second schema export succeeds"), SecondResult.bSuccess);

	TArray<uint8> FirstBytes;
	TArray<uint8> SecondBytes;
	FString Error;
	TestTrue(TEXT("first schema file bytes read"), Fixture.TryReadFileBytes(FirstOutPath, FirstBytes, Error));
	TestTrue(TEXT("second schema file bytes read"), Fixture.TryReadFileBytes(SecondOutPath, SecondBytes, Error));
	TestEqual(TEXT("repeated schema exports are byte-identical"), FirstBytes.Num(), SecondBytes.Num());
	TestTrue(TEXT("repeated schema exports compare equal"), FirstBytes == SecondBytes);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportPathSafetyTest,
	"Cortex.Data.Export.PathSafety",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportPathSafetyTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	FCortexCommandRouter Router = CreateDataExportTestRouter();

	auto ExecuteDatatableExport = [&Router](const FString& OutPath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("table_path"), TEXT("/Game/CortexExportTests/Missing.Missing"));
		Params->SetStringField(TEXT("out_path"), OutPath);
		return Router.Execute(TEXT("data.export_datatable_json"), Params);
	};

	const FString TraversalBase = Fixture.MakeSavedOutputDir(TEXT("TraversalBase"));
	const FString TraversalPath = FPaths::Combine(TraversalBase, TEXT(".."), TEXT("TraversalEscape"), TEXT("out.json"));
	const FString UnexpectedTraversalDirectory = FPaths::Combine(FPaths::GetPath(TraversalBase), TEXT("TraversalEscape"));
	const FCortexCommandResult TraversalResult = ExecuteDatatableExport(TraversalPath);
	TestFalse(TEXT("Traversal output paths are rejected"), TraversalResult.bSuccess);
	TestEqual(TEXT("Traversal rejection uses InvalidField"), TraversalResult.ErrorCode, CortexErrorCodes::InvalidField);
	TestFalse(TEXT("Traversal rejection creates no directory"), IFileManager::Get().DirectoryExists(*UnexpectedTraversalDirectory));

	FString ProjectRootForSibling = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::NormalizeDirectoryName(ProjectRootForSibling);
	const FString SiblingEscapePath = ProjectRootForSibling + TEXT("_Evil/out.json");
	const FCortexCommandResult SiblingResult = ExecuteDatatableExport(SiblingEscapePath);
	TestFalse(TEXT("Sibling-prefix output paths are rejected"), SiblingResult.bSuccess);
	TestEqual(TEXT("Sibling-prefix rejection uses InvalidField"), SiblingResult.ErrorCode, CortexErrorCodes::InvalidField);

	const FCortexCommandResult DevicePathResult = ExecuteDatatableExport(TEXT("\\\\?\\C:\\CortexExportTests\\out.json"));
	TestFalse(TEXT("Win32 device output paths are rejected"), DevicePathResult.bSuccess);
	TestEqual(TEXT("Win32 device rejection uses InvalidField"), DevicePathResult.ErrorCode, CortexErrorCodes::InvalidField);

	const FCortexCommandResult DotDevicePathResult = ExecuteDatatableExport(TEXT("\\\\.\\C:\\CortexExportTests\\out.json"));
	TestFalse(TEXT("Win32 dot-device output paths are rejected"), DotDevicePathResult.bSuccess);
	TestEqual(TEXT("Win32 dot-device rejection uses InvalidField"), DotDevicePathResult.ErrorCode, CortexErrorCodes::InvalidField);

	const FCortexCommandResult UncPathResult = ExecuteDatatableExport(TEXT("\\\\server\\share\\out.json"));
	TestFalse(TEXT("UNC output paths are rejected"), UncPathResult.bSuccess);
	TestEqual(TEXT("UNC path rejection uses InvalidField"), UncPathResult.ErrorCode, CortexErrorCodes::InvalidField);

	const FCortexCommandResult DriveRelativePathResult = ExecuteDatatableExport(TEXT("C:relative\\out.json"));
	TestFalse(TEXT("Drive-relative output paths are rejected"), DriveRelativePathResult.bSuccess);
	TestEqual(TEXT("Drive-relative path rejection uses InvalidField"), DriveRelativePathResult.ErrorCode, CortexErrorCodes::InvalidField);

	const FString ExistingDirectoryTarget = Fixture.MakeSavedOutputDir(TEXT("ExistingDirectoryTarget"));
	IFileManager::Get().MakeDirectory(*ExistingDirectoryTarget, true);
	const FCortexCommandResult ExistingDirectoryResult = ExecuteDatatableExport(ExistingDirectoryTarget);
	TestFalse(TEXT("Existing directory output targets are rejected"), ExistingDirectoryResult.bSuccess);
	TestEqual(TEXT("Existing directory rejection uses InvalidField"), ExistingDirectoryResult.ErrorCode, CortexErrorCodes::InvalidField);

	const FString ExternalTargetDir = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("CortexExportSymlinkTarget"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString LinkDir = Fixture.MakeSavedOutputDir(TEXT("SymlinkParent"));
	IFileManager::Get().MakeDirectory(*ExternalTargetDir, true);
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*ExternalTargetDir, false, true);
	};

#if PLATFORM_WINDOWS
	FString StdOut;
	FString StdErr;
	int32 MklinkExitCode = INDEX_NONE;
	const FString MklinkArgs = FString::Printf(TEXT("/C mklink /J \"%s\" \"%s\""), *LinkDir, *ExternalTargetDir);
	const bool bMklinkStarted = FPlatformProcess::ExecProcess(TEXT("cmd.exe"), *MklinkArgs, &MklinkExitCode, &StdOut, &StdErr);
	if (!bMklinkStarted || MklinkExitCode != 0 || !IFileManager::Get().DirectoryExists(*LinkDir))
	{
		AddInfo(FString::Printf(TEXT("Skipping symlink/junction escape test; mklink /J failed with code %d: %s %s"), MklinkExitCode, *StdOut, *StdErr));
		return true;
	}
#else
	AddInfo(TEXT("Skipping symlink/junction escape test on this platform"));
	return true;
#endif

	const FCortexCommandResult SymlinkResult = ExecuteDatatableExport(FPaths::Combine(LinkDir, TEXT("NewDir"), TEXT("out.json")));
	TestFalse(TEXT("Symlink/junction parent escapes are rejected"), SymlinkResult.bSuccess);
	TestEqual(TEXT("Symlink/junction rejection uses InvalidField"), SymlinkResult.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportDatatableProjectionSchemaTest,
	"Cortex.Data.Export.Datatable.ProjectionSchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportDatatableProjectionSchemaTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	UDataTable* RegularTable = Fixture.CreateRegularDataTable();
	TestNotNull(TEXT("regular DataTable fixture is created"), RegularTable);
	if (RegularTable == nullptr)
	{
		return true;
	}

	FCortexCommandRouter Router = CreateDataExportTestRouter();
	const FString OutPath = Fixture.MakeSavedOutputPath(TEXT("projection-schema.json"));

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("table_path"), RegularTable->GetPathName());
	Params->SetStringField(TEXT("out_path"), OutPath);
	Params->SetStringField(TEXT("row_name_pattern"), TEXT("*a"));
	Params->SetBoolField(TEXT("include_schema"), true);
	AddStringArrayField(Params, TEXT("fields"), TArray<FString>{ TEXT("Title") });

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_datatable_json"), Params);
	TestTrue(TEXT("DataTable export succeeds"), Result.bSuccess);
	TestTrue(TEXT("DataTable export returns data"), Result.Data.IsValid());

	if (Result.Data.IsValid())
	{
		TestTrue(TEXT("summary omits raw rows"), !Result.Data->HasField(TEXT("rows")));
		TestTrue(TEXT("summary remains compact"), !Result.Data->HasField(TEXT("schema")));
		TestTrue(TEXT("summary reports byte count"), Result.Data->GetNumberField(TEXT("bytes_written")) > 0.0);
		TestEqual(TEXT("summary reports exported count"), static_cast<int32>(Result.Data->GetNumberField(TEXT("exported_count"))), 2);
	}

	TSharedPtr<FJsonObject> FileJson;
	FString ParseError;
	TestTrue(TEXT("DataTable export file parses"), Fixture.TryReadJsonFile(OutPath, FileJson, ParseError));
	if (!ParseError.IsEmpty())
	{
		AddError(ParseError);
	}

	if (FileJson.IsValid())
	{
		TestTrue(TEXT("file contains stable row array"), FileJson->HasTypedField<EJson::Array>(TEXT("rows")));
		TestTrue(TEXT("file contains row_struct"), FileJson->HasTypedField<EJson::String>(TEXT("row_struct")));
		TestTrue(TEXT("file contains schema only when requested"), FileJson->HasTypedField<EJson::Object>(TEXT("schema")));
		TestTrue(TEXT("file contains projected fields array"), FileJson->HasTypedField<EJson::Array>(TEXT("fields")));
		TestEqual(TEXT("total filtered count is reported"), static_cast<int32>(FileJson->GetNumberField(TEXT("total_count"))), 2);
		TestEqual(TEXT("exported count is reported"), static_cast<int32>(FileJson->GetNumberField(TEXT("exported_count"))), 2);

		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (FileJson->TryGetArrayField(TEXT("rows"), Rows) && Rows != nullptr)
		{
			TestEqual(TEXT("row_name_pattern filters matching rows"), Rows->Num(), 2);
			TestTrue(TEXT("filtered rows are sorted by row_name"), GetRowNamesFromExport(*Rows) == TArray<FString>{ TEXT("alpha"), TEXT("zeta") });

			for (const TSharedPtr<FJsonValue>& RowValue : *Rows)
			{
				const TSharedPtr<FJsonObject>* RowObject = nullptr;
				if (RowValue.IsValid() && RowValue->TryGetObject(RowObject) && RowObject != nullptr && RowObject->IsValid())
				{
					TSharedPtr<FJsonObject> RowData = GetRowDataFromExportRow(*RowObject);
					TestTrue(TEXT("row wrapper includes row_name metadata"), (*RowObject)->HasTypedField<EJson::String>(TEXT("row_name")));
					TestTrue(TEXT("row wrapper includes row_data object"), RowData.IsValid());
					if (RowData.IsValid())
					{
						TestTrue(TEXT("projected row_data includes requested Title"), RowData->HasField(TEXT("Title")));
						TestFalse(TEXT("projected row_data omits unrequested Steps"), RowData->HasField(TEXT("Steps")));
						TestFalse(TEXT("projected row_data omits unrequested row_name field"), RowData->HasField(TEXT("row_name")));
					}
				}
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportRelativeSavedPathTest,
	"Cortex.Data.Export.PathSafety.RelativeSavedPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportRelativeSavedPathTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	UDataTable* RegularTable = Fixture.CreateRegularDataTable();
	TestNotNull(TEXT("regular DataTable fixture is created"), RegularTable);
	if (RegularTable == nullptr)
	{
		return true;
	}

	const FString RelativeOutPath = TEXT("Saved/CortexExportTests/relative-saved.json");
	const FString ExpectedOutPath = FPaths::Combine(FPaths::ProjectDir(), RelativeOutPath);
	const FString UnexpectedNestedPath = FPaths::Combine(FPaths::ProjectSavedDir(), RelativeOutPath);

	IFileManager::Get().Delete(*ExpectedOutPath, false, true);
	IFileManager::Get().Delete(*UnexpectedNestedPath, false, true);

	FCortexCommandRouter Router = CreateDataExportTestRouter();
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("table_path"), RegularTable->GetPathName());
	Params->SetStringField(TEXT("out_path"), RelativeOutPath);

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_datatable_json"), Params);
	TestTrue(TEXT("DataTable export succeeds with project-relative Saved path"), Result.bSuccess);
	TestTrue(TEXT("project-relative Saved path is written under project Saved"), IFileManager::Get().FileExists(*ExpectedOutPath));
	TestFalse(TEXT("project-relative Saved path is not nested under Saved/Saved"), IFileManager::Get().FileExists(*UnexpectedNestedPath));

	if (Result.Data.IsValid())
	{
		TestEqual(TEXT("summary reports normalized project Saved path"), Result.Data->GetStringField(TEXT("out_path")), FPaths::ConvertRelativePathToFull(ExpectedOutPath));
	}

	IFileManager::Get().Delete(*ExpectedOutPath, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportDatatableRowWrapperTest,
	"Cortex.Data.Export.Datatable.RowWrapperAvoidsNameCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportDatatableRowWrapperTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	UDataTable* RegularTable = Fixture.CreateRegularDataTable();
	TestNotNull(TEXT("regular DataTable fixture is created"), RegularTable);
	if (RegularTable == nullptr)
	{
		return true;
	}

	FCortexCommandRouter Router = CreateDataExportTestRouter();
	const FString OutPath = Fixture.MakeSavedOutputPath(TEXT("row-wrapper.json"));

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("table_path"), RegularTable->GetPathName());
	Params->SetStringField(TEXT("out_path"), OutPath);
	AddStringArrayField(Params, TEXT("row_names"), TArray<FString>{ TEXT("alpha") });

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_datatable_json"), Params);
	TestTrue(TEXT("DataTable export succeeds"), Result.bSuccess);

	TSharedPtr<FJsonObject> FileJson;
	FString ParseError;
	TestTrue(TEXT("DataTable export file parses"), Fixture.TryReadJsonFile(OutPath, FileJson, ParseError));
	if (!ParseError.IsEmpty())
	{
		AddError(ParseError);
	}

	if (FileJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (FileJson->TryGetArrayField(TEXT("rows"), Rows) && Rows != nullptr)
		{
			TestEqual(TEXT("exact row filter exports one row"), Rows->Num(), 1);
			if (Rows->Num() == 1)
			{
				const TSharedPtr<FJsonObject>* RowObject = nullptr;
				if ((*Rows)[0].IsValid() && (*Rows)[0]->TryGetObject(RowObject) && RowObject != nullptr && RowObject->IsValid())
				{
					TestEqual(TEXT("wrapper row_name is export metadata"), (*RowObject)->GetStringField(TEXT("row_name")), TEXT("alpha"));
					TSharedPtr<FJsonObject> RowData = GetRowDataFromExportRow(*RowObject);
					TestTrue(TEXT("wrapper preserves serialized row_data"), RowData.IsValid());
					if (RowData.IsValid())
					{
						TestEqual(TEXT("row_data preserves real row_name field"), RowData->GetStringField(TEXT("row_name")), TEXT("Data Alpha"));
						TestEqual(TEXT("row_data preserves other fields"), GetExportedTextValue(RowData, TEXT("Title")), TEXT("Alpha"));
					}
				}
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportDatatableMissingExplicitRowTest,
	"Cortex.Data.Export.Datatable.MissingExplicitRowFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportDatatableMissingExplicitRowTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	UDataTable* RegularTable = Fixture.CreateRegularDataTable();
	TestNotNull(TEXT("regular DataTable fixture is created"), RegularTable);
	if (RegularTable == nullptr)
	{
		return true;
	}

	FCortexCommandRouter Router = CreateDataExportTestRouter();
	const FString OutPath = Fixture.MakeSavedOutputPath(TEXT("missing-explicit-row.json"));

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("table_path"), RegularTable->GetPathName());
	Params->SetStringField(TEXT("out_path"), OutPath);
	AddStringArrayField(Params, TEXT("row_names"), TArray<FString>{ TEXT("alpha"), TEXT("missing") });

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_datatable_json"), Params);
	TestFalse(TEXT("missing explicit row_names fail export"), Result.bSuccess);
	TestEqual(TEXT("missing explicit row_names use RowNotFound"), Result.ErrorCode, CortexErrorCodes::RowNotFound);
	TestFalse(TEXT("missing explicit row_names do not write partial file"), IFileManager::Get().FileExists(*OutPath));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportDatatableMixedCaseExplicitRowTest,
	"Cortex.Data.Export.Datatable.MixedCaseExplicitRowResolvesActualName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportDatatableMixedCaseExplicitRowTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	UDataTable* RegularTable = Fixture.CreateRegularDataTable();
	TestNotNull(TEXT("regular DataTable fixture is created"), RegularTable);
	if (RegularTable == nullptr)
	{
		return true;
	}

	FCortexCommandRouter Router = CreateDataExportTestRouter();
	const FString OutPath = Fixture.MakeSavedOutputPath(TEXT("mixed-case-explicit-row.json"));

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("table_path"), RegularTable->GetPathName());
	Params->SetStringField(TEXT("out_path"), OutPath);
	AddStringArrayField(Params, TEXT("row_names"), TArray<FString>{ TEXT("ALPHA") });

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_datatable_json"), Params);
	TestTrue(TEXT("mixed-case explicit row_names export succeeds"), Result.bSuccess);

	TSharedPtr<FJsonObject> FileJson;
	FString ParseError;
	TestTrue(TEXT("mixed-case explicit row export file parses"), Fixture.TryReadJsonFile(OutPath, FileJson, ParseError));
	if (!ParseError.IsEmpty())
	{
		AddError(ParseError);
	}

	if (FileJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (FileJson->TryGetArrayField(TEXT("rows"), Rows) && Rows != nullptr)
		{
			TestEqual(TEXT("mixed-case explicit row exports one resolved row"), Rows->Num(), 1);
			TestTrue(TEXT("mixed-case explicit row uses actual stored row name"), GetRowNamesFromExport(*Rows) == TArray<FString>{ TEXT("alpha") });
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportCompositeDatatableTest,
	"Cortex.Data.Export.Datatable.CompositeResolvedRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportCompositeDatatableTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	UCompositeDataTable* CompositeTable = Fixture.CreateCompositeDataTable();
	TestNotNull(TEXT("CompositeDataTable fixture is created"), CompositeTable);
	if (CompositeTable == nullptr)
	{
		return true;
	}

	FCortexCommandRouter Router = CreateDataExportTestRouter();
	const FString OutPath = Fixture.MakeSavedOutputPath(TEXT("composite.json"));

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("table_path"), CompositeTable->GetPathName());
	Params->SetStringField(TEXT("out_path"), OutPath);

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_datatable_json"), Params);
	TestTrue(TEXT("CompositeDataTable export succeeds"), Result.bSuccess);
	if (Result.Data.IsValid())
	{
		TestTrue(TEXT("summary omits raw rows"), !Result.Data->HasField(TEXT("rows")));
		TestTrue(TEXT("summary reports byte count"), Result.Data->GetNumberField(TEXT("bytes_written")) > 0.0);
	}

	TSharedPtr<FJsonObject> FileJson;
	FString ParseError;
	TestTrue(TEXT("CompositeDataTable export file parses"), Fixture.TryReadJsonFile(OutPath, FileJson, ParseError));
	if (!ParseError.IsEmpty())
	{
		AddError(ParseError);
	}

	FString FileContents;
	TestTrue(TEXT("CompositeDataTable export file can be read as text"), FFileHelper::LoadFileToString(FileContents, *OutPath));
	TestFalse(TEXT("file does not include parent A table path"), FileContents.Contains(TEXT("DT_CortexExportParentA")));
	TestFalse(TEXT("file does not include parent B table path"), FileContents.Contains(TEXT("DT_CortexExportParentB")));
	TestFalse(TEXT("file does not include parent-table sections"), FileContents.Contains(TEXT("parent_tables")));
	TestFalse(TEXT("file does not include source-table sections"), FileContents.Contains(TEXT("source_tables")));
	TestFalse(TEXT("overridden parent row payload is not exported"), FileContents.Contains(TEXT("Base Shared")));

	if (FileJson.IsValid())
	{
		TestTrue(TEXT("file contains stable row array"), FileJson->HasTypedField<EJson::Array>(TEXT("rows")));
		TestTrue(TEXT("file contains row_struct"), FileJson->HasTypedField<EJson::String>(TEXT("row_struct")));

		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (FileJson->TryGetArrayField(TEXT("rows"), Rows) && Rows != nullptr)
		{
			TestEqual(TEXT("composite export returns merged resolved rows"), Rows->Num(), 3);
			TestTrue(TEXT("composite rows are sorted by row_name"), GetRowNamesFromExport(*Rows) == TArray<FString>{ TEXT("alpha"), TEXT("beta"), TEXT("shared") });

			const TSharedPtr<FJsonObject>* SharedRowObject = nullptr;
			if ((*Rows)[2].IsValid() && (*Rows)[2]->TryGetObject(SharedRowObject) && SharedRowObject != nullptr && SharedRowObject->IsValid())
			{
				TSharedPtr<FJsonObject> SharedRowData = GetRowDataFromExportRow(*SharedRowObject);
				TestTrue(TEXT("composite row wrapper includes row_data"), SharedRowData.IsValid());
				if (SharedRowData.IsValid())
				{
					TestEqual(TEXT("later parent override wins for duplicate row names"), GetExportedTextValue(SharedRowData, TEXT("Title")), TEXT("Override Shared"));
					TestEqual(TEXT("row_data includes overridden real row_name field"), SharedRowData->GetStringField(TEXT("row_name")), TEXT("Data Override Shared"));
				}
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportDatatableDeterministicBytesTest,
	"Cortex.Data.Export.Datatable.DeterministicBytes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportDatatableDeterministicBytesTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	UDataTable* RegularTable = Fixture.CreateRegularDataTable();
	TestNotNull(TEXT("regular DataTable fixture is created"), RegularTable);
	if (RegularTable == nullptr)
	{
		return true;
	}

	FCortexCommandRouter Router = CreateDataExportTestRouter();
	const FString FirstOutPath = Fixture.MakeSavedOutputPath(TEXT("deterministic-a.json"));
	const FString SecondOutPath = Fixture.MakeSavedOutputPath(TEXT("deterministic-b.json"));

	auto ExecuteDatatableExport = [&Router, RegularTable](const FString& OutPath)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("table_path"), RegularTable->GetPathName());
		Params->SetStringField(TEXT("out_path"), OutPath);
		Params->SetBoolField(TEXT("include_schema"), true);
		return Router.Execute(TEXT("data.export_datatable_json"), Params);
	};

	const FCortexCommandResult FirstResult = ExecuteDatatableExport(FirstOutPath);
	TestTrue(TEXT("first DataTable export succeeds"), FirstResult.bSuccess);
	const FCortexCommandResult SecondResult = ExecuteDatatableExport(SecondOutPath);
	TestTrue(TEXT("second DataTable export succeeds"), SecondResult.bSuccess);

	TArray<uint8> FirstBytes;
	TArray<uint8> SecondBytes;
	FString FirstReadError;
	FString SecondReadError;
	TestTrue(TEXT("first export file reads as bytes"), Fixture.TryReadFileBytes(FirstOutPath, FirstBytes, FirstReadError));
	TestTrue(TEXT("second export file reads as bytes"), Fixture.TryReadFileBytes(SecondOutPath, SecondBytes, SecondReadError));
	if (!FirstReadError.IsEmpty())
	{
		AddError(FirstReadError);
	}
	if (!SecondReadError.IsEmpty())
	{
		AddError(SecondReadError);
	}

	TestTrue(TEXT("first export bytes are non-empty"), FirstBytes.Num() > 0);
	TestTrue(TEXT("second export bytes are non-empty"), SecondBytes.Num() > 0);
	TestTrue(TEXT("DataTable exports are byte-for-byte stable"), FirstBytes == SecondBytes);

	FString FirstContents;
	TestTrue(TEXT("first export file reads as text"), FFileHelper::LoadFileToString(FirstContents, *FirstOutPath));
	TestFalse(TEXT("first export text is non-empty"), FirstContents.IsEmpty());
	const int32 ExportedCountIndex = FirstContents.Find(TEXT("\"exported_count\""));
	const int32 FieldsIndex = FirstContents.Find(TEXT("\"fields\""));
	const int32 RowStructIndex = FirstContents.Find(TEXT("\"row_struct\""));
	const int32 RowsIndex = FirstContents.Find(TEXT("\"rows\""));
	const int32 SchemaIndex = FirstContents.Find(TEXT("\"schema\""));
	const int32 TablePathIndex = FirstContents.Find(TEXT("\"table_path\""));
	const int32 TotalCountIndex = FirstContents.Find(TEXT("\"total_count\""));
	TestTrue(TEXT("canonical writer sorts top-level export keys"),
		ExportedCountIndex != INDEX_NONE
		&& ExportedCountIndex < FieldsIndex
		&& FieldsIndex < RowStructIndex
		&& RowStructIndex < RowsIndex
		&& RowsIndex < SchemaIndex
		&& SchemaIndex < TablePathIndex
		&& TablePathIndex < TotalCountIndex);
	const int32 RowDataIndex = RowsIndex != INDEX_NONE
		? FirstContents.Find(TEXT("\"row_data\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, RowsIndex)
		: INDEX_NONE;
	const int32 NestedStepsIndex = RowDataIndex != INDEX_NONE
		? FirstContents.Find(TEXT("\"Steps\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, RowDataIndex)
		: INDEX_NONE;
	const int32 NestedTitleIndex = NestedStepsIndex != INDEX_NONE
		? FirstContents.Find(TEXT("\"Title\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, NestedStepsIndex)
		: INDEX_NONE;
	const int32 NestedRowNameIndex = NestedTitleIndex != INDEX_NONE
		? FirstContents.Find(TEXT("\"row_name\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, NestedTitleIndex)
		: INDEX_NONE;
	const int32 WrapperRowNameIndex = NestedRowNameIndex != INDEX_NONE
		? FirstContents.Find(TEXT("\"row_name\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, NestedRowNameIndex + 1)
		: INDEX_NONE;
	TestTrue(TEXT("canonical writer sorts nested row wrapper keys"),
		RowDataIndex != INDEX_NONE
		&& NestedStepsIndex != INDEX_NONE
		&& NestedTitleIndex != INDEX_NONE
		&& NestedRowNameIndex != INDEX_NONE
		&& WrapperRowNameIndex != INDEX_NONE
		&& RowDataIndex < NestedStepsIndex
		&& NestedStepsIndex < NestedTitleIndex
		&& NestedTitleIndex < NestedRowNameIndex
		&& NestedRowNameIndex < WrapperRowNameIndex);

	TSharedPtr<FJsonObject> FirstJson;
	FString ParseError;
	TestTrue(TEXT("first deterministic export parses"), Fixture.TryReadJsonFile(FirstOutPath, FirstJson, ParseError));
	if (!ParseError.IsEmpty())
	{
		AddError(ParseError);
	}
	if (FirstJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (FirstJson->TryGetArrayField(TEXT("rows"), Rows) && Rows != nullptr)
		{
			TestEqual(TEXT("deterministic export writes expected row count"), Rows->Num(), 3);
			TestTrue(TEXT("export preserves sorted rows array order"), GetRowNamesFromExport(*Rows) == TArray<FString>{ TEXT("alpha"), TEXT("middle"), TEXT("zeta") });
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportDataAssetsCatalogAndPropertiesTest,
	"Cortex.Data.Export.DataAssets.CatalogAndEditableProperties",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportDataAssetsCatalogAndPropertiesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataExportTestFixture Fixture;
	UCortexTestDataAsset* DataAsset = Fixture.CreateDataAsset();
	TestNotNull(TEXT("DataAsset fixture is created"), DataAsset);
	if (DataAsset == nullptr)
	{
		return true;
	}

	FCortexCommandRouter Router = CreateDataExportTestRouter();
	const FString CatalogOutPath = Fixture.MakeSavedOutputPath(TEXT("data-assets-catalog.json"));

	TSharedRef<FJsonObject> CatalogParams = MakeShared<FJsonObject>();
	CatalogParams->SetStringField(TEXT("out_path"), CatalogOutPath);
	CatalogParams->SetStringField(TEXT("class_name"), TEXT("CortexTestDataAsset"));
	CatalogParams->SetStringField(TEXT("path_filter"), FPaths::GetPath(DataAsset->GetPathName()));
	CatalogParams->SetBoolField(TEXT("include_properties"), false);

	const FCortexCommandResult CatalogResult = Router.Execute(TEXT("data.export_data_assets_json"), CatalogParams);
	TestTrue(TEXT("DataAsset catalog export succeeds"), CatalogResult.bSuccess);
	TestTrue(TEXT("DataAsset catalog export returns data"), CatalogResult.Data.IsValid());
	if (CatalogResult.Data.IsValid())
	{
		TestTrue(TEXT("summary omits data_assets payload"), !CatalogResult.Data->HasField(TEXT("data_assets")));
		TestEqual(TEXT("exported count is one"), static_cast<int32>(CatalogResult.Data->GetNumberField(TEXT("exported_count"))), 1);
	}

	TSharedPtr<FJsonObject> CatalogFileJson;
	FString CatalogParseError;
	TestTrue(TEXT("DataAsset catalog export file parses"), Fixture.TryReadJsonFile(CatalogOutPath, CatalogFileJson, CatalogParseError));
	if (!CatalogParseError.IsEmpty())
	{
		AddError(CatalogParseError);
	}
	if (CatalogFileJson.IsValid())
	{
		TestTrue(TEXT("file contains data_assets array"), CatalogFileJson->HasTypedField<EJson::Array>(TEXT("data_assets")));
		TestEqual(TEXT("file count remains a compatibility alias for exported_count"), static_cast<int32>(CatalogFileJson->GetNumberField(TEXT("count"))), 1);
		TestEqual(TEXT("file exported count is one"), static_cast<int32>(CatalogFileJson->GetNumberField(TEXT("exported_count"))), 1);
		TestEqual(TEXT("file includes class_name provenance"), CatalogFileJson->GetStringField(TEXT("class_name")), TEXT("CortexTestDataAsset"));
		TestEqual(TEXT("file includes path_filter provenance"), CatalogFileJson->GetStringField(TEXT("path_filter")), FPaths::GetPath(DataAsset->GetPathName()));
		TestFalse(TEXT("file records include_properties provenance"), CatalogFileJson->GetBoolField(TEXT("include_properties")));
		TestFalse(TEXT("file records explicit path mode"), CatalogFileJson->GetBoolField(TEXT("explicit_asset_paths")));
		TestTrue(TEXT("file records null asset_paths for registry exports"), CatalogFileJson->HasTypedField<EJson::Null>(TEXT("asset_paths")));

		const TArray<TSharedPtr<FJsonObject>> Entries = GetDataAssetEntries(CatalogFileJson);
		TestEqual(TEXT("catalog export writes one entry"), Entries.Num(), 1);
		if (Entries.Num() == 1)
		{
			TestEqual(TEXT("catalog entry path matches fixture"), Entries[0]->GetStringField(TEXT("path")), DataAsset->GetPathName());
			TestEqual(TEXT("catalog entry name matches fixture"), Entries[0]->GetStringField(TEXT("name")), DataAsset->GetName());
			TestEqual(TEXT("catalog entry class matches fixture"), Entries[0]->GetStringField(TEXT("asset_class")), DataAsset->GetClass()->GetName());
			TestFalse(TEXT("catalog entry omits properties when include_properties is false"), Entries[0]->HasField(TEXT("properties")));
		}
	}

	const FString PropertiesOutPath = Fixture.MakeSavedOutputPath(TEXT("data-assets-properties.json"));
	TSharedRef<FJsonObject> PropertiesParams = MakeShared<FJsonObject>();
	PropertiesParams->SetStringField(TEXT("out_path"), PropertiesOutPath);
	PropertiesParams->SetStringField(TEXT("class_name"), TEXT("CortexTestDataAsset"));
	PropertiesParams->SetStringField(TEXT("path_filter"), FPaths::GetPath(DataAsset->GetPathName()));
	PropertiesParams->SetBoolField(TEXT("include_properties"), true);

	const FCortexCommandResult PropertiesResult = Router.Execute(TEXT("data.export_data_assets_json"), PropertiesParams);
	TestTrue(TEXT("DataAsset properties export succeeds"), PropertiesResult.bSuccess);
	TestTrue(TEXT("DataAsset properties export returns data"), PropertiesResult.Data.IsValid());
	if (PropertiesResult.Data.IsValid())
	{
		TestTrue(TEXT("summary omits data_assets payload"), !PropertiesResult.Data->HasField(TEXT("data_assets")));
		TestEqual(TEXT("exported count is one"), static_cast<int32>(PropertiesResult.Data->GetNumberField(TEXT("exported_count"))), 1);
	}

	TSharedPtr<FJsonObject> PropertiesFileJson;
	FString PropertiesParseError;
	TestTrue(TEXT("DataAsset properties export file parses"), Fixture.TryReadJsonFile(PropertiesOutPath, PropertiesFileJson, PropertiesParseError));
	if (!PropertiesParseError.IsEmpty())
	{
		AddError(PropertiesParseError);
	}
	if (PropertiesFileJson.IsValid())
	{
		TestTrue(TEXT("file contains data_assets array"), PropertiesFileJson->HasTypedField<EJson::Array>(TEXT("data_assets")));
		TestEqual(TEXT("properties file count remains a compatibility alias for exported_count"), static_cast<int32>(PropertiesFileJson->GetNumberField(TEXT("count"))), 1);
		TestEqual(TEXT("file exported count is one"), static_cast<int32>(PropertiesFileJson->GetNumberField(TEXT("exported_count"))), 1);
		TestTrue(TEXT("properties file records include_properties provenance"), PropertiesFileJson->GetBoolField(TEXT("include_properties")));

		const TArray<TSharedPtr<FJsonObject>> Entries = GetDataAssetEntries(PropertiesFileJson);
		TestEqual(TEXT("properties export writes one entry"), Entries.Num(), 1);
		if (Entries.Num() == 1)
		{
			TestTrue(TEXT("asset entry includes partial flag"), Entries[0]->HasTypedField<EJson::Boolean>(TEXT("partial")));
			TestTrue(TEXT("asset entry includes issues array"), Entries[0]->HasTypedField<EJson::Array>(TEXT("issues")));
			const TSharedPtr<FJsonObject>* PropertiesObject = nullptr;
			TestTrue(TEXT("properties entry includes properties object"),
				Entries[0]->TryGetObjectField(TEXT("properties"), PropertiesObject) && PropertiesObject != nullptr && PropertiesObject->IsValid());
			if (PropertiesObject != nullptr && PropertiesObject->IsValid())
			{
				TestEqual(TEXT("editable string property is exported"), (*PropertiesObject)->GetStringField(TEXT("TestProperty")), TEXT("Editable export value"));
				TestEqual(TEXT("editable number property is exported"), static_cast<int32>((*PropertiesObject)->GetNumberField(TEXT("TestNumber"))), 42);
				TestEqual(TEXT("editable allowed property is exported"), (*PropertiesObject)->GetStringField(TEXT("EditableExportAllowed")), TEXT("Editable export allowed value"));
				TestTrue(TEXT("recursive export includes editable nested object"), (*PropertiesObject)->HasTypedField<EJson::Object>(TEXT("Nested")));
				const TSharedPtr<FJsonObject> Nested = (*PropertiesObject)->GetObjectField(TEXT("Nested"));
				TestTrue(TEXT("recursive export includes editable nested field"), Nested->HasTypedField<EJson::String>(TEXT("Visible")));
				TestFalse(TEXT("recursive export excludes non-edit nested field"), Nested->HasField(TEXT("Internal")));
				TestFalse(TEXT("transient editable property is blocked"), (*PropertiesObject)->HasField(TEXT("ExportTransientProperty")));
				TestFalse(TEXT("transient non-editable property is blocked"), (*PropertiesObject)->HasField(TEXT("TransientExportBlocked")));
#if WITH_EDITORONLY_DATA
				TestFalse(TEXT("editor-only property is blocked"), (*PropertiesObject)->HasField(TEXT("ExportEditorOnlyProperty")));
#endif
				TestFalse(TEXT("internal non-editable property is blocked"), (*PropertiesObject)->HasField(TEXT("ExportInternalProperty")));
			}
		}
	}

	const FString TrailingSlashOutPath = Fixture.MakeSavedOutputPath(TEXT("data-assets-trailing-slash-filter.json"));
	TSharedRef<FJsonObject> TrailingSlashParams = MakeShared<FJsonObject>();
	TrailingSlashParams->SetStringField(TEXT("out_path"), TrailingSlashOutPath);
	TrailingSlashParams->SetStringField(TEXT("class_name"), TEXT("CortexTestDataAsset"));
	TrailingSlashParams->SetStringField(TEXT("path_filter"), FPaths::GetPath(DataAsset->GetPathName()) + TEXT("/"));
	TrailingSlashParams->SetBoolField(TEXT("include_properties"), false);

	const FCortexCommandResult TrailingSlashResult = Router.Execute(TEXT("data.export_data_assets_json"), TrailingSlashParams);
	TestTrue(TEXT("DataAsset path_filter accepts trailing slash"), TrailingSlashResult.bSuccess);
	TSharedPtr<FJsonObject> TrailingSlashFileJson;
	FString TrailingSlashParseError;
	TestTrue(TEXT("trailing slash DataAsset export file parses"), Fixture.TryReadJsonFile(TrailingSlashOutPath, TrailingSlashFileJson, TrailingSlashParseError));
	if (!TrailingSlashParseError.IsEmpty())
	{
		AddError(TrailingSlashParseError);
	}
	if (TrailingSlashFileJson.IsValid())
	{
		TestEqual(TEXT("trailing slash path_filter is canonicalized in file"), TrailingSlashFileJson->GetStringField(TEXT("path_filter")), FPaths::GetPath(DataAsset->GetPathName()));
		TestEqual(TEXT("trailing slash path_filter preserves matching assets"), static_cast<int32>(TrailingSlashFileJson->GetNumberField(TEXT("exported_count"))), 1);
	}

	TSharedRef<FJsonObject> InvalidPathFilterParams = MakeShared<FJsonObject>();
	InvalidPathFilterParams->SetStringField(TEXT("out_path"), Fixture.MakeSavedOutputPath(TEXT("data-assets-invalid-filter.json")));
	InvalidPathFilterParams->SetStringField(TEXT("class_name"), TEXT("CortexTestDataAsset"));
	InvalidPathFilterParams->SetStringField(TEXT("path_filter"), TEXT("Not/A/LongPackagePath"));
	const FCortexCommandResult InvalidPathFilterResult = Router.Execute(TEXT("data.export_data_assets_json"), InvalidPathFilterParams);
	TestFalse(TEXT("DataAsset path_filter rejects invalid long package paths"), InvalidPathFilterResult.bSuccess);
	TestEqual(TEXT("invalid DataAsset path_filter uses InvalidField"), InvalidPathFilterResult.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportDataAssetsStrictSerializationIssuesTest,
	"Cortex.Data.Export.DataAssets.StrictSerializationIssues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportDataAssetsStrictSerializationIssuesTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	FCortexCommandRouter Router = CreateDataExportTestRouter();

	UCortexTestDataAsset* DataAsset = Fixture.CreateDataAsset();
	TestNotNull(TEXT("DataAsset fixture is created"), DataAsset);
	if (DataAsset == nullptr)
	{
		return false;
	}
	UCortexTestDataAssetUnsupportedObject* UnsupportedObject = NewObject<UCortexTestDataAssetUnsupportedObject>(DataAsset);
	TScriptInterface<ICortexTestDataAssetUnsupportedInterface> UnsupportedInterface;
	UnsupportedInterface.SetObject(UnsupportedObject);
	UnsupportedInterface.SetInterface(Cast<ICortexTestDataAssetUnsupportedInterface>(UnsupportedObject));
	DataAsset->UnsupportedExportInterface = UnsupportedInterface;

	const FString StrictOutPath = Fixture.MakeSavedOutputPath(TEXT("strict-serialization.json"));
	FFileHelper::SaveStringToFile(TEXT("{\"existing\":true}"), *StrictOutPath);

	TSharedPtr<FJsonObject> StrictParams = MakeShared<FJsonObject>();
	StrictParams->SetStringField(TEXT("out_path"), StrictOutPath);
	StrictParams->SetStringField(TEXT("class_name"), TEXT("CortexTestDataAsset"));
	StrictParams->SetStringField(TEXT("path_filter"), FPaths::GetPath(DataAsset->GetPathName()));
	StrictParams->SetBoolField(TEXT("include_properties"), true);
	StrictParams->SetBoolField(TEXT("allow_partial"), false);

	const FCortexCommandResult StrictResult = Router.Execute(TEXT("data.export_data_assets_json"), StrictParams);
	TestFalse(TEXT("strict export fails on blocking serializer issues"), StrictResult.bSuccess);
	TestEqual(TEXT("strict export failure is a serialization error"), StrictResult.ErrorCode, CortexErrorCodes::SerializationError);

	FString ExistingContents;
	FFileHelper::LoadFileToString(ExistingContents, *StrictOutPath);
	TestEqual(TEXT("strict failed export does not replace existing file"), ExistingContents, TEXT("{\"existing\":true}"));

	const FString PartialOutPath = Fixture.MakeSavedOutputPath(TEXT("partial-serialization.json"));
	TSharedPtr<FJsonObject> PartialParams = MakeShared<FJsonObject>();
	PartialParams->SetStringField(TEXT("out_path"), PartialOutPath);
	PartialParams->SetStringField(TEXT("class_name"), TEXT("CortexTestDataAsset"));
	PartialParams->SetStringField(TEXT("path_filter"), FPaths::GetPath(DataAsset->GetPathName()));
	PartialParams->SetBoolField(TEXT("include_properties"), true);
	PartialParams->SetBoolField(TEXT("allow_partial"), true);

	const FCortexCommandResult PartialResult = Router.Execute(TEXT("data.export_data_assets_json"), PartialParams);
	TestTrue(TEXT("partial export succeeds with allow_partial"), PartialResult.bSuccess);

	TSharedPtr<FJsonObject> PartialFileJson;
	FString ParseError;
	TestTrue(TEXT("partial serializer output parses"), Fixture.TryReadJsonFile(PartialOutPath, PartialFileJson, ParseError));
	TestTrue(TEXT("partial file reports partial"), PartialFileJson->GetBoolField(TEXT("partial")));
	TestTrue(TEXT("partial file includes issue count"), PartialFileJson->GetNumberField(TEXT("issue_count")) > 0.0);
	TestTrue(TEXT("partial file includes omitted_assets array"), PartialFileJson->HasTypedField<EJson::Array>(TEXT("omitted_assets")));
	TestEqual(TEXT("partial file exports no blocking asset entries"), GetDataAssetEntries(PartialFileJson).Num(), 0);
	TestEqual(TEXT("partial file reports one omitted asset"), GetOmittedAssetEntries(PartialFileJson).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportDataAssetsExplicitPathsTest,
	"Cortex.Data.Export.DataAssets.ExplicitPathsPartial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportDataAssetsExplicitPathsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FCortexDataExportTestFixture Fixture;
	UCortexTestDataAsset* DataAsset = Fixture.CreateDataAsset();
	TestNotNull(TEXT("DataAsset fixture is created"), DataAsset);
	UCortexTestDataAsset* EarlierSortDataAsset = Fixture.CreateDataAsset(TEXT("DA_CortexExportAlpha"));
	TestNotNull(TEXT("second DataAsset fixture is created"), EarlierSortDataAsset);
	if (DataAsset == nullptr || EarlierSortDataAsset == nullptr)
	{
		return true;
	}

	FCortexCommandRouter Router = CreateDataExportTestRouter();
	const FString AssetPath = DataAsset->GetPathName();
	const FString EarlierSortAssetPath = EarlierSortDataAsset->GetPathName();
	const FString MissingAssetPath = MakeMissingDataAssetObjectPath(DataAsset);

	TSharedRef<FJsonObject> NonStringPathParams = MakeShared<FJsonObject>();
	NonStringPathParams->SetStringField(TEXT("out_path"), Fixture.MakeSavedOutputPath(TEXT("data-assets-non-string-path.json")));
	NonStringPathParams->SetArrayField(TEXT("asset_paths"), TArray<TSharedPtr<FJsonValue>>{ MakeShared<FJsonValueNumber>(123.0) });
	const FCortexCommandResult NonStringPathResult = Router.Execute(TEXT("data.export_data_assets_json"), NonStringPathParams);
	TestFalse(TEXT("non-string explicit asset path is rejected"), NonStringPathResult.bSuccess);
	TestEqual(TEXT("non-string explicit asset path uses InvalidField"), NonStringPathResult.ErrorCode, CortexErrorCodes::InvalidField);

	TSharedRef<FJsonObject> EmptyArrayParams = MakeShared<FJsonObject>();
	EmptyArrayParams->SetStringField(TEXT("out_path"), Fixture.MakeSavedOutputPath(TEXT("data-assets-empty-array.json")));
	EmptyArrayParams->SetArrayField(TEXT("asset_paths"), TArray<TSharedPtr<FJsonValue>>());
	const FCortexCommandResult EmptyArrayResult = Router.Execute(TEXT("data.export_data_assets_json"), EmptyArrayParams);
	TestFalse(TEXT("empty explicit asset_paths array is rejected"), EmptyArrayResult.bSuccess);
	TestEqual(TEXT("empty explicit asset_paths array uses InvalidField"), EmptyArrayResult.ErrorCode, CortexErrorCodes::InvalidField);

	TSharedRef<FJsonObject> InvalidPathParams = MakeShared<FJsonObject>();
	InvalidPathParams->SetStringField(TEXT("out_path"), Fixture.MakeSavedOutputPath(TEXT("data-assets-invalid-path.json")));
	AddStringArrayField(InvalidPathParams, TEXT("asset_paths"), TArray<FString>{ TEXT("") });
	const FCortexCommandResult InvalidPathResult = Router.Execute(TEXT("data.export_data_assets_json"), InvalidPathParams);
	TestFalse(TEXT("empty explicit asset path is rejected"), InvalidPathResult.bSuccess);
	TestEqual(TEXT("empty explicit asset path uses InvalidField"), InvalidPathResult.ErrorCode, CortexErrorCodes::InvalidField);

	TSharedRef<FJsonObject> ClassMismatchParams = MakeShared<FJsonObject>();
	ClassMismatchParams->SetStringField(TEXT("out_path"), Fixture.MakeSavedOutputPath(TEXT("data-assets-class-mismatch.json")));
	ClassMismatchParams->SetStringField(TEXT("class_name"), TEXT("CortexDerivedTestDataAsset"));
	ClassMismatchParams->SetBoolField(TEXT("allow_partial"), false);
	AddStringArrayField(ClassMismatchParams, TEXT("asset_paths"), TArray<FString>{ AssetPath });
	const FCortexCommandResult ClassMismatchResult = Router.Execute(TEXT("data.export_data_assets_json"), ClassMismatchParams);
	TestFalse(TEXT("explicit DataAsset class mismatch fails"), ClassMismatchResult.bSuccess);
	TestEqual(TEXT("explicit DataAsset class mismatch uses InvalidField"), ClassMismatchResult.ErrorCode, CortexErrorCodes::InvalidField);

	UCortexTestDataAsset* SiblingPrefixDataAsset = Fixture.CreateDataAsset(TEXT("AB_CortexExportSibling"));
	TestNotNull(TEXT("sibling-prefix DataAsset fixture is created"), SiblingPrefixDataAsset);
	if (SiblingPrefixDataAsset == nullptr)
	{
		return true;
	}

	const FString RunPackagePath = FPaths::GetPath(AssetPath);
	const FString SiblingPrefixFilter = FPaths::Combine(RunPackagePath, TEXT("A"));
	TSharedRef<FJsonObject> SiblingPrefixParams = MakeShared<FJsonObject>();
	SiblingPrefixParams->SetStringField(TEXT("out_path"), Fixture.MakeSavedOutputPath(TEXT("data-assets-sibling-prefix.json")));
	SiblingPrefixParams->SetStringField(TEXT("path_filter"), SiblingPrefixFilter);
	SiblingPrefixParams->SetBoolField(TEXT("allow_partial"), false);
	AddStringArrayField(SiblingPrefixParams, TEXT("asset_paths"), TArray<FString>{ SiblingPrefixDataAsset->GetPathName() });
	const FCortexCommandResult SiblingPrefixResult = Router.Execute(TEXT("data.export_data_assets_json"), SiblingPrefixParams);
	TestFalse(TEXT("explicit DataAsset path_filter rejects sibling prefix"), SiblingPrefixResult.bSuccess);
	TestEqual(TEXT("explicit DataAsset path_filter sibling prefix uses InvalidField"), SiblingPrefixResult.ErrorCode, CortexErrorCodes::InvalidField);

	const FString FailOutPath = Fixture.MakeSavedOutputPath(TEXT("data-assets-missing-fails.json"));
	TSharedRef<FJsonObject> FailParams = MakeShared<FJsonObject>();
	FailParams->SetStringField(TEXT("out_path"), FailOutPath);
	FailParams->SetStringField(TEXT("class_filter"), TEXT("CortexTestDataAsset"));
	FailParams->SetStringField(TEXT("path_filter"), FPaths::GetPath(AssetPath));
	FailParams->SetBoolField(TEXT("include_properties"), true);
	FailParams->SetBoolField(TEXT("allow_partial"), false);
	AddStringArrayField(FailParams, TEXT("asset_paths"), TArray<FString>{ AssetPath, AssetPath, MissingAssetPath, EarlierSortAssetPath });

	FCortexExportSkipPackageWarningCapture Capture;
	GLog->AddOutputDevice(&Capture);
	const FCortexCommandResult FailResult = Router.Execute(TEXT("data.export_data_assets_json"), FailParams);
	GLog->RemoveOutputDevice(&Capture);

	TestFalse(TEXT("missing explicit DataAsset fails when allow_partial is false"), FailResult.bSuccess);
	TestEqual(TEXT("missing explicit DataAsset uses AssetNotFound"), FailResult.ErrorCode, CortexErrorCodes::AssetNotFound);
	TestEqual(TEXT("missing explicit DataAsset emits no SkipPackage warnings"), Capture.SkipPackageWarnings, 0);
	TestFalse(TEXT("missing explicit DataAsset does not write partial file"), IFileManager::Get().FileExists(*FailOutPath));

	const FString PartialOutPath = Fixture.MakeSavedOutputPath(TEXT("data-assets-partial.json"));
	TSharedRef<FJsonObject> PartialParams = MakeShared<FJsonObject>();
	PartialParams->SetStringField(TEXT("out_path"), PartialOutPath);
	PartialParams->SetStringField(TEXT("class_name"), TEXT("CortexTestDataAsset"));
	PartialParams->SetStringField(TEXT("path_filter"), FPaths::GetPath(AssetPath));
	PartialParams->SetBoolField(TEXT("include_properties"), true);
	PartialParams->SetBoolField(TEXT("allow_partial"), true);
	AddStringArrayField(PartialParams, TEXT("asset_paths"), TArray<FString>{ MissingAssetPath, AssetPath, AssetPath, EarlierSortAssetPath });

	const FCortexCommandResult PartialResult = Router.Execute(TEXT("data.export_data_assets_json"), PartialParams);
	TestTrue(TEXT("explicit DataAsset export succeeds partially when allow_partial is true"), PartialResult.bSuccess);
	TestTrue(TEXT("partial explicit export returns data"), PartialResult.Data.IsValid());
	if (PartialResult.Data.IsValid())
	{
		TestTrue(TEXT("summary omits data_assets payload"), !PartialResult.Data->HasField(TEXT("data_assets")));
		TestTrue(TEXT("summary marks partial export"), PartialResult.Data->GetBoolField(TEXT("partial")));
		TestEqual(TEXT("exported count excludes duplicate and missing paths"), static_cast<int32>(PartialResult.Data->GetNumberField(TEXT("exported_count"))), 2);

		const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
		TestTrue(TEXT("partial export reports missing asset error"),
			PartialResult.Data->TryGetArrayField(TEXT("errors"), Errors) && Errors != nullptr && Errors->Num() == 1);
	}

	TSharedPtr<FJsonObject> PartialFileJson;
	FString PartialParseError;
	TestTrue(TEXT("partial DataAsset export file parses"), Fixture.TryReadJsonFile(PartialOutPath, PartialFileJson, PartialParseError));
	if (!PartialParseError.IsEmpty())
	{
		AddError(PartialParseError);
	}
	if (PartialFileJson.IsValid())
	{
		TestTrue(TEXT("file contains data_assets array"), PartialFileJson->HasTypedField<EJson::Array>(TEXT("data_assets")));
		TestEqual(TEXT("file exported count excludes duplicate and missing paths"), static_cast<int32>(PartialFileJson->GetNumberField(TEXT("exported_count"))), 2);
		TestTrue(TEXT("partial explicit file records explicit path mode"), PartialFileJson->GetBoolField(TEXT("explicit_asset_paths")));
		TestTrue(TEXT("partial explicit file records asset_paths provenance"), PartialFileJson->HasTypedField<EJson::Array>(TEXT("asset_paths")));

		const TArray<TSharedPtr<FJsonObject>> Entries = GetDataAssetEntries(PartialFileJson);
		TestEqual(TEXT("duplicate explicit DataAsset paths are deduplicated"), Entries.Num(), 2);
		if (Entries.Num() == 2)
		{
			TestEqual(TEXT("explicit DataAsset paths are sorted"), Entries[0]->GetStringField(TEXT("path")), EarlierSortAssetPath);
			TestEqual(TEXT("second sorted explicit DataAsset path is written"), Entries[1]->GetStringField(TEXT("path")), AssetPath);
			TestTrue(TEXT("first valid explicit DataAsset output includes properties"), Entries[0]->HasTypedField<EJson::Object>(TEXT("properties")));
			TestTrue(TEXT("second valid explicit DataAsset output includes properties"), Entries[1]->HasTypedField<EJson::Object>(TEXT("properties")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportStringTableJsonTest,
	"Cortex.Data.Export.StringTable.FilteredEntries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportStringTableJsonTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	UStringTable* StringTable = Fixture.CreateStringTable();
	TestNotNull(TEXT("StringTable fixture is created"), StringTable);
	if (StringTable == nullptr)
	{
		return true;
	}

	FCortexCommandRouter Router = CreateDataExportTestRouter();
	const FString OutPath = Fixture.MakeSavedOutputPath(TEXT("string-table.json"));

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("string_table_path"), StringTable->GetPathName());
	Params->SetStringField(TEXT("out_path"), OutPath);
	Params->SetStringField(TEXT("key_pattern"), TEXT("*.key"));

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_string_table_json"), Params);
	TestTrue(TEXT("StringTable export succeeds"), Result.bSuccess);
	TestTrue(TEXT("StringTable export returns data"), Result.Data.IsValid());
	if (Result.Data.IsValid())
	{
		TestTrue(TEXT("summary omits raw entries"), !Result.Data->HasField(TEXT("entries")));
		TestTrue(TEXT("summary reports byte count"), Result.Data->GetNumberField(TEXT("bytes_written")) > 0.0);
		TestEqual(TEXT("summary reports exported entry count"), static_cast<int32>(Result.Data->GetNumberField(TEXT("exported_count"))), 3);
	}

	TSharedPtr<FJsonObject> FileJson;
	FString ParseError;
	TestTrue(TEXT("StringTable export file parses"), Fixture.TryReadJsonFile(OutPath, FileJson, ParseError));
	if (!ParseError.IsEmpty())
	{
		AddError(ParseError);
	}

	FString FileContents;
	TestTrue(TEXT("StringTable export file can be read as text"), FFileHelper::LoadFileToString(FileContents, *OutPath));
	TestFalse(TEXT("key_pattern excludes non-matching keys"), FileContents.Contains(TEXT("ignored.other")));
	TestFalse(TEXT("key_pattern excludes non-matching source strings"), FileContents.Contains(TEXT("Ignored text")));

	if (FileJson.IsValid())
	{
		TestTrue(TEXT("file contains entries array"), FileJson->HasTypedField<EJson::Array>(TEXT("entries")));
		TestTrue(TEXT("file contains count"), FileJson->HasTypedField<EJson::Number>(TEXT("count")));
		TestEqual(TEXT("file reports filtered entry count"), static_cast<int32>(FileJson->GetNumberField(TEXT("count"))), 3);

		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (FileJson->TryGetArrayField(TEXT("entries"), Entries) && Entries != nullptr)
		{
			TestEqual(TEXT("string entries are sorted by key"), Entries->Num(), 3);
			if (Entries->Num() == 3)
			{
				const TSharedPtr<FJsonObject>* FirstEntry = nullptr;
				const TSharedPtr<FJsonObject>* SecondEntry = nullptr;
				const TSharedPtr<FJsonObject>* ThirdEntry = nullptr;
				if ((*Entries)[0].IsValid() && (*Entries)[0]->TryGetObject(FirstEntry) && FirstEntry != nullptr && FirstEntry->IsValid()
					&& (*Entries)[1].IsValid() && (*Entries)[1]->TryGetObject(SecondEntry) && SecondEntry != nullptr && SecondEntry->IsValid()
					&& (*Entries)[2].IsValid() && (*Entries)[2]->TryGetObject(ThirdEntry) && ThirdEntry != nullptr && ThirdEntry->IsValid())
				{
					TestEqual(TEXT("first key is alpha"), (*FirstEntry)->GetStringField(TEXT("key")), TEXT("alpha.key"));
					TestEqual(TEXT("second key is middle"), (*SecondEntry)->GetStringField(TEXT("key")), TEXT("middle.key"));
					TestEqual(TEXT("third key is zeta"), (*ThirdEntry)->GetStringField(TEXT("key")), TEXT("zeta.key"));
					TestEqual(TEXT("entry contains source string"), (*FirstEntry)->GetStringField(TEXT("source_string")), TEXT("Alpha text"));
				}
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportBulkPathSafetyTest,
	"Cortex.Data.Export.BulkPathSafety",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportBulkPathSafetyTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	FCortexCommandRouter Router = CreateDataExportTestRouter();

	TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("type"), TEXT("datatable"));
	Item->SetStringField(TEXT("table_path"), TEXT("/Game/CortexExportTests/Missing.Missing"));
	Item->SetStringField(TEXT("out_path"), Fixture.MakeSavedOutputPath(TEXT("absolute-item.json")));

	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Add(MakeShared<FJsonValueObject>(Item));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("out_dir"), Fixture.MakeSavedOutputDir(TEXT("BulkOut")));
	Params->SetArrayField(TEXT("items"), Items);

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_bulk_json"), Params);
	TestFalse(TEXT("Bulk item absolute output paths are rejected"), Result.bSuccess);
	TestEqual(TEXT("Bulk item absolute path rejection uses InvalidField"), Result.ErrorCode, CortexErrorCodes::InvalidField);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportBulkRejectsCollidingOutputPathsTest,
	"Cortex.Data.Export.Bulk.RejectsCollidingOutputPaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportBulkRejectsCollidingOutputPathsTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	FCortexCommandRouter Router = CreateDataExportTestRouter();

	UDataTable* DataTable = Fixture.CreateRegularDataTable();
	UStringTable* StringTable = Fixture.CreateStringTable();
	TestNotNull(TEXT("DataTable fixture is created"), DataTable);
	TestNotNull(TEXT("StringTable fixture is created"), StringTable);
	if (DataTable == nullptr || StringTable == nullptr)
	{
		return false;
	}

	const FString CollidingOutPath = Fixture.MakeSavedOutputPath(TEXT("BulkOut/shared.json"));

	TArray<TSharedPtr<FJsonValue>> Items;
	AddBulkItem(Items, MakeBulkDataTableItem(TEXT("table"), DataTable->GetPathName(), TEXT("shared.json")));
	AddBulkItem(Items, MakeBulkStringTableItem(TEXT("string_table"), StringTable->GetPathName(), TEXT("./shared.json")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("out_dir"), Fixture.MakeSavedOutputDir(TEXT("BulkOut")));
	Params->SetBoolField(TEXT("allow_partial"), true);
	Params->SetArrayField(TEXT("items"), Items);

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_bulk_json"), Params);
	TestFalse(TEXT("bulk output path collisions are rejected"), Result.bSuccess);
	TestEqual(TEXT("bulk output path collision uses InvalidField"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	TestFalse(TEXT("colliding bulk preflight writes no file"), IFileManager::Get().FileExists(*CollidingOutPath));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportBulkDefaultOutputNamesTest,
	"Cortex.Data.Export.Bulk.DefaultOutputNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportBulkDefaultOutputNamesTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	FCortexCommandRouter Router = CreateDataExportTestRouter();

	UDataTable* DataTable = Fixture.CreateRegularDataTable();
	UStringTable* StringTable = Fixture.CreateStringTable();
	TestNotNull(TEXT("DataTable fixture is created"), DataTable);
	TestNotNull(TEXT("StringTable fixture is created"), StringTable);
	if (DataTable == nullptr || StringTable == nullptr)
	{
		return false;
	}

	const FString NamedDefaultOutPath = Fixture.MakeSavedOutputPath(TEXT("BulkOut/Named_Table_01.json"));
	const FString IndexDefaultOutPath = Fixture.MakeSavedOutputPath(TEXT("BulkOut/item_1.json"));

	TArray<TSharedPtr<FJsonValue>> Items;
	AddBulkItem(Items, MakeBulkDataTableItem(TEXT("Named Table 01"), DataTable->GetPathName(), TEXT("")));
	AddBulkItem(Items, MakeBulkStringTableItem(TEXT(""), StringTable->GetPathName(), TEXT("")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("out_dir"), Fixture.MakeSavedOutputDir(TEXT("BulkOut")));
	Params->SetBoolField(TEXT("allow_partial"), true);
	Params->SetArrayField(TEXT("items"), Items);

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_bulk_json"), Params);
	TestTrue(TEXT("bulk export with default output names succeeds"), Result.bSuccess);
	TestTrue(TEXT("bulk export returns data"), Result.Data.IsValid());
	if (!Result.Data.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("bulk succeeds both default-named items"), static_cast<int32>(Result.Data->GetNumberField(TEXT("succeeded"))), 2);
	TestEqual(TEXT("bulk fails no default-named items"), static_cast<int32>(Result.Data->GetNumberField(TEXT("failed"))), 0);
	TestEqual(TEXT("bulk skips no default-named items"), static_cast<int32>(Result.Data->GetNumberField(TEXT("skipped"))), 0);
	TestTrue(TEXT("named item uses sanitized default file name"), IFileManager::Get().FileExists(*NamedDefaultOutPath));
	TestTrue(TEXT("unnamed item uses index default file name"), IFileManager::Get().FileExists(*IndexDefaultOutPath));

	const TArray<TSharedPtr<FJsonObject>> ItemSummaries = GetBulkItemSummaries(Result.Data);
	TestEqual(TEXT("bulk returns default name item summaries"), ItemSummaries.Num(), 2);
	if (ItemSummaries.Num() == 2)
	{
		TestEqual(TEXT("named default item is written"), ItemSummaries[0]->GetStringField(TEXT("status")), TEXT("written"));
		TestEqual(TEXT("index default item is written"), ItemSummaries[1]->GetStringField(TEXT("status")), TEXT("written"));
		TestEqual(TEXT("named default summary out_path"), ItemSummaries[0]->GetStringField(TEXT("out_path")), NamedDefaultOutPath);
		TestEqual(TEXT("index default summary out_path"), ItemSummaries[1]->GetStringField(TEXT("out_path")), IndexDefaultOutPath);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportBulkUnsupportedTypeTest,
	"Cortex.Data.Export.Bulk.UnsupportedType",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportBulkUnsupportedTypeTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	FCortexCommandRouter Router = CreateDataExportTestRouter();

	TSharedRef<FJsonObject> UnsupportedItem = MakeShared<FJsonObject>();
	UnsupportedItem->SetStringField(TEXT("type"), TEXT("curve_table"));
	UnsupportedItem->SetStringField(TEXT("name"), TEXT("unsupported"));
	UnsupportedItem->SetStringField(TEXT("out_path"), TEXT("unsupported.json"));

	TArray<TSharedPtr<FJsonValue>> Items;
	AddBulkItem(Items, UnsupportedItem);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("out_dir"), Fixture.MakeSavedOutputDir(TEXT("BulkOut")));
	Params->SetBoolField(TEXT("allow_partial"), true);
	Params->SetArrayField(TEXT("items"), Items);

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_bulk_json"), Params);
	TestTrue(TEXT("unsupported bulk item returns structured summary"), Result.bSuccess);
	TestTrue(TEXT("unsupported bulk item returns data"), Result.Data.IsValid());
	if (!Result.Data.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("unsupported bulk item increments failed count"), static_cast<int32>(Result.Data->GetNumberField(TEXT("failed"))), 1);
	TestEqual(TEXT("unsupported bulk item increments no succeeded count"), static_cast<int32>(Result.Data->GetNumberField(TEXT("succeeded"))), 0);

	const TArray<TSharedPtr<FJsonObject>> ItemSummaries = GetBulkItemSummaries(Result.Data);
	TestEqual(TEXT("unsupported bulk item returns one summary"), ItemSummaries.Num(), 1);
	if (ItemSummaries.Num() == 1)
	{
		TestEqual(TEXT("unsupported item status is failed"), ItemSummaries[0]->GetStringField(TEXT("status")), TEXT("failed"));
		TestEqual(TEXT("unsupported item uses InvalidOperation"), ItemSummaries[0]->GetStringField(TEXT("error_code")), CortexErrorCodes::InvalidOperation);
		TestTrue(TEXT("unsupported item reports error"), ItemSummaries[0]->HasTypedField<EJson::String>(TEXT("error")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportBulkDataAssetsPassthroughTest,
	"Cortex.Data.Export.Bulk.DataAssetsPassthrough",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportBulkDataAssetsPassthroughTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	FCortexCommandRouter Router = CreateDataExportTestRouter();

	UCortexTestDataAsset* DataAsset = Fixture.CreateDataAsset();
	TestNotNull(TEXT("DataAsset fixture is created"), DataAsset);
	if (DataAsset == nullptr)
	{
		return false;
	}

	const FString OutPath = Fixture.MakeSavedOutputPath(TEXT("BulkOut/data-asset.json"));
	TSharedRef<FJsonObject> DataAssetsItem = MakeBulkDataAssetsItem(
		TEXT("data_asset"),
		TEXT("data-asset.json"),
		TArray<FString>{ DataAsset->GetPathName() },
		true,
		false);
	DataAssetsItem->SetStringField(TEXT("type"), TEXT("data_asset"));

	TArray<TSharedPtr<FJsonValue>> Items;
	AddBulkItem(Items, DataAssetsItem);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("out_dir"), Fixture.MakeSavedOutputDir(TEXT("BulkOut")));
	Params->SetBoolField(TEXT("allow_partial"), true);
	Params->SetArrayField(TEXT("items"), Items);

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_bulk_json"), Params);
	TestTrue(TEXT("bulk data_asset item succeeds"), Result.bSuccess);
	TestTrue(TEXT("bulk data_asset item returns data"), Result.Data.IsValid());
	if (!Result.Data.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("bulk data_asset item increments succeeded count"), static_cast<int32>(Result.Data->GetNumberField(TEXT("succeeded"))), 1);
	TestEqual(TEXT("bulk data_asset item increments no failed count"), static_cast<int32>(Result.Data->GetNumberField(TEXT("failed"))), 0);
	TestTrue(TEXT("bulk data_asset item writes file"), IFileManager::Get().FileExists(*OutPath));

	TSharedPtr<FJsonObject> FileJson;
	FString ParseError;
	TestTrue(TEXT("bulk data_asset output parses"), Fixture.TryReadJsonFile(OutPath, FileJson, ParseError));
	if (!ParseError.IsEmpty())
	{
		AddError(ParseError);
	}

	const TArray<TSharedPtr<FJsonObject>> Entries = GetDataAssetEntries(FileJson);
	TestEqual(TEXT("bulk data_asset asset_paths filters to one entry"), Entries.Num(), 1);
	if (Entries.Num() == 1)
	{
		TestEqual(TEXT("bulk data_asset entry path matches fixture"), Entries[0]->GetStringField(TEXT("path")), DataAsset->GetPathName());
		TestTrue(TEXT("bulk data_asset include_properties is passed through"), Entries[0]->HasTypedField<EJson::Object>(TEXT("properties")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportBulkStrictPartialChildSkipsRemainingTest,
	"Cortex.Data.Export.Bulk.StrictPartialChildSkipsRemaining",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportBulkStrictPartialChildSkipsRemainingTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	FCortexCommandRouter Router = CreateDataExportTestRouter();

	UCortexTestDataAsset* DataAsset = Fixture.CreateDataAsset();
	UStringTable* StringTable = Fixture.CreateStringTable();
	TestNotNull(TEXT("DataAsset fixture is created"), DataAsset);
	TestNotNull(TEXT("StringTable fixture is created"), StringTable);
	if (DataAsset == nullptr || StringTable == nullptr)
	{
		return false;
	}

	const FString PartialOutPath = Fixture.MakeSavedOutputPath(TEXT("BulkOut/partial-data-assets.json"));
	const FString SkippedOutPath = Fixture.MakeSavedOutputPath(TEXT("BulkOut/skipped-string-table.json"));
	const FString MissingAssetPath = MakeMissingDataAssetObjectPath(DataAsset);

	TArray<TSharedPtr<FJsonValue>> Items;
	AddBulkItem(Items, MakeBulkDataAssetsItem(
		TEXT("partial_data_assets"),
		TEXT("partial-data-assets.json"),
		TArray<FString>{ DataAsset->GetPathName(), MissingAssetPath },
		true,
		true));
	AddBulkItem(Items, MakeBulkStringTableItem(TEXT("skipped_string_table"), StringTable->GetPathName(), TEXT("skipped-string-table.json")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("out_dir"), Fixture.MakeSavedOutputDir(TEXT("BulkOut")));
	Params->SetBoolField(TEXT("allow_partial"), false);
	Params->SetArrayField(TEXT("items"), Items);

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_bulk_json"), Params);
	TestTrue(TEXT("strict bulk partial child returns structured summary"), Result.bSuccess);
	TestTrue(TEXT("strict bulk partial child returns data"), Result.Data.IsValid());
	if (!Result.Data.IsValid())
	{
		return false;
	}

	TestFalse(TEXT("strict bulk partial child does not complete"), Result.Data->GetBoolField(TEXT("completed")));
	TestTrue(TEXT("strict bulk partial child marks partial"), Result.Data->GetBoolField(TEXT("partial")));
	TestEqual(TEXT("strict bulk partial child counts no succeeded items"), static_cast<int32>(Result.Data->GetNumberField(TEXT("succeeded"))), 0);
	TestEqual(TEXT("strict bulk partial child counts failed item"), static_cast<int32>(Result.Data->GetNumberField(TEXT("failed"))), 1);
	TestEqual(TEXT("strict bulk partial child skips remaining item"), static_cast<int32>(Result.Data->GetNumberField(TEXT("skipped"))), 1);

	const TArray<TSharedPtr<FJsonObject>> ItemSummaries = GetBulkItemSummaries(Result.Data);
	TestEqual(TEXT("strict bulk partial child returns two summaries"), ItemSummaries.Num(), 2);
	if (ItemSummaries.Num() == 2)
	{
		TestEqual(TEXT("partial child item is failed for strict bulk"), ItemSummaries[0]->GetStringField(TEXT("status")), TEXT("failed"));
		TestTrue(TEXT("partial child preserves exported count"), ItemSummaries[0]->GetNumberField(TEXT("exported_count")) > 0.0);
		TestTrue(TEXT("partial child preserves bytes written"), ItemSummaries[0]->GetNumberField(TEXT("bytes_written")) > 0.0);
		TestTrue(TEXT("partial child reports error code"), ItemSummaries[0]->HasTypedField<EJson::String>(TEXT("error_code")));
		TestTrue(TEXT("partial child reports error"), ItemSummaries[0]->HasTypedField<EJson::String>(TEXT("error")));
		TestEqual(TEXT("remaining item is skipped"), ItemSummaries[1]->GetStringField(TEXT("status")), TEXT("skipped"));
		TestTrue(TEXT("skipped item reports error code"), ItemSummaries[1]->HasTypedField<EJson::String>(TEXT("error_code")));
		TestTrue(TEXT("skipped item reports error"), ItemSummaries[1]->HasTypedField<EJson::String>(TEXT("error")));
	}

	TestTrue(TEXT("partial child file remains written"), IFileManager::Get().FileExists(*PartialOutPath));
	TestFalse(TEXT("strict bulk skipped remaining item does not write file"), IFileManager::Get().FileExists(*SkippedOutPath));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportBulkStopsAndSkipsAfterFirstFailureTest,
	"Cortex.Data.Export.Bulk.StopsAndSkipsAfterFirstFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportBulkStopsAndSkipsAfterFirstFailureTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	FCortexCommandRouter Router = CreateDataExportTestRouter();

	UDataTable* DataTable = Fixture.CreateRegularDataTable();
	UStringTable* StringTable = Fixture.CreateStringTable();
	TestNotNull(TEXT("DataTable fixture is created"), DataTable);
	TestNotNull(TEXT("StringTable fixture is created"), StringTable);
	if (DataTable == nullptr || StringTable == nullptr)
	{
		return false;
	}

	const FString FirstOutPath = Fixture.MakeSavedOutputPath(TEXT("BulkOut/first-table.json"));
	const FString SkippedOutPath = Fixture.MakeSavedOutputPath(TEXT("BulkOut/skipped-string-table.json"));

	TArray<TSharedPtr<FJsonValue>> Items;
	AddBulkItem(Items, MakeBulkDataTableItem(TEXT("first_table"), DataTable->GetPathName(), TEXT("first-table.json")));
	AddBulkItem(Items, MakeBulkDataTableItem(TEXT("missing_table"), TEXT("/Game/CortexExportTests/Missing.Missing"), TEXT("missing-table.json")));
	AddBulkItem(Items, MakeBulkStringTableItem(TEXT("skipped_string_table"), StringTable->GetPathName(), TEXT("skipped-string-table.json")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("out_dir"), Fixture.MakeSavedOutputDir(TEXT("BulkOut")));
	Params->SetBoolField(TEXT("allow_partial"), false);
	Params->SetArrayField(TEXT("items"), Items);

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_bulk_json"), Params);
	TestTrue(TEXT("bulk export returns structured summary"), Result.bSuccess);
	TestTrue(TEXT("bulk export returns data"), Result.Data.IsValid());
	if (!Result.Data.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("bulk summary omits raw payloads"), !Result.Data->HasField(TEXT("rows")) && !Result.Data->HasField(TEXT("entries")) && !Result.Data->HasField(TEXT("data_assets")));
	TestTrue(TEXT("bulk has item summaries"), Result.Data->HasTypedField<EJson::Array>(TEXT("items")));
	TestTrue(TEXT("bulk has succeeded count"), Result.Data->HasTypedField<EJson::Number>(TEXT("succeeded")));
	TestTrue(TEXT("bulk has failed count"), Result.Data->HasTypedField<EJson::Number>(TEXT("failed")));
	TestTrue(TEXT("bulk has skipped count"), Result.Data->HasTypedField<EJson::Number>(TEXT("skipped")));
	TestFalse(TEXT("bulk does not complete after failure"), Result.Data->GetBoolField(TEXT("completed")));
	TestTrue(TEXT("bulk marks partial after failure"), Result.Data->GetBoolField(TEXT("partial")));
	TestEqual(TEXT("bulk succeeds one item"), static_cast<int32>(Result.Data->GetNumberField(TEXT("succeeded"))), 1);
	TestEqual(TEXT("bulk fails one item"), static_cast<int32>(Result.Data->GetNumberField(TEXT("failed"))), 1);
	TestEqual(TEXT("bulk skips remaining item"), static_cast<int32>(Result.Data->GetNumberField(TEXT("skipped"))), 1);

	const TArray<TSharedPtr<FJsonObject>> ItemSummaries = GetBulkItemSummaries(Result.Data);
	TestEqual(TEXT("bulk returns three item summaries"), ItemSummaries.Num(), 3);
	if (ItemSummaries.Num() == 3)
	{
		TestEqual(TEXT("first item is written"), ItemSummaries[0]->GetStringField(TEXT("status")), TEXT("written"));
		TestEqual(TEXT("second item fails"), ItemSummaries[1]->GetStringField(TEXT("status")), TEXT("failed"));
		TestEqual(TEXT("third item is skipped"), ItemSummaries[2]->GetStringField(TEXT("status")), TEXT("skipped"));
		TestEqual(TEXT("failed item reports error code"), ItemSummaries[1]->GetStringField(TEXT("error_code")), CortexErrorCodes::TableNotFound);
		TestTrue(TEXT("failed item reports error"), ItemSummaries[1]->HasTypedField<EJson::String>(TEXT("error")));
		TestTrue(TEXT("skipped item reports error"), ItemSummaries[2]->HasTypedField<EJson::String>(TEXT("error")));
	}

	TestTrue(TEXT("first bulk item writes file"), IFileManager::Get().FileExists(*FirstOutPath));
	TestFalse(TEXT("skipped bulk item does not write file"), IFileManager::Get().FileExists(*SkippedOutPath));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexDataExportBulkAllowsPartialFailuresTest,
	"Cortex.Data.Export.Bulk.AllowsPartialFailures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexDataExportBulkAllowsPartialFailuresTest::RunTest(const FString& Parameters)
{
	FCortexDataExportTestFixture Fixture;
	FCortexCommandRouter Router = CreateDataExportTestRouter();

	UDataTable* DataTable = Fixture.CreateRegularDataTable();
	UStringTable* StringTable = Fixture.CreateStringTable();
	TestNotNull(TEXT("DataTable fixture is created"), DataTable);
	TestNotNull(TEXT("StringTable fixture is created"), StringTable);
	if (DataTable == nullptr || StringTable == nullptr)
	{
		return false;
	}

	const FString TableOutPath = Fixture.MakeSavedOutputPath(TEXT("BulkOut/table.json"));
	const FString StringTableOutPath = Fixture.MakeSavedOutputPath(TEXT("BulkOut/string-table.json"));

	TArray<TSharedPtr<FJsonValue>> Items;
	AddBulkItem(Items, MakeBulkDataTableItem(TEXT("table"), DataTable->GetPathName(), TEXT("table.json")));
	AddBulkItem(Items, MakeBulkDataTableItem(TEXT("missing_table"), TEXT("/Game/CortexExportTests/Missing.Missing"), TEXT("missing-table.json")));
	AddBulkItem(Items, MakeBulkStringTableItem(TEXT("string_table"), StringTable->GetPathName(), TEXT("string-table.json")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("out_dir"), Fixture.MakeSavedOutputDir(TEXT("BulkOut")));
	Params->SetBoolField(TEXT("allow_partial"), true);
	Params->SetArrayField(TEXT("items"), Items);

	const FCortexCommandResult Result = Router.Execute(TEXT("data.export_bulk_json"), Params);
	TestTrue(TEXT("bulk export with allow_partial returns success"), Result.bSuccess);
	TestTrue(TEXT("bulk export returns data"), Result.Data.IsValid());
	if (!Result.Data.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("bulk summary omits raw payloads"), !Result.Data->HasField(TEXT("rows")) && !Result.Data->HasField(TEXT("entries")) && !Result.Data->HasField(TEXT("data_assets")));
	TestTrue(TEXT("bulk has item summaries"), Result.Data->HasTypedField<EJson::Array>(TEXT("items")));
	TestTrue(TEXT("bulk has succeeded count"), Result.Data->HasTypedField<EJson::Number>(TEXT("succeeded")));
	TestTrue(TEXT("bulk has failed count"), Result.Data->HasTypedField<EJson::Number>(TEXT("failed")));
	TestTrue(TEXT("bulk has skipped count"), Result.Data->HasTypedField<EJson::Number>(TEXT("skipped")));
	TestFalse(TEXT("bulk is not complete with partial failure"), Result.Data->GetBoolField(TEXT("completed")));
	TestTrue(TEXT("bulk marks partial failure"), Result.Data->GetBoolField(TEXT("partial")));
	TestEqual(TEXT("bulk succeeds valid items"), static_cast<int32>(Result.Data->GetNumberField(TEXT("succeeded"))), 2);
	TestEqual(TEXT("bulk fails invalid item"), static_cast<int32>(Result.Data->GetNumberField(TEXT("failed"))), 1);
	TestEqual(TEXT("bulk skips no items with allow_partial"), static_cast<int32>(Result.Data->GetNumberField(TEXT("skipped"))), 0);

	const TArray<TSharedPtr<FJsonObject>> ItemSummaries = GetBulkItemSummaries(Result.Data);
	TestEqual(TEXT("bulk returns three item summaries"), ItemSummaries.Num(), 3);
	if (ItemSummaries.Num() == 3)
	{
		TestEqual(TEXT("first item is written"), ItemSummaries[0]->GetStringField(TEXT("status")), TEXT("written"));
		TestEqual(TEXT("second item fails"), ItemSummaries[1]->GetStringField(TEXT("status")), TEXT("failed"));
		TestEqual(TEXT("third item is written"), ItemSummaries[2]->GetStringField(TEXT("status")), TEXT("written"));
		TestTrue(TEXT("written item reports bytes"), ItemSummaries[0]->GetNumberField(TEXT("bytes_written")) > 0.0);
		TestTrue(TEXT("written string table reports bytes"), ItemSummaries[2]->GetNumberField(TEXT("bytes_written")) > 0.0);
		TestEqual(TEXT("failed item reports error code"), ItemSummaries[1]->GetStringField(TEXT("error_code")), CortexErrorCodes::TableNotFound);
		TestTrue(TEXT("failed item reports error"), ItemSummaries[1]->HasTypedField<EJson::String>(TEXT("error")));
	}

	TestTrue(TEXT("valid DataTable item writes file"), IFileManager::Get().FileExists(*TableOutPath));
	TestTrue(TEXT("valid StringTable item writes file"), IFileManager::Get().FileExists(*StringTableOutPath));

	TSharedPtr<FJsonObject> TableFileJson;
	FString TableParseError;
	TestTrue(TEXT("bulk DataTable output remains valid JSON"), Fixture.TryReadJsonFile(TableOutPath, TableFileJson, TableParseError));
	if (!TableParseError.IsEmpty())
	{
		AddError(TableParseError);
	}

	TSharedPtr<FJsonObject> StringTableFileJson;
	FString StringTableParseError;
	TestTrue(TEXT("bulk StringTable output remains valid JSON"), Fixture.TryReadJsonFile(StringTableOutPath, StringTableFileJson, StringTableParseError));
	if (!StringTableParseError.IsEmpty())
	{
		AddError(StringTableParseError);
	}

	if (TableFileJson.IsValid())
	{
		TestTrue(TEXT("bulk DataTable output contains rows"), TableFileJson->HasTypedField<EJson::Array>(TEXT("rows")));
	}
	if (StringTableFileJson.IsValid())
	{
		TestTrue(TEXT("bulk StringTable output contains entries"), StringTableFileJson->HasTypedField<EJson::Array>(TEXT("entries")));
	}

	return true;
}
