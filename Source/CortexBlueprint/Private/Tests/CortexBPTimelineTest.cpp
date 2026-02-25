#include "Misc/AutomationTest.h"
#include "CortexBPCommandHandler.h"
#include "CortexCommandRouter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/TimelineTemplate.h"
#include "Misc/PackageName.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexBPTimelineTest,
	"Cortex.Blueprint.ConfigureTimeline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexBPTimelineTest::RunTest(const FString& Parameters)
{
	FCortexBPCommandHandler Handler;
	const FString TestPath = TEXT("/Game/Temp/CortexBPTest_Timeline");
	const FString TestBPPath = TestPath + TEXT("/BP_TimelineTest");

	{
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("name"), TEXT("BP_TimelineTest"));
		CreateParams->SetStringField(TEXT("path"), TestPath);
		CreateParams->SetStringField(TEXT("type"), TEXT("Actor"));
		FCortexCommandResult CreateResult = Handler.Execute(TEXT("create"), CreateParams);
		TestTrue(TEXT("create should succeed"), CreateResult.bSuccess);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("timeline_name"), TEXT("BounceTimeline"));
		Params->SetNumberField(TEXT("length"), 0.5);
		Params->SetBoolField(TEXT("loop"), false);

		TArray<TSharedPtr<FJsonValue>> TracksArray;
		TSharedPtr<FJsonObject> Track = MakeShared<FJsonObject>();
		Track->SetStringField(TEXT("type"), TEXT("float"));
		Track->SetStringField(TEXT("name"), TEXT("ZOffset"));

		TArray<TSharedPtr<FJsonValue>> Keys;
		auto AddKey = [&Keys](const float Time, const float Value)
		{
			TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
			Key->SetNumberField(TEXT("time"), Time);
			Key->SetNumberField(TEXT("value"), Value);
			Keys.Add(MakeShared<FJsonValueObject>(Key));
		};

		AddKey(0.0f, 0.0f);
		AddKey(0.25f, 150.0f);
		AddKey(0.5f, 0.0f);
		Track->SetArrayField(TEXT("keys"), Keys);
		TracksArray.Add(MakeShared<FJsonValueObject>(Track));
		Params->SetArrayField(TEXT("tracks"), TracksArray);

		FCortexCommandResult Result = Handler.Execute(TEXT("configure_timeline"), Params);
		TestTrue(TEXT("configure_timeline should succeed"), Result.bSuccess);

		if (Result.Data.IsValid())
		{
			FString TimelineName;
			Result.Data->TryGetStringField(TEXT("timeline_name"), TimelineName);
			TestEqual(TEXT("timeline_name should match"), TimelineName, TEXT("BounceTimeline"));

			double TrackCount = 0.0;
			Result.Data->TryGetNumberField(TEXT("track_count"), TrackCount);
			TestEqual(TEXT("track_count should be 1"), static_cast<int32>(TrackCount), 1);
		}
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		FCortexCommandResult Result = Handler.Execute(TEXT("configure_timeline"), Params);
		TestFalse(TEXT("missing timeline_name should fail"), Result.bSuccess);
		TestEqual(TEXT("error should be INVALID_FIELD"), Result.ErrorCode, CortexErrorCodes::InvalidField);
	}

	// Invalid updates should fail without mutating existing track set.
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TestBPPath);
		Params->SetStringField(TEXT("timeline_name"), TEXT("BounceTimeline"));

		TArray<TSharedPtr<FJsonValue>> TracksArray;
		TSharedPtr<FJsonObject> BadTrack = MakeShared<FJsonObject>();
		BadTrack->SetStringField(TEXT("type"), TEXT("float"));
		BadTrack->SetStringField(TEXT("name"), TEXT("BadTrack"));
		// Intentionally omit keys to force validation failure.
		TracksArray.Add(MakeShared<FJsonValueObject>(BadTrack));
		Params->SetArrayField(TEXT("tracks"), TracksArray);

		FCortexCommandResult Result = Handler.Execute(TEXT("configure_timeline"), Params);
		TestFalse(TEXT("invalid configure_timeline should fail"), Result.bSuccess);
	}

	{
		if (UObject* Obj = LoadObject<UBlueprint>(nullptr, *TestBPPath))
		{
			UBlueprint* BP = Cast<UBlueprint>(Obj);
			TestNotNull(TEXT("Blueprint should load"), BP);
			if (BP != nullptr)
			{
				UTimelineTemplate* Timeline = BP->FindTimelineTemplateByVariableName(TEXT("BounceTimeline"));
				TestNotNull(TEXT("Timeline should still exist"), Timeline);
				if (Timeline != nullptr)
				{
					TestEqual(TEXT("Float tracks should remain unchanged after failed update"),
						Timeline->FloatTracks.Num(), 1);
				}
			}
		}
	}

	const FString PackagePath = FPackageName::ObjectPathToPackageName(TestBPPath);
	if (FindPackage(nullptr, *PackagePath) || FPackageName::DoesPackageExist(PackagePath))
	{
		if (UObject* BP = LoadObject<UBlueprint>(nullptr, *TestBPPath))
		{
			BP->GetOutermost()->MarkAsGarbage();
		}
	}

	return true;
}
