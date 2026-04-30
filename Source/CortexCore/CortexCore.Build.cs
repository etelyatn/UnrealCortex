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
            // Shared asset registry access is used by asset operations and fingerprinting.
            "AssetRegistry",
            // Test-only: required for CortexSerializerInstancedSubObjectTest.cpp (InputMappingContext, InputModifiers)
            // Not used in production CortexCore code.
            "EnhancedInput",
            "InputCore",
            "StructUtils",
        });

        // StructUtils was deprecated in 5.5 — FInstancedStruct moved to CoreUObject
        if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion < 5)
        {
            PrivateDependencyModuleNames.Add("StructUtils");
        }
    }
}
