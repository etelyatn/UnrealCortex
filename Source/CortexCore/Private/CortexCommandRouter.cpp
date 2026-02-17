
#include "CortexCommandRouter.h"
#include "CortexBatchScope.h"
#include "CortexCoreModule.h"
#include "ICortexDomainHandler.h"
#include "Misc/EngineVersion.h"
#include "Misc/App.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Materials/Material.h"
#include "MaterialGraph/MaterialGraph.h"
#include "ScopedTransaction.h"

int32 FCortexCommandRouter::BatchDepth = 0;

// FCortexBatchScope implementation
TSet<TWeakObjectPtr<UMaterial>> FCortexBatchScope::DirtyMaterials;
TMap<FString, FCortexBatchScope::FBatchCleanupCallback> FCortexBatchScope::CleanupActions;

FCortexBatchScope::FCortexBatchScope()
{
	FCortexCommandRouter::BatchDepth++;
}

FCortexBatchScope::~FCortexBatchScope()
{
	FCortexCommandRouter::BatchDepth--;

	if (FCortexCommandRouter::BatchDepth == 0)
	{
		// Invoke generic cleanup actions (MoveTemp for re-entrancy safety:
		// callbacks like NotifyGraphChanged may trigger delegates that call AddCleanupAction)
		TMap<FString, FBatchCleanupCallback> PendingActions = MoveTemp(CleanupActions);
		CleanupActions.Empty();
		for (auto& Pair : PendingActions)
		{
			Pair.Value();
		}

		// Flush deferred PostEditChange for all dirty materials
		for (const TWeakObjectPtr<UMaterial>& WeakMat : DirtyMaterials)
		{
			if (UMaterial* Material = WeakMat.Get())
			{
				Material->PostEditChange();
				if (UMaterialGraph* MaterialGraph = Material->MaterialGraph)
				{
					MaterialGraph->RebuildGraph();
				}
			}
		}
		DirtyMaterials.Empty();
	}
}

void FCortexBatchScope::MarkMaterialDirty(UMaterial* Material)
{
	if (Material != nullptr)
	{
		DirtyMaterials.Add(Material);
	}
}

void FCortexBatchScope::AddCleanupAction(const FString& Key, FBatchCleanupCallback Callback)
{
	if (!FCortexCommandRouter::IsInBatch())
	{
		UE_LOG(LogCortex, Warning, TEXT("AddCleanupAction called outside batch, executing immediately: %s"), *Key);
		Callback();
		return;
	}
	if (!CleanupActions.Contains(Key))
	{
		CleanupActions.Add(Key, MoveTemp(Callback));
	}
}

FCortexCommandResult FCortexCommandRouter::Execute(const FString& Command, const TSharedPtr<FJsonObject>& Params)
{
	// Built-in commands (no namespace)
	if (Command == TEXT("ping"))
	{
		return HandlePing(Params);
	}
	if (Command == TEXT("get_status"))
	{
		return HandleGetStatus(Params);
	}
	if (Command == TEXT("get_capabilities"))
	{
		return HandleGetCapabilities(Params);
	}
	if (Command == TEXT("batch"))
	{
		return HandleBatch(Params);
	}

	// Namespace routing: "data.list_datatables" -> domain "data", command "list_datatables"
	FString Namespace;
	FString SubCommand;

	if (Command.Split(TEXT("."), &Namespace, &SubCommand))
	{
		for (const FCortexRegisteredDomain& Domain : RegisteredDomains)
		{
			if (Domain.Namespace == Namespace)
			{
				return Domain.Handler->Execute(SubCommand, Params);
			}
		}

		return Error(CortexErrorCodes::UnknownCommand,
			FString::Printf(TEXT("Unknown domain: %s"), *Namespace));
	}

	return Error(CortexErrorCodes::UnknownCommand, FString::Printf(TEXT("Unknown command: %s"), *Command));
}

