#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"
#include "UObject/WeakObjectPtr.h"

class AActor;
class FJsonObject;
class FJsonValue;
class USkeletalMeshComponent;
class UWorld;

/** The space anim.sample_runtime_pose reports bone transforms in. */
enum class ECortexAnimPoseSpace : uint8
{
	/** Relative to the parent bone (RTS_ParentBoneSpace). */
	Local,
	/** Relative to the mesh component (RTS_Component). */
	Component,
	/** The local transform relative to the bone's local reference pose. */
	DeltaFromRef,
};

/** A validated anim.sample_runtime_pose request against one live skeletal mesh. */
struct FCortexAnimPoseRequest
{
	TWeakObjectPtr<AActor> Actor;
	TWeakObjectPtr<USkeletalMeshComponent> Mesh;
	TArray<FName> Bones;
	TArray<FName> Properties;
	ECortexAnimPoseSpace Space = ECortexAnimPoseSpace::Local;
	int32 SampleCount = 10;
	/** Game seconds between samples; 0 samples every frame. */
	double IntervalSeconds = 0.1;
	/** Real seconds the whole run may take. */
	double TimeoutSeconds = 60.0;
};

/**
 * Samples a request over game time. The owner calls Step once per frame (the shared PIE poll, or a test driving a transient
 * world); Step returns the final result once every sample is taken, the real-time budget is spent or the mesh is gone.
 */
class FCortexAnimPoseSampler
{
public:
	FCortexAnimPoseSampler(FCortexAnimPoseRequest InRequest, double InStartRealSeconds);

	TOptional<FCortexCommandResult> Step(UWorld& World, double RealSeconds);

private:
	void TakeSample(UWorld& World, const USkeletalMeshComponent& Mesh);
	FCortexCommandResult Finish(const TCHAR* StopReason);

	FCortexAnimPoseRequest Request;
	double StartRealSeconds = 0.0;
	double FirstSampleGameTime = 0.0;
	TArray<TSharedPtr<FJsonValue>> Samples;
	/** Identity captured at validation, so a stop after the target is destroyed still names it. */
	TSharedPtr<FJsonObject> Header;
};

class FCortexAnimRuntimeOps
{
public:
	/** Validates Params against World (the PIE world) and resolves the actor, mesh, bones and anim-instance properties. */
	static bool ParseRequest(UWorld* World, const TSharedPtr<FJsonObject>& Params, FCortexAnimPoseRequest& OutRequest, FCortexCommandResult& OutError);

	/** anim.sample_runtime_pose: validates synchronously, then answers deferred through FCortexPIEUtils::StartDeferredPIEPoll. */
	static FCortexCommandResult SampleRuntimePose(const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback DeferredCallback);
};
