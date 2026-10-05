using UnrealBuildTool;

public class CortexReplay : ModuleRules
{
	public CortexReplay(ReadOnlyTargetRules Target) : base(Target)
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
			"EditorStyle",
			"InputCore",
			"Json",
			"JsonUtilities",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"UnrealEd",
			"CortexEditor",
		});
	}
}
