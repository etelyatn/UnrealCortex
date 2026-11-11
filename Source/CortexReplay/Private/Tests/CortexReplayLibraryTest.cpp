#include "Misc/AutomationTest.h"

#include "CortexReplayErrorCodes.h"
#include "CortexReplayGuardEvaluator.h"
#include "CortexReplayLibrary.h"
#include "CortexReplayStorage.h"
#include "CortexReplayTestUtils.h"
#include "CortexReplayTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Templates/Function.h"
#include "Widgets/Layout/SBox.h"

#include <limits>

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace
{
FCortexReplayTimedEvent MakeKeyDownEvent(int32 Sequence, double TimeSeconds, const FKey& Key = EKeys::SpaceBar)
{
	FCortexReplayTimedEvent Timed;
	Timed.TimeSeconds = TimeSeconds;
	Timed.Event.Sequence = Sequence;
	Timed.Event.Input.Kind = ECortexEditorPhysicalInputKind::KeyDown;
	Timed.Event.Input.Key = Key;
	return Timed;
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

/** Parses one row (default: the first) of a gzip stream as a JSON object. */
TSharedPtr<FJsonObject> LoadStreamRowObject(const FString& GzPath, int32 RowIndex = 0)
{
	TArray<FString> Rows;
	if (!FCortexReplayTestFixture::ReadPayloadRows(GzPath, Rows) || !Rows.IsValidIndex(RowIndex))
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Rows[RowIndex]);
	if (!FJsonSerializer::Deserialize(Reader, Object))
	{
		return nullptr;
	}

	return Object;
}

/**
 * Replaces one row (default: the first) of a gzip stream with Object, keeping the other rows, then
 * refreshes the stream digest named by HashField so Load reaches row validation.
 */
bool StoreStreamRowObject(const FCortexReplayTestFixture& Fixture, int32 Id, const TCHAR* HashField,
	const FString& GzPath, const TSharedPtr<FJsonObject>& Object, int32 RowIndex = 0)
{
	TArray<FString> Rows;
	if (!Object.IsValid() || !FCortexReplayTestFixture::ReadPayloadRows(GzPath, Rows) || !Rows.IsValidIndex(RowIndex))
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

	Rows[RowIndex] = MoveTemp(Text);
	return FCortexReplayTestFixture::WritePayloadRows(GzPath, Rows)
		&& Fixture.RewriteMetadataStreamHash(Id, HashField, GzPath);
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
		// The current recording format is 3, so a legacy 1 must be refused as mismatched.
		Snapshot.InitialState.SchemaVersion = 1;
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
		FCortexReplayTestFixture::WritePayloadRows(InputsPath, { FString(TEXT("{\"schema_version\":1}")) }));

	TSharedPtr<const FCortexReplaySnapshot> Snapshot;
	FCortexReplayLibrary Reopened(Fixture.GetProjectRoot());
	const FCortexCommandResult Result = Reopened.Load(Id, false, Snapshot);
	TestFalse(TEXT("Tampered input bytes deny load"), Result.bSuccess);
	TestEqual(TEXT("A changed stream without a refreshed digest is an invalid recording"), Result.ErrorCode,
		FString(CortexReplayErrorCodes::InvalidRecording));

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

	TArray<uint8> InitialBytes;
	TArray<uint8> InputBytes;
	TestTrue(TEXT("Read initial bytes"), FFileHelper::LoadFileToArray(InitialBytes, *Fixture.GetInitialStatePath(Id)));
	TestTrue(TEXT("Read input bytes"), FFileHelper::LoadFileToArray(InputBytes, *Fixture.GetInputsPath(Id)));

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

	TArray<uint8> InitialAfter;
	TArray<uint8> InputAfter;
	TestTrue(TEXT("Read initial bytes after save"), FFileHelper::LoadFileToArray(InitialAfter, *Fixture.GetInitialStatePath(Id)));
	TestTrue(TEXT("Read input bytes after save"), FFileHelper::LoadFileToArray(InputAfter, *Fixture.GetInputsPath(Id)));
	TestTrue(TEXT("Initial bytes unchanged"), InitialAfter == InitialBytes);
	TestTrue(TEXT("Input bytes unchanged"), InputAfter == InputBytes);

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

	// Hash first: a hand-edited row with a stale digest is an invalid recording; only a malformed
	// row whose digest was also refreshed reaches row validation (an unsupported format).
	TestTrue(TEXT("Restore metadata"), FFileHelper::SaveStringToFile(OriginalMetadata, *MetadataPath));
	const FString InputsPath = Fixture.GetInputsPath(Id);
	TSharedPtr<FJsonObject> Row = LoadStreamRowObject(InputsPath);
	TestTrue(TEXT("Read input row"), Row.IsValid());
	if (Row.IsValid())
	{
		Row->SetStringField(TEXT("wheel_delta"), TEXT("x"));
		TestTrue(TEXT("Write non-numeric wheel_delta"),
			StoreStreamRowObject(Fixture, Id, TEXT("inputs_sha256"), InputsPath, Row));
	}
	ExpectFormatError(TEXT("Non-numeric wheel_delta is rejected"));

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
		FCortexReplayTimedEvent Event = MakeKeyDownEvent(0, 0.0, FKey(FName(TEXT("CortexReplayNotAKey"))));
		TestFalse(TEXT("Unregistered key rejected"), Library.Publish(Fixture.MakeRecording(Id, false, { Event })).bSuccess);
	}

	{
		FCortexReplayTimedEvent Event = MakeKeyDownEvent(0, 0.0, EKeys::Gamepad_FaceButton_Bottom);
		TestFalse(TEXT("Gamepad key rejected"), Library.Publish(Fixture.MakeRecording(Id, false, { Event })).bSuccess);
	}

	{
		FCortexReplayTimedEvent Event = MakeKeyDownEvent(0, 0.0, EKeys::LeftMouseButton);
		Event.Event.Input.Kind = ECortexEditorPhysicalInputKind::PointerDown;
		TestTrue(TEXT("Mouse pointer press accepted"), Library.Publish(Fixture.MakeRecording(Id, false, { Event })).bSuccess);
	}

	const FString InputsPath = Fixture.GetInputsPath(Id);
	TSharedPtr<FJsonObject> Row = LoadStreamRowObject(InputsPath);
	TestTrue(TEXT("Read input row"), Row.IsValid());
	if (Row.IsValid())
	{
		Row->SetStringField(TEXT("key"), TEXT("CortexReplayNotAKey"));
		TestTrue(TEXT("Write unregistered key row"),
			StoreStreamRowObject(Fixture, Id, TEXT("inputs_sha256"), InputsPath, Row));
	}

	TSharedPtr<const FCortexReplaySnapshot> Snapshot;
	FCortexReplayLibrary Reopened(Fixture.GetProjectRoot());
	const FCortexCommandResult Result = Reopened.Load(Id, false, Snapshot);
	TestFalse(TEXT("Unregistered persisted key denies load"), Result.bSuccess);
	TestEqual(TEXT("Unregistered key is a format error"), Result.ErrorCode, FString(CortexReplayErrorCodes::UnsupportedRecordingFormat));

	return true;
}