FString FCortexCommandRouter::ResultToJson(const FCortexCommandResult& Result, double TimingMs)
{
	TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
	ResponseJson->SetBoolField(TEXT("success"), Result.bSuccess);

	if (Result.bSuccess)
	{
		if (Result.Data.IsValid())
		{
			ResponseJson->SetObjectField(TEXT("data"), Result.Data);
		}

		if (Result.Warnings.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> WarningsArray;
			for (const FString& Warning : Result.Warnings)
			{
				WarningsArray.Add(MakeShared<FJsonValueString>(Warning));
			}
			ResponseJson->SetArrayField(TEXT("warnings"), WarningsArray);
		}
	}
	else
	{
		TSharedRef<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
		ErrorObj->SetStringField(TEXT("code"), Result.ErrorCode);
		ErrorObj->SetStringField(TEXT("message"), Result.ErrorMessage);

		if (Result.ErrorDetails.IsValid())
		{
			ErrorObj->SetObjectField(TEXT("details"), Result.ErrorDetails);
		}

		ResponseJson->SetObjectField(TEXT("error"), ErrorObj);
	}

	ResponseJson->SetNumberField(TEXT("timing_ms"), TimingMs);

	FString OutputString;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
	FJsonSerializer::Serialize(ResponseJson, Writer);

	return OutputString;
}

FCortexCommandResult FCortexCommandRouter::Success(TSharedPtr<FJsonObject> Data)
{
	FCortexCommandResult Result;
	Result.bSuccess = true;
	Result.Data = MoveTemp(Data);
	return Result;
}

FCortexCommandResult FCortexCommandRouter::Error(const FString& Code, const FString& Message, TSharedPtr<FJsonObject> Details)
{
	FCortexCommandResult Result;
	Result.bSuccess = false;
	Result.ErrorCode = Code;
	Result.ErrorMessage = Message;
	Result.ErrorDetails = MoveTemp(Details);
	return Result;
}

void FCortexCommandRouter::RegisterDomain(
	const FString& Namespace,
	const FString& DisplayName,
	const FString& Version,
	TSharedPtr<ICortexDomainHandler> Handler)
{
	RegisteredDomains.Add({ Namespace, DisplayName, Version, Handler });
	UE_LOG(LogCortex, Log, TEXT("Registered domain: %s (%s v%s)"),
		*Namespace, *DisplayName, *Version);
}

const TArray<FCortexRegisteredDomain>& FCortexCommandRouter::GetRegisteredDomains() const
{
	return RegisteredDomains;
}

FCortexCommandResult FCortexCommandRouter::HandlePing(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("message"), TEXT("pong"));
	return Success(Data);
}

bool FCortexCommandRouter::IsInBatch()
{
	return BatchDepth > 0;
}

TSharedPtr<FJsonObject> FCortexCommandRouter::DeepCopyJsonObject(const TSharedPtr<FJsonObject>& Source)
{
	if (!Source.IsValid())
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
	for (const auto& Pair : Source->Values)
	{
		Copy->SetField(Pair.Key, DeepCopyJsonValue(Pair.Value));
	}
	return Copy;
}

TSharedPtr<FJsonValue> FCortexCommandRouter::DeepCopyJsonValue(const TSharedPtr<FJsonValue>& Source)
{
	if (!Source.IsValid())
	{
		return MakeShared<FJsonValueNull>();
	}

	switch (Source->Type)
	{
	case EJson::String:
	{
		FString Str;
		Source->TryGetString(Str);
		return MakeShared<FJsonValueString>(Str);
	}
	case EJson::Number:
	{
		double Num;
		Source->TryGetNumber(Num);
		return MakeShared<FJsonValueNumber>(Num);
	}
	case EJson::Boolean:
	{
		bool Bool;
		Source->TryGetBool(Bool);
		return MakeShared<FJsonValueBoolean>(Bool);
	}
	case EJson::Array:
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (Source->TryGetArray(Arr))
		{
			TArray<TSharedPtr<FJsonValue>> CopyArr;
			CopyArr.Reserve(Arr->Num());
			for (const TSharedPtr<FJsonValue>& Elem : *Arr)
			{
				CopyArr.Add(DeepCopyJsonValue(Elem));
			}
			return MakeShared<FJsonValueArray>(CopyArr);
		}
		return MakeShared<FJsonValueNull>();
	}
	case EJson::Object:
	{
		const TSharedPtr<FJsonObject>* Obj;
		if (Source->TryGetObject(Obj))
		{
			return MakeShared<FJsonValueObject>(DeepCopyJsonObject(*Obj));
		}
		return MakeShared<FJsonValueNull>();
	}
	case EJson::Null:
	default:
		return MakeShared<FJsonValueNull>();
	}
}

