#include "Operations/CortexDataTypeVerificationOps.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "CortexAssetMutationGuard.h"
#include "CortexEngineCompat.h"
#include "CortexSafeFileContract.h"
#include "CortexTypes.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_StructMemberSet.h"
#include "K2Node_StructOperation.h"
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

	/** The enum behind a property when it is a (container-free) enum or byte-with-enum property. */
	const UEnum* PropertyEnum(const FProperty* Property)
	{
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			return EnumProperty->GetEnum();
		}
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			return ByteProperty->GetIntPropertyEnum();
		}
		return nullptr;
	}

	/** "ECortexIssue122Mode::A" -> "A": the exporter writes map keys as exported property text, which may carry the enum namespace. */
	FString StripEnumNamespace(const FString& Text)
	{
		int32 Index = INDEX_NONE;
		return Text.FindLastChar(TEXT(':'), Index) ? Text.RightChop(Index + 1) : Text;
	}

	/**
	 * Renames the baseline into the vocabulary of a fresh export. The exporter writes raw property names, so a raw diff would flag every
	 * key of a user-defined struct: the GUID-suffixed names (of the replaced struct and of its recorded inner prerequisites) go through the
	 * plan's member map and the struct name in _struct_type / row_struct becomes the native one. Enum values are normalized by TYPE, never
	 * by string value: only a value whose reflected property is the replaced enum (also as array/set element, map key or map value, at any
	 * struct depth) goes through the value map; record identities and unrelated strings are left alone, so a real difference stays one.
	 */
	struct FNormalizer
	{
		TMap<FString, FString> MemberMap;
		/** Native enum -> (old enumerator token -> native enumerator): the replaced enum of an enum plan and every inner enum a struct plan recorded. */
		TMap<const UEnum*, TMap<FString, FString>> EnumValueMaps;
		TMap<FString, FString> StructNames;
		FString UserPath;
		FString NativePath;
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
					const FString* Renamed = MemberMap.Find(Key);
					const bool bStructName = Key == TEXT("_struct_type") || Key == TEXT("row_struct");
					FString Text;
					const FString* NativeName = bStructName && Pair.Value.IsValid() && Pair.Value->TryGetString(Text) ? StructNames.Find(Text) : nullptr;
					if (NativeName != nullptr)
					{
						Out->SetStringField(Key, *NativeName);
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
			default:
				return Value;
			}
		}

		const TMap<FString, FString>* ValuesOf(const FProperty* Property) const
		{
			const UEnum* Enum = PropertyEnum(Property);
			return Enum != nullptr ? EnumValueMaps.Find(Enum) : nullptr;
		}

		TSharedPtr<FJsonValue> NormalizeTyped(const TSharedPtr<FJsonValue>& Value, const FProperty* Property, const bool bFresh) const
		{
			if (!Value.IsValid() || Property == nullptr)
			{
				return Value;
			}
			if (const TMap<FString, FString>* Values = ValuesOf(Property))
			{
				FString Text;
				if (!bFresh && Value->TryGetString(Text))
				{
					if (const FString* Mapped = Values->Find(StripEnumNamespace(Text)))
					{
						return MakeShared<FJsonValueString>(*Mapped);
					}
				}
				return Value;
			}
			// A reference to the replaced type itself (a UScriptStruct*/UEnum* field) is exported as its path: the same object, renamed.
			if (const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
			{
				FString Text;
				if (!ObjectProperty->HasAllPropertyFlags(CPF_InstancedReference) && Value->TryGetString(Text) && Text == UserPath)
				{
					return MakeShared<FJsonValueString>(NativePath);
				}
				return Value;
			}
			if (CastField<FSoftObjectProperty>(Property) != nullptr)
			{
				FString Text;
				if (Value->TryGetString(Text) && Text == UserPath)
				{
					return MakeShared<FJsonValueString>(NativePath);
				}
				return Value;
			}
			if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				if (Value->Type == EJson::Object && StructProperty->Struct != nullptr)
				{
					return MakeShared<FJsonValueObject>(NormalizeStruct(Value->AsObject(), StructProperty->Struct, bFresh));
				}
				return Value;
			}
			const FProperty* Element = nullptr;
			if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
			{
				Element = ArrayProperty->Inner;
			}
			else if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
			{
				Element = SetProperty->ElementProp;
			}
			if (Element != nullptr)
			{
				if (Value->Type != EJson::Array)
				{
					return Value;
				}
				TArray<TSharedPtr<FJsonValue>> Out;
				for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
				{
					Out.Add(NormalizeTyped(Item, Element, bFresh));
				}
				return MakeShared<FJsonValueArray>(Out);
			}
			if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
			{
				if (Value->Type != EJson::Object)
				{
					return Value;
				}
				const TMap<FString, FString>* KeyValues = ValuesOf(MapProperty->KeyProp);
				const bool bEnumKey = KeyValues != nullptr;
				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				for (const auto& Pair : Value->AsObject()->Values)
				{
					FString Key = CortexEngineCompat::JsonKeyToString(Pair.Key);
					if (bEnumKey)
					{
						Key = StripEnumNamespace(Key);
						if (!bFresh)
						{
							if (const FString* Mapped = KeyValues->Find(Key))
							{
								Key = *Mapped;
							}
						}
					}
					Out->SetField(Key, NormalizeTyped(Pair.Value, MapProperty->ValueProp, bFresh));
				}
				return MakeShared<FJsonValueObject>(Out);
			}
			return Value;
		}

		TSharedRef<FJsonObject> NormalizeStruct(const TSharedPtr<FJsonObject>& Object, const UStruct* Struct, const bool bFresh) const
		{
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			for (const auto& Pair : Object->Values)
			{
				const FString Key = CortexEngineCompat::JsonKeyToString(Pair.Key);
				Out->SetField(Key, NormalizeTyped(Pair.Value, Struct->FindPropertyByName(FName(*Key)), bFresh));
			}
			return Out;
		}

		/** An exported DataTable ({rows:[{row_name,row_data}]}) or DataAssets ({data_assets:[{path,properties}]}) file; the type context is the row struct / asset class. */
		TSharedRef<FJsonObject> NormalizeExport(const TSharedPtr<FJsonObject>& Root, const FString& Kind, const UStruct* TypeContext, const bool bFresh) const
		{
			const bool bTable = Kind == TEXT("datatable");
			const TCHAR* const ListField = bTable ? TEXT("rows") : TEXT("data_assets");
			const TCHAR* const BodyField = bTable ? TEXT("row_data") : TEXT("properties");
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			for (const auto& Pair : Root->Values)
			{
				const FString Key = CortexEngineCompat::JsonKeyToString(Pair.Key);
				if (Key != ListField || !Pair.Value.IsValid() || Pair.Value->Type != EJson::Array)
				{
					Out->SetField(Key, Pair.Value);
					continue;
				}
				TArray<TSharedPtr<FJsonValue>> Records;
				for (const TSharedPtr<FJsonValue>& Record : Pair.Value->AsArray())
				{
					const TSharedPtr<FJsonObject> RecordObject = Record.IsValid() ? Record->AsObject() : nullptr;
					if (!RecordObject.IsValid() || !RecordObject->HasTypedField<EJson::Object>(BodyField) || TypeContext == nullptr)
					{
						Records.Add(Record);
						continue;
					}
					TSharedRef<FJsonObject> Copy = MakeShared<FJsonObject>();
					for (const auto& Field : RecordObject->Values)
					{
						const FString FieldName = CortexEngineCompat::JsonKeyToString(Field.Key);
						if (FieldName == BodyField)
						{
							Copy->SetObjectField(FieldName, NormalizeStruct(Field.Value->AsObject(), TypeContext, bFresh));
						}
						else
						{
							Copy->SetField(FieldName, Field.Value);
						}
					}
					Records.Add(MakeShared<FJsonValueObject>(Copy));
				}
				Out->SetArrayField(Key, Records);
			}
			return Out;
		}
	};

	/** Result of repairing the K2 struct-node pins of one Blueprint. */
	struct FPinRepair
	{
		int32 Repaired = 0;
		/** "<node guid>: <pin>" for every stale pin that could not be mapped (fail-closed). */
		TArray<FString> Unmapped;
	};

	const FCortexDataTypeReplacementMember* MemberForPinName(const TArray<FCortexDataTypeReplacementMember>& Members, const FString& PinName)
	{
		const FString Squashed = PinName.Replace(TEXT(" "), TEXT(""));
		for (const FCortexDataTypeReplacementMember& Member : Members)
		{
			if (PinName == Member.UserName || PinName == Member.FriendlyName || PinName == Member.DisplayName
				|| Squashed == Member.FriendlyName.Replace(TEXT(" "), TEXT("")) || Squashed == Member.DisplayName.Replace(TEXT(" "), TEXT("")))
			{
				return &Member;
			}
		}
		return nullptr;
	}

	/**
	 * The engine redirects the struct TYPE of a K2 struct node but its member pins keep the user-defined struct's names: a Set Members node
	 * has no renamed-pin matching at all, so a pin with a non-default value or a link becomes an orphan on the next reconstruct and the
	 * Blueprint fails to compile. Before compiling, every pin of a node of the redirected native struct that no longer resolves to a
	 * property but matches a plan member (GUID name, friendly or display name) is renamed to the member's native property (a Set Members
	 * node also shows that property's pin); links, defaults, direction and type are kept by the reconstruct that follows. Pins that already
	 * resolve are never touched. An orphan that stays orphaned with a link or a non-default value is reported, not dropped.
	 */
	FPinRepair RepairStructNodePins(UBlueprint* Blueprint, const UScriptStruct* NativeStruct, const TArray<FCortexDataTypeReplacementMember>& Members)
	{
		FPinRepair Result;
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			const TArray<TObjectPtr<UEdGraphNode>> Nodes = Graph->Nodes;
			for (UEdGraphNode* Node : Nodes)
			{
				UK2Node_StructOperation* Operation = Cast<UK2Node_StructOperation>(Node);
				if (Operation == nullptr || Operation->StructType != NativeStruct)
				{
					continue;
				}

				bool bChanged = false;
				const TArray<UEdGraphPin*> Pins = Operation->Pins;
				for (UEdGraphPin* Pin : Pins)
				{
					if (Pin == nullptr || Pin->ParentPin != nullptr || NativeStruct->FindPropertyByName(Pin->PinName) != nullptr)
					{
						continue;
					}
					const FCortexDataTypeReplacementMember* Member = MemberForPinName(Members, Pin->PinName.ToString());
					if (Member == nullptr || NativeStruct->FindPropertyByName(FName(*Member->NativeName)) == nullptr || Pin->SubPins.Num() > 0)
					{
						continue;
					}
					const FName NativeName(*Member->NativeName);
					UEdGraphPin* Existing = Operation->FindPin(NativeName, Pin->Direction);
					Operation->Modify();
					if (Existing != nullptr && Existing != Pin)
					{
						// The reconstruct already made the native pin: the stale pin's value and links move onto it.
						Existing->CopyPersistentDataFromOldPin(*Pin);
						Operation->RemovePin(Pin);
					}
					else
					{
						Pin->Modify();
						Pin->PinName = NativeName;
						Pin->bOrphanedPin = false;
					}
					if (UK2Node_StructMemberSet* MemberSet = Cast<UK2Node_StructMemberSet>(Operation))
					{
						for (FOptionalPinFromProperty& Entry : MemberSet->ShowPinForProperties)
						{
							if (Entry.PropertyName == NativeName)
							{
								Entry.bShowPin = true;
							}
						}
					}
					++Result.Repaired;
					bChanged = true;
				}
				if (bChanged)
				{
					Operation->ReconstructNode();
				}
				for (const UEdGraphPin* Pin : Operation->Pins)
				{
					if (Pin != nullptr && Pin->bOrphanedPin && (Pin->LinkedTo.Num() > 0 || !Pin->DoesDefaultValueMatchAutogenerated()))
					{
						Result.Unmapped.Add(FString::Printf(TEXT("%s: %s"), *Operation->NodeGuid.ToString(), *Pin->PinName.ToString()));
					}
				}
			}
		}
		return Result;
	}

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
		UObject* Loaded,
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
		const UStruct* TypeContext = Baseline.Kind == TEXT("datatable")
			? static_cast<const UStruct*>(Cast<UDataTable>(Loaded) != nullptr ? Cast<UDataTable>(Loaded)->GetRowStruct() : nullptr)
			: static_cast<const UStruct*>(Loaded->GetClass());
		// Struct plans rename the GUID vocabulary first; both kinds then normalize typed values against the reloaded type context.
		const TSharedPtr<FJsonObject> Renamed = Normalizer.bStruct ? Normalizer.Normalize(MakeShared<FJsonValueObject>(BaselineJson))->AsObject() : BaselineJson;
		const TSharedRef<FJsonObject> Normalized = Normalizer.NormalizeExport(Renamed, Baseline.Kind, TypeContext, /*bFresh=*/false);

		FCortexResolvedFilePath ResolvedNormalized;
		FString ErrorCode;
		FString ErrorMessage;
		if (!FCortexSafeFileContract::ResolveWritePath(NormalizedPath, ResolvedNormalized, ErrorCode, ErrorMessage))
		{
			Entry->SetStringField(TEXT("error"), ErrorMessage);
			return;
		}
		const FCortexJsonFileWriteResult Written = FCortexSafeFileContract::WriteJsonReportAtomic(ResolvedNormalized, Normalized);
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
		if (!Normalizer.bStruct)
		{
			// Enum-keyed map keys are exported as property text that may carry the enum namespace; the fresh side is brought to the same form.
			TSharedPtr<FJsonObject> FreshJson;
			FCortexResolvedFilePath ResolvedFresh;
			if (!ReadJsonFile(FreshPath, FreshJson, ReadError) || !FCortexSafeFileContract::ResolveWritePath(FreshPath, ResolvedFresh, ErrorCode, ErrorMessage)
				|| !FCortexSafeFileContract::WriteJsonReportAtomic(ResolvedFresh, Normalizer.NormalizeExport(FreshJson, Baseline.Kind, TypeContext, /*bFresh=*/true)).bWritten)
			{
				Entry->SetStringField(TEXT("error"), TEXT("the fresh export could not be normalized"));
				return;
			}
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

	// The report must never overwrite an input: the plan or a recorded baseline (checked before anything is loaded, compiled or saved).
	if (!OutPath.IsEmpty())
	{
		FCortexResolvedFilePath ResolvedOut;
		FString ErrorCode;
		FString ErrorMessage;
		if (!FCortexSafeFileContract::ResolveWritePath(OutPath, ResolvedOut, ErrorCode, ErrorMessage))
		{
			return FCortexCommandRouter::Error(ErrorCode, ErrorMessage);
		}
		TArray<FString> Inputs;
		Inputs.Add(PlanPath);
		for (const FCortexDataTypeReplacementBaseline& Baseline : Plan.Baseline)
		{
			Inputs.Add(Baseline.File);
		}
		for (const FString& Input : Inputs)
		{
			FCortexResolvedFilePath ResolvedInput;
			if (FCortexSafeFileContract::ResolveReadPath(Input, ResolvedInput, ErrorCode, ErrorMessage)
				&& FCortexSafeFileContract::AreSameCanonicalFile(ResolvedInput.AbsolutePath, ResolvedOut.AbsolutePath))
			{
				return FCortexCommandRouter::Error(CortexErrorCodes::InvalidFilePath, FString::Printf(TEXT("out_path must not be the plan or a baseline file it would overwrite: %s"), *Input));
			}
		}
	}

	const bool bStruct = Plan.Kind == TEXT("struct");
	UScriptStruct* NativeStruct = bStruct ? FCortexDataTypeReplacementOps::FindNativeStruct(Plan.NativeType) : nullptr;
	UEnum* NativeEnum = bStruct ? nullptr : FCortexDataTypeReplacementOps::FindNativeEnum(Plan.NativeType);
	if (NativeStruct == nullptr && NativeEnum == nullptr)
	{
		return VerifyInvalidField(FString::Printf(TEXT("The native type %s of the plan does not exist in this editor"), *Plan.NativeType));
	}

	// 2. The redirects must be active in this editor; they are never added here.
	TArray<FCortexDataRedirectProbeFailure> Failures;
	if (bStruct)
	{
		FCortexDataTypeReplacementOps::ProbeStructContract(Plan.UserType, Plan.NativeType, NativeStruct, Plan.Members, Failures);
	}
	else
	{
		FCortexDataTypeReplacementOps::ProbeEnumContract(Plan.UserType, Plan.NativeType, NativeEnum, Plan.Values, Failures);
	}
	// The inner user-defined types the plan was accepted under: their complete replacement has to be active here too.
	TArray<FString> MissingPrerequisites;
	for (const FCortexDataTypeReplacementPrerequisite& Prerequisite : Plan.Prerequisites)
	{
		TArray<FCortexDataRedirectProbeFailure> Inner;
		if (Prerequisite.Kind == TEXT("struct"))
		{
			const UScriptStruct* InnerStruct = FCortexDataTypeReplacementOps::FindNativeStruct(Prerequisite.NativeType);
			if (InnerStruct == nullptr)
			{
				MissingPrerequisites.Add(FString::Printf(TEXT("%s: the native struct %s does not exist in this editor"), *Prerequisite.UserType, *Prerequisite.NativeType));
				continue;
			}
			FCortexDataTypeReplacementOps::ProbeStructContract(Prerequisite.UserType, Prerequisite.NativeType, InnerStruct, Prerequisite.Members, Inner);
		}
		else
		{
			const UEnum* InnerEnum = FCortexDataTypeReplacementOps::FindNativeEnum(Prerequisite.NativeType);
			if (InnerEnum == nullptr)
			{
				MissingPrerequisites.Add(FString::Printf(TEXT("%s: the native enum %s does not exist in this editor"), *Prerequisite.UserType, *Prerequisite.NativeType));
				continue;
			}
			FCortexDataTypeReplacementOps::ProbeEnumContract(Prerequisite.UserType, Prerequisite.NativeType, InnerEnum, Prerequisite.Values, Inner);
		}
		for (const FCortexDataRedirectProbeFailure& Failure : Inner)
		{
			MissingPrerequisites.Add(Failure.Reason);
		}
	}
	if (Failures.Num() > 0 || MissingPrerequisites.Num() > 0)
	{
		TArray<FString> Missing;
		TArray<TSharedPtr<FJsonValue>> Reasons;
		for (const FCortexDataRedirectProbeFailure& Failure : Failures)
		{
			if (Plan.RedirectLines.IsValidIndex(Failure.LineIndex))
			{
				Missing.AddUnique(Plan.RedirectLines[Failure.LineIndex]);
			}
			Reasons.Add(MakeShared<FJsonValueString>(Failure.Reason));
		}
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("missing_redirects"), VerifyStringValues(Missing));
		for (const FString& Reason : MissingPrerequisites)
		{
			Reasons.Add(MakeShared<FJsonValueString>(Reason));
		}
		Details->SetArrayField(TEXT("reasons"), Reasons);
		Details->SetArrayField(TEXT("missing_prerequisites"), VerifyStringValues(MissingPrerequisites));
		Details->SetStringField(TEXT("instruction"), TEXT("Add the missing lines to the [CoreRedirects] section of Config/DefaultEngine.ini and restart the editor (editor_restart): redirects are read once at start and apply while a package loads, so nothing already loaded converts. Cortex never adds redirects at runtime."));
		return FCortexCommandRouter::Error(
			CortexErrorCodes::RedirectsNotActive,
			FString::Printf(TEXT("%d redirect line(s) and %d inner-type prerequisite redirect(s) of the plan are not active in this editor; nothing was loaded or saved"), Missing.Num(), MissingPrerequisites.Num()),
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
	// The mutation guard applies to every mode: an in-memory verification still recompiles Blueprints, which regenerates a quarantined
	// asset's class and defaults.
	{
		TArray<FString> Blocked;
		for (const FCortexDataTypeReplacementAsset& Asset : Verifiable)
		{
			FString Reason;
			if (FCortexAssetMutationGuard::IsPathBlocked(Asset.Path, Reason))
			{
				Blocked.Add(FString::Printf(TEXT("%s: %s"), *Asset.Path, *Reason));
			}
		}
		if (Blocked.Num() > 0)
		{
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetArrayField(TEXT("blocked_assets"), VerifyStringValues(Blocked));
			return FCortexCommandRouter::Error(
				CortexErrorCodes::InvalidOperation,
				TEXT("These assets are blocked after an unverified recovery: verification would recompile or save them, so nothing was loaded or touched"),
				Details);
		}
	}
	if (bSave)
	{
		TArray<FString> Dirty;
		for (const FCortexDataTypeReplacementAsset& Asset : Verifiable)
		{
			const UPackage* Loaded = FindPackage(nullptr, *FPackageName::ObjectPathToPackageName(Asset.Path));
			if (Loaded != nullptr && Loaded->IsDirty())
			{
				Dirty.Add(Asset.Path);
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
	}

	// 4. Load, compile, diff, resave.
	FNormalizer Normalizer;
	Normalizer.MemberMap = Plan.MemberMap();
	Normalizer.bStruct = bStruct;
	if (NativeEnum != nullptr)
	{
		Normalizer.EnumValueMaps.Add(NativeEnum, Plan.ValueMap());
	}
	Normalizer.UserPath = Plan.UserType;
	Normalizer.NativePath = Plan.NativeType;
	Normalizer.StructNames.Add(FPackageName::ObjectPathToObjectName(Plan.UserType), bStruct ? NativeStruct->GetName() : NativeEnum->GetName());
	for (const FCortexDataTypeReplacementPrerequisite& Prerequisite : Plan.Prerequisites)
	{
		// The inner types' GUID member names and struct names are baseline vocabulary too.
		for (const FCortexDataTypeReplacementMember& Member : Prerequisite.Members)
		{
			Normalizer.MemberMap.Add(Member.UserName, Member.NativeName);
		}
		Normalizer.StructNames.Add(FPackageName::ObjectPathToObjectName(Prerequisite.UserType), FPackageName::ObjectPathToObjectName(Prerequisite.NativeType));
		if (Prerequisite.Kind == TEXT("enum"))
		{
			// An inner enum recorded by a struct plan: its typed fields are mapped like the replaced enum of an enum plan.
			if (const UEnum* InnerEnum = FCortexDataTypeReplacementOps::FindNativeEnum(Prerequisite.NativeType))
			{
				TMap<FString, FString>& Values = Normalizer.EnumValueMaps.FindOrAdd(InnerEnum);
				for (const TPair<FString, FString>& Value : Prerequisite.Values)
				{
					Values.Add(Value.Key, Value.Value);
				}
			}
		}
	}
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
				int32 PinsRepaired = 0;
				if (bStruct && NativeStruct != nullptr)
				{
					const FPinRepair Repair = RepairStructNodePins(Blueprint, NativeStruct, Plan.Members);
					PinsRepaired = Repair.Repaired;
					if (Repair.Unmapped.Num() > 0)
					{
						Entry->SetArrayField(TEXT("unmapped_pins"), VerifyStringValues(Repair.Unmapped));
						Entry->SetStringField(TEXT("error"), TEXT("stale struct-node pins could not be mapped to a native member; the asset was not saved"));
						bOk = false;
					}
				}
				Entry->SetNumberField(TEXT("pins_repaired"), PinsRepaired);
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
				if (bOk)
				{
					// Compile only when the pins are sound; a compile success is required before the explicit save.
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
				}
			}
			else if (const UDataTable* Table = Cast<UDataTable>(Loaded))
			{
				if (bStruct && Asset.bRowStruct && Table->GetRowStruct() != NativeStruct)
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
				DiffBaseline(*Baseline, Normalizer, Loaded, WorkDir, Entry, bEqual);
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
