#include "Misc/AutomationTest.h"

#include "Containers/StringConv.h"
#include "CortexCommandRouter.h"
#include "CortexReplayCommandHandler.h"
#include "CortexReplayErrorCodes.h"
#include "CortexReplayLibrary.h"
#include "CortexReplayService.h"
#include "CortexReplayTestUtils.h"
#include "CortexReplayTypes.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "InputCoreTypes.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Tests/AutomationCommon.h"

namespace
{
/**
 * Native byte budget for one compact list page (plan wire contract). The response must reserve the
 * recovery/paging fields, then include as many full metadata rows as fit without ever trimming a
 * row or dropping a recovery summary.
 */
constexpr int32 ReplayCommandsPageBudgetBytes = 39000;

/** The five exact operations; nothing else is routed. */
const TCHAR* const ReplayCommandsExpectedOperations[] = {
	TEXT("list_recordings"),
	TEXT("get_recording"),
	TEXT("start_replay"),
	TEXT("get_run"),
	TEXT("cancel_replay"),
};

/** Human-only authoring/status names and legacy QA names: none may exist as transport commands. */
const TCHAR* const ReplayCommandsForbiddenOperations[] = {
	TEXT("record"),
	TEXT("stop_recording"),
	TEXT("start_capture"),
	TEXT("stop_capture"),
	TEXT("save_metadata"),
	TEXT("delete_recording"),
	TEXT("grant_permission"),
	TEXT("set_permission"),
	TEXT("list_human_recordings"),
	TEXT("get_current_operation"),
	TEXT("get_last_run_for_recording"),
	TEXT("replay_session"),
	TEXT("cancel_recording"),
	TEXT("start_recording"),
};

/**
 * One fixture root with a lazily started real router. Build the library state (recordings and any
 * retained run records) before StartDomain so the run store's Initialize scan sees the run files.
 */
struct FCortexReplayCommandsHarness
{
	TSharedRef<FCortexReplayTestFixture> Fixture;
	TSharedRef<FCortexReplayLibrary> Library;
	TSharedPtr<FCortexReplayService> Service;
	TSharedPtr<FCortexReplayCommandHandler> Handler;
	TUniquePtr<FCortexCommandRouter> Router;

	FCortexReplayCommandsHarness()
		: Fixture(MakeShared<FCortexReplayTestFixture>())
		, Library(MakeShared<FCortexReplayLibrary>(Fixture->GetProjectRoot()))
	{
	}

	void StartDomain()
	{
		Service = MakeShared<FCortexReplayService>(Fixture->GetProjectRoot());
		Handler = MakeShared<FCortexReplayCommandHandler>(Service);
		Router = MakeUnique<FCortexCommandRouter>();
		Router->RegisterDomain(TEXT("replay"), TEXT("Cortex Replay"), TEXT("1.0.0"), Handler);
	}

	/** A fresh service (re-scans the retained run files) plus a fresh router: an editor restart. */
	void RestartDomain()
	{
		Router.Reset();
		Handler.Reset();
		Service.Reset();
		StartDomain();
	}

	TSharedRef<FCortexReplayService> ServiceRef() const
	{
		return Service.ToSharedRef();
	}

	FCortexCommandResult Execute(const FString& Command, const TSharedPtr<FJsonObject>& Params)
	{
		return Router->Execute(Command, Params);
	}
};

TSharedPtr<FJsonObject> ReplayCommandsParams()
{
	return MakeShared<FJsonObject>();
}

TSharedPtr<FJsonObject> ReplayCommandsRecordingParams(int32 RecordingId)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetNumberField(TEXT("recording_id"), RecordingId);
	return Params;
}

TSharedPtr<FJsonObject> ReplayCommandsRunParams(const FString& RunId)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("run_id"), RunId);
	return Params;
}

FCortexReplayEvent ReplayCommandsKeyDown(int32 Sequence)
{
	FCortexReplayEvent Event;
	Event.Sequence = Sequence;
	Event.TimeSeconds = static_cast<double>(Sequence);
	Event.Input.Kind = ECortexEditorPhysicalInputKind::KeyDown;
	Event.Input.Key = EKeys::E;
	return Event;
}

FString ReplayCommandsWorstCaseText(int32 Scalars)
{
	// A mix of JSON-escaped ASCII (backslash, quote) and 3-byte BMP Unicode so both the escaping
	// policy and UTF-8 expansion are exercised by the real serializer.
	const TCHAR Pattern[] = { TEXT('\\'), TEXT('"'), TEXT('漢') };
	FString Text;
	Text.Reserve(Scalars);
	for (int32 Index = 0; Index < Scalars; ++Index)
	{
		Text.AppendChar(Pattern[Index % 3]);
	}
	return Text;
}

/** Exactly `Scalars` code points mixing 1-, 2-, 3- and 4-byte UTF-8 scalar widths. */
FString ReplayCommandsMixedWidthText(int32 Scalars)
{
	static const TCHAR* const Atoms[] = {
		TEXT("a"), TEXT("\u00e9"), TEXT("\u6f22"), TEXT("\U0001F600")
	};
	FString Text;
	Text.Reserve(Scalars * 2);
	for (int32 Index = 0; Index < Scalars; ++Index)
	{
		Text.Append(Atoms[Index % 4]);
	}
	return Text;
}

bool ReplayCommandsPublish(
	FAutomationTestBase& Test,
	FCortexReplayTestFixture& Fixture,
	FCortexReplayLibrary& Library,
	int32 Id,
	bool bAIEnabled,
	const TArray<FCortexReplayEvent>& Events,
	const FString& Name,
	const FString& Description,
	const FString& MapPath,
	const FString& PawnPath)
{
	FCortexReplaySnapshot Snapshot = Fixture.MakeRecording(Id, bAIEnabled, Events);
	if (!Name.IsEmpty())
	{
		Snapshot.Metadata.Name = Name;
	}
	Snapshot.Metadata.Description = Description;
	if (!MapPath.IsEmpty())
	{
		Snapshot.Metadata.MapAssetPath = MapPath;
	}
	if (!PawnPath.IsEmpty())
	{
		Snapshot.InitialState.PawnClassPath = PawnPath;
		Snapshot.Metadata.Prerequisites.PawnClassPath = PawnPath;
	}
	return Test.TestTrue(
		FString::Printf(TEXT("Recording %d published"), Id),
		Library.Publish(Snapshot).bSuccess);
}

FString ReplayCommandsRunsDirectory(const FCortexReplayTestFixture& Fixture)
{
	return FPaths::Combine(Fixture.GetProjectRoot(), TEXT("Saved/CortexReplay/Runs"));
}

FString ReplayCommandsRunFilePath(const FCortexReplayTestFixture& Fixture, const FGuid& Id)
{
	return FPaths::Combine(ReplayCommandsRunsDirectory(Fixture),
		Id.ToString(EGuidFormats::DigitsWithHyphens).ToLower() + TEXT(".json"));
}

int32 ReplayCommandsRunFileCount(const FCortexReplayTestFixture& Fixture)
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(
		Files, *(ReplayCommandsRunsDirectory(Fixture) / TEXT("*.json")), true, false);
	return Files.Num();
}

/** Builds one valid retained terminal run record with caller-supplied identity and frame progress. */
TSharedRef<FJsonObject> ReplayCommandsRetainedRunJson(
	const FGuid& Id,
	int32 RecordingId,
	const FString& Origin,
	const FString& State,
	const FString& EditorInstanceId,
	const FDateTime& Started,
	const FDateTime& Finalized,
	const FString& SnapshotHash,
	const FString& InitialHash,
	const FString& InputsHash,
	int32 CompletedFrames = 2,
	int32 FrameCount = 2,
	FString FramesSha256 = FString())
{
	if (FramesSha256.IsEmpty())
	{
		FramesSha256 = FString::ChrN(64, TEXT('7'));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("format"), TEXT("CortexReplayRun"));
	Object->SetNumberField(TEXT("schema_version"), 2);
	Object->SetStringField(TEXT("run_id"), Id.ToString(EGuidFormats::DigitsWithHyphens).ToLower());
	Object->SetNumberField(TEXT("recording_id"), RecordingId);
	Object->SetStringField(TEXT("origin"), Origin);
	Object->SetStringField(TEXT("state"), State);
	Object->SetStringField(TEXT("editor_instance_id"), EditorInstanceId);
	Object->SetStringField(TEXT("started_at_utc"), Started.ToIso8601());
	Object->SetStringField(TEXT("finalized_at_utc"), Finalized.ToIso8601());
	Object->SetNumberField(TEXT("dispatched_events"), 1);
	Object->SetNumberField(TEXT("total_events"), 2);
	Object->SetNumberField(TEXT("authorized_wait_seconds"), 0.0);
	Object->SetNumberField(TEXT("completed_frames"), CompletedFrames);
	Object->SetNumberField(TEXT("frame_count"), FrameCount);
	Object->SetStringField(TEXT("frames_sha256"), FramesSha256);
	Object->SetStringField(TEXT("recording_snapshot_sha256"), SnapshotHash);
	Object->SetStringField(TEXT("initial_state_sha256"), InitialHash);
	Object->SetStringField(TEXT("inputs_sha256"), InputsHash);

	TSharedRef<FJsonObject> Coverage = MakeShared<FJsonObject>();
	Coverage->SetNumberField(TEXT("pose_presses"), 1);
	Coverage->SetNumberField(TEXT("ui_supported_presses"), 0);
	Coverage->SetNumberField(TEXT("ui_unavailable_presses"), 0);
	Coverage->SetNumberField(TEXT("ui_not_applicable_presses"), 1);
	Object->SetObjectField(TEXT("guard_coverage"), Coverage);

	if (State == TEXT("Error"))
	{
		TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetStringField(TEXT("code"), TEXT("REPLAY_POSE_GUARD_FAILED"));
		Error->SetStringField(TEXT("message"), TEXT("Recorded press pose no longer matches"));
		TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetNumberField(TEXT("sequence"), 0);
		Details->SetStringField(TEXT("kind"), TEXT("KeyDown"));
		Details->SetNumberField(TEXT("deviation_cm"), 1.0);
		Error->SetObjectField(TEXT("details"), Details);
		Object->SetObjectField(TEXT("execution_error"), Error);
	}
	return Object;
}

/**
 * Builds one pre-frame (schema 1) retained run record. Historical records must keep loading for
 * compact discovery while their full terminal access stays unsupported.
 */
TSharedRef<FJsonObject> ReplayCommandsLegacyRunJson(
	const FGuid& Id,
	int32 RecordingId,
	const FString& Origin,
	const FString& State,
	const FDateTime& Started,
	const FDateTime& Finalized)
{
	TSharedRef<FJsonObject> Object = ReplayCommandsRetainedRunJson(Id, RecordingId, Origin, State,
		TEXT("origin-editor"), Started, Finalized, FString::ChrN(64, TEXT('a')),
		FString::ChrN(64, TEXT('b')), FString::ChrN(64, TEXT('c')));
	Object->SetNumberField(TEXT("schema_version"), 1);
	Object->RemoveField(TEXT("completed_frames"));
	Object->RemoveField(TEXT("frame_count"));
	Object->RemoveField(TEXT("frames_sha256"));
	return Object;
}

