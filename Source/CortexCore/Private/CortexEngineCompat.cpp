#include "CortexEngineCompat.h"

#include "Engine/Engine.h"
#if WITH_EDITOR
#include "Editor.h"
#include "Editor/EditorEngine.h"
#endif

namespace
{
	FCortexCommandResult MakeCompatError(const FString& Code, const FString& Message)
	{
		FCortexCommandResult Result;
		Result.bSuccess = false;
		Result.ErrorCode = Code;
		Result.ErrorMessage = Message;
		return Result;
	}

	FCortexCommandResult MakeCompatSuccess()
	{
		FCortexCommandResult Result;
		Result.bSuccess = true;
		return Result;
	}
}

#if WITH_EDITOR && defined(UE_SCOPED_PIE_SESSION_API_VERSION) && UE_SCOPED_PIE_SESSION_API_VERSION == 1

/**
 * Complete type of the opaque authority. Defined only here so no other translation unit can
 * construct or serialize it; the engine-issued handle inside is the actual authority.
 */
struct FCortexScopedPIEAuthority
{
	FScopedPIEHandle Handle;
};

namespace
{
	UEditorEngine* ResolveScopedEditor()
	{
		UEditorEngine* EditorEngine = GEditor;
		if (!EditorEngine || EditorEngine->GetScopedPIESessionCapabilityVersion() != UE_SCOPED_PIE_SESSION_API_VERSION)
		{
			return nullptr;
		}
		return EditorEngine;
	}

	ECortexScopedPIEPhase ToCortexPhase(EScopedPIEPhase Phase)
	{
		switch (Phase)
		{
		case EScopedPIEPhase::Worldless:	return ECortexScopedPIEPhase::Worldless;
		case EScopedPIEPhase::Live:			return ECortexScopedPIEPhase::Live;
		case EScopedPIEPhase::Quiescent:	return ECortexScopedPIEPhase::Quiescent;
		case EScopedPIEPhase::Ending:		return ECortexScopedPIEPhase::Ending;
		case EScopedPIEPhase::Ended:		return ECortexScopedPIEPhase::Ended;
		case EScopedPIEPhase::Cancelled:	return ECortexScopedPIEPhase::Cancelled;
		case EScopedPIEPhase::Pending:
		default:							return ECortexScopedPIEPhase::Pending;
		}
	}

	void CopyScopedSnapshot(const FScopedPIESnapshot& In, FCortexScopedPIESnapshot& Out)
	{
		Out.RequestSerial = In.RequestSerial;
		Out.ContextIncarnation = In.ContextIncarnation;
		Out.TopologyRevision = In.TopologyRevision;
		Out.ContextHandle = In.ContextHandle;
		Out.Phase = ToCortexPhase(In.Phase);
		Out.World = In.World;
		Out.GameInstance = In.GameInstance;
		Out.Settings = In.Settings;
		Out.bHasForeignPlayWork = In.bHasForeignPlayWork;
		Out.bScopedEndQueued = In.bScopedEndQueued;
	}
}

bool CortexEngineCompat::SupportsScopedPIE(const UEngine& Engine)
{
	const UEditorEngine* EditorEngine = Cast<UEditorEngine>(&Engine);
	return EditorEngine != nullptr
		&& EditorEngine->GetScopedPIESessionCapabilityVersion() == UE_SCOPED_PIE_SESSION_API_VERSION;
}

FCortexCommandResult CortexEngineCompat::RequestOwnedPIE(
	UEngine& Engine,
	const FRequestPlaySessionParams& Params,
	TSharedPtr<FCortexScopedPIEAuthority>& OutAuthority)
{
	OutAuthority.Reset();

	UEditorEngine* EditorEngine = Cast<UEditorEngine>(&Engine);
	if (!EditorEngine || EditorEngine->GetScopedPIESessionCapabilityVersion() != UE_SCOPED_PIE_SESSION_API_VERSION)
	{
		return MakeCompatError(CortexErrorCodes::InvalidOperation,
			TEXT("This engine does not provide the ownership-scoped PIE session capability; the frame-clock workflow requires it."));
	}
	if (EditorEngine->IsPlaySessionInProgress())
	{
		return MakeCompatError(CortexErrorCodes::PIEAlreadyActive,
			TEXT("A PIE session is already queued or running; the owned request was refused rather than adopting it."));
	}

	FScopedPIEHandle Handle;
	FText Error;
	if (!EditorEngine->RequestPlaySessionWithScope(Params, Handle, Error))
	{
		return MakeCompatError(CortexErrorCodes::InvalidOperation, Error.ToString());
	}

	TSharedPtr<FCortexScopedPIEAuthority> Authority = MakeShared<FCortexScopedPIEAuthority>();
	Authority->Handle = Handle;
	OutAuthority = Authority;
	return MakeCompatSuccess();
}

FCortexCommandResult CortexEngineCompat::QuiesceOwnedPIE(const TSharedRef<FCortexScopedPIEAuthority>& Authority)
{
	UEditorEngine* EditorEngine = ResolveScopedEditor();
	if (!EditorEngine || !Authority->Handle.IsValid())
	{
		return MakeCompatError(CortexErrorCodes::InvalidOperation, TEXT("No scoped PIE authority is available to quiesce."));
	}

	FText Error;
	if (!EditorEngine->QuiescePlaySession(Authority->Handle, Error))
	{
		return MakeCompatError(CortexErrorCodes::InvalidOperation, Error.ToString());
	}
	return MakeCompatSuccess();
}

