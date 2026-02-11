using UnrealBuildTool;

public class CortexBlueprint : ModuleRules
{
	public CortexBlueprint(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CortexCore",
			"CortexGraph",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"AssetRegistry",
			"Json",
			"Kismet",
			"KismetCompiler",
			"BlueprintGraph",
		});
	}
}
