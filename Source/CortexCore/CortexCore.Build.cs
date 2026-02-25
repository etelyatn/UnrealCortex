using UnrealBuildTool;

public class CortexCore : ModuleRules
{
    public CortexCore(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "CoreUObject",
            "Engine",
            "DeveloperSettings",
            "Sockets",
            "Networking",
            "Json",
            "JsonUtilities",
            "GameplayTags",
            "UnrealEd",
            "AssetRegistry",
            // Test-only: required for CortexSerializerInstancedSubObjectTest.cpp (InputMappingContext, InputModifiers)
            // Not used in production CortexCore code.
            "EnhancedInput",
            "InputCore",
        });
    }
}
