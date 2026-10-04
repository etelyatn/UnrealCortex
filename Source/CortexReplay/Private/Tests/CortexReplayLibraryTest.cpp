#include "Misc/AutomationTest.h"

#include "CortexReplayErrorCodes.h"
#include "CortexReplayLibrary.h"
#include "CortexReplayTestUtils.h"
#include "CortexReplayTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Templates/Function.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace
{
FCortexReplayEvent MakeKeyDownEvent(int32 Sequence, double TimeSeconds, const FKey& Key = EKeys::SpaceBar)
{
	FCortexReplayEvent Event;
	Event.Sequence = Sequence;
	Event.TimeSeconds = TimeSeconds;
	Event.Input.Kind = ECortexEditorPhysicalInputKind::KeyDown;
	Event.Input.Key = Key;
	return Event;
}

TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity> MakeWidgetIdentity()
{
	TSharedPtr<FCortexEditorPhysicalInputWidgetIdentity> Identity =
		MakeShared<FCortexEditorPhysicalInputWidgetIdentity>();
	Identity->RootClassPath = TEXT("/Game/UI/WBP_ReplayTest.WBP_ReplayTest_C");
	Identity->WidgetAncestry.Add(FName(TEXT("Root")));
	Identity->IdentitySha256 = FString::ChrN(64, TEXT('a'));
	return Identity;
}

bool LoadTextFile(const FString& Path, FString& OutText)
{
	return FFileHelper::LoadFileToString(OutText, *Path);
}

TSharedPtr<FJsonObject> LoadJsonObject(const FString& Path)
{
	FString Text;
	if (!LoadTextFile(Path, Text))
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Object))
	{
		return nullptr;
	}

	return Object;
}

bool SaveJsonObject(const FString& Path, const TSharedPtr<FJsonObject>& Object)
{
	if (!Object.IsValid())
	{
		return false;
	}

	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	if (!FJsonSerializer::Serialize(Object.ToSharedRef(), Writer))
	{
		return false;
	}

	return FFileHelper::SaveStringToFile(Text, *Path);
}

bool SaveJsonLine(const FString& Path, const TSharedPtr<FJsonObject>& Object)
{
	if (!Object.IsValid())
	{
		return false;
	}

	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	if (!FJsonSerializer::Serialize(Object.ToSharedRef(), Writer))
	{
		return false;
	}

	Text += TEXT("\n");
	return FFileHelper::SaveStringToFile(Text, *Path);
}

TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity> MakeWidgetIdentityWithPath(const FString& RootClassPath)
{
	TSharedPtr<FCortexEditorPhysicalInputWidgetIdentity> Identity =
		MakeShared<FCortexEditorPhysicalInputWidgetIdentity>();
	Identity->RootKind = ECortexEditorUIRootKind::UMG;
	Identity->Surface = ECortexEditorUISurface::Viewport;
	Identity->Discriminator = ECortexEditorUIRootDiscriminator::SingletonClass;
	Identity->RootClassPath = RootClassPath;
	Identity->WidgetAncestry.Add(FName(TEXT("Root")));
	Identity->IdentitySha256 = FString::ChrN(64, TEXT('a'));
	return TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity>(Identity);
}

FCortexReplayInteractionGuard MakeSupportedGuard(const FString& RootClassPath)
{
	FCortexReplayInteractionGuard Guard;
	Guard.UICoverage = ECortexEditorUICoverage::Supported;
	Guard.UIUnavailableReason = ECortexEditorUIUnavailableReason::None;
	Guard.UITarget = MakeWidgetIdentityWithPath(RootClassPath);
	Guard.ExpectedLocalPosition = FVector2D(0.5, 0.5);
	return Guard;
}

TSharedPtr<FCortexEditorPhysicalInputWidgetIdentity> MakeMutableUmgIdentity(const FString& RootClassPath)
{
	TSharedPtr<FCortexEditorPhysicalInputWidgetIdentity> Identity =
		MakeShared<FCortexEditorPhysicalInputWidgetIdentity>();
	Identity->RootKind = ECortexEditorUIRootKind::UMG;
	Identity->Surface = ECortexEditorUISurface::Viewport;
	Identity->Discriminator = ECortexEditorUIRootDiscriminator::SingletonClass;
	Identity->RootClassPath = RootClassPath;
	Identity->WidgetAncestry.Add(FName(TEXT("Root")));
	return Identity;
}

FString ReplayRootPath(const FCortexReplayTestFixture& Fixture)
{
	return FPaths::Combine(Fixture.GetProjectRoot(), TEXT(".cortex/replay"));
}

FString ReplaySubPath(const FCortexReplayTestFixture& Fixture, const TCHAR* Relative)
{
	return FPaths::Combine(ReplayRootPath(Fixture), Relative);
}

FString RecordingDirectoryPath(const FCortexReplayTestFixture& Fixture, int32 Id)
{
	return FPaths::Combine(FPaths::Combine(ReplayRootPath(Fixture), TEXT("recordings")), FString::FromInt(Id));
}

TSharedPtr<FJsonObject> GetGuardObject(const TSharedPtr<FJsonObject>& Row)
{
	const TSharedPtr<FJsonObject>* Guard = nullptr;
	if (!Row.IsValid() || !Row->TryGetObjectField(TEXT("guard"), Guard) || Guard == nullptr)
	{
		return nullptr;
	}

	return *Guard;
}

TSharedPtr<FJsonObject> GetGuardUiObject(const TSharedPtr<FJsonObject>& Row)
{
	const TSharedPtr<FJsonObject> Guard = GetGuardObject(Row);
	if (!Guard.IsValid())
	{
		return nullptr;
	}

	const TSharedPtr<FJsonObject>* Ui = nullptr;
	if (!Guard->TryGetObjectField(TEXT("ui"), Ui) || Ui == nullptr)
	{
		return nullptr;
	}

	return *Ui;
}

