// Plugins/UnrealCortex/Source/CortexFrontend/Private/Utilities/CortexTokenUtils.h
#pragma once

#include "CoreMinimal.h"
#include "CortexConversionTypes.h"

namespace CortexTokenUtils
{
	/** Format token count as "~Nk tokens" or "~N tokens" */
	inline FString FormatTokenCount(int32 Tokens)
	{
		if (Tokens <= 0)
		{
			return FString();
		}
		if (Tokens >= 1000)
		{
			const float K = static_cast<float>(Tokens) / 1000.0f;
			if (K >= 10.0f || FMath::IsNearlyEqual(K, FMath::RoundToFloat(K)))
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

	/** Estimate token count from JSON character length (~4 chars per token for ASCII) */
	inline int32 EstimateTokensFromJson(int32 JsonCharLen)
	{
		return JsonCharLen / 4;
	}

	/** Estimate processing time in seconds for a given token count.
	 *  Formula: connection overhead (10s) + base rate (10s per 1K tokens) + gap buffer.
	 *  Returns 0 for 0 tokens. */
	inline float EstimateSecondsForTokens(int32 Tokens)
	{
		if (Tokens <= 0)
		{
			return 0.0f;
		}
		static constexpr float ConnectionOverheadSeconds = 10.0f;
		float GapBuffer = 0.0f;
		if (Tokens > 20000)
		{
			GapBuffer = 30.0f;
		}
		else if (Tokens > 5000)
		{
			GapBuffer = 15.0f;
		}
		return ConnectionOverheadSeconds + (Tokens / 1000.0f) * 10.0f + GapBuffer;
	}

	/** Format token estimate with ETA: "~12k tokens · est. ~2m 10s" */
	inline FString FormatTokenEstimate(int32 Tokens)
	{
		if (Tokens <= 0)
		{
			return FString();
		}
		const int32 EstSec = FMath::RoundToInt(EstimateSecondsForTokens(Tokens));
		const FString TokenStr = FormatTokenCount(Tokens);

		if (EstSec >= 60)
		{
			return FString::Printf(TEXT("%s \u00B7 est. ~%dm %ds"), *TokenStr, EstSec / 60, EstSec % 60);
		}
		return FString::Printf(TEXT("%s \u00B7 est. ~%ds"), *TokenStr, EstSec);
	}

	/** Estimate tokens for a given scope.
	 *  Unified formula used by both conversion and analysis config screens.
	 *  @param Scope             Selected conversion scope
	 *  @param TotalTokens       Token estimate for EntireBlueprint
	 *  @param NumGraphs         Number of graphs in the Blueprint
	 *  @param TotalNodeCount    Total node count across all graphs
	 *  @param SelectedNodeCount Number of selected nodes (for SelectedNodes scope)
	 *  @param SelectedFunctions List of selected function names (for EventOrFunction scope)
	 *  @param PerFunctionTokens Per-function token estimates (may be empty if not yet computed)
	 *  @param NumFunctions      Total number of events+functions in the Blueprint */
	inline int32 EstimateTokensForScope(
		ECortexConversionScope Scope,
		int32 TotalTokens,
		int32 NumGraphs,
		int32 TotalNodeCount,
		int32 SelectedNodeCount,
		const TArray<FString>& SelectedFunctions,
		const TMap<FString, int32>& PerFunctionTokens,
		int32 NumFunctions = 0)
	{
		if (TotalTokens <= 0)
		{
			return 0;
		}

		NumGraphs = FMath::Max(1, NumGraphs);

		switch (Scope)
		{
		case ECortexConversionScope::EntireBlueprint:
			return TotalTokens;

		case ECortexConversionScope::SelectedNodes:
			// Proportional to actual node selection ratio; fall back to minimum if no node data
			if (TotalNodeCount <= 0)
			{
				return 500;
			}
			return FMath::Max(500, static_cast<int32>(static_cast<int64>(TotalTokens) * SelectedNodeCount / FMath::Max(1, TotalNodeCount)));

		case ECortexConversionScope::CurrentGraph:
			return TotalTokens / NumGraphs;

		case ECortexConversionScope::EventOrFunction:
		{
			// Sum per-function estimates for selected functions if available
			if (SelectedFunctions.Num() > 0 && PerFunctionTokens.Num() > 0)
			{
				int32 Sum = 0;
				for (const FString& Func : SelectedFunctions)
				{
					if (const int32* Found = PerFunctionTokens.Find(Func))
					{
						Sum += *Found;
					}
				}
				if (Sum > 0)
				{
					return Sum;
				}
			}
			// Fallback: proportional estimate
			NumFunctions = FMath::Max(1, NumFunctions);
			const int32 Selected = FMath::Max(1, SelectedFunctions.Num());
			return static_cast<int32>(static_cast<int64>(TotalTokens) * Selected / NumFunctions);
		}

		default:
			return TotalTokens;
		}
	}
}
