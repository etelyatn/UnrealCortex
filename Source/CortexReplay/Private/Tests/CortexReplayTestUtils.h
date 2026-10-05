#pragma once

#include "CoreMinimal.h"
#include "CortexReplayTypes.h"

/**
 * Test-only fixture for CortexReplay library automation tests.
 *
 * Owns a unique Saved directory per instance so tests never touch the real shared
 * library. MakeRecording supplies a valid default initial pose/class/schema and
 * prerequisites, fills missing guards for press rows with that pose/not_applicable,
 * computes coverage and a duration covering the final event time. It must not repair
 * explicitly malformed data supplied by negative-case tests.
 */
class FCortexReplayTestFixture
{
public:
	FCortexReplayTestFixture();
	~FCortexReplayTestFixture();

	const FString& GetProjectRoot() const;
	FCortexReplaySnapshot MakeRecording(int32 Id, bool bAIEnabled, const TArray<FCortexReplayEvent>& Events);
	FString GetInputsPath(int32 Id) const;
	FString GetInitialStatePath(int32 Id) const;

private:
	FString ProjectRoot;
};
