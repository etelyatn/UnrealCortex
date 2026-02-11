using UnrealBuildTool;

public class CortexBlueprintTests : ModuleRules
{
	public CortexBlueprintTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"CortexCore",
			"CortexBlueprint",
			"UnrealEd",
			"AssetRegistry",
			"Kismet",
			"KismetCompiler",
			"BlueprintGraph",
		});

		// Access CortexBlueprint Private headers for test setup (command handler)
		PrivateIncludePaths.Add(System.IO.Path.Combine(ModuleDirectory, "..", "CortexBlueprint", "Private"));
	}
}
