using UnrealBuildTool;

public class CortexDataTests : ModuleRules
{
    public CortexDataTests(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "CortexCore",
            "CortexData",
            "Sockets",
            "Networking",
            "Json",
            "JsonUtilities",
            "UnrealEd",
        });

        // Access CortexData Private headers for test setup (domain handler registration)
        PrivateIncludePaths.Add(System.IO.Path.Combine(ModuleDirectory, "..", "CortexData", "Private"));
    }
}
