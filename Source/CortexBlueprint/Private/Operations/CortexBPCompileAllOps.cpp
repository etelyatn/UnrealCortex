#include "Operations/CortexBPCompileAllOps.h"

#include "Algo/Sort.h"
#include "AssetRegistry/AssetData.h"
#include "CortexAssetMutationGuard.h"
#include "CortexCommandRouter.h"
#include "CortexSafeFileContract.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Operations/CortexBPAssetEnumeration.h"
#include "Operations/CortexBPAssetOps.h"
#include "UObject/GarbageCollection.h"

namespace CortexBPCompileAllPrivate
{
	/** Failing assets echoed in the response; the full list is in the report file. */
	constexpr int32 MaxFirstFailures = 10;

	/** Order matches the totals counters. */
	enum class EStatus : uint8 { Success, Warning, Error, Skipped, NotFound };

	const TCHAR* StatusName(const EStatus Status)
	{
		switch (Status)
		{
		case EStatus::Success: return TEXT("success");
		case EStatus::Warning: return TEXT("warning");
		case EStatus::Error: return TEXT("error");
		case EStatus::Skipped: return TEXT("skipped");
		default: return TEXT("not_found");
		}
	}

	struct FEntry
	{
		FString AssetPath;
		FString Type;
		EStatus Status = EStatus::Error;
		int32 ErrorCount = 0;
		int32 WarningCount = 0;
		TArray<TSharedPtr<FJsonValue>> Diagnostics;
		FString Reason;
	};

	TSharedPtr<FJsonObject> ToJson(const FEntry& Entry, const bool bWithDiagnostics)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("asset_path"), Entry.AssetPath);
		Json->SetStringField(TEXT("type"), Entry.Type);
		Json->SetStringField(TEXT("compile_status"), StatusName(Entry.Status));
		Json->SetNumberField(TEXT("error_count"), Entry.ErrorCount);
		Json->SetNumberField(TEXT("warning_count"), Entry.WarningCount);
		if (bWithDiagnostics)
		{
			Json->SetArrayField(TEXT("diagnostics"), Entry.Diagnostics);
		}
		if (!Entry.Reason.IsEmpty())
		{
			Json->SetStringField(TEXT("reason"), Entry.Reason);
		}
		return Json;
	}

	FEntry CompileOne(const FAssetData& Asset, const FString& Type)
	{
		FEntry Entry;
		Entry.AssetPath = Asset.GetObjectPathString();
		Entry.Type = Type;

		FString WritableError;
		if (!FCortexBPAssetOps::ValidateWritableBlueprintAssetPath(Entry.AssetPath, WritableError))
		{
			Entry.Status = EStatus::Skipped;
			Entry.Reason = WritableError;
			return Entry;
		}

		// Same fail-closed guard the single and batch compile paths enforce.
		FString BlockReason;
		if (FCortexAssetMutationGuard::IsPathBlocked(Entry.AssetPath, BlockReason))
		{
			Entry.Status = EStatus::Skipped;
			Entry.Reason = FString::Printf(TEXT("Asset is blocked after failed recovery: %s"), *BlockReason);
			return Entry;
		}

		TSharedPtr<FJsonObject> CompileParams = MakeShared<FJsonObject>();
		CompileParams->SetStringField(TEXT("asset_path"), Entry.AssetPath);
		const FCortexCommandResult Result = FCortexBPAssetOps::Compile(CompileParams, /*bSkipGarbageCollection=*/true);

		// Compile answers a failed build with COMPILE_FAILED and the payload in the error details.
		const TSharedPtr<FJsonObject> Payload = Result.bSuccess ? Result.Data : Result.ErrorDetails;
		FString CompileStatus;
		if (!Payload.IsValid() || !Payload->TryGetStringField(TEXT("compile_status"), CompileStatus))
		{
			Entry.Status = EStatus::Error;
			Entry.Reason = Result.ErrorMessage;
			return Entry;
		}
		Entry.Status = CompileStatus == TEXT("error") ? EStatus::Error
			: CompileStatus == TEXT("warning") ? EStatus::Warning : EStatus::Success;
		Entry.ErrorCount = static_cast<int32>(Payload->GetNumberField(TEXT("error_count")));
		Entry.WarningCount = static_cast<int32>(Payload->GetNumberField(TEXT("warning_count")));
		const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
		if (Payload->TryGetArrayField(TEXT("diagnostics"), Diagnostics))
		{
			Entry.Diagnostics = *Diagnostics;
		}
		return Entry;
	}
}

