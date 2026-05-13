#include "Operations/CortexSTValidationOps.h"

#include "CortexCommandRouter.h"
#include "CortexSTTypes.h"
#include "CortexStateTreeModule.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/PackageName.h"
#include "Operations/CortexSTAssetOps.h"
#include "ScopedTransaction.h"
#include "StateTree.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"

namespace
{
struct FCortexSTLoadedAsset
{
	FString AssetPath;
	UStateTree* StateTree = nullptr;
};

struct FCortexSTValidationSummary
{
	TArray<FString> Errors;
	TArray<FString> Warnings;
};

bool LoadStateTree(
	const TSharedPtr<FJsonObject>& Params,
	FCortexSTLoadedAsset& OutAsset,
	FCortexCommandResult& OutError)
{
	FString AssetPath;
	if (!CortexST::GetRequiredString(Params, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}

	FString PackageName;
	if (!CortexST::ValidateReadablePackage(AssetPath, PackageName, OutError))
	{
		return false;
	}

	const FString ObjectPath = CortexST::NormalizeAssetPath(AssetPath);
	UStateTree* StateTree = LoadObject<UStateTree>(nullptr, *ObjectPath);
	if (StateTree == nullptr)
	{
		OutError = FCortexCommandRouter::Error(
			CortexErrorCodes::StateTreeNotFound,
			FString::Printf(TEXT("Asset is not a StateTree: %s"), *AssetPath));
		return false;
	}

	OutAsset.AssetPath = ObjectPath;
	OutAsset.StateTree = StateTree;
	return true;
}

FCortexSTValidationSummary BuildValidationSummary(UStateTree* StateTree)
{
	FCortexSTValidationSummary Summary;
	if (StateTree == nullptr)
	{
		Summary.Errors.Add(TEXT("StateTree asset is null"));
		return Summary;
	}

	UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(StateTree->EditorData);
	if (EditorData == nullptr)
	{
		Summary.Errors.Add(TEXT("StateTree has no editor data"));
		return Summary;
	}

	UStateTreeState* RootState = EditorData->SubTrees.Num() > 0 ? EditorData->SubTrees[0] : nullptr;
	if (RootState == nullptr)
	{
		Summary.Errors.Add(TEXT("StateTree has no root state"));
		return Summary;
	}

	TArray<FCortexSTStateRef> States;
	CortexST::CollectStates(RootState, States);
	if (States.Num() == 0)
	{
		Summary.Errors.Add(TEXT("StateTree has no root state"));
		return Summary;
	}

	TSet<FString> SeenIds;
	TSet<FString> DuplicateIds;
	TMap<FString, int32> PathCounts;

	for (const FCortexSTStateRef& StateRef : States)
	{
		if (StateRef.State == nullptr)
		{
			Summary.Errors.Add(TEXT("StateTree contains a null state reference"));
			continue;
		}

		if (!StateRef.State->ID.IsValid())
		{
			Summary.Errors.Add(FString::Printf(
				TEXT("State has invalid ID at path %s"),
				StateRef.Path.IsEmpty() ? TEXT("<unknown>") : *StateRef.Path));
		}

		if (SeenIds.Contains(StateRef.Id))
		{
			DuplicateIds.Add(StateRef.Id);
		}
		SeenIds.Add(StateRef.Id);

		if (StateRef.Path.IsEmpty())
		{
			Summary.Errors.Add(FString::Printf(TEXT("State %s has empty path"), *StateRef.Id));
		}
		else
		{
			int32& Count = PathCounts.FindOrAdd(StateRef.Path);
			++Count;
		}

		for (int32 TransitionIndex = 0; TransitionIndex < StateRef.State->Transitions.Num(); ++TransitionIndex)
		{
			const FStateTreeTransition& Transition = StateRef.State->Transitions[TransitionIndex];
			if (!Transition.State.ID.IsValid())
			{
				Summary.Errors.Add(FString::Printf(
					TEXT("Transition %d in state %s has invalid target ID"),
					TransitionIndex,
					*StateRef.Path));
				continue;
			}

			if (EditorData->GetStateByID(Transition.State.ID) == nullptr)
			{
				Summary.Errors.Add(FString::Printf(
					TEXT("Transition %d in state %s targets missing state %s"),
					TransitionIndex,
					*StateRef.Path,
					*Transition.State.ID.ToString(EGuidFormats::DigitsWithHyphens)));
			}
		}
	}

	for (const FString& DuplicateId : DuplicateIds)
	{
		Summary.Errors.Add(FString::Printf(TEXT("Duplicate state ID: %s"), *DuplicateId));
	}

	for (const TPair<FString, int32>& PathEntry : PathCounts)
	{
		if (PathEntry.Value > 1)
		{
			Summary.Warnings.Add(FString::Printf(TEXT("Ambiguous state path: %s"), *PathEntry.Key));
		}
	}

	return Summary;
}

TSharedPtr<FJsonObject> BuildValidationPayload(UStateTree* StateTree)
{
	const FCortexSTValidationSummary Summary = BuildValidationSummary(StateTree);
	return CortexST::MakeValidationPayload(Summary.Errors.Num() == 0, Summary.Errors, Summary.Warnings);
}

FString LexToStringSeverity(const EMessageSeverity::Type Severity)
{
	switch (Severity)
	{
	case EMessageSeverity::Error:
		return TEXT("error");
	case EMessageSeverity::PerformanceWarning:
	case EMessageSeverity::Warning:
		return TEXT("warning");
	case EMessageSeverity::Info:
	default:
		return TEXT("info");
	}
}

bool IsErrorSeverity(const EMessageSeverity::Type Severity)
{
	return Severity == EMessageSeverity::Error || static_cast<int32>(Severity) < static_cast<int32>(EMessageSeverity::Error);
}

bool IsWarningSeverity(const EMessageSeverity::Type Severity)
{
	return Severity == EMessageSeverity::PerformanceWarning || Severity == EMessageSeverity::Warning;
}

TSharedPtr<FJsonObject> BuildCompilePayload(
	const FString& AssetPath,
	UStateTree* StateTree,
	const FStateTreeCompilerLog& CompileLog,
	const bool bCompiled)
{
	TArray<TSharedRef<FTokenizedMessage>> TokenizedMessages = CompileLog.ToTokenizedMessages();
	TArray<TSharedPtr<FJsonValue>> Diagnostics;
	Diagnostics.Reserve(TokenizedMessages.Num());

	int32 ErrorCount = 0;
	int32 WarningCount = 0;
	for (const TSharedRef<FTokenizedMessage>& TokenizedMessage : TokenizedMessages)
	{
		const EMessageSeverity::Type Severity = TokenizedMessage->GetSeverity();
		if (IsErrorSeverity(Severity))
		{
			++ErrorCount;
		}
		else if (IsWarningSeverity(Severity))
		{
			++WarningCount;
		}

		TSharedPtr<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
		Diagnostic->SetStringField(TEXT("severity"), LexToStringSeverity(Severity));
		Diagnostic->SetStringField(TEXT("message"), TokenizedMessage->ToText().ToString());
		Diagnostics.Add(MakeShared<FJsonValueObject>(Diagnostic));
	}

	if (!bCompiled && ErrorCount == 0)
	{
		++ErrorCount;
		TSharedPtr<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
		Diagnostic->SetStringField(TEXT("severity"), TEXT("error"));
		Diagnostic->SetStringField(TEXT("message"), TEXT("StateTree compilation failed"));
		Diagnostics.Add(MakeShared<FJsonValueObject>(Diagnostic));
	}

	const FString Status = ErrorCount > 0 ? TEXT("error") : (WarningCount > 0 ? TEXT("warning") : TEXT("success"));

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("asset_path"), AssetPath);
	Payload->SetStringField(TEXT("status"), Status);
	Payload->SetStringField(TEXT("compile_status"), Status);
	Payload->SetNumberField(TEXT("error_count"), ErrorCount);
	Payload->SetNumberField(TEXT("warning_count"), WarningCount);
	Payload->SetArrayField(TEXT("diagnostics"), Diagnostics);
	Payload->SetObjectField(TEXT("fingerprint"), CortexST::MakeFingerprint(StateTree));
	return Payload;
}

bool SaveIfRequested(
	const TSharedPtr<FJsonObject>& Params,
	const FString& AssetPath,
	const TSharedPtr<FJsonObject>& Data,
	FCortexCommandResult& OutError)
{
	if (!CortexST::GetOptionalBool(Params, TEXT("save"), false))
	{
		return true;
	}

	const FCortexCommandResult SaveResult = FCortexSTAssetOps::SaveAsset(AssetPath);
	if (!SaveResult.bSuccess)
	{
		OutError = SaveResult;
		return false;
	}

	if (SaveResult.Data.IsValid() && SaveResult.Data->HasTypedField<EJson::Object>(TEXT("fingerprint")))
	{
		Data->SetObjectField(TEXT("fingerprint"), SaveResult.Data->GetObjectField(TEXT("fingerprint")));
	}

	return true;
}
}

