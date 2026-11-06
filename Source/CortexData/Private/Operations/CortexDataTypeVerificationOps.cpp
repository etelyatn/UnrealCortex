#include "Operations/CortexDataTypeVerificationOps.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "CortexAssetMutationGuard.h"
#include "CortexEngineCompat.h"
#include "CortexSafeFileContract.h"
#include "CortexTypes.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Operations/CortexDataExportOps.h"
#include "Operations/CortexDataJsonDiffOps.h"
#include "Operations/CortexDataTypeReplacementOps.h"
#include "Operations/CortexDataTypeReplacementPlan.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/CoreRedirects.h"
#include "UObject/SavePackage.h"

namespace
{
	const TCHAR* const VerifyScriptStructClass = TEXT("/Script/CoreUObject.ScriptStruct");
	const TCHAR* const VerifyEnumClass = TEXT("/Script/CoreUObject.Enum");
	constexpr int32 MaxChangedSamples = 10;
	constexpr int32 MaxDiagnostics = 10;
	constexpr int32 VerifyMaxResponseAssets = 100;

	TArray<TSharedPtr<FJsonValue>> VerifyStringValues(const TArray<FString>& Strings)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& String : Strings)
		{
			Values.Add(MakeShared<FJsonValueString>(String));
		}
		return Values;
	}

	FCortexCommandResult VerifyInvalidField(const FString& Message)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, Message);
	}

	/** Why a redirect line of the plan is not active in this editor; empty when it is. */
	struct FProbeFailure
	{
		int32 LineIndex = INDEX_NONE;
		FString Reason;
	};

	void ProbeStructRedirects(const FCortexDataTypeReplacementPlan& Plan, const UScriptStruct* NativeStruct, TArray<FProbeFailure>& Failures)
	{
		const FCoreRedirectObjectName Old(Plan.UserType);

		// Line 0: a UDS import is a Type_Object import; OverrideClassName swaps its class.
		FCoreRedirectObjectName Redirected;
		const FCoreRedirect* ValueRedirect = nullptr;
		FCoreRedirects::RedirectNameAndValues(ECoreRedirectFlags::Type_Object, Old, Redirected, &ValueRedirect);
		if (Redirected.ToString() != Plan.NativeType)
		{
			Failures.Add({ 0, FString::Printf(TEXT("Type_Object redirect of %s to %s is not registered"), *Plan.UserType, *Plan.NativeType) });
		}
		else if (ValueRedirect == nullptr || ValueRedirect->OverrideClassName.ToString() != VerifyScriptStructClass)
		{
			Failures.Add({ 0, TEXT("the object redirect has no OverrideClassName=/Script/CoreUObject.ScriptStruct, so the UDS import keeps its class") });
		}

		// Line 1: struct-typed tagged values (a Blueprint's CDO defaults) are looked up as Type_Struct.
		if (FCoreRedirects::GetRedirectedName(ECoreRedirectFlags::Type_Struct, Old).ToString() != Plan.NativeType)
		{
			Failures.Add({ 1, FString::Printf(TEXT("Type_Struct redirect of %s to %s is not registered"), *Plan.UserType, *Plan.NativeType) });
		}

		// Lines 2..: one property redirect per member, keyed on the native struct.
		for (int32 Index = 0; Index < Plan.Members.Num(); ++Index)
		{
			const FCortexDataTypeReplacementMember& Member = Plan.Members[Index];
			if (FProperty::FindRedirectedPropertyName(NativeStruct, FName(*Member.UserName)) != FName(*Member.NativeName))
			{
				Failures.Add({ 2 + Index, FString::Printf(TEXT("Property redirect %s -> %s is not registered"), *Member.UserName, *Member.NativeName) });
			}
		}
	}

	void ProbeEnumRedirects(const FCortexDataTypeReplacementPlan& Plan, const UEnum* NativeEnum, TArray<FProbeFailure>& Failures)
	{
		const FCoreRedirectObjectName Old(Plan.UserType);
		FCoreRedirectObjectName Redirected;
		const FCoreRedirect* ValueRedirect = nullptr;
		FCoreRedirects::RedirectNameAndValues(ECoreRedirectFlags::Type_Enum, Old, Redirected, &ValueRedirect);
		if (Redirected.ToString() != Plan.NativeType)
		{
			Failures.Add({ 0, FString::Printf(TEXT("Type_Enum redirect of %s to %s is not registered"), *Plan.UserType, *Plan.NativeType) });
			return;
		}
		if (ValueRedirect == nullptr || ValueRedirect->OverrideClassName.ToString() != VerifyEnumClass)
		{
			Failures.Add({ 0, TEXT("the enum redirect has no OverrideClassName=/Script/CoreUObject.Enum") });
		}
		const TMap<FString, FString>* Values = FCoreRedirects::GetValueRedirects(ECoreRedirectFlags::Type_Enum, FCoreRedirectObjectName(NativeEnum));
		for (const TPair<FString, FString>& Value : Plan.Values)
		{
			const FString* Mapped = Values != nullptr ? Values->Find(Value.Key) : nullptr;
			if (Mapped == nullptr || *Mapped != Value.Value)
			{
				Failures.Add({ 0, FString::Printf(TEXT("ValueChanges %s -> %s is not registered"), *Value.Key, *Value.Value) });
			}
		}
	}

	/**
	 * Renames the baseline into the vocabulary of a fresh export. The exporter writes raw property names, so a raw diff would flag every
	 * key of a user-defined struct: the GUID-suffixed names go through the plan's member map, enum tokens through its value map, and the
	 * struct name in _struct_type / row_struct becomes the native one. Nothing else is touched, so a real difference stays one.
	 */
	struct FNormalizer
	{
		TMap<FString, FString> MemberMap;
		TMap<FString, FString> ValueMap;
		FString UserName;
		FString NativeName;
		bool bStruct = true;

		TSharedPtr<FJsonValue> Normalize(const TSharedPtr<FJsonValue>& Value) const
		{
			if (!Value.IsValid())
			{
				return Value;
			}
			switch (Value->Type)
			{
			case EJson::Object:
			{
				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				for (const auto& Pair : Value->AsObject()->Values)
				{
					const FString Key = CortexEngineCompat::JsonKeyToString(Pair.Key);
					const FString* Renamed = bStruct ? MemberMap.Find(Key) : nullptr;
					const bool bStructName = bStruct && (Key == TEXT("_struct_type") || Key == TEXT("row_struct"));
					FString Text;
					if (bStructName && Pair.Value.IsValid() && Pair.Value->TryGetString(Text) && Text == UserName)
					{
						Out->SetStringField(Key, NativeName);
					}
					else
					{
						Out->SetField(Renamed != nullptr ? *Renamed : Key, Normalize(Pair.Value));
					}
				}
				return MakeShared<FJsonValueObject>(Out);
			}
			case EJson::Array:
			{
				TArray<TSharedPtr<FJsonValue>> Out;
				for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
				{
					Out.Add(Normalize(Item));
				}
				return MakeShared<FJsonValueArray>(Out);
			}
			case EJson::String:
			{
				if (!bStruct)
				{
					if (const FString* Mapped = ValueMap.Find(Value->AsString()))
					{
						return MakeShared<FJsonValueString>(*Mapped);
					}
				}
				return Value;
			}
			default:
				return Value;
			}
		}
	};

	bool ReadJsonFile(const FString& Path, TSharedPtr<FJsonObject>& Out, FCortexCommandResult& OutError)
	{
		FCortexResolvedFilePath Resolved;
		FString ErrorCode;
		FString ErrorMessage;
		FString Contents;
		if (!FCortexSafeFileContract::ResolveReadPath(Path, Resolved, ErrorCode, ErrorMessage)
			|| !FCortexSafeFileContract::ReadTextFile(Resolved, Contents, ErrorCode, ErrorMessage))
		{
			OutError = FCortexCommandRouter::Error(ErrorCode, ErrorMessage);
			return false;
		}
		TSharedPtr<FJsonValue> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Contents), Root) || !Root.IsValid() || Root->Type != EJson::Object)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::MalformedJson, FString::Printf(TEXT("%s is not a JSON object"), *Path));
			return false;
		}
		Out = Root->AsObject();
		return true;
	}

	const TCHAR* StatusName(const EBlueprintStatus Status)
	{
		switch (Status)
		{
		case BS_UpToDate:
			return TEXT("up_to_date");
		case BS_UpToDateWithWarnings:
			return TEXT("up_to_date_with_warnings");
		case BS_Error:
			return TEXT("error");
		case BS_Dirty:
			return TEXT("dirty");
		default:
			return TEXT("unknown");
		}
	}

	/** Saves the asset's package; the explicit, caller-requested save. */
	bool SaveAssetPackage(UObject* Asset, FString& OutFile)
	{
		UPackage* Package = Asset->GetOutermost();
		OutFile = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Package, Asset, *OutFile, SaveArgs);
	}

	/** Exports the loaded asset with the same exporter the plan used for its baseline. */
	FCortexCommandResult ExportAsset(const FCortexDataTypeReplacementBaseline& Baseline, const FString& OutPath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("out_path"), OutPath);
		if (Baseline.Kind == TEXT("datatable"))
		{
			Params->SetStringField(TEXT("table_path"), Baseline.AssetPath);
			return FCortexDataExportOps::ExportDatatableJson(Params);
		}
		TArray<TSharedPtr<FJsonValue>> AssetPaths;
		AssetPaths.Add(MakeShared<FJsonValueString>(Baseline.AssetPath));
		Params->SetArrayField(TEXT("asset_paths"), AssetPaths);
		Params->SetBoolField(TEXT("include_properties"), true);
		return FCortexDataExportOps::ExportDataAssetsJson(Params);
	}

	/** Diffs the normalized baseline of one asset against a fresh export; fills the per-asset result. */
	void DiffBaseline(
		const FCortexDataTypeReplacementBaseline& Baseline,
		const FNormalizer& Normalizer,
		const FString& WorkDir,
		TSharedRef<FJsonObject> Entry,
		bool& bOutEqual)
	{
		bOutEqual = false;
		const FString Stem = FPaths::GetBaseFilename(Baseline.File);
		const FString NormalizedPath = FPaths::Combine(WorkDir, Stem + TEXT(".baseline.normalized.json"));
		const FString FreshPath = FPaths::Combine(WorkDir, Stem + TEXT(".fresh.json"));
		const FString ReportPath = FPaths::Combine(WorkDir, Stem + TEXT(".diff.json"));

		TSharedPtr<FJsonObject> BaselineJson;
		FCortexCommandResult ReadError;
		if (!ReadJsonFile(Baseline.File, BaselineJson, ReadError))
		{
			Entry->SetStringField(TEXT("error"), FString::Printf(TEXT("baseline file unreadable: %s"), *ReadError.ErrorMessage));
			return;
		}
		const TSharedPtr<FJsonValue> Normalized = Normalizer.Normalize(MakeShared<FJsonValueObject>(BaselineJson));

		FCortexResolvedFilePath ResolvedNormalized;
		FString ErrorCode;
		FString ErrorMessage;
		if (!FCortexSafeFileContract::ResolveWritePath(NormalizedPath, ResolvedNormalized, ErrorCode, ErrorMessage))
		{
			Entry->SetStringField(TEXT("error"), ErrorMessage);
			return;
		}
		const FCortexJsonFileWriteResult Written = FCortexSafeFileContract::WriteJsonReportAtomic(ResolvedNormalized, Normalized->AsObject().ToSharedRef());
		if (!Written.bWritten)
		{
			Entry->SetStringField(TEXT("error"), Written.ErrorMessage);
			return;
		}

		const FCortexCommandResult Fresh = ExportAsset(Baseline, FreshPath);
		if (!Fresh.bSuccess)
		{
			Entry->SetStringField(TEXT("error"), FString::Printf(TEXT("fresh export failed: %s"), *Fresh.ErrorMessage));
			return;
		}

		TSharedPtr<FJsonObject> CompareParams = MakeShared<FJsonObject>();
		CompareParams->SetStringField(TEXT("left_path"), NormalizedPath);
		CompareParams->SetStringField(TEXT("right_path"), FreshPath);
		CompareParams->SetStringField(TEXT("report_path"), ReportPath);
		CompareParams->SetStringField(TEXT("mode"), Baseline.Kind == TEXT("datatable") ? TEXT("datatable_rows") : TEXT("data_assets"));
		const FCortexCommandResult Compared = FCortexDataJsonDiffOps::CompareDataJson(CompareParams);
		if (!Compared.bSuccess)
		{
			Entry->SetStringField(TEXT("error"), FString::Printf(TEXT("compare_data_json failed: %s"), *Compared.ErrorMessage));
			return;
		}

		TSharedPtr<FJsonObject> Report;
		if (!ReadJsonFile(ReportPath, Report, ReadError))
		{
			Entry->SetStringField(TEXT("error"), FString::Printf(TEXT("diff report unreadable: %s"), *ReadError.ErrorMessage));
			return;
		}
		const TSharedPtr<FJsonObject> Counts = Report->GetObjectField(TEXT("counts"));
		const int32 Added = static_cast<int32>(Counts->GetNumberField(TEXT("added")));
		const int32 Removed = static_cast<int32>(Counts->GetNumberField(TEXT("removed")));
		const int32 Changed = static_cast<int32>(Counts->GetNumberField(TEXT("changed")));
		Entry->SetNumberField(TEXT("rows_added"), Added);
		Entry->SetNumberField(TEXT("rows_removed"), Removed);
		Entry->SetNumberField(TEXT("rows_changed"), Changed);

		TArray<TSharedPtr<FJsonValue>> Samples;
		for (const TCHAR* Field : { TEXT("changed"), TEXT("added"), TEXT("removed") })
		{
			for (const TSharedPtr<FJsonValue>& Record : Report->GetArrayField(Field))
			{
				if (Samples.Num() >= MaxChangedSamples)
				{
					break;
				}
				const TSharedPtr<FJsonObject> Object = Record->AsObject();
				TSharedRef<FJsonObject> Sample = MakeShared<FJsonObject>();
				Sample->SetStringField(TEXT("key"), Object->GetStringField(TEXT("key")));
				Sample->SetStringField(TEXT("difference"), Field);
				if (FCString::Strcmp(Field, TEXT("changed")) == 0)
				{
					TArray<FString> Fields;
					for (const auto& Pair : Object->GetObjectField(TEXT("fields"))->Values)
					{
						Fields.Add(CortexEngineCompat::JsonKeyToString(Pair.Key));
					}
					Fields.Sort();
					Sample->SetArrayField(TEXT("fields"), VerifyStringValues(Fields));
				}
				Samples.Add(MakeShared<FJsonValueObject>(Sample));
			}
		}
		Entry->SetArrayField(TEXT("changed"), Samples);
		bOutEqual = Added == 0 && Removed == 0 && Changed == 0;
		Entry->SetBoolField(TEXT("equal"), bOutEqual);
	}
}

