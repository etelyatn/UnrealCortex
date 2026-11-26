#include "Operations/CortexAnimRuntimeOps.h"

#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "CortexCommandRouter.h"
#include "CortexPIEUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformTime.h"
#include "JsonObjectConverter.h"

namespace CortexAnimRuntimeOpsPrivate
{
	constexpr int32 MaxBones = 64;
	constexpr int32 MaxSamples = 300;
	constexpr int32 MaxProperties = 32;
	constexpr double MaxIntervalSeconds = 60.0;
	constexpr double MinTimeoutSeconds = 1.0;
	constexpr double MaxTimeoutSeconds = 1800.0;
	/** Frame deltas are floats, so N frames can sum to a hair below an exact multiple of the interval. */
	constexpr double ScheduleToleranceSeconds = 1.0e-6;

	FCortexCommandResult InvalidField(const FString& Message)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, Message);
	}

	/** An optional number in [Min, Max]; integers only when bInteger. Absent keeps OutValue. */
	bool ReadNumber(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, const double Min, const double Max, const bool bInteger,
		double& OutValue, FCortexCommandResult& OutError)
	{
		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
		if (!Value.IsValid())
		{
			return true;
		}
		double Number = 0.0;
		if (Value->Type != EJson::Number || !Value->TryGetNumber(Number) || !FMath::IsFinite(Number)
			|| Number < Min || Number > Max || (bInteger && FMath::Frac(Number) != 0.0))
		{
			OutError = InvalidField(FString::Printf(TEXT("%s must be a%s number in [%g, %g]"), Field, bInteger ? TEXT("n integer") : TEXT(""), Min, Max));
			return false;
		}
		OutValue = Number;
		return true;
	}

	/** An optional array of strings; Required refuses an absent field. */
	bool ReadStrings(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, const bool bRequired, TArray<FString>& OutValues, FCortexCommandResult& OutError)
	{
		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
		if (!Value.IsValid())
		{
			if (bRequired)
			{
				OutError = InvalidField(FString::Printf(TEXT("Missing required param: %s"), Field));
				return false;
			}
			return true;
		}
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Value->TryGetArray(Array))
		{
			OutError = InvalidField(FString::Printf(TEXT("%s must be an array of strings"), Field));
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			FString Text;
			if (!Entry.IsValid() || Entry->Type != EJson::String || !Entry->TryGetString(Text) || Text.IsEmpty())
			{
				OutError = InvalidField(FString::Printf(TEXT("%s must contain non-empty strings"), Field));
				return false;
			}
			if (OutValues.Contains(Text))
			{
				OutError = InvalidField(FString::Printf(TEXT("%s contains '%s' twice"), Field, *Text));
				return false;
			}
			OutValues.Add(Text);
		}
		return true;
	}

	/** The class or any super class is named Filter (Blueprint classes also without `_C`) or has the path Filter. */
	bool ClassMatches(const UClass* Class, const FString& Filter)
	{
		for (const UClass* Candidate = Class; Candidate != nullptr; Candidate = Candidate->GetSuperClass())
		{
			const FString Name = Candidate->GetName();
			if (Name == Filter || Name == Filter + TEXT("_C") || Candidate->GetPathName() == Filter)
			{
				return true;
			}
		}
		return false;
	}

	AActor* ResolveActor(UWorld* World, const TSharedPtr<FJsonObject>& Params, FCortexCommandResult& OutError)
	{
		FString ActorName;
		FString ActorClass;
		Params->TryGetStringField(TEXT("actor"), ActorName);
		Params->TryGetStringField(TEXT("actor_class"), ActorClass);
		if (ActorName.IsEmpty() == ActorClass.IsEmpty())
		{
			OutError = InvalidField(TEXT("Pass exactly one of actor or actor_class"));
			return nullptr;
		}

		if (!ActorName.IsEmpty())
		{
			AActor* Actor = FCortexPIEUtils::FindActorInPIE(World, ActorName);
			if (Actor == nullptr)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, FString::Printf(TEXT("Actor not found: %s"), *ActorName));
			}
			return Actor;
		}

		TArray<AActor*> Matches;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (IsValid(*It) && ClassMatches(It->GetClass(), ActorClass))
			{
				Matches.Add(*It);
			}
		}
		if (Matches.Num() == 1)
		{
			return Matches[0];
		}
		if (Matches.Num() == 0)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::ActorNotFound, FString::Printf(TEXT("No actor of class %s"), *ActorClass));
			return nullptr;
		}
		TArray<FString> Candidates;
		for (const AActor* Match : Matches)
		{
			Candidates.Add(Match->GetName());
		}
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::AmbiguousActor,
			FString::Printf(TEXT("%d actors match class %s; pass actor instead"), Matches.Num(), *ActorClass));
		OutError.AddContext(TEXT("candidates"), Candidates);
		return nullptr;
	}

	USkeletalMeshComponent* ResolveMesh(const AActor* Actor, const TSharedPtr<FJsonObject>& Params, FCortexCommandResult& OutError)
	{
		TArray<USkeletalMeshComponent*> Components;
		Actor->GetComponents(Components);
		TArray<FString> Names;
		TArray<USkeletalMeshComponent*> WithMesh;
		for (USkeletalMeshComponent* Component : Components)
		{
			Names.Add(Component->GetName());
			if (Component->GetSkeletalMeshAsset() != nullptr)
			{
				WithMesh.Add(Component);
			}
		}

		FString ComponentName;
		if (Params->TryGetStringField(TEXT("mesh_component"), ComponentName) && !ComponentName.IsEmpty())
		{
			USkeletalMeshComponent* const* Found = WithMesh.FindByPredicate([&ComponentName](const USkeletalMeshComponent* Component)
			{
				return Component->GetName() == ComponentName;
			});
			if (Found != nullptr)
			{
				return *Found;
			}
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::ComponentNotFound,
				FString::Printf(TEXT("%s has no skeletal mesh component '%s' with a mesh"), *Actor->GetName(), *ComponentName));
			OutError.AddContext(TEXT("candidates"), Names);
			return nullptr;
		}

		if (WithMesh.Num() == 1)
		{
			return WithMesh[0];
		}
		if (WithMesh.Num() == 0)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::ComponentNotFound,
				FString::Printf(TEXT("%s has no skeletal mesh component with a mesh"), *Actor->GetName()));
			return nullptr;
		}
		OutError = FCortexCommandRouter::Error(CortexErrorCodes::AmbiguousComponent,
			FString::Printf(TEXT("%s has %d skeletal mesh components; pass mesh_component"), *Actor->GetName(), WithMesh.Num()));
		OutError.AddContext(TEXT("candidates"), Names);
		return nullptr;
	}

	const TCHAR* SpaceName(const ECortexAnimPoseSpace Space)
	{
		switch (Space)
		{
		case ECortexAnimPoseSpace::Component:
			return TEXT("component");
		case ECortexAnimPoseSpace::DeltaFromRef:
			return TEXT("delta_from_ref");
		case ECortexAnimPoseSpace::Local:
		default:
			return TEXT("local");
		}
	}

	TSharedRef<FJsonObject> VectorJson(const FVector& Vector)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("x"), Vector.X);
		Object->SetNumberField(TEXT("y"), Vector.Y);
		Object->SetNumberField(TEXT("z"), Vector.Z);
		return Object;
	}

	TSharedRef<FJsonObject> RotatorJson(const FRotator& Rotator)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("pitch"), Rotator.Pitch);
		Object->SetNumberField(TEXT("yaw"), Rotator.Yaw);
		Object->SetNumberField(TEXT("roll"), Rotator.Roll);
		return Object;
	}

	TSharedRef<FJsonObject> BoneJson(const USkeletalMeshComponent& Mesh, const FName Bone, const ECortexAnimPoseSpace Space)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		if (Space == ECortexAnimPoseSpace::Component)
		{
			const FTransform Transform = Mesh.GetSocketTransform(Bone, RTS_Component);
			Entry->SetObjectField(TEXT("translation"), VectorJson(Transform.GetTranslation()));
			Entry->SetObjectField(TEXT("rotation"), RotatorJson(Transform.Rotator()));
			Entry->SetObjectField(TEXT("scale"), VectorJson(Transform.GetScale3D()));
			return Entry;
		}

		const FTransform Local = Mesh.GetSocketTransform(Bone, RTS_ParentBoneSpace);
		if (Space == ECortexAnimPoseSpace::Local)
		{
			Entry->SetObjectField(TEXT("translation"), VectorJson(Local.GetTranslation()));
			Entry->SetObjectField(TEXT("rotation"), RotatorJson(Local.Rotator()));
			Entry->SetObjectField(TEXT("scale"), VectorJson(Local.GetScale3D()));
			return Entry;
		}

		// Delta: Local = Delta * Reference in parent-bone space, on the shortest arc.
		const FTransform Reference = Mesh.GetRefPoseTransform(Mesh.GetBoneIndex(Bone));
		FQuat Delta = Local.GetRotation() * Reference.GetRotation().Inverse();
		Delta.Normalize();
		if (Delta.W < 0.0)
		{
			Delta = FQuat(-Delta.X, -Delta.Y, -Delta.Z, -Delta.W);
		}
		Entry->SetObjectField(TEXT("translation"), VectorJson(Local.GetTranslation() - Reference.GetTranslation()));
		Entry->SetObjectField(TEXT("rotation"), RotatorJson(Delta.Rotator()));
		Entry->SetObjectField(TEXT("scale"), VectorJson(Local.GetScale3D() - Reference.GetScale3D()));
		Entry->SetNumberField(TEXT("angle_degrees"), FMath::RadiansToDegrees(Delta.GetAngle()));
		return Entry;
	}
}

