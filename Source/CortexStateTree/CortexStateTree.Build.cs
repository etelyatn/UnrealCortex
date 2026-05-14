using UnrealBuildTool;

public class CortexStateTree : ModuleRules
{
	public CortexStateTree(ReadOnlyTargetRules Target) : base(Target)
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
			"AssetRegistry",
			"AssetTools",
			"UnrealEd",
			"GameplayTags",
			"GameplayStateTreeModule",
			"StateTreeModule",
			"StateTreeEditorModule",
		});
	}
}
