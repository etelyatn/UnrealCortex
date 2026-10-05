#include "Misc/AutomationTest.h"

#include "CortexCommandRouter.h"
#include "CortexEditorPhysicalInput.h"
#include "CortexReplayErrorCodes.h"
#include "CortexReplayLibrary.h"
#include "CortexReplayService.h"
#include "CortexReplayTestUtils.h"
#include "CortexReplayTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Input/Events.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PlayInEditorDataTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Slate/SceneViewport.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"
#include "Widgets/SViewport.h"

namespace
{
/** Harness-only failure reporting; the product deadline itself is asserted separately. */
constexpr double ReplayReadyWatchdogSeconds = 15.0;
constexpr double ReplayProductDeadlineWatchdogSeconds = 45.0;
constexpr double ReplayTeardownWatchdogSeconds = 15.0;

/** The recorded pawn class must load on the owned PIE target; the sandbox maps spawn a DefaultPawn. */
constexpr TCHAR ReplayPawnClassPath[] = TEXT("/Script/Engine.DefaultPawn");

/**
 * The map the recording names, chosen as the alternative to the currently open editor map so the
 * owned PIE load cannot disturb the editor world (same convention as the shared session tests).
 */
FString MakeReplayMapAssetPath()
{
	if (!GEditor)
	{
		return TEXT("/Game/Maps/TestMap");
	}
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return TEXT("/Game/Maps/TestMap");
	}
	const FString Current = UWorld::RemovePIEPrefix(World->GetPackage()->GetName());
	return Current == TEXT("/Game/Maps/TestMap") ? TEXT("/Game/Maps/TestRoomMap")
		: TEXT("/Game/Maps/TestMap");
}

FCortexReplayEvent MakeKeyPress(int32 Sequence, double TimeSeconds,
	ECortexEditorPhysicalInputKind Kind, const FKey& Key)
{
	FCortexReplayEvent Event;
	Event.Sequence = Sequence;
	Event.TimeSeconds = TimeSeconds;
	Event.Input.Kind = Kind;
	Event.Input.Key = Key;
	return Event;
}

/** Publishes one valid AI-eligible recording whose map is a real loadable PIE map. */
bool PublishReplayRecording(FAutomationTestBase& Test, FCortexReplayTestFixture& Fixture,
	int32 Id, const FString& MapPath, const TArray<FCortexReplayEvent>& Events, bool bAIEnabled,
	double DurationOverrideSeconds = 0.0)
{
	FCortexReplaySnapshot Snapshot = Fixture.MakeRecording(Id, bAIEnabled, Events);
	Snapshot.Metadata.MapAssetPath = MapPath;
	Snapshot.InitialState.PawnClassPath = ReplayPawnClassPath;
	if (DurationOverrideSeconds > 0.0)
	{
		Snapshot.Metadata.DurationSeconds = FMath::Max(Snapshot.Metadata.DurationSeconds,
			DurationOverrideSeconds);
	}
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());
	return Test.TestTrue(TEXT("Recording published"), Library.Publish(Snapshot).bSuccess);
}

FString ReplayMetadataPath(const FCortexReplayTestFixture& Fixture, int32 Id)
{
	return FPaths::Combine(
		FPaths::Combine(
			FPaths::Combine(Fixture.GetProjectRoot(), TEXT(".cortex/replay/recordings")),
			FString::FromInt(Id)),
		TEXT("metadata.json"));
}

FString ReplayRunRecordPath(const FCortexReplayTestFixture& Fixture, const FGuid& Id)
{
	return FPaths::Combine(
		FPaths::Combine(Fixture.GetProjectRoot(), TEXT("Saved/CortexReplay/Runs")),
		Id.ToString(EGuidFormats::DigitsWithHyphens).ToLower() + TEXT(".json"));
}

bool SaveJsonObjectFile(const FString& Path, const TSharedRef<FJsonObject>& Object)
{
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	if (!FJsonSerializer::Serialize(Object, Writer))
	{
		return false;
	}
	return FFileHelper::SaveStringToFile(Text, *Path);
}

/** Builds a retained terminal run record; the coverage counts are caller-supplied deliberately. */
TSharedRef<FJsonObject> MakeRetainedRunRecordJson(const FGuid& Id, int32 RecordingId,
	const FString& State, int32 Dispatched, int32 Total, int32 PosePresses, int32 SupportedPresses,
	int32 UnavailablePresses, int32 NotApplicablePresses)
{
	const FString Stamp = FDateTime::UtcNow().ToIso8601();
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("format"), TEXT("CortexReplayRun"));
	Object->SetNumberField(TEXT("schema_version"), 1);
	Object->SetStringField(TEXT("run_id"), Id.ToString(EGuidFormats::DigitsWithHyphens).ToLower());
	Object->SetNumberField(TEXT("recording_id"), RecordingId);
	Object->SetStringField(TEXT("origin"), TEXT("ai"));
	Object->SetStringField(TEXT("state"), State);
	Object->SetStringField(TEXT("editor_instance_id"), TEXT("test-editor"));
	Object->SetStringField(TEXT("started_at_utc"), Stamp);
	Object->SetStringField(TEXT("finalized_at_utc"), Stamp);
	Object->SetNumberField(TEXT("dispatched_events"), Dispatched);
	Object->SetNumberField(TEXT("total_events"), Total);
	Object->SetNumberField(TEXT("authorized_wait_seconds"), 0.0);
	Object->SetStringField(TEXT("recording_snapshot_sha256"), FString::ChrN(64, TEXT('a')));
	Object->SetStringField(TEXT("initial_state_sha256"), FString::ChrN(64, TEXT('b')));
	Object->SetStringField(TEXT("inputs_sha256"), FString::ChrN(64, TEXT('c')));

	TSharedRef<FJsonObject> Coverage = MakeShared<FJsonObject>();
	Coverage->SetNumberField(TEXT("pose_presses"), PosePresses);
	Coverage->SetNumberField(TEXT("ui_supported_presses"), SupportedPresses);
	Coverage->SetNumberField(TEXT("ui_unavailable_presses"), UnavailablePresses);
	Coverage->SetNumberField(TEXT("ui_not_applicable_presses"), NotApplicablePresses);
	Object->SetObjectField(TEXT("guard_coverage"), Coverage);
	return Object;
}

/** Rewrites only the live permission bit of a published recording's metadata. */
bool SetMetadataAIEnabled(const FString& MetadataPath, bool bEnabled)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *MetadataPath))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		return false;
	}
	Object->SetBoolField(TEXT("ai_enabled"), bEnabled);
	return SaveJsonObjectFile(MetadataPath, Object.ToSharedRef());
}

FString RunState(const FCortexCommandResult& Result)
{
	return Result.Data.IsValid() ? Result.Data->GetStringField(TEXT("state")) : FString();
}

FGuid ParseRunId(const FCortexCommandResult& Result)
{
	FGuid Id;
	if (Result.Data.IsValid())
	{
		FGuid::Parse(Result.Data->GetStringField(TEXT("run_id")), Id);
	}
	return Id;
}

bool IsTerminalRunState(const FString& State)
{
	return State == TEXT("Completed") || State == TEXT("Cancelled")
		|| State == TEXT("Interrupted") || State == TEXT("Error");
}

FString RunExecutionErrorCode(const FCortexCommandResult& Result)
{
	const TSharedPtr<FJsonObject>* Error = nullptr;
	if (Result.Data.IsValid()
		&& Result.Data->TryGetObjectField(TEXT("execution_error"), Error)
		&& Error != nullptr && Error->IsValid())
	{
		return (*Error)->GetStringField(TEXT("code"));
	}
	return FString();
}

int32 RunDispatchedEvents(const FCortexCommandResult& Result)
{
	return Result.Data.IsValid()
		? static_cast<int32>(Result.Data->GetNumberField(TEXT("dispatched_events"))) : INDEX_NONE;
}

bool IsPIEWorldPresentForMap(const FString& MapAssetPath)
{
	if (!GEngine)
	{
		return false;
	}
	const FString RequestedPackage = FPackageName::ObjectPathToPackageName(MapAssetPath);
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType != EWorldType::PIE || Context.World() == nullptr)
		{
			continue;
		}
		if (UWorld::RemovePIEPrefix(Context.World()->GetPackage()->GetName()) == RequestedPackage)
		{
			return true;
		}
	}
	return false;
}

bool HasAnyPIEWorldContext()
{
	if (!GEngine)
	{
		return false;
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE)
		{
			return true;
		}
	}
	return false;
}

TSharedRef<FCortexReplayService> MakeService(const FCortexReplayTestFixture& Fixture)
{
	return MakeShared<FCortexReplayService>(Fixture.GetProjectRoot());
}

// ---------------------------------------------------------------------------
// Latent commands
// ---------------------------------------------------------------------------