// ---------------------------------------------------------------------------
// CR-01: the ONE kind-specific portable key representation. Motion carries the canonical
// Mouse2D axis, wheel the canonical MouseWheelAxis; the recording is accepted, loadable and
// preserves the canonical keys, while any other representation is rejected.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryPortableMotionWheelKeysTest,
	"Cortex.Replay.Library.PortableMotionWheelKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryPortableMotionWheelKeysTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 0;
	TestTrue(TEXT("Reserve"), Library.ReserveId(Id).bSuccess);

	FCortexReplayTimedEvent Key = MakeKeyDownEvent(0, 0.0, EKeys::W);

	FCortexReplayEvent Move;
	Move.Sequence = 1;
	Move.Input.Kind = ECortexEditorPhysicalInputKind::PointerMove;
	Move.Input.Key = EKeys::Mouse2D;
	Move.Input.ViewportPosition = FVector2D(0.25, 0.5);
	Move.Input.Delta = FVector2D(3.0, -2.0);

	FCortexReplayEvent Relative;
	Relative.Sequence = 2;
	Relative.Input.Kind = ECortexEditorPhysicalInputKind::RelativeMove;
	Relative.Input.Key = EKeys::Mouse2D;
	Relative.Input.Delta = FVector2D(12.0, 0.0);

	FCortexReplayEvent Wheel;
	Wheel.Sequence = 3;
	Wheel.Input.Kind = ECortexEditorPhysicalInputKind::Wheel;
	Wheel.Input.Key = EKeys::MouseWheelAxis;
	Wheel.Input.WheelDelta = 1.0f;

	TestTrue(TEXT("Recording with captured motion and wheel publishes"),
		Library.Publish(Fixture.MakeRecording(Id, false, { Key, FCortexReplayTimedEvent{Move, 0.01},
			FCortexReplayTimedEvent{Relative, 0.02}, FCortexReplayTimedEvent{Wheel, 0.03} })).bSuccess);

	TSharedPtr<const FCortexReplaySnapshot> Loaded;
	TestTrue(TEXT("Recording with captured motion and wheel loads"),
		Library.Load(Id, false, Loaded).bSuccess);
	if (Loaded.IsValid() && Loaded->Events.Num() == 4)
	{
		TestEqual(TEXT("Loaded motion key is the canonical Mouse2D"),
			Loaded->Events[1].Input.Key, EKeys::Mouse2D);
		TestEqual(TEXT("Loaded relative key is the canonical Mouse2D"),
			Loaded->Events[2].Input.Key, EKeys::Mouse2D);
		TestEqual(TEXT("Loaded wheel key is the canonical MouseWheelAxis"),
			Loaded->Events[3].Input.Key, EKeys::MouseWheelAxis);
	}

	// Exactly one representation: a registered-but-different motion axis is not the canonical key.
	{
		int32 MoveId = 0;
		TestTrue(TEXT("Reserve non-canonical motion"), Library.ReserveId(MoveId).bSuccess);
		FCortexReplayEvent NonCanonicalMove;
		NonCanonicalMove.Sequence = 0;
		NonCanonicalMove.Input.Kind = ECortexEditorPhysicalInputKind::PointerMove;
		NonCanonicalMove.Input.Key = EKeys::MouseX;
		TestFalse(TEXT("Non-canonical motion key is rejected"),
			Library.Publish(Fixture.MakeRecording(MoveId, false, { FCortexReplayTimedEvent{NonCanonicalMove, 0.0} })).bSuccess);
	}
	{
		int32 WheelId = 0;
		TestTrue(TEXT("Reserve keyless wheel"), Library.ReserveId(WheelId).bSuccess);
		FCortexReplayEvent KeylessWheel;
		KeylessWheel.Sequence = 0;
		KeylessWheel.Input.Kind = ECortexEditorPhysicalInputKind::Wheel;
		KeylessWheel.Input.Key = EKeys::Invalid;
		TestFalse(TEXT("Keyless wheel is rejected"),
			Library.Publish(Fixture.MakeRecording(WheelId, false, { FCortexReplayTimedEvent{KeylessWheel, 0.0} })).bSuccess);
	}

	return true;
}