bool ReplayCommandsWriteRun(
	const FCortexReplayTestFixture& Fixture, const FGuid& Id, const TSharedRef<FJsonObject>& Object)
{
	IFileManager::Get().MakeDirectory(*ReplayCommandsRunsDirectory(Fixture), true);
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	if (!FJsonSerializer::Serialize(Object, Writer))
	{
		return false;
	}
	return FFileHelper::SaveStringToFile(Text, *ReplayCommandsRunFilePath(Fixture, Id));
}

/** Writes `Count` recent retained terminal AI runs so the recovery baseline is worst-case full. */
TArray<FGuid> ReplayCommandsWriteTerminalRuns(FCortexReplayCommandsHarness& Harness, int32 Count)
{
	TArray<FGuid> Ids;
	Ids.Reserve(Count);
	const FDateTime Base = FDateTime::UtcNow() - FTimespan::FromHours(1);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FGuid RunId = FGuid::NewGuid();
		Ids.Add(RunId);
		ReplayCommandsWriteRun(*Harness.Fixture, RunId, ReplayCommandsRetainedRunJson(
			RunId, 1, TEXT("ai"), TEXT("Cancelled"), TEXT("origin-editor"),
			Base, Base + FTimespan::FromSeconds(Index + 1),
			FString::ChrN(64, TEXT('a')), FString::ChrN(64, TEXT('b')), FString::ChrN(64, TEXT('c'))));
	}
	return Ids;
}

/** Compact UTF-8 size of one JSON object exactly as the native serializer emits it. */
int32 ReplayCommandsUtf8Size(const TSharedPtr<FJsonObject>& Object)
{
	if (!Object.IsValid())
	{
		return 0;
	}
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	if (!FJsonSerializer::Serialize(Object.ToSharedRef(), Writer))
	{
		return -1;
	}
	return FTCHARToUTF8(*Text).Length();
}

const TArray<TSharedPtr<FJsonValue>>* ReplayCommandsRows(const FCortexCommandResult& Result)
{
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (Result.Data.IsValid())
	{
		Result.Data->TryGetArrayField(TEXT("recordings"), Rows);
	}
	return Rows;
}

TSharedPtr<FJsonObject> ReplayCommandsFindOperation(
	const TArray<TSharedPtr<FJsonValue>>& Commands, const FString& Name)
{
	for (const TSharedPtr<FJsonValue>& Value : Commands)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (Value.IsValid() && Value->TryGetObject(Object) && Object != nullptr
			&& (*Object)->GetStringField(TEXT("name")) == Name)
		{
			return *Object;
		}
	}
	return nullptr;
}

TSharedPtr<FJsonObject> ReplayCommandsFindParam(
	const TSharedPtr<FJsonObject>& Command, const FString& Name)
{
	if (!Command.IsValid())
	{
		return nullptr;
	}
	const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
	if (!Command->TryGetArrayField(TEXT("params"), Params) || Params == nullptr)
	{
		return nullptr;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Params)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (Value.IsValid() && Value->TryGetObject(Object) && Object != nullptr
			&& (*Object)->GetStringField(TEXT("name")) == Name)
		{
			return *Object;
		}
	}
	return nullptr;
}

void ReplayCommandsExpectError(
	FAutomationTestBase& Test,
	const FString& What,
	const FCortexCommandResult& Result,
	const FString& ExpectedCode)
{
	Test.TestFalse(FString::Printf(TEXT("%s rejected"), *What), Result.bSuccess);
	Test.TestEqual(FString::Printf(TEXT("%s error code"), *What), Result.ErrorCode, ExpectedCode);
}

void ReplayCommandsExpectOperationParam(
	FAutomationTestBase& Test,
	const TSharedPtr<FJsonObject>& Command,
	const FString& ParamName,
	const FString& ExpectedType,
	bool bExpectedRequired)
{
	const TSharedPtr<FJsonObject> Param = ReplayCommandsFindParam(Command, ParamName);
	Test.TestTrue(FString::Printf(TEXT("Param %s advertised"), *ParamName), Param.IsValid());
	if (!Param.IsValid())
	{
		return;
	}
	Test.TestEqual(FString::Printf(TEXT("Param %s type"), *ParamName),
		Param->GetStringField(TEXT("type")), ExpectedType);
	bool bRequired = true;
	Param->TryGetBoolField(TEXT("required"), bRequired);
	Test.TestEqual(FString::Printf(TEXT("Param %s required"), *ParamName), bRequired, bExpectedRequired);
}

bool ReplayCommandsAnyPieWorld()
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

FString ReplayCommandsPickMapPath()
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

/** Waits until an admitted replay run (and any owned PIE it requested) has fully released. */
class FCortexReplayCommandsAwaitIdle : public IAutomationLatentCommand
{
public:
	FCortexReplayCommandsAwaitIdle(
		FAutomationTestBase* InTest,
		TSharedRef<FCortexReplayService> InService,
		TSharedPtr<FCortexReplayTestFixture> InKeepAlive)
		: Test(InTest), Service(MoveTemp(InService)), KeepAlive(MoveTemp(InKeepAlive))
	{
	}

	bool Update() override
	{
		if (StartTime == 0.0)
		{
			StartTime = FPlatformTime::Seconds();
		}
		if (!Service->GetCurrentOperation().Data.IsValid() && !ReplayCommandsAnyPieWorld())
		{
			return true;
		}
		if (FPlatformTime::Seconds() - StartTime > 45.0)
		{
			Test->AddError(TEXT("Replay command run did not return to idle"));
			return true;
		}
		return false;
	}

private:
	FAutomationTestBase* Test;
	TSharedRef<FCortexReplayService> Service;
	TSharedPtr<FCortexReplayTestFixture> KeepAlive;
	double StartTime = 0.0;
};
}

// ---------------------------------------------------------------------------
// Fresh discovery and the exact five-operation surface.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayCommandsDiscoveryTest,
	"Cortex.Replay.Commands.DiscoveryContracts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayCommandsDiscoveryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayCommandsHarness Harness;
	Harness.StartDomain();

	const FCortexCommandResult Caps = Harness.Execute(TEXT("get_capabilities"), ReplayCommandsParams());
	TestTrue(TEXT("get_capabilities succeeds"), Caps.bSuccess);
	if (!Caps.bSuccess || !Caps.Data.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Domains = nullptr;
	TestTrue(TEXT("Capabilities contain domains"),
		Caps.Data->TryGetObjectField(TEXT("domains"), Domains) && Domains != nullptr);
	const TSharedPtr<FJsonObject>* ReplayDomain = nullptr;
	TestTrue(TEXT("Capabilities contain the replay domain"),
		Domains != nullptr && (*Domains)->TryGetObjectField(TEXT("replay"), ReplayDomain)
			&& ReplayDomain != nullptr);
	if (ReplayDomain == nullptr)
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
	TestTrue(TEXT("Replay publishes commands"),
		(*ReplayDomain)->TryGetArrayField(TEXT("commands"), Commands) && Commands != nullptr);
	if (Commands == nullptr)
	{
		return false;
	}

	// Exactly the five operations, no authoring/status extras.
	TestEqual(TEXT("Exactly five replay operations"), Commands->Num(), 5);
	for (const TCHAR* Expected : ReplayCommandsExpectedOperations)
	{
		TestTrue(FString::Printf(TEXT("Operation %s advertised"), Expected),
			ReplayCommandsFindOperation(*Commands, Expected).IsValid());
	}

	// Allowed parameters and their declared types.
	const TSharedPtr<FJsonObject> List = ReplayCommandsFindOperation(*Commands, TEXT("list_recordings"));
	const TSharedPtr<FJsonObject> GetRec = ReplayCommandsFindOperation(*Commands, TEXT("get_recording"));
	const TSharedPtr<FJsonObject> Start = ReplayCommandsFindOperation(*Commands, TEXT("start_replay"));
	const TSharedPtr<FJsonObject> GetRun = ReplayCommandsFindOperation(*Commands, TEXT("get_run"));
	const TSharedPtr<FJsonObject> Cancel = ReplayCommandsFindOperation(*Commands, TEXT("cancel_replay"));

	if (List.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* ListParams = nullptr;
		List->TryGetArrayField(TEXT("params"), ListParams);
		TestEqual(TEXT("list_recordings has exactly two paging parameters"),
			ListParams != nullptr ? ListParams->Num() : 0, 2);
	}
	ReplayCommandsExpectOperationParam(*this, GetRec, TEXT("recording_id"), TEXT("integer"), true);
	ReplayCommandsExpectOperationParam(*this, Start, TEXT("recording_id"), TEXT("integer"), true);
	ReplayCommandsExpectOperationParam(*this, GetRun, TEXT("run_id"), TEXT("string"), true);
	ReplayCommandsExpectOperationParam(*this, Cancel, TEXT("run_id"), TEXT("string"), true);
	ReplayCommandsExpectOperationParam(*this, List, TEXT("after_recording_id"), TEXT("integer"), false);
	ReplayCommandsExpectOperationParam(*this, List, TEXT("page_size"), TEXT("integer"), false);

	// No override/authoring/alternate-source parameter may be advertised by any operation.
	static const TCHAR* const ForbiddenParams[] = {
		TEXT("source"), TEXT("pose"), TEXT("start_pose"), TEXT("guard"), TEXT("tolerance"),
		TEXT("skip"), TEXT("wait"), TEXT("speed"), TEXT("path"), TEXT("input_file"),
		TEXT("console_command"), TEXT("metadata"), TEXT("events"), TEXT("limit"), TEXT("cursor"),
	};
	for (const TSharedPtr<FJsonValue>& Value : *Commands)
	{
		const TSharedPtr<FJsonObject>* Command = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Command) || Command == nullptr)
		{
			continue;
		}
		for (const TCHAR* Forbidden : ForbiddenParams)
		{
			TestFalse(
				FString::Printf(TEXT("Operation does not advertise %s"), Forbidden),
				ReplayCommandsFindParam(*Command, Forbidden).IsValid());
		}
	}

	// Human-only authoring/status and legacy QA names are never transport commands.
	for (const TCHAR* Forbidden : ReplayCommandsForbiddenOperations)
	{
		const FCortexCommandResult Result = Harness.Execute(
			FString::Printf(TEXT("replay.%s"), Forbidden), ReplayCommandsParams());
		TestFalse(FString::Printf(TEXT("replay.%s is not routed"), Forbidden), Result.bSuccess);
		TestEqual(FString::Printf(TEXT("replay.%s reports UNKNOWN_COMMAND"), Forbidden),
			Result.ErrorCode, FString(CortexErrorCodes::UnknownCommand));
	}

	return true;
}

