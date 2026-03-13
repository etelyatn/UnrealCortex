using UnrealBuildTool;

public class CortexFrontend : ModuleRules
{
    public CortexFrontend(ReadOnlyTargetRules Target) : base(Target)
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
            "WorkspaceMenuStructure",
            "ApplicationCore",
            "Slate",
            "SlateCore",
            "InputCore",
            "Json",
            "JsonUtilities",
            "ToolMenus",
            "Projects",
        });
    }
}
