#pragma once

#include "CoreMinimal.h"

struct CORTEXCORE_API FCortexFileUtils
{
	static bool AtomicWriteFile(const FString& FilePath, const FString& Content);
};
