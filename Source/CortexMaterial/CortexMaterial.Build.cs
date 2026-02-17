using UnrealBuildTool;

public class CortexMaterial : ModuleRules
{
    public CortexMaterial(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CortexCore",
            "CortexGraph",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "CoreUObject",
            "Engine",
            "Json",
            "JsonUtilities",
            "UnrealEd",
            "MaterialEditor",
            "AssetRegistry",
        });
    }
}
