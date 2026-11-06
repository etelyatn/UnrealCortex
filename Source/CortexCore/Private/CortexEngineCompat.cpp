#include "CortexEngineCompat.h"

#if WITH_EDITOR && defined(UE_SCOPED_PIE_SESSION_API_VERSION) && UE_SCOPED_PIE_SESSION_API_VERSION == 1

#include "Editor.h"
#include "Editor/EditorEngine.h"

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

#else // Stock engine: no scoped PIE capability, so there is nothing to observe.

FDelegateHandle CortexEngineCompat::ObserveScopedPIELifecycle(TFunction<void(const FCortexScopedPIESnapshot&)>&& Callback)
{
	static_cast<void>(Callback);
	return FDelegateHandle();
}

void CortexEngineCompat::RemoveScopedPIELifecycleObserver(FDelegateHandle Handle)
{
	static_cast<void>(Handle);
}

#endif