FCortexCommandResult FCortexSTValidationOps::CheckStructure(const TSharedPtr<FJsonObject>& Params)
{
	FCortexSTLoadedAsset Asset;
	FCortexCommandResult Error;
	if (!LoadStateTree(Params, Asset, Error))
	{
		return Error;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Asset.AssetPath);
	Data->SetObjectField(TEXT("validation"), BuildValidationPayload(Asset.StateTree));
	Data->SetObjectField(TEXT("fingerprint"), CortexST::MakeFingerprint(Asset.StateTree));
	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexSTValidationOps::ValidateAsset(const TSharedPtr<FJsonObject>& Params)
{
	FCortexSTLoadedAsset Asset;
	FCortexCommandResult Error;
	if (!LoadStateTree(Params, Asset, Error))
	{
		return Error;
	}

	if (!CortexST::CheckExpectedFingerprint(Asset.StateTree, Params, Error))
	{
		return Error;
	}

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Validate StateTree %s"), *FPackageName::GetShortName(Asset.AssetPath))));

	Asset.StateTree->Modify();

	const FCortexCommandResult FixupResult = RunPostMutationFixups(Asset.StateTree);
	if (!FixupResult.bSuccess)
	{
		return FixupResult;
	}

	Asset.StateTree->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Asset.AssetPath);
	if (FixupResult.Data.IsValid() && FixupResult.Data->HasTypedField<EJson::Object>(TEXT("validation")))
	{
		Data->SetObjectField(TEXT("validation"), FixupResult.Data->GetObjectField(TEXT("validation")));
	}
	else
	{
		Data->SetObjectField(TEXT("validation"), BuildValidationPayload(Asset.StateTree));
	}
	Data->SetObjectField(TEXT("fingerprint"), CortexST::MakeFingerprint(Asset.StateTree));

	if (!SaveIfRequested(Params, Asset.AssetPath, Data, Error))
	{
		return Error;
	}

	UE_LOG(LogCortexStateTree, Log, TEXT("Validated StateTree: %s"), *Asset.AssetPath);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexSTValidationOps::Compile(const TSharedPtr<FJsonObject>& Params)
{
	FCortexSTLoadedAsset Asset;
	FCortexCommandResult Error;
	if (!LoadStateTree(Params, Asset, Error))
	{
		return Error;
	}

	if (!CortexST::CheckExpectedFingerprint(Asset.StateTree, Params, Error))
	{
		return Error;
	}

	const bool bWasReady = Asset.StateTree->IsReadyToRun();
	const uint32 PreviousCompiledHash = Asset.StateTree->LastCompiledEditorDataHash;

	FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Cortex: Compile StateTree %s"), *FPackageName::GetShortName(Asset.AssetPath))));

	Asset.StateTree->Modify();

	FStateTreeCompilerLog CompileLog;
	const bool bCompiled = UStateTreeEditingSubsystem::CompileStateTree(Asset.StateTree, CompileLog);

	const bool bIsReady = Asset.StateTree->IsReadyToRun();
	const uint32 CurrentCompiledHash = Asset.StateTree->LastCompiledEditorDataHash;
	const bool bCompiledDataChanged = bWasReady != bIsReady || PreviousCompiledHash != CurrentCompiledHash;
	if (bCompiledDataChanged)
	{
		Asset.StateTree->MarkPackageDirty();
	}

	TSharedPtr<FJsonObject> Data = BuildCompilePayload(Asset.AssetPath, Asset.StateTree, CompileLog, bCompiled);
	if (!bCompiled)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::CompileFailed,
			FString::Printf(TEXT("StateTree compilation failed for %s"), *Asset.AssetPath),
			Data);
	}

	if (!SaveIfRequested(Params, Asset.AssetPath, Data, Error))
	{
		return Error;
	}

	UE_LOG(LogCortexStateTree, Log, TEXT("Compiled StateTree: %s"), *Asset.AssetPath);

	return FCortexCommandRouter::Success(Data);
}

FCortexCommandResult FCortexSTValidationOps::RunPostMutationFixups(UStateTree* StateTree)
{
	if (StateTree == nullptr)
	{
		return FCortexCommandRouter::Error(
			CortexErrorCodes::InvalidOperation,
			TEXT("RunPostMutationFixups requires a valid StateTree"));
	}

	UStateTreeEditingSubsystem::ValidateStateTree(StateTree);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetObjectField(TEXT("validation"), BuildValidationPayload(StateTree));
	Data->SetObjectField(TEXT("fingerprint"), CortexST::MakeFingerprint(StateTree));
	return FCortexCommandRouter::Success(Data);
}
