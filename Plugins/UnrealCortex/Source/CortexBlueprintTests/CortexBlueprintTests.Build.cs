using UnrealBuildTool;

public class CortexBlueprintTests : ModuleRules
{
	public CortexBlueprintTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"CortexCore",
			"CortexBlueprint",
		});

		// Access CortexBlueprint Private headers for test setup (command handler)
		PrivateIncludePaths.Add(System.IO.Path.Combine(ModuleDirectory, "..", "CortexBlueprint", "Private"));
	}
}