bool FCortexCommandRouter::ResolveObjectRefs(
	TSharedPtr<FJsonObject>& Params,
	const TArray<TSharedPtr<FJsonValue>>& StepResults,
	int32 CurrentStepIndex,
	FString& OutError)
{
	if (!Params.IsValid())
	{
		return true;
	}

	// Iterate over all fields and resolve refs
	TArray<FString> Keys;
	Params->Values.GetKeys(Keys);

	for (const FString& Key : Keys)
	{
		TSharedPtr<FJsonValue> Value = Params->Values[Key];
		if (!ResolveValueRefs(Value, Key, StepResults, CurrentStepIndex, OutError, 0))
		{
			return false;
		}
		Params->SetField(Key, Value);
	}

	return true;
}

bool FCortexCommandRouter::ResolveValueRefs(
	TSharedPtr<FJsonValue>& Value,
	const FString& Key,
	const TArray<TSharedPtr<FJsonValue>>& StepResults,
	int32 CurrentStepIndex,
	FString& OutError,
	int32 Depth)
{
	if (!Value.IsValid())
	{
		return true;
	}

	// Depth check
	if (Depth > 10)
	{
		OutError = TEXT("Max recursion depth (10) exceeded during $ref resolution");
		return false;
	}

	// Check for string values that might be refs
	if (Value->Type == EJson::String)
	{
		FString StrValue;
		Value->TryGetString(StrValue);

		// Check for escape: $$steps[ -> $steps[
		if (StrValue.StartsWith(TEXT("$$steps[")))
		{
			FString UnescapedValue = StrValue.Mid(1); // Remove first $
			Value = MakeShared<FJsonValueString>(UnescapedValue);
			return true;
		}

		// Check for $ref pattern
		if (StrValue.StartsWith(TEXT("$steps[")))
		{
			TSharedPtr<FJsonValue> ResolvedValue;
			if (!ParseAndResolveRef(StrValue, StepResults, CurrentStepIndex, ResolvedValue, OutError))
			{
				return false;
			}
			Value = ResolvedValue;
			return true;
		}

		// Log if string contains $steps[ mid-string (but don't resolve - value passes through unchanged)
		if (StrValue.Contains(TEXT("$steps[")))
		{
			UE_LOG(LogCortex, Log, TEXT("String field '%s' contains '$steps[' mid-string - this is not resolved. Value: %s"), *Key, *StrValue);
		}

		return true;
	}

	// Recursively resolve refs in arrays
	if (Value->Type == EJson::Array)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (Value->TryGetArray(Arr))
		{
			TArray<TSharedPtr<FJsonValue>> NewArr;
			NewArr.Reserve(Arr->Num());
			for (TSharedPtr<FJsonValue> Elem : *Arr)
			{
				if (!ResolveValueRefs(Elem, Key, StepResults, CurrentStepIndex, OutError, Depth + 1))
				{
					return false;
				}
				NewArr.Add(Elem);
			}
			Value = MakeShared<FJsonValueArray>(NewArr);
		}
		return true;
	}

	// Recursively resolve refs in objects
	if (Value->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject>* Obj;
		if (Value->TryGetObject(Obj))
		{
			TSharedPtr<FJsonObject> NewObj = MakeShared<FJsonObject>(**Obj);
			if (!ResolveObjectRefs(NewObj, StepResults, CurrentStepIndex, OutError))
			{
				return false;
			}
			Value = MakeShared<FJsonValueObject>(NewObj);
		}
		return true;
	}

	// Other types (number, bool, null) - no resolution needed
	return true;
}

