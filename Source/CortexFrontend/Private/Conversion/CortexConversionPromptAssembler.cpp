#include "Conversion/CortexConversionPromptAssembler.h"

#include "CortexFrontendModule.h"
#include "Conversion/CortexConversionContext.h"
#include "Conversion/CortexConversionPrompts.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

FString FCortexConversionPromptAssembler::Assemble(
	const FCortexConversionContext& Context, const FString& BlueprintJson)
{
	FString Result;

	// 1. Base system prompt
	Result += CortexConversionPrompts::BaseSystemPrompt();
	Result += TEXT("\n\n");

	// 2. Scope layer
	if (ShouldUseSnippetMode(Context.SelectedScope, Context.SelectedDepth))
	{
		Result += CortexConversionPrompts::ScopeLayerSnippet();
	}
	else
	{
		Result += CortexConversionPrompts::ScopeLayerFullClass();
	}
	Result += TEXT("\n\n");

	// 3. Depth layer
	switch (Context.SelectedDepth)
	{
	case ECortexConversionDepth::PerformanceShell:
		Result += CortexConversionPrompts::DepthLayerPerformanceShell();
		break;
	case ECortexConversionDepth::CppCore:
		Result += CortexConversionPrompts::DepthLayerCppCore();
		break;
	case ECortexConversionDepth::FullExtraction:
		Result += CortexConversionPrompts::DepthLayerFullExtraction();
		break;
	case ECortexConversionDepth::Custom:
		if (!Context.CustomInstructions.IsEmpty())
		{
			Result += FString::Printf(TEXT("Conversion Instructions: CUSTOM\n\n%s"), *Context.CustomInstructions);
		}
		else
		{
			Result += CortexConversionPrompts::DepthLayerCppCore();
		}
		break;
	}
	Result += TEXT("\n\n");

	// 4. Inject mode layer (conditional)
	if (Context.SelectedDestination == ECortexConversionDestination::InjectIntoExisting)
	{
		Result += CortexConversionPrompts::InjectModeLayer();
		Result += TEXT("\n\n");

		// Include existing source files in context
		if (!Context.TargetHeaderPath.IsEmpty())
		{
			FString HeaderContent;
			if (FFileHelper::LoadFileToString(HeaderContent, *Context.TargetHeaderPath))
			{
				Result += FString::Printf(TEXT("<existing-header path=\"%s\">\n%s\n</existing-header>\n\n"),
					*Context.TargetHeaderPath, *HeaderContent);
			}
		}
		if (!Context.TargetSourcePath.IsEmpty())
		{
			FString SourceContent;
			if (FFileHelper::LoadFileToString(SourceContent, *Context.TargetSourcePath))
			{
				Result += FString::Printf(TEXT("<existing-implementation path=\"%s\">\n%s\n</existing-implementation>\n\n"),
					*Context.TargetSourcePath, *SourceContent);
			}
		}
	}

	// 5. Domain knowledge fragments
	TArray<FString> Fragments = SelectFragments(BlueprintJson);
	for (const FString& FragmentFile : Fragments)
	{
		FString FragmentContent = LoadFragment(FragmentFile);
		if (!FragmentContent.IsEmpty())
		{
			Result += FString::Printf(TEXT("<domain-knowledge source=\"%s\">\n%s\n</domain-knowledge>\n\n"),
				*FragmentFile, *FragmentContent);
		}
	}

	return Result;
}

TArray<FString> FCortexConversionPromptAssembler::SelectFragments(const FString& BlueprintJson)
{
	TArray<FString> Result;

	// Always include core patterns
	Result.Add(TEXT("core-patterns.md"));

	// Delegates/events
	if (BlueprintJson.Contains(TEXT("K2Node_CreateDelegate"))
		|| BlueprintJson.Contains(TEXT("MulticastInlineDelegate"))
		|| BlueprintJson.Contains(TEXT("MulticastDelegate"))
		|| BlueprintJson.Contains(TEXT("EventDispatcher")))
	{
		Result.Add(TEXT("delegates-events.md"));
	}

	// Replication
	if (BlueprintJson.Contains(TEXT("Replicated"))
		|| BlueprintJson.Contains(TEXT("RepNotify"))
		|| BlueprintJson.Contains(TEXT("\"Server\""))
		|| BlueprintJson.Contains(TEXT("\"Client\""))
		|| BlueprintJson.Contains(TEXT("NetMulticast")))
	{
		Result.Add(TEXT("replication.md"));
	}

	// Latent nodes
	if (BlueprintJson.Contains(TEXT("K2Node_Timeline"))
		|| BlueprintJson.Contains(TEXT("K2Node_Delay"))
		|| BlueprintJson.Contains(TEXT("AsyncLoad"))
		|| BlueprintJson.Contains(TEXT("K2Node_LatentGameplayTaskCall")))
	{
		Result.Add(TEXT("latent-nodes.md"));
	}

	// Blueprint flow nodes
	if (BlueprintJson.Contains(TEXT("FlipFlop"))
		|| BlueprintJson.Contains(TEXT("DoOnce"))
		|| BlueprintJson.Contains(TEXT("Gate"))
		|| BlueprintJson.Contains(TEXT("MultiGate"))
		|| BlueprintJson.Contains(TEXT("ForEachLoopWithBreak")))
	{
		Result.Add(TEXT("bp-flow-nodes.md"));
	}

	// Animation
	if (BlueprintJson.Contains(TEXT("AnimInstance"))
		|| BlueprintJson.Contains(TEXT("AnimBlueprint"))
		|| BlueprintJson.Contains(TEXT("AnimGraph")))
	{
		Result.Add(TEXT("animation.md"));
	}

	// UMG
	if (BlueprintJson.Contains(TEXT("UserWidget"))
		|| BlueprintJson.Contains(TEXT("WidgetBlueprint"))
		|| BlueprintJson.Contains(TEXT("UMG")))
	{
		Result.Add(TEXT("umg-patterns.md"));
	}

	return Result;
}

FString FCortexConversionPromptAssembler::LoadFragment(const FString& Filename)
{
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealCortex"));
	if (!Plugin.IsValid())
	{
		UE_LOG(LogCortexFrontend, Error, TEXT("UnrealCortex plugin not found for fragment loading"));
		return FString();
	}

	FString BasePath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("ConversionKnowledge"));
	FString FilePath = FPaths::Combine(BasePath, Filename);

	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *FilePath))
	{
		UE_LOG(LogCortexFrontend, Warning, TEXT("Failed to load conversion knowledge fragment: %s"), *FilePath);
		return FString();
	}

	return Content;
}

bool FCortexConversionPromptAssembler::ShouldUseSnippetMode(
	ECortexConversionScope Scope, ECortexConversionDepth Depth)
{
	// Snippet mode only for PerformanceShell with a narrow scope —
	// extracting one hot function as a code fragment.
	// All other depths (CppCore, FullExtraction, Custom) always produce
	// a complete class with header + implementation (full-class mode).
	return Depth == ECortexConversionDepth::PerformanceShell
		&& Scope != ECortexConversionScope::EntireBlueprint;
}
