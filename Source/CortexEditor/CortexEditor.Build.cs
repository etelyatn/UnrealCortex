using UnrealBuildTool;

public class CortexEditor : ModuleRules
{
	public CortexEditor(ReadOnlyTargetRules Target) : base(Target)
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
			"LevelEditor",
			"Slate",
			"SlateCore",
			"InputCore",
			"EnhancedInput",
			"ImageWrapper",
			"RenderCore",
		});
	}
}