bool FCortexCommandRouter::ParseAndResolveRef(
	const FString& RefString,
	const TArray<TSharedPtr<FJsonValue>>& StepResults,
	int32 CurrentStepIndex,
	TSharedPtr<FJsonValue>& OutValue,
	FString& OutError)
{
	// Parse: $steps[N].data.field.subfield
	// Expected format: $steps[INDEX].PATH

	// Find the closing bracket
	int32 BracketStart = RefString.Find(TEXT("["));
	int32 BracketEnd = RefString.Find(TEXT("]"));

	if (BracketStart == INDEX_NONE || BracketEnd == INDEX_NONE || BracketEnd <= BracketStart)
	{
		OutError = FString::Printf(TEXT("Malformed $ref: missing or invalid brackets in '%s'"), *RefString);
		return false;
	}

	// Extract index string
	FString IndexStr = RefString.Mid(BracketStart + 1, BracketEnd - BracketStart - 1);
	if (IndexStr.IsEmpty())
	{
		OutError = FString::Printf(TEXT("Malformed $ref: empty index in '%s'"), *RefString);
		return false;
	}

	// Parse index
	int32 StepIndex = FCString::Atoi(*IndexStr);

	// Validate index range
	if (StepIndex < 0)
	{
		OutError = FString::Printf(TEXT("Invalid $ref: negative index %d in '%s'"), StepIndex, *RefString);
		return false;
	}

	if (StepIndex >= CurrentStepIndex)
	{
		OutError = FString::Printf(TEXT("Invalid $ref: reference to future/self step %d from step %d in '%s'"), StepIndex, CurrentStepIndex, *RefString);
		return false;
	}

	if (StepIndex >= StepResults.Num())
	{
		OutError = FString::Printf(TEXT("Invalid $ref: step %d not found (only %d steps executed) in '%s'"), StepIndex, StepResults.Num(), *RefString);
		return false;
	}

	// Extract path after ]
	FString Path = RefString.Mid(BracketEnd + 1);
	if (!Path.StartsWith(TEXT(".")))
	{
		OutError = FString::Printf(TEXT("Malformed $ref: expected '.' after index in '%s'"), *RefString);
		return false;
	}

	Path = Path.Mid(1); // Remove leading '.'
	if (Path.IsEmpty())
	{
		OutError = FString::Printf(TEXT("Malformed $ref: empty path after index in '%s'"), *RefString);
		return false;
	}

	// Parse path to check for minimum depth
	TArray<FString> PathParts;
	Path.ParseIntoArray(PathParts, TEXT("."), true);

	// Require at least 2 parts: data.field (can't just reference $steps[0].data)
	if (PathParts.Num() < 2)
	{
		OutError = FString::Printf(TEXT("Malformed $ref: path must include field after 'data' in '%s'"), *RefString);
		return false;
	}

	// Get the step result object
	const TSharedPtr<FJsonValue>& StepResultValue = StepResults[StepIndex];
	const TSharedPtr<FJsonObject>* StepResultObj;
	if (!StepResultValue.IsValid() || !StepResultValue->TryGetObject(StepResultObj) || StepResultObj == nullptr)
	{
		OutError = FString::Printf(TEXT("Invalid $ref: step %d result is not a valid object"), StepIndex);
		return false;
	}

	// Check if step succeeded
	bool bStepSuccess = false;
	(*StepResultObj)->TryGetBoolField(TEXT("success"), bStepSuccess);
	if (!bStepSuccess)
	{
		OutError = FString::Printf(TEXT("Invalid $ref: step %d failed, cannot reference its data"), StepIndex);
		return false;
	}

	// Navigate the path (PathParts already parsed above)
	TSharedPtr<FJsonValue> CurrentValue = StepResultValue;

	for (const FString& Part : PathParts)
	{
		const TSharedPtr<FJsonObject>* CurrentObj;
		if (!CurrentValue.IsValid() || !CurrentValue->TryGetObject(CurrentObj) || CurrentObj == nullptr)
		{
			OutError = FString::Printf(TEXT("Invalid $ref: path '%s' not found in step %d result (intermediate object not found)"), *Path, StepIndex);
			return false;
		}

		TSharedPtr<FJsonValue> FieldValue = (*CurrentObj)->TryGetField(Part);
		if (!FieldValue.IsValid())
		{
			OutError = FString::Printf(TEXT("Invalid $ref: field '%s' not found in step %d result (path: '%s')"), *Part, StepIndex, *Path);
			return false;
		}

		CurrentValue = FieldValue;
	}

	OutValue = CurrentValue;
	return true;
}

