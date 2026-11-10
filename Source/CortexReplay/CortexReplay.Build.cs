using UnrealBuildTool;

public class CortexReplay : ModuleRules
{
	public CortexReplay(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CortexCore",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ApplicationCore",
			"CoreUObject",
			"Engine",
			"EditorStyle",
			"InputCore",
			"Json",
			"JsonUtilities",
			"LevelEditor",
			"Projects",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"UnrealEd",
			"CortexEditor",
		});

		// SHA-256 for recording snapshots: Windows uses BCrypt (bcrypt.lib comes with CortexCore); Unix uses the
		// engine's OpenSSL module, as CortexCore does for FCortexSafeFileContract::HashFileBytesSha256.
		bool bUseOpenSslSha256 = Target.IsInPlatformGroup(UnrealPlatformGroup.Unix);
		if (bUseOpenSslSha256)
		{
		    AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
		}
		PrivateDefinitions.Add("CORTEX_SHA256_OPENSSL=" + (bUseOpenSslSha256 ? "1" : "0"));
	}
}