FCortexCommandResult FCortexBPCompileAllOps::CompileAll(const TSharedPtr<FJsonObject>& Params)
{
	using namespace CortexBPCompileAllPrivate;

	FCortexBPEnumerationRequest Request;
	FCortexCommandResult RequestError;
	if (!FCortexBPAssetEnumeration::ParseRequest(Params, Request, RequestError))
	{
		return RequestError;
	}

	const FCortexBPEnumeration Enumeration = FCortexBPAssetEnumeration::Enumerate(Request);

	TArray<FEntry> Entries;
	for (const FString& Missing : Enumeration.NotFound)
	{
		FEntry Entry;
		Entry.AssetPath = Missing;
		Entry.Type = TEXT("Unknown");
		Entry.Status = EStatus::NotFound;
		Entry.Reason = TEXT("No Blueprint asset is registered at this path");
		Entries.Add(MoveTemp(Entry));
	}
	for (const FCortexBPEnumeratedAsset& Found : Enumeration.Assets)
	{
		Entries.Add(CompileOne(Found.Asset, Found.Type));
	}
	if (Enumeration.Assets.Num() > 0)
	{
		// CompileOne defers garbage collection per asset; one pass for the whole run.
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}
	Algo::SortBy(Entries, &FEntry::AssetPath, [](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });

	int32 Counts[5] = {0, 0, 0, 0, 0}; // success, warning, error, skipped, not_found
	TArray<TSharedPtr<FJsonValue>> AssetValues;
	TArray<TSharedPtr<FJsonValue>> FirstFailures;
	for (const FEntry& Entry : Entries)
	{
		++Counts[static_cast<int32>(Entry.Status)];
		AssetValues.Add(MakeShared<FJsonValueObject>(ToJson(Entry, /*bWithDiagnostics=*/true)));
		if (Entry.Status == EStatus::Error && FirstFailures.Num() < MaxFirstFailures)
		{
			FirstFailures.Add(MakeShared<FJsonValueObject>(ToJson(Entry, /*bWithDiagnostics=*/false)));
		}
	}

	TSharedPtr<FJsonObject> Totals = MakeShared<FJsonObject>();
	Totals->SetNumberField(TEXT("total"), Entries.Num());
	Totals->SetNumberField(TEXT("success"), Counts[0]);
	Totals->SetNumberField(TEXT("warning"), Counts[1]);
	Totals->SetNumberField(TEXT("error"), Counts[2]);
	Totals->SetNumberField(TEXT("skipped"), Counts[3]);
	Totals->SetNumberField(TEXT("not_found"), Counts[4]);
	Totals->SetNumberField(TEXT("excluded_by_type"), Enumeration.ExcludedByType);

	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	FCortexBPAssetEnumeration::WriteRequestFields(Request, *Report);
	Report->SetObjectField(TEXT("totals"), Totals);
	Report->SetArrayField(TEXT("assets"), AssetValues);

	const FCortexJsonFileWriteResult WriteResult = FCortexSafeFileContract::WriteJsonReportAtomic(Request.OutPath, Report);
	if (!WriteResult.bWritten)
	{
		return FCortexCommandRouter::Error(
			WriteResult.ErrorCode.IsEmpty() ? CortexErrorCodes::SaveFailed : WriteResult.ErrorCode, WriteResult.ErrorMessage);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("out_path"), Request.OutPath.AbsolutePath);
	Data->SetObjectField(TEXT("totals"), Totals);
	Data->SetArrayField(TEXT("first_failures"), FirstFailures);
	return FCortexCommandRouter::Success(Data);
}
