#include "Operations/CortexBPMigrationExportOps.h"

#include "Algo/Sort.h"
#include "CortexAssetMutationGuard.h"
#include "CortexCommandRouter.h"
#include "CortexSafeFileContract.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Misc/Paths.h"
#include "Operations/CortexBPAnalysisOps.h"
#include "Operations/CortexBPAssetEnumeration.h"
#include "Operations/CortexBPAssetOps.h"
#include "Operations/CortexBPGraphMetricsOps.h"
#include "UObject/GarbageCollection.h"

namespace CortexBPMigrationExportPrivate
{
	/** Failing assets echoed in the response; the full list is in the JSONL file. */
	constexpr int32 MaxFirstFailures = 10;

	enum class EStatus : uint8 { Exported, Failed, Skipped, NotFound };

	struct FRow
	{
		FString AssetPath;
		FString Type;
		EStatus Status = EStatus::Failed;
		FString ErrorCode;
		FString ErrorMessage;
		/** Serialized JSONL line without the line break. */
		FString Line;
		/** Index entry, set for exported Blueprints only. */
		TSharedPtr<FJsonObject> Index;
	};

	const TCHAR* StatusName(const EStatus Status)
	{
		switch (Status)
		{
		case EStatus::Exported: return TEXT("exported");
		case EStatus::Failed: return TEXT("failed");
		case EStatus::Skipped: return TEXT("skipped");
		default: return TEXT("not_found");
		}
	}

	TSharedPtr<FJsonObject> ErrorObject(const FRow& Row)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("asset_path"), Row.AssetPath);
		Json->SetStringField(TEXT("status"), StatusName(Row.Status));
		Json->SetStringField(TEXT("error_code"), Row.ErrorCode);
		Json->SetStringField(TEXT("error_message"), Row.ErrorMessage);
		return Json;
	}

	FRow MakeErrorRow(const FString& AssetPath, const FString& Type, const EStatus Status, const FString& Code, const FString& Message)
	{
		FRow Row;
		Row.AssetPath = AssetPath;
		Row.Type = Type;
		Row.Status = Status;
		Row.ErrorCode = Code;
		Row.ErrorMessage = Message;
		Row.Line = FCortexSafeFileContract::SerializeCanonicalJson(ErrorObject(Row).ToSharedRef());
		return Row;
	}

	/** Blueprint ancestors of Blueprint, immediate parent first, ending before the first native class. */
	TArray<TSharedPtr<FJsonValue>> BuildParentChain(const UBlueprint* Blueprint)
	{
		TArray<TSharedPtr<FJsonValue>> Chain;
		for (const UClass* Ancestor = Blueprint->ParentClass; Ancestor && Cast<UBlueprint>(Ancestor->ClassGeneratedBy); Ancestor = Ancestor->GetSuperClass())
		{
			Chain.Add(MakeShared<FJsonValueString>(FCortexBPAssetOps::ParentClassDisplayName(Ancestor)));
		}
		return Chain;
	}

	TSharedPtr<FJsonObject> BuildIndexRow(
		const UBlueprint* Blueprint, const FString& AssetPath, const TSharedPtr<FJsonObject>& Analysis, const TSharedPtr<FJsonObject>& Metrics)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), Analysis->GetStringField(TEXT("name")));
		Row->SetStringField(TEXT("asset_path"), AssetPath);
		Row->SetStringField(TEXT("type"), Analysis->GetStringField(TEXT("type")));
		Row->SetStringField(TEXT("parent_class"), Analysis->GetStringField(TEXT("parent_class")));
		Row->SetStringField(TEXT("native_parent_class"), Analysis->GetStringField(TEXT("native_parent_class")));
		Row->SetArrayField(TEXT("parent_chain"), BuildParentChain(Blueprint));

		// functional_nodes and unreachable_nodes are the Blueprint totals; conditionals sums the live entries.
		const TSharedPtr<FJsonObject>& MetricTotals = Metrics->GetObjectField(TEXT("totals"));
		int32 Conditionals = 0;
		for (const TSharedPtr<FJsonValue>& EntryValue : Metrics->GetArrayField(TEXT("entries")))
		{
			const TSharedPtr<FJsonObject> Entry = EntryValue->AsObject();
			if (Entry.IsValid() && Entry->GetBoolField(TEXT("live")))
			{
				Conditionals += static_cast<int32>(Entry->GetNumberField(TEXT("conditionals")));
			}
		}
		TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetNumberField(TEXT("functional_nodes"), MetricTotals->GetNumberField(TEXT("functional_nodes")));
		Summary->SetBoolField(TEXT("has_tick"), Metrics->GetBoolField(TEXT("has_tick")));
		Summary->SetNumberField(TEXT("unreachable_nodes"), MetricTotals->GetNumberField(TEXT("unreachable_nodes")));
		Summary->SetNumberField(TEXT("conditionals"), Conditionals);
		Row->SetObjectField(TEXT("metrics"), Summary);
		Row->SetArrayField(TEXT("violations"), Metrics->GetArrayField(TEXT("violations")));

		const TSharedPtr<FJsonObject> Complexity = Analysis->GetObjectField(TEXT("complexity_metrics"));
		const FString Confidence = Complexity->GetStringField(TEXT("migration_confidence"));
		const TArray<TSharedPtr<FJsonValue>>& Unsupported = Complexity->GetArrayField(TEXT("unsupported_node_types"));
		Row->SetStringField(TEXT("migration_confidence"), Confidence);
		Row->SetArrayField(TEXT("unsupported_node_types"), Unsupported);

		TArray<TSharedPtr<FJsonValue>> Blockers;
		const auto AddBlocker = [&Blockers](const TCHAR* Name) { Blockers.Add(MakeShared<FJsonValueString>(Name)); };
		if (Unsupported.Num() > 0)
		{
			AddBlocker(TEXT("unsupported_nodes"));
		}
		if (Complexity->GetBoolField(TEXT("has_latent_nodes")))
		{
			AddBlocker(TEXT("latent_nodes"));
		}
		if (Complexity->GetBoolField(TEXT("has_timelines")))
		{
			AddBlocker(TEXT("timelines"));
		}
		if (Confidence == TEXT("low"))
		{
			AddBlocker(TEXT("low_migration_confidence"));
		}
		Row->SetArrayField(TEXT("blockers"), Blockers);
		return Row;
	}

	/** Which documents each JSONL row carries. */
	struct FSelection
	{
		bool bAnalysisTrimmed = false;
		TSet<FString> AnalysisSections;
		bool bGraphMetrics = true;
	};

	FRow ExportOne(const FCortexBPEnumeratedAsset& Found, const FSelection& Selection, bool& bLoaded)
	{
		const FString AssetPath = Found.Asset.GetObjectPathString();

		// Same fail-closed guard the compile paths enforce: a blocked asset is not touched.
		FString BlockReason;
		if (FCortexAssetMutationGuard::IsPathBlocked(AssetPath, BlockReason))
		{
			return MakeErrorRow(AssetPath, Found.Type, EStatus::Skipped, CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("Asset is blocked after failed recovery: %s"), *BlockReason));
		}

		FString LoadError;
		UBlueprint* Blueprint = FCortexBPAssetOps::LoadBlueprint(AssetPath, LoadError);
		if (!Blueprint)
		{
			return MakeErrorRow(AssetPath, Found.Type, EStatus::Failed, CortexErrorCodes::BlueprintNotFound, LoadError);
		}
		bLoaded = true;

		// The index needs the full analysis and the metrics whatever the row carries.
		const TSharedPtr<FJsonObject> Analysis = FCortexBPAnalysisOps::BuildFullMigrationAnalysis(Blueprint, AssetPath);
		const TSharedPtr<FJsonObject> Metrics = FCortexBPGraphMetricsOps::BuildGraphMetrics(Blueprint, AssetPath, FCortexBPGraphMetricsOps::FThresholds());

		FRow Row;
		Row.AssetPath = AssetPath;
		Row.Type = Found.Type;
		Row.Status = EStatus::Exported;
		Row.Index = BuildIndexRow(Blueprint, AssetPath, Analysis, Metrics);

		if (Selection.bAnalysisTrimmed)
		{
			FCortexBPAnalysisOps::TrimMigrationAnalysis(Analysis, Selection.AnalysisSections);
		}
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("asset_path"), AssetPath);
		Json->SetObjectField(TEXT("analysis"), Analysis);
		if (Selection.bGraphMetrics)
		{
			Json->SetObjectField(TEXT("graph_metrics"), Metrics);
		}
		Row.Line = FCortexSafeFileContract::SerializeCanonicalJson(Json);
		return Row;
	}
}