// ---------------------------------------------------------------------------
// CR-02: a real tagged Slate selector captured live and the identical selector loaded from a
// published recording carry the SAME non-empty lower-case 64-hex SHA-256, so the live evaluator
// accepts the guard instead of reporting identity_mismatch.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibrarySlateIdentityDigestRoundTripTest,
	"Cortex.Replay.Library.SlateIdentityDigestRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibrarySlateIdentityDigestRoundTripTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Slate not initialized - skipping identity digest test"));
		return true;
	}

	// Capture side: a real tagged Slate root/control produce the live selector and digest.
	const TSharedRef<SBox> RootWidget = SNew(SBox);
	RootWidget->SetTag(FName(TEXT("CortexReplayDigestRoot")));
	const TSharedRef<SBox> TargetWidget = SNew(SBox);
	TargetWidget->SetTag(FName(TEXT("CortexReplayDigestTarget")));
	const FCortexEditorPhysicalInputWidgetIdentity Captured =
		FCortexEditorPhysicalInputSelectorBuilder::BuildSlateIdentity(*RootWidget, *TargetWidget);

	TestEqual(TEXT("Captured digest is 64 hex characters"), Captured.IdentitySha256.Len(), 64);
	TestFalse(TEXT("Captured digest is non-empty"), Captured.IdentitySha256.IsEmpty());
	bool bLowerHex = Captured.IdentitySha256.Len() == 64;
	for (int32 Index = 0; bLowerHex && Index < Captured.IdentitySha256.Len(); ++Index)
	{
		const TCHAR Character = Captured.IdentitySha256[Index];
		bLowerHex = (Character >= TEXT('0') && Character <= TEXT('9'))
			|| (Character >= TEXT('a') && Character <= TEXT('f'));
	}
	TestTrue(TEXT("Captured digest is lower-case hex"), bLowerHex);

	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 0;
	TestTrue(TEXT("Reserve"), Library.ReserveId(Id).bSuccess);

	FCortexReplayEvent Press;
	Press.Sequence = 0;
	Press.Input.Kind = ECortexEditorPhysicalInputKind::PointerDown;
	Press.Input.Key = EKeys::LeftMouseButton;
	Press.Input.ViewportPosition = FVector2D(0.5, 0.5);
	FCortexReplayInteractionGuard Guard;
	Guard.ExpectedPose = FCortexEditorPhysicalInputPlayerPose();
	Guard.UICoverage = ECortexEditorUICoverage::Supported;
	Guard.UITarget = MakeShared<const FCortexEditorPhysicalInputWidgetIdentity>(Captured);
	Guard.ExpectedLocalPosition = FVector2D(0.5, 0.5);
	Press.Guard = Guard;

	TestTrue(TEXT("Captured selector publishes"),
		Library.Publish(Fixture.MakeRecording(Id, false, { FCortexReplayTimedEvent{Press, 0.0} })).bSuccess);

	TSharedPtr<const FCortexReplaySnapshot> Loaded;
	TestTrue(TEXT("Captured selector loads"), Library.Load(Id, false, Loaded).bSuccess);
	if (!Loaded.IsValid() || Loaded->Events.Num() != 1
		|| !Loaded->Events[0].Guard.IsSet() || !Loaded->Events[0].Guard->UITarget.IsValid())
	{
		AddError(TEXT("Loaded recording did not carry the supported guard"));
		return false;
	}
	const FCortexEditorPhysicalInputWidgetIdentity& LoadedIdentity =
		*Loaded->Events[0].Guard->UITarget;
	TestFalse(TEXT("Loaded digest is non-empty"), LoadedIdentity.IdentitySha256.IsEmpty());
	TestEqual(TEXT("Load rebuilds the same digest as live capture"),
		LoadedIdentity.IdentitySha256, Captured.IdentitySha256);

	// Live resolution: the same real selector observed on the exact route is Ready.
	FCortexEditorPhysicalInputUIObservation Observation;
	Observation.State = ECortexEditorUIObservationState::Ready;
	Observation.LocalPosition = Guard.ExpectedLocalPosition;
	Observation.ActualTarget = MakeShared<const FCortexEditorPhysicalInputWidgetIdentity>(Captured);

	FCortexEditorPhysicalInputPlayerPose ActualPose;
	const FCortexReplayGuardDecision Decision =
		FCortexReplayGuardEvaluator::Evaluate(Loaded->Events[0], ActualPose, Observation, false);
	TestTrue(TEXT("A live selector matching the loaded selector is Ready, not identity_mismatch"),
		Decision.State == ECortexReplayGuardDecisionState::Ready);

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
		FCortexReplayTimedEvent Event = MakeKeyDownEvent(0, 0.0);
		Event.Event.Guard = MakeSupportedGuard(TEXT("/Game/UI/WBP_ReplayTest.WBP_ReplayTest_C"));
		TestTrue(TEXT("Publish supported guard"), Library.Publish(Fixture.MakeRecording(Id, false, { Event })).bSuccess);
	}

	const FString InputsPath = Fixture.GetInputsPath(Id);
	TArray<FString> OriginalRows;
	TestTrue(TEXT("Read original inputs"), FCortexReplayTestFixture::ReadPayloadRows(InputsPath, OriginalRows));

	// Each tampered row is stored with a refreshed digest so Load reaches row validation.
	auto RestoreInputs = [&]()
	{
		TestTrue(TEXT("Restore inputs"), FCortexReplayTestFixture::WritePayloadRows(InputsPath, OriginalRows));
		TestTrue(TEXT("Restore inputs digest"),
			Fixture.RewriteMetadataStreamHash(Id, TEXT("inputs_sha256"), InputsPath));
	};

	auto RestoreAndLoad = [&](const TCHAR* Label)
	{
		TSharedPtr<const FCortexReplaySnapshot> Snapshot;
		FCortexReplayLibrary Reopened(Fixture.GetProjectRoot());
		const FCortexCommandResult Result = Reopened.Load(Id, false, Snapshot);
		TestFalse(Label, Result.bSuccess);
		TestEqual(TEXT("Rejected as an unsupported format"), Result.ErrorCode, FString(CortexReplayErrorCodes::UnsupportedRecordingFormat));
	};

	// Unknown surface string must not be normalised to a valid enum.
	RestoreInputs();
	{
		TSharedPtr<FJsonObject> Row = LoadStreamRowObject(InputsPath);
		TestTrue(TEXT("Read identity row"), GetGuardIdentityObject(Row).IsValid());
		TSharedPtr<FJsonObject> Identity = GetGuardIdentityObject(Row);
		if (Identity.IsValid())
		{
			Identity->SetStringField(TEXT("surface"), TEXT("sidebar"));
			TestTrue(TEXT("Write unknown surface"), StoreStreamRowObject(Fixture, Id, TEXT("inputs_sha256"), InputsPath, Row));
		}
	}
	RestoreAndLoad(TEXT("Unknown identity surface is rejected"));

	// Non-string ancestry entries must be rejected rather than dropped.
	RestoreInputs();
	{
		TSharedPtr<FJsonObject> Row = LoadStreamRowObject(InputsPath);
		TSharedPtr<FJsonObject> Identity = GetGuardIdentityObject(Row);
		if (Identity.IsValid())
		{
			TArray<TSharedPtr<FJsonValue>> BadAncestry;
			BadAncestry.Add(MakeShared<FJsonValueNumber>(1));
			Identity->SetArrayField(TEXT("widget_ancestry"), BadAncestry);
			TestTrue(TEXT("Write non-string ancestry"), StoreStreamRowObject(Fixture, Id, TEXT("inputs_sha256"), InputsPath, Row));
		}
	}
	RestoreAndLoad(TEXT("Non-string ancestry is rejected"));

	// not_applicable must not retain selector or local-position fields.
	RestoreInputs();
	{
		TSharedPtr<FJsonObject> Row = LoadStreamRowObject(InputsPath);
		TSharedPtr<FJsonObject> Ui = GetGuardUiObject(Row);
		if (Ui.IsValid())
		{
			Ui->SetStringField(TEXT("coverage"), TEXT("not_applicable"));
			TestTrue(TEXT("Write contradictory not_applicable guard"), StoreStreamRowObject(Fixture, Id, TEXT("inputs_sha256"), InputsPath, Row));
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

	FCortexReplayTimedEvent First = MakeKeyDownEvent(0, 0.0);
	FCortexReplayTimedEvent Second = MakeKeyDownEvent(1, 0.1);
	First.Event.Guard = MakeSupportedGuard(TEXT("/Game/UI/WBP_Interning.WBP_Interning_C"));
	Second.Event.Guard = MakeSupportedGuard(TEXT("/Game/UI/WBP_Interning.WBP_Interning_C"));
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
	FCortexReplayTimedEvent Third = MakeKeyDownEvent(0, 0.0);
	FCortexReplayTimedEvent Fourth = MakeKeyDownEvent(1, 0.1);
	Third.Event.Guard = MakeSupportedGuard(TEXT("/Game/UI/WBP_Other.WBP_Other_C"));
	Fourth.Event.Guard = MakeSupportedGuard(TEXT("/Game/UI/WBP_Third.WBP_Third_C"));
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
	TArray<FString> Rows;
	TestTrue(TEXT("Read inputs"), FCortexReplayTestFixture::ReadPayloadRows(InputsPath, Rows));
	if (!TestEqual(TEXT("One persisted row"), Rows.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("Persisted row carries the recorded key"), Rows[0].Contains(TEXT("\"SpaceBar\"")));
	const FString OriginalRow = Rows[0];

	// Inject a raw carriage return inside the persisted key string (byte-level edit of the
	// decompressed stream). It is a framing defect and must not be stripped into "SpaceBar".
	Rows[0] = OriginalRow.Replace(TEXT("\"SpaceBar\""), TEXT("\"Space\rBar\""));
	TestTrue(TEXT("Write CR-bearing row"), FCortexReplayTestFixture::WritePayloadRows(InputsPath, Rows));
	TestTrue(TEXT("Refresh the inputs digest"),
		Fixture.RewriteMetadataStreamHash(Id, TEXT("inputs_sha256"), InputsPath));

	TSharedPtr<const FCortexReplaySnapshot> Snapshot;
	FCortexReplayLibrary Reopened(Fixture.GetProjectRoot());
	const FCortexCommandResult Result = Reopened.Load(Id, false, Snapshot);
	TestFalse(TEXT("Embedded carriage return is rejected"), Result.bSuccess);
	TestEqual(TEXT("Embedded carriage return is a framing (invalid recording) error"), Result.ErrorCode,
		FString(CortexReplayErrorCodes::InvalidRecording));

	// The JSON escape \r is legal framing; the resulting key is not registered: row validation.
	Rows[0] = OriginalRow.Replace(TEXT("\"SpaceBar\""), TEXT("\"Space\\rBar\""));
	TestTrue(TEXT("Write escaped-CR row"), FCortexReplayTestFixture::WritePayloadRows(InputsPath, Rows));
	TestTrue(TEXT("Refresh the inputs digest again"),
		Fixture.RewriteMetadataStreamHash(Id, TEXT("inputs_sha256"), InputsPath));
	const FCortexCommandResult Escaped = Reopened.Load(Id, false, Snapshot);
	TestFalse(TEXT("An escaped carriage return key is rejected"), Escaped.bSuccess);
	TestEqual(TEXT("An escaped carriage return key is a format error"), Escaped.ErrorCode,
		FString(CortexReplayErrorCodes::UnsupportedRecordingFormat));

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

// ---------------------------------------------------------------------------
// CR-02 phase-1 R4: with the real hash provider, capture, load and the live hashing path all
// carry the SAME non-empty lower-case 64-hex selector digest (cross-stage equality, not just
// non-emptiness). The load-time provider-failure rejection is a pre-existing fail-closed
// regression, not a phase-2 red assertion.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibrarySelectorDigestStageConsistencyTest,
	"Cortex.Replay.Library.SelectorDigestStageConsistency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibrarySelectorDigestStageConsistencyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Slate not initialized - skipping selector digest stage test"));
		return true;
	}

	FCortexEditorPhysicalInputSelectorBuilder::ClearSelectorDigestFailureForTests();

	auto IsLowerHex64 = [](const FString& Value)
	{
		if (Value.Len() != 64) { return false; }
		for (const TCHAR Character : Value)
		{
			const bool bHex = (Character >= TEXT('0') && Character <= TEXT('9'))
				|| (Character >= TEXT('a') && Character <= TEXT('f'));
			if (!bHex) { return false; }
		}
		return true;
	};

	// Capture stage: a real tagged Slate root/control produce the live selector and digest.
	const TSharedRef<SBox> RootWidget = SNew(SBox);
	RootWidget->SetTag(FName(TEXT("CortexDigestStageRoot")));
	const TSharedRef<SBox> TargetWidget = SNew(SBox);
	TargetWidget->SetTag(FName(TEXT("CortexDigestStageTarget")));
	const FCortexEditorPhysicalInputWidgetIdentity Captured =
		FCortexEditorPhysicalInputSelectorBuilder::BuildSlateIdentity(*RootWidget, *TargetWidget);
	TestTrue(TEXT("Captured digest is a non-empty lower-case 64-hex SHA-256"),
		IsLowerHex64(Captured.IdentitySha256));

	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());
	int32 Id = 0;
	TestTrue(TEXT("Reserve"), Library.ReserveId(Id).bSuccess);

	FCortexReplayEvent Press;
	Press.Sequence = 0;
	Press.Input.Kind = ECortexEditorPhysicalInputKind::PointerDown;
	Press.Input.Key = EKeys::LeftMouseButton;
	Press.Input.ViewportPosition = FVector2D(0.5, 0.5);
	FCortexReplayInteractionGuard Guard;
	Guard.ExpectedPose = FCortexEditorPhysicalInputPlayerPose();
	Guard.UICoverage = ECortexEditorUICoverage::Supported;
	Guard.UITarget = MakeShared<const FCortexEditorPhysicalInputWidgetIdentity>(Captured);
	Guard.ExpectedLocalPosition = FVector2D(0.5, 0.5);
	Press.Guard = Guard;
	TestTrue(TEXT("Captured selector publishes"),
		Library.Publish(Fixture.MakeRecording(Id, false, { FCortexReplayTimedEvent{Press, 0.0} })).bSuccess);

	// Load stage: the loader rebuilds the identical digest from the selector alone.
	TSharedPtr<const FCortexReplaySnapshot> Loaded;
	TestTrue(TEXT("Captured selector loads"), Library.Load(Id, false, Loaded).bSuccess);
	if (!Loaded.IsValid() || Loaded->Events.Num() != 1
		|| !Loaded->Events[0].Guard.IsSet() || !Loaded->Events[0].Guard->UITarget.IsValid())
	{
		AddError(TEXT("Loaded recording did not carry the supported guard"));
		return false;
	}
	const FCortexEditorPhysicalInputWidgetIdentity& LoadedIdentity = *Loaded->Events[0].Guard->UITarget;
	TestTrue(TEXT("Loaded digest is a non-empty lower-case 64-hex SHA-256"),
		IsLowerHex64(LoadedIdentity.IdentitySha256));
	TestEqual(TEXT("Capture and load carry the same digest"),
		LoadedIdentity.IdentitySha256, Captured.IdentitySha256);

	// Live stage: the live hashing path over the loaded selector yields the same bytes.
	const FString LiveDigest =
		FCortexEditorPhysicalInputSelectorBuilder::ComputeIdentitySha256(LoadedIdentity);
	TestEqual(TEXT("Load and live hashing carry the same digest"),
		LiveDigest, Captured.IdentitySha256);

	// Provider failure: loading an already published recording is rejected (fail-closed).
	FCortexEditorPhysicalInputSelectorBuilder::SetSelectorDigestFailureForTests(true);
	FCortexReplayLibrary ReloadProbe(Fixture.GetProjectRoot());
	TSharedPtr<const FCortexReplaySnapshot> AfterFailure;
	const FCortexCommandResult FailedReload = ReloadProbe.Load(Id, false, AfterFailure);
	FCortexEditorPhysicalInputSelectorBuilder::ClearSelectorDigestFailureForTests();
	TestFalse(TEXT("A digest provider failure during load is rejected"), FailedReload.bSuccess);
	TestFalse(TEXT("A digest provider failure yields no snapshot"), AfterFailure.IsValid());

	return true;
}

