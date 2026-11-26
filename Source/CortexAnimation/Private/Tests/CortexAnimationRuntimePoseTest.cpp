#include "Misc/AutomationTest.h"

#include "CoreGlobals.h"
#include "Animation/AnimSequence.h"
#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "CortexAnimationCommandHandler.h"
#include "CortexAnimationTestRuntimeInstance.h"
#include "CortexCommandRouter.h"
#include "CortexTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/PackageName.h"
#include "Operations/CortexAnimRuntimeOps.h"
#include "UObject/UObjectGlobals.h"

/**
 * anim.sample_runtime_pose (issue 233). The sampler is driven on a transient game world with explicit ticks, so game time,
 * time dilation and lifecycle are deterministic and the suite stays NullRHI safe; the PIE wiring itself is the shared
 * FCortexPIEUtils::StartDeferredPIEPoll (covered by Cortex.QA.PIE.DeferredOpsLifecycle) plus the live MCP smoke.
 */
namespace CortexAnimRuntimePoseTest
{
	const TCHAR* MeshPath = TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple");
	const TCHAR* JogPath = TEXT("/Game/Characters/Mannequins/Anims/Unarmed/Jog/MF_Unarmed_Jog_Fwd.MF_Unarmed_Jog_Fwd");
	constexpr float FrameSeconds = 0.01f;

	template <typename T>
	T* LoadContent(const TCHAR* ObjectPath)
	{
		const FString PackageName = FPackageName::ObjectPathToPackageName(FString(ObjectPath));
		return FPackageName::DoesPackageExist(PackageName) ? LoadObject<T>(nullptr, ObjectPath) : nullptr;
	}

	struct FFixture
	{
		UWorld* World = nullptr;
		ASkeletalMeshActor* Actor = nullptr;
		USkeletalMeshComponent* Mesh = nullptr;

		~FFixture()
		{
			if (World != nullptr)
			{
				World->BeginTearingDown();
				World->DestroyWorld(false);
				if (GEngine != nullptr)
				{
					GEngine->DestroyWorldContext(World);
				}
			}
		}

		/** A begun-play game world holding one ASkeletalMeshActor with the Manny mesh that refreshes bones without rendering. */
		bool Create(FAutomationTestBase& Test, USkeletalMesh* SkeletalMesh)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			Actor = World->SpawnActor<ASkeletalMeshActor>(ASkeletalMeshActor::StaticClass(), FTransform::Identity);
			if (!Test.TestNotNull(TEXT("the skeletal mesh actor spawns"), Actor))
			{
				return false;
			}
			Mesh = Actor->GetSkeletalMeshComponent();
			Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
			Mesh->SetSkeletalMesh(SkeletalMesh);
			World->BeginPlay();
			// Without a game mode the world dispatches no BeginPlay, so the mesh would never register its (anim) tick.
			if (!Actor->HasActorBegunPlay())
			{
				Actor->DispatchBeginPlay();
			}
			return true;
		}