FCortexAnimPoseSampler::FCortexAnimPoseSampler(FCortexAnimPoseRequest InRequest, const double InStartRealSeconds)
	: Request(MoveTemp(InRequest))
	, StartRealSeconds(InStartRealSeconds)
	, Header(MakeShared<FJsonObject>())
{
	using namespace CortexAnimRuntimeOpsPrivate;

	const AActor* Actor = Request.Actor.Get();
	const USkeletalMeshComponent* Mesh = Request.Mesh.Get();
	const UWorld* World = Actor != nullptr ? Actor->GetWorld() : nullptr;
	const USkeletalMesh* Asset = Mesh != nullptr ? Mesh->GetSkeletalMeshAsset() : nullptr;
	const UAnimInstance* AnimInstance = Mesh != nullptr ? Mesh->GetAnimInstance() : nullptr;

	Header->SetStringField(TEXT("world"), World != nullptr ? World->GetName() : FString());
	Header->SetStringField(TEXT("actor"), Actor != nullptr ? Actor->GetName() : FString());
	Header->SetStringField(TEXT("actor_class"), Actor != nullptr ? Actor->GetClass()->GetPathName() : FString());
	Header->SetStringField(TEXT("mesh_component"), Mesh != nullptr ? Mesh->GetName() : FString());
	Header->SetStringField(TEXT("skeletal_mesh"), Asset != nullptr ? Asset->GetPathName() : FString());
	if (AnimInstance != nullptr)
	{
		Header->SetStringField(TEXT("anim_instance_class"), AnimInstance->GetClass()->GetPathName());
	}
	else
	{
		Header->SetField(TEXT("anim_instance_class"), MakeShared<FJsonValueNull>());
	}
	Header->SetStringField(TEXT("space"), SpaceName(Request.Space));
	Header->SetNumberField(TEXT("interval_seconds"), Request.IntervalSeconds);
	Header->SetNumberField(TEXT("sample_count"), Request.SampleCount);
}