// ---------------------------------------------------------------------------
// Strict parameter/ID denial at the native boundary (never UNKNOWN_COMMAND).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayCommandsStrictDenialTest,
	"Cortex.Replay.Commands.StrictParameterDenial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayCommandsStrictDenialTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayCommandsHarness Harness;

	// A disabled recording (11) and an enabled recording (13); the guessed ID 12 is absent.
	ReplayCommandsPublish(*this, *Harness.Fixture, *Harness.Library, 11, false,
		{ ReplayCommandsKeyDown(0) }, FString(), FString(), FString(), FString());
	ReplayCommandsPublish(*this, *Harness.Fixture, *Harness.Library, 13, true,
		{ ReplayCommandsKeyDown(0) }, FString(), FString(), FString(), FString());
	Harness.StartDomain();

	// Boolean/string/fractional/zero/negative/overflow IDs are INVALID_VALUE, not a routing miss.
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetBoolField(TEXT("recording_id"), true);
		const FCortexCommandResult Result = Harness.Execute(TEXT("replay.get_recording"), Params);
		TestFalse(TEXT("Boolean ID rejected"), Result.bSuccess);
		TestEqual(TEXT("Strict ID error"), Result.ErrorCode, CortexErrorCodes::InvalidValue);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("recording_id"), TEXT("13"));
		ReplayCommandsExpectError(*this, TEXT("String ID"), Harness.Execute(TEXT("replay.get_recording"), Params),
			CortexErrorCodes::InvalidValue);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("recording_id"), 13.5);
		ReplayCommandsExpectError(*this, TEXT("Fractional ID"), Harness.Execute(TEXT("replay.get_recording"), Params),
			CortexErrorCodes::InvalidValue);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("recording_id"), 0);
		ReplayCommandsExpectError(*this, TEXT("Zero ID"), Harness.Execute(TEXT("replay.get_recording"), Params),
			CortexErrorCodes::InvalidValue);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("recording_id"), -1);
		ReplayCommandsExpectError(*this, TEXT("Negative ID"), Harness.Execute(TEXT("replay.get_recording"), Params),
			CortexErrorCodes::InvalidValue);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("recording_id"), 2147483648.0);
		ReplayCommandsExpectError(*this, TEXT("Overflow ID"), Harness.Execute(TEXT("replay.get_recording"), Params),
			CortexErrorCodes::InvalidValue);
	}
	ReplayCommandsExpectError(*this, TEXT("Missing recording_id for get_recording"),
		Harness.Execute(TEXT("replay.get_recording"), ReplayCommandsParams()),
		CortexErrorCodes::InvalidField);
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetBoolField(TEXT("recording_id"), true);
		ReplayCommandsExpectError(*this, TEXT("Boolean start ID"),
			Harness.Execute(TEXT("replay.start_replay"), Params), CortexErrorCodes::InvalidValue);
	}

	// Guessed/absent ID is denied with a strict recording code, never UNKNOWN_COMMAND.
	ReplayCommandsExpectError(*this, TEXT("Absent guessed recording for get_recording"),
		Harness.Execute(TEXT("replay.get_recording"), ReplayCommandsRecordingParams(12)),
		CortexReplayErrorCodes::RecordingNotFound);
	ReplayCommandsExpectError(*this, TEXT("Absent guessed recording for start_replay"),
		Harness.Execute(TEXT("replay.start_replay"), ReplayCommandsRecordingParams(12)),
		CortexReplayErrorCodes::RecordingNotFound);
	ReplayCommandsExpectError(*this, TEXT("Disabled recording for get_recording"),
		Harness.Execute(TEXT("replay.get_recording"), ReplayCommandsRecordingParams(11)),
		CortexReplayErrorCodes::PermissionDenied);
	ReplayCommandsExpectError(*this, TEXT("Disabled recording for start_replay"),
		Harness.Execute(TEXT("replay.start_replay"), ReplayCommandsRecordingParams(11)),
		CortexReplayErrorCodes::PermissionDenied);

	// A denied start allocates no run and no retained record.
	TestEqual(TEXT("Denied starts allocate no run file"), ReplayCommandsRunFileCount(*Harness.Fixture), 0);
	{
		const FCortexCommandResult List = Harness.Execute(TEXT("replay.list_recordings"), ReplayCommandsParams());
		TestTrue(TEXT("list_recordings succeeds"), List.bSuccess);
		const TArray<TSharedPtr<FJsonValue>>* Rows = ReplayCommandsRows(List);
		TestEqual(TEXT("Disabled/absent recordings are not discoverable"), Rows != nullptr ? Rows->Num() : 0, 1);
	}

	// Unknown-field overrides are rejected before the service is reached.
	for (const TCHAR* Override : { TEXT("source"), TEXT("start_pose"), TEXT("guard"),
		TEXT("tolerance"), TEXT("skip_events"), TEXT("wait"), TEXT("speed"), TEXT("path") })
	{
		TSharedPtr<FJsonObject> Params = ReplayCommandsRecordingParams(13);
		Params->SetStringField(Override, TEXT("x"));
		ReplayCommandsExpectError(*this, FString::Printf(TEXT("Override %s"), Override),
			Harness.Execute(TEXT("replay.start_replay"), Params), CortexErrorCodes::InvalidField);
	}
	{
		TSharedPtr<FJsonObject> Params = ReplayCommandsRecordingParams(13);
		Params->SetStringField(TEXT("source"), TEXT("human"));
		ReplayCommandsExpectError(*this, TEXT("Caller-supplied human source"),
			Harness.Execute(TEXT("replay.start_replay"), Params), CortexErrorCodes::InvalidField);
	}

	// Paging: strict integer/range, explicit 0 invalid, generic cursor/limit rejected.
	for (double BadSize : { 0.0, 101.0, 20.5 })
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("page_size"), BadSize);
		ReplayCommandsExpectError(*this, FString::Printf(TEXT("page_size %f"), BadSize),
			Harness.Execute(TEXT("replay.list_recordings"), Params), CortexErrorCodes::InvalidValue);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("page_size"), TEXT("20"));
		ReplayCommandsExpectError(*this, TEXT("String page_size"),
			Harness.Execute(TEXT("replay.list_recordings"), Params), CortexErrorCodes::InvalidValue);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("after_recording_id"), 0);
		ReplayCommandsExpectError(*this, TEXT("Explicit zero cursor"),
			Harness.Execute(TEXT("replay.list_recordings"), Params), CortexErrorCodes::InvalidValue);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("after_recording_id"), -3);
		ReplayCommandsExpectError(*this, TEXT("Negative cursor"),
			Harness.Execute(TEXT("replay.list_recordings"), Params), CortexErrorCodes::InvalidValue);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetBoolField(TEXT("after_recording_id"), true);
		ReplayCommandsExpectError(*this, TEXT("Boolean cursor"),
			Harness.Execute(TEXT("replay.list_recordings"), Params), CortexErrorCodes::InvalidValue);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 5);
		ReplayCommandsExpectError(*this, TEXT("Generic limit"),
			Harness.Execute(TEXT("replay.list_recordings"), Params), CortexErrorCodes::InvalidField);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("cursor"), TEXT("abc"));
		ReplayCommandsExpectError(*this, TEXT("Generic cursor"),
			Harness.Execute(TEXT("replay.list_recordings"), Params), CortexErrorCodes::InvalidField);
	}

	// Run IDs must be nonzero canonical GUIDs; traversal/path input is a value error.
	ReplayCommandsExpectError(*this, TEXT("Malformed run_id for get_run"),
		Harness.Execute(TEXT("replay.get_run"), ReplayCommandsRunParams(TEXT("not-a-guid"))),
		CortexErrorCodes::InvalidValue);
	ReplayCommandsExpectError(*this, TEXT("Path traversal run_id for get_run"),
		Harness.Execute(TEXT("replay.get_run"), ReplayCommandsRunParams(TEXT("../../etc/passwd"))),
		CortexErrorCodes::InvalidValue);
	ReplayCommandsExpectError(*this, TEXT("Missing run_id for get_run"),
		Harness.Execute(TEXT("replay.get_run"), ReplayCommandsParams()),
		CortexErrorCodes::InvalidField);
	ReplayCommandsExpectError(*this, TEXT("Malformed run_id for cancel_replay"),
		Harness.Execute(TEXT("replay.cancel_replay"), ReplayCommandsRunParams(TEXT("not-a-guid"))),
		CortexErrorCodes::InvalidValue);
	ReplayCommandsExpectError(*this, TEXT("Path traversal run_id for cancel_replay"),
		Harness.Execute(TEXT("replay.cancel_replay"), ReplayCommandsRunParams(TEXT("../Runs/x"))),
		CortexErrorCodes::InvalidValue);

	return true;
}

