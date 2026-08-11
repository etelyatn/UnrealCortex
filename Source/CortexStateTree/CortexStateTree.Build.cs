using UnrealBuildTool;

public class CortexStateTree : ModuleRules
{
	public CortexStateTree(ReadOnlyTargetRules Target) : base(Target)
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
			"Engine",
			"Json",
			"JsonUtilities",
			"AssetRegistry",
			"AssetTools",
			"UnrealEd",
			"GameplayTags",
			"GameplayStateTreeModule",
			"StateTreeModule",
			"StateTreeEditorModule",
		});

		// UE 5.4: FStateTreeCompilerLog has no ToTokenizedMessages(); messages are
		// extracted through a MessageLog listing instead (see CortexSTCompat.h).
		// StructUtils owns FInstancedStruct before its 5.5 move into CoreUObject.
		if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion < 5)
		{
			PrivateDependencyModuleNames.Add("MessageLog");
			PrivateDependencyModuleNames.Add("StructUtils");
		}

		// UE 5.6+: StateTree binding descriptors (FStateTreeBindableStructDesc, used by
		// FStateTreeCompilerLogMessage) now derive from FPropertyBindingBindableStructDescriptor,
		// whose dllimport destructor lives in the PropertyBindingUtils module. Without this the
		// destructor symbol is unresolved at link time. The type/module does not exist pre-5.6.
		if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 6)
		{
			PrivateDependencyModuleNames.Add("PropertyBindingUtils");
		}
	}
}
