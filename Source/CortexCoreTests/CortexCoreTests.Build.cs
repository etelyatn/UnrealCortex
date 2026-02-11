using UnrealBuildTool;

public class CortexCoreTests : ModuleRules
{
    public CortexCoreTests(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "CortexCore",
            "Sockets",
            "Networking",
            "Json",
            "JsonUtilities",
            "GameplayTags",
        });
    }
}
