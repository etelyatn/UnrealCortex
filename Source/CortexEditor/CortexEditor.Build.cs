using UnrealBuildTool;

public class CortexEditor : ModuleRules
{
	public CortexEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"CortexCore",
			"InputCore",
			"SlateCore",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ApplicationCore",
			"Engine",
			"Json",
			"JsonUtilities",
			"UnrealEd",
			"LevelEditor",
			"Slate",
			"EnhancedInput",
			"ImageWrapper",
			"RenderCore",
			"PythonScriptPlugin",
		});
	}
}
