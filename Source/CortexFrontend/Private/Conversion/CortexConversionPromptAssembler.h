#pragma once

#include "CoreMinimal.h"
#include "CortexConversionTypes.h"

struct FCortexConversionContext;

class FCortexConversionPromptAssembler
{
public:
	/** Assemble the full system prompt from layered components + domain fragments. */
	static FString Assemble(const FCortexConversionContext& Context, const FString& BlueprintJson);

	/** Select which knowledge fragment files to include based on Blueprint JSON content. */
	static TArray<FString> SelectFragments(const FString& BlueprintJson);

	/** Load a single fragment file from Resources/ConversionKnowledge/. */
	static FString LoadFragment(const FString& Filename);

	/** Determine whether to use Snippet (true) or FullClass (false) scope layer. */
	static bool ShouldUseSnippetMode(ECortexConversionScope Scope, ECortexConversionDepth Depth);
};
