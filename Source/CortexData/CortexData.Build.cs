using UnrealBuildTool;

public class CortexData : ModuleRules
{
    public CortexData(ReadOnlyTargetRules Target) : base(Target)
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
            "GameplayTags",
            "AssetRegistry",
            "UnrealEd",
        });
    }
}