FCortexCommandResult FCortexCommandRouter::HandleBatch(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* CommandsArray = nullptr;
	if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("commands"), CommandsArray) || CommandsArray == nullptr)
	{
		return Error(CortexErrorCodes::InvalidField, TEXT("Missing required param: commands (array)"));
	}

	if (CommandsArray->Num() > MaxBatchSize)
	{
		return Error(
			CortexErrorCodes::BatchLimitExceeded,
			FString::Printf(TEXT("Batch size %d exceeds maximum of %d"), CommandsArray->Num(), MaxBatchSize)
		);
	}

	// Read stop_on_error parameter (default false)
	bool bStopOnError = false;
	Params->TryGetBoolField(TEXT("stop_on_error"), bStopOnError);

	const double BatchStartTime = FPlatformTime::Seconds();

	// Single transaction for entire batch
	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Batch (%d commands)"), CommandsArray->Num())
	));

	// RAII: sets IsInBatch()=true, defers PostEditChange
	FCortexBatchScope BatchScope;

	TArray<TSharedPtr<FJsonValue>> ResultsArray;

	for (int32 Index = 0; Index < CommandsArray->Num(); ++Index)
	{
		TSharedRef<FJsonObject> EntryResult = MakeShared<FJsonObject>();
		EntryResult->SetNumberField(TEXT("index"), Index);

		const TSharedPtr<FJsonValue>& CmdVal = (*CommandsArray)[Index];
		const TSharedPtr<FJsonObject>* CmdObj = nullptr;

		if (!CmdVal.IsValid() || !CmdVal->TryGetObject(CmdObj) || CmdObj == nullptr)
		{
			EntryResult->SetStringField(TEXT("command"), TEXT(""));
			EntryResult->SetBoolField(TEXT("success"), false);
			EntryResult->SetStringField(TEXT("error_code"), CortexErrorCodes::InvalidField);
			EntryResult->SetStringField(TEXT("error_message"), TEXT("Invalid command entry (not an object)"));
			EntryResult->SetNumberField(TEXT("timing_ms"), 0.0);
			ResultsArray.Add(MakeShared<FJsonValueObject>(EntryResult));
			if (bStopOnError)
			{
				break;
			}
			continue;
		}

		FString SubCommand;
		(*CmdObj)->TryGetStringField(TEXT("command"), SubCommand);
		EntryResult->SetStringField(TEXT("command"), SubCommand);

		// Block nested batch
		if (SubCommand == TEXT("batch"))
		{
			EntryResult->SetBoolField(TEXT("success"), false);
			EntryResult->SetStringField(TEXT("error_code"), CortexErrorCodes::BatchRecursionBlocked);
			EntryResult->SetStringField(TEXT("error_message"), TEXT("Nested batch commands are not allowed"));
			EntryResult->SetNumberField(TEXT("timing_ms"), 0.0);
			ResultsArray.Add(MakeShared<FJsonValueObject>(EntryResult));
			if (bStopOnError)
			{
				break;
			}
			continue;
		}

		TSharedPtr<FJsonObject> SubParams;
		const TSharedPtr<FJsonObject>* SubParamsPtr = nullptr;
		if ((*CmdObj)->TryGetObjectField(TEXT("params"), SubParamsPtr) && SubParamsPtr != nullptr)
		{
			SubParams = *SubParamsPtr;
		}
		else
		{
			SubParams = MakeShared<FJsonObject>();
		}

		// Deep-copy params to prevent mutation of original batch request
		TSharedPtr<FJsonObject> ParamsCopy = DeepCopyJsonObject(SubParams);

		// Resolve $ref strings in the copied params
		FString RefError;
		if (!ResolveObjectRefs(ParamsCopy, ResultsArray, Index, RefError))
		{
			// $ref resolution failed
			EntryResult->SetBoolField(TEXT("success"), false);
			EntryResult->SetStringField(TEXT("error_code"), CortexErrorCodes::BatchRefResolutionFailed);
			EntryResult->SetStringField(TEXT("error_message"), RefError);
			EntryResult->SetNumberField(TEXT("timing_ms"), 0.0);
			ResultsArray.Add(MakeShared<FJsonValueObject>(EntryResult));
			if (bStopOnError)
			{
				break;
			}
			continue;
		}

		const double CmdStartTime = FPlatformTime::Seconds();
		FCortexCommandResult SubResult = Execute(SubCommand, ParamsCopy);
		const double CmdElapsed = (FPlatformTime::Seconds() - CmdStartTime) * 1000.0;

		EntryResult->SetBoolField(TEXT("success"), SubResult.bSuccess);
		EntryResult->SetNumberField(TEXT("timing_ms"), CmdElapsed);

		if (SubResult.bSuccess)
		{
			if (SubResult.Data.IsValid())
			{
				EntryResult->SetObjectField(TEXT("data"), SubResult.Data);
			}
		}
		else
		{
			EntryResult->SetStringField(TEXT("error_code"), SubResult.ErrorCode);
			EntryResult->SetStringField(TEXT("error_message"), SubResult.ErrorMessage);
		}

		ResultsArray.Add(MakeShared<FJsonValueObject>(EntryResult));

		// Stop on error if requested and step failed
		if (bStopOnError && !SubResult.bSuccess)
		{
			break;
		}
	}

	const double BatchElapsed = (FPlatformTime::Seconds() - BatchStartTime) * 1000.0;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("results"), ResultsArray);
	Data->SetNumberField(TEXT("count"), ResultsArray.Num());
	Data->SetNumberField(TEXT("total_timing_ms"), BatchElapsed);

	return Success(Data);
}

