using UnrealBuildTool;

public class CortexAnimation : ModuleRules
{
	public CortexAnimation(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CortexCore",
		});

		// The test-only alternative binding storage (Private/Tests/CortexAnimationTestAltBinding.h) derives from the engine's abstract
		// UAnimGraphNodeBinding, which lives in AnimGraph's Internal folder.
		PrivateIncludePaths.Add(System.IO.Path.Combine(EngineDirectory, "Source", "Editor", "AnimGraph", "Internal"));

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CortexGraph",
			"CoreUObject",
			"Engine",
			"Json",
			"JsonUtilities",
			"UnrealEd",
			"AssetRegistry",
			"AnimGraph",
			"BlueprintGraph",
			"Kismet",
			"SlateCore",
		});
	}
}