FCortexCommandResult CortexEngineCompat::EndOwnedPIE(const TSharedRef<FCortexScopedPIEAuthority>& Authority)
{
	UEditorEngine* EditorEngine = ResolveScopedEditor();
	if (!EditorEngine || !Authority->Handle.IsValid())
	{
		return MakeCompatError(CortexErrorCodes::InvalidOperation, TEXT("No scoped PIE authority is available to end."));
	}

	FText Error;
	if (!EditorEngine->RequestEndScopedPlaySession(Authority->Handle, Error))
	{
		return MakeCompatError(CortexErrorCodes::InvalidOperation, Error.ToString());
	}
	return MakeCompatSuccess();
}

FCortexCommandResult CortexEngineCompat::ReadOwnedPIE(
	const TSharedRef<FCortexScopedPIEAuthority>& Authority,
	FCortexScopedPIESnapshot& OutSnapshot)
{
	UEditorEngine* EditorEngine = ResolveScopedEditor();
	if (!EditorEngine || !Authority->Handle.IsValid())
	{
		return MakeCompatError(CortexErrorCodes::InvalidOperation, TEXT("No scoped PIE authority is available to read."));
	}

	FScopedPIESnapshot EngineSnapshot;
	if (!EditorEngine->QueryScopedPlaySession(Authority->Handle, EngineSnapshot))
	{
		return MakeCompatError(CortexErrorCodes::InvalidOperation, TEXT("The scoped PIE authority is not valid for this engine."));
	}

	CopyScopedSnapshot(EngineSnapshot, OutSnapshot);
	return MakeCompatSuccess();
}

FDelegateHandle CortexEngineCompat::ObserveScopedPIELifecycle(TFunction<void(const FCortexScopedPIESnapshot&)>&& Callback)
{
	if (!ResolveScopedEditor())
	{
		return FDelegateHandle();
	}

	TFunction<void(const FCortexScopedPIESnapshot&)> Sink = MoveTemp(Callback);
	return FScopedPIESessionDelegates::OnLifecycle().AddLambda(
		[Sink = MoveTemp(Sink)](const FScopedPIELifecycleEvent& Event)
		{
			FCortexScopedPIESnapshot Snapshot;
			CopyScopedSnapshot(Event.Snapshot, Snapshot);
			Sink(Snapshot);
		});
}

void CortexEngineCompat::RemoveScopedPIELifecycleObserver(FDelegateHandle Handle)
{
	if (Handle.IsValid())
	{
		FScopedPIESessionDelegates::OnLifecycle().Remove(Handle);
	}
}

FDelegateHandle CortexEngineCompat::ObservePIEAdmission(TFunction<void(uint64)>&& Callback)
{
	if (!ResolveScopedEditor())
	{
		return FDelegateHandle();
	}
	return FScopedPIESessionDelegates::OnAdmissionChanged().AddLambda(MoveTemp(Callback));
}

void CortexEngineCompat::RemovePIEAdmissionObserver(FDelegateHandle Handle)
{
	if (Handle.IsValid())
	{
		FScopedPIESessionDelegates::OnAdmissionChanged().Remove(Handle);
	}
}

#else // Unsupported engine build: explicit refusal, never a stock fallback.

bool CortexEngineCompat::SupportsScopedPIE(const UEngine& Engine)
{
	static_cast<void>(Engine);
	return false;
}

FCortexCommandResult CortexEngineCompat::RequestOwnedPIE(
	UEngine& Engine,
	const FRequestPlaySessionParams& Params,
	TSharedPtr<FCortexScopedPIEAuthority>& OutAuthority)
{
	OutAuthority.Reset();
	static_cast<void>(Engine);
	static_cast<void>(Params);
	return MakeCompatError(CortexErrorCodes::InvalidOperation,
		TEXT("This engine build does not compile the ownership-scoped PIE session capability."));
}

FCortexCommandResult CortexEngineCompat::QuiesceOwnedPIE(const TSharedRef<FCortexScopedPIEAuthority>& Authority)
{
	static_cast<void>(Authority);
	return MakeCompatError(CortexErrorCodes::InvalidOperation,
		TEXT("This engine build does not compile the ownership-scoped PIE session capability."));
}

FCortexCommandResult CortexEngineCompat::EndOwnedPIE(const TSharedRef<FCortexScopedPIEAuthority>& Authority)
{
	static_cast<void>(Authority);
	return MakeCompatError(CortexErrorCodes::InvalidOperation,
		TEXT("This engine build does not compile the ownership-scoped PIE session capability."));
}

FCortexCommandResult CortexEngineCompat::ReadOwnedPIE(
	const TSharedRef<FCortexScopedPIEAuthority>& Authority,
	FCortexScopedPIESnapshot& OutSnapshot)
{
	static_cast<void>(Authority);
	OutSnapshot = FCortexScopedPIESnapshot();
	return MakeCompatError(CortexErrorCodes::InvalidOperation,
		TEXT("This engine build does not compile the ownership-scoped PIE session capability."));
}

FDelegateHandle CortexEngineCompat::ObserveScopedPIELifecycle(TFunction<void(const FCortexScopedPIESnapshot&)>&& Callback)
{
	static_cast<void>(Callback);
	return FDelegateHandle();
}

void CortexEngineCompat::RemoveScopedPIELifecycleObserver(FDelegateHandle Handle)
{
	static_cast<void>(Handle);
}

FDelegateHandle CortexEngineCompat::ObservePIEAdmission(TFunction<void(uint64)>&& Callback)
{
	static_cast<void>(Callback);
	return FDelegateHandle();
}

void CortexEngineCompat::RemovePIEAdmissionObserver(FDelegateHandle Handle)
{
	static_cast<void>(Handle);
}

#endif