TOptional<FCortexCommandResult> FCortexAnimPoseSampler::Step(UWorld& World, const double RealSeconds)
{
	using namespace CortexAnimRuntimeOpsPrivate;

	const USkeletalMeshComponent* Mesh = Request.Mesh.Get();
	if (Mesh == nullptr || Mesh->GetSkeletalMeshAsset() == nullptr)
	{
		return Finish(TEXT("target_destroyed"));
	}
	if (RealSeconds - StartRealSeconds > Request.TimeoutSeconds)
	{
		return Finish(TEXT("timeout"));
	}
	// Paused frames take no sample, but the real-time budget above still answers.
	if (World.IsPaused())
	{
		return NullOpt;
	}

	// Each sample waits a full interval after the previous one, so a hitch never bunches the next samples together.
	const double Now = World.GetTimeSeconds();
	if (Samples.Num() > 0 && Now + ScheduleToleranceSeconds < NextSampleGameTime)
	{
		return NullOpt;
	}
	NextSampleGameTime = Now + Request.IntervalSeconds;

	TakeSample(World, *Mesh);
	if (Samples.Num() >= Request.SampleCount)
	{
		return Finish(TEXT("completed"));
	}
	return NullOpt;
}

void FCortexAnimPoseSampler::TakeSample(UWorld& World, const USkeletalMeshComponent& Mesh)
{
	using namespace CortexAnimRuntimeOpsPrivate;

	TSharedRef<FJsonObject> Sample = MakeShared<FJsonObject>();
	Sample->SetNumberField(TEXT("index"), Samples.Num());
	Sample->SetNumberField(TEXT("game_time"), World.GetTimeSeconds());
	const AWorldSettings* Settings = World.GetWorldSettings();
	Sample->SetNumberField(TEXT("time_dilation"), Settings != nullptr ? Settings->GetEffectiveTimeDilation() : 1.0);
	Sample->SetBoolField(TEXT("recently_rendered"), Mesh.WasRecentlyRendered());

	TSharedRef<FJsonObject> Bones = MakeShared<FJsonObject>();
	for (const FName Bone : Request.Bones)
	{
		Bones->SetObjectField(Bone.ToString(), BoneJson(Mesh, Bone, Request.Space));
	}
	Sample->SetObjectField(TEXT("bones"), Bones);

	if (Request.Properties.Num() > 0)
	{
		// Resolved per sample: an anim instance replaced mid-run reports null for members its class lacks.
		const UAnimInstance* AnimInstance = Mesh.GetAnimInstance();
		TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
		for (const FName Name : Request.Properties)
		{
			const FProperty* Property = AnimInstance != nullptr ? FindFProperty<FProperty>(AnimInstance->GetClass(), Name) : nullptr;
			TSharedPtr<FJsonValue> Value = Property != nullptr
				? FJsonObjectConverter::UPropertyToJsonValue(const_cast<FProperty*>(Property), Property->ContainerPtrToValuePtr<void>(AnimInstance))
				: nullptr;
			Properties->SetField(Name.ToString(), Value.IsValid() ? Value : MakeShared<FJsonValueNull>());
		}
		Sample->SetObjectField(TEXT("properties"), Properties);
	}
	Samples.Add(MakeShared<FJsonValueObject>(Sample));
}

