using UnrealBuildTool;

public class CortexLevel : ModuleRules
{
    public CortexLevel(ReadOnlyTargetRules Target) : base(Target)
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
            "CortexEditor",
            "UnrealEd",
            "LevelEditor",
            "DataLayerEditor",
            "SourceControl",
        });
    }
}
