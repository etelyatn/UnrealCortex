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

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"Json",
			"JsonUtilities",
			"UnrealEd",
			"AssetRegistry",
			"AnimGraph",
			"BlueprintGraph",
			"Kismet",
		});
	}
}
