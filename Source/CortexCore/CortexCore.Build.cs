using UnrealBuildTool;

public class CortexCore : ModuleRules
{
    public CortexCore(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "Json",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "CoreUObject",
            "Engine",
            "DeveloperSettings",
            "Projects",
            "Sockets",
            "Networking",
            "JsonUtilities",
            "GameplayTags",
            "UnrealEd",
            // Shared asset registry access is used by asset operations and fingerprinting.
            "AssetRegistry",
            // Test-only: required for CortexSerializerInstancedSubObjectTest.cpp (InputMappingContext, InputModifiers)
            // Not used in production CortexCore code.
            "EnhancedInput",
            "InputCore",
        });

        // StructUtils was deprecated in 5.5 — FInstancedStruct moved to CoreUObject
        if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion < 5)
        {
            PrivateDependencyModuleNames.Add("StructUtils");
        }

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicSystemLibraries.Add("bcrypt.lib");
        }

        // SHA-256 of file bytes (FCortexSafeFileContract::HashFileBytesSha256). Windows uses BCrypt
        // above; Unix uses the engine's own OpenSSL module, the same way S3Client and the DDC S3 store
        // do. Any other platform keeps the explicit "not implemented" error.
        bool bUseOpenSslSha256 = Target.IsInPlatformGroup(UnrealPlatformGroup.Unix);
        if (bUseOpenSslSha256)
        {
            AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
        }
        PrivateDefinitions.Add("CORTEX_SHA256_OPENSSL=" + (bUseOpenSslSha256 ? "1" : "0"));
    }
}
