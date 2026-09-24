#include "Operations/CortexGraphPatchState.h"
#include "CortexGraphFingerprint.h"

TSharedPtr<FJsonObject> FCortexGraphPatchState::ComputeFingerprint(UBlueprint* Blueprint)
{
	return FCortexGraphFingerprint::Compute(Blueprint);
}

FString FCortexGraphPatchState::ComputeGeneratedStateDigest(UBlueprint* Blueprint)
{
	return FCortexGraphFingerprint::ComputeGeneratedStateDigest(Blueprint);
}

bool FCortexGraphPatchState::ValidatePrecondition(
	const TSharedPtr<FJsonObject>& Expected,
	const TSharedPtr<FJsonObject>& Current,
	FCortexCommandResult& OutError)
{
	return FCortexGraphFingerprint::ValidatePrecondition(Expected, Current, OutError);
}
