using UnrealBuildTool;

public class CortexReflect : ModuleRules
{
	public CortexReflect(ReadOnlyTargetRules Target) : base(Target)
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
			"UnrealEd",
			"AssetRegistry",
			"Json",
			"BlueprintGraph",
			"Kismet",
		});
	}
}