		/** One game frame: tick functions run once per GFrameCounter, so a frame inside one automation tick advances it like the engine loop. */
		void Tick(const float Seconds = FrameSeconds)
		{
			++GFrameCounter;
			World->Tick(ELevelTick::LEVELTICK_All, Seconds);
		}
	};

	TSharedPtr<FJsonObject> Params(const FString& Actor, const TArray<FString>& Bones, const FString& Space, const int32 Count, const double Interval)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("actor"), Actor);
		TArray<TSharedPtr<FJsonValue>> BoneValues;
		for (const FString& Bone : Bones)
		{
			BoneValues.Add(MakeShared<FJsonValueString>(Bone));
		}
		Out->SetArrayField(TEXT("bones"), BoneValues);
		Out->SetStringField(TEXT("space"), Space);
		Out->SetNumberField(TEXT("sample_count"), Count);
		Out->SetNumberField(TEXT("interval_seconds"), Interval);
		return Out;
	}

	void SetStrings(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		for (const FString& Value : Values)
		{
			Array.Add(MakeShared<FJsonValueString>(Value));
		}
		Object->SetArrayField(Field, Array);
	}

	/** Ticks the world one frame, then steps the sampler, until it answers or MaxFrames pass. OnStep runs after each step. */
	TOptional<FCortexCommandResult> Run(FFixture& Fixture, FCortexAnimPoseSampler& Sampler, const int32 MaxFrames, int32& OutFrames,
		const TFunction<void()>& OnStep = nullptr)
	{
		for (OutFrames = 1; OutFrames <= MaxFrames; ++OutFrames)
		{
			Fixture.Tick();
			TOptional<FCortexCommandResult> Result = Sampler.Step(*Fixture.World, 0.0);
			if (OnStep)
			{
				OnStep();
			}
			if (Result.IsSet())
			{
				return Result;
			}
		}
		return NullOpt;
	}

	TArray<TSharedPtr<FJsonObject>> Samples(const FCortexCommandResult& Result)
	{
		TArray<TSharedPtr<FJsonObject>> Out;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Result.Data.IsValid() && Result.Data->TryGetArrayField(TEXT("samples"), Values))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				Out.Add(Value->AsObject());
			}
		}
		return Out;
	}

	TSharedPtr<FJsonObject> Bone(const TSharedPtr<FJsonObject>& Sample, const TCHAR* Name)
	{
		const TSharedPtr<FJsonObject>* Bones = nullptr;
		const TSharedPtr<FJsonObject>* Entry = nullptr;
		return Sample.IsValid() && Sample->TryGetObjectField(TEXT("bones"), Bones) && (*Bones)->TryGetObjectField(Name, Entry) ? *Entry : nullptr;
	}

	FVector Vector(const TSharedPtr<FJsonObject>& Entry, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>& Object = Entry->GetObjectField(Field);
		return FVector(Object->GetNumberField(TEXT("x")), Object->GetNumberField(TEXT("y")), Object->GetNumberField(TEXT("z")));
	}

	FRotator Rotator(const TSharedPtr<FJsonObject>& Entry)
	{
		const TSharedPtr<FJsonObject>& Object = Entry->GetObjectField(TEXT("rotation"));
		return FRotator(Object->GetNumberField(TEXT("pitch")), Object->GetNumberField(TEXT("yaw")), Object->GetNumberField(TEXT("roll")));
	}

	FCortexCommandResult Parse(UWorld* World, const TSharedPtr<FJsonObject>& Params, FCortexAnimPoseRequest& OutRequest)
	{
		FCortexCommandResult Error;
		Error.bSuccess = FCortexAnimRuntimeOps::ParseRequest(World, Params, OutRequest, Error);
		return Error;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationRuntimePoseRefPoseTest,
	"Cortex.Animation.RuntimePose.RefPose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationRuntimePoseRefPoseTest::RunTest(const FString& Parameters)
{
	using namespace CortexAnimRuntimePoseTest;
	USkeletalMesh* SkeletalMesh = LoadContent<USkeletalMesh>(MeshPath);
	if (SkeletalMesh == nullptr)
	{
		AddInfo(TEXT("SKM_Manny_Simple is not available; skipping"));
		return true;
	}

	FFixture Fixture;
	if (!Fixture.Create(*this, SkeletalMesh))
	{
		return false;
	}
	Fixture.Mesh->SetAnimInstanceClass(UCortexAnimTestRuntimeInstance::StaticClass());
	Fixture.Tick();

	// ── delta_from_ref: a graph-less instance keeps the reference pose, so every delta is zero ──
	TSharedPtr<FJsonObject> DeltaParams = Params(Fixture.Actor->GetName(), { TEXT("pelvis"), TEXT("hand_r") }, TEXT("delta_from_ref"), 3, 0.0);
	SetStrings(DeltaParams, TEXT("properties"), { TEXT("Elapsed"), TEXT("bBlendActive") });
	FCortexAnimPoseRequest Request;
	if (!TestTrue(TEXT("the delta request validates"), Parse(Fixture.World, DeltaParams, Request).bSuccess))
	{
		return false;
	}
	FCortexAnimPoseSampler DeltaSampler(Request, 0.0);
	int32 Frames = 0;
	TOptional<FCortexCommandResult> Delta = Run(Fixture, DeltaSampler, 20, Frames);
	if (!TestTrue(TEXT("the delta run answers"), Delta.IsSet() && Delta->bSuccess && Delta->Data.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("the delta run completes"), Delta->Data->GetBoolField(TEXT("completed")));
	TestEqual(TEXT("the delta run stops as completed"), Delta->Data->GetStringField(TEXT("stop_reason")), FString(TEXT("completed")));
	TestEqual(TEXT("the response names the space"), Delta->Data->GetStringField(TEXT("space")), FString(TEXT("delta_from_ref")));
	TestEqual(TEXT("the response names the anim instance class"), Delta->Data->GetStringField(TEXT("anim_instance_class")),
		UCortexAnimTestRuntimeInstance::StaticClass()->GetPathName());
	const TArray<TSharedPtr<FJsonObject>> DeltaSamples = Samples(*Delta);
	TestEqual(TEXT("interval 0 takes one sample per frame"), DeltaSamples.Num(), 3);
	TestEqual(TEXT("interval 0 needs exactly sample_count frames"), Frames, 3);
	double PreviousElapsed = -1.0;
	for (const TSharedPtr<FJsonObject>& Sample : DeltaSamples)
	{
		for (const TCHAR* Name : { TEXT("pelvis"), TEXT("hand_r") })
		{
			const TSharedPtr<FJsonObject> Entry = Bone(Sample, Name);
			if (!TestNotNull(*FString::Printf(TEXT("%s is sampled"), Name), Entry.Get()))
			{
				continue;
			}
			TestEqual(*FString::Printf(TEXT("%s has no rotation delta"), Name), Entry->GetNumberField(TEXT("angle_degrees")), 0.0, 0.01);
			TestTrue(*FString::Printf(TEXT("%s has no translation delta"), Name), Vector(Entry, TEXT("translation")).IsNearlyZero(0.01));
			TestTrue(*FString::Printf(TEXT("%s has no scale delta"), Name), Vector(Entry, TEXT("scale")).IsNearlyZero(0.001));
		}
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		if (TestTrue(TEXT("each sample logs the requested properties"), Sample->TryGetObjectField(TEXT("properties"), Properties)))
		{
			const double Elapsed = (*Properties)->GetNumberField(TEXT("Elapsed"));
			TestTrue(TEXT("Elapsed advances with every sample"), Elapsed > PreviousElapsed);
			PreviousElapsed = Elapsed;
			TestTrue(TEXT("bBlendActive is logged"), (*Properties)->GetBoolField(TEXT("bBlendActive")));
		}
	}

	// ── local: equals the reference pose from the engine ──
	FCortexAnimPoseRequest LocalRequest;
	if (!TestTrue(TEXT("the local request validates"),
		Parse(Fixture.World, Params(Fixture.Actor->GetName(), { TEXT("hand_r") }, TEXT("local"), 1, 0.0), LocalRequest).bSuccess))
	{
		return false;
	}
	FCortexAnimPoseSampler LocalSampler(LocalRequest, 0.0);
	TOptional<FCortexCommandResult> Local = Run(Fixture, LocalSampler, 5, Frames);
	if (TestTrue(TEXT("the local run answers"), Local.IsSet() && Local->bSuccess) && Samples(*Local).Num() == 1)
	{
		const FTransform Reference = Fixture.Mesh->GetRefPoseTransform(Fixture.Mesh->GetBoneIndex(TEXT("hand_r")));
		const TSharedPtr<FJsonObject> Entry = Bone(Samples(*Local)[0], TEXT("hand_r"));
		if (TestNotNull(TEXT("hand_r is sampled in local space"), Entry.Get()))
		{
			TestTrue(TEXT("local translation is the reference translation"), Vector(Entry, TEXT("translation")).Equals(Reference.GetTranslation(), 0.01));
			TestTrue(TEXT("local rotation is the reference rotation"), Rotator(Entry).Equals(Reference.Rotator(), 0.01));
			TestFalse(TEXT("local space reports no delta angle"), Entry->HasField(TEXT("angle_degrees")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationRuntimePosePlayingTest,
	"Cortex.Animation.RuntimePose.Playing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationRuntimePosePlayingTest::RunTest(const FString& Parameters)
{
	using namespace CortexAnimRuntimePoseTest;
	USkeletalMesh* SkeletalMesh = LoadContent<USkeletalMesh>(MeshPath);
	UAnimSequence* Jog = LoadContent<UAnimSequence>(JogPath);
	if (SkeletalMesh == nullptr || Jog == nullptr)
	{
		AddInfo(TEXT("Mannequin mesh or jog animation is not available; skipping"));
		return true;
	}

	FFixture Fixture;
	if (!Fixture.Create(*this, SkeletalMesh))
	{
		return false;
	}
	Fixture.Mesh->PlayAnimation(Jog, true);
	Fixture.Tick();

	FCortexAnimPoseRequest Request;
	if (!TestTrue(TEXT("the component request validates"),
		Parse(Fixture.World, Params(Fixture.Actor->GetName(), { TEXT("hand_r"), TEXT("foot_l") }, TEXT("component"), 6, 0.05), Request).bSuccess))
	{
		return false;
	}

	// What the engine reports right after each step, keyed by game time, to match against the samples.
	TArray<TPair<double, FTransform>> Observed;
	FCortexAnimPoseSampler Sampler(Request, 0.0);
	int32 Frames = 0;
	TOptional<FCortexCommandResult> Result = Run(Fixture, Sampler, 200, Frames, [&Fixture, &Observed]()
	{
		Observed.Emplace(Fixture.World->GetTimeSeconds(), Fixture.Mesh->GetSocketTransform(TEXT("hand_r"), RTS_Component));
	});
	if (!TestTrue(TEXT("the playing run answers"), Result.IsSet() && Result->bSuccess && Result->Data.IsValid()))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonObject>> PlayingSamples = Samples(*Result);
	if (!TestEqual(TEXT("six samples are taken"), PlayingSamples.Num(), 6))
	{
		return false;
	}
	for (const TSharedPtr<FJsonObject>& Sample : PlayingSamples)
	{
		const double GameTime = Sample->GetNumberField(TEXT("game_time"));
		const TPair<double, FTransform>* Match = Observed.FindByPredicate([GameTime](const TPair<double, FTransform>& Entry)
		{
			return FMath::IsNearlyEqual(Entry.Key, GameTime, 1.0e-6);
		});
		const TSharedPtr<FJsonObject> Entry = Bone(Sample, TEXT("hand_r"));
		if (TestNotNull(TEXT("the sample time is a frame the test observed"), Match) && TestNotNull(TEXT("hand_r is sampled"), Entry.Get()))
		{
			TestTrue(TEXT("component translation equals GetSocketTransform(RTS_Component)"),
				Vector(Entry, TEXT("translation")).Equals(Match->Value.GetTranslation(), 0.01));
			TestTrue(TEXT("component rotation equals GetSocketTransform(RTS_Component)"), Rotator(Entry).Equals(Match->Value.Rotator(), 0.01));
		}
		TestNotNull(TEXT("foot_l is sampled"), Bone(Sample, TEXT("foot_l")).Get());
	}
	const FVector First = Vector(Bone(PlayingSamples[0], TEXT("hand_r")), TEXT("translation"));
	const FVector Last = Vector(Bone(PlayingSamples.Last(), TEXT("hand_r")), TEXT("translation"));
	TestTrue(TEXT("the jogging hand moves between the first and last sample"), !First.Equals(Last, 1.0));
	TestEqual(TEXT("the response names the mesh"), Result->Data->GetStringField(TEXT("skeletal_mesh")), SkeletalMesh->GetPathName());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationRuntimePoseGameTimeTest,
	"Cortex.Animation.RuntimePose.GameTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationRuntimePoseGameTimeTest::RunTest(const FString& Parameters)
{
	using namespace CortexAnimRuntimePoseTest;
	USkeletalMesh* SkeletalMesh = LoadContent<USkeletalMesh>(MeshPath);
	if (SkeletalMesh == nullptr)
	{
		AddInfo(TEXT("SKM_Manny_Simple is not available; skipping"));
		return true;
	}

	FFixture Fixture;
	if (!Fixture.Create(*this, SkeletalMesh))
	{
		return false;
	}
	Fixture.Tick();

	constexpr double Interval = 0.02;
	int32 FramesByDilation[2] = { 0, 0 };
	const float Dilations[2] = { 1.0f, 0.5f };
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		Fixture.World->GetWorldSettings()->TimeDilation = Dilations[Pass];
		FCortexAnimPoseRequest Request;
		if (!TestTrue(TEXT("the request validates"),
			Parse(Fixture.World, Params(Fixture.Actor->GetName(), { TEXT("pelvis") }, TEXT("local"), 4, Interval), Request).bSuccess))
		{
			return false;
		}
		FCortexAnimPoseSampler Sampler(Request, 0.0);
		TOptional<FCortexCommandResult> Result = Run(Fixture, Sampler, 100, FramesByDilation[Pass]);
		if (!TestTrue(TEXT("the run answers"), Result.IsSet() && Result->bSuccess))
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonObject>> RunSamples = Samples(*Result);
		TestEqual(TEXT("four samples are taken"), RunSamples.Num(), 4);
		for (int32 Index = 1; Index < RunSamples.Num(); ++Index)
		{
			const double Spacing = RunSamples[Index]->GetNumberField(TEXT("game_time")) - RunSamples[Index - 1]->GetNumberField(TEXT("game_time"));
			TestTrue(*FString::Printf(TEXT("samples are at least interval_seconds of game time apart (%.4f)"), Spacing), Spacing >= Interval - 1.0e-4);
			TestTrue(*FString::Printf(TEXT("samples are not more than one frame late (%.4f)"), Spacing), Spacing < Interval + FrameSeconds * Dilations[Pass] + 1.0e-4);
			TestEqual(TEXT("each sample reports the time dilation"), RunSamples[Index]->GetNumberField(TEXT("time_dilation")), static_cast<double>(Dilations[Pass]), 1.0e-4);
		}
	}
	// 3 intervals of 0.02 s at 0.01 s frames: 1 + 3*2 = 7 frames undilated; at 0.5 dilation 1 + 3*4 = 13.
	TestEqual(TEXT("undilated sampling takes seven frames"), FramesByDilation[0], 7);
	TestEqual(TEXT("half-speed sampling takes thirteen frames"), FramesByDilation[1], 13);
	Fixture.World->GetWorldSettings()->TimeDilation = 1.0f;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationRuntimePoseLifecycleTest,
	"Cortex.Animation.RuntimePose.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationRuntimePoseLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace CortexAnimRuntimePoseTest;
	USkeletalMesh* SkeletalMesh = LoadContent<USkeletalMesh>(MeshPath);
	if (SkeletalMesh == nullptr)
	{
		AddInfo(TEXT("SKM_Manny_Simple is not available; skipping"));
		return true;
	}

	FFixture Fixture;
	if (!Fixture.Create(*this, SkeletalMesh))
	{
		return false;
	}
	Fixture.Tick();

	// ── Real-time budget: a step past timeout_seconds stops with the samples taken so far ──
	TSharedPtr<FJsonObject> TimeoutParams = Params(Fixture.Actor->GetName(), { TEXT("pelvis") }, TEXT("local"), 10, 0.0);
	TimeoutParams->SetNumberField(TEXT("timeout_seconds"), 5.0);
	FCortexAnimPoseRequest TimeoutRequest;
	if (!TestTrue(TEXT("the timeout request validates"), Parse(Fixture.World, TimeoutParams, TimeoutRequest).bSuccess))
	{
		return false;
	}
	FCortexAnimPoseSampler TimeoutSampler(TimeoutRequest, 100.0);
	Fixture.Tick();
	TestFalse(TEXT("a step inside the budget keeps sampling"), TimeoutSampler.Step(*Fixture.World, 101.0).IsSet());
	Fixture.Tick();
	TestFalse(TEXT("a second step inside the budget keeps sampling"), TimeoutSampler.Step(*Fixture.World, 104.0).IsSet());
	Fixture.Tick();
	TOptional<FCortexCommandResult> TimedOut = TimeoutSampler.Step(*Fixture.World, 105.5);
	if (TestTrue(TEXT("a step past the budget answers"), TimedOut.IsSet() && TimedOut->bSuccess && TimedOut->Data.IsValid()))
	{
		TestFalse(TEXT("a timed-out run is not complete"), TimedOut->Data->GetBoolField(TEXT("completed")));
		TestEqual(TEXT("the stop reason is timeout"), TimedOut->Data->GetStringField(TEXT("stop_reason")), FString(TEXT("timeout")));
		TestEqual(TEXT("the samples taken before the budget ran out are kept"), Samples(*TimedOut).Num(), 2);
	}

	// ── Target destroyed mid-run ──
	FCortexAnimPoseRequest Request;
	if (!TestTrue(TEXT("the request validates"),
		Parse(Fixture.World, Params(Fixture.Actor->GetName(), { TEXT("pelvis") }, TEXT("local"), 10, 0.0), Request).bSuccess))
	{
		return false;
	}
	const FString ActorName = Fixture.Actor->GetName();
	FCortexAnimPoseSampler Sampler(Request, 0.0);
	Fixture.Tick();
	TestFalse(TEXT("the first step samples"), Sampler.Step(*Fixture.World, 0.0).IsSet());
	Fixture.Tick();
	TestFalse(TEXT("the second step samples"), Sampler.Step(*Fixture.World, 0.0).IsSet());
	Fixture.Actor->Destroy();
	Fixture.Actor = nullptr;
	Fixture.Mesh = nullptr;
	Fixture.Tick();
	TOptional<FCortexCommandResult> Destroyed = Sampler.Step(*Fixture.World, 0.0);
	if (TestTrue(TEXT("the run answers once the target is gone"), Destroyed.IsSet() && Destroyed->bSuccess && Destroyed->Data.IsValid()))
	{
		TestFalse(TEXT("a run whose target was destroyed is not complete"), Destroyed->Data->GetBoolField(TEXT("completed")));
		TestEqual(TEXT("the stop reason is target_destroyed"), Destroyed->Data->GetStringField(TEXT("stop_reason")), FString(TEXT("target_destroyed")));
		TestEqual(TEXT("the samples taken before destruction are kept"), Samples(*Destroyed).Num(), 2);
		TestEqual(TEXT("the response still names the actor"), Destroyed->Data->GetStringField(TEXT("actor")), ActorName);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCortexAnimationRuntimePoseRefusalsTest,
	"Cortex.Animation.RuntimePose.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCortexAnimationRuntimePoseRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace CortexAnimRuntimePoseTest;

	// ── No PIE: the routed command refuses before deferring ──
	{
		FCortexCommandRouter Router;
		Router.RegisterDomain(TEXT("anim"), TEXT("Cortex Animation"), TEXT("1.0.0"), MakeShared<FCortexAnimationCommandHandler>());
		bool bCallbackRan = false;
		const FCortexCommandResult NoPie = Router.Execute(TEXT("anim.sample_runtime_pose"),
			Params(TEXT("Hero"), { TEXT("pelvis") }, TEXT("local"), 1, 0.0), [&bCallbackRan](FCortexCommandResult) { bCallbackRan = true; });
		TestFalse(TEXT("sample_runtime_pose without PIE fails"), NoPie.bSuccess);
		TestFalse(TEXT("sample_runtime_pose without PIE is not deferred"), NoPie.bIsDeferred);
		TestEqual(TEXT("sample_runtime_pose without PIE code"), NoPie.ErrorCode, CortexErrorCodes::PIENotActive);
		TestFalse(TEXT("no deferred answer follows a refusal"), bCallbackRan);
	}

	USkeletalMesh* SkeletalMesh = LoadContent<USkeletalMesh>(MeshPath);
	if (SkeletalMesh == nullptr)
	{
		AddInfo(TEXT("SKM_Manny_Simple is not available; skipping the world refusals"));
		return true;
	}
	FFixture Fixture;
	if (!Fixture.Create(*this, SkeletalMesh))
	{
		return false;
	}
	Fixture.Mesh->SetAnimInstanceClass(UCortexAnimTestRuntimeInstance::StaticClass());

	// A plain actor with two skeletal meshes and no anim instance.
	AActor* TwoMeshes = Fixture.World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity);
	for (const TCHAR* Name : { TEXT("MeshA"), TEXT("MeshB") })
	{
		USkeletalMeshComponent* Component = NewObject<USkeletalMeshComponent>(TwoMeshes, Name);
		Component->SetSkeletalMesh(SkeletalMesh);
		Component->RegisterComponent();
		TwoMeshes->AddInstanceComponent(Component);
	}
	AActor* NoMesh = Fixture.World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity);
	Fixture.Tick();

	const FString Hero = Fixture.Actor->GetName();
	const auto Expect = [this, &Fixture](const TCHAR* What, const TSharedPtr<FJsonObject>& In, const FString& Code)
	{
		FCortexAnimPoseRequest Request;
		const FCortexCommandResult Result = Parse(Fixture.World, In, Request);
		TestFalse(*FString::Printf(TEXT("%s is refused"), What), Result.bSuccess);
		TestEqual(*FString::Printf(TEXT("%s code"), What), Result.ErrorCode, Code);
	};
	const auto Valid = [&Hero]() { return Params(Hero, { TEXT("pelvis") }, TEXT("local"), 2, 0.1); };

	TSharedPtr<FJsonObject> Neither = Valid();
	Neither->RemoveField(TEXT("actor"));
	Expect(TEXT("neither actor nor actor_class"), Neither, CortexErrorCodes::InvalidField);
	TSharedPtr<FJsonObject> Both = Valid();
	Both->SetStringField(TEXT("actor_class"), TEXT("SkeletalMeshActor"));
	Expect(TEXT("both actor and actor_class"), Both, CortexErrorCodes::InvalidField);
	Expect(TEXT("an unknown actor"), Params(TEXT("NoSuchActor"), { TEXT("pelvis") }, TEXT("local"), 2, 0.1), CortexErrorCodes::ActorNotFound);

	TSharedPtr<FJsonObject> ByClass = Valid();
	ByClass->RemoveField(TEXT("actor"));
	ByClass->SetStringField(TEXT("actor_class"), TEXT("SkeletalMeshActor"));
	FCortexAnimPoseRequest ByClassRequest;
	if (TestTrue(TEXT("a class matching one actor resolves"), Parse(Fixture.World, ByClass, ByClassRequest).bSuccess))
	{
		TestTrue(TEXT("the class filter resolves the skeletal mesh actor"), ByClassRequest.Actor.Get() == Fixture.Actor);
	}
	ByClass->SetStringField(TEXT("actor_class"), TEXT("/Script/Engine.SkeletalMeshActor"));
	TestTrue(TEXT("a class path resolves too"), Parse(Fixture.World, ByClass, ByClassRequest).bSuccess);
	ByClass->SetStringField(TEXT("actor_class"), TEXT("Actor"));
	{
		FCortexAnimPoseRequest Request;
		const FCortexCommandResult Ambiguous = Parse(Fixture.World, ByClass, Request);
		TestEqual(TEXT("a class matching several actors is ambiguous"), Ambiguous.ErrorCode, CortexErrorCodes::AmbiguousActor);
		const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
		TestTrue(TEXT("the ambiguity lists candidates"), Ambiguous.ErrorDetails.IsValid()
			&& Ambiguous.ErrorDetails->TryGetArrayField(TEXT("candidates"), Candidates) && Candidates->Num() > 1);
	}
	ByClass->SetStringField(TEXT("actor_class"), TEXT("NoSuchClass"));
	Expect(TEXT("a class matching no actor"), ByClass, CortexErrorCodes::ActorNotFound);

	TSharedPtr<FJsonObject> MissingComponent = Valid();
	MissingComponent->SetStringField(TEXT("mesh_component"), TEXT("NoSuchMesh"));
	Expect(TEXT("an unknown mesh component"), MissingComponent, CortexErrorCodes::ComponentNotFound);
	Expect(TEXT("an actor without a skeletal mesh"), Params(NoMesh->GetName(), { TEXT("pelvis") }, TEXT("local"), 2, 0.1), CortexErrorCodes::ComponentNotFound);
	Expect(TEXT("an actor with two skeletal meshes"), Params(TwoMeshes->GetName(), { TEXT("pelvis") }, TEXT("local"), 2, 0.1), CortexErrorCodes::AmbiguousComponent);
	TSharedPtr<FJsonObject> Named = Params(TwoMeshes->GetName(), { TEXT("pelvis") }, TEXT("local"), 2, 0.1);
	Named->SetStringField(TEXT("mesh_component"), TEXT("MeshB"));
	FCortexAnimPoseRequest NamedRequest;
	if (TestTrue(TEXT("naming one of two meshes resolves"), Parse(Fixture.World, Named, NamedRequest).bSuccess))
	{
		TestEqual(TEXT("the named mesh is selected"), NamedRequest.Mesh.IsValid() ? NamedRequest.Mesh->GetName() : FString(), FString(TEXT("MeshB")));
	}
	SetStrings(Named, TEXT("properties"), { TEXT("Elapsed") });
	Expect(TEXT("properties on a mesh without an anim instance"), Named, CortexErrorCodes::InvalidOperation);

	TSharedPtr<FJsonObject> NoBones = Valid();
	NoBones->RemoveField(TEXT("bones"));
	Expect(TEXT("missing bones"), NoBones, CortexErrorCodes::InvalidField);
	Expect(TEXT("empty bones"), Params(Hero, {}, TEXT("local"), 2, 0.1), CortexErrorCodes::InvalidField);
	Expect(TEXT("an unknown bone"), Params(Hero, { TEXT("no_such_bone") }, TEXT("local"), 2, 0.1), CortexErrorCodes::InvalidField);
	Expect(TEXT("a duplicate bone"), Params(Hero, { TEXT("pelvis"), TEXT("pelvis") }, TEXT("local"), 2, 0.1), CortexErrorCodes::InvalidField);
	TArray<FString> TooMany;
	for (int32 Index = 0; Index < 65; ++Index)
	{
		TooMany.Add(FString::Printf(TEXT("bone_%d"), Index));
	}
	Expect(TEXT("more than 64 bones"), Params(Hero, TooMany, TEXT("local"), 2, 0.1), CortexErrorCodes::InvalidField);
	Expect(TEXT("an unknown space"), Params(Hero, { TEXT("pelvis") }, TEXT("world"), 2, 0.1), CortexErrorCodes::InvalidField);
	Expect(TEXT("zero samples"), Params(Hero, { TEXT("pelvis") }, TEXT("local"), 0, 0.1), CortexErrorCodes::InvalidField);
	Expect(TEXT("more than 300 samples"), Params(Hero, { TEXT("pelvis") }, TEXT("local"), 301, 0.1), CortexErrorCodes::InvalidField);
	TSharedPtr<FJsonObject> Fraction = Valid();
	Fraction->SetNumberField(TEXT("sample_count"), 2.5);
	Expect(TEXT("a fractional sample count"), Fraction, CortexErrorCodes::InvalidField);
	Expect(TEXT("a negative interval"), Params(Hero, { TEXT("pelvis") }, TEXT("local"), 2, -0.1), CortexErrorCodes::InvalidField);
	Expect(TEXT("an interval above 60 s"), Params(Hero, { TEXT("pelvis") }, TEXT("local"), 2, 61.0), CortexErrorCodes::InvalidField);
	TSharedPtr<FJsonObject> ZeroTimeout = Valid();
	ZeroTimeout->SetNumberField(TEXT("timeout_seconds"), 0.0);
	Expect(TEXT("a zero timeout"), ZeroTimeout, CortexErrorCodes::InvalidField);
	TSharedPtr<FJsonObject> LongTimeout = Valid();
	LongTimeout->SetNumberField(TEXT("timeout_seconds"), 1801.0);
	Expect(TEXT("a timeout above 1800 s"), LongTimeout, CortexErrorCodes::InvalidField);
	TSharedPtr<FJsonObject> UnknownProperty = Valid();
	SetStrings(UnknownProperty, TEXT("properties"), { TEXT("NoSuchProperty") });
	Expect(TEXT("an unknown anim instance property"), UnknownProperty, CortexErrorCodes::PropertyNotFound);
	return true;
}