// ---------------------------------------------------------------------------
// Legacy format-1 boundary: a recording labelled format 1 must be refused
// explicitly and never mutated by the refusal.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryVersionOneUnsupportedPreservedTest,
	"Cortex.Replay.Library.VersionOneUnsupportedPreserved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryVersionOneUnsupportedPreservedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 0;
	if (!TestTrue(TEXT("Reserve a recording id"), Library.ReserveId(Id).bSuccess))
	{
		return false;
	}

	// Publish a current recording, then relabel its metadata and initial state to the legacy
	// format-1 schema below; the library must refuse it explicitly rather than misread it as playable.
	const FCortexReplaySnapshot Legacy = Fixture.MakeRecording(Id, /*bAIEnabled=*/true, {});
	if (!TestTrue(TEXT("Publish the legacy recording"), Library.Publish(Legacy).bSuccess))
	{
		return false;
	}

	// Byte-snapshot every file this recording owns, before any read happens.
	const FString RecordingDir = FPaths::GetPath(Fixture.GetInputsPath(Id));

	// Relabel the freshly written recording to the legacy format-1 schema so this is a genuine
	// version-labelled legacy fixture, not a product legacy writer.
	{
		const FString MetadataPath = FPaths::Combine(RecordingDir, TEXT("metadata.json"));
		TSharedPtr<FJsonObject> Metadata = LoadJsonObject(MetadataPath);
		if (!TestTrue(TEXT("metadata.json is readable"), Metadata.IsValid()))
		{
			return false;
		}
		Metadata->SetNumberField(TEXT("schema_version"), 1);
		TestTrue(TEXT("metadata relabelled to the legacy schema"), SaveJsonObject(MetadataPath, Metadata));
	}
	{
		const FString InitialPath = Fixture.GetInitialStatePath(Id);
		TSharedPtr<FJsonObject> InitialState = LoadJsonObject(InitialPath);
		if (!TestTrue(TEXT("initial_state.json is readable"), InitialState.IsValid()))
		{
			return false;
		}
		InitialState->SetNumberField(TEXT("schema_version"), 1);
		TestTrue(TEXT("initial state relabelled to the legacy schema"), SaveJsonObject(InitialPath, InitialState));
	}

	TArray<FString> FilesBefore;
	IFileManager::Get().FindFilesRecursive(FilesBefore, *RecordingDir, TEXT("*"), /*Files=*/true, /*Directories=*/false);
	TMap<FString, TArray<uint8>> BytesBefore;
	for (const FString& File : FilesBefore)
	{
		TArray<uint8> Bytes;
		FFileHelper::LoadFileToArray(Bytes, *File);
		BytesBefore.Add(File, MoveTemp(Bytes));
	}
	TestTrue(TEXT("The legacy recording has on-disk files"), BytesBefore.Num() > 0);

	// Explicit refusal: a format-1 recording must never be presented as a loadable snapshot.
	TSharedPtr<const FCortexReplaySnapshot> Loaded;
	const FCortexCommandResult LoadResult = Library.Load(Id, /*bAIOnly=*/false, Loaded);
	TestFalse(TEXT("Loading a format-1 recording must fail explicitly"), LoadResult.bSuccess);
	TestFalse(TEXT("A refused legacy load must not yield a snapshot"), Loaded.IsValid());

	// AI listing must omit the unsupported recording entirely.
	TArray<FCortexReplayMetadata> AIList;
	TestTrue(TEXT("AI listing succeeds"), Library.List(/*bAIOnly=*/true, AIList).bSuccess);
	TestFalse(TEXT("AI listing must omit the unsupported format-1 recording"),
		AIList.ContainsByPredicate([Id](const FCortexReplayMetadata& Entry) { return Entry.RecordingId == Id; }));

	// The refusal must not have mutated a single byte.
	TArray<FString> FilesAfter;
	IFileManager::Get().FindFilesRecursive(FilesAfter, *RecordingDir, TEXT("*"), true, false);
	TestEqual(TEXT("The refusal must not add or remove files"), FilesAfter.Num(), BytesBefore.Num());
	for (const FString& File : FilesAfter)
	{
		const TArray<uint8>* const Before = BytesBefore.Find(File);
		if (Before == nullptr)
		{
			TestTrue(*FString::Printf(TEXT("unexpected new file after refusal: %s"), *File), false);
			continue;
		}
		TArray<uint8> After;
		FFileHelper::LoadFileToArray(After, *File);
		TestTrue(*FString::Printf(TEXT("bytes unchanged for %s"), *FPaths::GetCleanFilename(File)), After == *Before);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Format-2 frames stream boundary validation: each boundary is altered separately
// and must be refused as an invalid recording, not merely fail on a stale digest.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryFramesStreamValidationTest,
	"Cortex.Replay.Library.FramesStreamValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryFramesStreamValidationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	const auto MakeValidRecording = [&Fixture, &Library]()
	{
		int32 Id = 0;
		Library.ReserveId(Id);
		// Two events so sequence attribution has something to get wrong.
		return Fixture.MakeRecording(Id, false, { MakeKeyDownEvent(0, 0.0), MakeKeyDownEvent(1, 0.1) });
	};

	// Baseline: the fixture's warmup + owned frames publish and round-trip.
	{
		FCortexReplaySnapshot Snapshot = MakeValidRecording();
		TestTrue(TEXT("Valid frame set publishes"), Library.Publish(Snapshot).bSuccess);

		TSharedPtr<const FCortexReplaySnapshot> Loaded;
		TestTrue(TEXT("Valid frame set loads"), Library.Load(Snapshot.Metadata.RecordingId, false, Loaded).bSuccess);
		if (Loaded.IsValid())
		{
			TestEqual(TEXT("Loaded frames match the published frame count"), Loaded->Frames.Num(), Snapshot.Frames.Num());
			TestEqual(TEXT("Loaded timing matches"), Loaded->Metadata.Timing.FrameCount, Snapshot.Metadata.Timing.FrameCount);
			TestEqual(TEXT("Loaded duration is the sum of the frame deltas"), Loaded->Metadata.DurationSeconds,
				FMath::Abs(Snapshot.Metadata.DurationSeconds), 1e-9);
		}
	}

	// Each case below alters exactly one boundary of an otherwise valid recording.
	struct FFrameBoundaryCase
	{
		const TCHAR* Label;
		TFunction<void(FCortexReplaySnapshot&)> Alter;
	};

	// Alters the last frame that actually carries input, so a boundary case never degenerates into a
	// no-op when the valid fixture's frame layout has more than one input frame.
	const auto LastInputFrame = [](FCortexReplaySnapshot& S) -> FCortexReplayFrame&
	{
		for (int32 Index = S.Frames.Num() - 1; Index >= 0; --Index)
		{
			if (S.Frames[Index].EventCount > 0)
			{
				return S.Frames[Index];
			}
		}
		return S.Frames[1];
	};

	const TArray<FFrameBoundaryCase> Cases = {
		{ TEXT("Non-contiguous frame_index"), [](FCortexReplaySnapshot& S) { S.Frames[1].FrameIndex = 2; } },
		{ TEXT("FirstSequence does not continue from the previous frame"), [](FCortexReplaySnapshot& S) { S.Frames[1].FirstSequence = 5; } },
		{ TEXT("Frame attributes more events than the inputs stream"), [&](FCortexReplaySnapshot& S) { LastInputFrame(S).EventCount += 1; } },
		{ TEXT("Frame attributes fewer events than the inputs stream"), [&](FCortexReplaySnapshot& S) { LastInputFrame(S).EventCount -= 1; } },
		{ TEXT("timing.frame_count disagrees with the frame set"), [](FCortexReplaySnapshot& S) { S.Metadata.Timing.FrameCount = S.Frames.Num() + 1; } },
		{ TEXT("Non-finite application delta"), [](FCortexReplaySnapshot& S) { S.Frames[1].AppDeltaSeconds = std::numeric_limits<double>::infinity(); } },
		{ TEXT("Negative application delta"), [](FCortexReplaySnapshot& S) { S.Frames[1].AppDeltaSeconds = -0.5; } },
		{ TEXT("Zero application delta"), [](FCortexReplaySnapshot& S) { S.Frames[1].AppDeltaSeconds = 0.0; } },
		{ TEXT("Application delta above the documented cap"), [](FCortexReplaySnapshot& S) { S.Frames[1].AppDeltaSeconds = CortexReplayMaxFrameDeltaSeconds + 1.0; } },
		{ TEXT("duration_seconds disagrees with the frame deltas"), [](FCortexReplaySnapshot& S) { S.Metadata.DurationSeconds += 1.0; } },
		{ TEXT("Negative event count"), [](FCortexReplaySnapshot& S) { S.Frames[1].EventCount = -1; } },
	};

	for (const FFrameBoundaryCase& Case : Cases)
	{
		FCortexReplaySnapshot Snapshot = MakeValidRecording();
		Case.Alter(Snapshot);

		const FCortexCommandResult Result = Library.Publish(Snapshot);
		TestFalse(*FString::Printf(TEXT("%s is refused"), Case.Label), Result.bSuccess);
		TestEqual(*FString::Printf(TEXT("%s is classified as an invalid recording"), Case.Label),
			Result.ErrorCode, FString(CortexReplayErrorCodes::InvalidRecording));

		// A refused publication must leave no recording behind.
		TSharedPtr<const FCortexReplaySnapshot> Loaded;
		TestFalse(*FString::Printf(TEXT("%s leaves no loadable recording"), Case.Label),
			Library.Load(Snapshot.Metadata.RecordingId, false, Loaded).bSuccess);
	}

	// On-disk change: an edit of the frames stream that keeps it decodable is caught by its recorded
	// digest (the stale hash is refused before any row is parsed).
	{
		FCortexReplaySnapshot Snapshot = MakeValidRecording();
		TestTrue(TEXT("Tamper baseline publishes"), Library.Publish(Snapshot).bSuccess);

		const FString FramesPath = Fixture.GetFramesPath(Snapshot.Metadata.RecordingId);
		TestTrue(TEXT("frames.jsonl.gz exists"), FPaths::FileExists(FramesPath));

		TArray<FString> FrameRows;
		TestTrue(TEXT("frames.jsonl.gz is readable"),
			FCortexReplayTestFixture::ReadPayloadRows(FramesPath, FrameRows));
		// Insignificant trailing whitespace keeps every row valid, so only the recorded frames digest
		// can catch the change.
		for (FString& Row : FrameRows)
		{
			Row += TEXT(" ");
		}
		TestTrue(TEXT("Rewrite frames.jsonl.gz with insignificant whitespace"),
			FCortexReplayTestFixture::WritePayloadRows(FramesPath, FrameRows));

		TSharedPtr<const FCortexReplaySnapshot> Loaded;
		FCortexReplayLibrary Reopened(Fixture.GetProjectRoot());
		const FCortexCommandResult Result = Reopened.Load(Snapshot.Metadata.RecordingId, false, Loaded);
		TestFalse(TEXT("A changed frames stream denies load"), Result.bSuccess);
		TestEqual(TEXT("A changed frames stream is an invalid recording"),
			Result.ErrorCode, FString(CortexReplayErrorCodes::InvalidRecording));
	}

	return true;
}

