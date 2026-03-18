// Plugins/UnrealCortex/Source/CortexFrontend/Private/Utilities/CortexTokenUtils.h
#pragma once

#include "CoreMinimal.h"

namespace CortexTokenUtils
{
	/** Format token count as "~Nk tokens" or "~N tokens" */
	inline FString FormatTokenCount(int32 Tokens)
	{
		if (Tokens >= 1000)
		{
			const float K = static_cast<float>(Tokens) / 1000.0f;
			if (K >= 10.0f)
			{
				return FString::Printf(TEXT("~%dk tokens"), FMath::RoundToInt(K));
			}
			return FString::Printf(TEXT("~%.1fk tokens"), K);
		}
		return FString::Printf(TEXT("~%d tokens"), Tokens);
	}

	/** Token limit constants */
	inline constexpr int32 SoftTokenLimit = 40000;
	inline constexpr int32 HardTokenLimit = 80000;
}
