#pragma once

#include "CoreMinimal.h"
#include "CortexEditorUtils.h"

#if WITH_EDITOR && WITH_AUTOMATION_TESTS

/**
 * Test-only authorization for the transient `/Temp` mount.
 *
 * The patch route applies the same shared writable-content-root policy as every other graph
 * mutator (`FCortexEditorUtils::IsWritableMountedContentRoots`), and `/Temp` is not a project
 * content root. The patch fixtures are intentionally transient in-memory packages under `/Temp`,
 * so they register that one root through the sanctioned test hook instead of the production policy
 * growing a test-only exception or a second allowlist.
 *
 * Registration is idempotent (`AddTestWritableContentRoot` de-duplicates) and lives for the
 * automation process only: it is called from test request builders, which never run outside a test.
 */
inline void EnsureCortexGraphTestTempContentRoot()
{
	static const bool bRegistered = []()
	{
		FCortexEditorUtils::AddTestWritableContentRoot(TEXT("/Temp"));
		return true;
	}();
	(void)bRegistered;
}

#endif // WITH_EDITOR && WITH_AUTOMATION_TESTS