// ---------------------------------------------------------------------------
// Format 4: frames are the engine frames that consumed input after the epoch, written as compact
// rows. world_tick is omitted when derivable, an object, or an explicit null; the viewport state and
// input-config prerequisites round-trip, and duration_seconds is the sum of the frame deltas.
// Catches any serializer/parser regression of the frame row, initial_state.viewport or the
// prerequisites.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryFormatFourRoundTripTest,
	"Cortex.Replay.Library.FormatFourRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryFormatFourRoundTripTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 0;
	if (!TestTrue(TEXT("Reserve"), Library.ReserveId(Id).bSuccess))
	{
		return false;
	}

	FCortexReplaySnapshot Recording = Fixture.MakeRecording(Id, false, { MakeKeyDownEvent(0, 0.05) });
	Recording.Frames[0].WorldTick.Reset();
	Recording.Frames[1].WorldTick->bPaused = true;
	Recording.Metadata.Prerequisites.PlayerInputClass = TEXT("/Script/Engine.PlayerInput");
	Recording.Metadata.Prerequisites.InputConfigSha256 = FString::ChrN(64, TEXT('b'));
	Recording.InitialState.Viewport.bMouseCapture = true;
	Recording.InitialState.Viewport.bHighPrecision = true;
	if (!TestTrue(TEXT("Publish"), Library.Publish(Recording).bSuccess))
	{
		return false;
	}

	TSharedPtr<const FCortexReplaySnapshot> Loaded;
	if (!TestTrue(TEXT("Load"), Library.Load(Id, false, Loaded).bSuccess) || !Loaded.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("Metadata schema is 4"), Loaded->Metadata.SchemaVersion, 4);
	TestEqual(TEXT("Initial-state schema is 4"), Loaded->InitialState.SchemaVersion, 4);
	TestFalse(TEXT("A frame without a world tick stays without one"), Loaded->Frames[0].WorldTick.IsSet());
	if (TestTrue(TEXT("A ticked frame keeps its world tick"), Loaded->Frames[1].WorldTick.IsSet()))
	{
		TestTrue(TEXT("The paused flag round-trips"), Loaded->Frames[1].WorldTick->bPaused);
		TestEqual(TEXT("The real delta round-trips"), Loaded->Frames[1].WorldTick->RealDeltaSeconds,
			Recording.Frames[1].WorldTick->RealDeltaSeconds);
	}
	TestTrue(TEXT("Viewport mouse capture round-trips"), Loaded->InitialState.Viewport.bMouseCapture);
	TestTrue(TEXT("Viewport high precision round-trips"), Loaded->InitialState.Viewport.bHighPrecision);
	TestEqual(TEXT("Player input class round-trips"), Loaded->Metadata.Prerequisites.PlayerInputClass,
		Recording.Metadata.Prerequisites.PlayerInputClass);
	TestEqual(TEXT("Input config digest round-trips"), Loaded->Metadata.Prerequisites.InputConfigSha256,
		Recording.Metadata.Prerequisites.InputConfigSha256);
	TestEqual(TEXT("Duration is the sum of the frame deltas"), Loaded->Metadata.DurationSeconds,
		Recording.Metadata.DurationSeconds, 1e-9);

	TArray<FString> FrameRows;
	if (!TestTrue(TEXT("frames.jsonl.gz is readable"),
		FCortexReplayTestFixture::ReadPayloadRows(Fixture.GetFramesPath(Id), FrameRows)))
	{
		return false;
	}
	const FString FramesText = FString::Join(FrameRows, TEXT("\n"));
	TestTrue(TEXT("An unticked frame is written as an explicit null"), FramesText.Contains(TEXT("\"world_tick\":null")));
	TestFalse(TEXT("Retired frame fields are not written"), FramesText.Contains(TEXT("capture_frame")));

	if (TestTrue(TEXT("The stream has an idle frame row"), FrameRows.Num() > 2))
	{
		TestFalse(TEXT("An idle ticked frame row has no world_tick key"), FrameRows[2].Contains(TEXT("world_tick")));
		TestFalse(TEXT("An idle frame row has no event_count key"), FrameRows[2].Contains(TEXT("event_count")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Legacy format-3 recordings (plain .jsonl streams) are refused as UNSUPPORTED_RECORDING_FORMAT by
// Load, ahead of payload presence, and reported by the metadata-only listing with that load error.
// The refusal never changes a byte; the recording stays deletable.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryLegacyFormatThreeRefusedTest,
	"Cortex.Replay.Library.LegacyFormatThreeRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryLegacyFormatThreeRefusedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	const auto RelabelToThree = [&Fixture](int32 Id, bool bMetadataToo) -> bool
	{
		const FString InitialPath = Fixture.GetInitialStatePath(Id);
		TSharedPtr<FJsonObject> InitialState = LoadJsonObject(InitialPath);
		if (!InitialState.IsValid())
		{
			return false;
		}
		InitialState->SetNumberField(TEXT("schema_version"), 3);
		if (!SaveJsonObject(InitialPath, InitialState))
		{
			return false;
		}

		if (bMetadataToo)
		{
			const FString MetadataPath = Fixture.GetMetadataPath(Id);
			TSharedPtr<FJsonObject> Metadata = LoadJsonObject(MetadataPath);
			if (!Metadata.IsValid())
			{
				return false;
			}
			Metadata->SetNumberField(TEXT("schema_version"), 3);
			if (!SaveJsonObject(MetadataPath, Metadata))
			{
				return false;
			}
		}
		return true;
	};

	// Case 1: a format-3 directory: relabelled metadata and initial state, plain .jsonl payloads.
	int32 Id = 0;
	Library.ReserveId(Id);
	if (!TestTrue(TEXT("Publish"), Library.Publish(Fixture.MakeRecording(Id, false, {})).bSuccess))
	{
		return false;
	}

	const FString RecordingDirectory = FPaths::GetPath(Fixture.GetInputsPath(Id));
	TestTrue(TEXT("Relabel to format 3"), RelabelToThree(Id, true));
	IFileManager::Get().Delete(*Fixture.GetInputsPath(Id));
	IFileManager::Get().Delete(*Fixture.GetFramesPath(Id));
	TestTrue(TEXT("Write legacy inputs.jsonl"),
		FFileHelper::SaveStringToFile(TEXT(""), *FPaths::Combine(RecordingDirectory, TEXT("inputs.jsonl"))));
	TestTrue(TEXT("Write legacy frames.jsonl"),
		FFileHelper::SaveStringToFile(TEXT(""), *FPaths::Combine(RecordingDirectory, TEXT("frames.jsonl"))));

	TArray<FString> FilesBefore;
	IFileManager::Get().FindFilesRecursive(FilesBefore, *RecordingDirectory, TEXT("*"), true, false);
	TMap<FString, TArray<uint8>> BytesBefore;
	for (const FString& File : FilesBefore)
	{
		TArray<uint8> Bytes;
		FFileHelper::LoadFileToArray(Bytes, *File);
		BytesBefore.Add(File, MoveTemp(Bytes));
	}

	TSharedPtr<const FCortexReplaySnapshot> Loaded;
	const FCortexCommandResult LoadResult = Library.Load(Id, false, Loaded);
	TestFalse(TEXT("A format-3 recording does not load"), LoadResult.bSuccess);
	TestEqual(TEXT("It is refused as an unsupported format, not as incomplete"), LoadResult.ErrorCode,
		FString(CortexReplayErrorCodes::UnsupportedRecordingFormat));

	TArray<FCortexReplayLibraryEntry> Entries;
	TestTrue(TEXT("Listing succeeds"), Library.ListEntries(Entries).bSuccess);
	if (TestEqual(TEXT("The recording is listed"), Entries.Num(), 1))
	{
		TestFalse(TEXT("It has no usable metadata"), Entries[0].Metadata.IsSet());
		TestEqual(TEXT("It reports the unsupported format"), Entries[0].LoadError.ErrorCode,
			FString(CortexReplayErrorCodes::UnsupportedRecordingFormat));
	}

	TArray<FString> FilesAfter;
	IFileManager::Get().FindFilesRecursive(FilesAfter, *RecordingDirectory, TEXT("*"), true, false);
	TestEqual(TEXT("The refusal does not add or remove files"), FilesAfter.Num(), BytesBefore.Num());
	for (const FString& File : FilesAfter)
	{
		const TArray<uint8>* const Before = BytesBefore.Find(File);
		TArray<uint8> After;
		FFileHelper::LoadFileToArray(After, *File);
		TestTrue(*FString::Printf(TEXT("bytes unchanged for %s"), *FPaths::GetCleanFilename(File)),
			Before != nullptr && After == *Before);
	}

	TestTrue(TEXT("The refused recording deletes"), Library.Delete(Id).bSuccess);

	// Case 2: only the initial state is relabelled and the .gz payloads are gone: the initial-state
	// version gate precedes payload presence.
	int32 SecondId = 0;
	Library.ReserveId(SecondId);
	if (!TestTrue(TEXT("Publish second"), Library.Publish(Fixture.MakeRecording(SecondId, false, {})).bSuccess))
	{
		return false;
	}
	TestTrue(TEXT("Relabel initial state to format 3"), RelabelToThree(SecondId, false));
	IFileManager::Get().Delete(*Fixture.GetInputsPath(SecondId));
	IFileManager::Get().Delete(*Fixture.GetFramesPath(SecondId));

	Loaded.Reset();
	const FCortexCommandResult SecondResult = Library.Load(SecondId, false, Loaded);
	TestFalse(TEXT("A format-3 initial state does not load"), SecondResult.bSuccess);
	TestEqual(TEXT("The initial-state gate precedes payload presence"), SecondResult.ErrorCode,
		FString(CortexReplayErrorCodes::UnsupportedRecordingFormat));
	return true;
}

// ---------------------------------------------------------------------------
// The metadata-only listing neither parses nor hashes the payload: a recording whose inputs were
// corrupted is still listed with its metadata, while a directory without metadata is listed with a
// load error. Delete removes either kind. Catches a regression that makes the listing load payloads
// (the per-refresh cost) or hide unloadable recordings, and one that refuses to delete them.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryEntriesAndDeleteUnloadableTest,
	"Cortex.Replay.Library.EntriesAndDeleteUnloadable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryEntriesAndDeleteUnloadableTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Good = 0;
	int32 Corrupt = 0;
	int32 BadInitial = 0;
	Library.ReserveId(Good);
	Library.ReserveId(Corrupt);
	Library.ReserveId(BadInitial);
	TestTrue(TEXT("Publish good"), Library.Publish(Fixture.MakeRecording(Good, false, {})).bSuccess);
	TestTrue(TEXT("Publish corrupt"), Library.Publish(Fixture.MakeRecording(Corrupt, false, {})).bSuccess);
	TestTrue(TEXT("Publish bad initial state"), Library.Publish(Fixture.MakeRecording(BadInitial, false, {})).bSuccess);
	const FTCHARToUTF8 NotGzip(TEXT("not gzip"));
	TestTrue(TEXT("Corrupt the inputs payload"),
		FFileHelper::SaveArrayToFile(
			TArray<uint8>(reinterpret_cast<const uint8*>(NotGzip.Get()), NotGzip.Length()),
			*Fixture.GetInputsPath(Corrupt)));
	TestTrue(TEXT("Truncate the initial state"),
		FFileHelper::SaveStringToFile(TEXT("{"),
			*FPaths::Combine(FPaths::GetPath(Fixture.GetInputsPath(BadInitial)), TEXT("initial_state.json"))));

	const int32 Empty = BadInitial + 1;
	IFileManager::Get().MakeDirectory(
		*FPaths::GetPath(Fixture.GetInputsPath(Empty)), /*Tree=*/true);

	TArray<FCortexReplayLibraryEntry> Entries;
	TestTrue(TEXT("Listing succeeds"), Library.ListEntries(Entries).bSuccess);
	if (!TestEqual(TEXT("Every directory is listed"), Entries.Num(), 4))
	{
		return false;
	}
	TestEqual(TEXT("Entries are in id order"), Entries[0].RecordingId, Good);
	TestTrue(TEXT("The good recording has metadata"), Entries[0].Metadata.IsSet());
	TestTrue(TEXT("A corrupt payload does not hide the recording from the metadata listing"),
		Entries[1].Metadata.IsSet());
	// The initial state is already parsed by the listing: a malformed one is known to be unloadable.
	TestFalse(TEXT("A malformed initial state is not listed as playable"), Entries[2].Metadata.IsSet());
	TestEqual(TEXT("A malformed initial state names the format error"), Entries[2].LoadError.ErrorCode,
		FString(CortexReplayErrorCodes::UnsupportedRecordingFormat));
	TestFalse(TEXT("A directory without metadata has none"), Entries[3].Metadata.IsSet());
	TestFalse(TEXT("It explains why with a load error"), Entries[3].LoadError.bSuccess);

	TSharedPtr<const FCortexReplaySnapshot> Loaded;
	const FCortexCommandResult CorruptLoad = Library.Load(Corrupt, false, Loaded);
	TestFalse(TEXT("The corrupt recording does not load"), CorruptLoad.bSuccess);
	TestEqual(TEXT("A non-gzip payload is an invalid recording"), CorruptLoad.ErrorCode,
		FString(CortexReplayErrorCodes::InvalidRecording));

	const int64 RevisionBefore = Library.GetRevision();
	TestTrue(TEXT("The corrupt recording deletes"), Library.Delete(Corrupt).bSuccess);
	TestTrue(TEXT("The metadata-less directory deletes"), Library.Delete(Empty).bSuccess);
	TestTrue(TEXT("Deleting bumps the revision"), Library.GetRevision() > RevisionBefore);
	TestFalse(TEXT("The corrupt recording is gone"),
		IFileManager::Get().DirectoryExists(*FPaths::GetPath(Fixture.GetInputsPath(Corrupt))));
	TestFalse(TEXT("The metadata-less directory is gone"),
		IFileManager::Get().DirectoryExists(*FPaths::GetPath(Fixture.GetInputsPath(Empty))));
	TestTrue(TEXT("The good recording remains"),
		IFileManager::Get().DirectoryExists(*FPaths::GetPath(Fixture.GetInputsPath(Good))));
	return true;
}

// ---------------------------------------------------------------------------
// Audit C7: an occupied recording path is environment state, so publication reports a retryable
// storage failure, while content defects stay INVALID_RECORDING (see FramesStreamValidation).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryOccupiedPathRetryableTest,
	"Cortex.Replay.Library.OccupiedPathIsRetryable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryOccupiedPathRetryableTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 0;
	Library.ReserveId(Id);
	TestTrue(TEXT("First publish"), Library.Publish(Fixture.MakeRecording(Id, false, {})).bSuccess);
	const FCortexCommandResult Second = Library.Publish(Fixture.MakeRecording(Id, false, {}));
	TestFalse(TEXT("A second publish never overwrites"), Second.bSuccess);
	TestEqual(TEXT("The occupied path is a storage failure"), Second.ErrorCode,
		FString(CortexReplayErrorCodes::StorageFailure));
	return true;
}

