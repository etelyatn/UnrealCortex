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
			"PythonScriptPlugin",
			"AssetRegistry",
			"Kismet",
		});

		// LiveCoding lives in Developer/Windows and only exists where Live Coding is supported, so an
		// unconditional dependency would pull a Windows-only module into Linux and Mac editor builds.
		if (Target.bWithLiveCoding)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
		}
	}
}
