#include "Operations/CortexAssetSearchOps.h"
#include "CortexDataModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Algo/Sort.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"

namespace
{
	constexpr int32 DefaultSearchLimit = 50;

	const TCHAR* const ProjectionWhitelist[] = {
		TEXT("path"), TEXT("name"), TEXT("class_name"), TEXT("package_path")
	};

	bool IsWhitelistedProjectionField(const FString& Field)
	{
		for (const TCHAR* const Allowed : ProjectionWhitelist)
		{
			if (Field == Allowed)
			{
				return true;
			}
		}

		return false;
	}

	/** A path prefix is a canonical mounted package folder: absolute, no trailing slash, no parent
	 *  traversal or empty components, and a valid long package name. */
	bool IsCanonicalPackageFolder(const FString& Prefix)
	{
		if (Prefix.IsEmpty()
			|| !Prefix.StartsWith(TEXT("/"))
			|| Prefix.EndsWith(TEXT("/"))
			|| Prefix.Contains(TEXT(".."))
			|| Prefix.Contains(TEXT("//")))
		{
			return false;
		}

		return FPackageName::IsValidLongPackageName(Prefix);
	}

	/** Reads an optional string-array parameter. Returns false (with OutError) when the field is
	 *  present but is not an array of strings, so malformed filters refuse before any registry
	 *  fetch instead of silently degrading to an unfiltered project scan. */
	bool ReadOptionalStringArray(
		const TSharedPtr<FJsonObject>& Params,
		const FString& FieldName,
		TArray<FString>& OutValues,
		FString& OutError)
	{
		OutValues.Reset();

		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(FieldName, Values) || Values == nullptr)
		{
			OutError = FString::Printf(TEXT("'%s' must be an array of strings"), *FieldName);
			return false;
		}

		OutValues.Reserve(Values->Num());
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			// Strict type check: FJsonValueNumber/FJsonValueBool override TryGetString with a
			// conversion, so the element must actually be a JSON string before conversion.
			FString Element;
			if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Element))
			{
				OutError = FString::Printf(TEXT("'%s' entries must be strings"), *FieldName);
				return false;
			}

			OutValues.Add(Element);
		}

		return true;
	}
}

int32 FCortexDataAssetSearchOps::ComputePageEnd(int32 TotalMatches, int32 Offset, int32 Limit)
{
	// Widen before adding so the largest representable limit cannot wrap the window end; the result
	// is identical to Min(TotalMatches, Offset + Limit) for every non-wrapping input.
	const int64 RequestedEnd = static_cast<int64>(Offset) + static_cast<int64>(Limit);
	return static_cast<int32>(FMath::Min(static_cast<int64>(TotalMatches), RequestedEnd));
}