FCortexCommandResult FCortexBPMigrationExportOps::ExportMigrationAnalysis(const TSharedPtr<FJsonObject>& Params)
{
	using namespace CortexBPMigrationExportPrivate;

	FCortexBPEnumerationRequest Request;
	FCortexCommandResult RequestError;
	if (!FCortexBPAssetEnumeration::ParseRequest(Params, Request, RequestError))
	{
		return RequestError;
	}

	TSet<FString> Sections;
	if (!FCortexBPAnalysisOps::ParseMigrationSections(Params, Sections, RequestError, /*bAcceptGraphMetrics=*/true))
	{
		return RequestError;
	}
	FSelection Selection;
	if (Sections.Num() > 0)
	{
		Selection.bAnalysisTrimmed = true;
		Selection.AnalysisSections = Sections;
		Selection.AnalysisSections.Remove(FCortexBPAnalysisOps::GraphMetricsSectionName);
		Selection.bGraphMetrics = Sections.Contains(FCortexBPAnalysisOps::GraphMetricsSectionName);
	}

	// The index sits next to the JSONL and obeys the same path policy; refused before any Blueprint is touched.
	FCortexResolvedFilePath IndexPath;
	{
		const FString& OutAbsolute = Request.OutPath.AbsolutePath;
		const FString Requested = FPaths::Combine(FPaths::GetPath(OutAbsolute), FPaths::GetBaseFilename(OutAbsolute)) + TEXT(".index.json");
		FString PathErrorCode;
		FString PathErrorMessage;
		if (!FCortexSafeFileContract::ResolveWritePath(Requested, IndexPath, PathErrorCode, PathErrorMessage))
		{
			return FCortexCommandRouter::Error(PathErrorCode, PathErrorMessage);
		}
	}

	const FCortexBPEnumeration Enumeration = FCortexBPAssetEnumeration::Enumerate(Request);

	TArray<FRow> Rows;
	for (const FString& Missing : Enumeration.NotFound)
	{
		Rows.Add(MakeErrorRow(Missing, TEXT("Unknown"), EStatus::NotFound, CortexErrorCodes::BlueprintNotFound,
			TEXT("No Blueprint asset is registered at this path")));
	}
	bool bLoadedAny = false;
	for (const FCortexBPEnumeratedAsset& Found : Enumeration.Assets)
	{
		Rows.Add(ExportOne(Found, Selection, bLoadedAny));
	}
	if (bLoadedAny)
	{
		// Loaded Blueprints are not kept; one collection for the whole run.
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}
	Algo::SortBy(Rows, &FRow::AssetPath, [](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });

	int32 Counts[4] = {0, 0, 0, 0}; // exported, failed, skipped, not_found
	TSharedPtr<FJsonObject> ByType = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> IndexRows;
	TArray<TSharedPtr<FJsonValue>> FirstFailures;
	int32 JsonlChars = 0;
	for (const FRow& Row : Rows)
	{
		JsonlChars += Row.Line.Len() + 1;
	}
	FString Jsonl;
	Jsonl.Reserve(JsonlChars);
	for (FRow& Row : Rows)
	{
		++Counts[static_cast<int32>(Row.Status)];
		Jsonl.Append(Row.Line);
		Jsonl.AppendChar(TEXT('\n'));
		Row.Line.Empty();
		if (Row.Status != EStatus::NotFound)
		{
			ByType->SetNumberField(Row.Type, ByType->HasField(Row.Type) ? ByType->GetNumberField(Row.Type) + 1 : 1);
		}
		if (Row.Index.IsValid())
		{
			IndexRows.Add(MakeShared<FJsonValueObject>(Row.Index));
		}
		else if (Row.Status == EStatus::Failed && FirstFailures.Num() < MaxFirstFailures)
		{
			FirstFailures.Add(MakeShared<FJsonValueObject>(ErrorObject(Row)));
		}
	}

	TSharedPtr<FJsonObject> Totals = MakeShared<FJsonObject>();
	Totals->SetNumberField(TEXT("total"), Rows.Num());
	Totals->SetNumberField(TEXT("exported"), Counts[0]);
	Totals->SetNumberField(TEXT("failed"), Counts[1]);
	Totals->SetNumberField(TEXT("not_found"), Counts[3]);
	Totals->SetNumberField(TEXT("skipped"), Counts[2]);
	Totals->SetNumberField(TEXT("excluded_by_type"), Enumeration.ExcludedByType);
	if (Request.bHasPrefix)
	{
		Totals->SetNumberField(TEXT("matched_assets"), Enumeration.MatchedAssets);
	}

	TSharedRef<FJsonObject> Index = MakeShared<FJsonObject>();
	FCortexBPAssetEnumeration::WriteRequestFields(Request, *Index);
	Index->SetObjectField(TEXT("totals"), Totals);
	Index->SetObjectField(TEXT("by_type"), ByType);
	Index->SetArrayField(TEXT("assets"), IndexRows);

	const FCortexJsonFileWriteResult JsonlResult = FCortexSafeFileContract::WriteTextReportAtomic(Request.OutPath, Jsonl);
	if (!JsonlResult.bWritten)
	{
		return FCortexCommandRouter::Error(
			JsonlResult.ErrorCode.IsEmpty() ? CortexErrorCodes::SaveFailed : JsonlResult.ErrorCode, JsonlResult.ErrorMessage);
	}
	const FCortexJsonFileWriteResult IndexResult = FCortexSafeFileContract::WriteJsonReportAtomic(IndexPath, Index);
	if (!IndexResult.bWritten)
	{
		return FCortexCommandRouter::Error(
			IndexResult.ErrorCode.IsEmpty() ? CortexErrorCodes::SaveFailed : IndexResult.ErrorCode, IndexResult.ErrorMessage);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("out_path"), Request.OutPath.AbsolutePath);
	Data->SetStringField(TEXT("index_path"), IndexPath.AbsolutePath);
	Data->SetObjectField(TEXT("totals"), Totals);
	Data->SetObjectField(TEXT("by_type"), ByType);
	Data->SetArrayField(TEXT("first_failures"), FirstFailures);
	if (Request.bHasPrefix && Enumeration.MatchedAssets == 0)
	{
		TArray<TSharedPtr<FJsonValue>> Warnings;
		Warnings.Add(MakeShared<FJsonValueString>(
			FString::Printf(TEXT("path_prefix '%s' matched no Blueprints"), *Request.Prefix)));
		Data->SetArrayField(TEXT("warnings"), Warnings);
	}
	return FCortexCommandRouter::Success(Data);
}
