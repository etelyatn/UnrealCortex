using UnrealBuildTool;

public class CortexGen : ModuleRules
{
    public CortexGen(ReadOnlyTargetRules Target) : base(Target)
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
            "AssetTools",
            "HTTP",
            "Json",
            "JsonUtilities",
            "DeveloperSettings",
        });
    }
}
