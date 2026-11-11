using UnrealBuildTool;

public class CortexEditor : ModuleRules
{
	public CortexEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"CortexCore",
			"InputCore",
			"SlateCore",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ApplicationCore",
			"Json",
			"JsonUtilities",
			"UnrealEd",
			"LevelEditor",
			"Slate",
			"EnhancedInput",
			"ImageWrapper",
			"RenderCore",
			"PythonScriptPlugin",
			"AssetRegistry",
			"Kismet",
		});

		// LiveCoding lives in Developer/Windows and only exists where Live Coding is supported, so an
		// unconditional dependency would pull a Windows-only module into Linux and Mac editor builds.
		if (Target.bWithLiveCoding)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
		}

		// SHA-256 for the physical-input guard digests: Windows uses BCrypt (bcrypt.lib comes with CortexCore);
		// Unix uses the engine's OpenSSL module, as CortexCore does for FCortexSafeFileContract::HashFileBytesSha256.
		bool bUseOpenSslSha256 = Target.IsInPlatformGroup(UnrealPlatformGroup.Unix);
		if (bUseOpenSslSha256)
		{
			AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
		}
		PrivateDefinitions.Add("CORTEX_SHA256_OPENSSL=" + (bUseOpenSslSha256 ? "1" : "0"));
	}
}
