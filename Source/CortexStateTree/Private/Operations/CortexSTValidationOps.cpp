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
	Data->SetObjectField(TEXT("validation"), CortexST::BuildValidationPayload(Asset.StateTree));
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
		Data->SetObjectField(TEXT("validation"), CortexST::BuildValidationPayload(Asset.StateTree));
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
	Data->SetObjectField(TEXT("validation"), CortexST::BuildValidationPayload(StateTree));
	Data->SetObjectField(TEXT("fingerprint"), CortexST::MakeFingerprint(StateTree));
	return FCortexCommandRouter::Success(Data);
}