FCortexCommandResult FCortexDataAssetSearchOps::SearchAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString Query;
	TArray<FString> ClassNames;
	TArray<FString> PathPrefixes;
	TArray<FString> ProjectionFields;
	bool bCountOnly = false;
	int32 Offset = 0;
	int32 Limit = DefaultSearchLimit;

	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("query"), Query);

		FString ValidationError;
		if (!ReadOptionalStringArray(Params, TEXT("class_names"), ClassNames, ValidationError)
			|| !ReadOptionalStringArray(Params, TEXT("path_prefixes"), PathPrefixes, ValidationError)
			|| !ReadOptionalStringArray(Params, TEXT("fields"), ProjectionFields, ValidationError))
		{
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, ValidationError);
		}

		for (const FString& ClassName : ClassNames)
		{
			if (ClassName.IsEmpty())
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					TEXT("'class_names' entries must be non-empty class names")
				);
			}
		}

		for (const FString& Prefix : PathPrefixes)
		{
			if (!IsCanonicalPackageFolder(Prefix))
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(
						TEXT("'path_prefixes' entry '%s' is not a canonical package folder (expected an absolute mounted folder such as /Game/MyFolder)"),
						*Prefix)
				);
			}
		}

		for (const FString& Field : ProjectionFields)
		{
			if (Field.IsEmpty() || !IsWhitelistedProjectionField(Field))
			{
				return FCortexCommandRouter::Error(
					CortexErrorCodes::InvalidField,
					FString::Printf(
						TEXT("'fields' entry '%s' is not supported (allowed: path, name, class_name, package_path)"),
						*Field)
				);
			}
		}

		Params->TryGetBoolField(TEXT("count_only"), bCountOnly);

		int32 ParamOffset = 0;
		if (Params->TryGetNumberField(TEXT("offset"), ParamOffset) && ParamOffset > 0)
		{
			Offset = ParamOffset;
		}

		int32 ParamLimit = 0;
		if (Params->TryGetNumberField(TEXT("limit"), ParamLimit) && ParamLimit > 0)
		{
			Limit = ParamLimit;
		}
	}

	if (ProjectionFields.Num() == 0)
	{
		ProjectionFields = { TEXT("path"), TEXT("name"), TEXT("class_name") };
	}

	IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
	if (AssetRegistry == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::EditorNotReady,
			TEXT("AssetRegistry is not available")
		);
	}

	// Exact class-name semantics: compare the candidate's own class name, never subclasses. The
	// caller-provided class names are converted once so no class resolution or per-candidate
	// string allocation is needed.
	TArray<FName> ClassNameSet;
	ClassNameSet.Reserve(ClassNames.Num());
	for (const FString& ClassName : ClassNames)
	{
		ClassNameSet.Add(FName(*ClassName));
	}

	// Canonical recursive folder prefixes are applied by the registry itself, so path matches use
	// package-folder component boundaries rather than arbitrary string prefixes.
	TArray<FAssetData> Candidates;
	if (PathPrefixes.Num() == 0)
	{
		if (AssetRegistry->IsLoadingAssets())
		{
			return FCortexCommandRouter::Error(
				CortexErrorCodes::EditorNotReady,
				TEXT("Asset Registry scan in progress - retry search_assets in a few seconds")
			);
		}

		AssetRegistry->GetAllAssets(Candidates);
	}
	else
	{
		FARFilter Filter;
		Filter.bRecursivePaths = true;
		Filter.PackagePaths.Reserve(PathPrefixes.Num());
		for (const FString& Prefix : PathPrefixes)
		{
			Filter.PackagePaths.Add(FName(*Prefix));
		}

		AssetRegistry->GetAssets(Filter, Candidates);
	}

	// Filter every candidate first: total_matches is exact and independent of limit/offset.
	TArray<int32> MatchIndices;
	MatchIndices.Reserve(Candidates.Num());
	for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
	{
		const FAssetData& Candidate = Candidates[CandidateIndex];

		if (ClassNameSet.Num() > 0 && !ClassNameSet.Contains(Candidate.AssetClassPath.GetAssetName()))
		{
			continue;
		}

		if (!Query.IsEmpty())
		{
			const FString AssetName = Candidate.AssetName.ToString();
			if (!AssetName.Contains(Query, ESearchCase::IgnoreCase)
				&& !Candidate.GetObjectPathString().Contains(Query, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		MatchIndices.Add(CandidateIndex);
	}

	const int32 TotalMatches = MatchIndices.Num();

	TArray<TSharedPtr<FJsonValue>> ResultArray;
	// count_only returns before ordering and row construction: the exact count is the whole
	// filtered population, so the terminal offset/completeness below is already correct.
	int32 NextOffset = Offset;
	bool bIsComplete = true;

	if (!bCountOnly && Offset < TotalMatches)
	{
		// Deterministic canonical object-path order before paging: case-sensitive alphabetical
		// comparison of (package, asset name) equals object-path ordering for valid names and needs
		// no per-comparison path conversion. FName::LexicalLess ignores case, which would order
		// case-variant names non-canonically.
		Algo::Sort(MatchIndices, [&Candidates](int32 Left, int32 Right)
		{
			const FAssetData& A = Candidates[Left];
			const FAssetData& B = Candidates[Right];
			return A.PackageName != B.PackageName
				? A.PackageName.CompareSensitive(B.PackageName) < 0
				: A.AssetName.CompareSensitive(B.AssetName) < 0;
		});

		const int32 PageEnd = ComputePageEnd(TotalMatches, Offset, Limit);
		ResultArray.Reserve(PageEnd - Offset);

		for (int32 MatchIndex = Offset; MatchIndex < PageEnd; ++MatchIndex)
		{
			const FAssetData& AssetData = Candidates[MatchIndices[MatchIndex]];
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();

			for (const FString& Field : ProjectionFields)
			{
				if (Field == TEXT("path"))
				{
					Entry->SetStringField(Field, AssetData.GetObjectPathString());
				}
				else if (Field == TEXT("name"))
				{
					Entry->SetStringField(Field, AssetData.AssetName.ToString());
				}
				else if (Field == TEXT("class_name"))
				{
					Entry->SetStringField(Field, AssetData.AssetClassPath.GetAssetName().ToString());
				}
				else if (Field == TEXT("package_path"))
				{
					// Matches the existing native convention (AssetData.PackagePath: the containing
					// package folder, with the package stripped off).
					Entry->SetStringField(Field, AssetData.PackagePath.ToString());
				}
			}

			ResultArray.Add(MakeShared<FJsonValueObject>(Entry));
		}

		NextOffset = PageEnd;
		bIsComplete = PageEnd >= TotalMatches;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("assets"), ResultArray);
	Data->SetNumberField(TEXT("count"), ResultArray.Num());
	Data->SetNumberField(TEXT("total_matches"), TotalMatches);
	Data->SetNumberField(TEXT("offset"), Offset);
	Data->SetNumberField(TEXT("limit"), Limit);
	Data->SetNumberField(TEXT("next_offset"), NextOffset);
	Data->SetBoolField(TEXT("is_complete"), bIsComplete);

	return FCortexCommandRouter::Success(Data);
}
