#include "Misc/AutomationTest.h"

#include "CortexReplayLibrary.h"
#include "CortexReplayTestUtils.h"
#include "CortexReplayTypes.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

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