// ---------------------------------------------------------------------------
// get_recording / get_run read contracts and human-run invisibility.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayCommandsReadContractsTest,
	"Cortex.Replay.Commands.RecordingAndRunReadContracts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayCommandsReadContractsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayCommandsHarness Harness;

	ReplayCommandsPublish(*this, *Harness.Fixture, *Harness.Library, 1, true,
		{ ReplayCommandsKeyDown(0) }, FString(), FString(), FString(), FString());

	const FGuid AiRun = FGuid::NewGuid();
	const FGuid HumanRun = FGuid::NewGuid();
	const FString AiSnapshot = FString::ChrN(64, TEXT('a'));
	const FString AiInitial = FString::ChrN(64, TEXT('b'));
	const FString AiInputs = FString::ChrN(64, TEXT('c'));
	const FDateTime Started = FDateTime::UtcNow() - FTimespan::FromMinutes(5);
	const FDateTime Finalized = FDateTime::UtcNow();
	TestTrue(TEXT("Terminal Error AI run written"),
		ReplayCommandsWriteRun(*Harness.Fixture, AiRun, ReplayCommandsRetainedRunJson(
			AiRun, 1, TEXT("ai"), TEXT("Error"), TEXT("origin-editor"),
			Started, Finalized, AiSnapshot, AiInitial, AiInputs)));
	TestTrue(TEXT("Human run written"),
		ReplayCommandsWriteRun(*Harness.Fixture, HumanRun, ReplayCommandsRetainedRunJson(
			HumanRun, 1, TEXT("human"), TEXT("Cancelled"), TEXT("origin-editor"),
			Started, Finalized, FString::ChrN(64, TEXT('d')), FString::ChrN(64, TEXT('e')),
			FString::ChrN(64, TEXT('f')))));

	// Older AI records, so the newest-run assertions above keep resolving to the Error run.
	const FDateTime OlderStarted = Started - FTimespan::FromMinutes(30);
	const FDateTime OlderFinalized = Finalized - FTimespan::FromMinutes(30);

	// A pre-frame record must stay discoverable as a compact summary while its full detail is not.
	const FGuid LegacyAiRun = FGuid::NewGuid();
	TestTrue(TEXT("Historical pre-frame AI run written"),
		ReplayCommandsWriteRun(*Harness.Fixture, LegacyAiRun,
			ReplayCommandsLegacyRunJson(LegacyAiRun, 1, TEXT("ai"), TEXT("Cancelled"),
				OlderStarted, OlderFinalized)));

	const FGuid ConsistentCompleted = FGuid::NewGuid();
	TestTrue(TEXT("Completed run with a finished frontier written"),
		ReplayCommandsWriteRun(*Harness.Fixture, ConsistentCompleted, ReplayCommandsRetainedRunJson(
			ConsistentCompleted, 1, TEXT("ai"), TEXT("Completed"), TEXT("origin-editor"),
			OlderStarted, OlderFinalized, FString::ChrN(64, TEXT('1')), FString::ChrN(64, TEXT('2')),
			FString::ChrN(64, TEXT('3')), 3, 3)));

	// Malformed frame progress: an unfinished frontier claiming completion, an out-of-bounds count
	// and a digest that is not a SHA-256 are all refused by the retained record's own contract.
	const FGuid UnfinishedCompleted = FGuid::NewGuid();
	TestTrue(TEXT("Completed run with an unfinished frontier written"),
		ReplayCommandsWriteRun(*Harness.Fixture, UnfinishedCompleted, ReplayCommandsRetainedRunJson(
			UnfinishedCompleted, 1, TEXT("ai"), TEXT("Completed"), TEXT("origin-editor"),
			OlderStarted, OlderFinalized, FString::ChrN(64, TEXT('1')), FString::ChrN(64, TEXT('2')),
			FString::ChrN(64, TEXT('3')), 2, 3)));
	const FGuid OutOfBoundsProgress = FGuid::NewGuid();
	TestTrue(TEXT("Out-of-bounds frame progress written"),
		ReplayCommandsWriteRun(*Harness.Fixture, OutOfBoundsProgress, ReplayCommandsRetainedRunJson(
			OutOfBoundsProgress, 1, TEXT("ai"), TEXT("Error"), TEXT("origin-editor"),
			OlderStarted, OlderFinalized, FString::ChrN(64, TEXT('1')), FString::ChrN(64, TEXT('2')),
			FString::ChrN(64, TEXT('3')), 4, 3)));
	const FGuid MalformedFramesDigest = FGuid::NewGuid();
	TestTrue(TEXT("Malformed frames digest written"),
		ReplayCommandsWriteRun(*Harness.Fixture, MalformedFramesDigest, ReplayCommandsRetainedRunJson(
			MalformedFramesDigest, 1, TEXT("ai"), TEXT("Cancelled"), TEXT("origin-editor"),
			OlderStarted, OlderFinalized, FString::ChrN(64, TEXT('1')), FString::ChrN(64, TEXT('2')),
			FString::ChrN(64, TEXT('3')), 2, 2, FString::ChrN(63, TEXT('7')))));

	Harness.StartDomain();

	// get_recording: canonical map/start pose/press-only coverage, no input rows.
	const FCortexCommandResult Recording =
		Harness.Execute(TEXT("replay.get_recording"), ReplayCommandsRecordingParams(1));
	TestTrue(TEXT("get_recording succeeds"), Recording.bSuccess);
	if (Recording.bSuccess && Recording.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>& Data = Recording.Data;
		FString MapPath;
		TestTrue(TEXT("Canonical map exposed"), Data->TryGetStringField(TEXT("map_asset_path"), MapPath));
		TestTrue(TEXT("Snapshot digest exposed"), Data->HasField(TEXT("initial_state_sha256")));
		TestTrue(TEXT("Inputs digest exposed"), Data->HasField(TEXT("inputs_sha256")));
		TestTrue(TEXT("Recording snapshot digest exposed"), Data->HasField(TEXT("recording_snapshot_sha256")));

		const TSharedPtr<FJsonObject>* Coverage = nullptr;
		TestTrue(TEXT("Press-only guard coverage exposed"),
			Data->TryGetObjectField(TEXT("guard_coverage"), Coverage) && Coverage != nullptr);
		if (Coverage != nullptr)
		{
			TestTrue(TEXT("Coverage reports pose presses"), (*Coverage)->HasField(TEXT("pose_presses")));
		}

		const TSharedPtr<FJsonObject>* InitialState = nullptr;
		TestTrue(TEXT("Recorded start pose exposed"),
			Data->TryGetObjectField(TEXT("initial_state"), InitialState) && InitialState != nullptr);
		if (InitialState != nullptr)
		{
			FString PawnClassPath;
			TestTrue(TEXT("Start pose pawn class exposed"),
				(*InitialState)->TryGetStringField(TEXT("pawn_class_path"), PawnClassPath));
			const TSharedPtr<FJsonObject>* PawnTransform = nullptr;
			TestTrue(TEXT("Start pawn transform exposed"),
				(*InitialState)->TryGetObjectField(TEXT("pawn_transform"), PawnTransform)
					&& PawnTransform != nullptr);
			if (PawnTransform != nullptr)
			{
				TestTrue(TEXT("Start pawn location exposed"),
					(*PawnTransform)->HasField(TEXT("location_cm")));
				TestTrue(TEXT("Start pawn rotation exposed"),
					(*PawnTransform)->HasField(TEXT("rotation_deg")));
			}
			TestTrue(TEXT("Start control rotation exposed"),
				(*InitialState)->HasField(TEXT("control_rotation_deg")));
		}

		// No arbitrary file access or recorded input authoring rows.
		for (const TCHAR* Forbidden : { TEXT("events"), TEXT("inputs"), TEXT("input_rows"),
			TEXT("event_rows"), TEXT("selector_catalog"), TEXT("path"), TEXT("input_file") })
		{
			TestFalse(FString::Printf(TEXT("get_recording omits %s"), Forbidden),
				Data->HasField(Forbidden));
		}
	}

	// get_run: a terminal Error is a successful query carrying bounded execution diagnostics.
	const FCortexCommandResult Run = Harness.Execute(
		TEXT("replay.get_run"), ReplayCommandsRunParams(AiRun.ToString(EGuidFormats::DigitsWithHyphens).ToLower()));
	TestTrue(TEXT("Terminal Error run query succeeds"), Run.bSuccess);
	if (Run.bSuccess && Run.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>& Data = Run.Data;
		FString State;
		Data->TryGetStringField(TEXT("state"), State);
		TestEqual(TEXT("Execution Error state"), State, FString(TEXT("Error")));
		FString Scope;
		Data->TryGetStringField(TEXT("guard_scope"), Scope);
		TestEqual(TEXT("Press-only guard scope"), Scope, FString(TEXT("press_only")));
		double Wait = -1.0;
		Data->TryGetNumberField(TEXT("authorized_wait_seconds"), Wait);
		TestTrue(TEXT("Wait budget is a bounded nonnegative scalar"), Wait >= 0.0 && Wait <= 5.0);
		TestTrue(TEXT("Terminal waiting is null"), Data->HasTypedField<EJson::Null>(TEXT("waiting")));

		const TSharedPtr<FJsonObject>* Error = nullptr;
		TestTrue(TEXT("Bounded execution error exposed"),
			Data->TryGetObjectField(TEXT("execution_error"), Error) && Error != nullptr);
		if (Error != nullptr)
		{
			FString Code;
			TestTrue(TEXT("Execution error code exposed"), (*Error)->TryGetStringField(TEXT("code"), Code));
			TestEqual(TEXT("Execution error code preserved"), Code,
				FString(CortexReplayErrorCodes::ReplayPoseGuardFailed));
			TestTrue(TEXT("Execution error details bounded to identity/enums"),
				(*Error)->HasField(TEXT("details")));
		}

		FString Snapshot;
		Data->TryGetStringField(TEXT("recording_snapshot_sha256"), Snapshot);
		TestEqual(TEXT("Admitted snapshot identity retained"), Snapshot, AiSnapshot);

		// Frame identity: a terminal record reports the verified frame frontier, never a live frame.
		double CompletedFrames = -1.0;
		TestTrue(TEXT("Verified frame progress exposed"),
			Data->TryGetNumberField(TEXT("completed_frames"), CompletedFrames));
		TestEqual(TEXT("Dispatched-frame progress retained"), static_cast<int32>(CompletedFrames), 2);
		double FrameCount = -1.0;
		TestTrue(TEXT("Frame count exposed"), Data->TryGetNumberField(TEXT("frame_count"), FrameCount));
		TestEqual(TEXT("Admitted frame count retained"), static_cast<int32>(FrameCount), 2);
		FString FramesDigest;
		TestTrue(TEXT("Frames digest exposed"),
			Data->TryGetStringField(TEXT("frames_sha256"), FramesDigest));
		TestEqual(TEXT("Frames digest retained"), FramesDigest, FString::ChrN(64, TEXT('7')));
		TestTrue(TEXT("Terminal current frame is null"),
			Data->HasTypedField<EJson::Null>(TEXT("current_frame")));
	}

	// Human-originated runs are invisible to the AI surface and to recovery.
	const FString HumanId = HumanRun.ToString(EGuidFormats::DigitsWithHyphens).ToLower();
	ReplayCommandsExpectError(*this, TEXT("Human run query"),
		Harness.Execute(TEXT("replay.get_run"), ReplayCommandsRunParams(HumanId)),
		CortexReplayErrorCodes::RunNotFound);
	ReplayCommandsExpectError(*this, TEXT("Human run cancellation"),
		Harness.Execute(TEXT("replay.cancel_replay"), ReplayCommandsRunParams(HumanId)),
		CortexReplayErrorCodes::RunNotFound);
	ReplayCommandsExpectError(*this, TEXT("Unknown run query"),
		Harness.Execute(TEXT("replay.get_run"),
			ReplayCommandsRunParams(FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower())),
		CortexReplayErrorCodes::RunNotFound);

	{
		const FCortexCommandResult List = Harness.Execute(TEXT("replay.list_recordings"), ReplayCommandsParams());
		TestTrue(TEXT("list_recordings succeeds"), List.bSuccess);
		const TArray<TSharedPtr<FJsonValue>>* Recent = nullptr;
		if (List.Data.IsValid())
		{
			List.Data->TryGetArrayField(TEXT("recent_ai_runs"), Recent);
		}
		TestEqual(TEXT("Only the loadable AI runs are recovered"),
			Recent != nullptr ? Recent->Num() : 0, 3);
		if (Recent != nullptr && Recent->Num() == 3)
		{
			const TSharedPtr<FJsonObject>* Summary = nullptr;
			(*Recent)[0]->TryGetObject(Summary);
			if (Summary != nullptr)
			{
				FString RecoveredId;
				(*Summary)->TryGetStringField(TEXT("run_id"), RecoveredId);
				TestEqual(TEXT("Recovered summary is the newest AI run"), RecoveredId,
					AiRun.ToString(EGuidFormats::DigitsWithHyphens).ToLower());
			}
		}
	}

	// Frame identity is part of the retained record's contract, and a pre-frame record keeps its
	// compact summary discovery while its full terminal detail stays unsupported.
	const FCortexCommandResult Completed = Harness.Execute(TEXT("replay.get_run"),
		ReplayCommandsRunParams(ConsistentCompleted.ToString(EGuidFormats::DigitsWithHyphens).ToLower()));
	TestTrue(TEXT("Completed terminal run query succeeds"), Completed.bSuccess);
	if (Completed.bSuccess && Completed.Data.IsValid())
	{
		FString State;
		Completed.Data->TryGetStringField(TEXT("state"), State);
		TestEqual(TEXT("Completed state preserved"), State, FString(TEXT("Completed")));
		double CompletedFrames = -1.0;
		Completed.Data->TryGetNumberField(TEXT("completed_frames"), CompletedFrames);
		double FrameCount = -1.0;
		Completed.Data->TryGetNumberField(TEXT("frame_count"), FrameCount);
		TestEqual(TEXT("Completed run finished its frame frontier"),
			static_cast<int32>(CompletedFrames), static_cast<int32>(FrameCount));
		TestTrue(TEXT("Completed run reports no live frame"),
			Completed.Data->HasTypedField<EJson::Null>(TEXT("current_frame")));
	}

	const FString LegacyId = LegacyAiRun.ToString(EGuidFormats::DigitsWithHyphens).ToLower();
	ReplayCommandsExpectError(*this, TEXT("Historical full run access"),
		Harness.Execute(TEXT("replay.get_run"), ReplayCommandsRunParams(LegacyId)),
		CortexReplayErrorCodes::UnsupportedRunFormat);
	ReplayCommandsExpectError(*this, TEXT("Completed run with an unfinished frontier is refused"),
		Harness.Execute(TEXT("replay.get_run"), ReplayCommandsRunParams(
			UnfinishedCompleted.ToString(EGuidFormats::DigitsWithHyphens).ToLower())),
		CortexReplayErrorCodes::RunNotFound);
	ReplayCommandsExpectError(*this, TEXT("Out-of-bounds frame progress is refused"),
		Harness.Execute(TEXT("replay.get_run"), ReplayCommandsRunParams(
			OutOfBoundsProgress.ToString(EGuidFormats::DigitsWithHyphens).ToLower())),
		CortexReplayErrorCodes::RunNotFound);
	ReplayCommandsExpectError(*this, TEXT("Malformed frames digest is refused"),
		Harness.Execute(TEXT("replay.get_run"), ReplayCommandsRunParams(
			MalformedFramesDigest.ToString(EGuidFormats::DigitsWithHyphens).ToLower())),
		CortexReplayErrorCodes::RunNotFound);

	return true;
}