// ---------------------------------------------------------------------------
// Format 4 storage layout: a published recording is exactly metadata.json, initial_state.json and the
// two gzip streams; the recorded stream digests are the SHA-256 of the file bytes.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryPublishWritesCompressedStreamsTest,
	"Cortex.Replay.Library.PublishWritesCompressedStreams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryPublishWritesCompressedStreamsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 0;
	Library.ReserveId(Id);
	if (!TestTrue(TEXT("Publish"), Library.Publish(Fixture.MakeRecording(Id, false, { MakeKeyDownEvent(0, 0.0) })).bSuccess))
	{
		return false;
	}

	TArray<FString> Names;
	IFileManager::Get().FindFiles(Names, *FPaths::Combine(RecordingDirectoryPath(Fixture, Id), TEXT("*")), true, true);
	Names.Sort();
	const TArray<FString> Expected = { TEXT("frames.jsonl.gz"), TEXT("initial_state.json"), TEXT("inputs.jsonl.gz"), TEXT("metadata.json") };
	TestTrue(TEXT("The directory holds exactly the four recording files"), Names == Expected);

	const TSharedPtr<FJsonObject> Metadata = LoadJsonObject(Fixture.GetMetadataPath(Id));
	if (!TestTrue(TEXT("Metadata is readable"), Metadata.IsValid()))
	{
		return false;
	}

	const auto CheckStream = [this, &Metadata](const TCHAR* Label, const FString& Path, const TCHAR* HashField)
	{
		TArray<uint8> Bytes;
		TestTrue(*FString::Printf(TEXT("%s is readable"), Label), FFileHelper::LoadFileToArray(Bytes, *Path));
		if (TestTrue(*FString::Printf(TEXT("%s has a gzip header"), Label), Bytes.Num() >= 2))
		{
			TestEqual(*FString::Printf(TEXT("%s first magic byte"), Label), Bytes[0], static_cast<uint8>(0x1f));
			TestEqual(*FString::Printf(TEXT("%s second magic byte"), Label), Bytes[1], static_cast<uint8>(0x8b));
		}

		FString Sha256;
		FString Error;
		TestTrue(*FString::Printf(TEXT("%s hashes"), Label), CortexReplayStorage::ComputeSha256Hex(Bytes, Sha256, Error));
		TestEqual(*FString::Printf(TEXT("%s digest covers the file bytes"), Label),
			Metadata->GetStringField(HashField), Sha256);
	};
	CheckStream(TEXT("inputs.jsonl.gz"), Fixture.GetInputsPath(Id), TEXT("inputs_sha256"));
	CheckStream(TEXT("frames.jsonl.gz"), Fixture.GetFramesPath(Id), TEXT("frames_sha256"));
	return true;
}

