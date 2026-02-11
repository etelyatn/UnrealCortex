using UnrealBuildTool;

public class CortexGraphTests : ModuleRules
{
	public CortexGraphTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"CortexCore",
			"CortexGraph",
			"Json",
			"JsonUtilities",
			"UnrealEd",
			"BlueprintGraph",
			"KismetCompiler",
		});

		// Access CortexGraph Private headers for test setup (command handler)
		PrivateIncludePaths.Add(System.IO.Path.Combine(ModuleDirectory, "..", "CortexGraph", "Private"));
	}
}
