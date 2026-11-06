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
            "Settings",
            "Projects",
            "CortexCore",
            "CortexGen",
            "ImageWrapper",
            "EditorScriptingUtilities",
            "DesktopPlatform",
            "GraphEditor",
            "BlueprintGraph",
            "AssetRegistry",
            "MessageLog",
            "Kismet",
            "ContentBrowser",
        });

        // LiveCoding lives in Developer/Windows and only exists where Live Coding is
        // supported, so an unconditional dependency pulls a Windows-only module into
        // Linux and Mac editor builds. Gate it the way engine modules do.
        if (Target.bWithLiveCoding)
        {
            PrivateDependencyModuleNames.Add("LiveCoding");
        }
    }
}