// ---------------------------------------------------------------------------
// A recording without input events publishes an empty (valid) inputs stream and loads.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryEmptyInputStreamRoundTripTest,
	"Cortex.Replay.Library.EmptyInputStreamRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryEmptyInputStreamRoundTripTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 1;
	Library.ReserveId(Id);
	if (!TestTrue(TEXT("Publish"), Library.Publish(Fixture.MakeRecording(Id, false, {})).bSuccess))
	{
		return false;
	}

	TSharedPtr<const FCortexReplaySnapshot> Loaded;
	if (TestTrue(TEXT("Load"), Library.Load(Id, false, Loaded).bSuccess) && Loaded.IsValid())
	{
		TestEqual(TEXT("No events"), Loaded->Events.Num(), 0);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Hash before parse: an edited row whose stream digest was not refreshed is an invalid recording
// (replaces "strict validation must reject it before any hash comparison").
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryStaleHashEditedRowInvalidTest,
	"Cortex.Replay.Library.StaleHashEditedRowInvalid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryStaleHashEditedRowInvalidTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 0;
	Library.ReserveId(Id);
	if (!TestTrue(TEXT("Publish"), Library.Publish(Fixture.MakeRecording(Id, false, { MakeKeyDownEvent(0, 0.0) })).bSuccess))
	{
		return false;
	}

	const FString InputsPath = Fixture.GetInputsPath(Id);
	TArray<FString> Rows;
	if (!TestTrue(TEXT("Read rows"), FCortexReplayTestFixture::ReadPayloadRows(InputsPath, Rows))
		|| !TestTrue(TEXT("Has a row"), Rows.Num() > 0))
	{
		return false;
	}

	const int32 Replacements = Rows[0].ReplaceInline(TEXT("\"SpaceBar\""), TEXT("\"Enter\""));
	TestEqual(TEXT("Exactly one replacement"), Replacements, 1);
	TestTrue(TEXT("Write edited rows"), FCortexReplayTestFixture::WritePayloadRows(InputsPath, Rows));

	TArray<uint8> Bytes;
	FString Sha256;
	FString Error;
	TestTrue(TEXT("Read edited file"), FFileHelper::LoadFileToArray(Bytes, *InputsPath));
	TestTrue(TEXT("Hash edited file"), CortexReplayStorage::ComputeSha256Hex(Bytes, Sha256, Error));
	const TSharedPtr<FJsonObject> Metadata = LoadJsonObject(Fixture.GetMetadataPath(Id));
	if (TestTrue(TEXT("Metadata is readable"), Metadata.IsValid()))
	{
		TestNotEqual(TEXT("The file digest differs from the recorded one"),
			Sha256, Metadata->GetStringField(TEXT("inputs_sha256")));
	}

	TSharedPtr<const FCortexReplaySnapshot> Loaded;
	const FCortexCommandResult Result = Library.Load(Id, false, Loaded);
	TestFalse(TEXT("A stale digest denies load"), Result.bSuccess);
	TestEqual(TEXT("A stale digest is an invalid recording"), Result.ErrorCode,
		FString(CortexReplayErrorCodes::InvalidRecording));
	return true;
}