/** Runs one synchronous check once. */
class FCortexReplayRunOnce : public IAutomationLatentCommand
{
public:
	FCortexReplayRunOnce(FAutomationTestBase* InTest, TFunction<void(FAutomationTestBase&)> InAction,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Action(MoveTemp(InAction)), KeepAlive(MoveTemp(InKeepAlive)) {}
	bool Update() override
	{
		if (!bRan)
		{
			bRan = true;
			Action(*Test);
		}
		return true;
	}
private:
	FAutomationTestBase* Test;
	TFunction<void(FAutomationTestBase&)> Action;
	/**
	 * Keeps the owning fixture (and therefore its temporary project root) alive for as long as
	 * this latent command exists: latent commands run after RunTest returns, so a local fixture
	 * reference would otherwise delete the recorded library while the service still needs it.
	 */
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
	bool bRan = false;
};

/** Polls get_run until the run reports one exact state. */
class FCortexReplayAwaitRunState : public IAutomationLatentCommand
{
public:
	FCortexReplayAwaitRunState(FAutomationTestBase* InTest, TSharedRef<FCortexReplayService> InService,
		FGuid InRunId, FString InExpected, TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr,
		double InWatchdog = ReplayReadyWatchdogSeconds * 2)
		: Test(InTest), Service(MoveTemp(InService)), RunId(InRunId)
		, Expected(MoveTemp(InExpected)), Watchdog(InWatchdog), KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		const FCortexCommandResult Run = Service->GetRun(RunId, true);
		if (!Run.bSuccess)
		{
			Test->AddError(TEXT("get_run failed while awaiting a run state"));
			return true;
		}
		LastState = RunState(Run);
		if (LastState == Expected) { return true; }
		if (IsTerminalRunState(LastState) && LastState != Expected)
		{
			Test->AddError(FString::Printf(TEXT("Run reached %s instead of %s"), *LastState, *Expected));
			return true;
		}
		if (FPlatformTime::Seconds() - StartTime > Watchdog)
		{
			Test->AddError(FString::Printf(TEXT("Run never reached %s (last %s)"), *Expected, *LastState));
			return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	FGuid RunId;
	FString Expected;
	double Watchdog;
	double StartTime = 0.0;
	FString LastState;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};

/** Polls get_run until the run reports any terminal state and stores that state. */
class FCortexReplayAwaitRunTerminal : public IAutomationLatentCommand
{
public:
	FCortexReplayAwaitRunTerminal(FAutomationTestBase* InTest,
		TSharedRef<FCortexReplayService> InService, FGuid InRunId,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr,
		double InWatchdog = ReplayReadyWatchdogSeconds * 2)
		: Test(InTest), Service(MoveTemp(InService)), RunId(InRunId), Watchdog(InWatchdog)
		, KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		const FCortexCommandResult Run = Service->GetRun(RunId, true);
		if (!Run.bSuccess)
		{
			Test->AddError(TEXT("get_run failed while awaiting a terminal run"));
			return true;
		}
		FinalState = RunState(Run);
		if (IsTerminalRunState(FinalState)) { return true; }
		if (FPlatformTime::Seconds() - StartTime > Watchdog)
		{
			Test->AddError(FString::Printf(TEXT("Run never finalized (last %s)"), *FinalState));
			return true;
		}
		return false;
	}