FCortexCommandResult FCortexDataTypeVerificationOps::VerifyTypeReplacement(const TSharedPtr<FJsonObject>& Params)
{
	FString PlanPath;
	bool bSave = false;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("plan_path"), PlanPath) || PlanPath.TrimStartAndEnd().IsEmpty()
		|| !Params->TryGetBoolField(TEXT("save"), bSave))
	{
		return VerifyInvalidField(TEXT("Missing required params: plan_path (string) and save (boolean; the save is always an explicit choice)"));
	}
	FString OutPath;
	if (Params->HasField(TEXT("out_path")) && (!Params->TryGetStringField(TEXT("out_path"), OutPath) || OutPath.TrimStartAndEnd().IsEmpty()))
	{
		return VerifyInvalidField(TEXT("out_path must be a non-empty string"));
	}

	// 1. The plan, intact.
	TSharedPtr<FJsonObject> PlanJson;
	FCortexCommandResult ReadError;
	if (!ReadJsonFile(PlanPath, PlanJson, ReadError))
	{
		return ReadError;
	}
	FCortexDataTypeReplacementPlan Plan;
	FString PlanError;
	if (!FCortexDataTypeReplacementPlan::FromJson(PlanJson, Plan, PlanError))
	{
		return VerifyInvalidField(FString::Printf(TEXT("%s is not a type replacement plan: %s"), *PlanPath, *PlanError));
	}
	if (Plan.ComputeHash() != Plan.PlanHash)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::StalePrecondition, FString::Printf(TEXT("%s was edited after it was generated (plan_hash mismatch); plan again"), *PlanPath));
	}
	const bool bStruct = Plan.Kind == TEXT("struct");
	UScriptStruct* NativeStruct = bStruct ? FCortexDataTypeReplacementOps::FindNativeStruct(Plan.NativeType) : nullptr;
	UEnum* NativeEnum = bStruct ? nullptr : FCortexDataTypeReplacementOps::FindNativeEnum(Plan.NativeType);
	if (NativeStruct == nullptr && NativeEnum == nullptr)
	{
		return VerifyInvalidField(FString::Printf(TEXT("The native type %s of the plan does not exist in this editor"), *Plan.NativeType));
	}

	// 2. The redirects must be active in this editor; they are never added here.
	TArray<FProbeFailure> Failures;
	if (bStruct)
	{
		ProbeStructRedirects(Plan, NativeStruct, Failures);
	}
	else
	{
		ProbeEnumRedirects(Plan, NativeEnum, Failures);
	}
	if (Failures.Num() > 0)
	{
		TArray<FString> Missing;
		TArray<TSharedPtr<FJsonValue>> Reasons;
		for (const FProbeFailure& Failure : Failures)
		{
			if (Plan.RedirectLines.IsValidIndex(Failure.LineIndex))
			{
				Missing.AddUnique(Plan.RedirectLines[Failure.LineIndex]);
			}
			Reasons.Add(MakeShared<FJsonValueString>(Failure.Reason));
		}
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("missing_redirects"), VerifyStringValues(Missing));
		Details->SetArrayField(TEXT("reasons"), Reasons);
		Details->SetStringField(TEXT("instruction"), TEXT("Add the missing lines to the [CoreRedirects] section of Config/DefaultEngine.ini and restart the editor (editor_restart): redirects are read once at start and apply while a package loads, so nothing already loaded converts. Cortex never adds redirects at runtime."));
		return FCortexCommandRouter::Error(
			CortexErrorCodes::RedirectsNotActive,
			FString::Printf(TEXT("%d redirect line(s) of the plan are not active in this editor; nothing was loaded or saved"), Missing.Num()),
			Details);
	}

	// 3. Preconditions of the explicit save, checked before anything is loaded or touched: clean packages, no blocked asset.
	TArray<FCortexDataTypeReplacementAsset> Verifiable;
	TArray<TSharedPtr<FJsonValue>> Skipped;
	for (const FCortexDataTypeReplacementAsset& Asset : Plan.AffectedAssets)
	{
		if (Asset.Kind == TEXT("blueprint") || Asset.Kind == TEXT("datatable") || Asset.Kind == TEXT("data_asset"))
		{
			Verifiable.Add(Asset);
			continue;
		}
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_path"), Asset.Path);
		Entry->SetStringField(TEXT("kind"), Asset.Kind);
		Entry->SetStringField(TEXT("reason"), TEXT("not loaded by verify_type_replacement (a nested user type, a world or another asset class); its references show in still_referenced"));
		Skipped.Add(MakeShared<FJsonValueObject>(Entry));
	}
	if (bSave)
	{
		TArray<FString> Dirty;
		TArray<FString> Blocked;
		for (const FCortexDataTypeReplacementAsset& Asset : Verifiable)
		{
			const UPackage* Loaded = FindPackage(nullptr, *FPackageName::ObjectPathToPackageName(Asset.Path));
			if (Loaded != nullptr && Loaded->IsDirty())
			{
				Dirty.Add(Asset.Path);
			}
			FString Reason;
			if (FCortexAssetMutationGuard::IsPathBlocked(Asset.Path, Reason))
			{
				Blocked.Add(FString::Printf(TEXT("%s: %s"), *Asset.Path, *Reason));
			}
		}
		if (Dirty.Num() > 0)
		{
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetArrayField(TEXT("dirty_assets"), VerifyStringValues(Dirty));
			return FCortexCommandRouter::Error(
				CortexErrorCodes::DirtyEditorState,
				TEXT("save:true resaves every affected asset, but these have unsaved editor changes that a resave would write too; save or discard them first (or verify with save:false)"),
				Details);
		}
		if (Blocked.Num() > 0)
		{
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetArrayField(TEXT("blocked_assets"), VerifyStringValues(Blocked));
			return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("These assets are blocked after an unverified recovery and cannot be saved"), Details);
		}
	}

	// 4. Load, compile, diff, resave.
	FNormalizer Normalizer;
	Normalizer.MemberMap = Plan.MemberMap();
	Normalizer.ValueMap = Plan.ValueMap();
	Normalizer.bStruct = bStruct;
	Normalizer.UserName = FPackageName::ObjectPathToObjectName(Plan.UserType);
	Normalizer.NativeName = bStruct ? NativeStruct->GetName() : NativeEnum->GetName();
	const FString WorkDir = FPaths::Combine(FPaths::GetPath(PlanPath), TEXT("verify"));
	const UObject* UserTypeObject = FindObject<UObject>(nullptr, *Plan.UserType);

	TArray<TSharedPtr<FJsonValue>> AssetResults;
	TArray<FString> SavedFiles;
	int32 ProblemCount = 0;
	int32 SavedCount = 0;
	bool bAllEqual = Plan.Baseline.Num() > 0;
	for (const FCortexDataTypeReplacementAsset& Asset : Verifiable)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_path"), Asset.Path);
		Entry->SetStringField(TEXT("kind"), Asset.Kind);

		bool bOk = true;
		UObject* Loaded = FCortexDataTypeReplacementOps::LoadAssetObject(Asset.Path);
		const FCortexDataTypeReplacementBaseline* Baseline = Plan.Baseline.FindByPredicate(
			[&Asset](const FCortexDataTypeReplacementBaseline& Candidate) { return Candidate.AssetPath == Asset.Path; });
		if (Loaded == nullptr)
		{
			Entry->SetBoolField(TEXT("loaded"), false);
			Entry->SetStringField(TEXT("error"), TEXT("the asset could not be loaded"));
			bOk = false;
		}
		else
		{
			Entry->SetBoolField(TEXT("loaded"), true);
			UPackage* Package = Loaded->GetOutermost();
			const bool bWasDirty = Package->IsDirty();

			if (UBlueprint* Blueprint = Cast<UBlueprint>(Loaded))
			{
				bool bStillUsesUserType = false;
				for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
				{
					bStillUsesUserType |= UserTypeObject != nullptr
						&& (Variable.VarType.PinSubCategoryObject.Get() == UserTypeObject || Variable.VarType.PinValueType.TerminalSubCategoryObject.Get() == UserTypeObject);
				}
				FCompilerResultsLog Log;
				FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Log);
				Entry->SetStringField(TEXT("compile_status"), StatusName(Blueprint->Status));
				TArray<FString> Diagnostics;
				for (const TSharedRef<FTokenizedMessage>& Message : Log.Messages)
				{
					if (Message->GetSeverity() <= EMessageSeverity::Warning && Diagnostics.Num() < MaxDiagnostics)
					{
						Diagnostics.Add(Message->ToText().ToString());
					}
				}
				Entry->SetArrayField(TEXT("diagnostics"), VerifyStringValues(Diagnostics));
				if (Blueprint->Status == BS_Error)
				{
					bOk = false;
				}
				if (bStillUsesUserType)
				{
					Entry->SetBoolField(TEXT("converted"), false);
					Entry->SetStringField(TEXT("error"), TEXT("a variable still uses the user-defined type: the asset was loaded before the redirects were active; restart the editor"));
					bOk = false;
				}
				else
				{
					Entry->SetBoolField(TEXT("converted"), true);
				}
			}
			else if (const UDataTable* Table = Cast<UDataTable>(Loaded))
			{
				if (bStruct && Table->GetRowStruct() != NativeStruct)
				{
					Entry->SetBoolField(TEXT("converted"), false);
					Entry->SetStringField(TEXT("error"), FString::Printf(TEXT("the DataTable's row struct is %s, not the native struct: it was loaded before the redirects were active; restart the editor"),
						Table->GetRowStruct() != nullptr ? *Table->GetRowStruct()->GetPathName() : TEXT("missing")));
					bOk = false;
				}
				else
				{
					Entry->SetBoolField(TEXT("converted"), true);
				}
			}

			if (Baseline != nullptr && bOk)
			{
				bool bEqual = false;
				DiffBaseline(*Baseline, Normalizer, WorkDir, Entry, bEqual);
				bAllEqual &= bEqual;
				if (Entry->HasField(TEXT("error")))
				{
					bOk = false;
				}
			}
			else if (Baseline != nullptr)
			{
				bAllEqual = false;
			}

			bool bSaved = false;
			if (bSave && bOk)
			{
				FString File;
				Package->MarkPackageDirty();
				bSaved = SaveAssetPackage(Loaded, File);
				if (bSaved)
				{
					SavedFiles.Add(File);
					++SavedCount;
				}
				else
				{
					Entry->SetStringField(TEXT("error"), TEXT("the package could not be saved"));
					bOk = false;
				}
			}
			else if (!bSave && !bWasDirty)
			{
				// In-memory verification leaves the editor state as it found it.
				Package->ClearDirtyFlag();
			}
			Entry->SetBoolField(TEXT("saved"), bSaved);
		}
		if (!bOk)
		{
			++ProblemCount;
		}
		Entry->SetBoolField(TEXT("ok"), bOk);
		AssetResults.Add(MakeShared<FJsonValueObject>(Entry));
	}
	if (SavedFiles.Num() > 0)
	{
		IAssetRegistry::GetChecked().ScanModifiedAssetFiles(SavedFiles);
	}

	// 5. What still references the user type. The type itself is never deleted here.
	TArray<TSharedPtr<FJsonValue>> StillReferenced;
	for (const FCortexDataTypeReplacementAsset& Asset : FCortexDataTypeReplacementOps::FindAffectedAssets(Plan.UserType))
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_path"), Asset.Path);
		Entry->SetStringField(TEXT("kind"), Asset.Kind);
		StillReferenced.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("redirects_active"), true);
	Data->SetStringField(TEXT("plan_hash"), Plan.PlanHash);
	Data->SetStringField(TEXT("user_type"), Plan.UserType);
	Data->SetStringField(TEXT("native_type"), Plan.NativeType);
	Data->SetBoolField(TEXT("save_requested"), bSave);
	Data->SetNumberField(TEXT("saved_count"), SavedCount);
	Data->SetNumberField(TEXT("asset_count"), AssetResults.Num());
	Data->SetNumberField(TEXT("problem_count"), ProblemCount);
	const bool bBaselineCompared = Plan.Baseline.Num() > 0;
	Data->SetBoolField(TEXT("baseline_compared"), bBaselineCompared);
	if (bBaselineCompared)
	{
		Data->SetBoolField(TEXT("equal"), bAllEqual && ProblemCount == 0);
	}
	else
	{
		Data->SetField(TEXT("equal"), MakeShared<FJsonValueNull>());
		Data->SetStringField(TEXT("note"), TEXT("The plan has no baseline (plan_type_replacement ran without baseline_dir): only the structural results (redirects, load, compile, still_referenced) can be reported, not that the data is unchanged."));
	}
	Data->SetArrayField(TEXT("skipped"), Skipped);
	Data->SetNumberField(TEXT("still_referenced_count"), StillReferenced.Num());
	Data->SetArrayField(TEXT("still_referenced"), StillReferenced);
	Data->SetBoolField(TEXT("user_type_deleted"), false);
	Data->SetStringField(TEXT("next_step"), StillReferenced.Num() == 0
		? TEXT("Nothing references the user-defined type any more: delete it with core.delete_asset when ready, then remove the redirects from the ini.")
		: TEXT("Assets still reference the user-defined type (see still_referenced); resave them with save:true or migrate them before deleting it."));

	if (!OutPath.IsEmpty())
	{
		TSharedRef<FJsonObject> Full = MakeShared<FJsonObject>();
		for (const auto& Pair : Data->Values)
		{
			Full->SetField(CortexEngineCompat::JsonKeyToString(Pair.Key), Pair.Value);
		}
		Full->SetArrayField(TEXT("assets"), AssetResults);
		FCortexResolvedFilePath Resolved;
		FString ErrorCode;
		FString ErrorMessage;
		if (!FCortexSafeFileContract::ResolveWritePath(OutPath, Resolved, ErrorCode, ErrorMessage))
		{
			return FCortexCommandRouter::Error(ErrorCode, ErrorMessage);
		}
		const FCortexJsonFileWriteResult Written = FCortexSafeFileContract::WriteJsonReportAtomic(Resolved, Full);
		if (!Written.bWritten)
		{
			return FCortexCommandRouter::Error(Written.ErrorCode, Written.ErrorMessage);
		}
		Data->SetStringField(TEXT("report_path"), OutPath);
	}

	const bool bTruncated = AssetResults.Num() > VerifyMaxResponseAssets;
	if (bTruncated)
	{
		AssetResults.SetNum(VerifyMaxResponseAssets);
	}
	Data->SetBoolField(TEXT("assets_truncated"), bTruncated);
	Data->SetArrayField(TEXT("assets"), AssetResults);
	return FCortexCommandRouter::Success(Data);
}