// ---------------------------------------------------------------------------
// A malformed row whose stream digest was also refreshed passes the hash and fails row validation.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryMatchingHashMalformedRowUnsupportedTest,
	"Cortex.Replay.Library.MatchingHashMalformedRowUnsupported",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryMatchingHashMalformedRowUnsupportedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 0;
	Library.ReserveId(Id);
	if (!TestTrue(TEXT("Publish"), Library.Publish(Fixture.MakeRecording(Id, false, { MakeKeyDownEvent(0, 0.0) })).bSuccess))
	{
		return false;
	}

	const FString InputsPath = Fixture.GetInputsPath(Id);
	TSharedPtr<FJsonObject> Row = LoadStreamRowObject(InputsPath);
	if (!TestTrue(TEXT("Read the first row"), Row.IsValid()))
	{
		return false;
	}
	Row->SetStringField(TEXT("wheel_delta"), TEXT("x"));
	if (!TestTrue(TEXT("Store the malformed row with a matching digest"),
		StoreStreamRowObject(Fixture, Id, TEXT("inputs_sha256"), InputsPath, Row)))
	{
		return false;
	}

	TSharedPtr<const FCortexReplaySnapshot> Loaded;
	const FCortexCommandResult Result = Library.Load(Id, false, Loaded);
	TestFalse(TEXT("A malformed row denies load"), Result.bSuccess);
	TestEqual(TEXT("A malformed row with a matching digest is an unsupported format"), Result.ErrorCode,
		FString(CortexReplayErrorCodes::UnsupportedRecordingFormat));
	return true;
}

// ---------------------------------------------------------------------------
// The metadata version gate precedes the initial-state read: a legacy metadata.json whose
// initial_state.json is gone is an unsupported format, not an incomplete recording.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexReplayLibraryMetadataGatePrecedesInitialStateTest,
	"Cortex.Replay.Library.MetadataGatePrecedesInitialState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexReplayLibraryMetadataGatePrecedesInitialStateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FCortexReplayTestFixture Fixture;
	FCortexReplayLibrary Library(Fixture.GetProjectRoot());

	int32 Id = 0;
	Library.ReserveId(Id);
	if (!TestTrue(TEXT("Publish"), Library.Publish(Fixture.MakeRecording(Id, false, {})).bSuccess))
	{
		return false;
	}

	const FString MetadataPath = Fixture.GetMetadataPath(Id);
	TSharedPtr<FJsonObject> Metadata = LoadJsonObject(MetadataPath);
	if (!TestTrue(TEXT("Metadata is readable"), Metadata.IsValid()))
	{
		return false;
	}
	Metadata->SetNumberField(TEXT("schema_version"), 3);
	TestTrue(TEXT("Relabel metadata to format 3"), SaveJsonObject(MetadataPath, Metadata));
	TestTrue(TEXT("Delete initial_state.json"), IFileManager::Get().Delete(*Fixture.GetInitialStatePath(Id)));

	TSharedPtr<const FCortexReplaySnapshot> Loaded;
	const FCortexCommandResult Result = Library.Load(Id, false, Loaded);
	TestFalse(TEXT("The recording does not load"), Result.bSuccess);
	TestEqual(TEXT("The metadata version gate wins over the missing initial state"), Result.ErrorCode,
		FString(CortexReplayErrorCodes::UnsupportedRecordingFormat));

	TArray<FCortexReplayLibraryEntry> Entries;
	TestTrue(TEXT("Listing succeeds"), Library.ListEntries(Entries).bSuccess);
	if (TestEqual(TEXT("The recording is listed"), Entries.Num(), 1))
	{
		TestEqual(TEXT("The listing reports the unsupported format"), Entries[0].LoadError.ErrorCode,
			FString(CortexReplayErrorCodes::UnsupportedRecordingFormat));
	}
	return true;
}