	const FString& GetFinalState() const { return FinalState; }
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	FGuid RunId;
	double Watchdog;
	double StartTime = 0.0;
	FString FinalState;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};

/** Waits until no PIE world context remains. */
class FCortexReplayAwaitNoPieWorlds : public IAutomationLatentCommand
{
public:
	explicit FCortexReplayAwaitNoPieWorlds(FAutomationTestBase* InTest,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), KeepAlive(MoveTemp(InKeepAlive)) {}
	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		if (!HasAnyPIEWorldContext()) { return true; }
		if (FPlatformTime::Seconds() - StartTime > ReplayTeardownWatchdogSeconds)
		{
			Test->AddError(TEXT("PIE contexts did not disappear in time"));
			return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
	double StartTime = 0.0;
};

/** Waits for an externally started PIE session to begin playing. */
class FCortexReplayAwaitExternalPiePlaying : public IAutomationLatentCommand
{
public:
	explicit FCortexReplayAwaitExternalPiePlaying(FAutomationTestBase* InTest,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), KeepAlive(MoveTemp(InKeepAlive)) {}
	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		if (GEditor && GEditor->PlayWorld != nullptr
			&& GEditor->PlayWorld->HasBegunPlay()
			&& GEditor->PlayWorld->GetFirstPlayerController() != nullptr)
		{
			return true;
		}
		if (FPlatformTime::Seconds() - StartTime > ReplayReadyWatchdogSeconds)
		{
			Test->AddError(TEXT("Timed out waiting for the external PIE session"));
			return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
	double StartTime = 0.0;
};

/** Polls get_run until the dispatched-event count is reached. */
class FCortexReplayAwaitDispatchedEvents : public IAutomationLatentCommand
{
public:
	FCortexReplayAwaitDispatchedEvents(FAutomationTestBase* InTest,
		TSharedRef<FCortexReplayService> InService, FGuid InRunId, int32 InCount,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Service(MoveTemp(InService)), RunId(InRunId), Count(InCount)
		, KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		const FCortexCommandResult Run = Service->GetRun(RunId, true);
		if (!Run.bSuccess)
		{
			Test->AddError(TEXT("get_run failed while awaiting dispatched events"));
			return true;
		}
		if (RunDispatchedEvents(Run) >= Count) { return true; }
		if (IsTerminalRunState(RunState(Run))
			|| FPlatformTime::Seconds() - StartTime > ReplayReadyWatchdogSeconds * 2)
		{
			Test->AddError(FString::Printf(TEXT("Run never dispatched %d events (last %s)"),
				Count, *RunState(Run)));
			return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	FGuid RunId;
	int32 Count = 0;
	double StartTime = 0.0;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};

/** Polls a freshly constructed service until the retained terminal record is queryable. */
class FCortexReplayAwaitRetainedRun : public IAutomationLatentCommand
{
public:
	FCortexReplayAwaitRetainedRun(FAutomationTestBase* InTest,
		TSharedPtr<FCortexReplayTestFixture> InFixture, FGuid InRunId, FString InExpected,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Fixture(MoveTemp(InFixture)), RunId(InRunId)
		, Expected(MoveTemp(InExpected)), KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		// A new service re-loads the retained history from disk on construction.
		FCortexReplayService Probe(Fixture->GetProjectRoot());
		const FCortexCommandResult Run = Probe.GetRun(RunId, true);
		if (Run.bSuccess && RunState(Run) == Expected) { return true; }
		if (FPlatformTime::Seconds() - StartTime > ReplayReadyWatchdogSeconds * 2)
		{
			Test->AddError(FString::Printf(TEXT("Retained run never became %s"), *Expected));
			return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	TSharedPtr<FCortexReplayTestFixture> Fixture;
	FGuid RunId;
	FString Expected;
	double StartTime = 0.0;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};

/** Waits until the native service reports no active capture operation. */
class FCortexReplayAwaitCaptureIdle : public IAutomationLatentCommand
{
public:
	FCortexReplayAwaitCaptureIdle(FAutomationTestBase* InTest,
		TSharedRef<FCortexReplayService> InService,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Service(MoveTemp(InService)), KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		const FCortexCommandResult Current = Service->GetCurrentOperation();
		if (Current.bSuccess && !Current.Data.IsValid()) { return true; }
		if (FPlatformTime::Seconds() - StartTime > ReplayReadyWatchdogSeconds * 2)
		{
			Test->AddError(TEXT("Capture never released its retained publication state"));
			return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	double StartTime = 0.0;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};

/** Waits until the native service reports the expected capture state. */
class FCortexReplayAwaitCapturePhase : public IAutomationLatentCommand
{
public:
	FCortexReplayAwaitCapturePhase(FAutomationTestBase* InTest,
		TSharedRef<FCortexReplayService> InService, FString InExpected,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Service(MoveTemp(InService)), Expected(MoveTemp(InExpected))
		, KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		const FCortexCommandResult Current = Service->GetCurrentOperation();
		if (Current.bSuccess && Current.Data.IsValid()
			&& Current.Data->GetStringField(TEXT("state")) == Expected)
		{
			return true;
		}
		if (FPlatformTime::Seconds() - StartTime > ReplayReadyWatchdogSeconds * 2)
		{
			Test->AddError(FString::Printf(TEXT("Capture never reached %s"), *Expected));
			return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	FString Expected;
	double StartTime = 0.0;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};

/** Waits until the capture status reports a retained publication failure. */
class FCortexReplayAwaitCapturePublicationFailure : public IAutomationLatentCommand
{
public:
	FCortexReplayAwaitCapturePublicationFailure(FAutomationTestBase* InTest,
		TSharedRef<FCortexReplayService> InService,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Service(MoveTemp(InService)), KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		const FCortexCommandResult Current = Service->GetCurrentOperation();
		if (Current.bSuccess && Current.Data.IsValid()
			&& Current.Data->GetBoolField(TEXT("publication_failed")))
		{
			return true;
		}
		if (FPlatformTime::Seconds() - StartTime > ReplayReadyWatchdogSeconds * 2)
		{
			Test->AddError(TEXT("Capture publication failure was never reported"));
			return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	double StartTime = 0.0;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};

/**
 * While the matching owned PIE context exists after cancellation, get_run must stay Finalizing,
 * no further input may be dispatched and competing starts must be busy. The stable terminal
 * result becomes queryable only after the product's own teardown removed the context.
 */
class FCortexReplayObserveFinalizing : public IAutomationLatentCommand
{
public:
	FCortexReplayObserveFinalizing(FAutomationTestBase* InTest,
		TSharedRef<FCortexReplayService> InService, FGuid InRunId, FString InMapPath,
		int32 InAdmittedRecordingId, TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Service(MoveTemp(InService)), RunId(InRunId), MapPath(MoveTemp(InMapPath))
		, AdmittedRecordingId(InAdmittedRecordingId), KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (StartTime == 0.0) { StartTime = FPlatformTime::Seconds(); }
		const FCortexCommandResult Run = Service->GetRun(RunId, true);
		if (!Run.bSuccess)
		{
			Test->AddError(TEXT("get_run failed during finalization"));
			return true;
		}

		if (IsPIEWorldPresentForMap(MapPath))
		{
			if (!bObservedContext)
			{
				bObservedContext = true;
				DispatchedAtFinalizing = RunDispatchedEvents(Run);
				Test->TestTrue(TEXT("get_run remains Finalizing while the owned context exists"),
					RunState(Run) == TEXT("Finalizing"));
				const FCortexCommandResult Competing = Service->StartReplay(AdmittedRecordingId,
					ECortexReplayOrigin::AI);
				Test->TestFalse(TEXT("Competing start is busy during finalization"), Competing.bSuccess);
			}
			else
			{
				Test->TestEqual(TEXT("No input dispatched during finalization"),
					RunDispatchedEvents(Run), DispatchedAtFinalizing);
			}
			if (FPlatformTime::Seconds() - StartTime > ReplayTeardownWatchdogSeconds * 2)
			{
				Test->AddError(TEXT("Owned context never disappeared after cancellation"));
				return true;
			}
			return false;
		}

		Test->TestTrue(TEXT("Owned context really existed during finalization"), bObservedContext);

		// The product retains its finalization ticker until IsOwnedPIEEnded() confirms teardown and
		// only then publishes the stable terminal result, so allow that documented bounded window
		// instead of requiring the publication in the same frame the context disappeared.
		if (!IsTerminalRunState(RunState(Run)))
		{
			if (FPlatformTime::Seconds() - StartTime <= ReplayTeardownWatchdogSeconds * 2)
			{
				return false;
			}
			Test->AddError(TEXT("Owned PIE teardown never reached a stable terminal state"));
			return true;
		}
		Test->TestTrue(TEXT("Stable terminal state after product-owned teardown"),
			IsTerminalRunState(RunState(Run)));
		return true;
	}

private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	FGuid RunId;
	FString MapPath;
	int32 AdmittedRecordingId = 0;
	double StartTime = 0.0;
	bool bObservedContext = false;
	int32 DispatchedAtFinalizing = INDEX_NONE;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};

/**
 * Test-only prerequisite gate: keeps the owned PIE world created after the baseline from
 * possessing a pawn or from having its owned viewport registered, exposing exactly the
 * unavailability the product validates. It never touches an existing/external PIE session.
 */
enum class EReplayPreparationGateMode : uint8 { Possession, Viewport };

class FReplayPreparationGate
{
public:
	FReplayPreparationGate(EReplayPreparationGateMode InMode, FString InMapPath)
		: Mode(InMode), MapPath(MoveTemp(InMapPath)) {}

	void CaptureBaselineWorlds()
	{
		BaselineWorlds.Reset();
		if (GEngine)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType == EWorldType::PIE && Context.World())
				{
					BaselineWorlds.Add(Context.World());
				}
			}
		}
	}

	void Enforce()
	{
		if (!bClosed)
		{
			return;
		}
		UWorld* World = FindOwnedPIEWorld();
		if (World == nullptr)
		{
			return;
		}
		GatedWorld = World;
		if (Mode == EReplayPreparationGateMode::Possession)
		{
			bPrerequisiteWithheld |= EnforceMissingPawn(World);
		}
		else
		{
			bPrerequisiteWithheld |= EnforceUnregisteredViewport(World);
		}
	}

	bool WasPrerequisiteWithheld() const { return bPrerequisiteWithheld; }
	const FString& GetMapPath() const { return MapPath; }

	void Release()
	{
		if (!bClosed)
		{
			return;
		}
		bClosed = false;
		if (bSavedDefaultPawnClass)
		{
			if (UWorld* World = GatedWorld.Get())
			{
				if (AGameModeBase* GameMode = World->GetAuthGameMode())
				{
					GameMode->DefaultPawnClass = SavedDefaultPawnClass;
				}
			}
			bSavedDefaultPawnClass = false;
		}
		if (Mode == EReplayPreparationGateMode::Viewport
			&& WithheldViewportClient.IsValid() && WithheldSceneViewport != nullptr)
		{
			WithheldViewportClient->AddAssociation(*WithheldSceneViewport);
		}
	}

private:
	UWorld* FindOwnedPIEWorld()
	{
		if (GatedWorld.IsValid())
		{
			return GatedWorld.Get();
		}
		if (!GEngine)
		{
			return nullptr;
		}
		const FString RequestedPackage = FPackageName::ObjectPathToPackageName(MapPath);
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Context.WorldType == EWorldType::PIE && Candidate != nullptr
				&& !BaselineWorlds.Contains(Candidate)
				&& UWorld::RemovePIEPrefix(Candidate->GetPackage()->GetName()) == RequestedPackage)
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	bool EnforceMissingPawn(UWorld* World)
	{
		AGameModeBase* GameMode = World ? World->GetAuthGameMode() : nullptr;
		if (GameMode && !bSavedDefaultPawnClass)
		{
			SavedDefaultPawnClass = GameMode->DefaultPawnClass;
			bSavedDefaultPawnClass = true;
			GameMode->DefaultPawnClass = nullptr;
		}
		APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
		if (Controller == nullptr)
		{
			return GameMode != nullptr;
		}
		if (APawn* Pawn = Controller->GetPawn())
		{
			Controller->UnPossess();
			Pawn->Destroy();
		}
		return true;
	}

	bool EnforceUnregisteredViewport(UWorld* World)
	{
		UGameViewportClient* Client = World ? World->GetGameViewport() : nullptr;
		if (Client == nullptr)
		{
			return false;
		}
		if (WithheldViewportClient.Get() != Client)
		{
			WithheldViewportClient = Client;
			WithheldSceneViewport = nullptr;
		}
		if (WithheldSceneViewport == nullptr)
		{
			WithheldSceneViewport = Client->GetGameViewport();
		}
		if (WithheldSceneViewport != nullptr && Client->GetGameViewport() == WithheldSceneViewport)
		{
			Client->RemoveAssociation(*WithheldSceneViewport);
		}
		return WithheldSceneViewport != nullptr;
	}

	EReplayPreparationGateMode Mode;
	FString MapPath;
	bool bClosed = true;
	bool bPrerequisiteWithheld = false;
	TSubclassOf<APawn> SavedDefaultPawnClass;
	bool bSavedDefaultPawnClass = false;
	TArray<TWeakObjectPtr<UWorld>> BaselineWorlds;
	TWeakObjectPtr<UWorld> GatedWorld;
	TWeakObjectPtr<UGameViewportClient> WithheldViewportClient;
	FSceneViewport* WithheldSceneViewport = nullptr;
};

/**
 * Holds the real missing-pawn/missing-viewport prerequisite until the product's own preparation
 * deadline fails, then asserts the retained Error/INPUT_PREPARATION_TIMEOUT with zero dispatched
 * events, that a late readiness signal cannot mutate the expired run, and that a successor can
 * still be admitted.
 */
class FCortexReplayGatePreparationTimeout : public IAutomationLatentCommand
{
public:
	FCortexReplayGatePreparationTimeout(FAutomationTestBase* InTest,
		TSharedRef<FCortexReplayService> InService, FGuid InRunId, int32 InRecordingId,
		TSharedRef<FReplayPreparationGate> InGate,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive = nullptr)
		: Test(InTest), Service(MoveTemp(InService)), RunId(InRunId)
		, RecordingId(InRecordingId), Gate(MoveTemp(InGate)), KeepAlive(MoveTemp(InKeepAlive)) {}

	bool Update() override
	{
		if (!bAdmitted)
		{
			bAdmitted = true;
			Deadline = FPlatformTime::Seconds() + ReplayProductDeadlineWatchdogSeconds;
		}

		Gate->Enforce();

		const FCortexCommandResult Run = Service->GetRun(RunId, true);
		if (!Run.bSuccess)
		{
			Test->AddError(TEXT("get_run failed while awaiting the preparation timeout"));
			return true;
		}

		if (!bTimeoutSeen)
		{
			if (!IsTerminalRunState(RunState(Run)))
			{
				if (FPlatformTime::Seconds() > Deadline)
				{
					Test->AddError(TEXT("Replay preparation never timed out"));
					return true;
				}
				return false;
			}

			bTimeoutSeen = true;
			Test->TestTrue(TEXT("Preparation failed as a terminal Error"), RunState(Run) == TEXT("Error"));
			Test->TestEqual(TEXT("Retained primary preparation code"),
				RunExecutionErrorCode(Run), FString(TEXT("INPUT_PREPARATION_TIMEOUT")));
			Test->TestEqual(TEXT("Preparation failure dispatched nothing"),
				RunDispatchedEvents(Run), 0);
			Test->TestTrue(TEXT("Withheld prerequisite was really observed"), Gate->WasPrerequisiteWithheld());
			FrozenState = RunState(Run);
			FrozenDispatched = RunDispatchedEvents(Run);
			FrozenErrorCode = RunExecutionErrorCode(Run);
			// A late readiness signal appears only now, after the product already failed.
			Gate->Release();
			ReleaseTime = FPlatformTime::Seconds();
			return false;
		}

		// The released prerequisite must not resurrect or mutate the expired run.
		if (FPlatformTime::Seconds() - ReleaseTime <= 2.0)
		{
			return false;
		}

		if (!bLateChecked)
		{
			bLateChecked = true;
			Test->TestEqual(TEXT("Late readiness cannot change the expired state"),
				RunState(Run), FrozenState);
			Test->TestEqual(TEXT("Late readiness cannot dispatch into the expired run"),
				RunDispatchedEvents(Run), FrozenDispatched);
			Test->TestEqual(TEXT("Retained error code survives late readiness"),
				RunExecutionErrorCode(Run), FrozenErrorCode);
			Test->TestTrue(TEXT("No PIE context survives the preparation failure"),
				!IsPIEWorldPresentForMap(Gate->GetMapPath()));
		}

		if (!bSuccessorChecked)
		{
			// The expired run must have fully finalized (no Finalizing) before a successor.
			if (!IsTerminalRunState(RunState(Run)))
			{
				if (FPlatformTime::Seconds() - ReleaseTime <= ReplayProductDeadlineWatchdogSeconds)
				{
					return false;
				}
				Test->AddError(TEXT("Expired run never reached a stable terminal state"));
				return true;
			}

			bSuccessorChecked = true;
			// A successor run may be admitted after the expired run's owned context is gone.
			const FCortexCommandResult Successor = Service->StartReplay(RecordingId,
				ECortexReplayOrigin::AI);
			Test->TestTrue(TEXT("Successor replay admitted after the preparation timeout"),
				Successor.bSuccess);
			if (Successor.bSuccess)
			{
				SuccessorId = ParseRunId(Successor);
			}
		}

		if (SuccessorId.IsValid())
		{
			const FCortexCommandResult SuccessorRun = Service->GetRun(SuccessorId, true);
			if (SuccessorRun.bSuccess && IsTerminalRunState(RunState(SuccessorRun)))
			{
				return true;
			}
			if (FPlatformTime::Seconds() - ReleaseTime > ReplayProductDeadlineWatchdogSeconds)
			{
				Test->AddError(TEXT("Successor replay did not finalize"));
				return true;
			}
			return false;
		}

		return true;
	}

	FGuid GetSuccessorId() const { return SuccessorId; }
private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	FGuid RunId;
	int32 RecordingId = 0;
	TSharedRef<FReplayPreparationGate> Gate;
	double Deadline = 0.0;
	double ReleaseTime = 0.0;
	bool bAdmitted = false;
	bool bTimeoutSeen = false;
	bool bLateChecked = false;
	bool bSuccessorChecked = false;
	FString FrozenState;
	FString FrozenErrorCode;
	int32 FrozenDispatched = INDEX_NONE;
	FGuid SuccessorId;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
};


}

// ---------------------------------------------------------------------------
// Admission, busy rejection and repeated cancellation of an admitted run.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleAdmissionTest,
	"Cortex.Replay.Lifecycle.StartAdmissionBusyAndRepeatedCancellation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleAdmissionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	// No prior result for a recording that was never replayed.
	const FCortexCommandResult NoPrior = Service->GetLastRunForRecording(1);
	TestTrue(TEXT("get_last_run_for_recording succeeds without a prior run"), NoPrior.bSuccess);
	TestFalse(TEXT("No prior run summary is null"), NoPrior.Data.IsValid());
	const FCortexCommandResult Idle = Service->GetCurrentOperation();
	TestTrue(TEXT("get_current_operation succeeds while idle"), Idle.bSuccess);
	TestFalse(TEXT("Idle current operation is null"), Idle.Data.IsValid());

	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		 MakeKeyPress(1, 0.05, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W)}, true))
	{
		return false;
	}

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	TestTrue(TEXT("AI replay admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);
	TestTrue(TEXT("Accepted run has a canonical run id"), RunId.IsValid());
	TestEqual(TEXT("Acceptance reports Preparing"), RunState(Started), FString(TEXT("Preparing")));
	TestTrue(TEXT("Admitted record is in use"), Service->IsRecordInUse(1));

	const FCortexCommandResult Busy = Service->StartReplay(1, ECortexReplayOrigin::AI);
	TestFalse(TEXT("Competing start is busy while a run is active"), Busy.bSuccess);
	TestEqual(TEXT("Busy rejection uses EDITOR_BUSY"), Busy.ErrorCode,
		FString(CortexErrorCodes::EditorBusy));

	const FCortexCommandResult Cancelled = Service->CancelReplay(RunId, true);
	TestTrue(TEXT("Admitted run can be cancelled"), Cancelled.bSuccess);
	const FString FirstCancelState = RunState(Cancelled);
	const FCortexCommandResult CancelledAgain = Service->CancelReplay(RunId, true);
	TestTrue(TEXT("Repeated cancellation of the same run is stable"), CancelledAgain.bSuccess);
	TestEqual(TEXT("Repeated cancellation does not change the terminal state"),
		RunState(CancelledAgain), FirstCancelState);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunTerminal(this, Service, RunId, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, RunId](FAutomationTestBase& T)
		{
			const FCortexCommandResult First = Service->GetRun(RunId, true);
			const FCortexCommandResult Second = Service->GetRun(RunId, true);
			T.TestTrue(TEXT("Terminal run is queryable"), First.bSuccess && Second.bSuccess);
			T.TestEqual(TEXT("Terminal state is Cancelled"), RunState(First), FString(TEXT("Cancelled")));
			T.TestEqual(TEXT("Repeated status reads are identical"),
				RunState(Second), RunState(First));
			T.TestEqual(TEXT("Dispatched count is stable across reads"),
				RunDispatchedEvents(Second), RunDispatchedEvents(First));
			const FCortexCommandResult Last = Service->GetLastRunForRecording(1);
			T.TestTrue(TEXT("Retained terminal summary exists"), Last.bSuccess && Last.Data.IsValid());
			if (Last.Data.IsValid())
			{
				T.TestEqual(TEXT("Retained summary is the admitted run"),
					Last.Data->GetStringField(TEXT("run_id")),
					RunId.ToString(EGuidFormats::DigitsWithHyphens).ToLower());
				T.TestEqual(TEXT("Retained summary state"), Last.Data->GetStringField(TEXT("state")),
					FString(TEXT("Cancelled")));
			}
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			T.TestFalse(TEXT("No record remains in use after finalization"),
				Service->IsRecordInUse(1));
			const FCortexCommandResult IdleAfter = Service->GetCurrentOperation();
			T.TestTrue(TEXT("get_current_operation succeeds after finalization"), IdleAfter.bSuccess);
			T.TestFalse(TEXT("No active operation remains"), IdleAfter.Data.IsValid());
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// While the owned context exists, finalization is observable and stable.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleFinalizationTest,
	"Cortex.Replay.Lifecycle.FinalizationObservesOwnedTeardown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleFinalizationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	// A replay window wide enough that the cancel command reliably observes Replaying
	// before it cancels (the pre-fix 0.05s recording could complete between frames).
	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		 MakeKeyPress(1, 0.25, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W)}, true))
	{
		return false;
	}

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	TestTrue(TEXT("AI replay admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	// The run must actually reach playback before we cancel, so finalization has an owned
	// context to observe.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Replaying"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, RunId](FAutomationTestBase& T)
		{
			const FCortexCommandResult Cancel = Service->CancelReplay(RunId, true);
			T.TestTrue(TEXT("Active run cancelled"), Cancel.bSuccess);
		}, Fixture));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayObserveFinalizing(this, Service, RunId, MapPath, 1, Fixture));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, RunId](FAutomationTestBase& T)
		{
			const FCortexCommandResult Run = Service->GetRun(RunId, true);
			T.TestTrue(TEXT("Terminal result queryable after teardown"), Run.bSuccess);
			T.TestEqual(TEXT("Cancelled run stays Cancelled"), RunState(Run),
				FString(TEXT("Cancelled")));
			// A successor may now be admitted.
			const FCortexCommandResult Successor = Service->StartReplay(1, ECortexReplayOrigin::AI);
			T.TestTrue(TEXT("Successor may start after teardown"), Successor.bSuccess);
			if (Successor.bSuccess)
			{
				const FGuid SuccessorId = ParseRunId(Successor);
				Service->CancelReplay(SuccessorId, true);
			}
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Admission digests are immutable across external library edits and service restart.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleImmutabilityTest,
	"Cortex.Replay.Lifecycle.SnapshotImmutabilityAfterExternalEdit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleImmutabilityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		 MakeKeyPress(1, 0.25, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W)}, true))
	{
		return false;
	}

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	TestTrue(TEXT("AI replay admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);
	const FCortexCommandResult Admitted = Service->GetRun(RunId, true);
	TestTrue(TEXT("Admitted run is queryable"), Admitted.bSuccess);
	const FString AdmittedSnapshot = Admitted.Data.IsValid()
		? Admitted.Data->GetStringField(TEXT("recording_snapshot_sha256")) : FString();
	const FString AdmittedInitial = Admitted.Data.IsValid()
		? Admitted.Data->GetStringField(TEXT("initial_state_sha256")) : FString();
	const FString AdmittedInputs = Admitted.Data.IsValid()
		? Admitted.Data->GetStringField(TEXT("inputs_sha256")) : FString();
	TestFalse(TEXT("Admitted snapshot digest is exposed"), AdmittedSnapshot.IsEmpty());
	TestFalse(TEXT("Admitted initial digest is exposed"), AdmittedInitial.IsEmpty());
	TestFalse(TEXT("Admitted inputs digest is exposed"), AdmittedInputs.IsEmpty());

	// The payload is replaced with a different *valid* payload (fewer events) while the run is
	// still active, so the regression proves dispatch uses the admitted snapshot rather than
	// re-reading the library mid-flight.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Replaying"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Fixture](FAutomationTestBase& T)
		{
			const FString RecordDirectory = FPaths::Combine(
				FPaths::Combine(Fixture->GetProjectRoot(), TEXT(".cortex/replay/recordings")),
				FString::FromInt(1));
			const FString InputsPath = FPaths::Combine(RecordDirectory, TEXT("inputs.jsonl"));
			FString Original;
			T.TestTrue(TEXT("Admitted inputs payload is readable"),
				FFileHelper::LoadFileToString(Original, *InputsPath));
			FString FirstLine;
			T.TestTrue(TEXT("Admitted payload has a first event line"),
				Original.Split(TEXT("\n"), &FirstLine, nullptr) && !FirstLine.IsEmpty());
			T.TestTrue(TEXT("Active-run payload replaced with a different valid payload"),
				FFileHelper::SaveStringToFile(FirstLine + TEXT("\n"), *InputsPath));
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunTerminal(this, Service, RunId, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, RunId](FAutomationTestBase& T)
		{
			// The admitted two-event snapshot must still have driven the whole run.
			const FCortexCommandResult Run = Service->GetRun(RunId, true);
			T.TestTrue(TEXT("Completed run is queryable"), Run.bSuccess && Run.Data.IsValid());
			T.TestEqual(TEXT("Playback completed from the admitted snapshot"),
				RunState(Run), FString(TEXT("Completed")));
			T.TestEqual(TEXT("Every admitted event was dispatched"), RunDispatchedEvents(Run), 2);
			if (Run.Data.IsValid())
			{
				T.TestEqual(TEXT("Admitted event count retained, not the tampered payload"),
					static_cast<int32>(Run.Data->GetNumberField(TEXT("total_events"))), 2);
			}
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Fixture, RunId, AdmittedSnapshot, AdmittedInitial, AdmittedInputs](FAutomationTestBase& T)
		{
			FCortexReplayService Reloaded(Fixture->GetProjectRoot());
			const FCortexCommandResult Retained = Reloaded.GetRun(RunId, true);
			T.TestTrue(TEXT("Retained run reloads after restart"), Retained.bSuccess);
			if (Retained.Data.IsValid())
			{
				T.TestEqual(TEXT("Retained snapshot digest is the admitted one"),
					Retained.Data->GetStringField(TEXT("recording_snapshot_sha256")),
					AdmittedSnapshot);
				T.TestEqual(TEXT("Retained initial digest is the admitted one"),
					Retained.Data->GetStringField(TEXT("initial_state_sha256")), AdmittedInitial);
				T.TestEqual(TEXT("Retained inputs digest is the admitted one"),
					Retained.Data->GetStringField(TEXT("inputs_sha256")), AdmittedInputs);
			}
			const FCortexCommandResult Last = Reloaded.GetLastRunForRecording(1);
			T.TestTrue(TEXT("Retained terminal summary reloads"), Last.bSuccess && Last.Data.IsValid());
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// External PIE loss interrupts the run and yields one stable terminal result.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecyclePieLossTest,
	"Cortex.Replay.Lifecycle.PieLossInterruptionStableTerminal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecyclePieLossTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		 MakeKeyPress(1, 0.25, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W)}, true))
	{
		return false;
	}

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	TestTrue(TEXT("AI replay admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Replaying"), Fixture));
	// Target/PIE loss is an interruption, not an error.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[](FAutomationTestBase&)
		{
			if (GEditor) { GEditor->RequestEndPlayMap(); }
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunTerminal(this, Service, RunId, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, RunId](FAutomationTestBase& T)
		{
			const FCortexCommandResult Run = Service->GetRun(RunId, true);
			T.TestTrue(TEXT("Interrupted run is queryable"), Run.bSuccess);
			T.TestEqual(TEXT("Uncontrolled PIE loss interrupts rather than errors"),
				RunState(Run), FString(TEXT("Interrupted")));
			const FCortexCommandResult Again = Service->CancelReplay(RunId, true);
			T.TestTrue(TEXT("Cancelling an already-terminal run succeeds"), Again.bSuccess);
			T.TestEqual(TEXT("Repeated terminal cancellation preserves the result"),
				RunState(Again), FString(TEXT("Interrupted")));
			T.TestFalse(TEXT("Interrupted run no longer owns the record"),
				Service->IsRecordInUse(1));
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Revocation after a prior interruption preserves the first terminal result.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleRevocationTest,
	"Cortex.Replay.Lifecycle.RevocationAfterInterruptionPreservesTerminal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleRevocationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		 MakeKeyPress(1, 0.25, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W)}, true))
	{
		return false;
	}

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	TestTrue(TEXT("AI replay admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Replaying"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[](FAutomationTestBase&)
		{
			if (GEditor) { GEditor->RequestEndPlayMap(); }
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunTerminal(this, Service, RunId, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, RunId](FAutomationTestBase& T)
		{
			const FCortexCommandResult Before = Service->GetRun(RunId, true);
			T.TestEqual(TEXT("First terminal result is Interrupted"), RunState(Before),
				FString(TEXT("Interrupted")));

			// Revoking permission after the run already ended must not overwrite its result.
			const FCortexCommandResult Revoked = Service->SaveMetadata(1, TEXT("Revoked"),
				TEXT("ai disabled"), false);
			T.TestTrue(TEXT("Human metadata save revokes permission"), Revoked.bSuccess);

			const FCortexCommandResult After = Service->GetRun(RunId, true);
			T.TestEqual(TEXT("Revocation preserves the first terminal result"),
				RunState(After), FString(TEXT("Interrupted")));
			const FCortexCommandResult Last = Service->GetLastRunForRecording(1);
			T.TestTrue(TEXT("Retained summary still exists"), Last.bSuccess && Last.Data.IsValid());
			if (Last.Data.IsValid())
			{
				T.TestEqual(TEXT("Retained summary remains Interrupted"),
					Last.Data->GetStringField(TEXT("state")), FString(TEXT("Interrupted")));
			}

			const FCortexCommandResult Denied = Service->StartReplay(1, ECortexReplayOrigin::AI);
			T.TestFalse(TEXT("Revoked recording denies a new AI start"), Denied.bSuccess);
			T.TestEqual(TEXT("Denial uses PERMISSION_DENIED"), Denied.ErrorCode,
				FString(CortexReplayErrorCodes::PermissionDenied));
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Destroying the backend owner ends the owned run and neutralizes its PIE.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleShutdownTest,
	"Cortex.Replay.Lifecycle.ModuleShutdownNeutralizesOwnedRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleShutdownTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	// A recording whose replay stays in flight long enough to observe Replaying and hold a key
	// down while the backend owner is shut down.
	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		 MakeKeyPress(1, 1.0, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W)}, true))
	{
		return false;
	}

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	TestTrue(TEXT("AI replay admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Replaying"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase&)
		{
			// Destroying the actual backend owner must invoke shutdown cleanup; dropping a
			// client reference must not.
			Service->Shutdown();
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			// The reference is still held, so lifetime alone cannot have manufactured teardown.
			T.TestNotNull(TEXT("Backend owner reference still valid"), &Service.Get());
			T.TestTrue(TEXT("No owned PIE remains after backend shutdown"),
				!IsPIEWorldPresentForMap(TEXT("/Game/Maps/TestMap"))
				&& !IsPIEWorldPresentForMap(TEXT("/Game/Maps/TestRoomMap")));
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Borrowed human capture: Stop detaches without ending the human PIE session.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleBorrowedCaptureTest,
	"Cortex.Replay.Lifecycle.BorrowedCaptureStopKeepsHumanPie",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleBorrowedCaptureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitExternalPiePlaying(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
			T.TestNotNull(TEXT("External PIE world exists"), PlayWorld);

			TArray<FCortexReplayCaptureTargetChoice> Choices;
			const FCortexCommandResult Enumerated = Service->EnumerateHumanCaptureTargets(Choices);
			T.TestTrue(TEXT("Human capture target enumeration succeeds"), Enumerated.bSuccess);
			const FCortexReplayCaptureTargetChoice* Selected = nullptr;
			for (const FCortexReplayCaptureTargetChoice& Choice : Choices)
			{
				if (Choice.World.Get() == PlayWorld)
				{
					Selected = &Choice;
					break;
				}
			}
			T.TestNotNull(TEXT("Enumeration never picks an unrelated first world"), Selected);
			if (Selected == nullptr || PlayWorld == nullptr)
			{
				return;
			}
			const FCortexCommandResult Borrowed =
				Service->StartCaptureAtTarget(*PlayWorld, Selected->LocalPlayerIndex);
			T.TestTrue(TEXT("Borrowed capture admitted"), Borrowed.bSuccess);
			const FCortexCommandResult Active = Service->GetCurrentOperation();
			T.TestTrue(TEXT("Borrowed capture is the active operation"),
				Active.bSuccess && Active.Data.IsValid());

			// A focus change needed for human Stop/Edit is not itself capture interference.
			if (FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().ClearUserFocus(Selected->LocalPlayerIndex);
			}
			const FCortexCommandResult Stopped = Service->StopCapture(false);
			T.TestTrue(TEXT("Borrowed capture stops normally"), Stopped.bSuccess);
			T.TestTrue(TEXT("Borrowed human PIE still runs after Stop"),
				GEditor && GEditor->PlayWorld == PlayWorld);
			T.TestFalse(TEXT("Borrowed capture released the record"), Service->IsRecordInUse(1));

			const FCortexCommandResult Idle = Service->GetCurrentOperation();
			T.TestTrue(TEXT("get_current_operation succeeds after Stop"), Idle.bSuccess);
			T.TestFalse(TEXT("No operation remains after borrowed Stop"), Idle.Data.IsValid());
		}, Fixture));

	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// The production preparation deadline fails an owned replay with a missing pawn.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleTimeoutPossessionTest,
	"Cortex.Replay.Lifecycle.PreparationTimeoutPossessionGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleTimeoutPossessionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W)}, true))
	{
		return false;
	}

	// The baseline must be captured before admission, so only the owned PIE world is gated.
	const TSharedRef<FReplayPreparationGate> Gate =
		MakeShared<FReplayPreparationGate>(EReplayPreparationGateMode::Possession, MapPath);
	Gate->CaptureBaselineWorlds();

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	TestTrue(TEXT("AI replay admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayGatePreparationTimeout(this, Service, RunId, 1, Gate, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// The production preparation deadline fails an owned replay with an unregistered viewport.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleTimeoutViewportTest,
	"Cortex.Replay.Lifecycle.PreparationTimeoutViewportGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleTimeoutViewportTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W)}, true))
	{
		return false;
	}

	// The baseline must be captured before admission, so only the owned PIE world is gated.
	const TSharedRef<FReplayPreparationGate> Gate =
		MakeShared<FReplayPreparationGate>(EReplayPreparationGateMode::Viewport, MapPath);
	Gate->CaptureBaselineWorlds();

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	TestTrue(TEXT("AI replay admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayGatePreparationTimeout(this, Service, RunId, 1, Gate, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// A failed metadata write preserves the committed permission.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleMetadataWriteFailureTest,
	"Cortex.Replay.Lifecycle.MetadataWriteFailurePreservesPermission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleMetadataWriteFailureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const FString MapPath = TEXT("/Game/Maps/TestMap");

	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W)}, false))
	{
		return false;
	}

	const FString MetadataPath = FPaths::Combine(
		FPaths::Combine(
			FPaths::Combine(Fixture->GetProjectRoot(), TEXT(".cortex/replay/recordings")),
			FString::FromInt(1)),
		TEXT("metadata.json"));
	TestTrue(TEXT("Metadata file exists"), IFileManager::Get().FileExists(*MetadataPath));

	FString Before;
	TestTrue(TEXT("Original metadata readable"), FFileHelper::LoadFileToString(Before, *MetadataPath));
	TestTrue(TEXT("Metadata made read-only for the failure case"),
		FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*MetadataPath, true));

	FCortexReplayService Service(Fixture->GetProjectRoot());
	const FCortexCommandResult Denied = Service.SaveMetadata(1, TEXT("Renamed"),
		TEXT("should not commit"), true);
	TestFalse(TEXT("Metadata write failure is rejected"), Denied.bSuccess);

	// Restore write access and verify the committed permission is untouched.
	FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*MetadataPath, false);
	FString After;
	TestTrue(TEXT("Metadata readable after the failed write"),
		FFileHelper::LoadFileToString(After, *MetadataPath));
	TestEqual(TEXT("Failed write left the metadata bytes unchanged"), After, Before);

	FCortexReplayLibrary Library(Fixture->GetProjectRoot());
	TArray<FCortexReplayMetadata> All;
	TestTrue(TEXT("Library list succeeds"), Library.List(false, All).bSuccess);
	const FCortexReplayMetadata* Found = nullptr;
	for (const FCortexReplayMetadata& Metadata : All)
	{
		if (Metadata.RecordingId == 1)
		{
			Found = &Metadata;
			break;
		}
	}
	TestNotNull(TEXT("Recording remains listed"), Found);
	if (Found != nullptr)
	{
		TestFalse(TEXT("Permission stayed revoked/denied after the failed write"),
			Found->bAIEnabled);
		TestEqual(TEXT("Name unchanged after the failed write"),
			Found->Name, FString(TEXT("Recording 1")));
	}

	return true;
}

// ---------------------------------------------------------------------------
// An external revocation during trailing idle must cancel instead of completing.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleTrailingRevocationTest,
	"Cortex.Replay.Lifecycle.RevocationDuringTrailingIdleCancels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleTrailingRevocationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	// A long recorded duration leaves the run idle after its last event, which is exactly where an
	// external revocation must still be observed before completion is claimed.
	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		 MakeKeyPress(1, 0.05, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W)}, true,
		1.0 /* duration override: trailing idle */))
	{
		return false;
	}

	const FCortexCommandResult Started = Service->StartReplay(1, ECortexReplayOrigin::AI);
	TestTrue(TEXT("AI replay admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Replaying"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitDispatchedEvents(this, Service, RunId, 2, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Fixture](FAutomationTestBase& T)
		{
			// An external (out-of-process style) revocation while the run is only trailing idle.
			T.TestTrue(TEXT("Live permission revoked externally"),
				SetMetadataAIEnabled(ReplayMetadataPath(*Fixture, 1), false));
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Service, RunId,
		TEXT("Cancelled"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, RunId](FAutomationTestBase& T)
		{
			const FCortexCommandResult Run = Service->GetRun(RunId, true);
			T.TestTrue(TEXT("Revoked run is queryable"), Run.bSuccess);
			T.TestEqual(TEXT("Revocation during trailing idle cancels instead of completing"),
				RunState(Run), FString(TEXT("Cancelled")));
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Malformed retained terminal records are never indexed.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleMalformedRecordTest,
	"Cortex.Replay.Lifecycle.MalformedRetainedRecordRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleMalformedRecordTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const FString RunsRoot = FPaths::Combine(Fixture->GetProjectRoot(), TEXT("Saved/CortexReplay/Runs"));
	TestTrue(TEXT("Run store directory created"), IFileManager::Get().MakeDirectory(*RunsRoot, true));

	// Control: a consistent terminal record must be indexed.
	const FGuid ValidId = FGuid::NewGuid();
	TestTrue(TEXT("Valid retained record written"),
		SaveJsonObjectFile(ReplayRunRecordPath(*Fixture, ValidId),
			MakeRetainedRunRecordJson(ValidId, 1, TEXT("Cancelled"), 2, 2,
				1, 1, 0, 0)));

	// Malformed: the three UI counts sum far beyond int32, wrapping to the declared pose count in a
	// 32-bit sum (1500000000 * 3 = 4500000000 -> 205032704 in int32). An int64 partition rejects it.
	const FGuid MalformedId = FGuid::NewGuid();
	TestTrue(TEXT("Malformed retained record written"),
		SaveJsonObjectFile(ReplayRunRecordPath(*Fixture, MalformedId),
			MakeRetainedRunRecordJson(MalformedId, 1, TEXT("Cancelled"), 0, MAX_int32,
				205032704, 1500000000, 1500000000, 1500000000)));

	FCortexReplayService Service(Fixture->GetProjectRoot());
	const FCortexCommandResult Valid = Service.GetRun(ValidId, true);
	TestTrue(TEXT("Consistent retained record is indexed"), Valid.bSuccess);
	TestEqual(TEXT("Consistent record state"), RunState(Valid), FString(TEXT("Cancelled")));

	const FCortexCommandResult Malformed = Service.GetRun(MalformedId, true);
	TestFalse(TEXT("Malformed retained record is rejected"), Malformed.bSuccess);
	TestEqual(TEXT("Malformed record reports RUN_NOT_FOUND"), Malformed.ErrorCode,
		FString(CortexReplayErrorCodes::RunNotFound));

	return true;
}

// ---------------------------------------------------------------------------
// The lifetime-owned finalization backend outlives the destroyed service object.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleDestructionTest,
	"Cortex.Replay.Lifecycle.FinalizationSurvivesServiceDestruction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleDestructionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	// The holder lets one command drop the last client reference while the run is only Finalizing.
	const TSharedRef<TSharedPtr<FCortexReplayService>> Holder =
		MakeShared<TSharedPtr<FCortexReplayService>>();
	*Holder = MakeShared<FCortexReplayService>(Fixture->GetProjectRoot());
	const FString MapPath = MakeReplayMapAssetPath();

	if (!PublishReplayRecording(*this, *Fixture, 1, MapPath,
		{MakeKeyPress(0, 0.0, ECortexEditorPhysicalInputKind::KeyDown, EKeys::W),
		 MakeKeyPress(1, 0.25, ECortexEditorPhysicalInputKind::KeyUp, EKeys::W)}, true))
	{
		return false;
	}

	const FCortexCommandResult Started = (*Holder)->StartReplay(1, ECortexReplayOrigin::AI);
	TestTrue(TEXT("AI replay admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}
	const FGuid RunId = ParseRunId(Started);

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRunState(this, Holder->ToSharedRef(), RunId,
		TEXT("Replaying"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Holder, RunId](FAutomationTestBase& T)
		{
			T.TestTrue(TEXT("Cancellation accepted"), (*Holder)->CancelReplay(RunId, true).bSuccess);
			// Destroy the owning service while the owned teardown is still asynchronous.
			Holder->Reset();
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitRetainedRun(this, Fixture, RunId,
		TEXT("Cancelled"), Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// A failed owned/borrowed capture publication is retained and retried, not silently completed.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleCapturePublishFailureTest,
	"Cortex.Replay.Lifecycle.CapturePublicationFailureRetained",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleCapturePublishFailureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);

	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitExternalPiePlaying(this, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, Fixture](FAutomationTestBase& T)
		{
			UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
			T.TestNotNull(TEXT("External PIE world exists"), PlayWorld);

			TArray<FCortexReplayCaptureTargetChoice> Choices;
			T.TestTrue(TEXT("Human capture target enumeration succeeds"),
				Service->EnumerateHumanCaptureTargets(Choices).bSuccess);
			const FCortexReplayCaptureTargetChoice* Selected = nullptr;
			for (const FCortexReplayCaptureTargetChoice& Choice : Choices)
			{
				if (Choice.World.Get() == PlayWorld) { Selected = &Choice; break; }
			}
			T.TestNotNull(TEXT("Matching capture target enumerated"), Selected);
			if (Selected == nullptr || PlayWorld == nullptr) { return; }
			T.TestTrue(TEXT("Borrowed capture admitted"),
				Service->StartCaptureAtTarget(*PlayWorld, Selected->LocalPlayerIndex).bSuccess);

			// Occupy the canonical publication path with a file so Publish must fail.
			const FString RecordingsRoot = FPaths::Combine(Fixture->GetProjectRoot(),
				TEXT(".cortex/replay/recordings"));
			T.TestTrue(TEXT("Recordings root created"),
				IFileManager::Get().MakeDirectory(*RecordingsRoot, true));
			const FString BlockedPath = FPaths::Combine(RecordingsRoot, TEXT("1"));
			T.TestTrue(TEXT("Publication path blocked"),
				FFileHelper::SaveStringToFile(TEXT("blocked"), *BlockedPath));

			const FCortexCommandResult Stopped = Service->StopCapture(false);
			T.TestFalse(TEXT("Publication failure is reported to the caller"), Stopped.bSuccess);

			const FCortexCommandResult Active = Service->GetCurrentOperation();
			T.TestTrue(TEXT("Failed capture remains queryable"), Active.bSuccess && Active.Data.IsValid());
			if (Active.Data.IsValid())
			{
				T.TestEqual(TEXT("Failed capture keeps its reserved recording id"),
					static_cast<int32>(Active.Data->GetNumberField(TEXT("recording_id"))), 1);
				T.TestEqual(TEXT("Failed capture stays Finalizing"),
					Active.Data->GetStringField(TEXT("state")), FString(TEXT("Finalizing")));
			}
			T.TestTrue(TEXT("Repository still holds the failed recording"),
				Service->IsRecordInUse(1));

			// Allow the retained publication to be retried successfully.
			T.TestTrue(TEXT("Publication path unblocked"),
				IFileManager::Get().Delete(*BlockedPath, false, true, true));
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			const FCortexCommandResult Idle = Service->GetCurrentOperation();
			T.TestTrue(TEXT("get_current_operation succeeds after the retry"), Idle.bSuccess);
			T.TestFalse(TEXT("Capture released once its publication succeeded"), Idle.Data.IsValid());
			T.TestFalse(TEXT("Failed capture no longer owns the record"), Service->IsRecordInUse(1));
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// An owned capture whose asynchronous publication fails keeps the failure observable and retries.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleOwnedCapturePublishFailureTest,
	"Cortex.Replay.Lifecycle.OwnedCapturePublicationFailureRetained",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleOwnedCapturePublishFailureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	// Occupy the canonical publication path of the first reserved recording id with a file, so the
	// owned capture's teardown-time publication must fail.
	const FString RecordingsRoot = FPaths::Combine(Fixture->GetProjectRoot(),
		TEXT(".cortex/replay/recordings"));
	TestTrue(TEXT("Recordings root created"),
		IFileManager::Get().MakeDirectory(*RecordingsRoot, true));
	const FString BlockedPath = FPaths::Combine(RecordingsRoot, TEXT("1"));
	TestTrue(TEXT("Publication path blocked"),
		FFileHelper::SaveStringToFile(TEXT("blocked"), *BlockedPath));

	const FCortexCommandResult Started = Service->StartCapture(MapPath);
	TestTrue(TEXT("Owned capture admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}

	// The owned capture becomes Recording only after readiness, pose read and neutral-state arming.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCapturePhase(this, Service,
		TEXT("Recording"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			// The stop is accepted and the owned teardown/publication is still pending.
			T.TestTrue(TEXT("Owned capture stop accepted"), Service->StopCapture(false).bSuccess);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCapturePublicationFailure(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, Fixture, BlockedPath](FAutomationTestBase& T)
		{
			// While retries are pending the retained failure is observable through the status.
			FString RetainedCode;
			const FCortexCommandResult Active = Service->GetCurrentOperation();
			T.TestTrue(TEXT("Failed owned capture remains queryable"),
				Active.bSuccess && Active.Data.IsValid());
			if (Active.Data.IsValid())
			{
				T.TestEqual(TEXT("Owned capture keeps its reserved recording id"),
					static_cast<int32>(Active.Data->GetNumberField(TEXT("recording_id"))), 1);
				T.TestEqual(TEXT("Owned capture stays Finalizing"),
					Active.Data->GetStringField(TEXT("state")), FString(TEXT("Finalizing")));
				T.TestTrue(TEXT("Publication failure is reported"),
					Active.Data->GetBoolField(TEXT("publication_failed")));
				const TSharedPtr<FJsonObject>* PublicationError = nullptr;
				T.TestTrue(TEXT("Retained publication error exposed"),
					Active.Data->TryGetObjectField(TEXT("publication_error"), PublicationError)
					&& PublicationError != nullptr && PublicationError->IsValid());
				if (PublicationError != nullptr && PublicationError->IsValid())
				{
					RetainedCode = (*PublicationError)->GetStringField(TEXT("code"));
					T.TestFalse(TEXT("Publication error carries a code"), RetainedCode.IsEmpty());
				}
			}
			T.TestTrue(TEXT("Failed owned capture still owns the record"), Service->IsRecordInUse(1));

			// A subsequent stop returns the retained asynchronous publication failure instead of
			// the "No capture is active" Finalizing rejection.
			const FCortexCommandResult RetainedStop = Service->StopCapture(false);
			T.TestFalse(TEXT("Subsequent stop reports the retained publication failure"),
				RetainedStop.bSuccess);
			T.TestEqual(TEXT("Retained stop failure carries the published error code"),
				RetainedStop.ErrorCode, RetainedCode);

			// Let the retained publication retry succeed.
			T.TestTrue(TEXT("Publication path unblocked"),
				IFileManager::Get().Delete(*BlockedPath, false, true, true));
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, Fixture](FAutomationTestBase& T)
		{
			const FCortexCommandResult Idle = Service->GetCurrentOperation();
			T.TestTrue(TEXT("get_current_operation succeeds after the retry"), Idle.bSuccess);
			T.TestFalse(TEXT("Owned capture released once its publication succeeded"), Idle.Data.IsValid());
			T.TestFalse(TEXT("Retried capture no longer owns the record"), Service->IsRecordInUse(1));

			// The retried recording is now a normal loadable, listed recording.
			FCortexReplayLibrary Library(Fixture->GetProjectRoot());
			TArray<FCortexReplayMetadata> All;
			T.TestTrue(TEXT("Library lists the retried capture"), Library.List(false, All).bSuccess);
			bool bListed = false;
			for (const FCortexReplayMetadata& Metadata : All)
			{
				if (Metadata.RecordingId == 1) { bListed = true; break; }
			}
			T.TestTrue(TEXT("Retried capture is listed"), bListed);
			const FCortexCommandResult Recording = Service->GetRecording(1, false);
			T.TestTrue(TEXT("Retried capture loads"), Recording.bSuccess);
			if (Recording.Data.IsValid())
			{
				T.TestEqual(TEXT("Retried capture is complete"),
					Recording.Data->GetBoolField(TEXT("complete")), true);
			}
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// CR-06: a required pre-press pose that cannot be read at the press is a capture fault. Even
// though the pose recovers before the next service tick, the recording must not be published as
// complete with an invented (initial-pose) guard.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleCaptureRequiredPoseFaultTest,
	"Cortex.Replay.Lifecycle.CaptureRequiredPoseFaultWithheld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleCaptureRequiredPoseFaultTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	const FCortexCommandResult Started = Service->StartCapture(MapPath);
	TestTrue(TEXT("Owned capture admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCapturePhase(this, Service, TEXT("Recording"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
			T.TestNotNull(TEXT("Owned capture PIE world exists"), PlayWorld);
			if (!PlayWorld || !FSlateApplication::IsInitialized()) { return; }
			APlayerController* Controller = PlayWorld->GetFirstPlayerController();
			T.TestNotNull(TEXT("Owned capture controller exists"), Controller);
			if (!Controller) { return; }
			APawn* OriginalPawn = Controller->GetPawn();
			T.TestNotNull(TEXT("Owned capture pawn exists"), OriginalPawn);
			if (!OriginalPawn) { return; }

			ULocalPlayer* LocalPlayer = PlayWorld->GetGameInstance()
				? PlayWorld->GetGameInstance()->GetLocalPlayerByIndex(0) : nullptr;
			const TSharedPtr<FSlateUser> SlateUser = LocalPlayer ? LocalPlayer->GetSlateUser() : nullptr;
			UGameViewportClient* ViewportClient = PlayWorld->GetGameViewport();
			FSceneViewport* SceneViewport = ViewportClient ? ViewportClient->GetGameViewport() : nullptr;
			const TSharedPtr<SViewport> ViewportWidget =
				SceneViewport ? SceneViewport->GetViewportWidget().Pin() : nullptr;
			T.TestTrue(TEXT("Owned capture viewport widget exists"), ViewportWidget.IsValid());
			if (!SlateUser.IsValid() || !ViewportWidget.IsValid()) { return; }
			const FGeometry Geometry = ViewportWidget->GetCachedGeometry();
			const FVector2D ViewportSize = Geometry.GetLocalSize();
			if (ViewportSize.X <= 0.0 || ViewportSize.Y <= 0.0) { return; }
			const FVector2D Center =
				Geometry.LocalToAbsolute(FVector2D(ViewportSize.X * 0.5, ViewportSize.Y * 0.5));
			const FInputDeviceId Device = IPlatformInputDeviceMapper::Get()
				.GetPrimaryInputDeviceForUser(SlateUser->GetPlatformUserId());
			const int32 UserIndex = SlateUser->GetUserIndex();

			// The required pre-press pose is unreadable exactly at the press: the selected target's
			// pawn is transiently unbound (a state that does not destroy the target or trigger an
			// engine fault) and restored before the next service tick. A real pointer press at the
			// selected viewport centre is used so capture admission does not depend on editor
			// keyboard focus.
			Controller->UnPossess();

			// Slate routes device input only while the application is active unless inactive-input
			// handling is enabled (the session's dispatch does the same); automation drives input
			// while the editor window may not hold OS focus.
			const bool bSavedInactiveInputHandling =
				FSlateApplication::Get().GetHandleDeviceInputWhenApplicationNotActive();
			FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(true);

			const FModifierKeysState Modifiers;
			TSet<FKey> Pressed;
			Pressed.Add(EKeys::LeftMouseButton);
			FSlateApplication::Get().ProcessMouseButtonDownEvent(nullptr,
				FPointerEvent(Device, FSlateApplicationBase::CursorPointerIndex, Center, Center,
					Pressed, EKeys::LeftMouseButton, 0.0f, Modifiers, UserIndex));
			Pressed.Reset();
			FSlateApplication::Get().ProcessMouseButtonUpEvent(
				FPointerEvent(Device, FSlateApplicationBase::CursorPointerIndex, Center, Center,
					Pressed, EKeys::LeftMouseButton, 0.0f, Modifiers, UserIndex));

			Controller->Possess(OriginalPawn);
			FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(bSavedInactiveInputHandling);
			T.TestTrue(TEXT("Controller re-possessed the original pawn after the press"),
				Controller->GetPawn() == OriginalPawn);
		}, Fixture));

	// Stop is accepted or already auto-finalized; either way the fault withholds publication.
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase&)
		{
			Service->StopCapture(false);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, Fixture](FAutomationTestBase& T)
		{
			// Later recovery must not legitimize the guard that was never observed: no complete
			// recording is published for the reserved id.
			const FCortexCommandResult Recording = Service->GetRecording(1, false);
			T.TestFalse(TEXT("Faulted capture is not published as complete"), Recording.bSuccess);
			T.TestFalse(TEXT("Faulted capture no longer owns the record"), Service->IsRecordInUse(1));
			FCortexReplayLibrary Library(Fixture->GetProjectRoot());
			TArray<FCortexReplayMetadata> All;
			T.TestTrue(TEXT("Library listing still succeeds"), Library.List(false, All).bSuccess);
			for (const FCortexReplayMetadata& Metadata : All)
			{
				T.TestFalse(TEXT("Library has no recording for the faulted id"), Metadata.RecordingId == 1);
			}
		}, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// CR-07: the persisted immutable provenance version must come from the loaded UnrealCortex
// plugin descriptor, not a second hard-coded release string.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleCaptureProvenanceVersionTest,
	"Cortex.Replay.Lifecycle.CaptureProvenanceMatchesPluginDescriptor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleCaptureProvenanceVersionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	const FCortexCommandResult Started = Service->StartCapture(MapPath);
	TestTrue(TEXT("Owned capture admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCapturePhase(this, Service, TEXT("Recording"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[](FAutomationTestBase&)
		{
			// Record one real key edge through normal Slate routing so the capture is non-empty.
			if (FSlateApplication::IsInitialized())
			{
				const FModifierKeysState Modifiers;
				const uint32 UserIndex = FSlateApplication::Get().GetUserIndexForKeyboard();
				FSlateApplication::Get().ProcessKeyDownEvent(
					FKeyEvent(EKeys::W, Modifiers, UserIndex, false, 0, 0));
				FSlateApplication::Get().ProcessKeyUpEvent(
					FKeyEvent(EKeys::W, Modifiers, UserIndex, false, 0, 0));
			}
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			T.TestTrue(TEXT("Owned capture stop accepted"), Service->StopCapture(false).bSuccess);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			const TSharedPtr<IPlugin> Plugin =
				IPluginManager::Get().FindPlugin(FString(TEXT("UnrealCortex")));
			T.TestTrue(TEXT("UnrealCortex plugin descriptor is available"), Plugin.IsValid());
			const FString DescriptorVersion = Plugin.IsValid()
				? Plugin->GetDescriptor().VersionName : FString();
			T.TestFalse(TEXT("Plugin descriptor declares a version"), DescriptorVersion.IsEmpty());

			const FCortexCommandResult Recording = Service->GetRecording(1, false);
			T.TestTrue(TEXT("Captured recording loads"), Recording.bSuccess);
			if (Recording.Data.IsValid())
			{
				T.TestEqual(TEXT("Persisted provenance equals the plugin descriptor VersionName"),
					Recording.Data->GetStringField(TEXT("plugin_version")), DescriptorVersion);
			}
		}, Fixture));

	return true;
}

namespace
{
/** Asserts no recording was published for a reserved id and that the record is no longer owned. */
void AssertCaptureNotPublished(FAutomationTestBase& T, FCortexReplayService& Service,
	FCortexReplayTestFixture& Fixture, int32 Id)
{
	T.TestFalse(TEXT("Discarded capture is not loadable"),
		Service.GetRecording(Id, false).bSuccess);
	T.TestFalse(TEXT("Discarded capture no longer owns the record"), Service.IsRecordInUse(Id));
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());
	TArray<FCortexReplayMetadata> All;
	T.TestTrue(TEXT("Library listing still succeeds"), Library.List(false, All).bSuccess);
	for (const FCortexReplayMetadata& Metadata : All)
	{
		T.TestFalse(TEXT("Library has no recording for the discarded id"), Metadata.RecordingId == Id);
	}
}
}

// ---------------------------------------------------------------------------
// A human Stop before an owned capture reaches Recording is a discard, and it must say so
// explicitly (the previous behaviour silently returned success and saved nothing).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleCaptureStopBeforeRecordingTest,
	"Cortex.Replay.Lifecycle.CaptureStopBeforeRecordingReportsDiscard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleCaptureStopBeforeRecordingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	const FCortexCommandResult Started = Service->StartCapture(MapPath);
	TestTrue(TEXT("Owned capture admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}

	// Owned readiness is asynchronous, so the capture is still Preparing: the stop discards it.
	const FCortexCommandResult Stopped = Service->StopCapture(false);
	TestFalse(TEXT("Stopping a preparing capture reports a non-publishing outcome"), Stopped.bSuccess);
	TestEqual(TEXT("Discard reuses the module's invalid-operation code"),
		Stopped.ErrorCode, FString(CortexReplayErrorCodes::InvalidOperation));
	TestTrue(TEXT("Discard names the reason it was discarded"),
		Stopped.ErrorMessage.Contains(TEXT("never reached Recording")));
	TestTrue(TEXT("Discard names the Preparing phase"),
		Stopped.ErrorMessage.Contains(TEXT("Preparing")));

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, Fixture](FAutomationTestBase& T)
		{
			AssertCaptureNotPublished(T, *Service, *Fixture, 1);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// A human Stop of a faulted capture reports the fault reason explicitly instead of a silent
// success, and the capture status exposes the same reason while it finalizes.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleCaptureStopFaultedTest,
	"Cortex.Replay.Lifecycle.CaptureStopFaultedReportsReason",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleCaptureStopFaultedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	const FCortexCommandResult Started = Service->StartCapture(MapPath);
	TestTrue(TEXT("Owned capture admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCapturePhase(this, Service, TEXT("Recording"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
			T.TestNotNull(TEXT("Owned capture PIE world exists"), PlayWorld);
			if (!PlayWorld || !FSlateApplication::IsInitialized()) { return; }
			APlayerController* Controller = PlayWorld->GetFirstPlayerController();
			T.TestNotNull(TEXT("Owned capture controller exists"), Controller);
			if (!Controller) { return; }
			APawn* OriginalPawn = Controller->GetPawn();
			T.TestNotNull(TEXT("Owned capture pawn exists"), OriginalPawn);
			if (!OriginalPawn) { return; }

			ULocalPlayer* LocalPlayer = PlayWorld->GetGameInstance()
				? PlayWorld->GetGameInstance()->GetLocalPlayerByIndex(0) : nullptr;
			const TSharedPtr<FSlateUser> SlateUser = LocalPlayer ? LocalPlayer->GetSlateUser() : nullptr;
			UGameViewportClient* ViewportClient = PlayWorld->GetGameViewport();
			FSceneViewport* SceneViewport = ViewportClient ? ViewportClient->GetGameViewport() : nullptr;
			const TSharedPtr<SViewport> ViewportWidget =
				SceneViewport ? SceneViewport->GetViewportWidget().Pin() : nullptr;
			T.TestTrue(TEXT("Owned capture viewport widget exists"), ViewportWidget.IsValid());
			if (!SlateUser.IsValid() || !ViewportWidget.IsValid()) { return; }
			const FGeometry Geometry = ViewportWidget->GetCachedGeometry();
			const FVector2D ViewportSize = Geometry.GetLocalSize();
			if (ViewportSize.X <= 0.0 || ViewportSize.Y <= 0.0) { return; }
			const FVector2D Center =
				Geometry.LocalToAbsolute(FVector2D(ViewportSize.X * 0.5, ViewportSize.Y * 0.5));
			const FInputDeviceId Device = IPlatformInputDeviceMapper::Get()
				.GetPrimaryInputDeviceForUser(SlateUser->GetPlatformUserId());
			const int32 UserIndex = SlateUser->GetUserIndex();

			// Fault the recording synchronously: the required pre-press pose is unreadable at the
			// press (the pawn is transiently unbound and restored before the next service tick).
			Controller->UnPossess();
			const bool bSavedInactiveInputHandling =
				FSlateApplication::Get().GetHandleDeviceInputWhenApplicationNotActive();
			FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(true);

			const FModifierKeysState Modifiers;
			TSet<FKey> Pressed;
			Pressed.Add(EKeys::LeftMouseButton);
			FSlateApplication::Get().ProcessMouseButtonDownEvent(nullptr,
				FPointerEvent(Device, FSlateApplicationBase::CursorPointerIndex, Center, Center,
					Pressed, EKeys::LeftMouseButton, 0.0f, Modifiers, UserIndex));
			Pressed.Reset();
			FSlateApplication::Get().ProcessMouseButtonUpEvent(
				FPointerEvent(Device, FSlateApplicationBase::CursorPointerIndex, Center, Center,
					Pressed, EKeys::LeftMouseButton, 0.0f, Modifiers, UserIndex));

			Controller->Possess(OriginalPawn);
			FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(bSavedInactiveInputHandling);

			// The status exposes the fault reason while the capture is still active.
			FString FaultCode;
			FString FaultMessage;
			const FCortexCommandResult Active = Service->GetCurrentOperation();
			if (Active.bSuccess && Active.Data.IsValid())
			{
				const TSharedPtr<FJsonObject>* FaultError = nullptr;
				if (Active.Data->TryGetObjectField(TEXT("fault_error"), FaultError)
					&& FaultError != nullptr && FaultError->IsValid())
				{
					FaultCode = (*FaultError)->GetStringField(TEXT("code"));
					FaultMessage = (*FaultError)->GetStringField(TEXT("message"));
				}
			}
			T.TestFalse(TEXT("Fault reason is exposed in the capture status"), FaultMessage.IsEmpty());
			T.TestTrue(TEXT("Fault status is flagged"), Active.bSuccess && Active.Data.IsValid()
				&& Active.Data->GetBoolField(TEXT("faulted")));

			// The human stop reports the same reason the status exposes, not a silent success.
			const FCortexCommandResult Stopped = Service->StopCapture(false);
			T.TestFalse(TEXT("Stopping a faulted capture reports a non-publishing outcome"),
				Stopped.bSuccess);
			T.TestEqual(TEXT("Faulted stop reports the exposed fault code"),
				Stopped.ErrorCode, FaultCode);
			T.TestEqual(TEXT("Faulted stop reports the exposed fault reason"),
				Stopped.ErrorMessage, FaultMessage);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service, Fixture](FAutomationTestBase& T)
		{
			AssertCaptureNotPublished(T, *Service, *Fixture, 1);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitNoPieWorlds(this, Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Green control: a normal Stop while the owned capture is Recording still publishes and still
// returns success.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCortexReplayLifecycleCaptureStopDuringRecordingTest,
	"Cortex.Replay.Lifecycle.CaptureStopDuringRecordingPublishes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCortexReplayLifecycleCaptureStopDuringRecordingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!GEditor) { AddError(TEXT("GEditor missing")); return false; }
	const TSharedRef<FCortexReplayTestFixture> Fixture = MakeShared<FCortexReplayTestFixture>();
	const TSharedRef<FCortexReplayService> Service = MakeService(*Fixture);
	const FString MapPath = MakeReplayMapAssetPath();

	const FCortexCommandResult Started = Service->StartCapture(MapPath);
	TestTrue(TEXT("Owned capture admitted"), Started.bSuccess);
	if (!Started.bSuccess)
	{
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCapturePhase(this, Service, TEXT("Recording"), Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[](FAutomationTestBase&)
		{
			if (FSlateApplication::IsInitialized())
			{
				const FModifierKeysState Modifiers;
				const uint32 UserIndex = FSlateApplication::Get().GetUserIndexForKeyboard();
				FSlateApplication::Get().ProcessKeyDownEvent(
					FKeyEvent(EKeys::W, Modifiers, UserIndex, false, 0, 0));
				FSlateApplication::Get().ProcessKeyUpEvent(
					FKeyEvent(EKeys::W, Modifiers, UserIndex, false, 0, 0));
			}
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			T.TestTrue(TEXT("Recording stop is accepted and publishes"),
				Service->StopCapture(false).bSuccess);
		}, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayAwaitCaptureIdle(this, Service, Fixture));
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayRunOnce(this,
		[Service](FAutomationTestBase& T)
		{
			const FCortexCommandResult Recording = Service->GetRecording(1, false);
			T.TestTrue(TEXT("Recording stop published the capture"), Recording.bSuccess);
			if (Recording.Data.IsValid())
			{
				T.TestEqual(TEXT("Published capture is complete"),
					Recording.Data->GetBoolField(TEXT("complete")), true);
			}
			T.TestFalse(TEXT("Published capture no longer owns the record"), Service->IsRecordInUse(1));
		}, Fixture));

	return true;
}
