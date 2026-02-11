using UnrealBuildTool;

public class CortexUMGTests : ModuleRules
{
    public CortexUMGTests(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "CortexCore",
            "CortexUMG",
            "Json",
            "JsonUtilities",
            "UnrealEd",
            "UMG",
            "UMGEditor",
            "Slate",
            "SlateCore",
        });

        // Access CortexUMG Private headers for test setup (command handler)
        PrivateIncludePaths.Add(System.IO.Path.Combine(ModuleDirectory, "..", "CortexUMG", "Private"));
    }
}