FCortexCommandResult FCortexAnimPoseSampler::Finish(const TCHAR* StopReason)
{
	const FString Reason(StopReason);
	Header->SetBoolField(TEXT("completed"), Reason == TEXT("completed"));
	Header->SetStringField(TEXT("stop_reason"), Reason);
	Header->SetArrayField(TEXT("samples"), Samples);
	return FCortexCommandRouter::Success(Header);
}

bool FCortexAnimRuntimeOps::ParseRequest(UWorld* World, const TSharedPtr<FJsonObject>& Params, FCortexAnimPoseRequest& OutRequest, FCortexCommandResult& OutError)
{
	using namespace CortexAnimRuntimeOpsPrivate;

	if (World == nullptr)
	{
		OutError = FCortexPIEUtils::PIENotActiveError();
		return false;
	}
	if (!Params.IsValid())
	{
		OutError = InvalidField(TEXT("Missing params"));
		return false;
	}

	AActor* Actor = ResolveActor(World, Params, OutError);
	if (Actor == nullptr)
	{
		return false;
	}
	USkeletalMeshComponent* Mesh = ResolveMesh(Actor, Params, OutError);
	if (Mesh == nullptr)
	{
		return false;
	}

	TArray<FString> Bones;
	if (!ReadStrings(Params, TEXT("bones"), true, Bones, OutError))
	{
		return false;
	}
	if (Bones.Num() == 0 || Bones.Num() > MaxBones)
	{
		OutError = InvalidField(FString::Printf(TEXT("bones must name 1-%d bones"), MaxBones));
		return false;
	}
	for (const FString& Bone : Bones)
	{
		if (Mesh->GetBoneIndex(FName(*Bone)) == INDEX_NONE)
		{
			OutError = InvalidField(FString::Printf(TEXT("Bone '%s' is not on %s"), *Bone, *Mesh->GetSkeletalMeshAsset()->GetName()));
			OutError.AddContext(TEXT("bone"), Bone);
			return false;
		}
		OutRequest.Bones.Add(FName(*Bone));
	}

	FString Space = TEXT("local");
	Params->TryGetStringField(TEXT("space"), Space);
	if (Space == TEXT("local"))
	{
		OutRequest.Space = ECortexAnimPoseSpace::Local;
	}
	else if (Space == TEXT("component"))
	{
		OutRequest.Space = ECortexAnimPoseSpace::Component;
	}
	else if (Space == TEXT("delta_from_ref"))
	{
		OutRequest.Space = ECortexAnimPoseSpace::DeltaFromRef;
	}
	else
	{
		OutError = InvalidField(FString::Printf(TEXT("space must be local, component or delta_from_ref, got '%s'"), *Space));
		return false;
	}

	double SampleCount = OutRequest.SampleCount;
	if (!ReadNumber(Params, TEXT("sample_count"), 1.0, MaxSamples, true, SampleCount, OutError)
		|| !ReadNumber(Params, TEXT("interval_seconds"), 0.0, MaxIntervalSeconds, false, OutRequest.IntervalSeconds, OutError)
		|| !ReadNumber(Params, TEXT("timeout_seconds"), MinTimeoutSeconds, MaxTimeoutSeconds, false, OutRequest.TimeoutSeconds, OutError))
	{
		return false;
	}
	OutRequest.SampleCount = static_cast<int32>(SampleCount);

	TArray<FString> Properties;
	if (!ReadStrings(Params, TEXT("properties"), false, Properties, OutError))
	{
		return false;
	}
	if (Properties.Num() > MaxProperties)
	{
		OutError = InvalidField(FString::Printf(TEXT("properties may name at most %d members"), MaxProperties));
		return false;
	}
	if (Properties.Num() > 0)
	{
		const UAnimInstance* AnimInstance = Mesh->GetAnimInstance();
		if (AnimInstance == nullptr)
		{
			OutError = FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation,
				FString::Printf(TEXT("%s has no anim instance to read properties from"), *Mesh->GetName()));
			return false;
		}
		for (const FString& Property : Properties)
		{
			if (FindFProperty<FProperty>(AnimInstance->GetClass(), FName(*Property)) == nullptr)
			{
				OutError = FCortexCommandRouter::Error(CortexErrorCodes::PropertyNotFound,
					FString::Printf(TEXT("%s has no property '%s'"), *AnimInstance->GetClass()->GetName(), *Property));
				OutError.AddContext(TEXT("property"), Property);
				return false;
			}
			OutRequest.Properties.Add(FName(*Property));
		}
	}

	OutRequest.Actor = Actor;
	OutRequest.Mesh = Mesh;
	return true;
}

