// blueprint.export_migration_analysis analyses every Blueprint of a folder or an explicit list in one call with the
// builders behind analyze_for_migration and graph_metrics and writes a JSONL file plus an index document. These tests
// use saved assets under /Game/Temp/Cortex120/Export so the Asset Registry enumerates them, and report files under
// Saved/Cortex120.

#include "Misc/AutomationTest.h"
#include "CortexAssetMutationGuard.h"
#include "CortexBPCommandHandler.h"
#include "CortexSafeFileContract.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace CortexBPMigrationExportTestSupport
{
	TArray<TSharedPtr<FJsonValue>> JsonStrings(const TArray<FString>& Strings)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& String : Strings)
		{
			Values.Add(MakeShared<FJsonValueString>(String));
		}
		return Values;
	}

	TSharedPtr<FJsonObject> ParseObject(const FString& Text)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, Root) ? Root : nullptr;
	}

	TSharedPtr<FJsonObject> ReadJsonDocument(const FString& Path)
	{
		FString Text;
		return FFileHelper::LoadFileToString(Text, *Path) ? ParseObject(Text) : nullptr;
	}

	/** Parsed non-empty lines of a JSONL file; empty when the file is missing. */
	TArray<TSharedPtr<FJsonObject>> ReadJsonLines(const FString& Path)
	{
		TArray<TSharedPtr<FJsonObject>> Rows;
		TArray<FString> Lines;
		if (FFileHelper::LoadFileToStringArray(Lines, *Path))
		{
			for (const FString& Line : Lines)
			{
				if (!Line.IsEmpty())
				{
					Rows.Add(ParseObject(Line));
				}
			}
		}
		return Rows;
	}

	FString Canonical(const TSharedPtr<FJsonObject>& Object)
	{
		return Object.IsValid() ? FCortexSafeFileContract::SerializeCanonicalJson(Object.ToSharedRef()) : FString(TEXT("<null>"));
	}

	int32 TotalOf(const TSharedPtr<FJsonObject>& Data, const TCHAR* Name)
	{
		const TSharedPtr<FJsonObject>* Totals = nullptr;
		double Value = -1.0;
		if (Data.IsValid() && Data->TryGetObjectField(TEXT("totals"), Totals))
		{
			(*Totals)->TryGetNumberField(Name, Value);
		}
		return static_cast<int32>(Value);
	}

	TSharedPtr<FJsonObject> FindByAssetPath(const TArray<TSharedPtr<FJsonObject>>& Rows, const FString& AssetPath)
	{
		for (const TSharedPtr<FJsonObject>& Row : Rows)
		{
			if (Row.IsValid() && Row->GetStringField(TEXT("asset_path")) == AssetPath)
			{
				return Row;
			}
		}
		return nullptr;
	}

	TArray<TSharedPtr<FJsonObject>> IndexRows(const TSharedPtr<FJsonObject>& Index)
	{
		TArray<TSharedPtr<FJsonObject>> Rows;
		const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
		if (Index.IsValid() && Index->TryGetArrayField(TEXT("assets"), Assets))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Assets)
			{
				Rows.Add(Value->AsObject());
			}
		}
		return Rows;
	}

	FCortexCommandResult Run(const FString& Command, const TSharedPtr<FJsonObject>& Params)
	{
		FCortexBPCommandHandler Handler;
		return Handler.Execute(Command, Params);
	}

	FString IndexPathOf(const FString& OutPath)
	{
		return FPaths::Combine(FPaths::GetPath(OutPath), FPaths::GetBaseFilename(OutPath)) + TEXT(".index.json");
	}

	/** Saved Blueprints under /Game/Temp/Cortex120/Export/<guid>, plus the report files a test asks for. */
	struct FExportFixture
	{
		FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		FString Dir = FString::Printf(TEXT("/Game/Temp/Cortex120/Export/%s"), *Suffix);
		FString ParentPath;
		FString ChildPath;
		FString WidgetPath;
		TArray<FString> ReportFiles;

		static bool Save(UBlueprint* Blueprint)
		{
			UPackage* Package = Blueprint->GetOutermost();
			FString Filename;
			if (!FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), Filename, FPackageName::GetAssetPackageExtension()))
			{
				return false;
			}
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			return UPackage::SavePackage(Package, Blueprint, *Filename, SaveArgs);
		}

		UBlueprint* MakeSaved(const FString& SubDir, const FString& Name, UClass* ParentClass) const
		{
			UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s%s/%s"), *Dir, *SubDir, *Name));
			UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
				ParentClass, Package, FName(*Name), BPTYPE_Normal, UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass(), NAME_None);
			if (Blueprint)
			{
				FKismetEditorUtilities::CompileBlueprint(Blueprint);
				if (!Save(Blueprint))
				{
					return nullptr;
				}
			}
			return Blueprint;
		}

		/** BP_Parent (Actor), BP_Child (parent BP_Parent) and, with bWithWidget, WBP_Widget. */
		bool Create(FAutomationTestBase& Test, const bool bWithWidget)
		{
			UBlueprint* Parent = MakeSaved(TEXT(""), TEXT("BP_Parent"), AActor::StaticClass());
			if (!Test.TestNotNull(TEXT("parent fixture saved"), Parent) || !Test.TestNotNull(TEXT("parent generated class"), Parent->GeneratedClass.Get()))
			{
				return false;
			}
			UBlueprint* Child = MakeSaved(TEXT(""), TEXT("BP_Child"), Parent->GeneratedClass);
			if (!Test.TestNotNull(TEXT("child fixture saved"), Child))
			{
				return false;
			}
			ParentPath = Parent->GetPathName();
			ChildPath = Child->GetPathName();

			if (bWithWidget)
			{
				TSharedPtr<FJsonObject> CreateWidget = MakeShared<FJsonObject>();
				CreateWidget->SetStringField(TEXT("name"), TEXT("WBP_Widget"));
				CreateWidget->SetStringField(TEXT("path"), Dir);
				CreateWidget->SetStringField(TEXT("type"), TEXT("Widget"));
				const FCortexCommandResult Created = Run(TEXT("create"), CreateWidget);
				WidgetPath = Dir + TEXT("/WBP_Widget.WBP_Widget");
				UBlueprint* Widget = Created.bSuccess ? FindObject<UBlueprint>(nullptr, *WidgetPath) : nullptr;
				if (!Test.TestNotNull(TEXT("widget fixture created"), Widget))
				{
					return false;
				}
				FKismetEditorUtilities::CompileBlueprint(Widget);
				if (!Test.TestTrue(TEXT("widget fixture saved"), Save(Widget)))
				{
					return false;
				}
			}
			return true;
		}

		/** Report file under Saved/Cortex120; its index sibling is removed with it. */
		FString ReportPath(const FString& Label)
		{
			const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectSavedDir(), TEXT("Cortex120"), FString::Printf(TEXT("Export_%s_%s.jsonl"), *Label, *Suffix)));
			ReportFiles.Add(Path);
			ReportFiles.Add(IndexPathOf(Path));
			return Path;
		}

		void Cleanup()
		{
			for (const FString& File : ReportFiles)
			{
				IFileManager::Get().Delete(*File);
			}
			for (TObjectIterator<UObject> It; It; ++It)
			{
				UObject* Asset = *It;
				if (Asset && Asset->IsAsset() && Asset->GetOutermost()->GetName().StartsWith(Dir))
				{
					Asset->GetOutermost()->ClearDirtyFlag();
					Asset->ClearFlags(RF_Public | RF_Standalone);
					Asset->MarkAsGarbage();
				}
			}
			CollectGarbage(RF_NoFlags);
			IFileManager::Get().DeleteDirectory(*FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Temp"), TEXT("Cortex120"), TEXT("Export"), Suffix), false, true);
		}
	};

	TSharedPtr<FJsonObject> ExportParams(const FString& Prefix, const FString& OutPath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path_prefix"), Prefix);
		Params->SetStringField(TEXT("out_path"), OutPath);
		return Params;
	}

	/** The single-call command's data object, or null with a test error. */
	TSharedPtr<FJsonObject> SingleCall(
		FAutomationTestBase& Test, const TCHAR* Command, const FString& AssetPath, const TArray<FString>* Sections = nullptr)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		if (Sections)
		{
			Params->SetArrayField(TEXT("sections"), JsonStrings(*Sections));
		}
		const FCortexCommandResult Result = Run(Command, Params);
		Test.TestTrue(*FString::Printf(TEXT("%s succeeds for %s (%s)"), Command, *AssetPath, *Result.ErrorMessage), Result.bSuccess);
		return Result.Data;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPMigrationExportPrefixTest,
	"Cortex.Blueprint.MigrationExport.Prefix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPMigrationExportPrefixTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexBPMigrationExportTestSupport;
	FExportFixture Fixture;
	if (!Fixture.Create(*this, /*bWithWidget=*/true))
	{
		Fixture.Cleanup();
		return false;
	}

	const FString OutPath = Fixture.ReportPath(TEXT("prefix"));
	const FCortexCommandResult Result = Run(TEXT("export_migration_analysis"), ExportParams(Fixture.Dir, OutPath));
	if (!TestTrue(*FString::Printf(TEXT("export succeeds (%s: %s)"), *Result.ErrorCode, *Result.ErrorMessage), Result.bSuccess && Result.Data.IsValid()))
	{
		Fixture.Cleanup();
		return false;
	}

	TestEqual(TEXT("response out_path"), Result.Data->GetStringField(TEXT("out_path")), OutPath);
	TestEqual(TEXT("response index_path"), Result.Data->GetStringField(TEXT("index_path")), IndexPathOf(OutPath));
	TestFalse(TEXT("response carries no per-asset bodies"), Result.Data->HasField(TEXT("assets")));
	TestEqual(TEXT("total"), TotalOf(Result.Data, TEXT("total")), 3);
	TestEqual(TEXT("exported"), TotalOf(Result.Data, TEXT("exported")), 3);
	TestEqual(TEXT("failed"), TotalOf(Result.Data, TEXT("failed")), 0);
	TestEqual(TEXT("not_found"), TotalOf(Result.Data, TEXT("not_found")), 0);
	TestEqual(TEXT("skipped"), TotalOf(Result.Data, TEXT("skipped")), 0);
	TestEqual(TEXT("excluded_by_type"), TotalOf(Result.Data, TEXT("excluded_by_type")), 0);
	TestEqual(TEXT("matched_assets counts the folder's Blueprints"), TotalOf(Result.Data, TEXT("matched_assets")), 3);
	TestFalse(TEXT("no warning when the folder matched"), Result.Data->HasField(TEXT("warnings")));
	const TSharedPtr<FJsonObject>* ByType = nullptr;
	if (TestTrue(TEXT("by_type present"), Result.Data->TryGetObjectField(TEXT("by_type"), ByType)))
	{
		TestEqual(TEXT("two Actor Blueprints"), static_cast<int32>((*ByType)->GetNumberField(TEXT("Actor"))), 2);
		TestEqual(TEXT("one Widget Blueprint"), static_cast<int32>((*ByType)->GetNumberField(TEXT("Widget"))), 1);
	}

	// Every JSONL row equals what the single-call commands return for the same Blueprint.
	const TArray<TSharedPtr<FJsonObject>> Rows = ReadJsonLines(OutPath);
	if (TestEqual(TEXT("one JSONL row per Blueprint"), Rows.Num(), 3))
	{
		for (int32 Index = 1; Index < Rows.Num(); ++Index)
		{
			TestTrue(TEXT("rows sorted by asset_path"),
				Rows[Index - 1]->GetStringField(TEXT("asset_path")).Compare(Rows[Index]->GetStringField(TEXT("asset_path")), ESearchCase::CaseSensitive) <= 0);
		}
		for (const FString& Path : {Fixture.ParentPath, Fixture.ChildPath, Fixture.WidgetPath})
		{
			const TSharedPtr<FJsonObject> Row = FindByAssetPath(Rows, Path);
			if (!TestTrue(*FString::Printf(TEXT("row for %s"), *Path), Row.IsValid()))
			{
				continue;
			}
			TestEqual(*FString::Printf(TEXT("analysis equals analyze_for_migration for %s"), *Path),
				Canonical(Row->GetObjectField(TEXT("analysis"))), Canonical(SingleCall(*this, TEXT("analyze_for_migration"), Path)));
			TestEqual(*FString::Printf(TEXT("graph_metrics equals graph_metrics for %s"), *Path),
				Canonical(Row->GetObjectField(TEXT("graph_metrics"))), Canonical(SingleCall(*this, TEXT("graph_metrics"), Path)));
		}
	}

	// Index rows.
	const TSharedPtr<FJsonObject> Index = ReadJsonDocument(IndexPathOf(OutPath));
	if (TestTrue(TEXT("index parses"), Index.IsValid()))
	{
		TestEqual(TEXT("index totals match the response"), TotalOf(Index, TEXT("exported")), 3);
		const TArray<TSharedPtr<FJsonObject>> IndexEntries = IndexRows(Index);
		if (TestEqual(TEXT("index has a row per exported Blueprint"), IndexEntries.Num(), 3))
		{
			const TSharedPtr<FJsonObject> Child = FindByAssetPath(IndexEntries, Fixture.ChildPath);
			if (TestTrue(TEXT("child index row"), Child.IsValid()))
			{
				TestEqual(TEXT("child name"), Child->GetStringField(TEXT("name")), FString(TEXT("BP_Child")));
				TestEqual(TEXT("child type"), Child->GetStringField(TEXT("type")), FString(TEXT("Actor")));
				TestEqual(TEXT("child parent_class"), Child->GetStringField(TEXT("parent_class")), FString(TEXT("BP_Parent")));
				TestEqual(TEXT("child native_parent_class"), Child->GetStringField(TEXT("native_parent_class")), FString(TEXT("Actor")));
				const TArray<TSharedPtr<FJsonValue>>& Chain = Child->GetArrayField(TEXT("parent_chain"));
				if (TestEqual(TEXT("child parent_chain length"), Chain.Num(), 1))
				{
					TestEqual(TEXT("child parent_chain names the Blueprint ancestor"), Chain[0]->AsString(), FString(TEXT("BP_Parent")));
				}
				const TSharedPtr<FJsonObject>* Metrics = nullptr;
				if (TestTrue(TEXT("child metrics"), Child->TryGetObjectField(TEXT("metrics"), Metrics)))
				{
					const TSharedPtr<FJsonObject> Single = SingleCall(*this, TEXT("graph_metrics"), Fixture.ChildPath);
					if (Single.IsValid())
					{
						const TSharedPtr<FJsonObject> SingleTotals = Single->GetObjectField(TEXT("totals"));
						int32 LiveConditionals = 0;
						for (const TSharedPtr<FJsonValue>& EntryValue : Single->GetArrayField(TEXT("entries")))
						{
							const TSharedPtr<FJsonObject> Entry = EntryValue->AsObject();
							LiveConditionals += Entry->GetBoolField(TEXT("live")) ? static_cast<int32>(Entry->GetNumberField(TEXT("conditionals"))) : 0;
						}
						TestEqual(TEXT("metrics.functional_nodes equals graph_metrics totals"),
							(*Metrics)->GetNumberField(TEXT("functional_nodes")), SingleTotals->GetNumberField(TEXT("functional_nodes")));
						TestEqual(TEXT("metrics.unreachable_nodes equals graph_metrics totals"),
							(*Metrics)->GetNumberField(TEXT("unreachable_nodes")), SingleTotals->GetNumberField(TEXT("unreachable_nodes")));
						TestEqual(TEXT("metrics.has_tick equals graph_metrics"),
							(*Metrics)->GetBoolField(TEXT("has_tick")), Single->GetBoolField(TEXT("has_tick")));
						TestEqual(TEXT("metrics.conditionals equals live entry sum"),
							static_cast<int32>((*Metrics)->GetNumberField(TEXT("conditionals"))), LiveConditionals);
					}
				}
				TestTrue(TEXT("child violations array"), Child->HasTypedField<EJson::Array>(TEXT("violations")));
				TestTrue(TEXT("child unsupported_node_types array"), Child->HasTypedField<EJson::Array>(TEXT("unsupported_node_types")));
				TestTrue(TEXT("child blockers array"), Child->HasTypedField<EJson::Array>(TEXT("blockers")));
				const FString Confidence = Child->GetStringField(TEXT("migration_confidence"));
				TestTrue(TEXT("child migration_confidence is a known level"),
					Confidence == TEXT("high") || Confidence == TEXT("medium") || Confidence == TEXT("low"));
				TestEqual(TEXT("low confidence is a blocker"), Child->GetArrayField(TEXT("blockers")).ContainsByPredicate(
					[](const TSharedPtr<FJsonValue>& Blocker) { return Blocker->AsString() == TEXT("low_migration_confidence"); }),
					Confidence == TEXT("low"));
			}
			const TSharedPtr<FJsonObject> Parent = FindByAssetPath(IndexEntries, Fixture.ParentPath);
			if (TestTrue(TEXT("parent index row"), Parent.IsValid()))
			{
				TestEqual(TEXT("parent chain is empty below a native parent"), Parent->GetArrayField(TEXT("parent_chain")).Num(), 0);
				TestEqual(TEXT("parent parent_class"), Parent->GetStringField(TEXT("parent_class")), FString(TEXT("Actor")));
			}
			const TSharedPtr<FJsonObject> Widget = FindByAssetPath(IndexEntries, Fixture.WidgetPath);
			if (TestTrue(TEXT("widget index row"), Widget.IsValid()))
			{
				TestEqual(TEXT("widget type"), Widget->GetStringField(TEXT("type")), FString(TEXT("Widget")));
			}
		}
	}

	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPMigrationExportTypesAndSectionsTest,
	"Cortex.Blueprint.MigrationExport.TypesAndSections",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPMigrationExportTypesAndSectionsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexBPMigrationExportTestSupport;
	FExportFixture Fixture;
	if (!Fixture.Create(*this, /*bWithWidget=*/true))
	{
		Fixture.Cleanup();
		return false;
	}

	// types is case-insensitive and drops the widget before anything is loaded.
	{
		const FString OutPath = Fixture.ReportPath(TEXT("typed"));
		TSharedPtr<FJsonObject> Params = ExportParams(Fixture.Dir, OutPath);
		Params->SetArrayField(TEXT("types"), JsonStrings({TEXT("actor")}));
		const FCortexCommandResult Result = Run(TEXT("export_migration_analysis"), Params);
		if (TestTrue(*FString::Printf(TEXT("typed export succeeds (%s)"), *Result.ErrorMessage), Result.bSuccess && Result.Data.IsValid()))
		{
			TestEqual(TEXT("typed total"), TotalOf(Result.Data, TEXT("total")), 2);
			TestEqual(TEXT("widget excluded by type"), TotalOf(Result.Data, TEXT("excluded_by_type")), 1);
		}
		const TArray<TSharedPtr<FJsonObject>> Rows = ReadJsonLines(OutPath);
		TestEqual(TEXT("typed rows"), Rows.Num(), 2);
		TestFalse(TEXT("widget absent from typed JSONL"), FindByAssetPath(Rows, Fixture.WidgetPath).IsValid());
	}

	// sections = [variables, graph_metrics]: identity fields plus variables, and the metrics document.
	{
		const FString OutPath = Fixture.ReportPath(TEXT("sections"));
		TSharedPtr<FJsonObject> Params = ExportParams(Fixture.Dir, OutPath);
		Params->SetArrayField(TEXT("sections"), JsonStrings({TEXT("variables"), TEXT("graph_metrics")}));
		const FCortexCommandResult Result = Run(TEXT("export_migration_analysis"), Params);
		TestTrue(*FString::Printf(TEXT("sections export succeeds (%s)"), *Result.ErrorMessage), Result.bSuccess);
		const TArray<TSharedPtr<FJsonObject>> Rows = ReadJsonLines(OutPath);
		if (TestEqual(TEXT("sections rows"), Rows.Num(), 3))
		{
			const TArray<FString> VariablesOnly = {TEXT("variables")};
			for (const TSharedPtr<FJsonObject>& Row : Rows)
			{
				const FString Path = Row->GetStringField(TEXT("asset_path"));
				const TSharedPtr<FJsonObject> Analysis = Row->GetObjectField(TEXT("analysis"));
				TestTrue(TEXT("variables kept"), Analysis->HasField(TEXT("variables")));
				TestFalse(TEXT("other sections dropped"), Analysis->HasField(TEXT("complexity_metrics")) || Analysis->HasField(TEXT("functions")));
				TestEqual(*FString::Printf(TEXT("trimmed analysis equals analyze_for_migration sections for %s"), *Path),
					Canonical(Analysis), Canonical(SingleCall(*this, TEXT("analyze_for_migration"), Path, &VariablesOnly)));
				TestTrue(TEXT("graph_metrics kept"), Row->HasTypedField<EJson::Object>(TEXT("graph_metrics")));
			}
		}
		const TSharedPtr<FJsonObject> Index = ReadJsonDocument(IndexPathOf(OutPath));
		const TArray<TSharedPtr<FJsonObject>> IndexEntries = IndexRows(Index);
		if (TestEqual(TEXT("sections index rows"), IndexEntries.Num(), 3))
		{
			TestFalse(TEXT("index summary does not depend on the sections"), IndexEntries[0]->GetStringField(TEXT("migration_confidence")).IsEmpty());
		}
	}

	// sections = [graph_metrics] only: identity analysis plus metrics; sections = [variables]: no metrics document.
	{
		const FString OutPath = Fixture.ReportPath(TEXT("metrics_only"));
		TSharedPtr<FJsonObject> Params = ExportParams(Fixture.Dir, OutPath);
		Params->SetArrayField(TEXT("sections"), JsonStrings({TEXT("graph_metrics")}));
		TestTrue(TEXT("metrics-only export succeeds"), Run(TEXT("export_migration_analysis"), Params).bSuccess);
		const TArray<TSharedPtr<FJsonObject>> Rows = ReadJsonLines(OutPath);
		if (TestEqual(TEXT("metrics-only rows"), Rows.Num(), 3))
		{
			const TSharedPtr<FJsonObject> Analysis = Rows[0]->GetObjectField(TEXT("analysis"));
			TestTrue(TEXT("identity fields kept"), Analysis->HasField(TEXT("name")) && Analysis->HasField(TEXT("asset_path")));
			TestFalse(TEXT("no analysis sections"), Analysis->HasField(TEXT("variables")) || Analysis->HasField(TEXT("complexity_metrics")));
			TestTrue(TEXT("metrics document present"), Rows[0]->HasTypedField<EJson::Object>(TEXT("graph_metrics")));
		}
	}
	{
		const FString OutPath = Fixture.ReportPath(TEXT("no_metrics"));
		TSharedPtr<FJsonObject> Params = ExportParams(Fixture.Dir, OutPath);
		Params->SetArrayField(TEXT("sections"), JsonStrings({TEXT("variables")}));
		TestTrue(TEXT("analysis-only export succeeds"), Run(TEXT("export_migration_analysis"), Params).bSuccess);
		const TArray<TSharedPtr<FJsonObject>> Rows = ReadJsonLines(OutPath);
		if (TestEqual(TEXT("analysis-only rows"), Rows.Num(), 3))
		{
			TestFalse(TEXT("no metrics document"), Rows[0]->HasField(TEXT("graph_metrics")));
		}
	}

	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPMigrationExportRefusalsTest,
	"Cortex.Blueprint.MigrationExport.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPMigrationExportRefusalsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexBPMigrationExportTestSupport;
	FExportFixture Fixture;
	const FString OutPath = Fixture.ReportPath(TEXT("refused"));
	const FString Prefix = Fixture.Dir;

	const auto ExpectRefused = [&](const TCHAR* Label, const TSharedPtr<FJsonObject>& Params, const FString& ExpectedCode)
	{
		const FCortexCommandResult Result = Run(TEXT("export_migration_analysis"), Params);
		TestFalse(*FString::Printf(TEXT("%s refused"), Label), Result.bSuccess);
		TestEqual(*FString::Printf(TEXT("%s code"), Label), Result.ErrorCode, ExpectedCode);
		TestFalse(*FString::Printf(TEXT("%s writes no JSONL"), Label), IFileManager::Get().FileExists(*OutPath));
		TestFalse(*FString::Printf(TEXT("%s writes no index"), Label), IFileManager::Get().FileExists(*IndexPathOf(OutPath)));
		return Result;
	};

	TSharedPtr<FJsonObject> Neither = MakeShared<FJsonObject>();
	Neither->SetStringField(TEXT("out_path"), OutPath);
	ExpectRefused(TEXT("neither path_prefix nor paths"), Neither, CortexErrorCodes::InvalidField);

	TSharedPtr<FJsonObject> Both = ExportParams(Prefix, OutPath);
	Both->SetArrayField(TEXT("paths"), JsonStrings({Prefix + TEXT("/BP_Parent.BP_Parent")}));
	ExpectRefused(TEXT("both path_prefix and paths"), Both, CortexErrorCodes::InvalidField);

	TSharedPtr<FJsonObject> NoOut = MakeShared<FJsonObject>();
	NoOut->SetStringField(TEXT("path_prefix"), Prefix);
	ExpectRefused(TEXT("missing out_path"), NoOut, CortexErrorCodes::InvalidField);

	TSharedPtr<FJsonObject> BadType = ExportParams(Prefix, OutPath);
	BadType->SetArrayField(TEXT("types"), JsonStrings({TEXT("Spaceship")}));
	ExpectRefused(TEXT("unknown type"), BadType, CortexErrorCodes::InvalidField);

	TSharedPtr<FJsonObject> BadSection = ExportParams(Prefix, OutPath);
	BadSection->SetArrayField(TEXT("sections"), JsonStrings({TEXT("variables"), TEXT("not_a_section")}));
	const FCortexCommandResult BadSectionResult = ExpectRefused(TEXT("unknown section"), BadSection, CortexErrorCodes::InvalidField);
	TestTrue(TEXT("unknown section is named and graph_metrics listed as valid"),
		BadSectionResult.ErrorMessage.Contains(TEXT("not_a_section")) && BadSectionResult.ErrorMessage.Contains(TEXT("graph_metrics")));

	TSharedPtr<FJsonObject> BadFolder = ExportParams(TEXT("Game/NotMounted.."), OutPath);
	ExpectRefused(TEXT("invalid folder"), BadFolder, CortexErrorCodes::InvalidField);

	const FString Outside = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectDir(), TEXT("Source"), FString::Printf(TEXT("CortexMigrationExportOutside_%s.jsonl"), *Fixture.Suffix)));
	const FCortexCommandResult OutsideResult = Run(TEXT("export_migration_analysis"), ExportParams(Prefix, Outside));
	TestFalse(TEXT("out_path outside allowed roots refused"), OutsideResult.bSuccess);
	TestEqual(TEXT("outside code"), OutsideResult.ErrorCode, CortexErrorCodes::InvalidFilePath);
	TestFalse(TEXT("nothing written outside allowed roots"), IFileManager::Get().FileExists(*Outside));
	TestFalse(TEXT("no index written outside allowed roots"), IFileManager::Get().FileExists(*IndexPathOf(Outside)));

	// The analyze vocabulary itself is unchanged: graph_metrics is an export section only.
	TSharedPtr<FJsonObject> AnalyzeParams = MakeShared<FJsonObject>();
	AnalyzeParams->SetStringField(TEXT("asset_path"), Prefix + TEXT("/BP_Parent.BP_Parent"));
	AnalyzeParams->SetArrayField(TEXT("sections"), JsonStrings({TEXT("graph_metrics")}));
	const FCortexCommandResult AnalyzeResult = Run(TEXT("analyze_for_migration"), AnalyzeParams);
	TestFalse(TEXT("analyze_for_migration still refuses graph_metrics"), AnalyzeResult.bSuccess);
	TestEqual(TEXT("analyze refusal code"), AnalyzeResult.ErrorCode, CortexErrorCodes::InvalidField);

	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPMigrationExportEmptyPrefixTest,
	"Cortex.Blueprint.MigrationExport.EmptyPrefixWarning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPMigrationExportEmptyPrefixTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexBPMigrationExportTestSupport;
	FExportFixture Fixture;
	const FString OutPath = Fixture.ReportPath(TEXT("emptyprefix"));

	// A folder that holds no Blueprint succeeds with zero rows and says so instead of looking like an empty project.
	const FCortexCommandResult Result = Run(TEXT("export_migration_analysis"), ExportParams(Fixture.Dir + TEXT("/NoSuchFolder"), OutPath));
	if (TestTrue(*FString::Printf(TEXT("export succeeds (%s: %s)"), *Result.ErrorCode, *Result.ErrorMessage), Result.bSuccess && Result.Data.IsValid()))
	{
		TestEqual(TEXT("no rows"), TotalOf(Result.Data, TEXT("total")), 0);
		TestEqual(TEXT("matched_assets is zero"), TotalOf(Result.Data, TEXT("matched_assets")), 0);
		const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
		if (TestTrue(TEXT("warnings present"), Result.Data->TryGetArrayField(TEXT("warnings"), Warnings)))
		{
			TestEqual(TEXT("one warning names the folder"), Warnings->Num(), 1);
		}
	}
	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPMigrationExportMissingPathTest,
	"Cortex.Blueprint.MigrationExport.MissingPathRow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPMigrationExportMissingPathTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexBPMigrationExportTestSupport;
	FExportFixture Fixture;
	if (!Fixture.Create(*this, /*bWithWidget=*/false))
	{
		Fixture.Cleanup();
		return false;
	}
	UBlueprint* Blocked = Fixture.MakeSaved(TEXT("/Blocked"), TEXT("BP_Blocked"), AActor::StaticClass());
	if (!TestNotNull(TEXT("blocked fixture saved"), Blocked))
	{
		Fixture.Cleanup();
		return false;
	}
	const FString BlockedPath = Blocked->GetPathName();
	FCortexAssetMutationGuard::Block(Blocked, TEXT("forced recovery verification failure"));

	const FString MissingPath = Fixture.Dir + TEXT("/BP_Nope.BP_Nope");
	const FString OutPath = Fixture.ReportPath(TEXT("paths"));
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("paths"), JsonStrings({Fixture.ChildPath, MissingPath, BlockedPath}));
	Params->SetStringField(TEXT("out_path"), OutPath);
	const FCortexCommandResult Result = Run(TEXT("export_migration_analysis"), Params);
	if (TestTrue(*FString::Printf(TEXT("paths export succeeds (%s: %s)"), *Result.ErrorCode, *Result.ErrorMessage), Result.bSuccess && Result.Data.IsValid()))
	{
		TestEqual(TEXT("total"), TotalOf(Result.Data, TEXT("total")), 3);
		TestEqual(TEXT("exported"), TotalOf(Result.Data, TEXT("exported")), 1);
		TestEqual(TEXT("not_found"), TotalOf(Result.Data, TEXT("not_found")), 1);
		TestEqual(TEXT("skipped"), TotalOf(Result.Data, TEXT("skipped")), 1);
		TestEqual(TEXT("failed"), TotalOf(Result.Data, TEXT("failed")), 0);
	}

	const TArray<TSharedPtr<FJsonObject>> Rows = ReadJsonLines(OutPath);
	if (TestEqual(TEXT("a row per requested path"), Rows.Num(), 3))
	{
		const TSharedPtr<FJsonObject> Missing = FindByAssetPath(Rows, MissingPath);
		if (TestTrue(TEXT("missing row"), Missing.IsValid()))
		{
			TestEqual(TEXT("missing status"), Missing->GetStringField(TEXT("status")), FString(TEXT("not_found")));
			TestEqual(TEXT("missing code"), Missing->GetStringField(TEXT("error_code")), CortexErrorCodes::BlueprintNotFound);
			TestFalse(TEXT("missing message"), Missing->GetStringField(TEXT("error_message")).IsEmpty());
			TestFalse(TEXT("missing row carries no analysis"), Missing->HasField(TEXT("analysis")));
		}
		const TSharedPtr<FJsonObject> Skipped = FindByAssetPath(Rows, BlockedPath);
		if (TestTrue(TEXT("blocked row"), Skipped.IsValid()))
		{
			TestEqual(TEXT("blocked status"), Skipped->GetStringField(TEXT("status")), FString(TEXT("skipped")));
			TestTrue(TEXT("blocked message carries the guard reason"),
				Skipped->GetStringField(TEXT("error_message")).Contains(TEXT("forced recovery verification failure")));
		}
		const TSharedPtr<FJsonObject> Exported = FindByAssetPath(Rows, Fixture.ChildPath);
		if (TestTrue(TEXT("exported row"), Exported.IsValid()))
		{
			TestTrue(TEXT("exported row carries the analysis"), Exported->HasTypedField<EJson::Object>(TEXT("analysis")));
		}
	}
	const TArray<TSharedPtr<FJsonObject>> IndexEntries = IndexRows(ReadJsonDocument(IndexPathOf(OutPath)));
	if (TestEqual(TEXT("index lists only exported Blueprints"), IndexEntries.Num(), 1))
	{
		TestEqual(TEXT("index row is the child"), IndexEntries[0]->GetStringField(TEXT("asset_path")), Fixture.ChildPath);
	}

	Fixture.Cleanup();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPMigrationExportIndexContentTest,
	"Cortex.Blueprint.MigrationExport.IndexContent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexBPMigrationExportIndexContentTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace CortexBPMigrationExportTestSupport;
	FExportFixture Fixture;
	UBlueprint* Logic = Fixture.MakeSaved(TEXT(""), TEXT("BP_Logic"), AActor::StaticClass());
	if (!TestNotNull(TEXT("logic fixture saved"), Logic))
	{
		Fixture.Cleanup();
		return false;
	}
	const FString LogicPath = Logic->GetPathName();

	// BeginPlay -> Branch(then) -> Delay -> PrintString; one PrintString left unconnected; one Timeline template.
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Logic);
	UEdGraphNode* BeginPlay = nullptr;
	if (EventGraph)
	{
		for (UEdGraphNode* Node : EventGraph->Nodes)
		{
			const UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
			if (Event && Event->EventReference.GetMemberName() == TEXT("ReceiveBeginPlay"))
			{
				BeginPlay = Node;
			}
		}
	}
	if (!TestNotNull(TEXT("BeginPlay node"), BeginPlay))
	{
		Fixture.Cleanup();
		return false;
	}
	const auto AddCall = [EventGraph](const TCHAR* Function)
	{
		UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(EventGraph);
		Call->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(Function));
		Call->CreateNewGuid();
		EventGraph->AddNode(Call, false, false);
		Call->AllocateDefaultPins();
		return Call;
	};
	UK2Node_IfThenElse* Branch = NewObject<UK2Node_IfThenElse>(EventGraph);
	Branch->CreateNewGuid();
	EventGraph->AddNode(Branch, false, false);
	Branch->AllocateDefaultPins();
	UK2Node_CallFunction* Delay = AddCall(TEXT("Delay"));
	UK2Node_CallFunction* Print = AddCall(TEXT("PrintString"));
	AddCall(TEXT("PrintString")); // unconnected: unreachable
	const auto Link = [](UEdGraphNode* From, const FName FromPin, UEdGraphNode* To)
	{
		UEdGraphPin* Output = From->FindPin(FromPin, EGPD_Output);
		UEdGraphPin* Input = To->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
		return Output && Input && GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Output, Input);
	};
	const bool bWired = Link(BeginPlay, UEdGraphSchema_K2::PN_Then, Branch)
		&& Link(Branch, UEdGraphSchema_K2::PN_Then, Delay)
		&& Link(Delay, UEdGraphSchema_K2::PN_Then, Print);
	if (!TestTrue(TEXT("logic wired"), bWired) || !TestNotNull(TEXT("timeline added"), FBlueprintEditorUtils::AddNewTimeline(Logic, TEXT("ExportTimeline"))))
	{
		Fixture.Cleanup();
		return false;
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Logic);
	if (!TestTrue(TEXT("logic fixture re-saved"), FExportFixture::Save(Logic)))
	{
		Fixture.Cleanup();
		return false;
	}

	const FString OutPath = Fixture.ReportPath(TEXT("content"));
	const FCortexCommandResult Result = Run(TEXT("export_migration_analysis"), ExportParams(Fixture.Dir, OutPath));
	if (!TestTrue(*FString::Printf(TEXT("export succeeds (%s)"), *Result.ErrorMessage), Result.bSuccess))
	{
		Fixture.Cleanup();
		return false;
	}
	const TArray<TSharedPtr<FJsonObject>> IndexEntries = IndexRows(ReadJsonDocument(IndexPathOf(OutPath)));
	if (TestEqual(TEXT("one index row"), IndexEntries.Num(), 1))
	{
		const TSharedPtr<FJsonObject> Row = IndexEntries[0];
		const TSharedPtr<FJsonObject>* Metrics = nullptr;
		if (TestTrue(TEXT("metrics present"), Row->TryGetObjectField(TEXT("metrics"), Metrics)))
		{
			TestEqual(TEXT("functional_nodes: Branch, Delay, PrintString"), static_cast<int32>((*Metrics)->GetNumberField(TEXT("functional_nodes"))), 3);
			TestEqual(TEXT("conditionals: the Branch"), static_cast<int32>((*Metrics)->GetNumberField(TEXT("conditionals"))), 1);
			TestEqual(TEXT("unreachable_nodes: the unconnected PrintString"), static_cast<int32>((*Metrics)->GetNumberField(TEXT("unreachable_nodes"))), 1);
			TestFalse(TEXT("has_tick"), (*Metrics)->GetBoolField(TEXT("has_tick")));
		}
		TArray<FString> Blockers;
		for (const TSharedPtr<FJsonValue>& Blocker : Row->GetArrayField(TEXT("blockers")))
		{
			Blockers.Add(Blocker->AsString());
		}
		TestTrue(TEXT("Delay is a latent_nodes blocker"), Blockers.Contains(TEXT("latent_nodes")));
		TestTrue(TEXT("Timeline is a timelines blocker"), Blockers.Contains(TEXT("timelines")));
		TestFalse(TEXT("no unsupported nodes"), Blockers.Contains(TEXT("unsupported_nodes")));
		TestEqual(TEXT("unsupported_node_types empty"), Row->GetArrayField(TEXT("unsupported_node_types")).Num(), 0);
		TestEqual(TEXT("low confidence blocker follows migration_confidence"), Blockers.Contains(TEXT("low_migration_confidence")),
			Row->GetStringField(TEXT("migration_confidence")) == TEXT("low"));

		// The index agrees with the analysis document of the same run.
		const TSharedPtr<FJsonObject> Analysis = ReadJsonLines(OutPath)[0]->GetObjectField(TEXT("analysis"));
		const TSharedPtr<FJsonObject> Complexity = Analysis->GetObjectField(TEXT("complexity_metrics"));
		TestTrue(TEXT("analysis reports latent nodes"), Complexity->GetBoolField(TEXT("has_latent_nodes")));
		TestTrue(TEXT("analysis reports timelines"), Complexity->GetBoolField(TEXT("has_timelines")));
		TestEqual(TEXT("index confidence is the analysis confidence"),
			Row->GetStringField(TEXT("migration_confidence")), Complexity->GetStringField(TEXT("migration_confidence")));
	}

	Fixture.Cleanup();
	return true;
}