// ---------------------------------------------------------------------------
// Lost-ack recovery, terminal durability across revocation/deletion/restart,
// and stable repeated cancellation.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayCommandsRecoveryTest,
	"Cortex.Replay.Commands.RecoveryStatusAndRestart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayCommandsRecoveryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayCommandsHarness Harness;

	ReplayCommandsPublish(*this, *Harness.Fixture, *Harness.Library, 1, true,
		{ ReplayCommandsKeyDown(0) }, TEXT("Recovery"), TEXT("Recovery flow"), FString(), FString());

	const FGuid RunId = FGuid::NewGuid();
	const FString Snapshot = FString::ChrN(64, TEXT('1'));
	const FString Initial = FString::ChrN(64, TEXT('2'));
	const FString Inputs = FString::ChrN(64, TEXT('3'));
	const FDateTime Started = FDateTime::UtcNow() - FTimespan::FromMinutes(1);
	const FDateTime Finalized = FDateTime::UtcNow();
	TestTrue(TEXT("Terminal AI run written"),
		ReplayCommandsWriteRun(*Harness.Fixture, RunId, ReplayCommandsRetainedRunJson(
			RunId, 1, TEXT("ai"), TEXT("Cancelled"), TEXT("origin-editor"),
			Started, Finalized, Snapshot, Initial, Inputs)));
	Harness.StartDomain();

	const FString RunText = RunId.ToString(EGuidFormats::DigitsWithHyphens).ToLower();

	// A start acknowledgement lost after dispatch is recovered through live discovery/status.
	{
		const FCortexCommandResult List = Harness.Execute(TEXT("replay.list_recordings"), ReplayCommandsParams());
		TestTrue(TEXT("Recovery list succeeds"), List.bSuccess);
		if (List.Data.IsValid())
		{
			double Window = 0.0;
			double MaxRuns = 0.0;
			List.Data->TryGetNumberField(TEXT("recovery_window_seconds"), Window);
			List.Data->TryGetNumberField(TEXT("recovery_max_terminal_runs"), MaxRuns);
			TestEqual(TEXT("Recovery window is 24h"), Window, 86400.0);
			TestEqual(TEXT("Recovery keeps 100 terminal runs"), MaxRuns, 100.0);
			FString EditorInstance;
			TestTrue(TEXT("Current editor instance identified"),
				List.Data->TryGetStringField(TEXT("editor_instance_id"), EditorInstance));
			TestTrue(TEXT("Active AI run slot present"), List.Data->HasField(TEXT("active_ai_run")));
		}
		const TArray<TSharedPtr<FJsonValue>>* Recent = nullptr;
		if (List.Data.IsValid())
		{
			List.Data->TryGetArrayField(TEXT("recent_ai_runs"), Recent);
		}
		TestTrue(TEXT("Lost-ack run appears in recovery"), Recent != nullptr && Recent->Num() == 1);
		if (Recent != nullptr && Recent->Num() == 1)
		{
			const TSharedPtr<FJsonObject>* Summary = nullptr;
			(*Recent)[0]->TryGetObject(Summary);
			if (Summary != nullptr)
			{
				FString Recovered;
				(*Summary)->TryGetStringField(TEXT("run_id"), Recovered);
				TestEqual(TEXT("Recovery summary identifies the run"), Recovered, RunText);
				TestTrue(TEXT("Summary carries originating editor"), (*Summary)->HasField(TEXT("editor_instance_id")));
				TestTrue(TEXT("Summary carries start/end UTC"), (*Summary)->HasField(TEXT("started_at_utc"))
					&& (*Summary)->HasField(TEXT("finalized_at_utc")));
			}
		}
	}

	// Revocation does not erase the terminal result or its admitted identity.
	TestTrue(TEXT("Permission revoked by the human surface"),
		Harness.Library->SaveMetadata(1, TEXT("Recovery"), TEXT("Recovery flow"), false).bSuccess);
	{
		const FCortexCommandResult Run = Harness.Execute(TEXT("replay.get_run"), ReplayCommandsRunParams(RunText));
		TestTrue(TEXT("Revoked run remains queryable"), Run.bSuccess);
		if (Run.bSuccess && Run.Data.IsValid())
		{
			FString Digest;
			Run.Data->TryGetStringField(TEXT("recording_snapshot_sha256"), Digest);
			TestEqual(TEXT("Revocation preserves admitted snapshot identity"), Digest, Snapshot);
		}
		ReplayCommandsExpectError(*this, TEXT("get_recording after revocation"),
			Harness.Execute(TEXT("replay.get_recording"), ReplayCommandsRecordingParams(1)),
			CortexReplayErrorCodes::PermissionDenied);
		const FCortexCommandResult List = Harness.Execute(TEXT("replay.list_recordings"), ReplayCommandsParams());
		const TArray<TSharedPtr<FJsonValue>>* Recent = nullptr;
		if (List.Data.IsValid())
		{
			List.Data->TryGetArrayField(TEXT("recent_ai_runs"), Recent);
		}
		TestEqual(TEXT("Revocation keeps the recovery summary"),
			Recent != nullptr ? Recent->Num() : 0, 1);
	}

	// Deletion does not erase the terminal result.
	TestTrue(TEXT("Recording deleted"), Harness.Library->Delete(1).bSuccess);
	{
		const FCortexCommandResult Run = Harness.Execute(TEXT("replay.get_run"), ReplayCommandsRunParams(RunText));
		TestTrue(TEXT("Deleted recording's run remains queryable"), Run.bSuccess);
		if (Run.bSuccess && Run.Data.IsValid())
		{
			FString Digest;
			Run.Data->TryGetStringField(TEXT("recording_snapshot_sha256"), Digest);
			TestEqual(TEXT("Deletion preserves admitted snapshot identity"), Digest, Snapshot);
			FString State;
			Run.Data->TryGetStringField(TEXT("state"), State);
			TestEqual(TEXT("Deleted recording's run keeps its terminal state"), State, FString(TEXT("Cancelled")));
		}
		ReplayCommandsExpectError(*this, TEXT("get_recording after deletion"),
			Harness.Execute(TEXT("replay.get_recording"), ReplayCommandsRecordingParams(1)),
			CortexReplayErrorCodes::RecordingNotFound);

		// Repeated cancellation of the same finalized run returns the same terminal state.
		const FCortexCommandResult First = Harness.Execute(TEXT("replay.cancel_replay"), ReplayCommandsRunParams(RunText));
		const FCortexCommandResult Second = Harness.Execute(TEXT("replay.cancel_replay"), ReplayCommandsRunParams(RunText));
		TestTrue(TEXT("First finalized cancellation succeeds"), First.bSuccess);
		TestTrue(TEXT("Repeated finalized cancellation succeeds"), Second.bSuccess);
		FString FirstState;
		FString SecondState;
		if (First.Data.IsValid()) { First.Data->TryGetStringField(TEXT("state"), FirstState); }
		if (Second.Data.IsValid()) { Second.Data->TryGetStringField(TEXT("state"), SecondState); }
		TestEqual(TEXT("Repeated cancellation is stable"), SecondState, FirstState);
		TestEqual(TEXT("Repeated cancellation state is terminal"), FirstState, FString(TEXT("Cancelled")));
	}

	// An editor restart re-reads the retained local result with the same identities.
	Harness.RestartDomain();
	{
		const FCortexCommandResult Run = Harness.Execute(TEXT("replay.get_run"), ReplayCommandsRunParams(RunText));
		TestTrue(TEXT("Run survives service restart"), Run.bSuccess);
		if (Run.bSuccess && Run.Data.IsValid())
		{
			FString SnapshotDigest;
			FString InitialDigest;
			FString InputsDigest;
			Run.Data->TryGetStringField(TEXT("recording_snapshot_sha256"), SnapshotDigest);
			Run.Data->TryGetStringField(TEXT("initial_state_sha256"), InitialDigest);
			Run.Data->TryGetStringField(TEXT("inputs_sha256"), InputsDigest);
			TestEqual(TEXT("Restart preserves snapshot identity"), SnapshotDigest, Snapshot);
			TestEqual(TEXT("Restart preserves initial-state identity"), InitialDigest, Initial);
			TestEqual(TEXT("Restart preserves inputs identity"), InputsDigest, Inputs);
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// Native batch routing: start is forbidden; reads and cancellation stay allowed.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayCommandsBatchTest,
	"Cortex.Replay.Commands.BatchStartForbidden",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayCommandsBatchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayCommandsHarness Harness;
	ReplayCommandsPublish(*this, *Harness.Fixture, *Harness.Library, 1, true,
		{ ReplayCommandsKeyDown(0) }, FString(), FString(), FString(), FString());
	Harness.StartDomain();

	auto BuildBatch = [](const FString& Command, const TSharedPtr<FJsonObject>& StepParams)
	{
		TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
		Step->SetStringField(TEXT("command"), Command);
		if (StepParams.IsValid())
		{
			Step->SetObjectField(TEXT("params"), StepParams);
		}
		TArray<TSharedPtr<FJsonValue>> Commands;
		Commands.Add(MakeShared<FJsonValueObject>(Step));
		TSharedPtr<FJsonObject> Batch = MakeShared<FJsonObject>();
		Batch->SetArrayField(TEXT("commands"), Commands);
		return Batch;
	};

	auto ReadStepError = [](const FCortexCommandResult& Batch, FString& OutCode) -> bool
	{
		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		if (!Batch.Data.IsValid() || !Batch.Data->TryGetArrayField(TEXT("results"), Results)
			|| Results == nullptr || Results->Num() == 0)
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* Step = nullptr;
		if (!(*Results)[0]->TryGetObject(Step) || Step == nullptr)
		{
			return false;
		}
		return (*Step)->TryGetStringField(TEXT("error_code"), OutCode);
	};

	// start_replay inside a Core batch is refused before any run/request allocation.
	const FCortexCommandResult BatchStart = Harness.Execute(
		TEXT("batch"), BuildBatch(TEXT("replay.start_replay"), ReplayCommandsRecordingParams(1)));
	TestTrue(TEXT("Batch itself completes"), BatchStart.bSuccess);
	FString StartCode;
	TestTrue(TEXT("Batch start step reports an error"), ReadStepError(BatchStart, StartCode));
	TestEqual(TEXT("Batched start is INVALID_OPERATION"), StartCode, FString(CortexErrorCodes::InvalidOperation));
	TestEqual(TEXT("Batched start allocates no run"), ReplayCommandsRunFileCount(*Harness.Fixture), 0);
	{
		const FCortexCommandResult List = Harness.Execute(TEXT("replay.list_recordings"), ReplayCommandsParams());
		const TSharedPtr<FJsonObject>* Active = nullptr;
		const bool bHasActive = List.Data.IsValid()
			&& List.Data->TryGetObjectField(TEXT("active_ai_run"), Active);
		TestTrue(TEXT("No active run admitted via batch"), !bHasActive);
	}

	// Read-only and cancellation operations remain admissible inside ordinary batches.
	{
		const FCortexCommandResult BatchRead = Harness.Execute(
			TEXT("batch"), BuildBatch(TEXT("replay.get_recording"), ReplayCommandsRecordingParams(1)));
		TestTrue(TEXT("Batch get_recording completes"), BatchRead.bSuccess);
		FString ReadCode;
		TestFalse(TEXT("Batch get_recording has no error"), ReadStepError(BatchRead, ReadCode));
	}
	{
		const FCortexCommandResult BatchList = Harness.Execute(
			TEXT("batch"), BuildBatch(TEXT("replay.list_recordings"), ReplayCommandsParams()));
		TestTrue(TEXT("Batch list_recordings completes"), BatchList.bSuccess);
		FString ListCode;
		TestFalse(TEXT("Batch list_recordings has no error"), ReadStepError(BatchList, ListCode));
	}
	{
		// A cancel of an unknown (but well-formed) run must reach the native RUN_NOT_FOUND, which
		// proves cancellation is not globally refused by the batch guard.
		const FString UnknownRun =
			FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower();
		const FCortexCommandResult BatchCancel = Harness.Execute(
			TEXT("batch"), BuildBatch(TEXT("replay.cancel_replay"), ReplayCommandsRunParams(UnknownRun)));
		TestTrue(TEXT("Batch cancel_replay completes"), BatchCancel.bSuccess);
		FString CancelCode;
		TestTrue(TEXT("Batch cancel step reports native error"), ReadStepError(BatchCancel, CancelCode));
		TestEqual(TEXT("Batch cancel keeps native run-not-found"), CancelCode,
			FString(CortexReplayErrorCodes::RunNotFound));
	}

	return true;
}

// ---------------------------------------------------------------------------
// Bounded live pages: exact eligible set, revocation between pages, worst-case
// Unicode rows and the full 100-summary recovery window on every page.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayCommandsPageTest,
	"Cortex.Replay.Commands.PageBudgetAndRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayCommandsPageTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayCommandsHarness Harness;

	const FString MapPath = ReplayCommandsPickMapPath();
	const FString PawnPath = TEXT("/Script/Engine.DefaultPawn");
	const FString WorstName = ReplayCommandsWorstCaseText(128);
	const FString WorstDescription = ReplayCommandsWorstCaseText(1024);

	constexpr int32 MaxRecordings = 30;
	for (int32 Id = 1; Id <= MaxRecordings; ++Id)
	{
		// Recording 5 starts disabled so it can be enabled below an already-traversed cursor.
		if (!ReplayCommandsPublish(*this, *Harness.Fixture, *Harness.Library, Id, Id != 5,
			{ ReplayCommandsKeyDown(0) }, WorstName, WorstDescription, MapPath, PawnPath))
		{
			return false;
		}
	}

	// 100 retained terminal AI runs plus one human run that recovery must never leak.
	const TArray<FGuid> TerminalRuns = [&Harness]()
	{
		TArray<FGuid> Ids;
		const FDateTime Base = FDateTime::UtcNow() - FTimespan::FromHours(1);
		for (int32 Index = 0; Index < 100; ++Index)
		{
			const FGuid RunId = FGuid::NewGuid();
			Ids.Add(RunId);
			ReplayCommandsWriteRun(*Harness.Fixture, RunId, ReplayCommandsRetainedRunJson(
				RunId, 1, TEXT("ai"), TEXT("Cancelled"), TEXT("origin-editor"),
				Base, Base + FTimespan::FromSeconds(Index + 1),
				FString::ChrN(64, TEXT('a')), FString::ChrN(64, TEXT('b')), FString::ChrN(64, TEXT('c'))));
		}
		const FGuid HumanId = FGuid::NewGuid();
		ReplayCommandsWriteRun(*Harness.Fixture, HumanId, ReplayCommandsRetainedRunJson(
			HumanId, 1, TEXT("human"), TEXT("Cancelled"), TEXT("origin-editor"),
			Base, Base + FTimespan::FromSeconds(500),
			FString::ChrN(64, TEXT('d')), FString::ChrN(64, TEXT('e')), FString::ChrN(64, TEXT('f'))));
		return Ids;
	}();
	TestEqual(TEXT("Exactly 100 AI terminal runs retained"), TerminalRuns.Num(), 100);

	Harness.StartDomain();

	// The stable eligible set before any mutation; the aggregate must exceed one page budget.
	TArray<FCortexReplayMetadata> ExpectedMetadata;
	TestTrue(TEXT("Library lists eligible recordings"),
		Harness.Library->List(true, ExpectedMetadata).bSuccess);
	TSet<int32> ExpectedIds;
	TMap<int32, FCortexReplayMetadata> ExpectedById;
	int32 AggregateRowBytes = 0;
	for (const FCortexReplayMetadata& Metadata : ExpectedMetadata)
	{
		ExpectedIds.Add(Metadata.RecordingId);
		ExpectedById.Add(Metadata.RecordingId, Metadata);
	}
	TestEqual(TEXT("Disabled recording is not eligible"), ExpectedIds.Num(), MaxRecordings - 1);
	TestFalse(TEXT("Disabled recording omitted"), ExpectedIds.Contains(5));

	// Start one real AI run so every page carries a live active summary.
	const FCortexCommandResult Started = Harness.Execute(
		TEXT("replay.start_replay"), ReplayCommandsRecordingParams(1));
	TestTrue(TEXT("Active run admitted through the router"), Started.bSuccess);
	FString ActiveRunId;
	FGuid ParsedActiveRun;
	if (Started.Data.IsValid())
	{
		Started.Data->TryGetStringField(TEXT("run_id"), ActiveRunId);
	}
	TestTrue(TEXT("Active run has a canonical id"), FGuid::Parse(ActiveRunId, ParsedActiveRun));

	TSet<FGuid> TerminalRunSet;
	for (const FGuid& RunId : TerminalRuns)
	{
		TerminalRunSet.Add(RunId);
	}

	TSet<int32> VisitedIds;
	TSet<int32> RevokedIds;
	int32 After = 0;
	bool bHasMore = true;
	bool bFirstPage = true;
	int32 PageCount = 0;

	while (bHasMore && PageCount < 64)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("page_size"), 100);
		if (!bFirstPage)
		{
			Params->SetNumberField(TEXT("after_recording_id"), After);
		}

		const FCortexCommandResult Page = Harness.Execute(TEXT("replay.list_recordings"), Params);
		TestTrue(FString::Printf(TEXT("Page %d succeeds"), PageCount), Page.bSuccess);
		if (!Page.bSuccess || !Page.Data.IsValid())
		{
			return false;
		}

		const int32 EncodedBytes = ReplayCommandsUtf8Size(Page.Data);
		TestTrue(FString::Printf(TEXT("Page %d stays within the %d-byte budget"),
			PageCount, ReplayCommandsPageBudgetBytes),
			EncodedBytes >= 0 && EncodedBytes <= ReplayCommandsPageBudgetBytes);

		// Every page carries the full recovery window and the active summary.
		const TArray<TSharedPtr<FJsonValue>>* Recent = nullptr;
		Page.Data->TryGetArrayField(TEXT("recent_ai_runs"), Recent);
		TestEqual(FString::Printf(TEXT("Page %d carries 100 recovery summaries"), PageCount),
			Recent != nullptr ? Recent->Num() : 0, 100);
		if (Recent != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Recent)
			{
				const TSharedPtr<FJsonObject>* Summary = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(Summary) || Summary == nullptr)
				{
					continue;
				}
				FString RecoveredId;
				(*Summary)->TryGetStringField(TEXT("run_id"), RecoveredId);
				FGuid RecoveredGuid;
				TestTrue(FString::Printf(TEXT("Page %d recovery identity is a retained AI run"), PageCount),
					FGuid::Parse(RecoveredId, RecoveredGuid) && TerminalRunSet.Contains(RecoveredGuid));
			}
		}
		const TSharedPtr<FJsonObject>* Active = nullptr;
		TestTrue(FString::Printf(TEXT("Page %d carries the active summary"), PageCount),
			Page.Data->TryGetObjectField(TEXT("active_ai_run"), Active) && Active != nullptr);
		if (Active != nullptr)
		{
			FString ActiveId;
			(*Active)->TryGetStringField(TEXT("run_id"), ActiveId);
			TestEqual(FString::Printf(TEXT("Page %d active summary identity"), PageCount),
				ActiveId, ActiveRunId);
		}

		const TArray<TSharedPtr<FJsonValue>>* Rows = ReplayCommandsRows(Page);
		TestTrue(FString::Printf(TEXT("Page %d is non-empty"), PageCount),
			Rows != nullptr && Rows->Num() > 0);
		int32 LastId = After;
		if (Rows != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Rows)
			{
				const TSharedPtr<FJsonObject>* Row = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(Row) || Row == nullptr)
				{
					continue;
				}
				double IdValue = 0.0;
				(*Row)->TryGetNumberField(TEXT("recording_id"), IdValue);
				const int32 Id = static_cast<int32>(IdValue);
				TestTrue(FString::Printf(TEXT("Page %d row %d advances past the cursor"), PageCount, Id),
					Id > After);
				TestTrue(FString::Printf(TEXT("Page %d row %d is eligible"), PageCount, Id),
					ExpectedIds.Contains(Id) && !RevokedIds.Contains(Id));
				TestFalse(FString::Printf(TEXT("Page %d row %d is not duplicated"), PageCount, Id),
					VisitedIds.Contains(Id));
				VisitedIds.Add(Id);

				// Full metadata row (never trimmed to a bare ID).
				FString MapValue;
				(*Row)->TryGetStringField(TEXT("map_asset_path"), MapValue);
				TestEqual(FString::Printf(TEXT("Page %d row %d canonical map"), PageCount, Id),
					MapValue, MapPath);
				FString NameValue;
				(*Row)->TryGetStringField(TEXT("name"), NameValue);
				TestEqual(FString::Printf(TEXT("Page %d row %d full name"), PageCount, Id),
					NameValue, WorstName);
				FString DescriptionValue;
				(*Row)->TryGetStringField(TEXT("description"), DescriptionValue);
				TestEqual(FString::Printf(TEXT("Page %d row %d full description"), PageCount, Id),
					DescriptionValue, WorstDescription);

				const FCortexReplayMetadata* Expected = ExpectedById.Find(Id);
				if (Expected != nullptr)
				{
					FString InitialHash;
					FString InputsHash;
					(*Row)->TryGetStringField(TEXT("initial_state_sha256"), InitialHash);
					(*Row)->TryGetStringField(TEXT("inputs_sha256"), InputsHash);
					TestEqual(FString::Printf(TEXT("Page %d row %d initial-state hash"), PageCount, Id),
						InitialHash, Expected->InitialStateSha256);
					TestEqual(FString::Printf(TEXT("Page %d row %d inputs hash"), PageCount, Id),
						InputsHash, Expected->InputsSha256);
				}

				const int32 RowBytes = ReplayCommandsUtf8Size(*Row);
				AggregateRowBytes += FMath::Max(0, RowBytes);
				LastId = Id;
			}
		}

		bool bPageHasMore = false;
		Page.Data->TryGetBoolField(TEXT("has_more"), bPageHasMore);
		if (bPageHasMore)
		{
			double Next = 0.0;
			TestTrue(FString::Printf(TEXT("Page %d returns a next cursor"), PageCount),
				Page.Data->TryGetNumberField(TEXT("next_after_recording_id"), Next));
			TestEqual(FString::Printf(TEXT("Page %d cursor is the last included id"), PageCount),
				static_cast<int32>(Next), LastId);
		}
		else
		{
			TestTrue(FString::Printf(TEXT("Final page %d returns a null cursor"), PageCount),
				Page.Data->HasTypedField<EJson::Null>(TEXT("next_after_recording_id")));
		}

		// Revoke one not-yet-visited eligible recording between pages; later pages must skip it.
		if (PageCount == 0 && bPageHasMore)
		{
			int32 RevokedId = 0;
			for (const FCortexReplayMetadata& Metadata : ExpectedMetadata)
			{
				if (Metadata.RecordingId > LastId && !RevokedIds.Contains(Metadata.RecordingId))
				{
					RevokedId = Metadata.RecordingId;
					break;
				}
			}
			if (RevokedId != 0)
			{
				RevokedIds.Add(RevokedId);
				TestTrue(TEXT("Unvisited recording revoked between pages"),
					Harness.Library->SaveMetadata(RevokedId, WorstName, WorstDescription, false).bSuccess);
			}
		}

		After = LastId;
		bHasMore = bPageHasMore;
		bFirstPage = false;
		++PageCount;
	}

	TestTrue(TEXT("Multiple pages were required"), PageCount > 1);
	TestEqual(TEXT("Exactly one unvisited recording was revoked between pages"),
		RevokedIds.Num(), 1);
	TestTrue(TEXT("Aggregate eligible rows exceed one page budget"),
		AggregateRowBytes > ReplayCommandsPageBudgetBytes);
	TestEqual(TEXT("Every eligible recording was visited exactly once"),
		VisitedIds.Num(), ExpectedIds.Num() - RevokedIds.Num());
	for (const int32 Id : ExpectedIds)
	{
		if (!RevokedIds.Contains(Id))
		{
			TestTrue(FString::Printf(TEXT("Eligible recording %d visited"), Id), VisitedIds.Contains(Id));
		}
	}
	for (const int32 Id : RevokedIds)
	{
		TestFalse(FString::Printf(TEXT("Revoked recording %d never appears"), Id), VisitedIds.Contains(Id));
	}

	// Enabling a recording below an already-traversed cursor is only visible through a fresh
	// first-page discovery, never via the existing cursor.
	TestTrue(TEXT("Previously disabled recording enabled"),
		Harness.Library->SaveMetadata(5, WorstName, WorstDescription, true).bSuccess);
	{
		// A cursor above the newly enabled recording: it is below the traversed position.
		const int32 TraversedCursor = MaxRecordings / 2;
		TSharedPtr<FJsonObject> CursorParams = MakeShared<FJsonObject>();
		CursorParams->SetNumberField(TEXT("page_size"), 100);
		CursorParams->SetNumberField(TEXT("after_recording_id"), TraversedCursor);
		const FCortexCommandResult CursorPage = Harness.Execute(TEXT("replay.list_recordings"), CursorParams);
		TestTrue(TEXT("Continuation page succeeds"), CursorPage.bSuccess);
		const TArray<TSharedPtr<FJsonValue>>* Rows = ReplayCommandsRows(CursorPage);
		TestTrue(TEXT("Continuation page is non-empty"), Rows != nullptr && Rows->Num() > 0);
		if (Rows != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Rows)
			{
				const TSharedPtr<FJsonObject>* Row = nullptr;
				double IdValue = 0.0;
				if (Value.IsValid() && Value->TryGetObject(Row) && Row != nullptr
					&& (*Row)->TryGetNumberField(TEXT("recording_id"), IdValue))
				{
					TestFalse(TEXT("Continuation never reveals a below-cursor ID"),
						static_cast<int32>(IdValue) == 5);
				}
			}
		}
	}

	// A fresh discovery from the beginning must observe the newly enabled recording.
	bool bFreshSawEnabled = false;
	{
		TSet<int32> FreshVisited;
		int32 FreshAfter = 0;
		bool bFreshMore = true;
		bool bFreshFirst = true;
		int32 FreshPages = 0;
		while (bFreshMore && FreshPages < 64)
		{
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetNumberField(TEXT("page_size"), 100);
			if (!bFreshFirst)
			{
				Params->SetNumberField(TEXT("after_recording_id"), FreshAfter);
			}
			const FCortexCommandResult Page = Harness.Execute(TEXT("replay.list_recordings"), Params);
			TestTrue(FString::Printf(TEXT("Fresh page %d succeeds"), FreshPages), Page.bSuccess);
			if (!Page.bSuccess || !Page.Data.IsValid())
			{
				break;
			}
			const TArray<TSharedPtr<FJsonValue>>* Rows = ReplayCommandsRows(Page);
			int32 LastId = FreshAfter;
			if (Rows != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Rows)
				{
					const TSharedPtr<FJsonObject>* Row = nullptr;
					double IdValue = 0.0;
					if (Value.IsValid() && Value->TryGetObject(Row) && Row != nullptr
						&& (*Row)->TryGetNumberField(TEXT("recording_id"), IdValue))
					{
						const int32 Id = static_cast<int32>(IdValue);
						FreshVisited.Add(Id);
						bFreshSawEnabled |= (Id == 5);
						LastId = Id;
					}
				}
			}
			bool bMore = false;
			Page.Data->TryGetBoolField(TEXT("has_more"), bMore);
			FreshAfter = LastId;
			bFreshMore = bMore;
			bFreshFirst = false;
			++FreshPages;
		}
		TestTrue(TEXT("Fresh discovery observes the newly enabled recording"), bFreshSawEnabled);
		TestEqual(TEXT("Fresh discovery matches current eligibility"),
			FreshVisited.Num(), ExpectedIds.Num() - RevokedIds.Num() + 1);
	}

	// Release the active run and let any owned PIE teardown complete.
	const FCortexCommandResult Cancelled = Harness.Execute(
		TEXT("replay.cancel_replay"), ReplayCommandsRunParams(ActiveRunId));
	TestTrue(TEXT("Active run cancelled"), Cancelled.bSuccess);
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayCommandsAwaitIdle(
		this, Harness.ServiceRef(), Harness.Fixture));

	return true;
}

