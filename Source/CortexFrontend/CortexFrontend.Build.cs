using UnrealBuildTool;

public class CortexFrontend : ModuleRules
{
    public CortexFrontend(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "Engine",
            "DeveloperSettings",
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
            "CortexCore",
            "CortexGen",
            "ImageWrapper",
            "EditorScriptingUtilities",
            "DesktopPlatform",
            "GraphEditor",
            "BlueprintGraph",
            "LiveCoding",
            "AssetRegistry",
            "MessageLog",
            "Kismet",
            "ContentBrowser",
        });
    }
}
