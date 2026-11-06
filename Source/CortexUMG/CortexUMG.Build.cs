using UnrealBuildTool;

public class CortexUMG : ModuleRules
{
    public CortexUMG(ReadOnlyTargetRules Target) : base(Target)
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
            "AssetRegistry",
            "Engine",
            "Json",
            "JsonUtilities",
            "UnrealEd",
            "BlueprintGraph",
            "UMG",
            "UMGEditor",
            "Slate",
            "SlateCore",
            "MovieScene",
            "MovieSceneTracks",
        });
        // Canonical serialized binding guards use the engine-adopted SHA-256 implementation.
        AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
    }
}