FCortexCommandResult FCortexCommandRouter::HandleGetStatus(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("connected"), true);
	Data->SetStringField(TEXT("plugin_version"), TEXT("0.1.0"));

	// Engine version
	Data->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());

	// Project name
	Data->SetStringField(TEXT("project_name"), FApp::GetProjectName());

	// Subsystem availability
	TSharedPtr<FJsonObject> Subsystems = MakeShared<FJsonObject>();
	Subsystems->SetBoolField(TEXT("asset_registry"), IAssetRegistry::Get() != nullptr);
	Subsystems->SetBoolField(TEXT("gameplay_tags"), true);
	Subsystems->SetBoolField(TEXT("localization"), true);
	Data->SetObjectField(TEXT("subsystems"), Subsystems);

	return Success(Data);
}

FCortexCommandResult FCortexCommandRouter::HandleGetCapabilities(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("plugin_version"), TEXT("0.1.0"));

	TSharedPtr<FJsonObject> Domains = MakeShared<FJsonObject>();

	for (const FCortexRegisteredDomain& Domain : RegisteredDomains)
	{
		TSharedPtr<FJsonObject> DomainObj = MakeShared<FJsonObject>();
		DomainObj->SetStringField(TEXT("name"), Domain.DisplayName);
		DomainObj->SetStringField(TEXT("version"), Domain.Version);

		TArray<TSharedPtr<FJsonValue>> CommandArray;
		for (const FCortexCommandInfo& CmdInfo : Domain.Handler->GetSupportedCommands())
		{
			TSharedPtr<FJsonObject> CmdObj = MakeShared<FJsonObject>();
			CmdObj->SetStringField(TEXT("name"), CmdInfo.Name);
			CmdObj->SetStringField(TEXT("description"), CmdInfo.Description);
			CommandArray.Add(MakeShared<FJsonValueObject>(CmdObj));
		}
		DomainObj->SetArrayField(TEXT("commands"), CommandArray);

		Domains->SetObjectField(Domain.Namespace, DomainObj);
	}

	Data->SetObjectField(TEXT("domains"), Domains);
	return Success(Data);
}
