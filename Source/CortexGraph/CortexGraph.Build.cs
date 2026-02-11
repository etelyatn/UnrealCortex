using UnrealBuildTool;

public class CortexGraph : ModuleRules
{
	public CortexGraph(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CortexCore",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"Json",
			"JsonUtilities",
			"UnrealEd",
			"BlueprintGraph",
			"KismetCompiler",
		});
	}
}
