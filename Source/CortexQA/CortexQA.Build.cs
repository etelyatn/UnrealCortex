using UnrealBuildTool;

public class CortexQA : ModuleRules
{
    public CortexQA(ReadOnlyTargetRules Target) : base(Target)
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
            "NavigationSystem",
            "AIModule",
            "GameplayTags",
            "InputCore",
            "CortexEditor",
        });
    }
}
