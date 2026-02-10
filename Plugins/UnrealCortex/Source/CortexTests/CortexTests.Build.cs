using UnrealBuildTool;

public class CortexTests : ModuleRules
{
    public CortexTests(ReadOnlyTargetRules Target) : base(Target)
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
            "GameplayTags",
            "StructUtils",
            "UnrealEd",
        });

        // Access CortexData Private headers for test setup (domain handler registration)
        PrivateIncludePaths.Add(System.IO.Path.Combine(ModuleDirectory, "..", "CortexData", "Private"));
    }
}
