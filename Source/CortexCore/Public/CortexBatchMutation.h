#pragma once

#include "CoreMinimal.h"
#include "CortexTypes.h"

class CORTEXCORE_API FCortexBatchMutation
{
public:
	using FPreflightCallback = TFunction<FCortexBatchPreflightResult(const FCortexBatchMutationItem&)>;
	using FCommitCallback = TFunction<FCortexCommandResult(const FCortexBatchMutationItem&)>;

	static bool ParseRequest(
		const TSharedPtr<FJsonObject>& Params,
		const FString& SingleTargetField,
		FCortexBatchMutationRequest& OutRequest,
		FCortexCommandResult& OutError);

	static FCortexBatchMutationResult Run(
		const FCortexBatchMutationRequest& Request,
		FPreflightCallback Preflight,
		FCommitCallback Commit);

	static bool FingerprintsMatch(
		const TSharedPtr<FJsonObject>& CurrentFingerprint,
		const TSharedPtr<FJsonObject>& ExpectedFingerprint);
};