FCortexCommandResult FCortexAnimRuntimeOps::SampleRuntimePose(const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback)
{
	UWorld* World = FCortexPIEUtils::GetPIEWorld();
	if (World == nullptr)
	{
		return FCortexPIEUtils::PIENotActiveError();
	}
	if (!DeferredCallback)
	{
		return FCortexCommandRouter::Error(CortexErrorCodes::InvalidOperation, TEXT("sample_runtime_pose answers deferred and needs a deferred callback"));
	}

	FCortexAnimPoseRequest Request;
	FCortexCommandResult Error;
	if (!ParseRequest(World, Params, Request, Error))
	{
		return Error;
	}

	const double TimeoutSeconds = Request.TimeoutSeconds;
	TSharedRef<FCortexAnimPoseSampler> Sampler = MakeShared<FCortexAnimPoseSampler>(MoveTemp(Request), FPlatformTime::Seconds());
	return FCortexPIEUtils::StartDeferredPIEPoll(World, 0.0f, TimeoutSeconds, TEXT("sample_runtime_pose"), MoveTemp(DeferredCallback),
		[Sampler](UWorld& PIEWorld) -> TOptional<FCortexCommandResult>
		{
			return Sampler->Step(PIEWorld, FPlatformTime::Seconds());
		}, /*bPollWhilePaused=*/true);
}