TSharedPtr<FJsonObject> GetGuardIdentityObject(const TSharedPtr<FJsonObject>& Row)
{
	const TSharedPtr<FJsonObject> Ui = GetGuardUiObject(Row);
	if (!Ui.IsValid())
	{
		return nullptr;
	}

	const TSharedPtr<FJsonObject>* Identity = nullptr;
	if (!Ui->TryGetObjectField(TEXT("identity"), Identity) || Identity == nullptr)
	{
		return nullptr;
	}

	return *Identity;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryIdPublicationTest,
	"Cortex.Replay.Library.IdPublicationAndReuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryIdPublicationTest::RunTest(const FString& Parameters)
{
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());
	int32 First = 0;
	TestTrue(TEXT("Reserve first"), Library.ReserveId(First).bSuccess);
	TestTrue(TEXT("Publish first"), Library.Publish(Fixture.MakeRecording(First, false, {})).bSuccess);
	TestTrue(TEXT("Delete highest"), Library.Delete(First).bSuccess);
	FCortexReplayLibrary Reopened(Fixture.GetProjectRoot());
	int32 Next = 0;
	TestTrue(TEXT("Reserve after reopen"), Reopened.ReserveId(Next).bSuccess);
	TestEqual(TEXT("Deleted ID is not reused"), Next, First + 1);

	// A failed publication must not leave a playable partial record behind.
	int32 Partial = 0;
	TestTrue(TEXT("Reserve partial"), Reopened.ReserveId(Partial).bSuccess);

	FCortexReplaySnapshot Malformed = Fixture.MakeRecording(Partial, false, { MakeKeyDownEvent(0, 0.0) });
	Malformed.Events[0].Guard.Reset();
	TestFalse(TEXT("Malformed publish rejected"), Reopened.Publish(Malformed).bSuccess);

	TSharedPtr<const FCortexReplaySnapshot> PartialSnapshot;
	TestFalse(TEXT("Partial record is not loadable"), Reopened.Load(Partial, false, PartialSnapshot).bSuccess);

	TArray<FCortexReplayMetadata> Records;
	TestTrue(TEXT("List after partial publish"), Reopened.List(false, Records).bSuccess);
	bool bPartialListed = false;
	for (const FCortexReplayMetadata& Record : Records)
	{
		if (Record.RecordingId == Partial)
		{
			bPartialListed = true;
		}
	}
	TestFalse(TEXT("Partial record is not listed"), bPartialListed);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryReservationLockTest,
	"Cortex.Replay.Library.ReservationLock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryReservationLockTest::RunTest(const FString& Parameters)
{
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary First(Fixture.GetProjectRoot());
	FCortexReplayLibrary Second(Fixture.GetProjectRoot());

	int32 IdA = 0;
	int32 IdB = 0;
	TestTrue(TEXT("First reservation"), First.ReserveId(IdA).bSuccess);
	TestTrue(TEXT("Second reservation"), Second.ReserveId(IdB).bSuccess);
	TestNotEqual(TEXT("Independent libraries never share an ID"), IdA, IdB);

	// The committed counter, not an in-memory cache, must survive a reopen.
	FCortexReplayLibrary Reopened(Fixture.GetProjectRoot());
	int32 IdC = 0;
	TestTrue(TEXT("Reopened reservation"), Reopened.ReserveId(IdC).bSuccess);
	TestTrue(TEXT("Reservations are durable and strictly increasing"), IdC > IdB && IdB > IdA);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryInitialStateTest,
	"Cortex.Replay.Library.InitialStateValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryInitialStateTest::RunTest(const FString& Parameters)
{
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 0;
	TestTrue(TEXT("Reserve"), Library.ReserveId(Id).bSuccess);

	{
		FCortexReplaySnapshot Snapshot = Fixture.MakeRecording(Id, false, {});
		Snapshot.InitialState.RecordingId = 0;
		TestFalse(TEXT("Missing initial-state id rejected"), Library.Publish(Snapshot).bSuccess);
	}

	{
		FCortexReplaySnapshot Snapshot = Fixture.MakeRecording(Id, false, {});
		Snapshot.Metadata.RecordingId = Id;
		Snapshot.InitialState.RecordingId = Id + 1;
		TestFalse(TEXT("Mismatched initial-state id rejected"), Library.Publish(Snapshot).bSuccess);
	}

	{
		FCortexReplaySnapshot Snapshot = Fixture.MakeRecording(Id, false, {});
		Snapshot.InitialState.SchemaVersion = 2;
		TestFalse(TEXT("Mismatched initial-state schema rejected"), Library.Publish(Snapshot).bSuccess);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryGuardCoverageTest,
	"Cortex.Replay.Library.GuardAndCoverageValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryGuardCoverageTest::RunTest(const FString& Parameters)
{
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 0;
	TestTrue(TEXT("Reserve"), Library.ReserveId(Id).bSuccess);

	{
		FCortexReplaySnapshot Snapshot = Fixture.MakeRecording(Id, false, { MakeKeyDownEvent(0, 0.0) });
		Snapshot.Events[0].Guard.Reset();
		TestFalse(TEXT("Missing press guard rejected"), Library.Publish(Snapshot).bSuccess);
	}

	{
		FCortexReplaySnapshot Snapshot = Fixture.MakeRecording(Id, false, { MakeKeyDownEvent(0, 0.0) });
		TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity> EmptyIdentity =
			MakeShared<FCortexEditorPhysicalInputWidgetIdentity>();
		Snapshot.Events[0].Guard->UICoverage = ECortexEditorUICoverage::Supported;
		Snapshot.Events[0].Guard->UITarget = EmptyIdentity;
		Snapshot.Events[0].Guard->ExpectedLocalPosition = FVector2D(0.5, 0.5);
		TestFalse(TEXT("Malformed selector rejected"), Library.Publish(Snapshot).bSuccess);
	}

	{
		FCortexReplaySnapshot Snapshot = Fixture.MakeRecording(Id, false, { MakeKeyDownEvent(0, 0.0) });
		Snapshot.Events[0].Guard->UICoverage = ECortexEditorUICoverage::Supported;
		Snapshot.Events[0].Guard->UITarget = MakeWidgetIdentity();
		Snapshot.Events[0].Guard->ExpectedLocalPosition = FVector2D(1.5, 0.5);
		TestFalse(TEXT("Out-of-range local position rejected"), Library.Publish(Snapshot).bSuccess);
	}

	{
		FCortexReplaySnapshot Snapshot = Fixture.MakeRecording(Id, false, {});
		Snapshot.InitialState.Pose.PawnTransform.SetScale3D(FVector::ZeroVector);
		TestFalse(TEXT("Degenerate pawn scale rejected"), Library.Publish(Snapshot).bSuccess);
	}

	{
		FCortexReplaySnapshot Snapshot = Fixture.MakeRecording(Id, false, {});
		Snapshot.InitialState.Pose.PawnTransform.SetLocation(FVector(NAN, 0.0, 0.0));
		TestFalse(TEXT("Non-finite pawn location rejected"), Library.Publish(Snapshot).bSuccess);
	}

	{
		FCortexReplaySnapshot Snapshot = Fixture.MakeRecording(Id, false, { MakeKeyDownEvent(0, 0.0) });
		Snapshot.Metadata.GuardCoverage.PosePresses += 1;
		TestFalse(TEXT("Coverage total mismatch rejected"), Library.Publish(Snapshot).bSuccess);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryTamperedInitialBytesTest,
	"Cortex.Replay.Library.TamperedInitialBytes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryTamperedInitialBytesTest::RunTest(const FString& Parameters)
{
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 0;
	TestTrue(TEXT("Reserve"), Library.ReserveId(Id).bSuccess);
	TestTrue(TEXT("Publish"), Library.Publish(Fixture.MakeRecording(Id, false, {})).bSuccess);

	const FString InitialPath = Fixture.GetInitialStatePath(Id);
	TestTrue(TEXT("Initial-state file exists"), FPaths::FileExists(InitialPath));
	TestTrue(TEXT("Tamper initial bytes"),
		FFileHelper::SaveStringToFile(TEXT("{\"schema_version\":1}"), *InitialPath));

	TSharedPtr<const FCortexReplaySnapshot> Snapshot;
	FCortexReplayLibrary Reopened(Fixture.GetProjectRoot());
	TestFalse(TEXT("Tampered initial bytes deny load"), Reopened.Load(Id, false, Snapshot).bSuccess);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryTamperedInputBytesTest,
	"Cortex.Replay.Library.TamperedInputBytes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryTamperedInputBytesTest::RunTest(const FString& Parameters)
{
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 0;
	TestTrue(TEXT("Reserve"), Library.ReserveId(Id).bSuccess);
	TestTrue(TEXT("Publish"), Library.Publish(Fixture.MakeRecording(Id, false, { MakeKeyDownEvent(0, 0.0) })).bSuccess);

	const FString InputsPath = Fixture.GetInputsPath(Id);
	TestTrue(TEXT("Inputs file exists"), FPaths::FileExists(InputsPath));
	TestTrue(TEXT("Tamper input bytes"),
		FFileHelper::SaveStringToFile(TEXT("{\"schema_version\":1}\n"), *InputsPath));

	TSharedPtr<const FCortexReplaySnapshot> Snapshot;
	FCortexReplayLibrary Reopened(Fixture.GetProjectRoot());
	TestFalse(TEXT("Tampered input bytes deny load"), Reopened.Load(Id, false, Snapshot).bSuccess);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryConflictingHashesTest,
	"Cortex.Replay.Library.ConflictingHashes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryConflictingHashesTest::RunTest(const FString& Parameters)
{
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 0;
	TestTrue(TEXT("Reserve"), Library.ReserveId(Id).bSuccess);
	TestTrue(TEXT("Publish"), Library.Publish(Fixture.MakeRecording(Id, false, {})).bSuccess);

	const FString RecordingsRoot = FPaths::Combine(Fixture.GetProjectRoot(), TEXT(".cortex/replay/recordings"));
	const FString RecordingDirectory = FPaths::Combine(RecordingsRoot, FString::FromInt(Id));
	const FString MetadataPath = FPaths::Combine(RecordingDirectory, TEXT("metadata.json"));
	TSharedPtr<FJsonObject> Metadata = LoadJsonObject(MetadataPath);
	TestTrue(TEXT("Metadata is readable"), Metadata.IsValid());
	if (Metadata.IsValid())
	{
		Metadata->SetStringField(TEXT("inputs_sha256"), FString::ChrN(64, TEXT('b')));
		TestTrue(TEXT("Rewrite conflicting hash"), SaveJsonObject(MetadataPath, Metadata));
	}

	TSharedPtr<const FCortexReplaySnapshot> Snapshot;
	FCortexReplayLibrary Reopened(Fixture.GetProjectRoot());
	TestFalse(TEXT("Conflicting hashes deny load"), Reopened.Load(Id, false, Snapshot).bSuccess);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibrarySaveMetadataTest,
	"Cortex.Replay.Library.SaveMetadataPreservation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibrarySaveMetadataTest::RunTest(const FString& Parameters)
{
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 0;
	TestTrue(TEXT("Reserve"), Library.ReserveId(Id).bSuccess);
	TestTrue(TEXT("Publish"), Library.Publish(Fixture.MakeRecording(Id, false, { MakeKeyDownEvent(0, 0.0) })).bSuccess);

	FString InitialBytes;
	FString InputBytes;
	TestTrue(TEXT("Read initial bytes"), LoadTextFile(Fixture.GetInitialStatePath(Id), InitialBytes));
	TestTrue(TEXT("Read input bytes"), LoadTextFile(Fixture.GetInputsPath(Id), InputBytes));

	TSharedPtr<const FCortexReplaySnapshot> Before;
	TestTrue(TEXT("Load before save"), Library.Load(Id, false, Before).bSuccess);
	const FString CreatedAt = Before->Metadata.CreatedAtUtc.ToString();
	const FString MapPath = Before->Metadata.MapAssetPath;
	const FString InitialHash = Before->Metadata.InitialStateSha256;
	const FString InputsHash = Before->Metadata.InputsSha256;

	TestTrue(TEXT("Save metadata"),
		Library.SaveMetadata(Id, TEXT("Renamed"), TEXT("Edited description"), true).bSuccess);

	FCortexReplayLibrary Reopened(Fixture.GetProjectRoot());
	TSharedPtr<const FCortexReplaySnapshot> After;
	TestTrue(TEXT("Load after save"), Reopened.Load(Id, false, After).bSuccess);
	TestEqual(TEXT("Recording ID preserved"), After->Metadata.RecordingId, Id);
	TestEqual(TEXT("Created date preserved"), After->Metadata.CreatedAtUtc.ToString(), CreatedAt);
	TestEqual(TEXT("Map preserved"), After->Metadata.MapAssetPath, MapPath);
	TestEqual(TEXT("Initial hash preserved"), After->Metadata.InitialStateSha256, InitialHash);
	TestEqual(TEXT("Inputs hash preserved"), After->Metadata.InputsSha256, InputsHash);
	TestEqual(TEXT("Name updated"), After->Metadata.Name, FString(TEXT("Renamed")));
	TestEqual(TEXT("Description updated"), After->Metadata.Description, FString(TEXT("Edited description")));
	TestTrue(TEXT("AI flag updated"), After->Metadata.bAIEnabled);

	FString InitialAfter;
	FString InputAfter;
	TestTrue(TEXT("Read initial bytes after save"), LoadTextFile(Fixture.GetInitialStatePath(Id), InitialAfter));
	TestTrue(TEXT("Read input bytes after save"), LoadTextFile(Fixture.GetInputsPath(Id), InputAfter));
	TestEqual(TEXT("Initial bytes unchanged"), InitialAfter, InitialBytes);
	TestEqual(TEXT("Input bytes unchanged"), InputAfter, InputBytes);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryStrictIntegerTest,
	"Cortex.Replay.Library.StrictIntegerValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryStrictIntegerTest::RunTest(const FString& Parameters)
{
	FCortexReplayTestFixture Fixture;
	TestTrue(TEXT("Create replay root"), IFileManager::Get().MakeDirectory(*ReplayRootPath(Fixture), true));

	const FString LibraryPath = ReplaySubPath(Fixture, TEXT("library.json"));

	{
		TSharedPtr<FJsonObject> LibraryObject = MakeShared<FJsonObject>();
		LibraryObject->SetNumberField(TEXT("schema_version"), 1);
		LibraryObject->SetNumberField(TEXT("next_recording_id"), 1.4);
		TestTrue(TEXT("Write fractional counter"), SaveJsonObject(LibraryPath, LibraryObject));

		FCortexReplayLibrary Library(Fixture.GetProjectRoot());
		int32 Id = 0;
		const FCortexCommandResult Result = Library.ReserveId(Id);
		TestFalse(TEXT("Fractional counter is rejected"), Result.bSuccess);
		TestEqual(TEXT("Fractional counter is a format error"), Result.ErrorCode, FString(CortexReplayErrorCodes::UnsupportedRecordingFormat));
	}

	{
		TSharedPtr<FJsonObject> LibraryObject = MakeShared<FJsonObject>();
		LibraryObject->SetNumberField(TEXT("schema_version"), 1);
		LibraryObject->SetStringField(TEXT("next_recording_id"), TEXT("1"));
		TestTrue(TEXT("Write numeric-string counter"), SaveJsonObject(LibraryPath, LibraryObject));

		FCortexReplayLibrary Library(Fixture.GetProjectRoot());
		int32 Id = 0;
		TestFalse(TEXT("Numeric-string counter is rejected"), Library.ReserveId(Id).bSuccess);
	}

	IFileManager::Get().Delete(*LibraryPath, false, true, true);

	FCortexReplayLibrary Library(Fixture.GetProjectRoot());
	int32 Id = 0;
	TestTrue(TEXT("Reserve"), Library.ReserveId(Id).bSuccess);
	TestTrue(TEXT("Publish"), Library.Publish(Fixture.MakeRecording(Id, false, { MakeKeyDownEvent(0, 0.0) })).bSuccess);

	const FString MetadataPath = FPaths::Combine(RecordingDirectoryPath(Fixture, Id), TEXT("metadata.json"));
	FString OriginalMetadata;
	TestTrue(TEXT("Read original metadata"), LoadTextFile(MetadataPath, OriginalMetadata));

	auto TamperMetadata = [&](const TCHAR* Label, TFunctionRef<void(TSharedPtr<FJsonObject>&)> Mutate)
	{
		TestTrue(Label, FFileHelper::SaveStringToFile(OriginalMetadata, *MetadataPath));
		TSharedPtr<FJsonObject> Metadata = LoadJsonObject(MetadataPath);
		if (!Metadata.IsValid())
		{
			return;
		}

		Mutate(Metadata);
		TestTrue(TEXT("Write tampered metadata"), SaveJsonObject(MetadataPath, Metadata));
	};

	auto ExpectFormatError = [&](const TCHAR* Label)
	{
		TSharedPtr<const FCortexReplaySnapshot> Snapshot;
		FCortexReplayLibrary Reopened(Fixture.GetProjectRoot());
		const FCortexCommandResult Result = Reopened.Load(Id, false, Snapshot);
		TestFalse(Label, Result.bSuccess);
		TestEqual(TEXT("Rejected as an unsupported format"), Result.ErrorCode, FString(CortexReplayErrorCodes::UnsupportedRecordingFormat));
	};

	TamperMetadata(TEXT("Tamper schema_version"), [](TSharedPtr<FJsonObject>& Metadata)
	{
		Metadata->SetNumberField(TEXT("schema_version"), 1.4);
	});
	ExpectFormatError(TEXT("Fractional schema_version is rejected"));

	TamperMetadata(TEXT("Tamper recording_id"), [](TSharedPtr<FJsonObject>& Metadata)
	{
		Metadata->SetStringField(TEXT("recording_id"), TEXT("1"));
	});
	ExpectFormatError(TEXT("Numeric-string recording_id is rejected"));

	TamperMetadata(TEXT("Tamper coverage"), [](TSharedPtr<FJsonObject>& Metadata)
	{
		const TSharedPtr<FJsonObject>* Coverage = nullptr;
		if (Metadata->TryGetObjectField(TEXT("guard_coverage"), Coverage) && Coverage != nullptr)
		{
			(*Coverage)->SetNumberField(TEXT("pose_presses"), 0.5);
		}
	});
	ExpectFormatError(TEXT("Fractional coverage count is rejected"));

	// Sequence lives in inputs.jsonl; strict validation must reject it before any hash comparison.
	TestTrue(TEXT("Restore metadata"), FFileHelper::SaveStringToFile(OriginalMetadata, *MetadataPath));
	const FString InputsPath = Fixture.GetInputsPath(Id);
	TSharedPtr<FJsonObject> Row = LoadJsonObject(InputsPath);
	TestTrue(TEXT("Read input row"), Row.IsValid());
	if (Row.IsValid())
	{
		Row->SetNumberField(TEXT("sequence"), 0.5);
		TestTrue(TEXT("Write fractional sequence"), SaveJsonLine(InputsPath, Row));
	}
	ExpectFormatError(TEXT("Fractional sequence is rejected"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryUnicodeTextTest,
	"Cortex.Replay.Library.UnicodeHumanTextLimits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryUnicodeTextTest::RunTest(const FString& Parameters)
{
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 0;
	TestTrue(TEXT("Reserve"), Library.ReserveId(Id).bSuccess);
	TestTrue(TEXT("Publish"), Library.Publish(Fixture.MakeRecording(Id, false, {})).bSuccess);

	FString Supplementary;
	for (int32 Index = 0; Index < 65; ++Index)
	{
		Supplementary.AppendChar(static_cast<TCHAR>(0xD83D));
		Supplementary.AppendChar(static_cast<TCHAR>(0xDE00));
	}
	TestEqual(TEXT("Supplementary name occupies 130 UTF-16 units"), Supplementary.Len(), 130);

	TestTrue(TEXT("65 supplementary characters are accepted"),
		Library.SaveMetadata(Id, Supplementary, TEXT(""), false).bSuccess);

	FCortexReplayLibrary Reopened(Fixture.GetProjectRoot());
	TSharedPtr<const FCortexReplaySnapshot> Snapshot;
	TestTrue(TEXT("Supplementary name reloads"), Reopened.Load(Id, false, Snapshot).bSuccess);
	TestEqual(TEXT("Supplementary name is preserved"), Snapshot->Metadata.Name, Supplementary);

	TestTrue(TEXT("128-character name accepted"),
		Library.SaveMetadata(Id, FString::ChrN(128, TEXT('a')), TEXT(""), false).bSuccess);
	TestFalse(TEXT("129-character name rejected"),
		Library.SaveMetadata(Id, FString::ChrN(129, TEXT('a')), TEXT(""), false).bSuccess);

	FString LoneSurrogate = FString(TEXT("bad"));
	LoneSurrogate.AppendChar(static_cast<TCHAR>(0xD800));
	TestFalse(TEXT("Unpaired surrogate rejected"),
		Library.SaveMetadata(Id, LoneSurrogate, TEXT(""), false).bSuccess);

	TestFalse(TEXT("Over-long description rejected"),
		Library.SaveMetadata(Id, TEXT("Valid"), FString::ChrN(1025, TEXT('d')), false).bSuccess);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryKeyEligibilityTest,
	"Cortex.Replay.Library.KeyEligibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryKeyEligibilityTest::RunTest(const FString& Parameters)
{
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 0;
	TestTrue(TEXT("Reserve"), Library.ReserveId(Id).bSuccess);

	{
		FCortexReplayEvent Event = MakeKeyDownEvent(0, 0.0, FKey(FName(TEXT("CortexReplayNotAKey"))));
		TestFalse(TEXT("Unregistered key rejected"), Library.Publish(Fixture.MakeRecording(Id, false, { Event })).bSuccess);
	}

	{
		FCortexReplayEvent Event = MakeKeyDownEvent(0, 0.0, EKeys::Gamepad_FaceButton_Bottom);
		TestFalse(TEXT("Gamepad key rejected"), Library.Publish(Fixture.MakeRecording(Id, false, { Event })).bSuccess);
	}

	{
		FCortexReplayEvent Event = MakeKeyDownEvent(0, 0.0, EKeys::LeftMouseButton);
		Event.Input.Kind = ECortexEditorPhysicalInputKind::PointerDown;
		TestTrue(TEXT("Mouse pointer press accepted"), Library.Publish(Fixture.MakeRecording(Id, false, { Event })).bSuccess);
	}

	const FString InputsPath = Fixture.GetInputsPath(Id);
	TSharedPtr<FJsonObject> Row = LoadJsonObject(InputsPath);
	TestTrue(TEXT("Read input row"), Row.IsValid());
	if (Row.IsValid())
	{
		Row->SetStringField(TEXT("key"), TEXT("CortexReplayNotAKey"));
		TestTrue(TEXT("Write unregistered key row"), SaveJsonLine(InputsPath, Row));
	}

	TSharedPtr<const FCortexReplaySnapshot> Snapshot;
	FCortexReplayLibrary Reopened(Fixture.GetProjectRoot());
	const FCortexCommandResult Result = Reopened.Load(Id, false, Snapshot);
	TestFalse(TEXT("Unregistered persisted key denies load"), Result.bSuccess);
	TestEqual(TEXT("Unregistered key is a format error"), Result.ErrorCode, FString(CortexReplayErrorCodes::UnsupportedRecordingFormat));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryCaptureFrameOverflowTest,
	"Cortex.Replay.Library.CaptureFrameOverflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryCaptureFrameOverflowTest::RunTest(const FString& Parameters)
{
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 0;
	TestTrue(TEXT("Reserve"), Library.ReserveId(Id).bSuccess);

	{
		FCortexReplayEvent Event = MakeKeyDownEvent(0, 0.0);
		Event.CaptureContext.FrameNumber = MAX_uint64;
		TestTrue(TEXT("Maximum frame publishes"), Library.Publish(Fixture.MakeRecording(Id, false, { Event })).bSuccess);
	}

	{
		TSharedPtr<const FCortexReplaySnapshot> Snapshot;
		FCortexReplayLibrary Reopened(Fixture.GetProjectRoot());
		TestTrue(TEXT("Maximum frame loads"), Reopened.Load(Id, false, Snapshot).bSuccess);
	}

	const FString InputsPath = Fixture.GetInputsPath(Id);
	TSharedPtr<FJsonObject> Row = LoadJsonObject(InputsPath);
	TestTrue(TEXT("Read input row"), Row.IsValid());
	if (Row.IsValid())
	{
		Row->SetStringField(TEXT("capture_frame"), TEXT("18446744073709551616"));
		TestTrue(TEXT("Write overflowing frame"), SaveJsonLine(InputsPath, Row));
	}

	TSharedPtr<const FCortexReplaySnapshot> Snapshot;
	FCortexReplayLibrary Reopened(Fixture.GetProjectRoot());
	const FCortexCommandResult Result = Reopened.Load(Id, false, Snapshot);
	TestFalse(TEXT("Overflowing frame denies load"), Result.bSuccess);
	TestEqual(TEXT("Frame overflow is a format error"), Result.ErrorCode, FString(CortexReplayErrorCodes::UnsupportedRecordingFormat));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryAssetPathTest,
	"Cortex.Replay.Library.AssetPathValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryAssetPathTest::RunTest(const FString& Parameters)
{
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 0;
	TestTrue(TEXT("Reserve"), Library.ReserveId(Id).bSuccess);

	{
		FCortexReplaySnapshot Snapshot = Fixture.MakeRecording(Id, false, {});
		Snapshot.Metadata.MapAssetPath = TEXT("/");
		TestFalse(TEXT("Root-only map path rejected"), Library.Publish(Snapshot).bSuccess);
	}

	{
		FCortexReplaySnapshot Snapshot = Fixture.MakeRecording(Id, false, {});
		Snapshot.Metadata.MapAssetPath = TEXT("/Game/Bad Path");
		TestFalse(TEXT("Map path with an illegal character rejected"), Library.Publish(Snapshot).bSuccess);
	}

	{
		FCortexReplaySnapshot Snapshot = Fixture.MakeRecording(Id, false, {});
		Snapshot.InitialState.PawnClassPath = TEXT("BadPawn");
		TestFalse(TEXT("Invalid pawn class path rejected"), Library.Publish(Snapshot).bSuccess);
	}

	{
		FCortexReplaySnapshot Snapshot = Fixture.MakeRecording(Id, false, { MakeKeyDownEvent(0, 0.0) });
		Snapshot.Events[0].Guard->UICoverage = ECortexEditorUICoverage::Supported;
		Snapshot.Events[0].Guard->UITarget = MakeWidgetIdentityWithPath(TEXT("BadWidget"));
		Snapshot.Events[0].Guard->ExpectedLocalPosition = FVector2D(0.5, 0.5);
		TestFalse(TEXT("Invalid widget class path rejected"), Library.Publish(Snapshot).bSuccess);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibrarySelectorStrictnessTest,
	"Cortex.Replay.Library.SelectorStrictness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibrarySelectorStrictnessTest::RunTest(const FString& Parameters)
{
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 0;
	TestTrue(TEXT("Reserve"), Library.ReserveId(Id).bSuccess);

	{
		FCortexReplaySnapshot Snapshot = Fixture.MakeRecording(Id, false, { MakeKeyDownEvent(0, 0.0) });
		TSharedPtr<FCortexEditorPhysicalInputWidgetIdentity> Identity =
			MakeMutableUmgIdentity(TEXT("/Game/UI/WBP_ReplayTest.WBP_ReplayTest_C"));
		Identity->RootTag = TEXT("StrayTag");
		Snapshot.Events[0].Guard->UICoverage = ECortexEditorUICoverage::Supported;
		Snapshot.Events[0].Guard->UITarget = TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity>(Identity);
		Snapshot.Events[0].Guard->ExpectedLocalPosition = FVector2D(0.5, 0.5);
		TestFalse(TEXT("UMG singleton_class carrying a root_tag is rejected"), Library.Publish(Snapshot).bSuccess);
	}

	{
		FCortexReplaySnapshot Snapshot = Fixture.MakeRecording(Id, false, { MakeKeyDownEvent(0, 0.0) });
		TSharedPtr<FCortexEditorPhysicalInputWidgetIdentity> Identity =
			MakeShared<FCortexEditorPhysicalInputWidgetIdentity>();
		Identity->RootKind = ECortexEditorUIRootKind::Slate;
		Identity->Surface = ECortexEditorUISurface::Viewport;
		Identity->Discriminator = ECortexEditorUIRootDiscriminator::RootTag;
		Identity->WidgetAncestry.Add(FName(TEXT("Root")));
		Snapshot.Events[0].Guard->UICoverage = ECortexEditorUICoverage::Supported;
		Snapshot.Events[0].Guard->UITarget = TSharedPtr<const FCortexEditorPhysicalInputWidgetIdentity>(Identity);
		Snapshot.Events[0].Guard->ExpectedLocalPosition = FVector2D(0.5, 0.5);
		TestFalse(TEXT("Slate identity without tags is rejected"), Library.Publish(Snapshot).bSuccess);
	}

	{
		FCortexReplayEvent Event = MakeKeyDownEvent(0, 0.0);
		Event.Guard = MakeSupportedGuard(TEXT("/Game/UI/WBP_ReplayTest.WBP_ReplayTest_C"));
		TestTrue(TEXT("Publish supported guard"), Library.Publish(Fixture.MakeRecording(Id, false, { Event })).bSuccess);
	}

	const FString InputsPath = Fixture.GetInputsPath(Id);
	FString OriginalInputs;
	TestTrue(TEXT("Read original inputs"), LoadTextFile(InputsPath, OriginalInputs));

	auto RestoreAndLoad = [&](const TCHAR* Label)
	{
		TSharedPtr<const FCortexReplaySnapshot> Snapshot;
		FCortexReplayLibrary Reopened(Fixture.GetProjectRoot());
		const FCortexCommandResult Result = Reopened.Load(Id, false, Snapshot);
		TestFalse(Label, Result.bSuccess);
		TestEqual(TEXT("Rejected as an unsupported format"), Result.ErrorCode, FString(CortexReplayErrorCodes::UnsupportedRecordingFormat));
	};

	// Unknown surface string must not be normalised to a valid enum.
	TestTrue(TEXT("Restore inputs"), FFileHelper::SaveStringToFile(OriginalInputs, *InputsPath));
	{
		TSharedPtr<FJsonObject> Row = LoadJsonObject(InputsPath);
		TestTrue(TEXT("Read identity row"), GetGuardIdentityObject(Row).IsValid());
		TSharedPtr<FJsonObject> Identity = GetGuardIdentityObject(Row);
		if (Identity.IsValid())
		{
			Identity->SetStringField(TEXT("surface"), TEXT("sidebar"));
			TestTrue(TEXT("Write unknown surface"), SaveJsonLine(InputsPath, Row));
		}
	}
	RestoreAndLoad(TEXT("Unknown identity surface is rejected"));

	// Non-string ancestry entries must be rejected rather than dropped.
	TestTrue(TEXT("Restore inputs"), FFileHelper::SaveStringToFile(OriginalInputs, *InputsPath));
	{
		TSharedPtr<FJsonObject> Row = LoadJsonObject(InputsPath);
		TSharedPtr<FJsonObject> Identity = GetGuardIdentityObject(Row);
		if (Identity.IsValid())
		{
			TArray<TSharedPtr<FJsonValue>> BadAncestry;
			BadAncestry.Add(MakeShared<FJsonValueNumber>(1));
			Identity->SetArrayField(TEXT("widget_ancestry"), BadAncestry);
			TestTrue(TEXT("Write non-string ancestry"), SaveJsonLine(InputsPath, Row));
		}
	}
	RestoreAndLoad(TEXT("Non-string ancestry is rejected"));

	// not_applicable must not retain selector or local-position fields.
	TestTrue(TEXT("Restore inputs"), FFileHelper::SaveStringToFile(OriginalInputs, *InputsPath));
	{
		TSharedPtr<FJsonObject> Row = LoadJsonObject(InputsPath);
		TSharedPtr<FJsonObject> Ui = GetGuardUiObject(Row);
		if (Ui.IsValid())
		{
			Ui->SetStringField(TEXT("coverage"), TEXT("not_applicable"));
			TestTrue(TEXT("Write contradictory not_applicable guard"), SaveJsonLine(InputsPath, Row));
		}
	}
	RestoreAndLoad(TEXT("Not-applicable guard with selector fields is rejected"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryIdentityInterningTest,
	"Cortex.Replay.Library.IdentityInterning",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryIdentityInterningTest::RunTest(const FString& Parameters)
{
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 0;
	TestTrue(TEXT("Reserve"), Library.ReserveId(Id).bSuccess);

	FCortexReplayEvent First = MakeKeyDownEvent(0, 0.0);
	FCortexReplayEvent Second = MakeKeyDownEvent(1, 0.1);
	First.Guard = MakeSupportedGuard(TEXT("/Game/UI/WBP_Interning.WBP_Interning_C"));
	Second.Guard = MakeSupportedGuard(TEXT("/Game/UI/WBP_Interning.WBP_Interning_C"));
	TestTrue(TEXT("Publish repeated selectors"), Library.Publish(Fixture.MakeRecording(Id, false, { First, Second })).bSuccess);

	TSharedPtr<const FCortexReplaySnapshot> Snapshot;
	FCortexReplayLibrary Reopened(Fixture.GetProjectRoot());
	TestTrue(TEXT("Load repeated selectors"), Reopened.Load(Id, false, Snapshot).bSuccess);
	TestTrue(TEXT("Both guards carry identities"),
		Snapshot->Events[0].Guard->UITarget.IsValid() && Snapshot->Events[1].Guard->UITarget.IsValid());
	TestEqual(TEXT("Identity digest is rebuilt as lower-case hex"),
		Snapshot->Events[0].Guard->UITarget->IdentitySha256.Len(), 64);
	TestTrue(TEXT("Repeated selectors are interned to one instance"),
		Snapshot->Events[0].Guard->UITarget.Get() == Snapshot->Events[1].Guard->UITarget.Get());

	int32 OtherId = 0;
	TestTrue(TEXT("Reserve other"), Library.ReserveId(OtherId).bSuccess);
	FCortexReplayEvent Third = MakeKeyDownEvent(0, 0.0);
	FCortexReplayEvent Fourth = MakeKeyDownEvent(1, 0.1);
	Third.Guard = MakeSupportedGuard(TEXT("/Game/UI/WBP_Other.WBP_Other_C"));
	Fourth.Guard = MakeSupportedGuard(TEXT("/Game/UI/WBP_Third.WBP_Third_C"));
	TestTrue(TEXT("Publish distinct selectors"), Library.Publish(Fixture.MakeRecording(OtherId, false, { Third, Fourth })).bSuccess);

	TSharedPtr<const FCortexReplaySnapshot> OtherSnapshot;
	TestTrue(TEXT("Load distinct selectors"), Reopened.Load(OtherId, false, OtherSnapshot).bSuccess);
	TestTrue(TEXT("Distinct selectors are not aliased"),
		OtherSnapshot->Events[0].Guard->UITarget.Get() != OtherSnapshot->Events[1].Guard->UITarget.Get());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryAuthoringLockTest,
	"Cortex.Replay.Library.AuthoringLockContention",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryAuthoringLockTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS
	FCortexReplayTestFixture Fixture;
	TestTrue(TEXT("Create replay root"), IFileManager::Get().MakeDirectory(*ReplayRootPath(Fixture), true));

	const FString LockPath = ReplaySubPath(Fixture, TEXT(".authoring.lock"));
	const HANDLE Held = CreateFileW(
		*LockPath,
		GENERIC_READ | GENERIC_WRITE,
		0,
		nullptr,
		OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	TestTrue(TEXT("Test holds the authoring lock exclusively"), Held != INVALID_HANDLE_VALUE);

	FCortexReplayLibrary Library(Fixture.GetProjectRoot());
	int32 BlockedId = 0;
	TestFalse(TEXT("Reservation is blocked while the authoring lock is held"), Library.ReserveId(BlockedId).bSuccess);

	if (Held != INVALID_HANDLE_VALUE)
	{
		CloseHandle(Held);
	}

	int32 Id = 0;
	TestTrue(TEXT("Reservation succeeds after the lock is released"), Library.ReserveId(Id).bSuccess);
	return true;
#else
	AddInfo(TEXT("Authoring-lock contention requires the Windows exclusive file handle"));
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryPublishRenameFailureTest,
	"Cortex.Replay.Library.PublishRenameFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryPublishRenameFailureTest::RunTest(const FString& Parameters)
{
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 First = 0;
	TestTrue(TEXT("Reserve first"), Library.ReserveId(First).bSuccess);
	TestTrue(TEXT("Publish first"), Library.Publish(Fixture.MakeRecording(First, false, {})).bSuccess);

	FString MetadataBefore;
	TestTrue(TEXT("Read first metadata"),
		LoadTextFile(FPaths::Combine(RecordingDirectoryPath(Fixture, First), TEXT("metadata.json")), MetadataBefore));

	int32 Second = 0;
	TestTrue(TEXT("Reserve second"), Library.ReserveId(Second).bSuccess);

	// The counter is committed at reservation and is never rolled back by a failed publish.
	FString CounterBefore;
	TestTrue(TEXT("Read counter after reservation"), LoadTextFile(ReplaySubPath(Fixture, TEXT("library.json")), CounterBefore));

	// Occupy the canonical destination with a regular file so the publish rename fails.
	TestTrue(TEXT("Occupy rename target"), FFileHelper::SaveStringToFile(TEXT("occupied"), *RecordingDirectoryPath(Fixture, Second)));

	TestFalse(TEXT("Publish fails when the destination is occupied"),
		Library.Publish(Fixture.MakeRecording(Second, false, {})).bSuccess);

	FString CounterAfter;
	TestTrue(TEXT("Read counter after failure"), LoadTextFile(ReplaySubPath(Fixture, TEXT("library.json")), CounterAfter));
	TestEqual(TEXT("Committed counter reflects exactly the second reservation"), CounterAfter, CounterBefore);

	int32 Third = 0;
	TestTrue(TEXT("Reserve after failure"), Library.ReserveId(Third).bSuccess);
	TestEqual(TEXT("Reserved id is not reused after a failed publish"), Third, Second + 1);

	FString MetadataAfter;
	TestTrue(TEXT("Read first metadata after failure"),
		LoadTextFile(FPaths::Combine(RecordingDirectoryPath(Fixture, First), TEXT("metadata.json")), MetadataAfter));
	TestEqual(TEXT("Existing recording metadata is unchanged"), MetadataAfter, MetadataBefore);
	TSharedPtr<const FCortexReplaySnapshot> ExistingSnapshot;
	TestTrue(TEXT("Existing recording still loads"), Library.Load(First, false, ExistingSnapshot).bSuccess);

	TSharedPtr<const FCortexReplaySnapshot> Partial;
	TestFalse(TEXT("Failed record is not loadable"), Library.Load(Second, false, Partial).bSuccess);

	TArray<FCortexReplayMetadata> Records;
	TestTrue(TEXT("List after failure"), Library.List(false, Records).bSuccess);
	bool bPartialListed = false;
	for (const FCortexReplayMetadata& Record : Records)
	{
		if (Record.RecordingId == Second)
		{
			bPartialListed = true;
		}
	}
	TestFalse(TEXT("Failed record is not listed"), bPartialListed);

	TArray<FString> PendingEntries;
	const FString PendingRoot = ReplaySubPath(Fixture, TEXT(".pending"));
	if (IFileManager::Get().DirectoryExists(*PendingRoot))
	{
		IFileManager::Get().FindFiles(PendingEntries, *FPaths::Combine(PendingRoot, TEXT("*")), false, true);
	}
	TestEqual(TEXT("No pending staging directories remain"), PendingEntries.Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryPublishStagingFailureTest,
	"Cortex.Replay.Library.PublishStagingFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryPublishStagingFailureTest::RunTest(const FString& Parameters)
{
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 First = 0;
	TestTrue(TEXT("Reserve first"), Library.ReserveId(First).bSuccess);
	TestTrue(TEXT("Publish first"), Library.Publish(Fixture.MakeRecording(First, false, {})).bSuccess);

	int32 Second = 0;
	TestTrue(TEXT("Reserve second"), Library.ReserveId(Second).bSuccess);

	// The counter is committed at reservation and is never rolled back by a failed publish.
	FString CounterBefore;
	TestTrue(TEXT("Read counter after reservation"), LoadTextFile(ReplaySubPath(Fixture, TEXT("library.json")), CounterBefore));

	// Block the staging root with a regular file so no staging directory can be created.
	const FString PendingRoot = ReplaySubPath(Fixture, TEXT(".pending"));
	IFileManager::Get().DeleteDirectory(*PendingRoot, false, true);
	TestTrue(TEXT("Occupy staging root"), FFileHelper::SaveStringToFile(TEXT("occupied"), *PendingRoot));

	TestFalse(TEXT("Publish fails when staging is blocked"),
		Library.Publish(Fixture.MakeRecording(Second, false, {})).bSuccess);

	FString CounterAfter;
	TestTrue(TEXT("Read counter after failure"), LoadTextFile(ReplaySubPath(Fixture, TEXT("library.json")), CounterAfter));
	TestEqual(TEXT("Committed counter reflects exactly the second reservation"), CounterAfter, CounterBefore);

	int32 Third = 0;
	TestTrue(TEXT("Reserve after failure"), Library.ReserveId(Third).bSuccess);
	TestEqual(TEXT("Reserved id is not reused after a failed publish"), Third, Second + 1);

	TSharedPtr<const FCortexReplaySnapshot> Partial;
	TestFalse(TEXT("Failed record is not loadable"), Library.Load(Second, false, Partial).bSuccess);

	TArray<FCortexReplayMetadata> Records;
	TestTrue(TEXT("List after failure"), Library.List(false, Records).bSuccess);
	TestEqual(TEXT("Only the committed recording is listed"), Records.Num(), 1);
	if (Records.Num() == 1)
	{
		TestEqual(TEXT("Only the committed recording id is listed"), Records[0].RecordingId, First);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryEmbeddedCarriageReturnTest,
	"Cortex.Replay.Library.EmbeddedCarriageReturnRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryEmbeddedCarriageReturnTest::RunTest(const FString& Parameters)
{
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 0;
	TestTrue(TEXT("Reserve"), Library.ReserveId(Id).bSuccess);
	TestTrue(TEXT("Publish"), Library.Publish(Fixture.MakeRecording(Id, false, { MakeKeyDownEvent(0, 0.0) })).bSuccess);

	const FString InputsPath = Fixture.GetInputsPath(Id);
	FString Inputs;
	TestTrue(TEXT("Read inputs"), LoadTextFile(InputsPath, Inputs));
	TestTrue(TEXT("Persisted row carries the recorded key"), Inputs.Contains(TEXT("\"SpaceBar\"")));

	// Inject a raw carriage return inside the persisted key string. It must not be
	// stripped into the valid registered key "SpaceBar".
	Inputs.ReplaceInline(TEXT("\"SpaceBar\""), TEXT("\"Space\rBar\""));
	TestTrue(TEXT("Write CR-bearing row"), FFileHelper::SaveStringToFile(Inputs, *InputsPath));

	TSharedPtr<const FCortexReplaySnapshot> Snapshot;
	FCortexReplayLibrary Reopened(Fixture.GetProjectRoot());
	const FCortexCommandResult Result = Reopened.Load(Id, false, Snapshot);
	TestFalse(TEXT("Embedded carriage return is rejected"), Result.bSuccess);
	TestEqual(TEXT("Embedded carriage return is a format error"), Result.ErrorCode, FString(CortexReplayErrorCodes::UnsupportedRecordingFormat));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryViewportSizeTest,
	"Cortex.Replay.Library.ViewportSizeValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryViewportSizeTest::RunTest(const FString& Parameters)
{
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 0;
	TestTrue(TEXT("Reserve"), Library.ReserveId(Id).bSuccess);

	{
		FCortexReplaySnapshot Snapshot = Fixture.MakeRecording(Id, false, {});
		Snapshot.Metadata.Prerequisites.ViewportSize = FIntPoint(0, 1080);
		TestFalse(TEXT("Zero-width viewport rejected"), Library.Publish(Snapshot).bSuccess);
	}

	{
		FCortexReplaySnapshot Snapshot = Fixture.MakeRecording(Id, false, {});
		Snapshot.Metadata.Prerequisites.ViewportSize = FIntPoint(1920, -1);
		TestFalse(TEXT("Negative viewport height rejected"), Library.Publish(Snapshot).bSuccess);
	}

	TestTrue(TEXT("Publish valid"), Library.Publish(Fixture.MakeRecording(Id, false, {})).bSuccess);

	const FString MetadataPath = FPaths::Combine(RecordingDirectoryPath(Fixture, Id), TEXT("metadata.json"));
	FString OriginalMetadata;
	TestTrue(TEXT("Read metadata"), LoadTextFile(MetadataPath, OriginalMetadata));

	auto TamperViewportAndLoad = [&](const TCHAR* Label, double X, double Y)
	{
		TestTrue(TEXT("Restore metadata"), FFileHelper::SaveStringToFile(OriginalMetadata, *MetadataPath));
		TSharedPtr<FJsonObject> Metadata = LoadJsonObject(MetadataPath);
		if (!Metadata.IsValid())
		{
			return;
		}

		const TSharedPtr<FJsonObject>* Prerequisites = nullptr;
		if (Metadata->TryGetObjectField(TEXT("prerequisites"), Prerequisites) && Prerequisites != nullptr)
		{
			const TSharedPtr<FJsonObject>* ViewportSize = nullptr;
			if ((*Prerequisites)->TryGetObjectField(TEXT("viewport_size"), ViewportSize) && ViewportSize != nullptr)
			{
				(*ViewportSize)->SetNumberField(TEXT("x"), X);
				(*ViewportSize)->SetNumberField(TEXT("y"), Y);
			}
		}
		TestTrue(TEXT("Write tampered metadata"), SaveJsonObject(MetadataPath, Metadata));

		TSharedPtr<const FCortexReplaySnapshot> Snapshot;
		FCortexReplayLibrary Reopened(Fixture.GetProjectRoot());
		const FCortexCommandResult Result = Reopened.Load(Id, false, Snapshot);
		TestFalse(Label, Result.bSuccess);
		TestEqual(TEXT("Rejected as an unsupported format"), Result.ErrorCode, FString(CortexReplayErrorCodes::UnsupportedRecordingFormat));
	};

	TamperViewportAndLoad(TEXT("Fractional viewport width is rejected"), 1920.5, 1080.0);
	TamperViewportAndLoad(TEXT("Zero viewport height is rejected"), 1920.0, 0.0);

	return true;
}