// ---------------------------------------------------------------------------
// Actual encoded-size boundary: the bracket/separator accounting must keep a
// budget-filled page at or below 39,000 compact UTF-8 bytes.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayCommandsBoundaryTest,
	"Cortex.Replay.Commands.PageBudgetBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCortexReplayCommandsBoundaryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	constexpr int32 BoundaryBudgetBytes = 39000;
	constexpr int32 BoundaryRecordingCount = 20;
	constexpr int32 BoundaryFirstRecordingId = 1000;

	// Legal maximum-size metadata: 128-scalar name, 1024-scalar description, long canonical map
	// path (all mixed 1/2/3/4-byte UTF-8 scalars so the byte count is non-trivial).
	const FString MaxName = ReplayCommandsMixedWidthText(128);
	const FString MaxDescription = ReplayCommandsMixedWidthText(1024);
	const FString LongMapPath = TEXT("/Game/Maps/PageBudget") + FString::ChrN(100, TEXT('M'));
	const FString LiveMapPath = ReplayCommandsPickMapPath();

	FCortexReplayCommandsHarness Harness;
	ReplayCommandsWriteTerminalRuns(Harness, 100);
	TMap<int32, FString> ExpectedMapPath;
	bool bPublished = true;
	for (int32 Index = 0; Index < BoundaryRecordingCount; ++Index)
	{
		const int32 Id = BoundaryFirstRecordingId + Index;
		// The first recording must load for the live active summary; the rest use the long path.
		const FString MapPath = (Index == 0) ? LiveMapPath : LongMapPath;
		ExpectedMapPath.Add(Id, MapPath);
		bPublished &= ReplayCommandsPublish(*this, *Harness.Fixture, *Harness.Library, Id, true,
			{ ReplayCommandsKeyDown(0) }, MaxName, MaxDescription, MapPath,
			(Index == 0) ? FString(TEXT("/Script/Engine.DefaultPawn")) : FString());
	}
	if (!bPublished)
	{
		AddError(TEXT("Boundary recordings could not be published"));
		return true;
	}
	Harness.StartDomain();

	// One live AI run so every page carries an active summary.
	const FCortexCommandResult Started = Harness.Execute(
		TEXT("replay.start_replay"), ReplayCommandsRecordingParams(BoundaryFirstRecordingId));
	TestTrue(TEXT("Boundary active run admitted"), Started.bSuccess);
	if (!Started.bSuccess || !Started.Data.IsValid())
	{
		AddError(TEXT("Boundary active run was not admitted"));
		return true;
	}
	FString ActiveRunId;
	Started.Data->TryGetStringField(TEXT("run_id"), ActiveRunId);
	AddInfo(FString::Printf(TEXT("PageBudgetBoundary active run: %s"), *ActiveRunId));

	// Genuinely empty page (no rows) for the non-row baseline; it carries the same recovery and
	// active summaries every real page carries.
	int32 EmptyBaselineBytes = 0;
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("page_size"), 100);
		Params->SetNumberField(TEXT("after_recording_id"), 1000000);
		const FCortexCommandResult Page = Harness.Execute(TEXT("replay.list_recordings"), Params);
		TestTrue(TEXT("Empty baseline page succeeds"), Page.bSuccess);
		const TArray<TSharedPtr<FJsonValue>>* Rows = ReplayCommandsRows(Page);
		TestEqual(TEXT("Empty baseline page has no rows"), Rows != nullptr ? Rows->Num() : -1, 0);
		EmptyBaselineBytes = ReplayCommandsUtf8Size(Page.Data);
	}
	AddInfo(FString::Printf(TEXT("PageBudgetBoundary empty baseline bytes: %d"), EmptyBaselineBytes));
	if (EmptyBaselineBytes <= 0)
	{
		AddError(TEXT("Boundary empty baseline could not be measured"));
		return true;
	}

	TSet<int32> VisitedIds;
	int32 SumAllRowBytes = 0;
	int32 After = 0;
	bool bFirst = true;
	bool bMore = true;
	int32 Pages = 0;
	while (bMore && Pages < 64)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("page_size"), 100);
		if (!bFirst)
		{
			Params->SetNumberField(TEXT("after_recording_id"), After);
		}
		const FCortexCommandResult Page = Harness.Execute(TEXT("replay.list_recordings"), Params);
		TestTrue(FString::Printf(TEXT("Boundary page %d succeeds"), Pages), Page.bSuccess);
		if (!Page.bSuccess || !Page.Data.IsValid())
		{
			AddError(FString::Printf(TEXT("Boundary page %d returned no data"), Pages));
			return true;
		}

		const int32 Emitted = ReplayCommandsUtf8Size(Page.Data);
		TestTrue(FString::Printf(TEXT("Boundary page %d is within the encoded budget"), Pages),
			Emitted <= BoundaryBudgetBytes);

		// Recovery/active summaries are never trimmed.
		const TArray<TSharedPtr<FJsonValue>>* Recent = nullptr;
		Page.Data->TryGetArrayField(TEXT("recent_ai_runs"), Recent);
		TestEqual(FString::Printf(TEXT("Boundary page %d keeps 100 recovery summaries"), Pages),
			Recent != nullptr ? Recent->Num() : 0, 100);
		const TSharedPtr<FJsonObject>* Active = nullptr;
		TestTrue(FString::Printf(TEXT("Boundary page %d keeps the active summary"), Pages),
			Page.Data->TryGetObjectField(TEXT("active_ai_run"), Active) && Active != nullptr);
		if (Active != nullptr)
		{
			FString ActiveId;
			(*Active)->TryGetStringField(TEXT("run_id"), ActiveId);
			TestEqual(FString::Printf(TEXT("Boundary page %d active identity"), Pages),
				ActiveId, ActiveRunId);
		}

		const TArray<TSharedPtr<FJsonValue>>* Rows = ReplayCommandsRows(Page);
		const int32 RowCountOnPage = Rows != nullptr ? Rows->Num() : 0;
		TestTrue(FString::Printf(TEXT("Boundary page %d is non-empty"), Pages), RowCountOnPage > 0);

		int64 SumRowBytes = 0;
		int32 LastId = After;
		if (Rows != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Rows)
			{
				const TSharedPtr<FJsonObject>* Row = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(Row) || Row == nullptr)
				{
					AddError(FString::Printf(TEXT("Boundary page %d has a non-object row"), Pages));
					continue;
				}
				double IdValue = 0.0;
				(*Row)->TryGetNumberField(TEXT("recording_id"), IdValue);
				const int32 Id = static_cast<int32>(IdValue);
				TestTrue(FString::Printf(TEXT("Boundary page %d row %d advances"), Pages, Id), Id > After);
				TestFalse(FString::Printf(TEXT("Boundary page %d row %d not duplicated"), Pages, Id),
					VisitedIds.Contains(Id));
				VisitedIds.Add(Id);
				LastId = Id;
				const int32 RowBytes = ReplayCommandsUtf8Size(*Row);
				SumRowBytes += RowBytes;
				SumAllRowBytes += RowBytes;

				// Full metadata row: no field was trimmed to fit the budget.
				FString NameValue;
				FString DescriptionValue;
				FString MapValue;
				(*Row)->TryGetStringField(TEXT("name"), NameValue);
				(*Row)->TryGetStringField(TEXT("description"), DescriptionValue);
				(*Row)->TryGetStringField(TEXT("map_asset_path"), MapValue);
				TestEqual(FString::Printf(TEXT("Boundary row %d name intact"), Id), NameValue, MaxName);
				TestEqual(FString::Printf(TEXT("Boundary row %d description intact"), Id),
					DescriptionValue, MaxDescription);
				const FString* ExpectedMap = ExpectedMapPath.Find(Id);
				TestTrue(FString::Printf(TEXT("Boundary row %d map intact"), Id),
					ExpectedMap != nullptr && MapValue == *ExpectedMap);
				TestTrue(FString::Printf(TEXT("Boundary row %d hashes intact"), Id),
					(*Row)->HasField(TEXT("initial_state_sha256"))
						&& (*Row)->HasField(TEXT("inputs_sha256")));
			}
		}

		// The fix's identity: for n >= 2 rows the emitted page is the non-row baseline plus every
		// row plus one comma separator each. The baseline is the genuinely empty page, adjusted
		// only by the bounded has_more/cursor rewrite (ids here are always four digits).
		if (RowCountOnPage >= 2 && SumRowBytes > 0)
		{
			const int64 DerivedBaseline =
				static_cast<int64>(Emitted) - SumRowBytes - (RowCountOnPage - 1);
			TestTrue(FString::Printf(TEXT("Boundary page %d separator accounting"), Pages),
				DerivedBaseline == static_cast<int64>(EmptyBaselineBytes)
					|| DerivedBaseline == static_cast<int64>(EmptyBaselineBytes) - 1);
			AddInfo(FString::Printf(
				TEXT("PageBudgetBoundary page %d rows %d emitted %d baseline %d"),
				Pages, RowCountOnPage, Emitted, static_cast<int32>(DerivedBaseline)));
		}

		bool bPageMore = false;
		Page.Data->TryGetBoolField(TEXT("has_more"), bPageMore);
		if (bPageMore)
		{
			double Next = 0.0;
			TestTrue(FString::Printf(TEXT("Boundary page %d has a next cursor"), Pages),
				Page.Data->TryGetNumberField(TEXT("next_after_recording_id"), Next));
			TestEqual(FString::Printf(TEXT("Boundary page %d cursor is the last id"), Pages),
				static_cast<int32>(Next), LastId);
		}
		else
		{
			TestTrue(FString::Printf(TEXT("Final boundary page %d has a null cursor"), Pages),
				Page.Data->HasTypedField<EJson::Null>(TEXT("next_after_recording_id")));
		}

		After = LastId;
		bMore = bPageMore;
		bFirst = false;
		++Pages;
	}
	AddInfo(FString::Printf(TEXT("PageBudgetBoundary pages %d visited %d row bytes %d"),
		Pages, VisitedIds.Num(), SumAllRowBytes));
	TestTrue(TEXT("Boundary produced at least one page"), Pages >= 1);
	TestEqual(TEXT("Every boundary recording was visited"), VisitedIds.Num(), BoundaryRecordingCount);

	// Explicit two-row boundary case. With the corrected budget two legal maximum-metadata rows fit
	// alongside the same 100 recovery summaries (baseline + both rows + exactly one comma <=
	// 39,000), so the page must return exactly two rows; the old double-counted budget returned one
	// row here, making this case the direct regression guard for that defect.
	{
		TSharedPtr<FJsonObject> TwoParams = MakeShared<FJsonObject>();
		TwoParams->SetNumberField(TEXT("page_size"), 2);
		const FCortexCommandResult TwoPage = Harness.Execute(TEXT("replay.list_recordings"), TwoParams);
		TestTrue(TEXT("Two-row boundary page succeeds"), TwoPage.bSuccess);
		if (!TwoPage.bSuccess || !TwoPage.Data.IsValid())
		{
			AddError(TEXT("Two-row boundary page returned no data"));
		}
		else
		{
			const TArray<TSharedPtr<FJsonValue>>* TwoRows = ReplayCommandsRows(TwoPage);
			const int32 TwoRowCount = TwoRows != nullptr ? TwoRows->Num() : 0;
			TestTrue(TEXT("Two-row boundary page returns two rows"), TwoRowCount == 2);
			if (TwoRowCount != 2)
			{
				AddError(FString::Printf(
					TEXT("Two-row boundary page returned %d rows for page_size 2"), TwoRowCount));
			}
			else
			{
				const int32 TwoEmitted = ReplayCommandsUtf8Size(TwoPage.Data);
				const int32 Row1Bytes = ReplayCommandsUtf8Size((*TwoRows)[0]->AsObject());
				const int32 Row2Bytes = ReplayCommandsUtf8Size((*TwoRows)[1]->AsObject());
				TestTrue(TEXT("Two-row boundary page is within the encoded budget"),
					TwoEmitted <= BoundaryBudgetBytes);
				// Emitted == empty baseline + both rows + exactly one comma, allowing only the
				// bounded has_more/cursor rewrite (one byte here: false -> true).
				const int64 TwoBaseline = static_cast<int64>(TwoEmitted) - Row1Bytes - Row2Bytes - 1;
				TestTrue(TEXT("Two-row page counts exactly one separator"),
					TwoBaseline == static_cast<int64>(EmptyBaselineBytes)
						|| TwoBaseline == static_cast<int64>(EmptyBaselineBytes) - 1);
				AddInfo(FString::Printf(
					TEXT("PageBudgetBoundary two-row emitted %d row1 %d row2 %d baseline %d"),
					TwoEmitted, Row1Bytes, Row2Bytes, EmptyBaselineBytes));
			}
		}
	}

	// Release the active run and let any owned PIE teardown complete.
	const FCortexCommandResult Cancelled = Harness.Execute(
		TEXT("replay.cancel_replay"), ReplayCommandsRunParams(ActiveRunId));
	TestTrue(TEXT("Boundary active run cancelled"), Cancelled.bSuccess);
	ADD_LATENT_AUTOMATION_COMMAND(FCortexReplayCommandsAwaitIdle(
		this, Harness.ServiceRef(), Harness.Fixture));

	return true;
}
